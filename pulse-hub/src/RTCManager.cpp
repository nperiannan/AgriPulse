#include "RTCManager.h"
#include "Logger.h"
#include <Wire.h>
#include <sys/time.h>
#include <TimeLib.h>

RTC_DS3231 rtc;
bool       ds3231Found = false;

static unsigned long lastRTCSyncMs = 0;
static const unsigned long RTC_SYNC_INTERVAL_MS = 5UL * 60UL * 1000UL; // every 5 min

static void applyDateTime(const DateTime& dt) {
    struct tm t = {};
    t.tm_year = dt.year() - 1900;
    t.tm_mon  = dt.month() - 1;
    t.tm_mday = dt.day();
    t.tm_hour = dt.hour();
    t.tm_min  = dt.minute();
    t.tm_sec  = dt.second();
    time_t epoch = mktime(&t);
    timeval tv    = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    setTime(epoch);
}

void initRTC() {
    if (!rtc.begin()) {
        Log(ERROR, "[RTC] DS3231 not found – using compile-time as fallback");
        ds3231Found = false;
        // Set system clock to compile time
        DateTime ct(F(__DATE__), F(__TIME__));
        applyDateTime(ct);
        return;
    }

    ds3231Found = true;
    Log(INFO, "[RTC] DS3231 found");

    if (rtc.lostPower() || rtc.now().year() < 2024) {
        Log(WARN, "[RTC] Time invalid – setting from compile time");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    applyDateTime(rtc.now());
    Log(INFO, "[RTC] System clock synchronised from DS3231");
}

void checkAndSyncRTC() {
    if (!ds3231Found) return;
    // Once NTP has synced, the DS3231 was updated by NTP; no need to re-apply
    // DS3231→system every 5 min (that would re-introduce DS3231 drift).
    // Instead, only re-apply if NTP has never synced (no WiFi / no internet).
    extern bool hasNtpSynced();  // from WiFiManager
    if (hasNtpSynced()) return;  // NTP is the authority; DS3231 already updated by it

    if (millis() - lastRTCSyncMs < RTC_SYNC_INTERVAL_MS && lastRTCSyncMs != 0) return;
    lastRTCSyncMs = millis();

    DateTime dt = rtc.now();
    if (dt.year() < 2024) {
        Log(WARN, "[RTC] Periodic sync: invalid DS3231 time \u2013 skipped");
        return;
    }
    applyDateTime(dt);
    Log(DEBUG, "[RTC] Periodic sync OK (no NTP yet)");
}

void syncRTCFromEpoch(uint32_t epoch) {
    if (epoch < 1000000000UL) {
        Log(WARN, "[RTC] syncRTCFromEpoch: invalid epoch " + String(epoch));
        return;
    }
    timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    setTime((time_t)epoch);
    if (ds3231Found) {
        rtc.adjust(DateTime((uint32_t)epoch));
    }
    Log(INFO, "[RTC] Synced from BLE epoch " + String(epoch));
}
