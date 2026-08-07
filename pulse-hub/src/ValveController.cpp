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

static ValveBackend backend    = VALVE_BACKEND_NONE;
static uint8_t      i2cAddr    = 0;
static uint8_t      openMask   = 0;      // bit n set = zone n open (logical)
static bool         busFault   = false;

// Translate the logical "open" mask into the byte the chip wants.
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
// addressable registers, so IODIRA reads back 0xFF after reset and survives a
// write/read round trip. A PCF8574 has no register file and simply returns its
// port state, so the round trip does not hold.
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
//  Raw port write
// ---------------------------------------------------------------------------

static bool writePort(uint8_t mask) {
    uint8_t port = maskToPort(mask);
    bool ok = false;

    switch (backend) {
        case VALVE_BACKEND_PCF8574:
            Wire.beginTransmission(i2cAddr);
            Wire.write(port);
            ok = (Wire.endTransmission() == 0);
            break;

        case VALVE_BACKEND_MCP23017:
            Wire.beginTransmission(i2cAddr);
            Wire.write(MCP_OLATA);
            Wire.write(port);
            ok = (Wire.endTransmission() == 0);
            break;

        case VALVE_BACKEND_SIMULATED:
            Log(DEBUG, "[Valve] (simulated) port=0x" + String(port, HEX)
                       + " open mask=0x" + String(mask, HEX));
            ok = true;
            break;

        default:
            ok = false;
            break;
    }

    if (!ok && backend != VALVE_BACKEND_SIMULATED) {
        if (!busFault) Log(ERROR, "[Valve] I2C write failed at 0x" + String(i2cAddr, HEX));
        busFault = true;
    } else if (ok) {
        busFault = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
//  Detection
// ---------------------------------------------------------------------------

static void detectBackend() {
    backend = VALVE_BACKEND_NONE;
    i2cAddr = 0;

    // The LCD backpack is a PCF8574 living in exactly the same address range,
    // so whatever Display.cpp already claimed is off-limits here. Without this
    // the valve driver would happily "detect" the LCD and start writing relay
    // patterns into the display controller.
    const uint8_t lcd = getLcdAddress();

    // MCP23017 first: it only answers at 0x20-0x27, and a PCF8574 at the same
    // address would otherwise match first and be mis-driven.
    for (uint8_t a = 0x20; a <= 0x27; ++a) {
        if (a == lcd || !i2cPresent(a)) continue;
        if (looksLikeMcp23017(a)) {
            backend = VALVE_BACKEND_MCP23017;
            i2cAddr = a;
            // Port A all outputs, all relays released.
            Wire.beginTransmission(a);
            Wire.write(MCP_IODIRA);
            Wire.write(0x00);
            Wire.endTransmission();
            Log(INFO, "[Valve] MCP23017 valve board at 0x" + String(a, HEX));
            return;
        }
    }

    const uint8_t pcfAddrs[] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    };
    for (uint8_t a : pcfAddrs) {
        if (a == lcd || !i2cPresent(a)) continue;
        backend = VALVE_BACKEND_PCF8574;
        i2cAddr = a;
        Log(INFO, "[Valve] PCF8574 valve board at 0x" + String(a, HEX));
        return;
    }

    backend = VALVE_BACKEND_SIMULATED;
    i2cAddr = 0;
    Log(WARN, "[Valve] No valve board found - running SIMULATED. "
              "Zones and programs are fully functional; nothing is energised.");
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void valveInit() {
    detectBackend();
    openMask = 0;
    busFault = false;
    writePort(0);     // everything closed before anything else can ask
}

void valveRescan() {
    valveAllClosed();
    detectBackend();
    writePort(openMask);
}

bool valveHardwarePresent() {
    return backend == VALVE_BACKEND_PCF8574 || backend == VALVE_BACKEND_MCP23017;
}

ValveBackend valveBackendKind() { return backend; }
uint8_t      valveI2CAddress()  { return i2cAddr; }
bool         valveBusFault()    { return busFault; }

const char* valveBackendName() {
    switch (backend) {
        case VALVE_BACKEND_PCF8574:   return "PCF8574";
        case VALVE_BACKEND_MCP23017:  return "MCP23017";
        case VALVE_BACKEND_SIMULATED: return "simulated";
        default:                      return "none";
    }
}

bool valveSet(uint8_t ch, bool open) {
    if (ch >= VALVE_CHANNELS) return false;
    uint8_t next = open ? (uint8_t)(openMask | (1u << ch))
                        : (uint8_t)(openMask & ~(1u << ch));
    if (next == openMask) return true;          // already there
    uint8_t prev = openMask;
    openMask = next;
    if (!writePort(openMask)) {
        openMask = prev;                        // keep the mirror honest
        return false;
    }
    return true;
}

void valveAllClosed() {
    if (openMask == 0) return;
    openMask = 0;
    writePort(0);
    Log(INFO, "[Valve] All valves closed");
}

bool    valveIsOpen(uint8_t ch) { return ch < VALVE_CHANNELS && (openMask & (1u << ch)); }
uint8_t valveOpenMask()         { return openMask; }

uint8_t valveOpenCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < VALVE_CHANNELS; i++) if (openMask & (1u << i)) n++;
    return n;
}
