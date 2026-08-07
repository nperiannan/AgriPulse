#ifndef VALVE_CONTROLLER_H
#define VALVE_CONTROLLER_H

#include <Arduino.h>

// Low-level driver for the 8-channel I2C relay board that opens the zone
// valves.  This layer knows about chips and bits and nothing else — every
// safety rule (how many valves may be open, whether the pump is running) lives
// in Zones.cpp above it.  Keeping them apart means the interlocks are enforced
// in one place regardless of which board is fitted.
//
// Two chips are supported because both are common on cheap 8-channel boards:
//   PCF8574  — 8-bit I/O expander, one byte written straight to the port
//   MCP23017 — 16-bit expander, port A used
//
// When no board answers, the driver runs SIMULATED rather than failing. The
// zone and program engines above are then fully exercisable on the bench — the
// UI works, runtimes count down, interlocks fire — with the valve writes going
// to the log instead of to copper. That is what makes the irrigation layer
// developable before the board is built, rather than blocked on it.

enum ValveBackend : uint8_t {
    VALVE_BACKEND_NONE = 0,   // nothing detected yet
    VALVE_BACKEND_PCF8574,
    VALVE_BACKEND_MCP23017,
    VALVE_BACKEND_SIMULATED,
};

#define VALVE_CHANNELS 8

// Probe the I2C bus and pick a backend. Safe to call again to re-probe after a
// board is plugged in. Always leaves every channel closed.
void valveInit();

// Re-run detection at runtime (web UI "rescan" button), so fitting the board
// does not require a reboot.
void valveRescan();

bool         valveHardwarePresent();
ValveBackend valveBackendKind();
const char*  valveBackendName();
uint8_t      valveI2CAddress();      // 0 when simulated

// Open/close one channel. Returns false only on an I2C write failure — the
// caller has already decided this is permitted.
bool valveSet(uint8_t ch, bool open);

// Close every channel. Used on protection trip, fault, and at boot.
void valveAllClosed();

bool    valveIsOpen(uint8_t ch);
uint8_t valveOpenMask();
uint8_t valveOpenCount();

// True when a write has failed since the last successful one — surfaced in the
// UI so a dying board is visible rather than silently ignoring commands.
bool valveBusFault();

#endif // VALVE_CONTROLLER_H
