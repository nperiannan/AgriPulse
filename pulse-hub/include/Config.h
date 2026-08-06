#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//                              FIRMWARE VERSION
// =============================================================================
#define FW_VERSION "2.10.0"

// Known transmitter firmware version (update here when transmitter is reflashed).
#define TRANSMITTER_FW_VERSION "2.0.0"

// =============================================================================
//                              BOARD REVISION SELECT
// =============================================================================
// The v2.0 PCB (EasyEDA "esp32-otg" rev 1.0) moved a few pins vs the original
// v1.x board.  Only those pins are guarded by BOARD_V2 (see each #ifdef below);
// everything else is identical on both.
//   v2.0 differences: I2C moved to GPIO8/9, RFM95 DIO1 not wired, and the
//   touch/float inputs moved off the JTAG lines (GPIO40/41/42) to GPIO17/18/47.
//
// This project targets the v1.x board, so BOARD_V2 is left undefined here and
// the v1 envs (`nebulas3` / `nebulas3_serial`) are used for all builds.
// BOARD_V2 can still be supplied by the PlatformIO build environment
// (`-D BOARD_V2` in the *_v2 envs, e.g. `nebulas3_v2` / `nebulas3_v2_serial`),
// or by uncommenting the line below to force v2.0 pins in any build.
// #define BOARD_V2

// =============================================================================
//                              I2C CONFIGURATION
// =============================================================================
#ifdef BOARD_V2
  #define SDA_PIN    8     // v2.0: I2C1_SDA on GPIO8
  #define SCL_PIN    9     // v2.0: I2C1_SCL on GPIO9
#else
  #define SDA_PIN   18     // v1.x
  #define SCL_PIN   17     // v1.x
#endif

#define LCD_ADDRESS  0x27
#define LCD_COLUMNS  16
#define LCD_ROWS      2

// =============================================================================
//                              SPI / LORA PINS  (RFM95/96 on HSPI)
// =============================================================================
#define HSPI_MISO  12
#define HSPI_MOSI  11
#define HSPI_SCLK  13
#define HSPI_SS    10
#define LORA_CS    10
#define LORA_IRQ   14
#define LORA_RST   21
#ifdef BOARD_V2
  #define LORA_DIO1  RADIOLIB_NC   // v2.0: RFM95 DIO1 tied to GND; GPIO8 now used by I2C SDA
#else
  #define LORA_DIO1  8             // v1.x
#endif

// LoRa radio parameters  (must match the remote OH-tank node)
#define LORA_FREQUENCY          865.0f
#define LORA_BANDWIDTH          125.0f
#define LORA_SPREADING_FACTOR     9
#define LORA_CODING_RATE          7
#define LORA_SYNC_WORD         0x34
#define LORA_TX_POWER            20
#define LORA_PREAMBLE_LENGTH      8

// Packet type bytes
#define LORA_PKT_FLOAT_SWITCH  0x02   // Float switch state

// =============================================================================
//                              RELAY / MOTOR PINS
// =============================================================================
#define OH_RELAY_PIN  1    // RLY1 – Overhead tank motor
#define UG_RELAY_PIN  2    // RLY2 – Underground tank motor

// Relay logic: HIGH = motor ON
#define RELAY_ON   HIGH
#define RELAY_OFF  LOW

// =============================================================================
//                              UG TANK FLOAT SWITCH PINS
// =============================================================================
// Single-pin wiring (3-wire float switch):
//   NC  ──── GND
//   COM ──── GPIO 41  (INPUT_PULLUP)
//   NO  ──── 3.3 V
// Float UP   (tank FULL)  → COM→NO 3.3V  → pin HIGH
// Float DOWN (tank EMPTY) → COM→NC GND   → pin LOW  (GND overrides pullup)
// Disconnected/wire loose → pullup holds HIGH = FULL  (safe: motor stays off)
#ifdef BOARD_V2
  // v2.0 has no dedicated float net (GPIO42 is JTAG TMS), so the float switch
  //   wires to a spare header GPIO instead.  IO47 is broken out on the 10-pin
  //   GPIO header — connect the float COM lead there.
  #define UG_FLOAT_PIN  47   // spare header GPIO (10-pin GPIO breakout)
#else
  #define UG_FLOAT_PIN  42   // v1.x
#endif

// =============================================================================
//                              OTHER HARDWARE PINS
// =============================================================================
#define BUZZER_PIN   3
#define DEBUG_LED   38

// =============================================================================
//                              TOUCH SWITCH PINS (TTP223)
// =============================================================================
// TTP223 default mode: A=open, B=open → momentary HIGH while touched.
// We detect rising edge (LOW→HIGH) to toggle the motor.
#ifdef BOARD_V2
  // v2.0 has no touch nets (GPIO40/41 are JTAG TDO/TDI).  I2C moved off
  //   GPIO17/18, freeing them on the left header — wire the two buttons there.
  #define TOUCH_OH_PIN   17   // freed header GPIO (was I2C SCL on v1.x)
  #define TOUCH_UG_PIN   18   // freed header GPIO (was I2C SDA on v1.x)
#else
  #define TOUCH_OH_PIN   41   // v1.x  (TTP223 I/O → GPIO41)
  #define TOUCH_UG_PIN   40   // v1.x  (TTP223 I/O → GPIO40)
#endif
#define TOUCH_DEBOUNCE_MS  50UL   // Ignore re-trigger within 50 ms

// =============================================================================
//                              TIMING CONSTANTS (ms)
// =============================================================================
#define FLOAT_DEBOUNCE_MS         3000UL   // 3 s debounce for float switch state change
#define UG_SENSOR_POLL_MS         5000UL   // Poll UG float switch every 5 s
#define LCD_SCREEN_DURATION_MS    5000UL   // Each LCD screen visible for 5 s
#define LORA_EXPECTED_INTERVAL_MS  30000UL   // Transmitter sends every 30 s
#define LORA_MAX_MISSED_PACKETS       10    // 10 × 30s = 5 min → transmitter lost
#define LORA_MOTOR_SAFETY_TIMEOUT_MS  300000UL  // 5 min – stop motor if no LoRa while running
#define MAX_LORA_RETRIES              3
#define LORA_REINIT_INTERVAL_MS   60000UL
#define WIFI_CHECK_INTERVAL_MS    30000UL
#define WIFI_ATTEMPT_TIMEOUT_MS   10000UL   // wait up to 10 s per connection attempt
#define WIFI_MAX_ATTEMPTS_PER_NET 3          // attempts per SSID before trying next
#define WIFI_COOLDOWN_MS          900000UL   // 15-min pause after all SSIDs fail
#define NTP_SYNC_INTERVAL_MS    3600000UL
#define BLE_STATUS_INTERVAL_MS    5000UL
#define BUZZER_MAX_DURATION_MS      35000UL   // Auto-stop buzzer after 35 s
#define MOTOR_START_BUZZER_DELAY_MS 30000UL   // Buzz 30 s before motor starts
#define BOOT_GRACE_PERIOD_MS        20000UL   // Auto-control & scheduler suppressed for 20 s after boot
#define MOTOR_MIN_RUN_MS            60000UL   // Hysteresis: motor stays on for at least 60 s once started

// -----------------------------------------------------------------------------
// Motor start rejection codes — published as "oh_rej"/"ug_rej" in the status
// JSON. A refused start produces no motor/buzzer state change, so without this
// the app can only time out and show a misleading "command not delivered".
// The code is cleared on the next manual command for that motor.
// -----------------------------------------------------------------------------
#define MOTOR_REJ_NONE        0
#define MOTOR_REJ_TANK_FULL   1   // Start refused — tank already FULL (auto-stop would fire at once)

// =============================================================================
//                              BLE CONFIGURATION
// =============================================================================
// UUIDs must match the Flutter mobile app.
#define BLE_SERVICE_UUID       "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_TX_CHARACTERISTIC  "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_RX_CHARACTERISTIC  "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define BLE_DEVICE_NAME        "AgriPulse"

// BLE command strings (phone → ESP32)
#define CMD_GET_STATUS   "GET_STATUS"
#define CMD_MOTOR_OH_ON  "MOTOR_OH_ON"
#define CMD_MOTOR_OH_OFF "MOTOR_OH_OFF"
#define CMD_MOTOR_UG_ON  "MOTOR_UG_ON"
#define CMD_MOTOR_UG_OFF "MOTOR_UG_OFF"
#define CMD_GET_CONFIG   "GET_CONFIG"
#define CMD_SET_CONFIG   "SET_CONFIG"
#define CMD_SYNC_TIME    "SYNC_TIME"
#define CMD_BUZZER_ON    "BUZZER_ON"
#define CMD_BUZZER_OFF   "BUZZER_OFF"

// =============================================================================
//                              NVS NAMESPACES & KEYS
// =============================================================================
#define NVS_WIFI_NS       "wifi_config"
#define NVS_MOTOR_NS      "motor_cfg"
#define NVS_DISPLAY_NS    "display_cfg"
#define NVS_BUZZER_NS     "buzzer_cfg"
#define NVS_BLE_NS        "tank_settings"

#define NVS_KEY_WIFI_JSON      "wifi_json"       // JSON array of {ssid,pass} in NVS_WIFI_NS
#define NVS_KEY_WIFI_MODE      "ap_mode"
#define NVS_KEY_OH_DISP_ONLY   "oh_disp_only"
#define NVS_KEY_UG_DISP_ONLY   "ug_disp_only"
#define NVS_KEY_UG_IGNORE      "ug_ignore"       // Ignore UG state when deciding OH motor
#define NVS_KEY_BUZZER_DELAY   "buzzer_delay"    // Buzz before motor start
#define NVS_KEY_MANUAL_ASTOP   "man_auto_stop"   // Stop manual motors when tank full
#define NVS_KEY_BLE_ENABLED    "ble_enabled"
#define NVS_KEY_LCD_BL_MODE    "lcd_bl_mode"
#define NVS_KEY_OH_START_LVL   "oh_start_lvl"   // TankState for motor start threshold
#define NVS_KEY_OH_STOP_LVL    "oh_stop_lvl"    // TankState for motor stop threshold
#define NVS_KEY_OH_MAX_RUN     "oh_max_run"     // Max motor runtime in minutes (5-60)
#define NVS_KEY_MQTT_WATCHDOG   "mqtt_wd_min"    // MQTT watchdog: reboot after this many minutes disconnected (10-60)
#define NVS_KEY_OH_MOTOR_INTENT "oh_intent"     // Motor was running before power loss
#define NVS_KEY_UG_MOTOR_INTENT "ug_intent"     // Motor was running before power loss
#define NVS_KEY_HEARTBEAT       "hb_epoch"      // Last-alive epoch (power-cut off-time estimate)

// LCD backlight modes
#define LCD_BL_AUTO       0   // Off 7:00 AM – 5:30 PM, On at night (default)
#define LCD_BL_ALWAYS_ON  1   // Always on
#define LCD_BL_ALWAYS_OFF 2   // Always off

// MQTT watchdog: esp_restart() if MQTT stays disconnected this long (minutes)
#define MQTT_WATCHDOG_DEFAULT_MIN 15
#define MQTT_WATCHDOG_MIN_MIN     10
#define MQTT_WATCHDOG_MAX_MIN     60

// =============================================================================
//                              EEPROM (AT24C512 / 24T512) - I2C
// =============================================================================
#define EEPROM_I2C_ADDR    0x50      // Default I2C address (A2..A0 = GND → 0x50-0x57)
#define EEPROM_PAGE_SIZE   128       // Bytes per page for AT24C512
#define EEPROM_SIZE_BYTES  65536     // 512 kbit = 64 KB

// History circular buffer layout inside EEPROM:
//   Addr 0-7  : Header  (magic[2] + head[2] + count[2] + rsvd[2])
//   Addr 8+   : Records (8 bytes each, 8191 records max)
#define HIST_HEADER_ADDR   0
#define HIST_DATA_ADDR     8
// HIST_MAX_RECORDS is computed in History.cpp to avoid sizeof in header macros

// =============================================================================
//                              MQTT CONFIGURATION
// =============================================================================
// Credentials are seeded into NVS on first boot; change via web UI or redefine here.
#define MQTT_BROKER_DEFAULT   "nperiannan-nas.freemyip.com"
#define MQTT_PORT_DEFAULT     1883
#define MQTT_PORT_FALLBACK    8883            // Alternate port if primary is blocked by ISP
#define MQTT_USER_DEFAULT     "tankmonitor"
#define MQTT_PASS_DEFAULT     "Tank32!"
#define MQTT_LOCATION_DEFAULT "home"          // Legacy NVS key — no longer used for topics (topics now use MAC)

#define MQTT_NVS_NS           "mqtt_cfg"
// Periodic status publish now mainly exists as a keep-alive so `last_seen`/
// online status stays fresh and slow-changing fields (uptime, wifi rssi,
// wifi ssid) eventually reach the backend even with no discrete event.
// Motor changes, tank-state changes, and the app's manual "sync" command all
// still trigger an immediate out-of-band publish (see publishMQTTStatus()
// call sites in MotorControl.cpp / LoRaManager.cpp / Sensors.cpp / the
// "sync" cmd handler below) — so real changes are never delayed by this.
#define MQTT_PUBLISH_MS       60000UL         // Keep-alive status publish every 60 s
#define MQTT_RECONNECT_MS     15000UL         // Retry connection every 15 s
#define MQTT_PORT_FAIL_LIMIT  3               // Switch to fallback port after N consecutive failures
#define MQTT_HEARTBEAT_MS     300000UL        // Controller-initiated round-trip health check (tm/{mac}/hb) every 5 min
#define MQTT_HEARTBEAT_ACK_TIMEOUT_MS 20000UL  // Warn locally if backend doesn't ping back within this window

// HTTP OTA poll — ESP32 checks server for staged firmware over HTTP,
// independent of MQTT.  Uses web server port (1880) which ISPs don't block.
#define OTA_POLL_PORT         1880
#define OTA_POLL_INTERVAL_MS  300000UL        // Poll every 5 min

// =============================================================================
//                              WIFI DEFAULTS
// =============================================================================
#define DEFAULT_AP_SSID      "AgriPulse"
#define DEFAULT_AP_PASSWORD  "tank1234"
#define MAX_WIFI_NETWORKS        5
#define WIFI_RECONNECT_DELAY_MS  5000UL
#define MAX_WIFI_CONNECT_ATTEMPTS   20

// =============================================================================
//                              LCD SCHEDULE DEFAULTS
// =============================================================================
#define DEFAULT_LCD_ON_TIME   "17:30"
#define DEFAULT_LCD_OFF_TIME  "06:30"

#endif // CONFIG_H
