#ifndef I2C_SCAN_H
#define I2C_SCAN_H

#include <Arduino.h>

// On-demand I2C bus scanning.
//
// Exists because "nothing was detected" is not a diagnosis: it cannot tell you
// whether the bus is dead, or whether the firmware is simply driving the wrong
// two pins. This board has two documented pinouts (v1.x SDA18/SCL17 and v2.0
// SDA8/SCL9) selected at build time by BOARD_V2, so a scan that can try both
// answers in one boot what a rebuild-and-reflash cycle would take several to.

struct I2CScanResult {
    uint8_t sda;
    uint8_t scl;
    uint8_t found;             // number of devices that ACKed
    uint8_t addrs[16];         // first 16 addresses found
};

// Scan the currently configured bus, without touching its pin assignment.
I2CScanResult i2cScanCurrent();

// NOTE: there is deliberately no "scan an arbitrary pin pair" helper. On the
// v1.x board GPIO8 is LORA_DIO1 and is already owned by RadioLib's interrupt;
// re-pointing Wire at it hangs setup() before the radio and web server start.

// Read the bus at rest, floating, before Wire owns the pins. HIGH means real
// pull-up resistors are fitted; LOW means none are (or nothing is powered).
void i2cReadIdleLevels(bool* sdaHigh, bool* sclHigh);

// Same pins with the internal pull-up engaged. LOW here means a device or short
// is actively clamping the line, which the floating read alone cannot tell you.
void i2cReadPulledLevels(bool* sdaHigh, bool* sclHigh);

// True if SDA and SCL are the same electrical node (shorted). Actively drives
// one line and watches the other, because a crossed bus is indistinguishable
// from a healthy one on every passive reading — it idles HIGH and looks clean,
// yet nothing can ever ACK. Found on this board 2026-08-07.
bool i2cLinesShorted();

// Clock a stuck slave off SDA and issue a STOP. Returns true if SDA came back.
// Must be called before Wire.begin() claims the pins.
bool i2cBusRecover();

// Full boot diagnosis: idle levels, recovery if stuck, scan, and a plain-English
// verdict in the log. Called once at boot while the I2C bus is unexplained.
void i2cDiagnoseBothPinouts();

// JSON for the web UI's "Scan I2C bus" button.
String i2cScanJson();

#endif // I2C_SCAN_H
