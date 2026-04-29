#include <Arduino.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>

#include "config.h"
#include "board_compat.h"
#include "modem_iface.h"
#include "bike_logic.h"

#if defined(ARDUINO_ARCH_STM32)
HardwareSerial SerialAT(MODEM_RX_PIN, MODEM_TX_PIN);
#else
HardwareSerial SerialAT(1);
#endif

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient mqtt(client);

BikeMode currentMode = TRACKING_MODE;

static unsigned long lastSend = 0;
static unsigned long lastReconnectAttempt = 0;
static unsigned long lastBatteryPrint = 0;
static unsigned long lastGpsDebug = 0;
static bool gpsLostEventSent = false;
static bool bikeLocked = true;
static bool buzzerActive = false;
static int lastMqttState = 0;

static constexpr const char *FW_VERSION = "1.0.0";
static bool ensureDataConnection();
static void runGpsOnlyTest();
static void runGpsAtDiagnosticTest();

static void buildTopic(char *buffer, size_t size, const char *suffix)
{
    snprintf(buffer, size, "v1/bike/%s/%s", BIKE_ID, suffix);
}

static unsigned long currentTelemetryIntervalMs()
{
    return (currentMode == TRACKING_MODE) ? GPS_SEND_INTERVAL_MS : DOCKED_SEND_INTERVAL_MS;
}

static int gsmSignalToPct(int csq)
{
    if (csq <= 0 || csq == 99)
        return 0;

    if (csq > 31)
        csq = 31;

    return (csq * 100) / 31;
}

static float readBatteryVoltage()
{
    uint32_t sumMv = 0;
    constexpr uint8_t sampleCount = 16;

    for (uint8_t i = 0; i < sampleCount; i++)
    {
        sumMv += boardReadAnalogMilliVolts(BATTERY_ADC_PIN);
        delay(2);
    }

    const float adcMv = (float)sumMv / sampleCount;
    const float groveOutputV = (adcMv / 1000.0f) / BATTERY_ADC_EXTRA_DIVIDER_RATIO;
    const float batteryV = groveOutputV * BATTERY_GROVE_GAIN;

    return batteryV;
}

static int batteryVoltageToPct(float batteryV)
{
    if (batteryV <= BATTERY_EMPTY_V)
        return 0;

    if (batteryV >= BATTERY_FULL_V)
        return 100;

    return (int)(((batteryV - BATTERY_EMPTY_V) * 100.0f) / (BATTERY_FULL_V - BATTERY_EMPTY_V));
}

static void printBatteryStatus()
{
    const float batteryV = readBatteryVoltage();
    const int batteryPct = batteryVoltageToPct(batteryV);

    Serial.print("Battery | ");
    Serial.print(batteryV, 2);
    Serial.print(" V | ");
    Serial.print(batteryPct);
    Serial.println("%");
}

static bool publishJson(const char *topic, JsonDocument &doc, bool retained = false)
{
    char payload[384];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    return mqtt.publish(topic, payload, retained);
}

static const char *mqttStateToString(int state)
{
    switch (state)
    {
    case -4:
        return "MQTT_CONNECTION_TIMEOUT";
    case -3:
        return "MQTT_CONNECTION_LOST";
    case -2:
        return "MQTT_CONNECT_FAILED";
    case -1:
        return "MQTT_DISCONNECTED";
    case 0:
        return "MQTT_CONNECTED";
    case 1:
        return "MQTT_BAD_PROTOCOL";
    case 2:
        return "MQTT_BAD_CLIENT_ID";
    case 3:
        return "MQTT_UNAVAILABLE";
    case 4:
        return "MQTT_BAD_CREDENTIALS";
    case 5:
        return "MQTT_UNAUTHORIZED";
    default:
        return "MQTT_UNKNOWN_STATE";
    }
}

static bool testBrokerTcpReachability()
{
    Serial.print("Teste TCP broker ");
    Serial.print(MQTT_SERVER);
    Serial.print(":");
    Serial.println(MQTT_PORT);

    client.stop();
    bool connected = client.connect(MQTT_SERVER, MQTT_PORT, 10000);
    if (!connected)
    {
        Serial.println("TCP falhou: sem socket para o broker");
        return false;
    }

    Serial.println("TCP OK: broker acessivel via GPRS");
    client.stop();
    return true;
}

// --------------------------------------------------
// TIMESTAMP via rede GSM
// --------------------------------------------------
String getTimestamp()
{
    return modemGetTimestamp();
}

// --------------------------------------------------
// MQTT MESSAGES
// --------------------------------------------------
void sendAck(const char *messageId, const char *result, const char *detailReason = nullptr)
{
    char topic[100];
    buildTopic(topic, sizeof(topic), "ack");

    JsonDocument doc;
    doc["message_id"] = messageId;
    doc["timestamp"] = getTimestamp();
    doc["result"] = result;

    JsonObject details = doc["details"].to<JsonObject>();
    if (detailReason != nullptr)
    {
        details["reason"] = detailReason;
    }

    publishJson(topic, doc);
}

void sendEvent(const char *eventType, const char *detailKey = nullptr, const char *detailValue = nullptr)
{
    char topic[100];
    buildTopic(topic, sizeof(topic), "event");

    JsonDocument doc;
    doc["timestamp"] = getTimestamp();
    doc["event_type"] = eventType;

    JsonObject details = doc["details"].to<JsonObject>();
    if (detailKey != nullptr && detailValue != nullptr)
    {
        details[detailKey] = detailValue;
    }

    publishJson(topic, doc);
}

void publishConnectionStatus(const char *status)
{
    char topic[100];
    buildTopic(topic, sizeof(topic), "connection");

    JsonDocument doc;
    doc["status"] = status;

    publishJson(topic, doc, true);
}

// --------------------------------------------------
// MQTT CALLBACK
// --------------------------------------------------
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);

    if (err)
    {
        Serial.print("CMD JSON invalido: ");
        Serial.println(err.c_str());
        sendAck("unknown", "failure", "invalid_json");
        return;
    }

    const char *messageId = doc["message_id"] | "unknown";
    const char *command = doc["command"] | "";

    Serial.print("CMD recebido em ");
    Serial.println(topic);
    Serial.print("Command: ");
    Serial.println(command);

    if (strcmp(command, "lock") == 0)
    {
        bikeLocked = true;
        sendAck(messageId, "success");
    }
    else if (strcmp(command, "unlock") == 0)
    {
        bikeLocked = false;
        sendAck(messageId, "success");
    }
    else if (strcmp(command, "sleep") == 0)
    {
        currentMode = SLEEP_MODE;
        sendAck(messageId, "success");
    }
    else if (strcmp(command, "wake") == 0)
    {
        currentMode = TRACKING_MODE;
        sendAck(messageId, "success");
    }
    else if (strcmp(command, "ring_buzzer") == 0)
    {
        buzzerActive = true;
        sendAck(messageId, "success");
        buzzerActive = false;
    }
    else if (strcmp(command, "reboot") == 0)
    {
        sendAck(messageId, "success");
        delay(1000);
        boardRestart();
    }
    else if (strcmp(command, "firmware_update") == 0)
    {
        sendAck(messageId, "failure", "unsupported_command");
    }
    else
    {
        sendAck(messageId, "failure", "unknown_command");
    }
}

// --------------------------------------------------
// MQTT CONNECT + LWT
// --------------------------------------------------
bool connectMQTT()
{
    Serial.println("Ligar MQTT...");

    char willTopic[100];
    buildTopic(willTopic, sizeof(willTopic), "connection");

    if (mqtt.connect(
            BIKE_ID,
            MQTT_USER,
            MQTT_PASS,
            willTopic,
            1,
            true,
            "{\"status\":\"offline\"}"))
    {
        Serial.println("MQTT ligado");
        publishConnectionStatus("online");

        char cmdTopic[100];
        buildTopic(cmdTopic, sizeof(cmdTopic), "cmd");

        mqtt.subscribe(cmdTopic, 1);

        return true;
    }

    Serial.print("Erro MQTT: ");
    Serial.print(mqtt.state());
    Serial.print(" (");
    Serial.print(mqttStateToString(mqtt.state()));
    Serial.println(")");
    return false;
}

static bool recoverMqttConnection()
{
    const bool networkConnected = modem.isNetworkConnected();
    const bool gprsConnected = modem.isGprsConnected();

    Serial.print("Estado modem | network=");
    Serial.print(networkConnected ? "on" : "off");
    Serial.print(" gprs=");
    Serial.println(gprsConnected ? "on" : "off");

    if (!networkConnected || !gprsConnected)
    {
        if (!ensureDataConnection())
        {
            return false;
        }
    }

    if (!testBrokerTcpReachability())
    {
        return ensureDataConnection() && testBrokerTcpReachability() && connectMQTT();
    }

    return connectMQTT();
}

// --------------------------------------------------
// TELEMETRIA
// --------------------------------------------------
void sendTelemetry(float lat, float lon)
{
    char topic[100];
    buildTopic(topic, sizeof(topic), "telemetry");

    const float batteryV = readBatteryVoltage();
    const int batteryPct = batteryVoltageToPct(batteryV);

    JsonDocument doc;
    doc["timestamp"] = getTimestamp();
    doc["gps_lat"] = serialized(String(lat, 6));
    doc["gps_lng"] = serialized(String(lon, 6));
    doc["speed_kmh"] = 0.0;
    doc["battery_level_pct"] = batteryPct;
    doc["lock_state"] = bikeLocked ? "locked" : "unlocked";
    doc["ride_state"] = bikeLocked ? "idle" : "riding";
    doc["alarm_state"] = buzzerActive ? "active" : "inactive";
    doc["gsm_signal_pct"] = gsmSignalToPct(modem.getSignalQuality());
    doc["imu_tilt"] = 0.0;
    doc["firmware_version"] = FW_VERSION;

    publishJson(topic, doc);
}

bool ensureDataConnection()
{
    if (!modem.isNetworkConnected())
    {
        if (!modemConnectNetwork())
            return false;
    }

    if (!modem.isGprsConnected())
    {
        if (!modemConnectData())
            return false;
    }

    return true;
}

// --------------------------------------------------
// SETUP
// --------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(2000);

    Serial.println("START");

    boardConfigureAdc();

#if BATTERY_TEST_MODE
    Serial.println("BATTERY TEST MODE");
    return;
#endif

    boardBeginModemSerial(SerialAT, MODEM_BAUD);
    delay(3000);

    if (!modemProbeUART())
    {
        Serial.println("Falha de comunicacao com o SIM808");
        while (true)
            delay(1000);
    }

    modemInit();

#if GPS_ONLY_TEST_MODE
#if GPS_AT_DIAGNOSTIC_MODE
    Serial.println("GPS AT DIAGNOSTIC MODE");
#else
    modemEnableGPS();
    Serial.println("GPS ONLY TEST MODE");
#endif
    return;
#endif

    ensureDataConnection();

    modemEnableGPS();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(30);
    mqtt.setBufferSize(512);

    connectMQTT();
}

// --------------------------------------------------
// LOOP
// --------------------------------------------------
void loop()
{
#if BATTERY_TEST_MODE
    if (millis() - lastBatteryPrint >= 2000)
    {
        lastBatteryPrint = millis();

        const float batteryV = readBatteryVoltage();
        const int batteryPct = batteryVoltageToPct(batteryV);

        Serial.print("Battery: ");
        Serial.print(batteryV, 2);
        Serial.print(" V | ");
        Serial.print(batteryPct);
        Serial.println("%");
    }

    delay(50);
    return;
#endif

#if GPS_ONLY_TEST_MODE
#if GPS_AT_DIAGNOSTIC_MODE
    runGpsAtDiagnosticTest();
#else
    runGpsOnlyTest();
#endif
    delay(50);
    return;
#endif

    if (!mqtt.connected())
    {
        const int mqttState = mqtt.state();
        if (mqttState != lastMqttState)
        {
            Serial.print("MQTT desligado: ");
            Serial.print(mqttState);
            Serial.print(" (");
            Serial.print(mqttStateToString(mqttState));
            Serial.println(")");
            lastMqttState = mqttState;
        }

        if (millis() - lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS)
        {
            lastReconnectAttempt = millis();
            if (recoverMqttConnection())
            {
                lastMqttState = mqtt.state();
            }
        }
    }
    else
    {
        lastMqttState = 0;
    }

    if (mqtt.connected())
    {
        mqtt.loop();
    }

#if BATTERY_SERIAL_DEBUG
    if (millis() - lastBatteryPrint >= 5000)
    {
        lastBatteryPrint = millis();
        printBatteryStatus();
    }
#endif

    if (millis() - lastSend > currentTelemetryIntervalMs())
    {
        lastSend = millis();

        float lat = 0, lon = 0;

        if (modemGetGPS(lat, lon))
        {
            Serial.print("Enviar telemetry | mode=");
            Serial.println((currentMode == TRACKING_MODE) ? "tracking" : "sleep");
            sendTelemetry(lat, lon);
            gpsLostEventSent = false;
        }
        else
        {
            Serial.println("Sem GPS fix");
#if GPS_DEBUG_RAW
            String gpsRaw = modemGetGPSRaw();
            if (gpsRaw.length() > 0)
            {
                Serial.print("GPS raw: ");
                Serial.println(gpsRaw);
            }
#endif
#if GPS_DEBUG_SATS
            if (millis() - lastGpsDebug >= GPS_DEBUG_INTERVAL_MS)
            {
                lastGpsDebug = millis();
                int visibleSats = 0;
                int usedSats = 0;
                modemGetGPSStats(visibleSats, usedSats);
                Serial.print("GPS debug | visible=");
                Serial.print(visibleSats);
                Serial.print(" used=");
                Serial.println(usedSats);
            }
#endif

            if (!gpsLostEventSent && mqtt.connected())
            {
                sendEvent("gps_lost");
                gpsLostEventSent = true;
            }
        }
    }
}

static void runGpsOnlyTest()
{
    if (millis() - lastGpsDebug < 5000)
        return;

    lastGpsDebug = millis();

    int runStatus = 0;
    int fixStatus = 0;
    int visibleSats = 0;
    int usedSats = 0;
    float lat = 0.0f;
    float lon = 0.0f;
    String gpsRaw;

    if (modemReadGPSStatus(runStatus, fixStatus, visibleSats, usedSats, lat, lon, gpsRaw) && fixStatus == 1)
    {
        Serial.print("GPS fix | lat=");
        Serial.print(lat, 6);
        Serial.print(" lon=");
        Serial.println(lon, 6);
        return;
    }

#if GPS_ONLY_SHOW_RAW
    if (gpsRaw.length() > 0)
    {
        Serial.print("GPS raw | ");
        Serial.println(gpsRaw);
    }
#endif

    Serial.print("GPS only | run=");
    Serial.print(runStatus);
    Serial.print(" fix=");
    Serial.print(fixStatus);
    Serial.print(" visible=");
    Serial.print(visibleSats);
    Serial.print(" used=");
    Serial.println(usedSats);
}

static void runGpsAtDiagnosticTest()
{
    if (millis() - lastGpsDebug < 10000)
        return;

    lastGpsDebug = millis();

    Serial.println("---- GPS AT DIAG ----");

    String resp = modemSendATCommand("AT+CGNSPWR=1", 3000);
    Serial.print("AT+CGNSPWR=1 -> ");
    Serial.println(resp);

    resp = modemSendATCommand("AT+CGNSPWR?", 3000);
    Serial.print("AT+CGNSPWR? -> ");
    Serial.println(resp);

    resp = modemSendATCommand("AT+CGNSINF", 4000);
    Serial.print("AT+CGNSINF -> ");
    Serial.println(resp);
}
