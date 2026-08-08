#include "MotorDrive.h"
#include "ADE7758.h"
#include "MotorProtection.h"
#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "Buzzer.h"
#include "History.h"
#include "Zones.h"   // zonesBoreRoutingValid() - Valve_A/Valve_B routing gate for MOTOR_BORE

#include <Preferences.h>

static MotorDriveState state = MDRV_DISABLED;
static MotorId  selected      = MOTOR_WELL;
static MotorId  requested     = MOTOR_WELL;
static uint8_t  pendingReason = REASON_NONE;
static ProtTrip lastTrip      = PROT_OK;
static String   lastRefusalReason;   // non-ProtTrip refusal reason - currently bore routing only

static unsigned long stateSinceMs   = 0;
static unsigned long runningSinceMs = 0;
static unsigned long lastStopMs     = 0;
static bool          everStopped    = false;

static bool          enabled  = false;
static bool          lockout  = false;
static unsigned long minOffMs = MOTOR_MIN_OFF_MS_DEFAULT;

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------
const char* motorDriveStateName(MotorDriveState s) {
    switch (s) {
        case MDRV_DISABLED:        return "disabled";
        case MDRV_IDLE:            return "idle";
        case MDRV_LOCKED_OUT:      return "locked out";
        case MDRV_PRE_START:       return "pre-start warning";
        case MDRV_AWAIT_QUIESCENT: return "waiting for current to fall";
        case MDRV_CHANGEOVER:      return "changeover settling";
        case MDRV_START_PULSE:     return "start pulse";
        case MDRV_START_CONFIRM:   return "confirming start";
        case MDRV_RUNNING:         return "running";
        case MDRV_STOP_PULSE:      return "stop pulse";
        case MDRV_STOP_CONFIRM:    return "confirming stop";
        case MDRV_FAULT:           return "fault";
        case MDRV_WELDED:          return "WELDED CONTACTOR";
    }
    return "?";
}

static void enter(MotorDriveState s) {
    if (state == s) return;
    state = s;
    stateSinceMs = millis();
    Log(INFO, String("[Drive] -> ") + motorDriveStateName(s));
}

static unsigned long inStateMs() { return millis() - stateSinceMs; }

static void setChangeover(MotorId m) {
    digitalWrite(CHANGEOVER_RELAY_PIN, (m == MOTOR_BORE) ? RELAY_ON : RELAY_OFF);
    selected = m;
    Log(INFO, String("[Drive] Changeover -> ") + (m == MOTOR_BORE ? "BORE" : "WELL"));
}

static void pulseStart() { digitalWrite(OH_RELAY_PIN, RELAY_ON); }
static void pulseStop()  { digitalWrite(UG_RELAY_PIN, RELAY_ON); }
static void clearPulses() {
    digitalWrite(OH_RELAY_PIN, RELAY_OFF);
    digitalWrite(UG_RELAY_PIN, RELAY_OFF);
}

// Current below the protection floor means "not turning". Without calibration
// this is unreliable, which is why starts are gated on calibration too.
// ampLow is now per motor (well vs bore often differ in rating) — `selected`
// is whichever one the changeover last chose, correct here whether idle,
// starting or running.
static bool currentFlowing() {
    return adeMaxAmps() >= protConfig().ampLow[selected];
}

static void beginStop(uint8_t reason) {
    pendingReason = reason;
    clearPulses();
    pulseStop();
    enter(MDRV_STOP_PULSE);
}

// -----------------------------------------------------------------------------
//  Persistence
// -----------------------------------------------------------------------------
static void loadSettings() {
    Preferences p;
    p.begin(NVS_DRIVE_NS, true);
    enabled  = p.getBool (NVS_KEY_DRIVE_ENABLED, false);
    lockout  = p.getBool (NVS_KEY_DRIVE_LOCKOUT, false);
    minOffMs = p.getULong(NVS_KEY_DRIVE_MIN_OFF, MOTOR_MIN_OFF_MS_DEFAULT);
    p.end();
}

static void saveBool(const char* key, bool v) {
    Preferences p;
    p.begin(NVS_DRIVE_NS, false);
    p.putBool(key, v);
    p.end();
}

// -----------------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------------
void motorDriveInit() {
    pinMode(CHANGEOVER_RELAY_PIN, OUTPUT);
    digitalWrite(CHANGEOVER_RELAY_PIN, RELAY_OFF);
    clearPulses();

    loadSettings();

    // Legacy relay path retired 2026-08-07 - the drive is now the only motor
    // control path, so it must be on. One-time migration for units whose NVS
    // still holds the old "off by default" value.
    if (!enabled) {
        enabled = true;
        saveBool(NVS_KEY_DRIVE_ENABLED, true);
        Log(WARN, "[Drive] Force-enabled (legacy relay logic is retired, this is now the only motor control path)");
    }
    enter(lockout ? MDRV_LOCKED_OUT : MDRV_IDLE);
    if (lockout) {
        Log(WARN, "[Drive] MAINTENANCE LOCKOUT is active - all starts blocked");
    }
}

bool motorDriveEnabled()  { return enabled; }
MotorDriveState motorDriveState() { return state; }
MotorId motorDriveSelected() { return selected; }
ProtTrip motorDriveLastTrip() { return lastTrip; }
const char* motorDriveLastRefusalReason() { return lastRefusalReason.c_str(); }
bool motorDriveLockedOut() { return lockout; }

bool motorDriveIsRunning() {
    return state == MDRV_RUNNING;
}

bool motorDriveCurrentFlowing() {
    return currentFlowing();
}

unsigned long motorDriveRunningMs() {
    return (state == MDRV_RUNNING) ? (millis() - runningSinceMs) : 0;
}

void motorDriveSetEnabled(bool on) {
    enabled = on;
    saveBool(NVS_KEY_DRIVE_ENABLED, on);
    if (!on) {
        clearPulses();
        enter(MDRV_DISABLED);
    } else {
        enter(lockout ? MDRV_LOCKED_OUT : MDRV_IDLE);
    }
    Log(WARN, String("[Drive] Latching-starter drive ") + (on ? "ENABLED" : "disabled"));
}

void motorDriveSetLockout(bool on) {
    lockout = on;
    saveBool(NVS_KEY_DRIVE_LOCKOUT, on);
    Log(WARN, String("[Drive] Maintenance lockout ") + (on ? "ENGAGED" : "released"));

    if (!enabled) return;
    if (on) {
        // Engaging lockout while running stops the motor: the point is to make
        // the machine safe to approach.
        if (state == MDRV_RUNNING || state == MDRV_START_CONFIRM) {
            beginStop(REASON_LOCKOUT);
        } else if (state == MDRV_IDLE || state == MDRV_PRE_START) {
            stopBuzzer();
            enter(MDRV_LOCKED_OUT);
        }
    } else if (state == MDRV_LOCKED_OUT) {
        enter(MDRV_IDLE);
    }
}

void motorDriveClearFault() {
    if (state == MDRV_WELDED) {
        Log(ERROR, "[Drive] Refusing to clear WELDED state remotely - inspect the contactor");
        return;
    }
    if (state == MDRV_FAULT) {
        lastTrip = PROT_OK;
        enter(lockout ? MDRV_LOCKED_OUT : MDRV_IDLE);
    }
}

bool motorDriveRequestStart(MotorId motor, uint8_t reason) {
    lastRefusalReason = "";

    if (!enabled) return false;

    if (lockout) {
        Log(WARN, "[Drive] Start refused - maintenance lockout");
        return false;
    }
    if (state == MDRV_WELDED) {
        Log(ERROR, "[Drive] Start refused - welded contactor unresolved");
        return false;
    }
    if (state == MDRV_FAULT) {
        Log(WARN, "[Drive] Start refused - clear the fault first");
        return false;
    }
    if (state != MDRV_IDLE) return false;

    if (everStopped && millis() - lastStopMs < minOffMs) {
        unsigned long waitS = (minOffMs - (millis() - lastStopMs)) / 1000UL;
        Log(WARN, "[Drive] Start refused - anti-short-cycle, " + String(waitS) + " s remaining");
        return false;
    }

    // Without a calibrated CT we cannot confirm the motor actually started, so a
    // failed start or a welded contactor would go unnoticed.
    if (!protCurrentTripsArmed()) {
        Log(ERROR, "[Drive] Start refused - CT not calibrated, start cannot be verified");
        return false;
    }

    ProtTrip gate = protCheckStartAllowed();
    if (gate != PROT_OK) {
        lastTrip = gate;
        return false;
    }

    // Bore-specific: exactly one of the two routing valves (repurposed zone
    // relays) must be open, or the bore has nowhere defined to send water.
    // Checked centrally here rather than in each caller (zones pump
    // coordination, touch buttons, web, MQTT) - see Zones.h for why.
    if (motor == MOTOR_BORE) {
        String reason2;
        if (!zonesBoreRoutingValid(&reason2)) {
            lastRefusalReason = reason2;
            Log(WARN, "[Drive] BORE start refused - " + reason2);
            return false;
        }
    }

    requested     = motor;
    pendingReason = reason;
    startBuzzer(BUZZER_COUNTDOWN);
    enter(MDRV_PRE_START);
    return true;
}

void motorDriveRequestStop(uint8_t reason) {
    if (!enabled) return;

    switch (state) {
        case MDRV_PRE_START:
            stopBuzzer();
            enter(MDRV_IDLE);
            break;
        case MDRV_RUNNING:
        case MDRV_START_CONFIRM:
        case MDRV_START_PULSE:
            beginStop(reason);
            break;
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
//  State machine
// -----------------------------------------------------------------------------
void motorDriveTask() {
    if (!enabled) return;
    if (millis() < BOOT_GRACE_PERIOD_MS) return;

    switch (state) {

    case MDRV_DISABLED:
    case MDRV_LOCKED_OUT:
    case MDRV_FAULT:
        break;

    case MDRV_WELDED:
        // Current still flowing after a STOP. Keep the alarm audible; do not
        // retry the stop pulse, which would achieve nothing and mask the fault.
        if (!isBuzzerActive()) startBuzzer(BUZZER_SHORT_BEEPS);
        if (!currentFlowing()) {
            Log(INFO, "[Drive] Current cleared - welded-contactor alarm resolved");
            stopBuzzer();
            lastStopMs  = millis();
            everStopped = true;
            enter(MDRV_IDLE);
        }
        break;

    case MDRV_IDLE:
        // The motor may have been started at the panel. Report it rather than
        // pretending the relay state is the truth.
        if (currentFlowing()) {
            Log(WARN, "[Drive] Current detected while idle - started outside firmware control");
            runningSinceMs = millis();
            enter(MDRV_RUNNING);
        }
        break;

    case MDRV_PRE_START:
        if (inStateMs() >= MOTOR_START_BUZZER_DELAY_MS) {
            stopBuzzer();
            // Re-check the supply: the warning window is long enough for
            // conditions to have changed since the request.
            ProtTrip gate = protCheckStartAllowed();
            if (gate != PROT_OK) {
                lastTrip = gate;
                enter(MDRV_FAULT);
                break;
            }
            enter(requested == selected ? MDRV_START_PULSE : MDRV_AWAIT_QUIESCENT);
            if (requested == selected) pulseStart();
        }
        break;

    case MDRV_AWAIT_QUIESCENT:
        // Break-before-make: never move the changeover under load.
        if (!currentFlowing()) {
            setChangeover(requested);
            enter(MDRV_CHANGEOVER);
        } else if (inStateMs() >= QUIESCENT_TIMEOUT_MS) {
            Log(ERROR, "[Drive] Current never fell - refusing to switch changeover under load");
            lastTrip = PROT_STOP_FAILED;
            enter(MDRV_FAULT);
        }
        break;

    case MDRV_CHANGEOVER:
        if (inStateMs() >= CHANGEOVER_SETTLE_MS) {
            pulseStart();
            enter(MDRV_START_PULSE);
        }
        break;

    case MDRV_START_PULSE:
        if (inStateMs() >= MOTOR_PULSE_MS) {
            clearPulses();
            enter(MDRV_START_CONFIRM);
        }
        break;

    case MDRV_START_CONFIRM: {
        if (currentFlowing()) {
            runningSinceMs = millis();
            addHistoryRecord(HIST_MOTOR_OH_ON, ohTankState, ugTankState, pendingReason);
            enter(MDRV_RUNNING);
            break;
        }
        ProtTrip t = protCheckStartConfirm(inStateMs());
        if (t != PROT_OK) {
            lastTrip = t;
            enter(MDRV_FAULT);
        }
        break;
    }

    case MDRV_RUNNING: {
        if (!currentFlowing()) {
            Log(WARN, "[Drive] Current lost - motor stopped outside firmware control");
            lastStopMs  = millis();
            everStopped = true;
            addHistoryRecord(HIST_MOTOR_OH_OFF, ohTankState, ugTankState, REASON_POWER_CUT);
            enter(MDRV_IDLE);
            break;
        }
        ProtTrip t = protEvaluateRunning(millis() - runningSinceMs);
        if (t != PROT_OK) {
            lastTrip = t;
            beginStop(REASON_PROTECTION);
        }
        break;
    }

    case MDRV_STOP_PULSE:
        if (inStateMs() >= MOTOR_PULSE_MS) {
            clearPulses();
            enter(MDRV_STOP_CONFIRM);
        }
        break;

    case MDRV_STOP_CONFIRM: {
        if (!currentFlowing()) {
            lastStopMs  = millis();
            everStopped = true;
            addHistoryRecord(HIST_MOTOR_OH_OFF, ohTankState, ugTankState, pendingReason);
            enter(lockout ? MDRV_LOCKED_OUT
                          : (lastTrip != PROT_OK ? MDRV_FAULT : MDRV_IDLE));
            break;
        }
        ProtTrip t = protCheckStopConfirm(inStateMs());
        if (t != PROT_OK) {
            lastTrip = t;
            enter(MDRV_WELDED);
        }
        break;
    }
    }
}
