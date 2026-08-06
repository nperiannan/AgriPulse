#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "Globals.h"

// Configure GPIO pins for UG tank float switch (INPUT_PULLUP).
void initSensorPins();

// Poll the UG tank float switch and update ugTankState with debounce.
// Call from loop() on every UG_SENSOR_POLL_MS interval.
void readUGFloatSwitch();

#endif // SENSORS_H
