#ifndef MODEM_IFACE_H
#define MODEM_IFACE_H

#include <Arduino.h>

void modemInit();
bool modemProbeUART();
bool modemConnectNetwork();
bool modemConnectData();
bool modemEnableGPS();
bool modemDisableGPS();
bool modemGetGPS(float &lat, float &lon);
String modemGetGPSRaw();
bool modemGetGPSStats(int &visibleSats, int &usedSats);
bool modemReadGPSStatus(int &runStatus, int &fixStatus, int &visibleSats, int &usedSats,
                        float &lat, float &lon, String &raw);
String modemSendATCommand(const String &cmd, uint32_t timeoutMs = 2000);
String modemGetTimestamp();
String modemGetLocalIP();

#endif
