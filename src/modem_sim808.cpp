#include <Arduino.h>
#include <TinyGsmClient.h>
#include "modem_iface.h"
#include "config.h"

extern HardwareSerial SerialAT;
extern TinyGsm modem;
static String lastTimestamp = "1970-01-01T00:00:00Z";

static String readResponse(uint32_t timeoutMs)
{
    String resp;
    unsigned long start = millis();

    while (millis() - start < timeoutMs)
    {
        while (SerialAT.available())
        {
            char c = (char)SerialAT.read();
            resp += c;
        }

        if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0)
        {
            break;
        }

        delay(10);
    }

    return resp;
}

static bool sendATProbe(const String &cmd, uint32_t timeoutMs, String &resp)
{
    while (SerialAT.available())
        SerialAT.read();

    SerialAT.print(cmd);
    SerialAT.print("\r\n");

    resp = readResponse(timeoutMs);
    return resp.indexOf("OK") >= 0;
}

static bool sendATExpectOK(const String &cmd, uint32_t timeoutMs = 2000)
{
    while (SerialAT.available())
        SerialAT.read();

    Serial.print(">> ");
    Serial.println(cmd);

    SerialAT.println(cmd);

    String resp = readResponse(timeoutMs);

    Serial.print(resp);

    return resp.indexOf("OK") >= 0;
}

String modemSendATCommand(const String &cmd, uint32_t timeoutMs)
{
    while (SerialAT.available())
        SerialAT.read();

    SerialAT.println(cmd);
    return readResponse(timeoutMs);
}

bool modemProbeUART()
{
    // Many SIM808/SIM800 boards ship configured at 115200 or autobaud; try it early to reduce "Sem resposta".
    const uint32_t baudRates[] = {115200, MODEM_BAUD, 57600, 38400, 19200, 9600, 4800};
    constexpr uint8_t attemptsPerBaud = 3;

    Serial.println("Teste UART ESP32 <-> SIM808");

    for (uint8_t i = 0; i < sizeof(baudRates) / sizeof(baudRates[0]); i++)
    {
        const uint32_t baud = baudRates[i];
        if (i > 0 && baud == baudRates[i - 1])
            continue;

        Serial.printf("Probar modem a %lu baud...\n", (unsigned long)baud);
        SerialAT.begin(baud, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
        delay(1200);

        for (uint8_t attempt = 0; attempt < attemptsPerBaud; attempt++)
        {
            String resp;

            if (sendATProbe("AT", 1500, resp))
            {
                Serial.printf("UART OK a %lu baud\n", (unsigned long)baud);
                Serial.print(resp);

                if (baud != MODEM_BAUD)
                {
                    Serial.printf("Aviso: config.h usa %d baud mas o modem respondeu a %lu\n",
                                  MODEM_BAUD,
                                  (unsigned long)baud);
                }

                if (sendATProbe("ATI", 1500, resp))
                {
                    Serial.println("Identificacao modem:");
                    Serial.print(resp);
                }

                if (sendATProbe("AT+IPR?", 1500, resp))
                {
                    Serial.println("Baud reportada pelo modem:");
                    Serial.print(resp);
                }

                return true;
            }

            if (attempt == attemptsPerBaud - 1)
            {
                if (resp.length() > 0)
                {
                    Serial.println("Resposta recebida, mas sem OK:");
                    Serial.print(resp);
                }
                else
                {
                    Serial.println("Sem resposta");
                }
            }

            delay(500);
        }
    }

    Serial.println("Falha total na UART com o SIM808");
    Serial.println("Verifica TX/RX cruzados, GND comum, alimentacao do SIM808 e PWRKEY");
    return false;
}

void modemInit()
{
    Serial.println("Inicializar modem...");
    modem.restart();
    delay(1000);
    Serial.println("Modem iniciado");
}

bool modemConnectNetwork()
{
    Serial.println("Registar na rede...");

    // TinyGSM's waitForNetwork can be a long blocking call depending on network conditions.
    // On ESP32-C3 this has been observed to trigger watchdog resets, so we poll in short chunks.
    const unsigned long start = millis();
    while (millis() - start < MODEM_NETWORK_TIMEOUT_MS)
    {
        if (modem.isNetworkConnected())
        {
            Serial.println("Rede registada");
            return true;
        }

        // Short attempt; keep the MCU responsive.
        modem.waitForNetwork(1500L);
        delay(250);
        yield();
    }

    Serial.println("Falha no registo da rede");
    return false;
}

bool modemConnectData()
{
    Serial.println("Ativar GPRS...");

    const unsigned long start = millis();
    while (millis() - start < MODEM_NETWORK_TIMEOUT_MS)
    {
        if (modem.isGprsConnected())
            break;

        // Short attempt; keep the MCU responsive.
        if (modem.gprsConnect(APN, "", ""))
            break;

        delay(500);
        yield();
    }

    if (!modem.isGprsConnected())
    {
        Serial.println("Falha na ligacao GPRS");
        return false;
    }

    Serial.println("GPRS ligado");
    Serial.print("IP: ");
    Serial.println(modem.localIP());
    return true;
}

bool modemEnableGPS()
{
    Serial.println("Ligar GPS...");

    if (!modem.enableGPS())
    {
        Serial.println("Falha ao ligar GPS");
        return false;
    }

    return true;
}

bool modemDisableGPS()
{
    Serial.println("Desligar GPS...");

    if (!modem.disableGPS())
    {
        Serial.println("Falha ao desligar GPS");
        return false;
    }

    return true;
}

bool modemGetGPS(float &lat, float &lon)
{
    float speed = 0.0f;
    float altitude = 0.0f;
    int vsat = 0;
    int usat = 0;
    float accuracy = 0.0f;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (!modem.getGPS(&lat, &lon, &speed, &altitude, &vsat, &usat, &accuracy,
                      &year, &month, &day, &hour, &minute, &second))
        return false;

    if (year > 2000)
    {
        char iso[30];
        snprintf(iso, sizeof(iso),
                 "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 year, month, day, hour, minute, second);
        lastTimestamp = String(iso);
    }

    return true;
}

String modemGetGPSRaw()
{
    return modem.getGPSraw();
}

bool modemGetGPSStats(int &visibleSats, int &usedSats)
{
    int runStatus = 0;
    int fixStatus = 0;
    float lat = 0.0f;
    float lon = 0.0f;
    String raw;
    return modemReadGPSStatus(runStatus, fixStatus, visibleSats, usedSats, lat, lon, raw);
}

bool modemReadGPSStatus(int &runStatus, int &fixStatus, int &visibleSats, int &usedSats,
                        float &lat, float &lon, String &raw)
{
    raw = modem.getGPSraw();
    runStatus = 0;
    fixStatus = 0;
    visibleSats = 0;
    usedSats = 0;
    lat = 0.0f;
    lon = 0.0f;

    if (raw.length() == 0)
        return false;

    String fields[20];
    int fieldCount = 0;
    int start = 0;

    for (int i = 0; i <= raw.length(); i++)
    {
        if (i == raw.length() || raw[i] == ',')
        {
            if (fieldCount < 20)
            {
                fields[fieldCount++] = raw.substring(start, i);
            }
            start = i + 1;
        }
    }

    // CGNSINF expected positions:
    // [0] run status, [1] fix status, [2] UTC datetime, [3] lat, [4] lon, ... [14] sats in view, [15] sats used
    if (fieldCount > 0)
        runStatus = fields[0].toInt();

    if (fieldCount > 1)
        fixStatus = fields[1].toInt();

    if (fieldCount > 4)
    {
        lat = fields[3].toFloat();
        lon = fields[4].toFloat();
    }

    if (fieldCount > 15)
    {
        visibleSats = fields[14].toInt();
        usedSats = fields[15].toInt();
    }

    return fixStatus == 1;
}

String modemGetTimestamp()
{
    return lastTimestamp;
}

String modemGetLocalIP()
{
    return modem.localIP().toString();
}
