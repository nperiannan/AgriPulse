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

void loop() {
    // --- Non-blocking WiFi/OTA maintenance ---
    checkWiFiConnection();

    // --- RTC periodic sync ---
    checkAndSyncRTC();

    // --- LoRa: receive OH tank float state ---
    pollLoRa();

    // --- UG tank: poll float switch ---
    static unsigned long lastUGRead = 0;
    if (millis() - lastUGRead >= UG_SENSOR_POLL_MS) {
        lastUGRead = millis();
        readUGFloatSwitch();
    }

    // --- Touch switch polling ---
    pollTouchSwitches();

    // --- 3-phase metering ---
    adePoll();

    // --- Motor auto-control ---
    autoControlOHMotor();
    autoControlUGMotor();
    processPendingMotorStarts();
    motorHeartbeat();

    // --- Buzzer pattern update ---
    updateBuzzer();

    // --- LCD display rotation ---
    updateDisplay();

    // --- Scheduler ---
    checkSchedules();

    // --- MQTT ---
    mqttLoop();

    // --- Web server ---
    handleWebClients();

    // --- NTP resync hourly ---
    static unsigned long lastNtp = 0;
    if (!isAPMode && WiFi.status() == WL_CONNECTED &&
        (millis() - lastNtp >= NTP_SYNC_INTERVAL_MS || lastNtp == 0)) {
        lastNtp = millis();
        synchronizeTime();
    }
}
