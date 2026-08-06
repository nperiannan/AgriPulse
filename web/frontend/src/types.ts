export interface Schedule {
  i: number    // slot index in ESP32 schedules[]
  m: 'OH' | 'UG'
  t: string    // "HH:MM"
  d: number    // duration in minutes
  on: boolean  // currently running
}

export interface Status {
  oh_state:      string
  ug_state:      string
  oh_motor:      boolean
  ug_motor:      boolean
  lora_ok:       boolean
  wifi_rssi:     number
  uptime_s:      number
  fw:            string
  time:          string
  schedules:     Schedule[]
  // v2.0 fields
  oh_last_known:   string   // last known OH level when transmitter lost
  tx_lost:         boolean  // true when transmitter hasn't reported in 90s
  oh_start_level:  number   // motor start threshold (1=EMPTY, 2=LOW, 3=HALF)
  oh_stop_level:   number   // motor stop threshold (2=LOW, 3=HALF, 4=FULL)
  oh_max_run_min:  number   // max motor runtime in minutes (5-60)
  mqtt_watchdog_min: number // reboot if MQTT disconnected this many minutes (10-60)
  tx_fw:           string   // transmitter firmware version
  // Settings
  oh_disp_only:  boolean
  ug_disp_only:  boolean
  ug_ignore:     boolean
  buzzer_delay:  boolean
  manual_auto_stop: boolean
  lcd_bl_mode:   number   // 0=auto, 1=always_on, 2=always_off
  log_level:     string   // 'info' | 'debug'
}

export interface ControlCmd {
  cmd:       string
  motor?:    number
  time?:     string
  duration?: number
  index?:    number
  key?:      string
  value?:    boolean | number
  mode?:     string
  url?:      string
  pass?:     string
  level?:    string   // for set_log_level
}

export interface OtaStatus {
  has_firmware: boolean
  filename:     string
  size:         number
  uploaded_at:  string
  phase:        string   // idle | triggered | ack_received | downloading | success | failed
  prev_fw:      string
}
