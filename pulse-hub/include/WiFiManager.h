#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

void initWiFi();
void checkWiFiConnection();
void synchronizeTime();

bool     hasNtpSynced();
int32_t  getNtpDriftSeconds();
uint32_t getNtpSyncAgeSeconds();

String getFormattedTime();

bool addWifiNetwork(const String& ssid, const String& password);
bool removeWifiNetwork(const String& ssid);
bool setWifiPriority(const String& ssid, int newPriority); // 1 = highest
String getStoredNetworksJson();

void startAPMode();
void stopAPMode();

// AP and OTA passwords, overridable at deployment and persisted in NVS.
// Changing the AP password restarts the hotspot; changing the OTA password
// takes effect on the next boot and must match platformio.ini's --auth= value.
String getApPassword();
String getOtaPassword();
bool   setApPassword(const String& pass);   // false if shorter than 8 chars
bool   setOtaPassword(const String& pass);

#endif // WIFI_MANAGER_H
