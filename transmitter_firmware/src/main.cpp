#include <RadioLib.h>
#include <avr/wdt.h>

// ** Float Switch Configuration **
#define FLOAT_LOW_PIN   A1   // Bottom float switch (LOW level)
#define FLOAT_HALF_PIN  A2   // Middle float switch (HALF level)
#define FLOAT_FULL_PIN  A3   // Top float switch (FULL level)

// ** LoRa Module Configuration **
#define NSS 10              // Chip select pin for LoRa
#define DIO0 2              // Interrupt pin for LoRa
#define RESET 3             // Reset pin for LoRa
#define DEBUG_LED 9         // LED to indicate transmission

// ** Transmission Interval **
#define TX_INTERVAL_MS      30000UL  // Regular heartbeat every 30 seconds
#define DEBOUNCE_STABLE_MS   3000UL  // State must be stable for 3s before immediate send
#define POLL_INTERVAL_MS     1000UL  // Check switches every 1 second during sleep

static uint8_t txFailCount = 0;  // consecutive transmit failures
static uint8_t lastSentFull = 0xFF;  // last sent switch states (0xFF = none sent yet)
static uint8_t lastSentHalf = 0xFF;
static uint8_t lastSentLow  = 0xFF;

SX1276 radio = new Module(NSS, DIO0, RESET);

// ** Data Struct for Transmission **
#pragma pack(push, 1)
struct FloatPacket {
    uint8_t type;      // Packet type (0x02 = float switch)
    uint8_t sw_full;   // 1 = FULL switch triggered
    uint8_t sw_half;   // 1 = HALF switch triggered
    uint8_t sw_low;    // 1 = LOW  switch triggered
};
#pragma pack(pop)

// ** Function Declarations **
void blink_led();

void setup() {
    wdt_disable();  // Disable WDT early in case it was active from a reset
    Serial.begin(57600);
    delay(100);

    pinMode(FLOAT_LOW_PIN,  INPUT_PULLUP);
    pinMode(FLOAT_HALF_PIN, INPUT_PULLUP);
    pinMode(FLOAT_FULL_PIN, INPUT_PULLUP);
    pinMode(DEBUG_LED, OUTPUT);

    // 3 quick blinks = setup started
    for (uint8_t i = 0; i < 3; i++) {
        digitalWrite(DEBUG_LED, HIGH); delay(100);
        digitalWrite(DEBUG_LED, LOW);  delay(100);
    }

    Serial.print(F("LoRa Initializing ... "));
    int state = radio.begin(865.0, 125.0, 9, 7, 0x34, 20, 8, 0);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
        // 1 long blink = LoRa OK
        digitalWrite(DEBUG_LED, HIGH); delay(1000);
        digitalWrite(DEBUG_LED, LOW);
    } else {
        Serial.print(F("Failed, code: "));
        Serial.println(state);
        // Rapid blink forever = LoRa FAILED
        while (true) {
            digitalWrite(DEBUG_LED, HIGH); delay(50);
            digitalWrite(DEBUG_LED, LOW);  delay(50);
        }
    }

    state = radio.setOutputPower(20);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("Transmit power set!"));
    } else {
        Serial.print(F("Failed to set transmit power, code: "));
        Serial.println(state);
        while (true) {
            digitalWrite(DEBUG_LED, HIGH); delay(50);
            digitalWrite(DEBUG_LED, LOW);  delay(50);
        }
    }

    Serial.println(F("LoRa transmitter initialized!"));

    // Enable watchdog timer (8 second timeout)
    // If anything hangs, MCU auto-resets
    wdt_enable(WDTO_8S);
}

// ** Helper: read current float switch state **
static void readSwitches(uint8_t &lo, uint8_t &half, uint8_t &full) {
    lo   = (digitalRead(FLOAT_LOW_PIN)  == LOW) ? 1 : 0;
    half = (digitalRead(FLOAT_HALF_PIN) == LOW) ? 1 : 0;
    full = (digitalRead(FLOAT_FULL_PIN) == LOW) ? 1 : 0;
}

// ** Helper: transmit current state via LoRa **
static void transmitState(uint8_t lo, uint8_t half, uint8_t full) {
    FloatPacket pkt;
    pkt.type    = 0x02;
    pkt.sw_full = full;
    pkt.sw_half = half;
    pkt.sw_low  = lo;

    wdt_reset();
    Serial.println(F("LoRa Transmitting Data ..."));
    Serial.print(F("LOW=")); Serial.print(lo);
    Serial.print(F(" HALF=")); Serial.print(half);
    Serial.print(F(" FULL=")); Serial.println(full);

    int state = radio.transmit((uint8_t*)&pkt, sizeof(pkt));

    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("Success!"));
        blink_led();
        txFailCount = 0;
        lastSentFull = full;
        lastSentHalf = half;
        lastSentLow  = lo;
    } else {
        Serial.print(F("Transmission Failed, code: "));
        Serial.println(state);
        txFailCount++;
        if (txFailCount >= 3) {
            Serial.println(F("Reinitializing radio..."));
            radio.begin(865.0, 125.0, 9, 7, 0x34, 20, 8, 0);
            radio.setOutputPower(20);
            txFailCount = 0;
        }
    }
}

void loop() {
    // ** Read and transmit current state (regular heartbeat) **
    uint8_t lo, half, full;
    readSwitches(lo, half, full);
    transmitState(lo, half, full);

    // ** Sleep with state-change detection **
    // Instead of sleeping 30s blindly, poll switches every 1s.
    // If state changes and stays stable for DEBOUNCE_STABLE_MS, send immediately.
    unsigned long sleepStart = millis();
    unsigned long changeDetectedAt = 0;
    bool changeDetected = false;
    uint8_t candidateLo = lo, candidateHalf = half, candidateFull = full;

    while (millis() - sleepStart < TX_INTERVAL_MS) {
        wdt_reset();
        delay(POLL_INTERVAL_MS);

        uint8_t curLo, curHalf, curFull;
        readSwitches(curLo, curHalf, curFull);

        bool stateChanged = (curLo != lastSentLow || curHalf != lastSentHalf || curFull != lastSentFull);

        if (stateChanged) {
            // Check if this is a new change or the same candidate
            if (!changeDetected || curLo != candidateLo || curHalf != candidateHalf || curFull != candidateFull) {
                // New candidate — restart debounce timer
                changeDetectedAt = millis();
                candidateLo   = curLo;
                candidateHalf = curHalf;
                candidateFull = curFull;
                changeDetected = true;
            }

            // If candidate has been stable for DEBOUNCE_STABLE_MS, send immediately
            if (changeDetected && (millis() - changeDetectedAt >= DEBOUNCE_STABLE_MS)) {
                Serial.println(F("State change detected — immediate send"));
                transmitState(candidateLo, candidateHalf, candidateFull);
                changeDetected = false;
                sleepStart = millis();  // Reset heartbeat timer after immediate send
            }
        } else {
            // State matches last sent — cancel any pending change detection
            changeDetected = false;
        }
    }
}

// ** LED Blink Function **
void blink_led() {
    digitalWrite(DEBUG_LED, HIGH);
    delay(200);
    digitalWrite(DEBUG_LED, LOW);
    delay(200);
}
