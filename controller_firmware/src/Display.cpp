#include "Display.h"
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#include "LoRaManager.h"
#include "WiFiManager.h"
#include "MotorControl.h"
#include <Wire.h>
#include <WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include <new>          // placement new (rebuild LCD object with the detected address)

// Screen indices
#define SCREEN_OH_TANK   0
#define SCREEN_UG_TANK   1
#define SCREEN_NETWORK   2
#define SCREEN_DATETIME  3
#define SCREEN_LORA      4
#define SCREEN_COUNT     5

static LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);
static uint8_t  lcdAddr         = LCD_ADDRESS;   // actual address (auto-detected in initDisplay)
static bool     lcdInitOk       = false;
static bool     backlightOn     = true;
static bool     pairingPinShown = false;

static int           currentScreen    = 0;
static unsigned long lastScreenChange = 0;
static unsigned long lastBlCheckMs    = 0;  // last backlight mode check

// LCD blink state machine for lost transmitter
static bool          loraBlinkActive     = false;
static unsigned long loraBlinkStartMs    = 0;
static bool          loraBlinkPhase      = false;  // true = blinking 30s, false = idle 10min
static unsigned long loraBlinkToggleMs   = 0;

// Motor-start countdown screen state
static bool          countdownActive     = false;
static int           lastCountdownSec    = -1;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static void printTankLine(uint8_t row, const char* label, TankState state,
                           bool motorOn, bool displayOnly) {
    char buf[17] = {};
    const char* stateStr = tankStateStr(state);
    const char* motorStr = displayOnly ? "---" : (motorOn ? "ON " : "OFF");
    snprintf(buf, sizeof(buf), "%-2s:%-7s M:%s", label, stateStr, motorStr);
    lcd.setCursor(0, row);
    lcd.print(buf);
}

// ---------------------------------------------------------------------------
//  Screens
// ---------------------------------------------------------------------------

static void showOHTank() {
    lcd.clear();
    printTankLine(0, "OH", ohTankState,  ohMotorRunning, ohDisplayOnly);
    printTankLine(1, "UG", ugTankState, ugMotorRunning, ugDisplayOnly);
}

static void showNetwork() {
    lcd.clear();
    lcd.setCursor(0, 0);
    if (isAPMode) {
        lcd.print("AP: " DEFAULT_AP_SSID);
        lcd.setCursor(0, 1);
        lcd.print(WiFi.softAPIP().toString());
    } else if (WiFi.status() == WL_CONNECTED) {
        String ssid = WiFi.SSID();
        if (ssid.length() > 16) ssid = ssid.substring(0, 16);
        lcd.print(ssid);
        lcd.setCursor(0, 1);
        lcd.print(WiFi.localIP().toString());
    } else {
        lcd.print("WiFi: Disconnected");
        lcd.setCursor(0, 1);
        lcd.print("Reconnecting...");
    }
}

static void showDateTime() {
    lcd.clear();
    String t = getFormattedTime();   // "hh:mm:ss AM DD-MM-YYYY"
    // Row 0: "hh:mm AM"  (fits 16 chars easily)
    // Row 1: DD-MM-YYYY
    int spaceAfterAmpm = t.indexOf(' ', 9); // space after AM/PM
    lcd.setCursor(0, 0);
    lcd.print(t.substring(0, spaceAfterAmpm)); // "hh:mm:ss AM"
    lcd.setCursor(0, 1);
    lcd.print(t.substring(spaceAfterAmpm + 1)); // "DD-MM-YYYY"
}

static void showLora() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(isLoraOperational() ? "LoRa: OK" : "LoRa: ERROR");
    lcd.setCursor(0, 1);
    if (lastLoraReceivedTime == 0) {
        lcd.print("No data yet");
    } else {
        unsigned long secAgo = (millis() - lastLoraReceivedTime) / 1000UL;
        char buf[17];
        snprintf(buf, sizeof(buf), "Last: %lus ago", secAgo);
        lcd.print(buf);
    }
}

// Priority screen during a motor's buzzer-delay countdown: motor + seconds on
// row 0, and a 16-cell bar on row 1 that shrinks as the countdown runs.
static void showMotorStarting(bool isOH, int secs) {
    if (!backlightOn) { lcd.backlight(); backlightOn = true; }
    // Redraw only when the second changes (prevents flicker / I2C spam).
    if (countdownActive && secs == lastCountdownSec) return;
    countdownActive  = true;
    lastCountdownSec = secs;

    char l0[17];
    snprintf(l0, sizeof(l0), "%s Motor %6ds", isOH ? "OH" : "UG", secs);
    lcd.setCursor(0, 0);
    lcd.print(l0);

    const int total = (int)(MOTOR_START_BUZZER_DELAY_MS / 1000);
    int filled = (total > 0) ? (secs * 16 / total) : 0;
    if (filled > 16) filled = 16;
    if (filled < 0)  filled = 0;
    lcd.setCursor(0, 1);
    for (int i = 0; i < 16; i++) lcd.write(i < filled ? (uint8_t)0xFF : (uint8_t)' ');
}

static void renderCurrentScreen() {
    if (pairingPinShown) return;   // Pairing PIN has priority
    switch (currentScreen) {
        case SCREEN_OH_TANK:
        case SCREEN_UG_TANK:  showOHTank();  break;  // Both show combined view
        case SCREEN_NETWORK:  showNetwork(); break;
        case SCREEN_DATETIME: showDateTime(); break;
        case SCREEN_LORA:     showLora();    break;
        default:              showOHTank();  break;
    }
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

// Scan the I2C bus and return the LCD backpack address.  PCF8574 boards sit at
// 0x20–0x27 and PCF8574A at 0x38–0x3F, so we probe those (0x27/0x3F first, the
// two most common) and skip the RTC (0x68) / EEPROM (0x50–0x57) on the same bus.
// Falls back to the compile-time LCD_ADDRESS if nothing matching is found.
static uint8_t findLcdAddress() {
    String found;
    for (uint8_t a = 0x08; a <= 0x77; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            if (found.length()) found += ", ";
            found += "0x" + String(a, HEX);
        }
    }
    Log(INFO, "[I2C] Devices found: " + (found.length() ? found : String("none")));

    const uint8_t candidates[] = {
        0x27, 0x3F,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E
    };
    for (uint8_t a : candidates) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) return a;
    }
    Log(WARN, "[Display] No LCD backpack detected – falling back to 0x" + String(LCD_ADDRESS, HEX));
    return LCD_ADDRESS;
}

void initDisplay() {
    // Auto-detect the LCD's I2C address (backpacks vary: 0x27 vs 0x3F, etc.).
    // If it differs from the compile-time default, rebuild the LCD object
    // in-place (placement new) so every existing lcd.* call keeps working.
    lcdAddr = findLcdAddress();
    if (lcdAddr != LCD_ADDRESS) {
        lcd.~LiquidCrystal_I2C();
        new (&lcd) LiquidCrystal_I2C(lcdAddr, LCD_COLUMNS, LCD_ROWS);
    }

    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Tank Monitor");
    lcd.setCursor(0, 1);
    lcd.print("  FW: " FW_VERSION "  ");
    lcdInitOk   = true;
    backlightOn = true;
    applyBacklightMode();  // apply persisted mode at startup
    Log(INFO, "[Display] LCD initialised at 0x" + String(lcdAddr, HEX));
}

uint8_t getLcdAddress() { return lcdAddr; }

void updateDisplay() {
    if (!lcdInitOk || pairingPinShown) return;

    unsigned long now = millis();

    // --- Priority: a motor is about to start (buzzer-delay countdown) ---
    bool isOH = true;
    int cdSecs = getPendingStartCountdown(&isOH);
    if (cdSecs >= 0) {
        showMotorStarting(isOH, cdSecs);
        return;
    }
    if (countdownActive) {
        // Countdown ended (motor started or cancelled) — resume normal display
        countdownActive  = false;
        lastCountdownSec = -1;
        lastScreenChange = now;
        renderCurrentScreen();
    }

    // --- LCD blink for lost transmitter ---
    if (isTransmitterLost()) {
        if (!loraBlinkActive) {
            loraBlinkActive   = true;
            loraBlinkPhase    = true;   // start with 30s blink
            loraBlinkStartMs  = now;
            loraBlinkToggleMs = now;
            Log(INFO, "[Display] Transmitter lost – starting LCD blink");
        }
        if (loraBlinkPhase) {
            // Blinking phase: toggle backlight every 500ms for 30s
            if (now - loraBlinkToggleMs >= 500UL) {
                loraBlinkToggleMs = now;
                backlightOn = !backlightOn;
                if (backlightOn) lcd.backlight(); else lcd.noBacklight();
            }
            if (now - loraBlinkStartMs >= 30000UL) {
                // Switch to 10 min idle phase
                loraBlinkPhase   = false;
                loraBlinkStartMs = now;
                lcd.noBacklight();
                backlightOn = false;
            }
        } else {
            // Idle phase: wait 10 min, then restart blink
            if (now - loraBlinkStartMs >= 600000UL) {
                loraBlinkPhase   = true;
                loraBlinkStartMs = now;
            }
        }
    } else if (loraBlinkActive) {
        // Transmitter signal restored – stop blinking, restore normal mode
        loraBlinkActive = false;
        applyBacklightMode();
        Log(INFO, "[Display] Transmitter signal restored – LCD blink stopped");
    }

    // Re-check backlight every 30 s (auto mode follows time-of-day) — only when not blinking
    if (!loraBlinkActive && (now - lastBlCheckMs >= 30000UL || lastBlCheckMs == 0)) {
        lastBlCheckMs = now;
        applyBacklightMode();
    }

    if (now - lastScreenChange >= LCD_SCREEN_DURATION_MS) {
        currentScreen = (currentScreen + 1) % SCREEN_COUNT;
        lastScreenChange = now;
        renderCurrentScreen();
    } else if ((currentScreen == SCREEN_OH_TANK || currentScreen == SCREEN_UG_TANK)) {
        // Refresh tank screen every 1s so level/motor changes appear instantly
        static unsigned long lastTankRefreshMs = 0;
        if (now - lastTankRefreshMs >= 1000UL) {
            lastTankRefreshMs = now;
            showOHTank();
        }
    }
}

void displayPairingPin(uint32_t pin) {
    pairingPinShown = true;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("** BLE PAIRING **");
    char buf[7];
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)pin);
    lcd.setCursor(0, 1);
    lcd.print("PIN: ");
    lcd.print(buf);
    Log(INFO, "[Display] BLE pairing PIN shown: " + String(buf));
}

void clearPairingPinDisplay() {
    pairingPinShown = false;
    lcd.clear();
    renderCurrentScreen();
    Log(INFO, "[Display] Pairing PIN cleared");
}

void setLcdBacklight(bool on) {
    backlightOn = on;
    if (on) lcd.backlight();
    else    lcd.noBacklight();
}

bool isLcdBacklightOn() {
    return backlightOn;
}

// Apply the current lcdBacklightMode — called at startup and every 30 s
void applyBacklightMode() {
    if (!lcdInitOk) return;
    if (lcdBacklightMode == LCD_BL_ALWAYS_ON) {
        setLcdBacklight(true);
    } else if (lcdBacklightMode == LCD_BL_ALWAYS_OFF) {
        setLcdBacklight(false);
    } else {
        // AUTO: off during daytime 07:00–17:30, on at night
        struct tm ti;
        if (getLocalTime(&ti, 100)) {
            int totalMin = ti.tm_hour * 60 + ti.tm_min;
            bool isDaytime = (totalMin >= 7 * 60 && totalMin < 17 * 60 + 30);
            setLcdBacklight(!isDaytime);
        }
    }
}
