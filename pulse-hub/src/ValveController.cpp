#include "ValveController.h"
#include "Config.h"
#include "Logger.h"
#include "Display.h"     // getLcdAddress() — so we never claim the LCD's address
#include <Wire.h>

// MCP23017 registers (IOCON.BANK = 0, the power-on default)
#define MCP_IODIRA   0x00
#define MCP_GPIOA    0x12
#define MCP_OLATA    0x14

// Most 8-channel relay boards are active-LOW: the relay energises when the
// expander pin is pulled to ground. Writing 0xFF therefore releases all of
// them, which is also the safe power-on state.
#define VALVE_ACTIVE_LOW 1

struct ValveBoardInfo {
    ValveBackend backend;
    uint8_t      addr;
};

static ValveBoardInfo boards[VALVE_BOARDS_MAX];
static uint8_t         boardCount      = 0;
static bool             fullySimulated = true;   // true until at least one real board is found
static uint32_t         globalOpenMask = 0;       // bit n = channel n open (logical), spans every board
static bool             busFault       = false;

// Translate the logical "open" mask into the byte a given chip wants.
static inline uint8_t maskToPort(uint8_t m) {
#if VALVE_ACTIVE_LOW
    return (uint8_t)~m;
#else
    return m;
#endif
}

// ---------------------------------------------------------------------------
//  Chip probes
// ---------------------------------------------------------------------------

static bool i2cPresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// An MCP23017 is distinguishable from a PCF8574 at the same address: it has
// addressable registers, so IODIRA reads back 0x00 after being written and
// survives a write/read round trip. A PCF8574 has no register file and simply
// returns its port state, so the round trip does not hold.
static bool looksLikeMcp23017(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(MCP_IODIRA);
    Wire.write(0x00);                     // set port A to all-outputs
    if (Wire.endTransmission() != 0) return false;

    Wire.beginTransmission(addr);
    Wire.write(MCP_IODIRA);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
    uint8_t v = Wire.read();

    if (v != 0x00) return false;          // did not stick — not a register file

    // Put it back the way we found it before deciding either way.
    Wire.beginTransmission(addr);
    Wire.write(MCP_IODIRA);
    Wire.write(0xFF);
    Wire.endTransmission();
    return true;
}

// ---------------------------------------------------------------------------
//  Raw port write — one board's worth of channels
// ---------------------------------------------------------------------------

static bool writePortForBoard(uint8_t boardIdx, uint8_t localMask) {
    uint8_t port = maskToPort(localMask);
    const ValveBoardInfo& b = boards[boardIdx];
    bool ok = false;

    switch (b.backend) {
        case VALVE_BACKEND_PCF8574:
            Wire.beginTransmission(b.addr);
            Wire.write(port);
            ok = (Wire.endTransmission() == 0);
            break;

        case VALVE_BACKEND_MCP23017:
            Wire.beginTransmission(b.addr);
            Wire.write(MCP_OLATA);
            Wire.write(port);
            ok = (Wire.endTransmission() == 0);
            break;

        default:
            ok = false;
            break;
    }

    if (!ok) {
        if (!busFault) Log(ERROR, "[Valve] I2C write failed at 0x" + String(b.addr, HEX));
        busFault = true;
    } else {
        busFault = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
//  Detection — registers EVERY board found, not just the first
// ---------------------------------------------------------------------------

static void addBoard(ValveBackend be, uint8_t addr) {
    if (boardCount >= VALVE_BOARDS_MAX) return;
    boards[boardCount].backend = be;
    boards[boardCount].addr    = addr;
    boardCount++;
    fullySimulated = false;
    Log(INFO, String("[Valve] ") + (be == VALVE_BACKEND_MCP23017 ? "MCP23017" : "PCF8574")
              + " board at 0x" + String(addr, HEX) + " -> channels "
              + String((boardCount - 1) * VALVE_CHANNELS_PER_BOARD) + "-"
              + String(boardCount * VALVE_CHANNELS_PER_BOARD - 1));
}

static void detectBoards() {
    boardCount      = 0;
    fullySimulated  = true;

    // The LCD backpack is a PCF8574 living in exactly the same address range,
    // so whatever Display.cpp already claimed is off-limits here. Without this
    // the valve driver would happily "detect" the LCD and start writing relay
    // patterns into the display controller.
    const uint8_t lcd = getLcdAddress();

    // MCP23017 first: it only answers at 0x20-0x27, and a PCF8574 at the same
    // address would otherwise match first and be mis-driven.
    bool claimed[0x40] = {false};   // covers every address this function ever probes (0x20-0x3F)
    for (uint8_t a = 0x20; a <= 0x27 && boardCount < VALVE_BOARDS_MAX; ++a) {
        if (a == lcd || claimed[a] || !i2cPresent(a)) continue;
        if (looksLikeMcp23017(a)) {
            addBoard(VALVE_BACKEND_MCP23017, a);
            claimed[a] = true;
            // Port A all outputs, all relays released.
            Wire.beginTransmission(a);
            Wire.write(MCP_IODIRA);
            Wire.write(0x00);
            Wire.endTransmission();
        }
    }

    const uint8_t pcfAddrs[] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    };
    for (uint8_t a : pcfAddrs) {
        if (boardCount >= VALVE_BOARDS_MAX) break;
        if (a == lcd || claimed[a] || !i2cPresent(a)) continue;
        addBoard(VALVE_BACKEND_PCF8574, a);
        claimed[a] = true;
    }

    if (fullySimulated) {
        Log(WARN, "[Valve] No valve board found - running fully SIMULATED. "
                  "Zones and programs are fully functional; nothing is energised.");
    } else {
        Log(INFO, "[Valve] " + String(boardCount) + " board(s) found, "
                  + String(boardCount * VALVE_CHANNELS_PER_BOARD) + " channel(s) available");
    }
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void valveInit() {
    detectBoards();
    globalOpenMask = 0;
    busFault       = false;
    valveAllClosed();     // everything closed before anything else can ask
}

void valveRescan() {
    valveAllClosed();
    detectBoards();
    // Re-assert whatever was logically open before the rescan onto whichever
    // boards now answer for those channels — a board that dropped off and
    // came back should not silently forget state. Boards that no longer
    // answer simply won't get the write; Zones.cpp's active-flag refresh
    // (zonesRefreshActive(), called right after this) is what actually
    // decides whether those zones are still usable.
    for (uint8_t b = 0; b < boardCount; b++) {
        uint8_t localMask = (uint8_t)((globalOpenMask >> (b * VALVE_CHANNELS_PER_BOARD)) & 0xFF);
        if (localMask) writePortForBoard(b, localMask);
    }
}

bool    valveHardwarePresent() { return boardCount > 0; }
bool    valveFullySimulated()  { return fullySimulated; }
uint8_t valveBoardCount()      { return boardCount; }
bool    valveBusFault()        { return busFault; }

uint8_t valveBoardAddr(uint8_t b) {
    return b < boardCount ? boards[b].addr : 0;
}
ValveBackend valveBoardBackend(uint8_t b) {
    return b < boardCount ? boards[b].backend : VALVE_BACKEND_NONE;
}

const char* valveBackendName(ValveBackend be) {
    switch (be) {
        case VALVE_BACKEND_PCF8574:   return "PCF8574";
        case VALVE_BACKEND_MCP23017:  return "MCP23017";
        case VALVE_BACKEND_SIMULATED: return "simulated";
        default:                      return "none";
    }
}

bool valveChannelAvailable(uint8_t ch) {
    if (ch >= VALVE_CHANNELS) return false;
    if (fullySimulated) return true;           // every virtual channel usable on the bench
    return (ch / VALVE_CHANNELS_PER_BOARD) < boardCount;
}

bool valveSet(uint8_t ch, bool open) {
    if (ch >= VALVE_CHANNELS) return false;
    uint32_t next = open ? (globalOpenMask | (1UL << ch))
                         : (globalOpenMask & ~(1UL << ch));
    if (next == globalOpenMask) return true;    // already there

    uint8_t boardIdx = ch / VALVE_CHANNELS_PER_BOARD;
    if (boardIdx >= boardCount) {
        if (!fullySimulated) return false;      // real hardware present, but not for this channel
        globalOpenMask = next;
        Log(DEBUG, "[Valve] (simulated) ch=" + String(ch) + " open=" + String(open ? "1" : "0"));
        return true;
    }

    uint32_t prev = globalOpenMask;
    globalOpenMask = next;
    uint8_t localMask = (uint8_t)((globalOpenMask >> (boardIdx * VALVE_CHANNELS_PER_BOARD)) & 0xFF);
    if (!writePortForBoard(boardIdx, localMask)) {
        globalOpenMask = prev;                  // keep the mirror honest
        return false;
    }
    return true;
}

void valveAllClosed() {
    if (globalOpenMask == 0 && boardCount == 0) return;
    globalOpenMask = 0;
    for (uint8_t b = 0; b < boardCount; b++) writePortForBoard(b, 0);
    Log(INFO, "[Valve] All valves closed");
}

bool valveIsOpen(uint8_t ch) {
    return ch < VALVE_CHANNELS && (globalOpenMask & (1UL << ch));
}

uint8_t valveOpenCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < VALVE_CHANNELS; i++) if (globalOpenMask & (1UL << i)) n++;
    return n;
}
