#include "ValveController.h"
#include "Config.h"
#include "Logger.h"
#include "Display.h"     // getLcdAddress() — so we never claim the LCD's address
#include <Wire.h>
#include <Preferences.h>

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
    bool         present;   // answered THIS boot/rescan — a declared board still
                             // gets a slot (and its zones just go inactive) even
                             // when it doesn't
};

static ValveBoardInfo boards[VALVE_BOARDS_MAX];
static uint8_t         boardCount      = 0;   // number of SLOTS, not number present
static bool             fullySimulated = true; // true only when NOTHING is declared or detected at all
static uint32_t         globalOpenMask = 0;    // bit n = channel n open (logical), spans every board
static bool             busFault       = false;

// ---------------------------------------------------------------------------
//  Declared expansion-board addresses — see ValveController.h
// ---------------------------------------------------------------------------
#define NVS_I2CCFG_NS   "i2ccfg"
#define NVS_KEY_EXPADDR "expaddr"   // comma-separated decimal addresses

static uint8_t declared[VALVE_DECLARED_MAX];
static uint8_t declaredCount = 0;

static void saveDeclared() {
    String blob;
    for (uint8_t i = 0; i < declaredCount; i++) {
        if (i) blob += ',';
        blob += String(declared[i]);
    }
    Preferences p;
    p.begin(NVS_I2CCFG_NS, false);
    p.putString(NVS_KEY_EXPADDR, blob);
    p.end();
}

void valveConfigInit() {
    Preferences p;
    p.begin(NVS_I2CCFG_NS, true);
    String blob = p.getString(NVS_KEY_EXPADDR, "");
    p.end();

    declaredCount = 0;
    int from = 0;
    while (declaredCount < VALVE_DECLARED_MAX && from < (int)blob.length()) {
        int comma = blob.indexOf(',', from);
        String tok = (comma < 0) ? blob.substring(from) : blob.substring(from, comma);
        tok.trim();
        int v = tok.toInt();
        if (v > 0 && v <= 0xFF) declared[declaredCount++] = (uint8_t)v;
        if (comma < 0) break;
        from = comma + 1;
    }
    if (declaredCount) {
        String s;
        for (uint8_t i = 0; i < declaredCount; i++) { if (i) s += ", "; s += "0x" + String(declared[i], HEX); }
        Log(INFO, "[Valve] " + String(declaredCount) + " declared expansion address(es): " + s);
    }
}

uint8_t valveDeclaredCount()          { return declaredCount; }
uint8_t valveDeclaredAddr(uint8_t i)  { return i < declaredCount ? declared[i] : 0; }

bool valveIsDeclaredExpansion(uint8_t addr) {
    for (uint8_t i = 0; i < declaredCount; i++) if (declared[i] == addr) return true;
    return false;
}

static bool addrInBoardRange(uint8_t a) {
    return (a >= 0x20 && a <= 0x27) || (a >= 0x38 && a <= 0x3F);
}

bool valveAddDeclaredExpansion(uint8_t addr) {
    if (!addrInBoardRange(addr)) return false;
    if (valveIsDeclaredExpansion(addr)) return false;
    if (declaredCount >= VALVE_DECLARED_MAX) return false;
    declared[declaredCount++] = addr;
    saveDeclared();
    Log(INFO, "[Valve] Declared 0x" + String(addr, HEX) + " as an expansion board");
    return true;
}

bool valveRemoveDeclaredExpansion(uint8_t addr) {
    for (uint8_t i = 0; i < declaredCount; i++) {
        if (declared[i] != addr) continue;
        for (uint8_t j = i; j < declaredCount - 1; j++) declared[j] = declared[j + 1];
        declaredCount--;
        saveDeclared();
        Log(INFO, "[Valve] Un-declared 0x" + String(addr, HEX));
        return true;
    }
    return false;
}

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

// This used to write 0x00 to IODIRA and check whether it "stuck" on
// readback, on the theory that a PCF8574 (no register file, a raw byte
// write just sets its port) couldn't replicate that. It's wrong: an
// UNLOADED PCF8574 — nothing connected to its P0-P7 pins yet, exactly the
// bench-testing case this whole board-declaration feature exists for —
// trivially "remembers" 0x00 too, purely because nothing else is driving
// those floating pins. Round-tripping ANY value this way just reflects
// what was last written on a PCF8574, register semantics or not; it proves
// nothing. This is what got a real PCF8574T module misidentified as an
// MCP23017 here.
//
// The real, hardware-enforced tell: IOCON bit0 is permanently unimplemented
// on a genuine MCP23017 and always reads back 0, no matter what is written
// to it — a dumb port expander has no way to fake a bit that refuses to
// stick regardless of load. Only bit0 is touched (never BANK/MIRROR/etc —
// flipping BANK on a real MCP23017 would silently break every other
// register address this driver assumes), so this is safe even against a
// genuine MCP23017 already in normal use.
#define MCP_IOCON 0x0A
static bool looksLikeMcp23017(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(MCP_IOCON);
    Wire.write(0x01);                     // only the unimplemented bit0; every other IOCON bit stays 0
    if (Wire.endTransmission() != 0) return false;

    Wire.beginTransmission(addr);
    Wire.write(MCP_IOCON);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
    uint8_t v = Wire.read();

    // Restore IOCON to its power-on default either way.
    Wire.beginTransmission(addr);
    Wire.write(MCP_IOCON);
    Wire.write(0x00);
    Wire.endTransmission();

    return v == 0x00;   // bit0 refused to stick -> a real register file (MCP23017)
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
//  Detection — declared addresses get FIXED slots, in declared order
//
//  A slot's index (board*8+local = its channels) must never depend on live
//  scan order: if it did, a board that's temporarily unplugged (or just
//  slower to power up than another) would let a LATER board slide into an
//  EARLIER slot, silently repointing every zone mapped to that slot at the
//  next reboot or rescan — exactly the kind of thing that should be
//  impossible for something driving a real pump. Declaring an address (web
//  UI, Network tab) is what pins it to a slot; "Expansion Board #1" always
//  means declared[0], whether or not it happens to answer this boot.
//
//  A board that hasn't been declared yet still auto-detects for bench
//  convenience — it's just appended after every declared slot instead of
//  wherever scan order would have put it, so it can't disturb slots that are
//  already pinned.
// ---------------------------------------------------------------------------

static bool identifyAndClaim(uint8_t addr, ValveBackend* outBackend) {
    if (looksLikeMcp23017(addr)) {
        *outBackend = VALVE_BACKEND_MCP23017;
        Wire.beginTransmission(addr);       // port A all outputs, all relays released
        Wire.write(MCP_IODIRA);
        Wire.write(0x00);
        Wire.endTransmission();
    } else {
        *outBackend = VALVE_BACKEND_PCF8574;
    }
    return true;
}

static void detectBoards() {
    boardCount = 0;

    // The LCD backpack is a PCF8574 living in exactly the same address range,
    // so whatever Display.cpp already claimed is off-limits here. Without this
    // the valve driver would happily "detect" the LCD and start writing relay
    // patterns into the display controller.
    const uint8_t lcd = getLcdAddress();
    bool claimed[0x40] = {false};   // covers every address this function ever probes (0x20-0x3F)

    for (uint8_t i = 0; i < declaredCount && boardCount < VALVE_BOARDS_MAX; i++) {
        uint8_t a = declared[i];
        ValveBoardInfo& slot = boards[boardCount];
        slot.addr    = a;
        slot.present = (a != lcd) && !claimed[a] && i2cPresent(a);
        slot.backend = VALVE_BACKEND_NONE;
        if (slot.present) {
            claimed[a] = true;
            identifyAndClaim(a, &slot.backend);
        }
        Log(INFO, "[Valve] Expansion Board #" + String(boardCount + 1) + " (0x" + String(a, HEX)
                  + ") -> channels " + String(boardCount * VALVE_CHANNELS_PER_BOARD) + "-"
                  + String(boardCount * VALVE_CHANNELS_PER_BOARD + VALVE_CHANNELS_PER_BOARD - 1)
                  + (slot.present ? (" - " + String(valveBackendName(slot.backend)))
                                  : String(" - not detected this boot")));
        boardCount++;
    }

    // Anything else answering that hasn't been declared: MCP23017 first (it
    // only answers at 0x20-0x27, and a PCF8574 at the same address would
    // otherwise match first and be mis-driven), then PCF8574 across both
    // ranges. Same detection logic as before, just appended after the
    // declared slots instead of assigned scan-order slots of its own.
    for (uint8_t a = 0x20; a <= 0x27 && boardCount < VALVE_BOARDS_MAX; ++a) {
        if (a == lcd || claimed[a] || !i2cPresent(a)) continue;
        if (!looksLikeMcp23017(a)) continue;
        claimed[a] = true;
        Wire.beginTransmission(a);
        Wire.write(MCP_IODIRA);
        Wire.write(0x00);
        Wire.endTransmission();
        boards[boardCount] = {VALVE_BACKEND_MCP23017, a, true};
        Log(INFO, "[Valve] Auto-detected (undeclared) MCP23017 at 0x" + String(a, HEX)
                  + " -> channels " + String(boardCount * VALVE_CHANNELS_PER_BOARD) + "-"
                  + String(boardCount * VALVE_CHANNELS_PER_BOARD + VALVE_CHANNELS_PER_BOARD - 1));
        boardCount++;
    }

    const uint8_t pcfAddrs[] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    };
    for (uint8_t a : pcfAddrs) {
        if (boardCount >= VALVE_BOARDS_MAX) break;
        if (a == lcd || claimed[a] || !i2cPresent(a)) continue;
        claimed[a] = true;
        boards[boardCount] = {VALVE_BACKEND_PCF8574, a, true};
        Log(INFO, "[Valve] Auto-detected (undeclared) PCF8574 at 0x" + String(a, HEX)
                  + " -> channels " + String(boardCount * VALVE_CHANNELS_PER_BOARD) + "-"
                  + String(boardCount * VALVE_CHANNELS_PER_BOARD + VALVE_CHANNELS_PER_BOARD - 1));
        boardCount++;
    }

    // Fully simulated means nothing at all is declared or detected — a
    // declared board that just isn't answering this boot still occupies a
    // real slot (present=false), so its zones go inactive rather than
    // quietly falling back to "simulated" and pretending to work.
    fullySimulated = (boardCount == 0);

    if (fullySimulated) {
        Log(WARN, "[Valve] No valve board found - running fully SIMULATED. "
                  "Zones and programs are fully functional; nothing is energised.");
    } else {
        Log(INFO, "[Valve] " + String(boardCount) + " board slot(s), "
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
    // came back should not silently forget state. A slot that isn't present
    // this time is skipped outright (writing to it would just be an I2C
    // error against nothing); Zones.cpp's active-flag refresh
    // (zonesRefreshActive(), called right after this) is what actually
    // decides whether those zones are still usable.
    for (uint8_t b = 0; b < boardCount; b++) {
        if (!boards[b].present) continue;
        uint8_t localMask = (uint8_t)((globalOpenMask >> (b * VALVE_CHANNELS_PER_BOARD)) & 0xFF);
        if (localMask) writePortForBoard(b, localMask);
    }
}

bool valveHardwarePresent() {
    for (uint8_t i = 0; i < boardCount; i++) if (boards[i].present) return true;
    return false;
}
bool    valveFullySimulated()  { return fullySimulated; }
uint8_t valveBoardCount()      { return boardCount; }
bool    valveBusFault()        { return busFault; }

uint8_t valveBoardAddr(uint8_t b) {
    return b < boardCount ? boards[b].addr : 0;
}
ValveBackend valveBoardBackend(uint8_t b) {
    return b < boardCount ? boards[b].backend : VALVE_BACKEND_NONE;
}
bool valveBoardPresent(uint8_t b) {
    return b < boardCount ? boards[b].present : false;
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
    uint8_t b = ch / VALVE_CHANNELS_PER_BOARD;
    return b < boardCount && boards[b].present;
}

bool valveSet(uint8_t ch, bool open) {
    if (ch >= VALVE_CHANNELS) return false;
    uint32_t next = open ? (globalOpenMask | (1UL << ch))
                         : (globalOpenMask & ~(1UL << ch));
    if (next == globalOpenMask) return true;    // already there

    uint8_t boardIdx = ch / VALVE_CHANNELS_PER_BOARD;
    if (boardIdx >= boardCount || !boards[boardIdx].present) {
        if (!fullySimulated) return false;      // a real slot exists somewhere, just not (yet) for this channel
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
    // Skip slots with no board present — writing to one would hit
    // writePortForBoard()'s default case (unknown backend) and incorrectly
    // raise busFault for what is actually just an undeclared-or-unplugged
    // slot, not a real board that stopped answering.
    for (uint8_t b = 0; b < boardCount; b++) if (boards[b].present) writePortForBoard(b, 0);
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
