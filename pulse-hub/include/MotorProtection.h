#ifndef MOTOR_PROTECTION_H
#define MOTOR_PROTECTION_H

#include <Arduino.h>

// Electrical protection for the 3-phase motor.
//
// This is a pure evaluator: it reads the meter and answers "is this safe?".
// It never drives a relay itself — the motor state machine owns that — which
// keeps the safety rules in one auditable place and independent of whoever is
// asking (schedule, app, touch button, power-restore resume).
//
// Deliberately not bypassable: there is no "force" or privilege argument. A
// maintenance override belongs in the state machine as a logged, time-boxed
// state, not as a flag threaded through here.

enum ProtTrip : uint8_t {
    PROT_OK = 0,
    PROT_METER_UNHEALTHY,    // can't see the supply, so can't vouch for it
    PROT_UNDER_VOLTAGE,
    PROT_OVER_VOLTAGE,
    PROT_PHASE_MISSING,      // a phase has no voltage / no zero crossings
    PROT_PHASE_SEQUENCE,     // rotation reversed — pump would run backwards
    PROT_FREQUENCY,
    PROT_OVER_CURRENT,       // jam, locked rotor, single-phasing
    PROT_UNDER_CURRENT,      // dry run, lost prime, closed discharge
    PROT_IMBALANCE,          // uneven phase currents — failing phase
    PROT_NO_START_CONFIRM,   // START pulsed but no current followed
    PROT_STOP_FAILED,        // STOP pulsed but current persists: welded contactor
};

struct ProtConfig {
    float voltLow, voltHigh;      // shared — same incoming supply feeds either motor
    // Per motor (index by MotorId — 0=well, 1=bore — see MotorDrive.h). Current
    // draw is a property of the specific motor's load, not the shared supply,
    // so unlike voltage/frequency this genuinely needs two sets: a well and a
    // bore motor commonly differ in HP/current rating on the same starter.
    float ampLow[2], ampHigh[2];
    float freqLow, freqHigh;      // shared
    float imbalanceMax;
    unsigned long inrushBlankMs;
    unsigned long dryRunBlankMs;
    unsigned long tripDebounceMs;
    unsigned long startConfirmMs;
    unsigned long stopConfirmMs;
    bool armWhenUncalibrated;   // default false — see protCurrentTripsArmed()

    // TEMPORARY maintenance overrides (requested 2026-08-08, meant to be
    // removed later — not a permanent feature). Deliberately still separate
    // booleans rather than one blanket flag, and deliberately still NOT
    // covering everything:
    //   - bypassPreStart only overrides protCheckStartAllowed()'s verdict
    //     (voltage/phase/frequency/meter-health) so a start can proceed
    //     despite a failed reading — the real reading is still logged.
    //   - bypassRunning additionally silences protEvaluateRunning() for the
    //     whole run (over-current, dry-run/under-current, imbalance, phase
    //     loss, sag) — nothing will stop the motor once it's going.
    //   - protCheckStopConfirm() — specifically PROT_STOP_FAILED, welded
    //     contactor — is NEVER bypassed by either flag, alone or together,
    //     and needs no special-case here: with no real current ever having
    //     flowed, the stop-confirm check already sees it gone immediately
    //     and succeeds on its own. Bypassing it deliberately would hide
    //     actual hardware damage behind a "successful" stop.
    //   - Genuine no-hardware bench testing (letting MDRV_START_CONFIRM
    //     reach RUNNING with zero current) is DELIBERATELY NOT controlled by
    //     either flag here — see motorDriveBenchMode()/motorDriveSetBenchMode()
    //     in MotorDrive.h. An earlier version of this feature coupled that
    //     behavior to bypassPreStart && bypassRunning together; an
    //     adversarial safety review (2026-08-08) found that let a REAL start
    //     failure on a REAL motor (blown fuse, tripped overload) be silently
    //     reported as a healthy run whenever both flags happened to still be
    //     set from earlier testing, since nothing distinguished "bench test,
    //     nothing wired" from "real motor, both leniencies on, genuinely
    //     failed". It now lives behind its own independent, RAM-only flag
    //     that cannot be conflated with these two or survive a reboot.
    bool bypassPreStart;
    bool bypassRunning;
};

void protInit();
void protLoadConfig();
void protSaveConfig();
ProtConfig& protConfig();

// Supply-side gate, evaluated before the starter is pulsed. Current is not
// considered here because the motor is stopped.
ProtTrip protCheckStartAllowed();

// Same evaluation without logging, for status polling by the UI.
ProtTrip protSupplyStatus();

// Evaluated continuously while the motor is confirmed running.
// runningMs is time since the motor was *confirmed* running (not since the
// START pulse), and drives the inrush / dry-run blanking windows.
ProtTrip protEvaluateRunning(unsigned long runningMs);

// Pulse verification for the latching starter. msSincePulse is measured from
// the end of the relay pulse.
//   protCheckStartConfirm - PROT_NO_START_CONFIRM once the window expires with
//                           no current: thermal overload, lost phase, no supply.
//   protCheckStopConfirm  - PROT_STOP_FAILED if current outlives the window.
//                           That means the contactor is welded and the motor
//                           cannot be stopped: alarm, do not silently retry.
ProtTrip protCheckStartConfirm(unsigned long msSincePulse);
ProtTrip protCheckStopConfirm(unsigned long msSincePulse);

// False while the CT ratio is still unknown. Current-based trips are suppressed
// (and logged instead) so protection never acts on placeholder scaling.
bool protCurrentTripsArmed();

const char* protTripName(ProtTrip t);
bool protIsTripped(ProtTrip t);

#endif // MOTOR_PROTECTION_H
