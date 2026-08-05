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
#include "MotorControl.h"

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

    // --- OH button (active LOW) ---
    bool ohNow = (digitalRead(TOUCH_OH_PIN) == LOW);
    if (ohNow && !lastOHTouch && (now - lastOHTriggerMs >= TOUCH_DEBOUNCE_MS)) {
        lastOHTriggerMs = now;
        if (ohMotorRunning || isOHBuzzerPending()) {
            Log(INFO, "[Touch] OH button → OFF / cancel");
            turnOffOHMotor(REASON_MANUAL_TOUCH);
        } else {
            Log(INFO, "[Touch] OH button → Motor ON");
            turnOnOHMotor(REASON_MANUAL_TOUCH);   // returns early if already running/pending
        }
    }
    lastOHTouch = ohNow;

    // --- UG button (active LOW) ---
    bool ugNow = (digitalRead(TOUCH_UG_PIN) == LOW);
    if (ugNow && !lastUGTouch && (now - lastUGTriggerMs >= TOUCH_DEBOUNCE_MS)) {
        lastUGTriggerMs = now;
        if (ugMotorRunning || isUGBuzzerPending()) {
            Log(INFO, "[Touch] UG button → OFF / cancel");
            turnOffUGMotor(REASON_MANUAL_TOUCH);
        } else {
            Log(INFO, "[Touch] UG button → Motor ON");
            turnOnUGMotor(REASON_MANUAL_TOUCH);   // returns early if already running/pending
        }
    }
    lastUGTouch = ugNow;
}
