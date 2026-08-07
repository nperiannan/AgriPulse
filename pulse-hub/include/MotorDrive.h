#ifndef MOTOR_DRIVE_H
#define MOTOR_DRIVE_H

#include <Arduino.h>
#include "MotorProtection.h"

// Motor drive for the two-wire latching starter plus changeover contactor.
//
// Owns RLY1 (START), RLY2 (STOP) and RLY3 (changeover) exclusively.  Nothing
// else may write those pins while the drive is enabled.
//
// The central idea: relay state is a *request*, measured current is the *fact*.
// Every transition is confirmed against the meter rather than assumed, which is
// what lets the drive detect a starter that refused to latch or a contactor
// that has welded shut.
//
// Disabled by default so it cannot fight the legacy OH/UG relay logic; enable
// deliberately once the wiring is verified on the bench.

enum MotorId : uint8_t {
    MOTOR_WELL = 0,   // changeover de-energised — the default connection
    MOTOR_BORE = 1,   // changeover energised
};

enum MotorDriveState : uint8_t {
    MDRV_DISABLED = 0,
    MDRV_IDLE,
    MDRV_LOCKED_OUT,
    MDRV_PRE_START,        // pre-start warning sounding
    MDRV_AWAIT_QUIESCENT,  // waiting for current to fall before changeover
    MDRV_CHANGEOVER,       // changeover thrown, contactor settling
    MDRV_START_PULSE,      // RLY1 energised
    MDRV_START_CONFIRM,    // pulse ended, waiting for current to appear
    MDRV_RUNNING,          // current confirmed
    MDRV_STOP_PULSE,       // RLY2 energised
    MDRV_STOP_CONFIRM,     // pulse ended, waiting for current to vanish
    MDRV_FAULT,            // tripped or failed to start; needs clearing
    MDRV_WELDED,           // STOP failed and current persists — unrecoverable
};

void motorDriveInit();
void motorDriveTask();

// Returns false if the request was refused outright (locked out, disabled,
// still within the minimum-off window, or the supply gate failed).
bool motorDriveRequestStart(MotorId motor, uint8_t reason);
void motorDriveRequestStop(uint8_t reason);

// Clears FAULT back to IDLE. Deliberately does not clear WELDED — that needs a
// human at the panel, not a button in an app.
void motorDriveClearFault();

MotorDriveState motorDriveState();
const char* motorDriveStateName(MotorDriveState s);

// True only when current confirms the motor is turning (state == MDRV_RUNNING
// specifically — the drive's own idea of "normal operation").
bool motorDriveIsRunning();

// True whenever MEASURED CURRENT says the motor is turning, independent of
// drive state. Deliberately different from motorDriveIsRunning(): during
// MDRV_STOP_PULSE/STOP_CONFIRM the state has already left MDRV_RUNNING but
// current has not yet fallen, and during MDRV_WELDED current is — by
// definition — still confirmed flowing even though state is neither RUNNING
// nor any other "operating" state. Anything that needs to know "is it safe to
// close a valve" must ask this, not motorDriveIsRunning() or the state name.
bool motorDriveCurrentFlowing();

// What the changeover is currently selecting.
MotorId motorDriveSelected();

ProtTrip motorDriveLastTrip();
unsigned long motorDriveRunningMs();

// Maintenance lockout: persisted, survives reboot, blocks every start
// including automatic resume after a power cut.
void motorDriveSetLockout(bool on);
bool motorDriveLockedOut();

bool motorDriveEnabled();
void motorDriveSetEnabled(bool on);

#endif // MOTOR_DRIVE_H
