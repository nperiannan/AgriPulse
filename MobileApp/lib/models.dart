class Schedule {
  final int i;
  final String m; // 'OH' or 'UG'
  final String t; // "HH:MM"
  final int d;    // duration minutes
  final bool on;  // currently running

  const Schedule({
    required this.i,
    required this.m,
    required this.t,
    required this.d,
    required this.on,
  });

  factory Schedule.fromJson(Map<String, dynamic> j) => Schedule(
        i: j['i'] as int,
        m: j['m'] as String,
        t: j['t'] as String,
        d: j['d'] as int,
        on: j['on'] as bool,
      );
}

class Status {
  final String ohState;
  final String ugState;
  final bool ohMotor;
  final bool ugMotor;
  final bool loraOk;
  final double loraRssi;
  final double loraSNR;
  final String lastLoraReceived;
  final int wifiRssi;
  final int uptimeS;
  final String fw;
  final String time;
  final List<Schedule> schedules;
  final bool ohDispOnly;
  final bool ugDispOnly;
  final bool ugIgnore;
  final bool buzzerDelay;
  final bool manualAutoStop;
  final int  lcdBlMode;   // 0=auto, 1=always_on, 2=always_off
  final String logLevel;  // 'info' | 'debug'
  final bool buzzerActive;
  final bool ohBuzzer;    // buzzer countdown running for OH motor
  final bool ugBuzzer;    // buzzer countdown running for UG motor
  final int  ohCd;        // buzzer countdown remaining seconds for OH (0 when idle)
  final int  ugCd;        // buzzer countdown remaining seconds for UG (0 when idle)
  final int  ohRej;       // MOTOR_REJ_* — why the last OH manual start did nothing (0 = honoured)
  final int  ugRej;       // MOTOR_REJ_* — why the last UG manual start did nothing (0 = honoured)
  final String txFw;      // transmitter firmware version
  final String mgmtIp;    // ESP32 management IP (WiFi STA IP)
  final String wifiSsid;  // connected SSID, empty when in AP mode
  final String ohLastKnown; // last valid OH state before UNKNOWN
  final bool txLost;      // transmitter declared lost
  final int  ohStartLevel; // motor start threshold (TankState enum)
  final int  ohStopLevel;  // motor stop threshold
  final int  ohMaxRunMin;  // max motor runtime in minutes
  final int  mqttWatchdogMin; // reboot after this many minutes disconnected (10-60)

  const Status({
    required this.ohState,
    required this.ugState,
    required this.ohMotor,
    required this.ugMotor,
    required this.loraOk,
    required this.loraRssi,
    required this.loraSNR,
    required this.lastLoraReceived,
    required this.wifiRssi,
    required this.uptimeS,
    required this.fw,
    required this.time,
    required this.schedules,
    required this.ohDispOnly,
    required this.ugDispOnly,
    required this.ugIgnore,
    required this.buzzerDelay,
    required this.manualAutoStop,
    required this.lcdBlMode,
    required this.logLevel,
    required this.buzzerActive,
    required this.ohBuzzer,
    required this.ugBuzzer,
    required this.ohCd,
    required this.ugCd,
    required this.ohRej,
    required this.ugRej,
    required this.txFw,
    required this.mgmtIp,
    required this.wifiSsid,
    required this.ohLastKnown,
    required this.txLost,
    required this.ohStartLevel,
    required this.ohStopLevel,
    required this.ohMaxRunMin,
    required this.mqttWatchdogMin,
  });

  factory Status.fromJson(Map<String, dynamic> j) => Status(
        ohState:    j['oh_state']    as String? ?? '',
        ugState:    j['ug_state']    as String? ?? '',
        ohMotor:    j['oh_motor']    as bool?   ?? false,
        ugMotor:    j['ug_motor']    as bool?   ?? false,
        loraOk:     j['lora_ok']     as bool?   ?? false,
        loraRssi:   (j['loraRSSI']   as num?)?.toDouble() ?? 0.0,
        loraSNR:    (j['loraSNR']    as num?)?.toDouble() ?? 0.0,
        lastLoraReceived: j['lastLoraReceived'] as String? ?? '',
        wifiRssi:   j['wifi_rssi']   as int?    ?? 0,
        uptimeS:    j['uptime_s']    as int?    ?? 0,
        fw:         j['fw']          as String? ?? '',
        time:       j['time']        as String? ?? '',
        schedules:  (j['schedules']  as List<dynamic>? ?? [])
            .map((e) => Schedule.fromJson(e as Map<String, dynamic>))
            .toList(),
        ohDispOnly: j['oh_disp_only'] as bool? ?? false,
        ugDispOnly: j['ug_disp_only'] as bool? ?? false,
        ugIgnore:   j['ug_ignore']    as bool? ?? false,
        buzzerDelay:j['buzzer_delay'] as bool? ?? false,
        manualAutoStop:j['manual_auto_stop'] as bool? ?? true,
        lcdBlMode:  j['lcd_bl_mode']  as int?  ?? 0,
        logLevel:   j['log_level']    as String? ?? 'info',
        buzzerActive: j['buzzer_active'] as bool? ?? false,
        ohBuzzer:   j['oh_buzzer']    as bool? ?? false,
        ugBuzzer:   j['ug_buzzer']    as bool? ?? false,
        ohCd:       j['oh_cd']         as int?  ?? 0,
        ugCd:       j['ug_cd']         as int?  ?? 0,
        ohRej:      j['oh_rej']        as int?  ?? 0,
        ugRej:      j['ug_rej']        as int?  ?? 0,
        txFw:       j['tx_fw']        as String? ?? '',
        mgmtIp:     j['ip']           as String? ?? '',
        wifiSsid:   j['wifi_ssid']    as String? ?? '',
        ohLastKnown: j['oh_last_known'] as String? ?? '',
        txLost:     j['tx_lost']      as bool?   ?? false,
        ohStartLevel: j['oh_start_level'] as int? ?? 1,
        ohStopLevel:  j['oh_stop_level']  as int? ?? 4,
        ohMaxRunMin:  j['oh_max_run_min'] as int? ?? 20,
        mqttWatchdogMin: j['mqtt_watchdog_min'] as int? ?? 15,
      );
}

class Device {
  final String mac;
  final String typeId;
  final String displayName;
  final String fwVersion;
  final String role;   // 'owner' | 'viewer' | '' (unclaimed, admin-visible only)
  final bool online;

  const Device({
    required this.mac,
    required this.typeId,
    required this.displayName,
    required this.fwVersion,
    required this.role,
    required this.online,
  });

  factory Device.fromJson(Map<String, dynamic> j) => Device(
        mac:         j['mac']          as String? ?? '',
        typeId:      j['type_id']      as String? ?? '',
        displayName: j['display_name'] as String? ?? '',
        fwVersion:   j['fw_version']   as String? ?? '',
        role:        j['role']         as String? ?? '',
        online:      j['online']       as bool?   ?? false,
      );
}

/// A locally-recorded motor command that the controller never acknowledged
/// (no ack received within the timeout). Shown as a separate card in History.
class DeliveryIssue {
  final DateTime time;
  final String motor;      // 'OH' | 'UG'
  final bool start;        // true = start request, false = stop/cancel request
  final String deviceName; // device display name at the time

  const DeliveryIssue({
    required this.time,
    required this.motor,
    required this.start,
    required this.deviceName,
  });

  Map<String, dynamic> toJson() => {
        't': time.toIso8601String(),
        'm': motor,
        's': start,
        'd': deviceName,
      };

  factory DeliveryIssue.fromJson(Map<String, dynamic> j) => DeliveryIssue(
        time: DateTime.tryParse(j['t'] as String? ?? '') ?? DateTime.now(),
        motor: j['m'] as String? ?? '',
        start: j['s'] as bool? ?? true,
        deviceName: j['d'] as String? ?? '',
      );
}
