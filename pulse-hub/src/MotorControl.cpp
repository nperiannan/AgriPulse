#include "MotorControl.h"
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#include "Buzzer.h"
#include "History.h"
#include "LoRaManager.h"
#include "MQTTManager.h"
#include <TimeLib.h>
#include <esp_system.h>

// Reason to attach to the next OH/UG motor history record.  For buzzer-delayed
// starts the reason is latched here until the relay is actually energised.
static uint8_t       ohPendingReason = REASON_NONE;
static uint8_t       ugPendingReason = REASON_NONE;

// Reason of the most recent OH/UG relay change (published in the status JSON so
// cloud-derived history can show the real cause instead of guessing).
static uint8_t       ohLastReason = REASON_NONE;
static uint8_t       ugLastReason = REASON_NONE;

// Why the last manual start request for this motor did nothing (MOTOR_REJ_*).
// A refused start changes neither the relay nor the buzzer, so the app has no
// observable ack to wait for — it would otherwise time out after ~12 s and show
// a misleading "command not delivered". Cleared on the next manual command.
static uint8_t       ohRejectCode = MOTOR_REJ_NONE;
static uint8_t       ugRejectCode = MOTOR_REJ_NONE;

// Pending motor start state (for buzzer-delay feature)
static bool          ohMotorStartPending = false;
static unsigned long ohMotorPendingStart = 0;
static bool          ugMotorStartPending = false;
static unsigned long ugMotorPendingStart = 0;

// Timestamp when motor was actually energised (for max runtime & hysteresis)
static unsigned long ohMotorStartedMs = 0;
static unsigned long ugMotorStartedMs = 0;

// Boot recovery: checked once after first valid LoRa packet
static bool bootRecoveryDone = false;

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------

void initMotorPins() {
    pinMode(OH_RELAY_PIN, OUTPUT);
    pinMode(UG_RELAY_PIN, OUTPUT);
    digitalWrite(OH_RELAY_PIN, RELAY_OFF);
    digitalWrite(UG_RELAY_PIN, RELAY_OFF);
    ohMotorRunning = false;
    ugMotorRunning = false;
    Log(INFO, "[Motor] Relay pins init: OH=" + String(OH_RELAY_PIN)
              + " UG=" + String(UG_RELAY_PIN));
}

// ---------------------------------------------------------------------------
//  Config persistence
// ---------------------------------------------------------------------------

void loadMotorConfig() {
    preferences.begin(NVS_MOTOR_NS, true);
    ohDisplayOnly      = preferences.getBool (NVS_KEY_OH_DISP_ONLY, false);
    ugDisplayOnly      = preferences.getBool (NVS_KEY_UG_DISP_ONLY, false);
    ugIgnoreForOH      = preferences.getBool (NVS_KEY_UG_IGNORE,    false);
    buzzerDelayEnabled = preferences.getBool (NVS_KEY_BUZZER_DELAY, true);
    manualAutoStop     = preferences.getBool (NVS_KEY_MANUAL_ASTOP, true);
    lcdBacklightMode   = preferences.getUChar(NVS_KEY_LCD_BL_MODE,  LCD_BL_AUTO);
    ohStartLevel       = (TankState)preferences.getUChar(NVS_KEY_OH_START_LVL, TANK_STATE_EMPTY);
    ohStopLevel        = (TankState)preferences.getUChar(NVS_KEY_OH_STOP_LVL,  TANK_STATE_FULL);
    ohMaxRunMin        = preferences.getUChar(NVS_KEY_OH_MAX_RUN, 20);
    if (ohMaxRunMin < 5)  ohMaxRunMin = 5;
    if (ohMaxRunMin > 60) ohMaxRunMin = 60;
    mqttWatchdogMin    = preferences.getUChar(NVS_KEY_MQTT_WATCHDOG, MQTT_WATCHDOG_DEFAULT_MIN);
    if (mqttWatchdogMin < MQTT_WATCHDOG_MIN_MIN) mqttWatchdogMin = MQTT_WATCHDOG_MIN_MIN;
    if (mqttWatchdogMin > MQTT_WATCHDOG_MAX_MIN) mqttWatchdogMin = MQTT_WATCHDOG_MAX_MIN;
    preferences.end();
    Log(INFO, "[Motor] Config loaded: ohDisp=" + String(ohDisplayOnly)
              + " ugDisp=" + String(ugDisplayOnly)
              + " ugIgnore=" + String(ugIgnoreForOH)
              + " buzzerDelay=" + String(buzzerDelayEnabled)
              + " manualAutoStop=" + String(manualAutoStop)
              + " lcdBl=" + String(lcdBacklightMode)
              + " ohStart=" + tankStateStr(ohStartLevel)
              + " ohStop=" + tankStateStr(ohStopLevel)
              + " ohMaxRun=" + String(ohMaxRunMin) + "min"
              + " mqttWatchdog=" + String(mqttWatchdogMin) + "min");
}

void saveMotorConfig() {
    preferences.begin(NVS_MOTOR_NS, false);
    preferences.putBool (NVS_KEY_OH_DISP_ONLY, ohDisplayOnly);
    preferences.putBool (NVS_KEY_UG_DISP_ONLY, ugDisplayOnly);
    preferences.putBool (NVS_KEY_UG_IGNORE,    ugIgnoreForOH);
    preferences.putBool (NVS_KEY_BUZZER_DELAY, buzzerDelayEnabled);
    preferences.putBool (NVS_KEY_MANUAL_ASTOP, manualAutoStop);
    preferences.putUChar(NVS_KEY_LCD_BL_MODE,  lcdBacklightMode);
    preferences.putUChar(NVS_KEY_OH_START_LVL, (uint8_t)ohStartLevel);
    preferences.putUChar(NVS_KEY_OH_STOP_LVL,  (uint8_t)ohStopLevel);
    preferences.putUChar(NVS_KEY_OH_MAX_RUN,   ohMaxRunMin);
    preferences.putUChar(NVS_KEY_MQTT_WATCHDOG, mqttWatchdogMin);
    preferences.end();
    Log(INFO, "[Motor] Config saved");
}

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

static void energiseOHRelay(bool on, uint8_t reason) {
    digitalWrite(OH_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
    ohMotorRunning = on;
    ohLastReason = reason;
    if (on) ohMotorStartedMs = millis();
    // Persist motor intent for power failure recovery
    preferences.begin(NVS_MOTOR_NS, false);
    preferences.putBool(NVS_KEY_OH_MOTOR_INTENT, on);
    preferences.end();
    Log(INFO, String("[Motor] OH relay ") + (on ? "ON" : "OFF"));
    addHistoryRecord(on ? HIST_MOTOR_OH_ON : HIST_MOTOR_OH_OFF, ohTankState, ugTankState, reason);
}

static void energiseUGRelay(bool on, uint8_t reason) {
    digitalWrite(UG_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
    ugMotorRunning = on;
    ugLastReason = reason;
    if (on) ugMotorStartedMs = millis();
    preferences.begin(NVS_MOTOR_NS, false);
    preferences.putBool(NVS_KEY_UG_MOTOR_INTENT, on);
    preferences.end();
    Log(INFO, String("[Motor] UG relay ") + (on ? "ON" : "OFF"));
    addHistoryRecord(on ? HIST_MOTOR_UG_ON : HIST_MOTOR_UG_OFF, ohTankState, ugTankState, reason);
}

// Stop the pre-start warning buzzer, but ONLY when neither motor is still in
// its buzzer-delay countdown.  Otherwise cancelling/starting one motor would
// silence the warning buzzer of the other motor that is still counting down.
static void stopMotorBuzzer() {
    if (!ohMotorStartPending && !ugMotorStartPending) stopBuzzer();
}

// ---------------------------------------------------------------------------
//  OH auto-control
//  Rule:
//    TANK_STATE_FULL  → turn motor OFF  (safety – always honoured)
//    TANK_STATE_LOW   → turn motor ON   (only if UG has water, or ugIgnoreForOH)
//    TANK_STATE_UNKNOWN → turn motor OFF (safety)
// ---------------------------------------------------------------------------

void autoControlOHMotor() {
    if (millis() < BOOT_GRACE_PERIOD_MS) return;  // wait for sensors to settle after boot
    if (ohDisplayOnly) return;

    bool ugOk = ugIgnoreForOH || (ugTankState == TANK_STATE_FULL);

    // --- Max runtime safety (applies to ALL sources) ---
    if (ohMotorRunning) {
        unsigned long runMs = millis() - ohMotorStartedMs;
        if (runMs >= (unsigned long)ohMaxRunMin * 60000UL) {
            Log(WARN, "[Motor] OH OFF – max runtime " + String(ohMaxRunMin) + " min exceeded");
            ohMotorStartPending = false;
            stopMotorBuzzer();
            energiseOHRelay(false, REASON_MAX_RUNTIME);
            ohMotorSource = MOTOR_SRC_NONE;
            return;
        }
    }

    // --- LoRa safety: stop motor if no signal for 5 min (applies to ALL sources) ---
    if (ohMotorRunning && lastLoraReceivedTime > 0) {
        if (millis() - lastLoraReceivedTime >= LORA_MOTOR_SAFETY_TIMEOUT_MS) {
            Log(WARN, "[Motor] OH OFF – no LoRa signal for 5 min while motor running");
            ohMotorStartPending = false;
            stopMotorBuzzer();
            // Intent stays true in NVS (energiseOHRelay(false) clears it,
            // but we want to resume after LoRa returns), so set it back
            energiseOHRelay(false, REASON_LORA_LOST);
            // Re-set intent so power recovery can resume
            preferences.begin(NVS_MOTOR_NS, false);
            preferences.putBool(NVS_KEY_OH_MOTOR_INTENT, true);
            preferences.end();
            ohMotorSource = MOTOR_SRC_NONE;
            return;
        }
    }

    // --- Tank FULL safety stop (overflow protection) ---
    // FULL is the physical max, so stop immediately — bypass the min-run
    // hysteresis (mirrors the UG float-switch behaviour where FULL stops at
    // once). Scheduled runs keep their existing immunity; a manual run is
    // honoured only when manualAutoStop is enabled.
    if (ohTankState == TANK_STATE_FULL && ohMotorRunning
        && ohMotorSource != MOTOR_SRC_SCHEDULED
        && (ohMotorSource != MOTOR_SRC_MANUAL || manualAutoStop)) {
        Log(INFO, "[Motor] OH " + String(ohMotorSource == MOTOR_SRC_MANUAL ? "MANUAL" : "AUTO")
                  + " OFF – tank FULL (safety)");
        ohMotorStartPending = false;
        stopMotorBuzzer();
        energiseOHRelay(false, REASON_AUTO_FULL);
        ohMotorSource = MOTOR_SRC_NONE;
        return;
    }

    // --- Hysteresis: don't stop motor within MOTOR_MIN_RUN_MS ---
    if (ohMotorRunning && (millis() - ohMotorStartedMs < MOTOR_MIN_RUN_MS)) return;

    // Scheduler-started motors are immune to auto-start/stop logic below
    if (ohMotorSource == MOTOR_SRC_SCHEDULED) return;

    // --- Stop motor when tank reaches stop threshold ---
    if (ohTankState >= ohStopLevel && ohMotorRunning
        && (ohMotorSource != MOTOR_SRC_MANUAL || manualAutoStop)) {
        Log(INFO, "[Motor] OH " + String(ohMotorSource == MOTOR_SRC_MANUAL ? "MANUAL" : "AUTO")
                  + " OFF – tank reached " + String(tankStateStr(ohTankState)));
        ohMotorStartPending = false;
        stopMotorBuzzer();
        energiseOHRelay(false, REASON_AUTO_FULL);
        ohMotorSource = MOTOR_SRC_NONE;
        return;
    }

    // UNKNOWN while running: keep running (don't abort a fill on lost LoRa signal)
    if (ohTankState == TANK_STATE_UNKNOWN && ohMotorRunning) return;

    // Cancel a pending (buzzer-delay) AUTO start if state is no longer at/below start level
    if (ohMotorStartPending && ohMotorSource == MOTOR_SRC_AUTO && ohTankState > ohStartLevel) {
        Log(WARN, "[Motor] OH AUTO cancel pending – state above start level");
        ohMotorStartPending = false;
        stopMotorBuzzer();
        ohMotorSource = MOTOR_SRC_NONE;
        return;
    }

    // --- Start motor when tank at/below start threshold ---
    if (ohTankState != TANK_STATE_UNKNOWN && ohTankState <= ohStartLevel
        && !ohMotorRunning && !ohMotorStartPending) {
        if (!ugOk) {
            Log(WARN, "[Motor] OH AUTO ON blocked – UG tank not ready (state="
                      + String(tankStateStr(ugTankState)) + ")");
            return;
        }
        ohMotorSource = MOTOR_SRC_AUTO;
        ohPendingReason = REASON_AUTO;
        if (buzzerDelayEnabled) {
            Log(INFO, "[Motor] OH AUTO ON pending – buzzer delay started");
            ohMotorStartPending = true;
            ohMotorPendingStart = millis();
            startBuzzer(BUZZER_COUNTDOWN);
        } else {
            energiseOHRelay(true, REASON_AUTO);
        }
    }
}

void autoControlUGMotor() {
    if (millis() < BOOT_GRACE_PERIOD_MS) return;
    if (ugDisplayOnly) return;

    // --- Max runtime safety (applies to ALL sources) ---
    if (ugMotorRunning) {
        unsigned long runMs = millis() - ugMotorStartedMs;
        if (runMs >= (unsigned long)ohMaxRunMin * 60000UL) {
            Log(WARN, "[Motor] UG OFF – max runtime " + String(ohMaxRunMin) + " min exceeded");
            ugMotorStartPending = false;
            stopMotorBuzzer();
            energiseUGRelay(false, REASON_MAX_RUNTIME);
            ugMotorSource = MOTOR_SRC_NONE;
            return;
        }
    }

    // --- LoRa safety: stop motor if no signal for 5 min (applies to ALL sources) ---
    if (ugMotorRunning && lastLoraReceivedTime > 0) {
        if (millis() - lastLoraReceivedTime >= LORA_MOTOR_SAFETY_TIMEOUT_MS) {
            Log(WARN, "[Motor] UG OFF – no LoRa signal for 5 min while motor running");
            ugMotorStartPending = false;
            stopMotorBuzzer();
            energiseUGRelay(false, REASON_LORA_LOST);
            preferences.begin(NVS_MOTOR_NS, false);
            preferences.putBool(NVS_KEY_UG_MOTOR_INTENT, true);
            preferences.end();
            ugMotorSource = MOTOR_SRC_NONE;
            return;
        }
    }

    // Scheduler-started motors are immune to auto-start/stop logic below
    if (ugMotorSource == MOTOR_SRC_SCHEDULED) return;

    if (ugTankState == TANK_STATE_FULL && ugMotorRunning
        && (ugMotorSource != MOTOR_SRC_MANUAL || manualAutoStop)) {
        Log(INFO, "[Motor] UG " + String(ugMotorSource == MOTOR_SRC_MANUAL ? "MANUAL" : "AUTO")
                  + " OFF – tank full");
        ugMotorStartPending = false;
        energiseUGRelay(false, REASON_AUTO_FULL);
        ugMotorSource = MOTOR_SRC_NONE;
        return;
    }
    if (ugTankState == TANK_STATE_UNKNOWN && ugMotorRunning) return;

    if (ugMotorStartPending && ugMotorSource == MOTOR_SRC_AUTO && ugTankState != TANK_STATE_LOW) {
        Log(WARN, "[Motor] UG AUTO cancel pending – state no longer LOW");
        ugMotorStartPending = false;
        stopMotorBuzzer();
        ugMotorSource = MOTOR_SRC_NONE;
        return;
    }

    if (ugTankState == TANK_STATE_LOW && !ugMotorRunning && !ugMotorStartPending) {
        ugMotorSource = MOTOR_SRC_AUTO;
        ugPendingReason = REASON_AUTO;
        if (buzzerDelayEnabled) {
            Log(INFO, "[Motor] UG AUTO ON pending – buzzer delay");
            ugMotorStartPending = true;
            ugMotorPendingStart = millis();
            startBuzzer(BUZZER_COUNTDOWN);
        } else {
            energiseUGRelay(true, REASON_AUTO);
        }
    }
}

// ---------------------------------------------------------------------------
//  Pending motor start processing (buzzer delay)
// ---------------------------------------------------------------------------

void processPendingMotorStarts() {
    unsigned long now = millis();

    if (ohMotorStartPending && now - ohMotorPendingStart >= MOTOR_START_BUZZER_DELAY_MS) {
        ohMotorStartPending = false;
        stopMotorBuzzer();
        if (ohMotorSource == MOTOR_SRC_MANUAL || ohMotorSource == MOTOR_SRC_SCHEDULED) {
            // Don't energise into an already-FULL OH tank when auto-stop is on —
            // it would only run for one loop before the safety stop fires.
            if (ohMotorSource == MOTOR_SRC_MANUAL && manualAutoStop
                && ohTankState == TANK_STATE_FULL) {
                Log(INFO, "[Motor] OH MANUAL start cancelled – tank already FULL");
                ohRejectCode  = MOTOR_REJ_TANK_FULL;
                ohMotorSource = MOTOR_SRC_NONE;
            } else {
                energiseOHRelay(true, ohPendingReason);
            }
        } else {
            // Auto start: only proceed if still at/below start level and not display-only
            if (ohTankState != TANK_STATE_UNKNOWN && ohTankState <= ohStartLevel && !ohDisplayOnly) {
                energiseOHRelay(true, ohPendingReason);
            } else {
                ohMotorSource = MOTOR_SRC_NONE;
            }
        }
        // Immediate status push — the buzzer-delay countdown just ended (started,
        // cancelled, or blocked), so the app shouldn't have to wait up to
        // MQTT_PUBLISH_MS for the next periodic tick to find out.
        publishMQTTStatus();
    }

    if (ugMotorStartPending && now - ugMotorPendingStart >= MOTOR_START_BUZZER_DELAY_MS) {
        ugMotorStartPending = false;
        stopMotorBuzzer();
        if (ugMotorSource == MOTOR_SRC_MANUAL || ugMotorSource == MOTOR_SRC_SCHEDULED) {
            energiseUGRelay(true, ugPendingReason);
        } else {
            if (ugTankState == TANK_STATE_LOW && !ugDisplayOnly) {
                energiseUGRelay(true, ugPendingReason);
            } else {
                ugMotorSource = MOTOR_SRC_NONE;
            }
        }
        publishMQTTStatus();
    }

    // --- Boot recovery: check NVS motor intent after first valid LoRa + UG data ---
    if (!bootRecoveryDone && millis() > BOOT_GRACE_PERIOD_MS
        && ohTankState != TANK_STATE_UNKNOWN && ugTankState != TANK_STATE_UNKNOWN) {
        bootRecoveryDone = true;
        preferences.begin(NVS_MOTOR_NS, true);
        bool ohIntent = preferences.getBool(NVS_KEY_OH_MOTOR_INTENT, false);
        bool ugIntent = preferences.getBool(NVS_KEY_UG_MOTOR_INTENT, false);
        preferences.end();

        if (ohIntent && !ohMotorRunning && ohTankState < ohStopLevel) {
            bool ugOk = ugIgnoreForOH || (ugTankState == TANK_STATE_FULL);
            if (ugOk && !ohDisplayOnly) {
                Log(INFO, "[Motor] Boot recovery: resuming OH motor (intent=true, state="
                    + String(tankStateStr(ohTankState)) + ")");
                ohMotorSource = MOTOR_SRC_AUTO;
                ohPendingReason = REASON_POWER_RESTORE;
                if (buzzerDelayEnabled) {
                    ohMotorStartPending = true;
                    ohMotorPendingStart = millis();
                    startBuzzer(BUZZER_COUNTDOWN);
                } else {
                    energiseOHRelay(true, REASON_POWER_RESTORE);
                }
            }
        } else if (ohIntent) {
            // Clear stale intent — tank already reached stop level
            preferences.begin(NVS_MOTOR_NS, false);
            preferences.putBool(NVS_KEY_OH_MOTOR_INTENT, false);
            preferences.end();
            Log(INFO, "[Motor] Boot recovery: OH intent cleared (tank at " + String(tankStateStr(ohTankState)) + ")");
        }

        if (ugIntent && !ugMotorRunning && ugTankState != TANK_STATE_FULL) {
            if (!ugDisplayOnly) {
                Log(INFO, "[Motor] Boot recovery: resuming UG motor");
                ugMotorSource = MOTOR_SRC_AUTO;
                ugPendingReason = REASON_POWER_RESTORE;
                if (buzzerDelayEnabled) {
                    ugMotorStartPending = true;
                    ugMotorPendingStart = millis();
                    startBuzzer(BUZZER_COUNTDOWN);
                } else {
                    energiseUGRelay(true, REASON_POWER_RESTORE);
                }
            }
        } else if (ugIntent) {
            preferences.begin(NVS_MOTOR_NS, false);
            preferences.putBool(NVS_KEY_UG_MOTOR_INTENT, false);
            preferences.end();
            Log(INFO, "[Motor] Boot recovery: UG intent cleared");
        }
    }
}

// ---------------------------------------------------------------------------
//  Manual commands
// ---------------------------------------------------------------------------

void turnOnOHMotor(uint8_t reason) {
    ohRejectCode = MOTOR_REJ_NONE;
    if (ohMotorRunning || ohMotorStartPending) return;
    // Refuse up-front rather than buzzing for 30 s and cancelling later (or
    // energising for a single loop before the FULL safety stop fires). The
    // reject code is published so the app can explain the no-op.
    if (manualAutoStop && ohTankState == TANK_STATE_FULL) {
        ohRejectCode = MOTOR_REJ_TANK_FULL;
        Log(INFO, "[Motor] OH MANUAL start refused – tank already FULL");
        return;
    }
    if (ohMotorSource == MOTOR_SRC_NONE) ohMotorSource = MOTOR_SRC_MANUAL;
    ohPendingReason = reason;
    if (buzzerDelayEnabled) {
        ohMotorStartPending = true;
        ohMotorPendingStart = millis();
        startBuzzer(BUZZER_COUNTDOWN);
        Log(INFO, "[Motor] OH ON – buzzer delay started");
    } else {
        energiseOHRelay(true, reason);
    }
}

void turnOffOHMotor(uint8_t reason) {
    ohRejectCode = MOTOR_REJ_NONE;
    const bool wasRunning = ohMotorRunning;
    ohMotorStartPending   = false;
    ohMotorSource         = MOTOR_SRC_NONE;
    stopMotorBuzzer();
    if (wasRunning) {
        energiseOHRelay(false, reason);   // real stop — logs OFF + clears intent
    }
    // else: pending start cancelled (or already off) — nothing energised, no OFF record
}

void turnOnUGMotor(uint8_t reason) {
    ugRejectCode = MOTOR_REJ_NONE;
    if (ugMotorRunning || ugMotorStartPending) return;
    // Same rationale as turnOnOHMotor() — the FULL stop at the top of
    // updateUGMotorControl() would cut it short immediately.
    if (manualAutoStop && ugTankState == TANK_STATE_FULL) {
        ugRejectCode = MOTOR_REJ_TANK_FULL;
        Log(INFO, "[Motor] UG MANUAL start refused – tank already FULL");
        return;
    }
    if (ugMotorSource == MOTOR_SRC_NONE) ugMotorSource = MOTOR_SRC_MANUAL;
    ugPendingReason = reason;
    if (buzzerDelayEnabled) {
        ugMotorStartPending = true;
        ugMotorPendingStart = millis();
        startBuzzer(BUZZER_COUNTDOWN);
        Log(INFO, "[Motor] UG ON – buzzer delay started");
    } else {
        energiseUGRelay(true, reason);
    }
}

void turnOffUGMotor(uint8_t reason) {
    ugRejectCode = MOTOR_REJ_NONE;
    const bool wasRunning = ugMotorRunning;
    ugMotorStartPending   = false;
    ugMotorSource         = MOTOR_SRC_NONE;
    stopMotorBuzzer();
    if (wasRunning) {
        energiseUGRelay(false, reason);   // real stop — logs OFF + clears intent
    }
    // else: pending start cancelled (or already off) — nothing energised, no OFF record
}

// ---------------------------------------------------------------------------
//  Scheduled starts — buzzer pre-warned BEFORE the scheduled time by the
//  caller (Scheduler.cpp), so the relay energises immediately here with no
//  further delay. This keeps the motor's actual run time equal to the
//  configured schedule duration (previously the shared buzzer-delay pending
//  flow ran the 30 s warning AFTER the trigger, so the relay energised late
//  and the motor ran ~30 s short of the configured duration).
// ---------------------------------------------------------------------------

void preWarnBuzzer() {
    startBuzzer(BUZZER_COUNTDOWN);
}

void startScheduledOHMotor(uint8_t reason) {
    if (ohMotorRunning) return;
    ohMotorStartPending = false;   // any unrelated auto/manual pending start is superseded
    ohMotorSource = MOTOR_SRC_SCHEDULED;
    stopMotorBuzzer();
    energiseOHRelay(true, reason);
}

void startScheduledUGMotor(uint8_t reason) {
    if (ugMotorRunning) return;
    ugMotorStartPending = false;
    ugMotorSource = MOTOR_SRC_SCHEDULED;
    stopMotorBuzzer();
    energiseUGRelay(true, reason);
}

bool isOHBuzzerPending() { return ohMotorStartPending; }
bool isUGBuzzerPending() { return ugMotorStartPending; }

int getPendingStartCountdown(bool* isOH) {
    unsigned long now = millis();
    if (ohMotorStartPending) {
        if (isOH) *isOH = true;
        long rem = (long)MOTOR_START_BUZZER_DELAY_MS - (long)(now - ohMotorPendingStart);
        return rem > 0 ? (int)(rem / 1000) : 0;
    }
    if (ugMotorStartPending) {
        if (isOH) *isOH = false;
        long rem = (long)MOTOR_START_BUZZER_DELAY_MS - (long)(now - ugMotorPendingStart);
        return rem > 0 ? (int)(rem / 1000) : 0;
    }
    return -1;
}

int getOHStartCountdown() {
    if (!ohMotorStartPending) return 0;
    long rem = (long)MOTOR_START_BUZZER_DELAY_MS - (long)(millis() - ohMotorPendingStart);
    return rem > 0 ? (int)((rem + 999) / 1000) : 0;
}

int getUGStartCountdown() {
    if (!ugMotorStartPending) return 0;
    long rem = (long)MOTOR_START_BUZZER_DELAY_MS - (long)(millis() - ugMotorPendingStart);
    return rem > 0 ? (int)((rem + 999) / 1000) : 0;
}

uint8_t getOHLastReason() { return ohLastReason; }
uint8_t getUGLastReason() { return ugLastReason; }

uint8_t getOHRejectCode() { return ohRejectCode; }
uint8_t getUGRejectCode() { return ugRejectCode; }

// ---------------------------------------------------------------------------
//  Power-cut recovery + heartbeat
// ---------------------------------------------------------------------------

// Called from loop().  While a motor is running, periodically persist the
// current epoch to NVS so that, after an unexpected power loss, we can estimate
// when the motor actually stopped.  Only writes while pumping → minimal wear.
void motorHeartbeat() {
    if (!ohMotorRunning && !ugMotorRunning) return;
    if (timeStatus() == timeNotSet) return;
    static unsigned long lastHbMs = 0;
    if (lastHbMs != 0 && millis() - lastHbMs < 30000UL) return;
    lastHbMs = millis();
    preferences.begin(NVS_MOTOR_NS, false);
    preferences.putUInt(NVS_KEY_HEARTBEAT, (uint32_t)now());
    preferences.end();
}

// Called once from setup() AFTER initHistory().  If the last reset was a power
// event and a motor was still intended ON, the clean OFF was never written —
// synthesize it, backdated to the last heartbeat (best estimate of the cut).
void checkPowerCutRecovery() {
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT) return;

    preferences.begin(NVS_MOTOR_NS, true);
    bool     ohIntent = preferences.getBool(NVS_KEY_OH_MOTOR_INTENT, false);
    bool     ugIntent = preferences.getBool(NVS_KEY_UG_MOTOR_INTENT, false);
    uint32_t hb       = preferences.getUInt(NVS_KEY_HEARTBEAT, 0);
    preferences.end();

    if (!ohIntent && !ugIntent) return;
    uint32_t offTs = hb ? hb : (uint32_t)now();

    if (ohIntent) {
        addHistoryRecord(HIST_MOTOR_OH_OFF, ohTankState, ugTankState, REASON_POWER_CUT, offTs);
        Log(WARN, "[Motor] Power-cut inferred – OH motor OFF logged (backdated)");
    }
    if (ugIntent) {
        addHistoryRecord(HIST_MOTOR_UG_OFF, ohTankState, ugTankState, REASON_POWER_CUT, offTs);
        Log(WARN, "[Motor] Power-cut inferred – UG motor OFF logged (backdated)");
    }
}
