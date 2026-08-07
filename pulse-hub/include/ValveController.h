#ifndef VALVE_CONTROLLER_H
#define VALVE_CONTROLLER_H

#include <Arduino.h>

// Low-level driver for the I2C relay board(s) that open the zone valves. This
// layer knows about chips, addresses and bits and nothing else — every safety
// rule (how many valves may be open, whether the pump is running) lives in
// Zones.cpp above it. Keeping them apart means the interlocks are enforced in
// one place regardless of how many boards are fitted or which chip they use.
//
// Two chips are supported because both are common on cheap 8-channel boards:
//   PCF8574  — 8-bit I/O expander, one byte written straight to the port
//   MCP23017 — 16-bit expander, port A used
//
// Multiple boards are supported (up to VALVE_BOARDS_MAX) so zone count is not
// tied to a single 8-channel board — each board contributes
// VALVE_CHANNELS_PER_BOARD more addressable channels, and channels are
// numbered globally (board 0 = channels 0-7, board 1 = channels 8-15, ...).
//
// When no board answers at all, the driver runs fully SIMULATED rather than
// failing — every one of the VALVE_CHANNELS virtual channels reports
// available, so the zone/program engines above are fully exercisable on the
// bench with zero hardware: the UI works, runtimes count down, interlocks
// fire, zones can be created against any virtual channel — with the writes
// going to the log instead of to copper. That is what makes the irrigation
// layer (and now the zone-management UI) developable before any board is
// built, rather than blocked on one. Once a real board answers, only ITS
// channels are available; a zone mapped to a channel with no board behind it
// is marked inactive rather than silently treated as simulated.

enum ValveBackend : uint8_t {
    VALVE_BACKEND_NONE = 0,   // nothing detected yet
    VALVE_BACKEND_PCF8574,
    VALVE_BACKEND_MCP23017,
    VALVE_BACKEND_SIMULATED,
};

#define VALVE_BOARDS_MAX          4
#define VALVE_CHANNELS_PER_BOARD  8
#define VALVE_CHANNELS            (VALVE_BOARDS_MAX * VALVE_CHANNELS_PER_BOARD)

// Probe the I2C bus and register every board found (up to VALVE_BOARDS_MAX).
// Safe to call again to re-probe after a board is plugged in. Always leaves
// every channel closed.
void valveInit();

// Re-run detection at runtime (web UI "rescan" button), so fitting a board
// does not require a reboot.
void valveRescan();

bool    valveHardwarePresent();     // true if at least one real board answered
bool    valveFullySimulated();      // true if zero real boards — every channel is virtual
uint8_t valveBoardCount();          // real boards found, 0 in fully-simulated mode

// Per-board info, 0 <= b < valveBoardCount().
uint8_t      valveBoardAddr(uint8_t b);
ValveBackend valveBoardBackend(uint8_t b);
const char*  valveBackendName(ValveBackend be);

// True if this global channel can actually be driven right now — either a
// real board answers for it, or nothing at all was found (fully simulated,
// so every channel is "available" for bench testing).
bool valveChannelAvailable(uint8_t ch);

bool valveBusFault();               // a write has failed since the last success

// Open/close one global channel (0..VALVE_CHANNELS-1). Returns false if the
// channel's board isn't present (and we're not fully simulated) or the write
// itself failed — the caller has already decided this is otherwise permitted.
bool valveSet(uint8_t ch, bool open);

// Close every channel across every board. Used on protection trip, fault,
// and at boot.
void valveAllClosed();

bool    valveIsOpen(uint8_t ch);
uint8_t valveOpenCount();

#endif // VALVE_CONTROLLER_H
