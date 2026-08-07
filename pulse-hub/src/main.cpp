#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "RTCManager.h"
#include "WiFiManager.h"
#include "Buzzer.h"
#include "Display.h"
#include "Sensors.h"
#include "LoRaManager.h"
#include "MotorControl.h"
#include "BLEManager.h"
#include "HttpServer.h"
#include "Scheduler.h"
#include "History.h"
#include "TouchSwitch.h"
#include "MQTTManager.h"
#include "ADE7758.h"
#include "MotorProtection.h"
#include "MotorDrive.h"
#include "Zones.h"
#include "Programs.h"
#include "I2CScan.h"

// =============================================================================
//                              GLOBAL STATE DEFINITIONS
// =============================================================================

TankState      ugTankState           = TANK_STATE_UNKNOWN;  // display ? until sensor confirms; motor won't fire (hardware OFF + grace period)
TankState      ohTankState           = TANK_STATE_UNKNOWN;
TankState      ohLastKnownState      = TANK_STATE_UNKNOWN;

bool           ohMotorRunning        = false;
bool           ugMotorRunning        = false;

MotorSource    ohMotorSource         = MOTOR_SRC_NONE;
MotorSource    ugMotorSource         = MOTOR_SRC_NONE;

bool           loraOperational       = false;
unsigned long  lastLoraReceivedTime  = 0;

bool           isAPMode              = false;
String         wifiSSID              = "";
int            wifiRSSI              = 0;

bool           ohDisplayOnly         = false;
bool           ugDisplayOnly         = false;
bool           ugIgnoreForOH         = false;
bool           buzzerDelayEnabled    = true;
bool           manualAutoStop        = true;
uint8_t        lcdBacklightMode      = LCD_BL_AUTO;

TankState      ohStartLevel          = TANK_STATE_EMPTY;
TankState      ohStopLevel           = TANK_STATE_FULL;
uint8_t        ohMaxRunMin           = 20;
uint8_t        mqttWatchdogMin       = MQTT_WATCHDOG_DEFAULT_MIN;

Preferences    preferences;

// =============================================================================
//                              SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(3000);  // Wait for USB CDC to enumerate so early messages are visible

    // --- CRITICAL: force relay pins OFF immediately before any other init ---
    // Prevents relay from energising during ESP32 GPIO floating at boot.
    // Must be the very first hardware action.
    pinMode(OH_RELAY_PIN, OUTPUT);
    digitalWrite(OH_RELAY_PIN, RELAY_OFF);
    pinMode(UG_RELAY_PIN, OUTPUT);
    digitalWrite(UG_RELAY_PIN, RELAY_OFF);

    // I2C bus
    Wire.begin(SDA_PIN, SCL_PIN);

    // Debug LED
    pinMode(DEBUG_LED, OUTPUT);
    digitalWrite(DEBUG_LED, LOW);

    Log(INFO, "=== AgriPulse Controller v" FW_VERSION " ===");

    // Run before initRTC()/initDisplay()/initHistory(), all of which probe I2C
    // and would otherwise each report their own chip "missing" without ever
    // establishing whether the bus is alive at all.
    i2cDiagnoseBothPinouts();

    initRTC();
    initBuzzer();
    initDisplay();

    // Load persisted configuration before any motor or sensor logic
    loadMotorConfig();

    initSensorPins();
    initMotorPins();

    // 3-phase energy meter. Returns false if the meter doesn't answer; the rest
    // of the system still runs, but current/voltage protection stays disarmed.
    adeInit();
    protInit();
    motorDriveInit();

    // Zones after the drive: zonesInit() probes the I2C valve board and must
    // not claim the LCD's address, so it runs after initDisplay() has decided
    // which address (if any) the display owns.
    zonesInit();
    programsInit();

    // LoRa for OH tank remote node
    initLoRa();

    // EEPROM history (after Wire and RTC are ready)
    initHistory();

    // If power was lost while a motor was running, log the (backdated) OFF now
    checkPowerCutRecovery();

    // TTP223 capacitive touch switches
    initTouchSwitches();

    // WiFi (STA + AP fallback) + NTP + OTA
    initWiFi();

    // Web server
    setupWebServer();

    // Scheduler
    initScheduler();

    // MQTT remote monitoring
    initMQTT();

    // ── OTA safety guard ──────────────────────────────────────────────────
    // If we reached this point, all subsystems initialised successfully.
    // Mark this firmware image as valid so the bootloader does NOT roll back
    // to the previous partition on the next reboot.  Without this call, a
    // new OTA image stays in "pending verification" state — if it crashes
    // before reaching here, the bootloader automatically reverts.
    esp_ota_mark_app_valid_cancel_rollback();
    Log(INFO, "[OTA] Firmware marked valid — rollback cancelled");

    Log(INFO, "=== System Ready ===");
}

// =============================================================================
//                              LOOP
// =============================================================================

// Diagnostic instrumentation: loop() shares one core with the HTTP server, so
// any stage that blocks stalls every pending web request along with it. That
// is exactly the still-unexplained 3-6 s /api/power delay noted in the
// investigation that fixed the digest-auth/overlapping-poll/LCD-stall bugs —
// pollLoRa(), the ADE7758 SPI poll and AP+STA coexistence were the leading
// suspects but none was pinned down. Rather than guess further, log which
// stage(s) actually blew the budget whenever a whole iteration is slow, so
// the next occurrence is caught with evidence instead of another probe
// session. Overhead in the normal (fast) case is ~13 extra millis() calls.
#define LOOP_STAGE_WARN_MS   20UL   // a single stage taking this long is suspicious
#define LOOP_TOTAL_WARN_MS   50UL   // whole-iteration time worth logging

struct LoopStageTiming {
    const char*   name;
    unsigned long ms;
};

void loop() {
    unsigned long loopStart = millis();
    LoopStageTiming stages[16];
    uint8_t nStages = 0;
    unsigned long mark = loopStart;
    auto stage = [&](const char* name) {
        unsigned long now = millis();
        stages[nStages++] = { name, now - mark };
        mark = now;
    };

    // --- Non-blocking WiFi/OTA maintenance ---
    checkWiFiConnection();
    stage("wifi");

    // --- RTC periodic sync ---
    checkAndSyncRTC();
    stage("rtc");

    // --- LoRa: receive OH tank float state ---
    pollLoRa();
    stage("lora");

    // --- UG tank: poll float switch ---
    static unsigned long lastUGRead = 0;
    if (millis() - lastUGRead >= UG_SENSOR_POLL_MS) {
        lastUGRead = millis();
        readUGFloatSwitch();
    }
    stage("ug_float");

    // --- Touch switch polling ---
    pollTouchSwitches();
    stage("touch");

    // --- 3-phase metering ---
    adePoll();
    stage("ade");

    // --- Motor control ---
    // Legacy two-motor (OH/UG) relay logic retired 2026-08-07: real hardware
    // is one starter + changeover, not two independent motors. MotorDrive
    // exclusively owns OH_RELAY_PIN/UG_RELAY_PIN now.
    motorDriveTask();
    stage("motor");

    // --- Irrigation zones: run timers, valve/pump interlocks ---
    zonesTask();
    programsTask();
    stage("zones");

    // --- Buzzer pattern update ---
    updateBuzzer();
    stage("buzzer");

    // --- LCD display rotation ---
    updateDisplay();
    stage("display");

    // --- Scheduler ---
    checkSchedules();
    stage("sched");

    // --- MQTT ---
    mqttLoop();
    stage("mqtt");

    // --- Web server ---
    handleWebClients();
    stage("web");

    // --- NTP resync hourly ---
    static unsigned long lastNtp = 0;
    if (!isAPMode && WiFi.status() == WL_CONNECTED &&
        (millis() - lastNtp >= NTP_SYNC_INTERVAL_MS || lastNtp == 0)) {
        lastNtp = millis();
        synchronizeTime();
    }
    stage("ntp");

    unsigned long total = millis() - loopStart;
    if (total >= LOOP_TOTAL_WARN_MS) {
        String detail;
        for (uint8_t i = 0; i < nStages; i++) {
            if (stages[i].ms >= LOOP_STAGE_WARN_MS) {
                if (detail.length()) detail += ", ";
                detail += String(stages[i].name) + "=" + String(stages[i].ms) + "ms";
            }
        }
        if (detail.length() == 0) {
            detail = "no single stage over " + String(LOOP_STAGE_WARN_MS) + "ms - time spread thin across many";
        }
        Log(WARN, "[Loop] Slow iteration: " + String(total) + "ms (" + detail + ")");
    }
}
