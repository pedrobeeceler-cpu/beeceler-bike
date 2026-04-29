#ifndef CONFIG_H
#define CONFIG_H

#pragma once

// Identificação da bicicleta
#define BIKE_ID "bike001"

// MQTT
#define MQTT_SERVER "89.115.231.186"
#define MQTT_PORT 1884
#define MQTT_USER "beeceler"
#define MQTT_PASS "beeceler"

// APN
#define APN "internet.vodafone.pt"

// UART MCU <-> SIM808
// Blue Pill default: USART2, SIM808 TX -> PA3 RX, SIM808 RX -> PA2 TX.
#if defined(ARDUINO_ARCH_STM32)
#define MODEM_RX_PIN PA3
#define MODEM_TX_PIN PA2
#else
#define MODEM_RX_PIN 16
#define MODEM_TX_PIN 17
#endif
#define MODEM_BAUD 9600

// Battery sensing via Grove Voltage Divider (bike battery: 36V empty, 42V full, module gain 10)
#define BATTERY_TEST_MODE 0
#define GPS_ONLY_TEST_MODE 0
#define GPS_AT_DIAGNOSTIC_MODE 0
#if defined(ARDUINO_ARCH_STM32)
#define BATTERY_ADC_PIN PA0
#else
#define BATTERY_ADC_PIN 34
#endif
#define BATTERY_ADC_MAX_MV 3300.0f
#define BATTERY_ADC_RESOLUTION_BITS 12
#define BATTERY_ADC_EXTRA_DIVIDER_RATIO 0.733333f
#define BATTERY_GROVE_GAIN 10.0f
#define BATTERY_FULL_V 42.0f
#define BATTERY_EMPTY_V 36.0f
#define GPS_DEBUG_RAW 0
#define GPS_DEBUG_SATS 0
#define GPS_DEBUG_INTERVAL_MS 10000UL
#define GPS_ONLY_SHOW_RAW 0
#define BATTERY_SERIAL_DEBUG 1

// Timings
#define GPS_SEND_INTERVAL_MS 10000UL
#define DOCKED_SEND_INTERVAL_MS 60000UL
#define MQTT_RECONNECT_INTERVAL_MS 5000UL
#define MODEM_NETWORK_TIMEOUT_MS 120000UL
#define MODEM_GPS_TIMEOUT_MS 3000UL

#endif
