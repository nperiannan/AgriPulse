#ifndef TOUCH_SWITCH_H
#define TOUCH_SWITCH_H

// Initialise push button GPIO pins.
void initTouchSwitches();

// Call from loop() — detects rising-edge touches and toggles motors.
void pollTouchSwitches();

#endif // TOUCH_SWITCH_H
