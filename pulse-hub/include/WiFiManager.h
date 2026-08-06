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

#endif // WIFI_MANAGER_H
