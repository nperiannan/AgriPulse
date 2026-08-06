#include "MotorProtection.h"
#include "ADE7758.h"
#include "Config.h"
#include "Logger.h"

#include <Preferences.h>

static ProtConfig cfg;

// A candidate trip must hold for tripDebounceMs before it is acted on, so one
// noisy sample can't stop a pump. Phase loss bypasses this.
static ProtTrip       pendingTrip     = PROT_OK;
static unsigned long  pendingSinceMs  = 0;

ProtConfig& protConfig() { return cfg; }

bool protIsTripped(ProtTrip t) { return t != PROT_OK; }

const char* protTripName(ProtTrip t) {
    switch (t) {
        case PROT_OK:               return "ok";
        case PROT_METER_UNHEALTHY:  return "meter unhealthy";
        case PROT_UNDER_VOLTAGE:    return "under voltage";
        case PROT_OVER_VOLTAGE:     return "over voltage";
        case PROT_PHASE_MISSING:    return "phase missing";
        case PROT_PHASE_SEQUENCE:   return "phase sequence";
        case PROT_FREQUENCY:        return "frequency out of range";
        case PROT_OVER_CURRENT:     return "overload";
        case PROT_UNDER_CURRENT:    return "low current (dry run)";
        case PROT_IMBALANCE:        return "phase imbalance";
        case PROT_NO_START_CONFIRM: return "start not confirmed";
        case PROT_STOP_FAILED:      return "STOP FAILED - welded contactor";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
//  Configuration
// -----------------------------------------------------------------------------
void protLoadConfig() {
    Preferences p;
    p.begin(NVS_PROT_NS, true);
    cfg.voltLow             = p.getFloat(NVS_KEY_PROT_VLOW,      PROT_VOLT_LOW_DEFAULT);
    cfg.voltHigh            = p.getFloat(NVS_KEY_PROT_VHIGH,     PROT_VOLT_HIGH_DEFAULT);
    cfg.ampLow              = p.getFloat(NVS_KEY_PROT_ILOW,      PROT_AMP_LOW_DEFAULT);
    cfg.ampHigh             = p.getFloat(NVS_KEY_PROT_IHIGH,     PROT_AMP_HIGH_DEFAULT);
    cfg.freqLow             = p.getFloat(NVS_KEY_PROT_FLOW,      PROT_FREQ_LOW_DEFAULT);
    cfg.freqHigh            = p.getFloat(NVS_KEY_PROT_FHIGH,     PROT_FREQ_HIGH_DEFAULT);
    cfg.imbalanceMax        = p.getFloat(NVS_KEY_PROT_IMBAL,     PROT_IMBALANCE_MAX_DEFAULT);
    cfg.inrushBlankMs       = p.getULong(NVS_KEY_PROT_INRUSH,    PROT_INRUSH_BLANK_MS_DEFAULT);
    cfg.dryRunBlankMs       = p.getULong(NVS_KEY_PROT_DRYRUN,    PROT_DRYRUN_BLANK_MS_DEFAULT);
    cfg.tripDebounceMs      = p.getULong(NVS_KEY_PROT_DEBOUNCE,  PROT_TRIP_DEBOUNCE_MS_DEFAULT);
    cfg.startConfirmMs      = p.getULong(NVS_KEY_PROT_STARTCONF, PROT_START_CONFIRM_MS_DEFAULT);
    cfg.stopConfirmMs       = p.getULong(NVS_KEY_PROT_STOPCONF,  PROT_STOP_CONFIRM_MS_DEFAULT);
    cfg.armWhenUncalibrated = p.getBool (NVS_KEY_PROT_ARM_UNCAL, false);
    p.end();
}

void protSaveConfig() {
    Preferences p;
    p.begin(NVS_PROT_NS, false);
    p.putFloat(NVS_KEY_PROT_VLOW,      cfg.voltLow);
    p.putFloat(NVS_KEY_PROT_VHIGH,     cfg.voltHigh);
    p.putFloat(NVS_KEY_PROT_ILOW,      cfg.ampLow);
    p.putFloat(NVS_KEY_PROT_IHIGH,     cfg.ampHigh);
    p.putFloat(NVS_KEY_PROT_FLOW,      cfg.freqLow);
    p.putFloat(NVS_KEY_PROT_FHIGH,     cfg.freqHigh);
    p.putFloat(NVS_KEY_PROT_IMBAL,     cfg.imbalanceMax);
    p.putULong(NVS_KEY_PROT_INRUSH,    cfg.inrushBlankMs);
    p.putULong(NVS_KEY_PROT_DRYRUN,    cfg.dryRunBlankMs);
    p.putULong(NVS_KEY_PROT_DEBOUNCE,  cfg.tripDebounceMs);
    p.putULong(NVS_KEY_PROT_STARTCONF, cfg.startConfirmMs);
    p.putULong(NVS_KEY_PROT_STOPCONF,  cfg.stopConfirmMs);
    p.putBool (NVS_KEY_PROT_ARM_UNCAL, cfg.armWhenUncalibrated);
    p.end();
}

void protInit() {
    protLoadConfig();
    pendingTrip = PROT_OK;
    if (!protCurrentTripsArmed()) {
        Log(WARN, "[Prot] Current trips DISARMED - enter the CT ratio to arm overload "
                  "and dry-run protection");
    }
}

bool protCurrentTripsArmed() {
    return adeCalibration().calibrated || cfg.armWhenUncalibrated;
}

// -----------------------------------------------------------------------------
//  Supply checks (shared by start gate and running evaluation)
// -----------------------------------------------------------------------------
static ProtTrip checkSupply() {
    if (!adeIsHealthy()) return PROT_METER_UNHEALTHY;

    const AdeReadings& r = adeGetReadings();
    if (!r.valid) return PROT_METER_UNHEALTHY;

    for (uint8_t p = 0; p < ADE_PHASE_COUNT; p++) {
        if (!r.phasePresent[p]) return PROT_PHASE_MISSING;
    }
    if (!r.phaseSequenceOk) return PROT_PHASE_SEQUENCE;

    for (uint8_t p = 0; p < ADE_PHASE_COUNT; p++) {
        if (r.volts[p] < cfg.voltLow)  return PROT_UNDER_VOLTAGE;
        if (r.volts[p] > cfg.voltHigh) return PROT_OVER_VOLTAGE;
    }

    // Frequency is only meaningful with the supply live; a dead phase already
    // returned above.
    float f = r.frequency[ADE_PHASE_A];
    if (f > 0.0f && (f < cfg.freqLow || f > cfg.freqHigh)) return PROT_FREQUENCY;

    return PROT_OK;
}

ProtTrip protCheckStartAllowed() {
    ProtTrip t = checkSupply();
    if (t != PROT_OK) {
        Log(WARN, String("[Prot] Start refused - ") + protTripName(t));
    }
    return t;
}

// -----------------------------------------------------------------------------
//  Running evaluation
// -----------------------------------------------------------------------------
static ProtTrip evaluateRunningRaw(unsigned long runningMs) {
    // Hardware events first: the meter flags these itself, so they are both
    // faster and more trustworthy than anything derived from polled RMS.
    uint32_t events = adeTakeEvents();
    if (events & ADE_EVT_ANY_ZXTO) {
        Log(ERROR, "[Prot] Zero-crossing timeout - phase lost while running");
        return PROT_PHASE_MISSING;
    }
    if (events & ADE_EVT_SEQ_ERROR) return PROT_PHASE_SEQUENCE;
    if (events & ADE_EVT_ANY_SAG)   return PROT_UNDER_VOLTAGE;

    ProtTrip supply = checkSupply();
    if (supply != PROT_OK) return supply;

    if (!protCurrentTripsArmed()) return PROT_OK;

    // Over-current only after inrush has passed.
    if (runningMs >= cfg.inrushBlankMs) {
        if (adeMaxAmps() > cfg.ampHigh) return PROT_OVER_CURRENT;

        float imb = adeCurrentImbalance();
        if (imb > cfg.imbalanceMax) return PROT_IMBALANCE;
    }

    // Under-current needs the longer window: flow takes time to establish and a
    // healthy start looks exactly like a dry run until it does.
    if (runningMs >= cfg.dryRunBlankMs) {
        if (adeMaxAmps() < cfg.ampLow) return PROT_UNDER_CURRENT;
    }

    return PROT_OK;
}

ProtTrip protEvaluateRunning(unsigned long runningMs) {
    ProtTrip t = evaluateRunningRaw(runningMs);

    // Phase loss is not debounced — single-phasing burns windings in minutes.
    if (t == PROT_PHASE_MISSING) {
        pendingTrip = PROT_OK;
        return t;
    }

    if (t == PROT_OK) {
        pendingTrip = PROT_OK;
        return PROT_OK;
    }

    if (t != pendingTrip) {
        pendingTrip    = t;
        pendingSinceMs = millis();
        return PROT_OK;
    }

    if (millis() - pendingSinceMs >= cfg.tripDebounceMs) {
        Log(ERROR, String("[Prot] TRIP: ") + protTripName(t));
        pendingTrip = PROT_OK;
        return t;
    }
    return PROT_OK;
}

// -----------------------------------------------------------------------------
//  Latching-starter pulse verification
// -----------------------------------------------------------------------------
ProtTrip protCheckStartConfirm(unsigned long msSincePulse) {
    if (!protCurrentTripsArmed()) return PROT_OK;   // can't judge without scaling

    if (adeMaxAmps() > cfg.ampLow) return PROT_OK;  // current flowing: started

    if (msSincePulse >= cfg.startConfirmMs) {
        Log(ERROR, "[Prot] START pulsed but no current - starter tripped, "
                   "phase missing, or no supply");
        return PROT_NO_START_CONFIRM;
    }
    return PROT_OK;
}

ProtTrip protCheckStopConfirm(unsigned long msSincePulse) {
    if (!protCurrentTripsArmed()) return PROT_OK;

    if (adeMaxAmps() < cfg.ampLow) return PROT_OK;  // current gone: stopped

    if (msSincePulse >= cfg.stopConfirmMs) {
        Log(ERROR, "[Prot] STOP pulsed but current persists - CONTACTOR MAY BE "
                   "WELDED, motor still running");
        return PROT_STOP_FAILED;
    }
    return PROT_OK;
}
