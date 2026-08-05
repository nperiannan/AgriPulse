#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include <Arduino.h>
#include <RTClib.h>

extern RTC_DS3231 rtc;
extern bool ds3231Found;

// Initialise DS3231. Falls back to compile-time if not found.
void initRTC();

// Periodically re-syncs internal ESP32 clock from DS3231 (call from loop).
void checkAndSyncRTC();

// Sync DS3231 from a Unix epoch value (used by BLE CMD_SYNC_TIME).
void syncRTCFromEpoch(uint32_t epoch);

#endif // RTC_MANAGER_H
