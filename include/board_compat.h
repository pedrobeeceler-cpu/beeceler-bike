#ifndef BOARD_COMPAT_H
#define BOARD_COMPAT_H

#include <Arduino.h>

#include "config.h"

inline void boardBeginModemSerial(HardwareSerial &serial, uint32_t baud)
{
#if defined(ARDUINO_ARCH_ESP32)
    serial.begin(baud, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
#else
    serial.begin(baud, SERIAL_8N1);
#endif
}

inline void boardConfigureAdc()
{
    analogReadResolution(BATTERY_ADC_RESOLUTION_BITS);

#if defined(ARDUINO_ARCH_ESP32)
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
#elif defined(ARDUINO_ARCH_STM32)
    pinMode(BATTERY_ADC_PIN, INPUT_ANALOG);
#endif
}

inline uint32_t boardReadAnalogMilliVolts(uint32_t pin)
{
#if defined(ARDUINO_ARCH_ESP32)
    return analogReadMilliVolts(pin);
#else
    const uint32_t raw = analogRead(pin);
    const uint32_t maxRaw = (1UL << BATTERY_ADC_RESOLUTION_BITS) - 1UL;
    const float milliVolts = ((float)raw * BATTERY_ADC_MAX_MV) / (float)maxRaw;
    return (uint32_t)(milliVolts + 0.5f);
#endif
}

inline void boardRestart()
{
#if defined(ARDUINO_ARCH_ESP32)
    ESP.restart();
#elif defined(ARDUINO_ARCH_STM32)
    NVIC_SystemReset();
#else
    void (*resetFunc)(void) = 0;
    resetFunc();
#endif
}

#endif
