import 'dart:async';
import 'dart:convert';
import 'package:http/http.dart' as http;
import 'models.dart';

/// Talks directly to the ESP32 controller's HTTP API (port 80).
/// No auth required — relies on network security (home WiFi / AP).
class DirectDeviceService {
  final String baseUrl; // e.g. "http://192.168.0.105" or "http://192.168.4.1"

  DirectDeviceService(this.baseUrl);

  // ── Status polling ──────────────────────────────────────────────────────

  /// Fetches /status + /systeminfo + /schedulelist and merges into a Status.
  Future<Status?> fetchStatus() async {
    try {
      final results = await Future.wait([
        http.get(Uri.parse('$baseUrl/status')).timeout(const Duration(seconds: 5)),
        http.get(Uri.parse('$baseUrl/systeminfo')).timeout(const Duration(seconds: 5)),
        http.get(Uri.parse('$baseUrl/schedulelist')).timeout(const Duration(seconds: 5)),
      ]);

      if (results[0].statusCode != 200) return null;

      final s = jsonDecode(results[0].body) as Map<String, dynamic>;
      final si = results[1].statusCode == 200
          ? jsonDecode(results[1].body) as Map<String, dynamic>
          : <String, dynamic>{};
      final sl = results[2].statusCode == 200
          ? jsonDecode(results[2].body) as List<dynamic>
          : <dynamic>[];

      return Status.fromJson(_mapStatus(s, si, sl));
    } catch (_) {
      return null;
    }
  }

  /// Translate controller JSON fields into the Status.fromJson format.
  Map<String, dynamic> _mapStatus(
    Map<String, dynamic> s,
    Map<String, dynamic> si,
    List<dynamic> sl,
  ) {
    // Build schedules array matching the WS format {i, m, t, d, on}
    final schedules = <Map<String, dynamic>>[];
    for (int i = 0; i < sl.length; i++) {
      final sched = sl[i] as Map<String, dynamic>;
      if (sched['enabled'] == true) {
        schedules.add({
          'i': i,
          'm': (sched['motorType'] as int?) == 1 ? 'UG' : 'OH',
          't': sched['time'] as String? ?? '00:00',
          'd': sched['duration'] as int? ?? 0,
          'on': sched['running'] as bool? ?? false,
        });
      }
    }

    return {
      'oh_state': s['ohState'] as String? ?? '',
      'ug_state': s['ugState'] as String? ?? '',
      'oh_motor': (s['oh_motor'] as String?) == 'ON',
      'ug_motor': (s['ug_motor'] as String?) == 'ON',
      'lora_ok': s['loraOk'] as bool? ?? false,
      'loraRSSI': s['loraRSSI'] ?? 0,
      'loraSNR': s['loraSNR'] ?? 0,
      'lastLoraReceived': s['lastLoraReceived'] as String? ?? '',
      'wifi_rssi': 0, // not available from controller /status
      'wifi_ssid': s['wifiSSID'] as String? ?? '',
      'uptime_s': si['uptime'] as int? ?? 0,
      'fw': s['fwVersion'] as String? ?? '',
      'time': s['time'] as String? ?? '',
      'schedules': schedules,
      'oh_disp_only': s['ohDisplayOnly'] as bool? ?? false,
      'ug_disp_only': s['ugDisplayOnly'] as bool? ?? false,
      'ug_ignore': s['ugIgnore'] as bool? ?? false,
      'buzzer_delay': s['buzzerDelay'] as bool? ?? false,
      'manual_auto_stop': s['manualAutoStop'] as bool? ?? true,
      'lcd_bl_mode': s['lcdBlMode'] as int? ?? 0,
      'log_level': s['logLevel'] as String? ?? 'info',
      'buzzer_active': s['buzzerActive'] as bool? ?? false,
      'oh_buzzer': s['ohBuzzer'] as bool? ?? false,
      'ug_buzzer': s['ugBuzzer'] as bool? ?? false,
      'oh_cd': (s['ohCd'] as num?)?.toInt() ?? 0,
      'ug_cd': (s['ugCd'] as num?)?.toInt() ?? 0,
      'oh_rsn': (s['ohRsn'] as num?)?.toInt() ?? 0,
      'ug_rsn': (s['ugRsn'] as num?)?.toInt() ?? 0,
      'oh_rej': (s['ohRej'] as num?)?.toInt() ?? 0,
      'ug_rej': (s['ugRej'] as num?)?.toInt() ?? 0,
      'tx_fw': s['txFw'] as String? ?? '',
      'ip': s['wifiIP'] as String? ?? '',
      'oh_last_known': s['ohLastKnown'] as String? ?? '',
      'tx_lost': s['txLost'] as bool? ?? false,
      'oh_start_level': s['ohStartLevel'] as int? ?? 1,
      'oh_stop_level': s['ohStopLevel'] as int? ?? 4,
      'oh_max_run_min': s['ohMaxRunMin'] as int? ?? 20,
      'mqtt_watchdog_min': s['mqttWatchdogMin'] as int? ?? 15,
    };
  }

  // ── Motor control ───────────────────────────────────────────────────────

  Future<bool> motorOH(bool on) =>
      _getOk('/motor?state=${on ? "on" : "off"}');

  Future<bool> motorUG(bool on) =>
      _getOk('/undergroundmotor?state=${on ? "on" : "off"}');

  // ── Settings ────────────────────────────────────────────────────────────

  Future<bool> setConfig({
    bool? ohDispOnly,
    bool? ugDispOnly,
    bool? ugIgnore,
    bool? buzzerDelay,
    bool? manualAutoStop,
    int? ohStartLevel,
    int? ohStopLevel,
    int? ohMaxRun,
  }) async {
    final body = <String, String>{};
    if (ohDispOnly == true) body['oh_disp_only'] = '1';
    if (ugDispOnly == true) body['ug_disp_only'] = '1';
    if (ugIgnore == true) body['ug_ignore'] = '1';
    if (buzzerDelay == true) body['buzzer_delay'] = '1';
    if (manualAutoStop == true) body['manual_auto_stop'] = '1';
    if (ohStartLevel != null) body['oh_start_level'] = '$ohStartLevel';
    if (ohStopLevel != null) body['oh_stop_level'] = '$ohStopLevel';
    if (ohMaxRun != null) body['oh_max_run'] = '$ohMaxRun';
    return _postOk('/setconfig', body);
  }

  Future<bool> setLcdMode(String mode) =>
      _postOk('/setlcdmode', {'mode': mode});

  Future<bool> setLogLevel(String level) =>
      _postOk('/setloglevel', {'level': level});

  Future<bool> setMqttPass(String pass) =>
      _postOk('/setmqttpass', {'pass': pass});

  Future<bool> syncNtp() => _postOk('/syncntp', {});

  Future<bool> reboot() => _postOk('/reboot', {});

  Future<bool> factoryReset() => _postOk('/factoryreset', {});

  // ── Schedules ───────────────────────────────────────────────────────────

  /// Fetches current schedule list from controller.
  Future<List<Map<String, dynamic>>> fetchSchedules() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/schedulelist'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        return (jsonDecode(res.body) as List<dynamic>)
            .cast<Map<String, dynamic>>();
      }
    } catch (_) {}
    return [];
  }

  /// Saves all 10 schedule slots to the controller.
  Future<bool> saveSchedules(List<Map<String, dynamic>> slots) async {
    final body = <String, String>{};
    for (int i = 0; i < 10; i++) {
      if (i < slots.length && slots[i]['enabled'] == true) {
        body['enabled$i'] = '1';
        body['motorType$i'] = '${slots[i]['motorType'] ?? 0}';
        body['time$i'] = slots[i]['time'] as String? ?? '00:00';
        body['duration$i'] = '${slots[i]['duration'] ?? 10}';
      }
    }
    return _postOk('/updateAllSchedules', body);
  }

  Future<bool> cancelSchedule() => _postOk('/cancelSchedule', {});
  Future<bool> clearSchedules() => _postOk('/clearSchedules', {});

  // ── Logs ────────────────────────────────────────────────────────────────

  Future<List<String>> fetchLogs() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/logs'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        final data = jsonDecode(res.body) as List<dynamic>;
        return data.cast<String>();
      }
    } catch (_) {}
    return [];
  }

  Future<bool> clearLogs() => _postOk('/clearlogs', {});

  // ── WiFi management ─────────────────────────────────────────────────────

  Future<List<Map<String, dynamic>>> scanWifi() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/wifiscan'))
          .timeout(const Duration(seconds: 15)); // scan can be slow
      if (res.statusCode == 200) {
        return (jsonDecode(res.body) as List<dynamic>)
            .cast<Map<String, dynamic>>();
      }
    } catch (_) {}
    return [];
  }

  Future<Map<String, dynamic>> fetchWifiList() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/wifilist'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        return jsonDecode(res.body) as Map<String, dynamic>;
      }
    } catch (_) {}
    return {};
  }

  Future<bool> addWifi(String ssid, String password) =>
      _postOk('/addwifi', {'ssid': ssid, 'password': password});

  Future<bool> deleteWifi(String ssid) =>
      _getOk('/deletewifi?ssid=${Uri.encodeComponent(ssid)}');

  Future<bool> setWifiPriority(String ssid, int priority) =>
      _postOk('/setwifipriority', {
        'ssid': ssid,
        'priority': '$priority',
      });

  // ── Event history ───────────────────────────────────────────────────────

  Future<Map<String, dynamic>> fetchHistory() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/history'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        return jsonDecode(res.body) as Map<String, dynamic>;
      }
    } catch (_) {}
    return {'count': 0, 'eeprom': false, 'records': []};
  }

  Future<bool> clearHistory() => _postOk('/clearhistory', {});

  // ── MQTT config ─────────────────────────────────────────────────────────

  Future<Map<String, dynamic>> fetchMqttConfig() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/mqttconfig'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        return jsonDecode(res.body) as Map<String, dynamic>;
      }
    } catch (_) {}
    return {};
  }

  Future<bool> setMqttBroker(String broker, int port) =>
      _postOk('/setmqttbroker', {'broker': broker, 'port': '$port'});

  // ── System info ─────────────────────────────────────────────────────────

  Future<Map<String, dynamic>> fetchSystemInfo() async {
    try {
      final res = await http.get(Uri.parse('$baseUrl/systeminfo'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        return jsonDecode(res.body) as Map<String, dynamic>;
      }
    } catch (_) {}
    return {};
  }

  // ── Helpers ─────────────────────────────────────────────────────────────

  Future<bool> _getOk(String path) async {
    try {
      final res = await http.get(Uri.parse('$baseUrl$path'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        final data = jsonDecode(res.body) as Map<String, dynamic>;
        return data['ok'] == true;
      }
    } catch (_) {}
    return false;
  }

  Future<bool> _postOk(String path, Map<String, String> body) async {
    try {
      final res = await http.post(
        Uri.parse('$baseUrl$path'),
        body: body,
      ).timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        final data = jsonDecode(res.body) as Map<String, dynamic>;
        return data['ok'] == true;
      }
    } catch (_) {}
    return false;
  }
}
