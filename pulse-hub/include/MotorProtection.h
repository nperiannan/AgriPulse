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
    float voltLow, voltHigh;
    float ampLow, ampHigh;
    float freqLow, freqHigh;
    float imbalanceMax;
    unsigned long inrushBlankMs;
    unsigned long dryRunBlankMs;
    unsigned long tripDebounceMs;
    unsigned long startConfirmMs;
    unsigned long stopConfirmMs;
    bool armWhenUncalibrated;   // default false — see protCurrentTripsArmed()
};

void protInit();
void protLoadConfig();
void protSaveConfig();
ProtConfig& protConfig();

// Supply-side gate, evaluated before the starter is pulsed. Current is not
// considered here because the motor is stopped.
ProtTrip protCheckStartAllowed();

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
