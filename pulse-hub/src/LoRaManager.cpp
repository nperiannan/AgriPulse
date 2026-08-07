#include "LoRaManager.h"
#include "Logger.h"
#include "Config.h"
#include "History.h"
#include "MQTTManager.h"
#include <RadioLib.h>
#include <SPI.h>

// RadioLib: RFM95 on HSPI
static SPIClass     spi2(HSPI);
static RFM95        radio = new Module(LORA_CS, LORA_IRQ, LORA_RST, LORA_DIO1, spi2);

static bool          loraReady        = false;
static int           retryCount       = 0;
static unsigned long lastReinitMs     = 0;
static float         lastRSSI         = 0.0f;
static float         lastSNR          = 0.0f;
static uint8_t       missedPackets    = 0;          // counts consecutive missed 30s windows
static unsigned long lastExpectedMs   = 0;          // millis() at which next packet is expected
static bool          transmitterLost  = false;      // true after LORA_MAX_MISSED_PACKETS misses

// Interrupt-driven receive: DIO0 fires when a packet arrives.  The ISR only
// sets a flag so pollLoRa() can read the packet without blocking the main loop.
static volatile bool packetReceived = false;

static void IRAM_ATTR onLoraPacket() {
    packetReceived = true;
}

// Non-blocking debug-LED flash: 0 = idle, otherwise the millis() deadline at
// which the LED should turn back off. A blocking delayMicroseconds(50000)
// here used to stall the loop — and the web server with it — for 50 ms on
// every received packet, which defeated the whole point of the interrupt-
// driven, non-blocking receive path above.
static unsigned long ledOffAtMs = 0;

static void serviceDebugLed() {
    if (ledOffAtMs != 0 && (long)(millis() - ledOffAtMs) >= 0) {
        digitalWrite(DEBUG_LED, LOW);
        ledOffAtMs = 0;
    }
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

bool initLoRa() {
    spi2.begin(HSPI_SCLK, HSPI_MISO, HSPI_MOSI, HSPI_SS);
    int state = radio.begin(
        LORA_FREQUENCY,
        LORA_BANDWIDTH,
        LORA_SPREADING_FACTOR,
        LORA_CODING_RATE,
        LORA_SYNC_WORD,
        LORA_TX_POWER,
        LORA_PREAMBLE_LENGTH
    );

    if (state == RADIOLIB_ERR_NONE) {
        loraReady  = true;
        retryCount = 0;
        packetReceived = false;
        radio.setPacketReceivedAction(onLoraPacket);
        radio.startReceive();
        Log(INFO, "[LoRa] Initialised successfully");
        loraOperational = true;
    } else {
        loraReady       = false;
        loraOperational = false;
        Log(ERROR, "[LoRa] Init failed, code=" + String(state));
    }
    return loraReady;
}

void pollLoRa() {
    serviceDebugLed();   // turn off a pending flash even if we return early below

    // --- Re-init logic if radio failed ---
    if (!loraReady) {
        if (retryCount >= MAX_LORA_RETRIES) {
            if (millis() - lastReinitMs >= LORA_REINIT_INTERVAL_MS) {
                Log(INFO, "[LoRa] Attempting re-init...");
                lastReinitMs = millis();
                if (initLoRa()) {
                    retryCount = 0;
                    return;
                }
                retryCount = 0;
            }
        }
        return;
    }

    // --- Missed-packet detection ---
    // Transmitter sends every 30 s. After 10 consecutive misses → transmitter lost.
    if (lastLoraReceivedTime > 0 && !transmitterLost) {
        unsigned long elapsed = millis() - lastLoraReceivedTime;
        uint8_t windowsMissed = (uint8_t)(elapsed / LORA_EXPECTED_INTERVAL_MS);
        if (windowsMissed > missedPackets) {
            missedPackets = windowsMissed;
        }
        if (missedPackets >= LORA_MAX_MISSED_PACKETS && ohTankState != TANK_STATE_UNKNOWN) {
            transmitterLost = true;
            Log(WARN, "[LoRa] Transmitter lost – " + String(missedPackets) + " packets missed, marking UNKNOWN");
            ohLastKnownState = ohTankState;
            ohTankState = TANK_STATE_UNKNOWN;
            publishMQTTStatus();   // don't wait for the next keep-alive tick
        }
    }

    // --- Interrupt-driven receive (non-blocking) ---
    // radio.startReceive() runs continuously; the DIO0 ISR sets packetReceived.
    // If no packet arrived this iteration, return immediately so loop() — and the
    // web server — stays responsive.  (A blocking radio.receive() here stalls the
    // loop ~410 ms per call and makes the web UI time out once a radio is fitted.)
    if (!packetReceived) {
        return;
    }
    packetReceived = false;

    uint8_t buf[8] = {};
    int state = radio.readData(buf, sizeof(buf));
    radio.startReceive();   // re-arm for the next packet

    if (state == RADIOLIB_ERR_NONE) {
        if (!loraOperational) {
            Log(INFO, "[LoRa] Communication restored");
            loraOperational = true;
        }
        retryCount       = 0;
        lastLoraReceivedTime = millis();
        missedPackets    = 0;
        transmitterLost  = false;
        lastRSSI         = radio.getRSSI();
        lastSNR          = radio.getSNR();

        // Flash debug LED — non-blocking; serviceDebugLed() turns it back off
        // on a later pollLoRa() call once 50 ms have passed.
        digitalWrite(DEBUG_LED, HIGH);
        ledOffAtMs = millis() + 50;
        if (ledOffAtMs == 0) ledOffAtMs = 1;   // dodge the 0-means-idle sentinel

        uint8_t pktType = buf[0];
        TankState newState = TANK_STATE_UNKNOWN;

        if (pktType == LORA_PKT_FLOAT_SWITCH && sizeof(FloatPacket) <= sizeof(buf)) {
            FloatPacket* fp = reinterpret_cast<FloatPacket*>(buf);
            if (fp->sw_full)      newState = TANK_STATE_FULL;
            else if (fp->sw_half) newState = TANK_STATE_HALF;
            else if (fp->sw_low)  newState = TANK_STATE_LOW;
            else                  newState = TANK_STATE_EMPTY;
            Log(INFO, "[LoRa] Float pkt: low=" + String(fp->sw_low)
                      + " half=" + String(fp->sw_half)
                      + " full=" + String(fp->sw_full)
                      + " → " + tankStateStr(newState));

        } else {
            Log(WARN, "[LoRa] Unknown packet type: 0x" + String(pktType, HEX));
        }

        if (newState != ohTankState) {
            if (ohTankState != TANK_STATE_UNKNOWN) {
                ohLastKnownState = ohTankState;
            }
            ohTankState = newState;
            if (newState != TANK_STATE_UNKNOWN) {
                ohLastKnownState = newState;
            }
            addHistoryRecord(HIST_OH_STATE_CHG, ohTankState, ugTankState);
            publishMQTTStatus();   // don't wait for the next keep-alive tick
        } else {
            ohTankState = newState;
        }

    } else {
        retryCount++;
        Log(WARN, "[LoRa] Read error " + String(state) + " (retry " + String(retryCount) + ")");
        if (retryCount >= MAX_LORA_RETRIES) {
            loraReady       = false;
            loraOperational = false;
            Log(ERROR, "[LoRa] Too many errors – marking non-operational");
        }
        // (receiver already re-armed above via startReceive())
    }
}

bool isLoraOperational() { return loraReady; }
bool isTransmitterLost() { return transmitterLost; }
float getLoraRSSI()      { return lastRSSI;  }
float getLoraSNR()       { return lastSNR;   }
