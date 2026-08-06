#include "Sensors.h"
#include "Logger.h"
#include "Config.h"
#include "History.h"
#include "MQTTManager.h"

// ---------------------------------------------------------------------------
//  UG tank float switch – single-pin, debounced state machine
// ---------------------------------------------------------------------------
//  Wiring (3-wire float switch):
//    NC  → GND
//    COM → GPIO UG_FLOAT_PIN (INPUT)
//    NO  → 3.3 V
//  HIGH = FULL (float up, COM on NO rail)
//  LOW  = EMPTY (float down, COM on NC/GND)
//
//  Boot default: TANK_STATE_FULL – prevents motor from firing before the
//  first stable reading is confirmed after debounce.
// ---------------------------------------------------------------------------

// Boot: pendingState=FULL so the first stable HIGH reading confirms FULL instantly.
// ugTankState starts UNKNOWN; motor logic ignores UNKNOWN (won't auto-start).
static TankState pendingState     = TANK_STATE_FULL;
static unsigned long pendingStart = 0;

void initSensorPins() {
    pinMode(UG_FLOAT_PIN, INPUT_PULLUP); // pullup: disconnected=HIGH=FULL (safe default)
    Log(INFO, "[Sensors] UG float switch pin: " + String(UG_FLOAT_PIN)
              + "  (HIGH=FULL, LOW=EMPTY, pullup enabled)");
}

void readUGFloatSwitch() {
    TankState sampled = (digitalRead(UG_FLOAT_PIN) == HIGH)
                        ? TANK_STATE_FULL
                        : TANK_STATE_LOW;

    if (sampled != pendingState) {
        pendingState = sampled;
        pendingStart = millis();
        Log(DEBUG, "[Sensors] UG float: pending=" + String(tankStateStr(sampled)));
        return;
    }

    if (millis() - pendingStart >= FLOAT_DEBOUNCE_MS) {
        if (sampled != ugTankState) {
            ugTankState = sampled;
            Log(INFO, "[Sensors] UG tank state → " + String(tankStateStr(ugTankState)));
            addHistoryRecord(HIST_UG_STATE_CHG, ohTankState, ugTankState);
            publishMQTTStatus();   // don't wait for the next keep-alive tick
        }
    }
}
