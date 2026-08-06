#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

// Initialise LCD over I2C.
void initDisplay();

// Call from loop() – rotates through LCD screens on a timer.
void updateDisplay();

// Show a BLE pairing PIN on the LCD (blocks normal rotation until cleared).
void displayPairingPin(uint32_t pin);

// Clear the pairing PIN and resume normal display rotation.
void clearPairingPinDisplay();

// Force the LCD backlight on/off.
void setLcdBacklight(bool on);

// Returns true when the LCD is currently backlit.
bool isLcdBacklightOn();

// Detected LCD I2C address (auto-scanned in initDisplay).
uint8_t getLcdAddress();

// Apply the stored lcdBacklightMode (auto / always-on / always-off).
// Called automatically every 30 s from updateDisplay(); can also be
// called explicitly after the mode changes via MQTT.
void applyBacklightMode();

#endif // DISPLAY_H
