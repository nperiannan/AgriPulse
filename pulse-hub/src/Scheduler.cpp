#include "Scheduler.h"
#include "Logger.h"
#include "Globals.h"
#include "MotorDrive.h"
#include "MotorControl.h"   // preWarnBuzzer() still used for the pre-start warning
#include "Buzzer.h"
#include "Config.h"
#include <Preferences.h>
#include <time.h>

Schedule schedules[MAX_SCHEDULES];

static unsigned long lastScheduleCheck = 0;

// -----------------------------------------------------------------------
//  NVS helpers
// -----------------------------------------------------------------------

void initScheduler() {
    Preferences prefs;
    prefs.begin("scheduler", true);
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        String prefix = String(i) + "_";
        schedules[i].enabled   = prefs.getBool  ((prefix + "en").c_str(),   false);
        schedules[i].motorType = prefs.getUChar  ((prefix + "mt").c_str(),   0);   // 0=OH, 1=UG
        schedules[i].time      = prefs.getString ((prefix + "t").c_str(),    "00:00");
        schedules[i].duration  = prefs.getUShort ((prefix + "d").c_str(),    2);
        schedules[i].isRunning = false;
        schedules[i].startTime = 0;
        schedules[i].preBuzzing = false;
    }
    prefs.end();
    Log(INFO, "[Sched] Loaded " + String(MAX_SCHEDULES) + " schedules");
}

void saveSchedules() {
    Preferences prefs;
    prefs.begin("scheduler", false);
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        String prefix = String(i) + "_";
        prefs.putBool  ((prefix + "en").c_str(),  schedules[i].enabled);
        prefs.putUChar ((prefix + "mt").c_str(),  schedules[i].motorType);
        prefs.putString((prefix + "t").c_str(),   schedules[i].time);
        prefs.putUShort((prefix + "d").c_str(),   schedules[i].duration);
    }
    prefs.end();
    Log(INFO, "[Sched] Saved");
}

void clearAllSchedules() {
    stopBuzzer();   // in case a schedule's pre-start warning was sounding
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = {false, 0, "00:00", 2, false, 0, false};
    }
    saveSchedules();
    Log(INFO, "[Sched] All cleared");
}

// -----------------------------------------------------------------------
//  Runtime check – call from loop()
// -----------------------------------------------------------------------

void checkSchedules() {
    if (millis() < BOOT_GRACE_PERIOD_MS) return;   // suppress scheduler during boot
    // 1 s granularity so the pre-start buzzer warning and the actual start
    // land on the exact seconds we intend (a coarser 10 s poll made both the
    // warning timing and the start-of-run timestamp sloppy by up to 10 s).
    if (millis() - lastScheduleCheck < 1000UL) return;
    lastScheduleCheck = millis();

    struct tm ti;
    if (!getLocalTime(&ti)) return;

    char nowBuf[6];
    snprintf(nowBuf, sizeof(nowBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    String nowStr = String(nowBuf);
    const int nowSec = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
    const int buzzerLeadSec = (int)(MOTOR_START_BUZZER_DELAY_MS / 1000UL);

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        Schedule& s = schedules[i];
        bool isUG = (s.motorType == 1);

        // Disabled but was running – stop
        if (!s.enabled) {
            if (s.preBuzzing) {
                s.preBuzzing = false;
                stopBuzzer();
            }
            if (s.isRunning) {
                s.isRunning = false;
                bool anyRunning = false;
                for (int j = 0; j < MAX_SCHEDULES; j++) {
                    if (schedules[j].isRunning && schedules[j].motorType == s.motorType) {
                        anyRunning = true; break;
                    }
                }
                if (!anyRunning) {
                    motorDriveRequestStop(REASON_SCHEDULED);
                    Log(INFO, "[Sched] " + String(i) + " stopped (disabled)");
                }
            }
            continue;
        }

        // Target time-of-day in seconds since midnight (schedule times are "HH:MM")
        const int targetSec = s.time.substring(0, 2).toInt() * 3600
                             + s.time.substring(3, 5).toInt() * 60;

        // Pre-start warning — sound the buzzer buzzerLeadSec BEFORE the
        // scheduled time so it finishes exactly as the schedule hits, instead
        // of after (which used to delay the relay and shorten the run).
        if (!s.isRunning && !s.preBuzzing && buzzerDelayEnabled
            && nowSec == targetSec - buzzerLeadSec) {
            preWarnBuzzer();
            s.preBuzzing = true;
            Log(INFO, "[Sched] " + String(i) + " (" + (isUG?"UG":"OH") + ") pre-start buzzer — motor at " + s.time);
        }

        // Start trigger — fires on the exact scheduled second; the nowStr
        // guard is a safety net so a missed tick still starts before the
        // scheduled minute rolls over.
        if (!s.isRunning && nowSec >= targetSec && nowStr == s.time) {
            Log(INFO, "[Sched] " + String(i) + " (" + (isUG?"UG":"OH") + ") starting at " + s.time
                + " for " + String(s.duration) + " min");
            s.preBuzzing = false;
            motorDriveRequestStart(isUG ? MOTOR_BORE : MOTOR_WELL, REASON_SCHEDULED);
            s.isRunning = true;
            s.startTime = millis();
        }

        // Stop trigger: duration expired
        if (s.isRunning) {
            unsigned long elapsed = millis() - s.startTime;
            if (elapsed >= (unsigned long)s.duration * 60000UL) {
                s.isRunning = false;
                Log(INFO, "[Sched] " + String(i) + " (" + (isUG?"UG":"OH") + ") finished after "
                    + String(s.duration) + " min");
                bool anyRunning = false;
                for (int j = 0; j < MAX_SCHEDULES; j++) {
                    if (schedules[j].isRunning && schedules[j].motorType == s.motorType) {
                        anyRunning = true; break;
                    }
                }
                if (!anyRunning) motorDriveRequestStop(REASON_SCHEDULED);
            }
        }
    }
}
