#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//                           FIRMWARE VERSION
// =============================================================================
#define FW_VERSION "2.1.0"

// =============================================================================
//                    LORA RADIO PARAMETERS
//   Must match pulse-hub/include/Config.h exactly.
// =============================================================================
#define LORA_FREQUENCY          865.0f   // MHz  (RadioLib units)
#define LORA_BANDWIDTH          125.0f   // kHz  (RadioLib units)
#define LORA_SPREADING_FACTOR       9
#define LORA_CODING_RATE            7    // 4/7
#define LORA_SYNC_WORD           0x34
#define LORA_TX_POWER              20    // dBm
#define LORA_PREAMBLE_LENGTH        8

// =============================================================================
//                    SPI / LORA PINS  (RFM95 on ATmega328P hardware SPI)
// =============================================================================
#define LORA_SS_PIN    10
#define LORA_RST_PIN    3
#define LORA_DIO0_PIN   2

// =============================================================================
//                    DEBUG LED
//   Pin 9 blinks on every LoRa transmission.
// =============================================================================
#define DEBUG_LED       9

// =============================================================================
//                    PACKET TYPE  (must match pulse-hub Config.h)
// =============================================================================
#define LORA_PKT_FLOAT_SWITCH  0x02

// =============================================================================
//                    FLOAT SWITCH INPUT PINS
//
//   Three NO (Normally Open) float switches at different tank heights.
//   Wired: one terminal to GND, the other to the MCU pin.
//   Pins use INPUT_PULLUP → HIGH when open (no water), LOW when closed (water).
//
//   A0 = bottom  (LOW level)
//   A1 = middle  (HALF level)
//   A2 = top     (FULL level)
// =============================================================================
#define FLOAT_LOW_PIN   A0
#define FLOAT_HALF_PIN  A1
#define FLOAT_FULL_PIN  A2

// =============================================================================
//                    TANK LEVEL VALUES  (sent in FloatPacket.level)
// =============================================================================
#define LEVEL_UNKNOWN  0   // unstable / inconsistent reading
#define LEVEL_EMPTY    1   // no switches triggered
#define LEVEL_LOW      2   // only bottom switch triggered
#define LEVEL_HALF     3   // bottom + middle triggered
#define LEVEL_FULL     4   // all three triggered
#define LEVEL_COUNT    5   // number of level values (for array sizing)

// =============================================================================
//                    STABILITY SAMPLING
//   Rolling window: sample every 500 ms, 40 samples = 20 s window.
//   At least 75 % of samples must agree for the level to be accepted.
// =============================================================================
#define SAMPLE_INTERVAL_MS    5000UL
#define STABILITY_SAMPLES       5      // 5 × 5 s = 25 s
#define STABILITY_PERCENT      60      // majority threshold %

// =============================================================================
//                    TRANSMIT INTERVAL
// =============================================================================
#define TX_INTERVAL_MS  30000UL  // 30 seconds

#endif // CONFIG_H
