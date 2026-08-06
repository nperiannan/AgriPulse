# OH Tank Float Switch LoRa Transmitter

Firmware for the overhead (OH) water tank transmitter. Reads three float switches to determine water level and broadcasts the state via LoRa every 30 seconds to the controller.

## Hardware

| Component | Details |
|---|---|
| MCU | ATmega328P @ 8 MHz, 3.3 V |
| LoRa Module | RFM95 (SX1276) |
| Bootloader | urclock @ 57600 baud |
| Float Switches | 3× Normally Open (NO), wired to GND |

### Pin Map

| Pin | Function |
|---|---|
| A1 | Float switch — LOW (bottom) |
| A2 | Float switch — HALF (middle) |
| A3 | Float switch — FULL (top) |
| D10 | LoRa NSS (SPI chip select) |
| D2 | LoRa DIO0 (interrupt) |
| D3 | LoRa RESET |
| D9 | Debug LED |

### Float Switch Wiring

Each float switch connects between the analog pin and GND. The MCU uses `INPUT_PULLUP`, so:
- **Pin reads LOW** → switch closed → water is present at that level
- **Pin reads HIGH** → switch open → no water at that level

## LoRa Configuration

| Parameter | Value |
|---|---|
| Frequency | 865.0 MHz |
| Bandwidth | 125.0 kHz |
| Spreading Factor | 9 |
| Coding Rate | 4/7 |
| Sync Word | 0x34 |
| TX Power | 20 dBm |
| Preamble Length | 8 |

Library: [RadioLib](https://github.com/jgromes/RadioLib) v6.6.0 (SX1276 driver)

## Transmitted Data

### FloatPacket (4 bytes)

```c
#pragma pack(push, 1)
struct FloatPacket {
    uint8_t type;      // Always 0x02
    uint8_t sw_full;   // 1 = FULL switch triggered
    uint8_t sw_half;   // 1 = HALF switch triggered
    uint8_t sw_low;    // 1 = LOW switch triggered
};
#pragma pack(pop)
```

**Byte layout (over the wire):**

| Byte | Field | Description |
|---|---|---|
| 0 | `type` | Packet type identifier = `0x02` |
| 1 | `sw_full` | 1 if water reaches FULL level, else 0 |
| 2 | `sw_half` | 1 if water reaches HALF level, else 0 |
| 3 | `sw_low` | 1 if water reaches LOW level, else 0 |

### Tank States

| State | sw_full | sw_half | sw_low | Raw bytes |
|---|---|---|---|---|
| FULL | 1 | 1 | 1 | `02 01 01 01` |
| HALF | 0 | 1 | 1 | `02 00 01 01` |
| LOW | 0 | 0 | 1 | `02 00 00 01` |
| EMPTY | 0 | 0 | 0 | `02 00 00 00` |

## Hub Side — Expected Changes

The hub firmware (`pulse-hub/`) currently expects a **3-byte** `FloatPacket`:

```c
// CURRENT hub struct (Globals.h) — NEEDS UPDATE
struct FloatPacket {
    uint8_t type;   // 0x02
    uint8_t low;    // LOW switch
    uint8_t full;   // FULL switch
};
```

**What needs to change on the hub:**

1. **Update `FloatPacket`** in `pulse-hub/include/Globals.h` to match the new 4-byte format:
   ```c
   struct FloatPacket {
       uint8_t type;      // 0x02
       uint8_t sw_full;   // FULL switch
       uint8_t sw_half;   // HALF switch (new)
       uint8_t sw_low;    // LOW switch
   };
   ```

2. **Update `TankState` enum** to add HALF and EMPTY states:
   ```c
   enum TankState {
       TANK_STATE_UNKNOWN = 0,
       TANK_STATE_EMPTY   = 1,
       TANK_STATE_LOW     = 2,
       TANK_STATE_HALF    = 3,
       TANK_STATE_FULL    = 4
   };
   ```

3. **Update `LoRaManager.cpp`** parsing logic to handle all 4 states:
   ```
   sw_full=1 → TANK_STATE_FULL
   sw_half=1 → TANK_STATE_HALF
   sw_low=1  → TANK_STATE_LOW
   all 0     → TANK_STATE_EMPTY
   ```

> **Note:** Field order changed from `{type, low, full}` → `{type, sw_full, sw_half, sw_low}`. The controller **must** be updated before it can correctly parse packets from this transmitter.

## Building & Uploading

```bash
# Build only
pio run -e uno

# Build and upload (COM5)
pio run -e uno -t upload

# Serial monitor (57600 baud)
pio device monitor
```

## Unit Tests

Run the packet encoding tests (Python, no extra dependencies):

```bash
cd pulse-node
python -m unittest discover -s test -p "test_*.py" -v
```

Tests verify the exact byte layout for all 4 tank states, field ordering, and edge cases.

## Debug LED Patterns

| Pattern | Meaning |
|---|---|
| 3 quick blinks (100ms) | Setup started |
| 1 long blink (1s) | LoRa initialized OK |
| Rapid non-stop blinking | LoRa init failed — check wiring |
| Single blink every 30s | Successful LoRa transmission |
