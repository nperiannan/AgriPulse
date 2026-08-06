#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <Arduino.h>
#include "History.h"   // HistReason codes used as default arguments below

// Initialise relay GPIO pins (OUTPUT, relay OFF at boot).
void initMotorPins();

// Load display-only / ignore-UG settings from NVS.
void loadMotorConfig();

// Save current display-only / ignore-UG settings to NVS.
void saveMotorConfig();

// Run automatic OH motor control based on ohTankState.
// Call from loop() whenever ohTankState may have changed.
void autoControlOHMotor();

// Run automatic UG motor control based on ugTankState.
// Call from loop() whenever ugTankState may have changed.
void autoControlUGMotor();

// Manual motor commands (from BLE / web UI / touch).  `reason` records who
// initiated it (REASON_MANUAL_APP / _WEB / _TOUCH / REASON_SCHEDULED).
void turnOnOHMotor(uint8_t reason = REASON_MANUAL_WEB);
void turnOffOHMotor(uint8_t reason = REASON_MANUAL_WEB);
void turnOnUGMotor(uint8_t reason = REASON_MANUAL_WEB);
void turnOffUGMotor(uint8_t reason = REASON_MANUAL_WEB);

// Sound the pre-start warning buzzer ahead of time, without changing any motor
// state. Used by the scheduler to warn MOTOR_START_BUZZER_DELAY_MS *before*
// the scheduled time, so the relay can energise exactly on time and run for
// the full configured duration (previously the buzzer ran AFTER the trigger,
// delaying the relay and silently shortening the scheduled run by 30 s).
void preWarnBuzzer();

// Energise the OH/UG relay for a scheduled start immediately, bypassing the
// buzzer-delay pending-start flow — the scheduler already sounded the warning
// via preWarnBuzzer() before the scheduled time arrived.
void startScheduledOHMotor(uint8_t reason = REASON_SCHEDULED);
void startScheduledUGMotor(uint8_t reason = REASON_SCHEDULED);

// Check and handle pending motor starts (after buzzer delay).
void processPendingMotorStarts();

// Persist a heartbeat epoch while pumping (for power-cut inference). Call from loop().
void motorHeartbeat();

// At boot (after initHistory): log a synthetic power-cut OFF if a motor was
// running when power was lost. Call once from setup().
void checkPowerCutRecovery();

// Per-motor buzzer-pending state (true while buzzer countdown is running for that motor).
bool isOHBuzzerPending();
bool isUGBuzzerPending();

// Seconds until the pending motor energises (buzzer-delay countdown), or -1 if
// none is pending. *isOH is set true when OH is pending, false when UG.
int getPendingStartCountdown(bool* isOH);

// Per-motor buzzer-delay countdown remaining seconds (0 when not counting).
// Rounded up so the app's progress bar stays visible until the motor starts.
int getOHStartCountdown();
int getUGStartCountdown();

// Reason code (HistReason) of the most recent OH/UG relay change. Published in
// the status JSON so cloud-derived history can show the real cause.
uint8_t getOHLastReason();
uint8_t getUGLastReason();

// MOTOR_REJ_* code explaining why the last manual start request for that motor
// did nothing (MOTOR_REJ_NONE when the last request was honoured). Published in
// the status JSON so the app can say "tank already full" instead of timing out
// with a generic "command not delivered".
uint8_t getOHRejectCode();
uint8_t getUGRejectCode();

#endif // MOTOR_CONTROL_H
