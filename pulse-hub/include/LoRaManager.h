#ifndef LORA_MANAGER_H
#define LORA_MANAGER_H

#include <Arduino.h>
#include "Globals.h"

// Initialise LoRa radio (RFM95/96 on HSPI).
// Returns true on success.
bool initLoRa();

// Poll radio for incoming packets and update ohTankState.
// Call from loop() on every iteration (non-blocking).
void pollLoRa();

// Returns true when LoRa radio is operational.
bool isLoraOperational();

// Returns true when transmitter is declared lost (10+ missed packets).
bool isTransmitterLost();

// Returns RSSI of the last received packet (dBm).
float getLoraRSSI();

// Returns SNR of the last received packet (dB).
float getLoraSNR();

#endif // LORA_MANAGER_H
