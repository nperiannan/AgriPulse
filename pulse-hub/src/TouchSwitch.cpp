/**
 * TouchSwitch.cpp
 *
 * Handles momentary push buttons for manual motor toggle.
 *
 * Wiring: one side to GPIO, other side to GND.
 * pinMode INPUT_PULLUP → pin reads HIGH at rest, LOW when pressed.
 *
 * We detect the falling edge (HIGH → LOW) and toggle the motor.
 * A 50 ms debounce window prevents double-triggers.
 *
 * Behaviour mirrors the web UI ON/OFF buttons:
 *   - Press while motor OFF (or pending) → turn ON  (respects buzzer delay setting)
 *   - Press while motor ON              → turn OFF
 * The GUI reflects the change on its next 5-second status poll automatically.
 */

#include "TouchSwitch.h"
#include "Config.h"
#include "Logger.h"
#include "Globals.h"
#include "MotorDrive.h"
#include "History.h"   // REASON_MANUAL_TOUCH

// Last raw pin state (for edge detection)
static bool lastOHTouch = false;
static bool lastUGTouch = false;

// Timestamps of last accepted trigger (debounce)
static unsigned long lastOHTriggerMs = 0;
static unsigned long lastUGTriggerMs = 0;

// ---------------------------------------------------------------------------

void initTouchSwitches() {
    pinMode(TOUCH_OH_PIN, INPUT_PULLUP);
    pinMode(TOUCH_UG_PIN, INPUT_PULLUP);
    Log(INFO, "[Touch] Push button pins init: OH=" + String(TOUCH_OH_PIN)
              + " UG=" + String(TOUCH_UG_PIN));
}

// ---------------------------------------------------------------------------

void pollTouchSwitches() {
    unsigned long now = millis();

    // Retired 2026-08-07: real hardware is one starter selected between WELL
    // and BORE via changeover, not two independent OH/UG motors. The two
    // panel buttons now toggle WELL and BORE respectively through the drive.
    bool ohNow = (digitalRead(TOUCH_OH_PIN) == LOW);
    if (ohNow && !lastOHTouch && (now - lastOHTriggerMs >= TOUCH_DEBOUNCE_MS)) {
        lastOHTriggerMs = now;
        if (motorDriveIsRunning() && motorDriveSelected() == MOTOR_WELL) {
            Log(INFO, "[Touch] Well button -> STOP");
            motorDriveRequestStop(REASON_MANUAL_TOUCH);
        } else {
            Log(INFO, "[Touch] Well button -> START");
            motorDriveRequestStart(MOTOR_WELL, REASON_MANUAL_TOUCH);
        }
    }
    lastOHTouch = ohNow;

    bool ugNow = (digitalRead(TOUCH_UG_PIN) == LOW);
    if (ugNow && !lastUGTouch && (now - lastUGTriggerMs >= TOUCH_DEBOUNCE_MS)) {
        lastUGTriggerMs = now;
        if (motorDriveIsRunning() && motorDriveSelected() == MOTOR_BORE) {
            Log(INFO, "[Touch] Bore button -> STOP");
            motorDriveRequestStop(REASON_MANUAL_TOUCH);
        } else {
            Log(INFO, "[Touch] Bore button -> START");
            motorDriveRequestStart(MOTOR_BORE, REASON_MANUAL_TOUCH);
        }
    }
    lastUGTouch = ugNow;
}
