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

bool    valveHardwarePresent();     // true if at least one board slot is present right now
bool    valveFullySimulated();      // true if NOTHING is declared or detected — every channel is virtual
uint8_t valveBoardCount();          // number of SLOTS (declared, in order, then auto-detected extras) —
                                     // not the same as how many actually answered; see valveBoardPresent()

// Per-slot info, 0 <= b < valveBoardCount(). Declared addresses occupy
// slots 0..valveDeclaredCount()-1 in declared order — "Expansion Board #N"
// in the UI is slot N-1 — regardless of whether they answer this boot;
// anything auto-detected but undeclared is appended after.
uint8_t      valveBoardAddr(uint8_t b);
ValveBackend valveBoardBackend(uint8_t b);
bool         valveBoardPresent(uint8_t b);   // false = declared but not answering right now
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

// ---------------------------------------------------------------------------
//  Declared expansion-board addresses (web UI, Network tab)
//
//  A PCF8574 with an LCD backpack on it and a bare PCF8574 driving a relay
//  board are electrically identical — I2C has no way to ask a chip "what are
//  you soldered to". Display.cpp's LCD probe therefore just takes the first
//  PCF8574/PCF8574A-shaped address that answers, which is right as long as
//  the real LCD is on the bus every boot, but wrong the moment a relay board
//  ends up being the only (or first-found) PCF8574 present — exactly what
//  happens bench-testing a new board before the LCD is reconnected.
//
//  This lets the address be declared explicitly instead of inferred: once an
//  address is here, Display.cpp's probe skips it unconditionally, so a relay
//  board can never be mistaken for the LCD again regardless of scan order or
//  what else is or isn't connected that boot. valveInit()'s own board
//  detection is unaffected either way — it already finds real boards by
//  probing for the chip, not from this list; declaring an address here is a
//  safety statement ("this is never the display"), not what makes detection
//  work.
// ---------------------------------------------------------------------------
#define VALVE_DECLARED_MAX 4

// Load the declared list from NVS. Call once from setup(), BEFORE
// initDisplay() — that's the only consumer, and it needs the list already
// loaded the first time it probes the bus.
void valveConfigInit();

uint8_t valveDeclaredCount();
uint8_t valveDeclaredAddr(uint8_t i);
bool    valveIsDeclaredExpansion(uint8_t addr);

// Both persist immediately. add() refuses a duplicate, an address outside
// the two ranges a relay board can actually live at (0x20-0x27, 0x38-0x3F),
// or VALVE_DECLARED_MAX already reached.
bool valveAddDeclaredExpansion(uint8_t addr);
bool valveRemoveDeclaredExpansion(uint8_t addr);

#endif // VALVE_CONTROLLER_H
