import 'dart:async';
import 'dart:io';
import 'dart:math';
import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:open_filex/open_filex.dart';
import 'package:path_provider/path_provider.dart';
import 'package:provider/provider.dart';
import 'package:http/http.dart' as http;
import 'package:url_launcher/url_launcher.dart';
import 'models.dart';
import 'tank_service.dart';
import 'schedule_sheet.dart';
import 'setup_screen.dart';
import 'login_screen.dart';
import 'device_list_screen.dart';
import 'app_preferences.dart';
import 'dashboard_themes.dart';
import 'theme_data.dart';
import 'wifi_management_screen.dart';
import 'event_history_screen.dart';
import 'push_service.dart';
import 'haptics.dart';

// ─── Colours (resolved from theme for backward-compat helpers) ──────────────
// These are still used by many helper widgets below.
// They get the right dark-mode values; the theme-aware widgets in
// dashboard_themes.dart use cardBg()/cardBd()/labelColor() instead.
const _bg      = kDarkBg;
const _cardBg  = kDarkCard;
const _cardBd  = kDarkCardBd;
const _rowBd   = kDarkCardBd;
// ─── Tank/Motor widgets are now in dashboard_themes.dart ─────────────────────

// ─── Dashboard screen ─────────────────────────────────────────────────────────
class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen>
    with WidgetsBindingObserver {

  double? _downloadProgress; // null = idle, 0.0–1.0 = downloading

  // OTA upload state
  double? _uploadProgress; // null = idle, 0.0–1.0 = uploading
  String? _uploadError;
  // OTA server state (polled from backend)
  bool    _otaHasFirmware  = false;
  String  _otaStagedName   = '';
  int     _otaStagedSize   = 0;
  String  _otaStagedAt     = '';
  // OTA flash state
  bool    _otaBusy         = false;
  String  _otaPhase        = ''; // triggered | ack_received | downloading | success | failed
  int     _otaSecondsElapsed = 0;
  Timer?  _otaCountdownTimer;
  Timer?  _otaPollTimer;

  // Device logs state
  List<String> _deviceLogs = [];
  String?      _logsAt;
  bool         _logsLoading = false;

  // Bottom nav tab
  int _tabIndex = 0;

  // Manual sync button state
  bool _syncing = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<TankService>().checkForUpdate();
      _loadOtaStatus();
      // Register (and keep refreshed) the FCM push token with the backend.
      PushService.onToken((t) => context.read<TankService>().registerPushToken(t));
      // Status is now event/keep-alive driven on the controller side — grab a
      // fresh snapshot as soon as the dashboard opens instead of waiting.
      context.read<TankService>().syncNow();
    });
  }

  @override
  void dispose() {
    _otaCountdownTimer?.cancel();
    _otaPollTimer?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      // Reconnect when app comes back from background
      context.read<TankService>().reconnectIfNeeded();
      context.read<TankService>().syncNow();
    }
  }

  Future<void> _handleSyncNow() async {
    setState(() => _syncing = true);
    AppHaptics.motor();
    await context.read<TankService>().syncNow();
    if (!mounted) return;
    setState(() => _syncing = false);
  }

  // ── OTA helpers ───────────────────────────────────────────────────────────

  Future<void> _loadOtaStatus() async {
    final svc = context.read<TankService>();
    final data = await svc.fetchOtaStatus();
    if (!mounted || data == null) return;
    setState(() {
      _otaHasFirmware = data['has_firmware'] == true;
      _otaStagedName  = data['filename']    as String? ?? '';
      _otaStagedSize  = (data['size'] as num?)?.toInt() ?? 0;
      _otaStagedAt    = data['uploaded_at'] as String? ?? '';
      final serverPhase = data['phase'] as String? ?? '';
      // Only update phase from server if we're not actively tracking a flash
      if (!_otaBusy) _otaPhase = serverPhase;
    });
  }

  Future<void> _pickAndUploadFirmware() async {
    final result = await FilePicker.platform.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['bin'],
      withData: true,
    );
    if (result == null || result.files.isEmpty) return;
    final file = result.files.first;
    if (file.bytes == null) return;
    if (!mounted) return;
    setState(() { _uploadProgress = 0; _uploadError = null; });
    final svc = context.read<TankService>();
    final ok = await svc.uploadFirmware(
      file.bytes!,
      onProgress: (p) {
        if (mounted) setState(() => _uploadProgress = p);
      },
    );
    if (!mounted) return;
    if (ok) {
      setState(() => _uploadProgress = null);
      await _loadOtaStatus();
    } else {
      setState(() { _uploadProgress = null; _uploadError = 'Upload failed — check connection.'; });
    }
  }

  /// Fetches the latest controller firmware.bin from the GitHub release and
  /// stages it to the backend automatically (no manual file picking needed).
  Future<void> _stageFromGitHub() async {
    final svc = context.read<TankService>();
    setState(() { _uploadProgress = 0; _uploadError = null; });
    http.Client? client;
    try {
      final asset = await svc.fetchLatestControllerFirmware();
      if (asset == null) {
        if (mounted) setState(() {
          _uploadProgress = null;
          _uploadError = 'No controller firmware release found on GitHub.';
        });
        return;
      }
      // ── Download the .bin from GitHub (first half of the progress bar) ──
      client = http.Client();
      final resp = await client.send(http.Request('GET', Uri.parse(asset.url)));
      if (resp.statusCode != 200) {
        throw Exception('GitHub returned ${resp.statusCode}');
      }
      final total = resp.contentLength ?? 0;
      final bytes = <int>[];
      await for (final chunk in resp.stream) {
        bytes.addAll(chunk);
        if (mounted && total > 0) {
          setState(() => _uploadProgress = (bytes.length / total) * 0.5);
        }
      }
      client.close();
      client = null;
      if (bytes.length < 100 * 1024) {
        throw Exception('firmware too small (${bytes.length} bytes)');
      }
      // ── Stage (upload) to the backend (second half of the progress bar) ──
      final ok = await svc.uploadFirmware(
        bytes,
        onProgress: (p) {
          if (mounted) setState(() => _uploadProgress = 0.5 + p * 0.5);
        },
      );
      if (!mounted) return;
      if (ok) {
        setState(() => _uploadProgress = null);
        await _loadOtaStatus();
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text('Staged controller firmware v${asset.version} from GitHub')),
          );
        }
      } else {
        setState(() { _uploadProgress = null; _uploadError = 'Staging failed — check connection.'; });
      }
    } catch (e) {
      client?.close();
      if (mounted) setState(() { _uploadProgress = null; _uploadError = 'GitHub fetch failed: $e'; });
    }
  }

  /// Firmware staging buttons: primary "Get latest from GitHub", secondary manual pick.
  Widget _stageFirmwareButtons() {
    final busy = _otaBusy || _uploadProgress != null;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(children: [
          _ActionButton(
            label: 'Get latest from GitHub',
            icon: Icons.cloud_download,
            enabled: !busy,
            onTap: _stageFromGitHub,
          ),
        ]),
        Align(
          alignment: Alignment.center,
          child: TextButton.icon(
            onPressed: busy ? null : _pickAndUploadFirmware,
            icon: const Icon(Icons.upload_file, size: 15),
            label: const Text('or choose a .bin file', style: TextStyle(fontSize: 12)),
          ),
        ),
      ],
    );
  }

  void _startOtaPolling() {
    _otaCountdownTimer?.cancel();
    _otaPollTimer?.cancel();
    _otaSecondsElapsed = 0;
    // Countdown tick every second
    _otaCountdownTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      setState(() => _otaSecondsElapsed++);
      if (_otaSecondsElapsed >= 150) {
        _otaCountdownTimer?.cancel();
      }
    });
    // Status poll every 5 seconds
    _otaPollTimer = Timer.periodic(const Duration(seconds: 5), (_) async {
      if (!mounted) return;
      final svc = context.read<TankService>();
      final data = await svc.fetchOtaStatus();
      if (!mounted || data == null) return;
      final phase = (data['phase'] as String?) ?? '';
      setState(() {
        _otaPhase = phase;
        _otaHasFirmware = data['has_firmware'] == true;
      });
      if (phase == 'success' || phase == 'failed' || phase == 'idle' || phase.isEmpty) {
        _otaCountdownTimer?.cancel();
        _otaPollTimer?.cancel();
        setState(() { _otaBusy = false; });
      }
    });
  }

  void _confirmFlash(BuildContext ctx, TankService svc) {
    showDialog(context: ctx, builder: (_) => AlertDialog(
      backgroundColor: cardBg(ctx),
      title: Text('Flash firmware to ESP32?', style: TextStyle(color: textColor(ctx))),
      content: Text(
        'The ESP32 will download and install the staged firmware, then reboot.',
        style: TextStyle(color: labelColor(ctx), fontSize: 13)),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(
          onPressed: () async {
            AppHaptics.confirm();
            Navigator.pop(ctx);
            setState(() { _otaBusy = true; _otaPhase = 'triggered'; });
            await svc.triggerOta();
            _startOtaPolling();
          },
          child: Text('Flash', style: TextStyle(color: accentBlue(ctx))),
        ),
      ],
    ));
  }

  Future<void> _downloadAndInstall() async {
    final svc = context.read<TankService>();
    final url = svc.latestApkUrl;
    if (url == null) return;

    // Guard: re-verify the version is actually newer before downloading
    final latest = svc.latestAppVersion;
    if (latest == null || !svc.updateAvailable) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('No update available')),
        );
      }
      return;
    }

    setState(() => _downloadProgress = 0);
    File? file;
    try {
      final client = http.Client();
      final request = http.Request('GET', Uri.parse(url));
      final response = await client.send(request);

      // Guard: HTTP must return 200
      if (response.statusCode != 200) {
        client.close();
        throw Exception('Server returned ${response.statusCode}');
      }

      final total = response.contentLength ?? 0;
      final dir = await getTemporaryDirectory();
      file = File('${dir.path}/TankMonitor-update.apk');
      final sink = file.openWrite();
      int received = 0;
      await for (final chunk in response.stream) {
        sink.add(chunk);
        received += chunk.length;
        if (mounted) setState(() => _downloadProgress = total > 0 ? received / total : null);
      }
      await sink.flush();
      await sink.close();
      client.close();

      // Guard: file must exist and be > 1 MB (a valid APK is at least several MB)
      if (!file.existsSync()) {
        throw Exception('Downloaded file not found');
      }
      final fileSize = file.lengthSync();
      if (fileSize < 1024 * 1024) {
        throw Exception('Downloaded file too small (${fileSize} bytes) — likely corrupt');
      }

      // Guard: verify APK (ZIP) magic bytes — PK\x03\x04
      final header = file.openSync()..setPositionSync(0);
      final magic = header.readSync(4);
      header.closeSync();
      if (magic.length < 4 || magic[0] != 0x50 || magic[1] != 0x4B ||
          magic[2] != 0x03 || magic[3] != 0x04) {
        throw Exception('Downloaded file is not a valid APK');
      }

      if (mounted) setState(() => _downloadProgress = null);
      await OpenFilex.open(file.path);
    } catch (e) {
      // Cleanup corrupt download
      try { file?.deleteSync(); } catch (_) {}
      if (mounted) {
        setState(() => _downloadProgress = null);
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Update failed: $e')),
        );
      }
    }
  }

  void _confirmRollback(BuildContext ctx, TankService svc) {
    showDialog(context: ctx, builder: (_) => AlertDialog(
      backgroundColor: cardBg(ctx),
      title: Text('Rollback firmware?', style: TextStyle(color: textColor(ctx))),
      content: Text(
        'ESP32 will reboot into the previous OTA partition.',
        style: TextStyle(color: labelColor(ctx), fontSize: 13)),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(
          onPressed: () async {
            AppHaptics.confirm();
            Navigator.pop(ctx);
            setState(() => _otaBusy = true);
            await svc.triggerRollback();
            if (mounted) setState(() => _otaBusy = false);
          },
          child: Text('Rollback', style: TextStyle(color: accentRed(ctx))),
        ),
      ],
    ));
  }

  void _logout() async {
    final svc = context.read<TankService>();
    await svc.logout();
    if (mounted) {
      Navigator.of(context).pushAndRemoveUntil(
        MaterialPageRoute(builder: (_) => const LoginScreen()),
        (_) => false,
      );
    }
  }

  void _switchToCloud() async {
    final svc = context.read<TankService>();
    svc.disconnect();
    await svc.saveDirectMode(false, '');
    if (mounted) {
      Navigator.of(context).pushAndRemoveUntil(
        MaterialPageRoute(builder: (_) => const LoginScreen()),
        (_) => false,
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final svc = context.watch<TankService>();
    final s   = svc.status;

    // All schedules shown; next-upcoming index per motor for highlight
    final enabledScheds = s?.schedules.toList() ?? [];
    final nextOHIdx = _nextScheduleIdx(
        enabledScheds.where((sc) => sc.m == 'OH').toList(), s?.time ?? '');
    final nextUGIdx = _nextScheduleIdx(
        enabledScheds.where((sc) => sc.m == 'UG').toList(), s?.time ?? '');
    // Navigate to login if token was invalidated (skip in direct mode — no auth needed)
    if (svc.unauthorized && !svc.directMode) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        Navigator.of(context).pushAndRemoveUntil(
          MaterialPageRoute(builder: (_) => const LoginScreen()),
          (_) => false,
        );
      });
    }

    // Backward compat: if firmware hasn't sent per-motor buzzer fields yet
    // (both oh_buzzer/ug_buzzer absent → false), fall back to the shared
    // buzzer_active flag so both cards blink (same as v1.4.3 behaviour).
    // Once firmware is updated the per-motor fields take over.
    final bool noPerMotorBuzzer = !(s?.ohBuzzer ?? false) && !(s?.ugBuzzer ?? false);
    final bool buzzerFallback   = noPerMotorBuzzer && (s?.buzzerActive ?? false);

    return Scaffold(
      backgroundColor: scaffoldBg(context),
      appBar: AppBar(
        backgroundColor: cardBg(context),
        elevation: 0,
        title: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              '💧 ${svc.currentDevice?.displayName ?? 'Tank Monitor'}',
              style: TextStyle(
                  color: accentBlue(context), fontWeight: FontWeight.w700, fontSize: 17),
              overflow: TextOverflow.ellipsis,
            ),
            if ((svc.currentDevice?.typeId ?? '').isNotEmpty)
              Text(
                svc.currentDevice!.typeId,
                style: TextStyle(color: labelColor(context), fontSize: 11),
                overflow: TextOverflow.ellipsis,
              ),
          ],
        ),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 4),
            child: Row(children: [
              Icon(Icons.circle, size: 8,
                color: svc.connected ? accentGreen(context) : accentOrange(context)),
              const SizedBox(width: 4),
              Text(
                svc.connected ? 'Live' : 'Connecting…',
                style: TextStyle(color: labelColor(context), fontSize: 12)),
            ]),
          ),
          if (!svc.directMode)
            IconButton(
              icon: Icon(Icons.devices, color: labelColor(context), size: 20),
              tooltip: 'Switch device',
              onPressed: () => Navigator.of(context).pushReplacement(
                MaterialPageRoute(builder: (_) => const DeviceListScreen(autoNavigate: false)),
              ),
            ),
          IconButton(
            icon: _syncing
                ? SizedBox(
                    width: 18, height: 18,
                    child: CircularProgressIndicator(strokeWidth: 2, color: labelColor(context)),
                  )
                : Icon(Icons.sync, color: labelColor(context), size: 20),
            tooltip: 'Sync now',
            onPressed: _syncing ? null : _handleSyncNow,
          ),
          IconButton(
            icon: Icon(Icons.settings_ethernet, color: labelColor(context), size: 20),
            tooltip: 'Change server',
            onPressed: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const SetupScreen()),
            ),
          ),
          IconButton(
            icon: Icon(svc.directMode ? Icons.cloud : Icons.logout,
                color: labelColor(context), size: 20),
            tooltip: svc.directMode ? 'Switch to Cloud' : 'Sign out',
            onPressed: svc.directMode ? _switchToCloud : _logout,
          ),
        ],
      ),
      body: IndexedStack(
              index: _tabIndex,
              children: [
                // ── Tab 0: Dashboard ──────────────────────────────────────
                ListView(
                  key: const PageStorageKey('dashboard'),
                  padding: const EdgeInsets.all(12),
                  children: [
                    if (svc.updateAvailable)
                      _UpdateBanner(
                        latestVersion: svc.latestAppVersion ?? '',
                        downloading: _downloadProgress != null,
                        progress: _downloadProgress,
                        onUpdate: _downloadAndInstall,
                      ),
                    if (svc.error != null)
                      _Banner(svc.error!, isError: true),
                    if (!svc.connected)
                      const _Banner('Connecting to device…', isError: false),
                    if (s?.txLost == true)
                      const _Banner('⚠ Transmitter lost — no signal received', isError: true),
                    // ── Tank + Motor cards (switchable concept) ──
                    Builder(builder: (_) {
                      final prefs = context.watch<AppPreferences>();
                      final data = DashboardData(
                        status: s,
                        connected: svc.connected,
                        txLost: s?.txLost ?? false,
                        ugMotorName: prefs.ugMotorName,
                        ohMotorName: prefs.ohMotorName,
                        ugBuzzer: (s?.ugBuzzer ?? false) || buzzerFallback,
                        ohBuzzer: (s?.ohBuzzer ?? false) || buzzerFallback,
                        onUgOn:  () { AppHaptics.motor(); svc.sendControl({'cmd': 'ug_on'}); },
                        onUgOff: () { AppHaptics.motor(); svc.sendControl({'cmd': 'ug_off'}); },
                        onOhOn:  () { AppHaptics.motor(); svc.sendControl({'cmd': 'oh_on'}); },
                        onOhOff: () { AppHaptics.motor(); svc.sendControl({'cmd': 'oh_off'}); },
                        ugCmdSending: svc.ugCmdSending,
                        ohCmdSending: svc.ohCmdSending,
                        ugCmdFailed: svc.ugCmdFailed,
                        ohCmdFailed: svc.ohCmdFailed,
                        ugCmdRejection: svc.ugCmdRejection,
                        ohCmdRejection: svc.ohCmdRejection,
                        ugCd: s?.ugCd ?? 0,
                        ohCd: s?.ohCd ?? 0,
                        onUgClearFailed: () => svc.clearCmdFailed(false),
                        onOhClearFailed: () => svc.clearCmdFailed(true),
                        loraOk: s?.loraOk ?? true,
                        loraRssi: s?.loraRssi ?? 0.0,
                        loraSNR: s?.loraSNR ?? 0.0,
                        lastLoraReceived: s?.lastLoraReceived ?? '',
                      );
                      switch (prefs.concept) {
                        case DashboardConcept.hybrid:    return ConceptFDashboard(d: data);
                        case DashboardConcept.nova:      return ConceptNovaDashboard(d: data);
                        case DashboardConcept.clean:     return ConceptCleanDashboard(d: data);
                        case DashboardConcept.console:   return ConceptConsoleDashboard(d: data);
                      }
                    }),
                    const SizedBox(height: 12),
                    // ── Schedules (compact — Open modal for full management) ──
                    Builder(builder: (ctx) {
                      final allScheds = s?.schedules ?? [];
                      final ohScheds = allScheds.where((sc) => sc.m == 'OH').toList();
                      final ugScheds = allScheds.where((sc) => sc.m == 'UG').toList();
                      final Schedule? nextOHSched = ohScheds.isEmpty ? null
                          : (nextOHIdx != null ? ohScheds.firstWhere((sc) => sc.i == nextOHIdx, orElse: () => ohScheds.first) : ohScheds.first);
                      final Schedule? nextUGSched = ugScheds.isEmpty ? null
                          : (nextUGIdx != null ? ugScheds.firstWhere((sc) => sc.i == nextUGIdx, orElse: () => ugScheds.first) : ugScheds.first);
                      final previewEntries = [
                        if (nextOHSched != null) nextOHSched,
                        if (nextUGSched != null) nextUGSched,
                      ];
                      void openScheduler() => showModalBottomSheet(
                        context: ctx,
                        isScrollControlled: true,
                        backgroundColor: cardBg(ctx),
                        shape: const RoundedRectangleBorder(
                            borderRadius: BorderRadius.vertical(top: Radius.circular(16))),
                        builder: (_) => _SchedulerModal(
                          svc: svc, s: s,
                          nextOHIdx: nextOHIdx, nextUGIdx: nextUGIdx,
                        ),
                      );
                      return _SectionCard(
                        title: 'Scheduler',
                        subtitle: s?.time != null ? 'Time: ${_to12hr(s!.time)}' : null,
                        titleMixed: true,
                        trailing: _SmallButton(label: 'Expand', onTap: openScheduler),
                        child: Column(
                          children: [
                            if (s == null || allScheds.isEmpty)
                              Padding(
                                padding: const EdgeInsets.symmetric(vertical: 8),
                                child: Center(child: Text(
                                  'No schedules — tap Open to add',
                                  style: TextStyle(color: labelColor(context), fontSize: 12),
                                )),
                              )
                            else ...
                              previewEntries.map((sch) => _SchedulePreviewRow(
                                sch: sch,
                                isNext: sch.i == nextOHIdx || sch.i == nextUGIdx,
                              )),
                            if (allScheds.length > previewEntries.length)
                              Padding(
                                padding: const EdgeInsets.only(top: 4),
                                child: Text(
                                  '+${allScheds.length - previewEntries.length} more — tap Open',
                                  style: TextStyle(color: accentBlue(context), fontSize: 11),
                                ),
                              ),
                          ],
                        ),
                      );
                    }),
                    const SizedBox(height: 12),
                    // ── History (moved here from Settings — Table or Trend Graph) ──
                    _SectionCard(
                      title: 'HISTORY',
                      child: Padding(
                        padding: const EdgeInsets.symmetric(vertical: 4),
                        child: Column(children: [
                          Padding(
                            padding: const EdgeInsets.only(bottom: 8),
                            child: Align(
                              alignment: Alignment.centerLeft,
                              child: Text('Motor & tank event history',
                                  style: TextStyle(color: labelColor(context), fontSize: 12)),
                            ),
                          ),
                          Row(children: [
                            _ActionButton(
                              label: 'Table',
                              icon: Icons.table_rows,
                              enabled: true,
                              onTap: () => Navigator.of(context).push(
                                MaterialPageRoute(
                                    builder: (_) => const EventHistoryScreen(initialGraph: false)),
                              ),
                            ),
                            const SizedBox(width: 10),
                            _ActionButton(
                              label: 'Trend Graph',
                              icon: Icons.show_chart,
                              enabled: true,
                              onTap: () => Navigator.of(context).push(
                                MaterialPageRoute(
                                    builder: (_) => const EventHistoryScreen(initialGraph: true)),
                              ),
                            ),
                          ]),
                        ]),
                      ),
                    ),
                    const SizedBox(height: 20),
                  ],
                ),

                // ── Tab 1: Settings ───────────────────────────────────────
                ListView(
                  key: const PageStorageKey('settings'),
                  padding: const EdgeInsets.all(12),
                  children: [
                    // ── Dashboard Theme ──
                    _DashboardThemePicker(),
                    const SizedBox(height: 10),
                    // ── App Theme ──
                    _AppThemePicker(),
                    const SizedBox(height: 10),
                    // ── Motor Names ──
                    _MotorNameEditor(),
                    const SizedBox(height: 10),
                    // ── Notifications ──
                    _NotificationSettings(),
                    const SizedBox(height: 10),
                    // ── Water Volume Display (History screen banners) ──
                    _WaterVolumeSettings(),
                    const SizedBox(height: 10),
                    // ── Device Settings ──
                    _SectionCard(
                      title: 'DEVICE SETTINGS',
                      child: Column(children: [
                        _SettingRow('OH Display Only',         s?.ohDispOnly,  (v) => svc.sendSettingControl('oh_disp_only', v)),
                        _SettingRow('UG Display Only',         s?.ugDispOnly,  (v) => svc.sendSettingControl('ug_disp_only', v)),
                        _SettingRow('Ignore UG for OH Motor',  s?.ugIgnore,    (v) => svc.sendSettingControl('ug_ignore',    v)),
                        _SettingRow('Buzzer Delay Before Start',s?.buzzerDelay,(v) => svc.sendSettingControl('buzzer_delay', v)),
                        _SettingRow('Stop Manual Motor on Full',s?.manualAutoStop,(v) => svc.sendSettingControl('manual_auto_stop', v)),
                        Divider(color: cardBd(context), height: 16),
                        // Motor Start Level
                        Padding(
                          padding: const EdgeInsets.symmetric(vertical: 6),
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Text('Start Level', style: TextStyle(color: textColor(context), fontSize: 13)),
                              _SegSelector<int>(
                                value: s?.ohStartLevel ?? 1,
                                options: const {'EMPTY': 1, 'LOW': 2, 'HALF': 3},
                                onChanged: s != null
                                  ? (v) => svc.sendControl({'cmd': 'set_setting', 'key': 'oh_start_level', 'value': v})
                                  : null,
                              ),
                            ],
                          ),
                        ),
                        // Motor Stop Level
                        Padding(
                          padding: const EdgeInsets.symmetric(vertical: 6),
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Text('Stop Level', style: TextStyle(color: textColor(context), fontSize: 13)),
                              _SegSelector<int>(
                                value: s?.ohStopLevel ?? 4,
                                options: const {'LOW': 2, 'HALF': 3, 'FULL': 4},
                                onChanged: s != null
                                  ? (v) => svc.sendControl({'cmd': 'set_setting', 'key': 'oh_stop_level', 'value': v})
                                  : null,
                              ),
                            ],
                          ),
                        ),
                        // Max Motor Runtime
                        Padding(
                          padding: const EdgeInsets.symmetric(vertical: 6),
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Text('Max Runtime (min)', style: TextStyle(color: textColor(context), fontSize: 13)),
                              SizedBox(
                                width: 70,
                                child: TextField(
                                  controller: TextEditingController(text: '${s?.ohMaxRunMin ?? 20}'),
                                  keyboardType: TextInputType.number,
                                  textAlign: TextAlign.center,
                                  style: TextStyle(color: textColor(context), fontSize: 13),
                                  decoration: InputDecoration(
                                    isDense: true,
                                    contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
                                    enabledBorder: OutlineInputBorder(borderSide: BorderSide(color: cardBd(context))),
                                    focusedBorder: const OutlineInputBorder(borderSide: BorderSide(color: Colors.blue)),
                                  ),
                                  inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                                  onSubmitted: (v) {
                                    final val = int.tryParse(v);
                                    if (val != null && val >= 5 && val <= 60) {
                                      svc.sendControl({'cmd': 'set_setting', 'key': 'oh_max_run_min', 'value': val});
                                    }
                                  },
                                ),
                              ),
                            ],
                          ),
                        ),
                        // MQTT Watchdog reboot timeout
                        Padding(
                          padding: const EdgeInsets.symmetric(vertical: 6),
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Expanded(
                                child: Text('Reboot if unreachable (min)',
                                    style: TextStyle(color: textColor(context), fontSize: 13)),
                              ),
                              SizedBox(
                                width: 70,
                                child: TextField(
                                  controller: TextEditingController(text: '${s?.mqttWatchdogMin ?? 15}'),
                                  keyboardType: TextInputType.number,
                                  textAlign: TextAlign.center,
                                  style: TextStyle(color: textColor(context), fontSize: 13),
                                  decoration: InputDecoration(
                                    isDense: true,
                                    contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
                                    enabledBorder: OutlineInputBorder(borderSide: BorderSide(color: cardBd(context))),
                                    focusedBorder: OutlineInputBorder(borderSide: BorderSide(color: accentBlue(context))),
                                  ),
                                  inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                                  onSubmitted: (v) {
                                    final val = int.tryParse(v);
                                    if (val != null && val >= 10 && val <= 60) {
                                      svc.sendControl({'cmd': 'set_setting', 'key': 'mqtt_watchdog_min', 'value': val});
                                    }
                                  },
                                ),
                              ),
                            ],
                          ),
                        ),
                        Divider(color: cardBd(context), height: 16),
                        // LCD Backlight mode
                        Padding(
                          padding: const EdgeInsets.symmetric(vertical: 6),
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Text('LCD Backlight', style: TextStyle(color: textColor(context), fontSize: 13)),
                              _SegSelector<int>(
                                value: s?.lcdBlMode ?? 0,
                                options: const {'Auto': 0, 'On': 1, 'Off': 2},
                                onChanged: s != null ? (v) => svc.setLcdMode(v) : null,
                              ),
                            ],
                          ),
                        ),
                        // MQTT Password
                        Padding(
                          padding: const EdgeInsets.only(top: 6),
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Text('MQTT Password', style: TextStyle(color: textColor(context), fontSize: 13)),
                              TextButton(
                                onPressed: s != null ? () => _changeMqttPassword(context, svc) : null,
                                style: TextButton.styleFrom(
                                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                                  minimumSize: Size.zero,
                                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                                ),
                                child: const Text('Change', style: TextStyle(fontSize: 12)),
                              ),
                            ],
                          ),
                        ),
                      ]),
                    ),
                    const SizedBox(height: 10),
                    // ── Quick Actions (2×2 grid) ──
                    _SectionCard(
                      title: 'QUICK ACTIONS',
                      child: Padding(
                        padding: const EdgeInsets.symmetric(vertical: 4),
                        child: Column(children: [
                          Row(children: [
                            _ActionButton(
                              label: 'Sync NTP',
                              icon: Icons.sync,
                              enabled: s != null,
                              onTap: () => svc.sendControl({'cmd': 'sync_ntp'}),
                            ),
                            const SizedBox(width: 10),
                            _ActionButton(
                              label: 'WiFi',
                              icon: Icons.wifi,
                              enabled: true,
                              onTap: () => Navigator.of(context).push(
                                MaterialPageRoute(builder: (_) => const WifiManagementScreen()),
                              ),
                            ),
                          ]),
                          const SizedBox(height: 10),
                          Row(children: [
                            _ActionButton(
                              label: 'Reboot',
                              icon: Icons.power_settings_new,
                              accent: accentOrange(context),
                              enabled: s != null,
                              onTap: () => _confirmReboot(context, svc),
                            ),
                            const SizedBox(width: 10),
                            _ActionButton(
                              label: 'Factory Reset',
                              icon: Icons.restore,
                              danger: true,
                              enabled: svc.directService != null,
                              onTap: () => _confirmFactoryReset(context, svc),
                            ),
                          ]),
                          if (!svc.directMode && svc.directService != null)
                            Padding(
                              padding: const EdgeInsets.only(top: 8),
                              child: Text('Using device IP: ${s?.mgmtIp ?? "—"}',
                                  style: TextStyle(color: labelColor(context), fontSize: 10)),
                            ),
                          if (svc.directService == null)
                            Padding(
                              padding: const EdgeInsets.only(top: 8),
                              child: Text('Waiting for device IP…',
                                  style: TextStyle(color: labelColor(context), fontSize: 10)),
                            ),
                        ]),
                      ),
                    ),
                    const SizedBox(height: 10),
                    // ── Firmware OTA ──
                    _SectionCard(
                      title: 'FIRMWARE UPDATE (OTA)',
                      trailing: IconButton(
                        icon: Icon(Icons.refresh, size: 17, color: labelColor(context)),
                        tooltip: 'Refresh OTA status',
                        onPressed: _otaBusy ? null : _loadOtaStatus,
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          // Current firmware version
                          Text('Current: ${s?.fw ?? '—'}',
                            style: TextStyle(color: labelColor(context), fontSize: 12)),
                          const SizedBox(height: 10),

                          // ── Step 1: Staged firmware status ──
                          Container(
                            padding: const EdgeInsets.all(10),
                            decoration: BoxDecoration(
                              color: subtleBg(context),
                              borderRadius: BorderRadius.circular(6),
                            ),
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text('STEP 1 — STAGE FIRMWARE',
                                  style: TextStyle(color: labelColor(context), fontSize: 10, letterSpacing: 0.8)),
                                const SizedBox(height: 6),
                                if (_uploadProgress != null) ...[
                                  // Uploading…
                                  Row(children: [
                                    SizedBox(width: 12, height: 12,
                                      child: CircularProgressIndicator(strokeWidth: 2, color: accentBlue(context))),
                                    const SizedBox(width: 8),
                                    Text('Uploading… ${(_uploadProgress! * 100).toStringAsFixed(0)}%',
                                      style: TextStyle(color: textColor(context).withOpacity(0.7), fontSize: 12)),
                                  ]),
                                  const SizedBox(height: 6),
                                  ClipRRect(
                                    borderRadius: BorderRadius.circular(3),
                                    child: LinearProgressIndicator(
                                      value: _uploadProgress,
                                      backgroundColor: cardBd(context),
                                      color: accentBlue(context),
                                      minHeight: 4,
                                    ),
                                  ),
                                ] else if (_uploadError != null) ...[
                                  Text('❌ $_uploadError',
                                    style: TextStyle(color: accentRed(context), fontSize: 12)),
                                  const SizedBox(height: 6),
                                  _stageFirmwareButtons(),
                                ] else if (_otaHasFirmware) ...[
                                  Row(children: [
                                    Icon(Icons.check_circle, color: accentGreen(context), size: 14),
                                    const SizedBox(width: 6),
                                    Expanded(child: Text(
                                      '${_otaStagedName.isEmpty ? 'firmware.bin' : _otaStagedName}'
                                      '  —  ${(_otaStagedSize / 1024).toStringAsFixed(0)} KB',
                                      style: TextStyle(color: textColor(context), fontSize: 12),
                                    )),
                                  ]),
                                  if (_otaStagedAt.isNotEmpty) ...[
                                    const SizedBox(height: 2),
                                    Text(
                                      'Uploaded ${DateTime.tryParse(_otaStagedAt)?.toLocal().toString().substring(0, 16) ?? _otaStagedAt}',
                                      style: TextStyle(color: labelColor(context), fontSize: 11),
                                    ),
                                  ],
                                  const SizedBox(height: 6),
                                  _stageFirmwareButtons(),
                                ] else ...[
                                  Text('No firmware staged.',
                                    style: TextStyle(color: labelColor(context), fontSize: 12)),
                                  const SizedBox(height: 6),
                                  _stageFirmwareButtons(),
                                ],
                              ],
                            ),
                          ),
                          const SizedBox(height: 10),

                          // ── Step 2: Flash ──
                          Container(
                            padding: const EdgeInsets.all(10),
                            decoration: BoxDecoration(
                              color: subtleBg(context),
                              borderRadius: BorderRadius.circular(6),
                            ),
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text('STEP 2 — FLASH TO DEVICE',
                                  style: TextStyle(color: labelColor(context), fontSize: 10, letterSpacing: 0.8)),
                                const SizedBox(height: 8),

                                // Phase message + countdown
                                if (_otaBusy || (_otaPhase.isNotEmpty && _otaPhase != 'idle')) ...[
                                  _otaPhase == 'success'
                                    ? Row(children: [
                                        Icon(Icons.check_circle, color: accentGreen(context), size: 14),
                                        const SizedBox(width: 6),
                                        Text('✅ Update successful! Device rebooted.',
                                          style: TextStyle(color: accentGreen(context), fontSize: 12)),
                                      ])
                                    : _otaPhase == 'failed'
                                      ? Row(children: [
                                          Icon(Icons.error_outline, color: accentRed(context), size: 14),
                                          const SizedBox(width: 6),
                                          Expanded(child: Text('❌ Update failed — timed out.',
                                            style: TextStyle(color: accentRed(context), fontSize: 12))),
                                        ])
                                      : Column(
                                          crossAxisAlignment: CrossAxisAlignment.start,
                                          children: [
                                            Row(children: [
                                              SizedBox(width: 12, height: 12,
                                                child: CircularProgressIndicator(strokeWidth: 2, color: accentBlue(context))),
                                              const SizedBox(width: 8),
                                              Text(
                                                _otaPhase == 'triggered'    ? '⚡ OTA triggered — waiting for ESP32…'
                                              : _otaPhase == 'ack_received' ? '✅ ESP32 confirmed — downloading…'
                                              : _otaPhase == 'downloading'  ? '⬇️ ESP32 is flashing firmware…'
                                              : 'In progress…',
                                                style: TextStyle(color: textColor(context).withOpacity(0.7), fontSize: 12),
                                              ),
                                            ]),
                                            const SizedBox(height: 6),
                                            Row(
                                              mainAxisAlignment: MainAxisAlignment.spaceBetween,
                                              children: [
                                                Text('${_otaSecondsElapsed}s / 150s',
                                                  style: TextStyle(color: labelColor(context), fontSize: 10)),
                                                Text('${150 - _otaSecondsElapsed > 0 ? 150 - _otaSecondsElapsed : 0}s remaining',
                                                  style: TextStyle(color: labelColor(context), fontSize: 10)),
                                              ],
                                            ),
                                            const SizedBox(height: 4),
                                            ClipRRect(
                                              borderRadius: BorderRadius.circular(3),
                                              child: LinearProgressIndicator(
                                                value: (_otaSecondsElapsed / 150).clamp(0.0, 1.0),
                                                backgroundColor: cardBd(context),
                                                color: accentBlue(context),
                                                minHeight: 6,
                                              ),
                                            ),
                                          ],
                                        ),
                                  const SizedBox(height: 8),
                                ],

                                Row(children: [
                                  _ActionButton(
                                    label: _otaBusy ? 'Flashing…' : 'Flash Firmware',
                                    icon: Icons.bolt,
                                    enabled: s != null && _otaHasFirmware && !_otaBusy
                                        && _otaPhase != 'triggered'
                                        && _otaPhase != 'ack_received'
                                        && _otaPhase != 'downloading',
                                    onTap: () => _confirmFlash(context, svc),
                                  ),
                                  const SizedBox(width: 10),
                                  _ActionButton(
                                    label: 'Rollback',
                                    icon: Icons.history,
                                    danger: true,
                                    enabled: s != null && !_otaBusy,
                                    onTap: () => _confirmRollback(context, svc),
                                  ),
                                ]),
                              ],
                            ),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 20),
                  ],
                ),

                // ── Tab 2: System ─────────────────────────────────────────
                ListView(
                  key: const PageStorageKey('system'),
                  padding: const EdgeInsets.all(12),
                  children: [
                    // ── System info ──
                    _SectionCard(
                      title: 'SYSTEM',
                      child: Column(children: [
                        _InfoRow('WiFi',        s != null ? '${s.wifiRssi} dBm' : '—'),
                        _InfoRow('LoRa',        null, loraOk: s?.loraOk),
                        _InfoRow('Transmitter', null, loraOk: s != null ? !s.txLost : null),
                        _InfoRow('Uptime',      s != null ? _formatUptime(s.uptimeS) : '—'),
                        _InfoRow('Controller',  s?.fw ?? '—'),
                        _InfoRow('Transmitter', s?.txFw.isNotEmpty == true ? s!.txFw : '—'),
                        _InfoRow('Web App',     svc.webAppVersion ?? '—'),
                        _InfoRow('Mobile App',  mobileAppVersion),
                        _LinkInfoRow('Project Repository', 'https://github.com/nperiannan/AgriPulse/releases'),
                        const Divider(height: 16),
                        _InfoRow('Device MAC',  svc.currentDevice?.mac ?? '—'),
                        _InfoRow('Device IP',   s?.mgmtIp.isNotEmpty == true ? s!.mgmtIp : '—'),
                        const Divider(height: 16),
                        _InfoRow('User',        svc.currentUsername ?? '—'),
                        _InfoRow('Access Level', _accessLevel(svc)),
                        const Divider(height: 16),
                        // Static — every unit shipped so far is the v2.0 PCB
                        // (Config.h forces BOARD_V2 unconditionally).
                        _InfoRow('Hardware Revision', 'v2.0 PCB', last: true),
                      ]),
                    ),
                    const SizedBox(height: 20),
                  ],
                ),

                // ── Tab 3: Logs ───────────────────────────────────────────
                ListView(
                  key: const PageStorageKey('logs'),
                  padding: const EdgeInsets.all(12),
                  children: [
                    if (_deviceLogs.isNotEmpty) ...[                    
                      _buildLogSummaryCard(_deviceLogs),
                      const SizedBox(height: 10),
                    ],
                    _SectionCard(
                      title: 'DEVICE LOGS',
                      trailing: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          if (_deviceLogs.isNotEmpty)
                            IconButton(
                              icon: Icon(Icons.copy_outlined, size: 17, color: labelColor(context)),
                              tooltip: 'Copy logs',
                              onPressed: () {
                                final text = _deviceLogs.reversed.join('\n');
                                _copyToClipboard(context, text);
                              },
                            ),
                          IconButton(
                            icon: _logsLoading
                                ? SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2, color: labelColor(context)))
                                : Icon(Icons.refresh, size: 18, color: labelColor(context)),
                            onPressed: s == null || _logsLoading ? null : () async {
                              svc.sendControl({'cmd': 'get_logs'});
                              await Future.delayed(const Duration(seconds: 2));
                              if (!mounted) return;
                              setState(() => _logsLoading = true);
                              final data = await svc.fetchLogs();
                              if (!mounted) return;
                              setState(() {
                                _logsLoading = false;
                                if (data != null) {
                                  _deviceLogs = (data['logs'] as List<dynamic>? ?? []).cast<String>();
                                  _logsAt = data['received_at'] as String?;
                                }
                              });
                            },
                          ),
                        ],
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          // Log level selector
                          Padding(
                            padding: const EdgeInsets.only(bottom: 8),
                            child: Row(
                              mainAxisAlignment: MainAxisAlignment.spaceBetween,
                              children: [
                                Text('Log Level',
                                  style: TextStyle(color: textColor(context), fontSize: 13)),
                                DropdownButton<String>(
                                  value: s?.logLevel ?? 'info',
                                  isDense: true,
                                  dropdownColor: cardBg(context),
                                  style: TextStyle(color: textColor(context), fontSize: 13),
                                  underline: const SizedBox(),
                                  items: const [
                                    DropdownMenuItem(
                                      value: 'info',
                                      child: Text('Info'),
                                    ),
                                    DropdownMenuItem(
                                      value: 'debug',
                                      child: Text('Debug'),
                                    ),
                                  ],
                                  onChanged: s != null
                                    ? (v) { if (v != null) svc.setLogLevel(v); }
                                    : null,
                                ),
                              ],
                            ),
                          ),
                          if (_logsAt != null)
                            Padding(
                              padding: const EdgeInsets.only(bottom: 4),
                              child: Text(
                                'Last received: ${DateTime.tryParse(_logsAt!)?.toLocal().toString().substring(0, 19) ?? _logsAt}',
                                style: TextStyle(color: labelColor(context), fontSize: 11),
                              ),
                            ),
                          Container(
                            constraints: const BoxConstraints(maxHeight: 200),
                            decoration: BoxDecoration(
                              color: subtleBg(context),
                              borderRadius: BorderRadius.circular(6),
                            ),
                            child: SingleChildScrollView(
                              padding: const EdgeInsets.all(8),
                              child: _deviceLogs.isEmpty
                                ? Text('No logs — tap Refresh to load.', style: TextStyle(color: labelColor(context), fontSize: 11))
                                : Column(
                                    crossAxisAlignment: CrossAxisAlignment.start,
                                    children: _deviceLogs.reversed.map((line) => Text(
                                      line,
                                      style: TextStyle(
                                        fontFamily: 'monospace',
                                        fontSize: 10,
                                        color: line.contains('[WARN]') ? accentOrange(context)
                                             : line.contains('[ERROR]') ? accentRed(context)
                                             : labelColor(context),
                                      ),
                                    )).toList(),
                                  ),
                            ),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 20),
                  ],
                ),
              ],
            ),
      bottomNavigationBar: BottomNavigationBar(
        currentIndex: _tabIndex,
        onTap: (i) {
          setState(() => _tabIndex = i);
          // Refresh OTA status when switching to Settings tab
          if (i == 1 && !_otaBusy) _loadOtaStatus();
          // Auto-poll logs when switching to Logs tab
          if (i == 3 && _deviceLogs.isEmpty && !_logsLoading) {
            final svc2 = context.read<TankService>();
            if (svc2.status != null) {
              svc2.sendControl({'cmd': 'get_logs'});
              Future.delayed(const Duration(seconds: 2), () async {
                if (!mounted) return;
                setState(() => _logsLoading = true);
                final data = await svc2.fetchLogs();
                if (!mounted) return;
                setState(() {
                  _logsLoading = false;
                  if (data != null) {
                    _deviceLogs = (data['logs'] as List<dynamic>? ?? []).cast<String>();
                    _logsAt = data['received_at'] as String?;
                  }
                });
              });
            }
          }
        },
        backgroundColor: cardBg(context),
        selectedItemColor: accentBlue(context),
        unselectedItemColor: labelColor(context),
        items: const [
          BottomNavigationBarItem(
            icon: Icon(Icons.dashboard_outlined),
            activeIcon: Icon(Icons.dashboard),
            label: 'Dashboard',
          ),
          BottomNavigationBarItem(
            icon: Icon(Icons.settings_outlined),
            activeIcon: Icon(Icons.settings),
            label: 'Settings',
          ),
          BottomNavigationBarItem(
            icon: Icon(Icons.monitor_heart_outlined),
            activeIcon: Icon(Icons.monitor_heart),
            label: 'System',
          ),
          BottomNavigationBarItem(
            icon: Icon(Icons.terminal_outlined),
            activeIcon: Icon(Icons.terminal),
            label: 'Logs',
          ),
        ],
      ),
    );
  }

  // ── Log summary helpers ──────────────────────────────────────────────────

  Widget _buildLogSummaryCard(List<String> logs) {
    final errors = logs.where((l) => l.contains('[ERROR]')).toList();
    final warns  = logs.where((l) => l.contains('[WARN]')).toList();
    final hasIssues = errors.isNotEmpty || warns.isNotEmpty;

    final parts = <String>[];
    if (errors.isNotEmpty) parts.add('${errors.length} error${errors.length > 1 ? 's' : ''}');
    if (warns.isNotEmpty)  parts.add('${warns.length} warning${warns.length > 1 ? 's' : ''}');
    if (!hasIssues)        parts.add('No errors or warnings');

    final items = <Widget>[
      _SummaryLine(
        icon: errors.isNotEmpty
            ? Icons.error_outline
            : warns.isNotEmpty
                ? Icons.warning_amber_outlined
                : Icons.check_circle_outline,
        color: errors.isNotEmpty ? accentRed(context)
             : warns.isNotEmpty  ? accentOrange(context)
             : accentGreen(context),
        text: '${parts.join(' · ')}  (${logs.length} entries)',
        bold: true,
      ),
    ];

    // Last 2 errors
    for (final e in errors.reversed.take(2)) {
      items.add(_SummaryLine(
        icon: Icons.cancel_outlined,
        color: accentRed(context),
        text: _trimLogLine(e),
      ));
    }

    // Fill remaining slots with latest warnings (max 2)
    final warnSlots = (4 - errors.take(2).length).clamp(0, 2);
    for (final w in warns.reversed.take(warnSlots)) {
      items.add(_SummaryLine(
        icon: Icons.warning_amber_outlined,
        color: accentOrange(context),
        text: _trimLogLine(w),
      ));
    }

    return _SectionCard(
      title: 'LOG SUMMARY',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: items,
      ),
    );
  }

  String _trimLogLine(String line) {
    final match = RegExp(r'\[(?:ERROR|WARN)\]\s*(.+)').firstMatch(line);
    final content = match != null ? match.group(1)!.trim() : line.trim();
    return content.length > 72 ? '${content.substring(0, 72)}…' : content;
  }

  void _confirmClear(BuildContext ctx, TankService svc) {
    showDialog(context: ctx, builder: (_) => AlertDialog(
      backgroundColor: cardBg(ctx),
      title: Text('Clear all schedules?', style: TextStyle(color: textColor(ctx))),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(
          onPressed: () { Navigator.pop(ctx); svc.sendControl({'cmd': 'sched_clear'}); },
          child: Text('Clear', style: TextStyle(color: accentRed(ctx))),
        ),
      ],
    ));
  }

  void _confirmReboot(BuildContext ctx, TankService svc) {
    showDialog(context: ctx, builder: (_) => AlertDialog(
      backgroundColor: cardBg(ctx),
      title: Text('Reboot ESP32?', style: TextStyle(color: textColor(ctx))),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(
          onPressed: () { AppHaptics.confirm(); Navigator.pop(ctx); svc.sendControl({'cmd': 'reboot'}); },
          child: Text('Reboot', style: TextStyle(color: accentRed(ctx))),
        ),
      ],
    ));
  }

  void _confirmFactoryReset(BuildContext ctx, TankService svc) {
    showDialog(context: ctx, builder: (_) => AlertDialog(
      backgroundColor: cardBg(ctx),
      title: Text('Factory Reset?', style: TextStyle(color: textColor(ctx))),
      content: Text('This will erase all settings (WiFi, schedules, motor config) and reboot the device.',
          style: TextStyle(color: labelColor(ctx), fontSize: 13)),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(
          onPressed: () async {
            AppHaptics.confirm();
            Navigator.pop(ctx);
            final ds = svc.directService;
            if (ds != null) await ds.factoryReset();
          },
          child: Text('Reset', style: TextStyle(color: accentRed(ctx))),
        ),
      ],
    ));
  }

  void _changeMqttPassword(BuildContext ctx, TankService svc) {
    final ctrl = TextEditingController();
    bool obscure = true;
    showDialog(
      context: ctx,
      builder: (dCtx) => StatefulBuilder(
        builder: (dCtx, setState) => AlertDialog(
          backgroundColor: cardBg(ctx),
          title: Text('Change MQTT Password', style: TextStyle(color: textColor(ctx))),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text(
                'Enter new password. The device will save it and reconnect.\nChange the broker password after.',
                style: TextStyle(color: Colors.grey, fontSize: 12),
              ),
              const SizedBox(height: 12),
              TextField(
                controller: ctrl,
                obscureText: obscure,
                autofocus: true,
                style: TextStyle(color: textColor(ctx)),
                decoration: InputDecoration(
                  labelText: 'New password',
                  labelStyle: TextStyle(color: labelColor(ctx)),
                  enabledBorder: OutlineInputBorder(borderSide: BorderSide(color: cardBd(ctx))),
                  focusedBorder: const OutlineInputBorder(borderSide: BorderSide(color: Colors.blue)),
                  suffixIcon: IconButton(
                    icon: Icon(obscure ? Icons.visibility_off : Icons.visibility, color: labelColor(ctx), size: 18),
                    onPressed: () => setState(() => obscure = !obscure),
                  ),
                ),
              ),
            ],
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(dCtx), child: const Text('Cancel')),
            TextButton(
              onPressed: () {
                final pass = ctrl.text.trim();
                if (pass.isEmpty) return;
                Navigator.pop(dCtx);
                svc.setMqttCreds(pass);
              },
              child: const Text('Update'),
            ),
          ],
        ),
      ),
    );
  }
} // end _DashboardScreenState

// ═══════════════════════════════════════════════════════════════════════════════
// Settings widgets: Dashboard Theme Picker, App Theme, Motor Names
// ═══════════════════════════════════════════════════════════════════════════════

class _DashboardThemePicker extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final prefs = context.watch<AppPreferences>();
    const items = [
      (DashboardConcept.hybrid,    '⚡', 'Hybrid',      'Arcs + grid motors'),
      (DashboardConcept.nova,      '✨', 'Nova',        'Big & simple, for everyone'),
      (DashboardConcept.clean,     '🧊', 'Clean',       'Minimal cards'),
      (DashboardConcept.console,   '📟', 'Console',     'Dense, no scroll'),
    ];

    return _SectionCard(
      title: 'DASHBOARD THEME',
      child: GridView.count(
        crossAxisCount: 2,
        shrinkWrap: true,
        physics: const NeverScrollableScrollPhysics(),
        mainAxisSpacing: 8, crossAxisSpacing: 8,
        childAspectRatio: 1.5,
        children: items.map((item) {
          final selected = prefs.concept == item.$1;
          return GestureDetector(
            onTap: () => prefs.setConcept(item.$1),
            child: Container(
              padding: const EdgeInsets.all(8),
              decoration: BoxDecoration(
                color: selected ? accentBlue(context).withValues(alpha: 0.12) : subtleBg(context),
                border: Border.all(color: selected ? accentBlue(context) : cardBd(context), width: selected ? 2 : 1),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Text(item.$2, style: const TextStyle(fontSize: 24)),
                  const SizedBox(height: 4),
                  Text(item.$3, style: TextStyle(
                    color: selected ? accentBlue(context) : textColor(context),
                    fontSize: 11, fontWeight: FontWeight.w600),
                    textAlign: TextAlign.center,
                  ),
                  const SizedBox(height: 2),
                  Text(item.$4, style: TextStyle(
                    color: labelColor(context), fontSize: 9),
                    textAlign: TextAlign.center,
                  ),
                  if (selected) ...[
                    const SizedBox(height: 4),
                    Text('★', style: TextStyle(color: accentBlue(context), fontSize: 10, fontWeight: FontWeight.w700)),
                  ],
                ],
              ),
            ),
          );
        }).toList(),
      ),
    );
  }
}

class _AppThemePicker extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final prefs = context.watch<AppPreferences>();
    const items = [
      (AppThemeMode.light,  '☀️', 'Light'),
      (AppThemeMode.dark,   '🌙', 'Dark'),
      (AppThemeMode.system, '💻', 'System'),
    ];

    return _SectionCard(
      title: 'APP THEME',
      child: Row(
        children: items.map((item) {
          final selected = prefs.themeMode == item.$1;
          return Expanded(
            child: GestureDetector(
              onTap: () => prefs.setThemeMode(item.$1),
              child: Container(
                margin: const EdgeInsets.symmetric(horizontal: 4),
                padding: const EdgeInsets.symmetric(vertical: 12),
                decoration: BoxDecoration(
                  color: selected ? accentBlue(context).withValues(alpha: 0.12) : subtleBg(context),
                  border: Border.all(color: selected ? accentBlue(context) : cardBd(context), width: selected ? 2 : 1),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Column(
                  children: [
                    Text(item.$2, style: const TextStyle(fontSize: 22)),
                    const SizedBox(height: 4),
                    Text(item.$3, style: TextStyle(
                      color: selected ? accentBlue(context) : textColor(context),
                      fontSize: 11, fontWeight: FontWeight.w600),
                    ),
                    if (selected) ...[
                      const SizedBox(height: 4),
                      Text('★ SELECTED', style: TextStyle(color: accentBlue(context), fontSize: 9, fontWeight: FontWeight.w700)),
                    ],
                  ],
                ),
              ),
            ),
          );
        }).toList(),
      ),
    );
  }
}

class _MotorNameEditor extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final prefs = context.watch<AppPreferences>();
    return _SectionCard(
      title: 'MOTOR NAMES',
      child: Column(children: [
        Text('Tap to rename. Applies to all dashboard themes.',
          style: TextStyle(color: labelColor(context), fontSize: 11)),
        const SizedBox(height: 10),
        _MotorNameRow(label: 'Underground', currentName: prefs.ugMotorName,
          onRename: (n) => prefs.setMotorName('UG', n)),
        const SizedBox(height: 6),
        _MotorNameRow(label: 'Overhead', currentName: prefs.ohMotorName,
          onRename: (n) => prefs.setMotorName('OH', n)),
      ]),
    );
  }
}

class _MotorNameRow extends StatelessWidget {
  final String label;
  final String currentName;
  final ValueChanged<String> onRename;
  const _MotorNameRow({required this.label, required this.currentName, required this.onRename});

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: () {
        final ctrl = TextEditingController(text: currentName);
        showDialog(
          context: context,
          builder: (ctx) => AlertDialog(
            backgroundColor: cardBg(context),
            title: Text('Rename $label Motor', style: TextStyle(color: textColor(context))),
            content: TextField(
              controller: ctrl,
              autofocus: true,
              style: TextStyle(color: textColor(context)),
              decoration: InputDecoration(
                hintText: 'Motor name',
                hintStyle: TextStyle(color: labelColor(context)),
                enabledBorder: OutlineInputBorder(borderSide: BorderSide(color: cardBd(context))),
                focusedBorder: OutlineInputBorder(borderSide: BorderSide(color: accentBlue(context))),
              ),
            ),
            actions: [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              TextButton(
                onPressed: () {
                  final name = ctrl.text.trim();
                  if (name.isNotEmpty) onRename(name);
                  Navigator.pop(ctx);
                },
                child: const Text('Save'),
              ),
            ],
          ),
        );
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
        decoration: BoxDecoration(
          color: subtleBg(context),
          borderRadius: BorderRadius.circular(10),
          border: Border.all(color: cardBd(context), style: BorderStyle.solid),
        ),
        child: Row(children: [
          Expanded(child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(label, style: TextStyle(color: labelColor(context), fontSize: 10)),
              Text(currentName, style: TextStyle(color: textColor(context), fontSize: 14, fontWeight: FontWeight.w600)),
            ],
          )),
          Icon(Icons.edit, color: labelColor(context), size: 16),
        ]),
      ),
    );
  }
}

class _NotificationSettings extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final prefs = context.watch<AppPreferences>();
    return _SectionCard(
      title: 'NOTIFICATIONS',
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Motor On/Off Alerts',
                  style: TextStyle(color: textColor(context), fontSize: 13, fontWeight: FontWeight.w600)),
                const SizedBox(height: 2),
                Text('Get notified when motors turn on or off',
                  style: TextStyle(color: labelColor(context), fontSize: 11)),
              ],
            ),
          ),
          Switch(
            value: prefs.motorNotify,
            activeColor: accentBlue(context),
            onChanged: (v) => prefs.setMotorNotify(v),
          ),
        ],
      ),
    );
  }
}

class _WaterVolumeSettings extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final prefs = context.watch<AppPreferences>();
    return _SectionCard(
      title: 'WATER VOLUME DISPLAY',
      child: Column(children: [
        _waterToggleRow(context, 'Show OH water pumped',
          'Display the estimated litres banner on the History screen',
          prefs.showOhWater, (v) => prefs.setShowWaterBanner('OH', v)),
        Divider(color: cardBd(context), height: 20),
        _waterToggleRow(context, 'Show UG water drawn',
          'Display the estimated litres banner on the History screen',
          prefs.showUgWater, (v) => prefs.setShowWaterBanner('UG', v)),
      ]),
    );
  }

  Widget _waterToggleRow(BuildContext context, String title, String subtitle, bool value, ValueChanged<bool> onChanged) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: TextStyle(color: textColor(context), fontSize: 13, fontWeight: FontWeight.w600)),
              const SizedBox(height: 2),
              Text(subtitle, style: TextStyle(color: labelColor(context), fontSize: 11)),
            ],
          ),
        ),
        Switch(
          value: value,
          activeColor: accentBlue(context),
          onChanged: onChanged,
        ),
      ],
    );
  }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// Converts "HH:MM" or "HH:MM:SS" (24hr) to "H:MM AM/PM"
String _to12hr(String t) {
  try {
    final parts = t.split(':');
    int h = int.parse(parts[0]);
    final m = parts[1].padLeft(2, '0');
    final period = h >= 12 ? 'PM' : 'AM';
    h = h % 12;
    if (h == 0) h = 12;
    return '$h:$m $period';
  } catch (_) {
    return t;
  }
}

/// Returns the index (i) of the next upcoming schedule in [scheds] relative to [currentTime] (HH:MM[:SS]).
int? _nextScheduleIdx(List<Schedule> scheds, String currentTime) {
  if (scheds.isEmpty || currentTime.isEmpty) return null;
  try {
    final parts = currentTime.split(':');
    final nowMins = int.parse(parts[0]) * 60 + int.parse(parts[1]);
    int? nextIdx;
    int minDiff = 999999;
    for (final sch in scheds) {
      final tp = sch.t.split(':');
      final schedMins = int.parse(tp[0]) * 60 + int.parse(tp[1]);
      int diff = schedMins - nowMins;
      if (diff <= 0) diff += 24 * 60;
      if (diff < minDiff) { minDiff = diff; nextIdx = sch.i; }
    }
    return nextIdx;
  } catch (_) {
    return null;
  }
}

String _formatUptime(int s) {
  if (s < 60)   return '${s}s';
  if (s < 3600) return '${s ~/ 60}m ${s % 60}s';
  return '${s ~/ 3600}h ${(s % 3600) ~/ 60}m';
}

/// Returns access level label based on service state.
String _accessLevel(TankService svc) {
  if (svc.isAdmin) return 'Full (Admin)';
  final role = svc.currentDevice?.role ?? '';
  if (role == 'owner') return 'Control';
  if (role == 'viewer') return 'Monitor';
  return '—';
}

void _copyToClipboard(BuildContext context, String text) {
  Clipboard.setData(ClipboardData(text: text));
  ScaffoldMessenger.of(context).showSnackBar(
    const SnackBar(
      content: Text('Logs copied to clipboard'),
      duration: Duration(seconds: 2),
    ),
  );
}

class _UpdateBanner extends StatelessWidget {
  final String latestVersion;
  final bool downloading;
  final double? progress;
  final VoidCallback onUpdate;

  const _UpdateBanner({
    required this.latestVersion,
    required this.downloading,
    required this.progress,
    required this.onUpdate,
  });

  @override
  Widget build(BuildContext context) {
    final color = accentGreen(context);
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.1),
        border: Border.all(color: color),
        borderRadius: BorderRadius.circular(8),
      ),
      child: downloading
          ? Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Downloading v$latestVersion…',
                    style: TextStyle(color: color, fontSize: 13)),
                const SizedBox(height: 8),
                LinearProgressIndicator(
                  value: progress,
                  backgroundColor: color.withValues(alpha: 0.2),
                  color: color,
                ),
                if (progress != null)
                  Padding(
                    padding: const EdgeInsets.only(top: 4),
                    child: Text('${(progress! * 100).toStringAsFixed(0)}%',
                        style: TextStyle(color: labelColor(context), fontSize: 11)),
                  ),
              ],
            )
          : Row(
              children: [
                Icon(Icons.system_update, color: color, size: 16),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    'Update available: v$latestVersion  (current v$mobileAppVersion)',
                    style: TextStyle(color: color, fontSize: 13),
                  ),
                ),
                GestureDetector(
                  onTap: onUpdate,
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                    decoration: BoxDecoration(
                      color: color.withValues(alpha: 0.15),
                      border: Border.all(color: color),
                      borderRadius: BorderRadius.circular(6),
                    ),
                    child: Text('Update',
                        style: TextStyle(color: color, fontSize: 12, fontWeight: FontWeight.w600)),
                  ),
                ),
              ],
            ),
    );
  }
}

class _Banner extends StatelessWidget {
  final String msg;
  final bool isError;
  const _Banner(this.msg, {this.isError = true});

  @override
  Widget build(BuildContext context) {
    final red = accentRed(context);
    final orange = accentOrange(context);
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: isError ? red.withValues(alpha: 0.1) : orange.withValues(alpha: 0.1),
        border: Border.all(color: isError ? red : orange),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Text(msg, style: TextStyle(color: isError ? red : orange, fontSize: 13)),
    );
  }
}

class _SummaryLine extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String text;
  final bool bold;
  const _SummaryLine({required this.icon, required this.color, required this.text, this.bold = false});

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 3),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(top: 1),
          child: Icon(icon, size: 13, color: color),
        ),
        const SizedBox(width: 6),
        Expanded(
          child: Text(
            text,
            style: TextStyle(
              color: color,
              fontSize: 12,
              fontWeight: bold ? FontWeight.w600 : FontWeight.normal,
            ),
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
          ),
        ),
      ],
    ),
  );
}

class _SectionCard extends StatelessWidget {
  final String title;
  final String? subtitle;
  final Widget child;
  final Widget? trailing;
  final bool titleMixed;
  const _SectionCard({required this.title, required this.child, this.trailing, this.titleMixed = false, this.subtitle});

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.all(14),
    decoration: BoxDecoration(
      color: cardBg(context), border: Border.all(color: cardBd(context)),
      borderRadius: BorderRadius.circular(12),
    ),
    child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
      Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
        Expanded(child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            if (subtitle != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 2),
                child: Text(subtitle!,
                  style: TextStyle(color: labelColor(context), fontSize: 11)),
              ),
            Text(title,
              style: TextStyle(
                color: titleMixed ? textColor(context) : labelColor(context),
                fontSize: titleMixed ? 14 : 10,
                fontWeight: titleMixed ? FontWeight.w700 : FontWeight.w400,
                letterSpacing: titleMixed ? 0.3 : 1,
              )),
          ],
        )),
        if (trailing != null) trailing!,
      ]),
      const SizedBox(height: 10),
      child,
    ]),
  );
}

class _SmallButton extends StatelessWidget {
  final String label;
  final bool danger;
  final VoidCallback onTap;
  const _SmallButton({required this.label, required this.onTap, this.danger = false});

  @override
  Widget build(BuildContext context) {
    final clr = danger ? accentRed(context) : accentBlue(context);
    return GestureDetector(
      onTap: () { AppHaptics.tap(); onTap(); },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
        decoration: BoxDecoration(
          color: clr.withOpacity(0.1),
          border: Border.all(color: clr),
          borderRadius: BorderRadius.circular(8),
        ),
        child: Text(label,
          style: TextStyle(color: clr, fontSize: 11, fontWeight: FontWeight.w600)),
      ),
    );
  }
}

class _ActionButton extends StatelessWidget {
  final String label;
  final IconData icon;
  final bool danger;
  final bool enabled;
  final VoidCallback onTap;
  final Color? accent;
  const _ActionButton({required this.label, required this.icon, required this.onTap,
    this.danger = false, this.enabled = true, this.accent});

  @override
  Widget build(BuildContext context) {
    final Color? ac = accent ?? (danger ? accentRed(context) : null);
    return Expanded(
      child: SizedBox(
        height: 46,
        child: FilledButton.tonalIcon(
          onPressed: enabled ? () { AppHaptics.tap(); onTap(); } : null,
          icon: Icon(icon, size: 16),
          label: Text(label, overflow: TextOverflow.ellipsis, maxLines: 1),
          style: FilledButton.styleFrom(
            backgroundColor: ac != null ? ac.withValues(alpha: 0.12) : subtleBg(context),
            foregroundColor: ac ?? textColor(context),
            disabledBackgroundColor: subtleBg(context).withValues(alpha: 0.4),
            disabledForegroundColor: labelColor(context).withValues(alpha: 0.5),
            padding: const EdgeInsets.symmetric(horizontal: 10),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(10),
              side: BorderSide(color: ac != null ? ac.withValues(alpha: 0.45) : cardBd(context)),
            ),
          ),
        ),
      ),
    );
  }
}

// ─── Segmented pill selector (Settings level / LCD choices) ──────────────────
class _SegSelector<T> extends StatelessWidget {
  final T value;
  final Map<String, T> options; // label -> value
  final ValueChanged<T>? onChanged;
  const _SegSelector({required this.value, required this.options, this.onChanged});

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.all(3),
    decoration: BoxDecoration(color: subtleBg(context), borderRadius: BorderRadius.circular(9)),
    child: Row(mainAxisSize: MainAxisSize.min, children: options.entries.map((e) {
      final on = e.value == value;
      return GestureDetector(
        onTap: onChanged == null ? null : () { AppHaptics.tap(); onChanged!(e.value); },
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
          decoration: BoxDecoration(
            color: on ? accentBlue(context) : Colors.transparent,
            borderRadius: BorderRadius.circular(7),
          ),
          child: Text(e.key, style: TextStyle(
            color: on ? Colors.white : labelColor(context),
            fontSize: 11, fontWeight: on ? FontWeight.w600 : FontWeight.w400)),
        ),
      );
    }).toList()),
  );
}

// ─── Compact read-only schedule preview row (no edit/delete) ─────────────────
class _SchedulePreviewRow extends StatelessWidget {
  final Schedule sch;
  final bool isNext;
  const _SchedulePreviewRow({required this.sch, this.isNext = false});

  @override
  Widget build(BuildContext context) => Container(
    margin: const EdgeInsets.symmetric(vertical: 3),
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
    decoration: BoxDecoration(
      color: isNext ? accentGreen(context).withOpacity(0.08) : subtleBg(context),
      border: isNext ? Border(left: BorderSide(color: accentGreen(context), width: 3)) : null,
      borderRadius: BorderRadius.circular(10),
    ),
    child: Row(children: [
      Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: sch.m == 'OH' ? accentGreen(context).withOpacity(0.1) : const Color(0xFF7c4dff).withOpacity(0.1),
          border: Border.all(color: sch.m == 'OH' ? accentGreen(context) : const Color(0xFF7c4dff)),
          borderRadius: BorderRadius.circular(6),
        ),
        child: Text(sch.m, style: TextStyle(
          color: sch.m == 'OH' ? accentGreen(context) : const Color(0xFF7c4dff),
          fontSize: 10, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
      ),
      const SizedBox(width: 10),
      Text(_to12hr(sch.t), style: TextStyle(color: textColor(context), fontSize: 13, fontWeight: FontWeight.w600)),
      const SizedBox(width: 8),
      Text('${sch.d} min', style: TextStyle(color: labelColor(context), fontSize: 11)),
      if (isNext) ...[        
        const Spacer(),
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
          decoration: BoxDecoration(
            color: accentGreen(context).withOpacity(0.1),
            borderRadius: BorderRadius.circular(4),
          ),
          child: Text('NEXT', style: TextStyle(
            color: accentGreen(context), fontSize: 9, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
        ),
      ],
    ]),
  );
}

// ─── Full scheduler modal bottom sheet ────────────────────────────────────────
class _SchedulerModal extends StatelessWidget {
  final TankService svc;
  final dynamic s; // DeviceStatus?
  final int? nextOHIdx;
  final int? nextUGIdx;
  const _SchedulerModal({required this.svc, required this.s, this.nextOHIdx, this.nextUGIdx});

  void _confirmClear(BuildContext ctx) {
    showDialog(context: ctx, builder: (_) => AlertDialog(
      backgroundColor: cardBg(ctx),
      title: Text('Clear all schedules?', style: TextStyle(color: textColor(ctx))),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(
          onPressed: () { Navigator.pop(ctx); svc.sendControl({'cmd': 'sched_clear'}); },
          child: Text('Clear', style: TextStyle(color: accentRed(ctx))),
        ),
      ],
    ));
  }

  @override
  Widget build(BuildContext context) {
    final allScheds = (s?.schedules as List<dynamic>?)?.cast<Schedule>() ?? <Schedule>[];
    return DraggableScrollableSheet(
      initialChildSize: 0.75,
      minChildSize: 0.4,
      maxChildSize: 0.95,
      expand: false,
      builder: (ctx, scrollCtrl) => Column(
        children: [
          // Handle bar
          Center(
            child: Container(
              margin: const EdgeInsets.only(top: 10, bottom: 6),
              width: 36, height: 4,
              decoration: BoxDecoration(
                color: cardBd(context), borderRadius: BorderRadius.circular(2)),
            ),
          ),
          // Header
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 4, 8, 8),
            child: Row(
              children: [
                Expanded(child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text('Scheduler', style: TextStyle(
                      color: textColor(context), fontSize: 16, fontWeight: FontWeight.w700)),
                    if (s?.time != null)
                      Text('Device time: ${_to12hr(s.time)}',
                          style: TextStyle(color: labelColor(context), fontSize: 11)),
                  ],
                )),
                _SmallButton(
                  label: '+ Add',
                  onTap: () => showModalBottomSheet(
                    context: ctx, isScrollControlled: true,
                    backgroundColor: cardBg(ctx),
                    builder: (_) => ScheduleSheet(svc: svc),
                  ),
                ),
                const SizedBox(width: 6),
                _SmallButton(
                  label: 'Clear All', danger: true,
                  onTap: () => _confirmClear(ctx),
                ),
                const SizedBox(width: 8),
                IconButton(
                  icon: Icon(Icons.close, color: labelColor(context), size: 20),
                  tooltip: 'Close',
                  padding: EdgeInsets.zero,
                  constraints: const BoxConstraints(),
                  onPressed: () => Navigator.pop(ctx),
                ),
                const SizedBox(width: 4),
              ],
            ),
          ),
          Divider(height: 1, color: cardBd(context)),
          // Schedule list
          Expanded(
            child: allScheds.isEmpty
                ? Center(child: Text('No schedules yet',
                    style: TextStyle(color: labelColor(context), fontSize: 14)))
                : ListView.builder(
                    controller: scrollCtrl,
                    padding: const EdgeInsets.all(12),
                    itemCount: allScheds.length,
                    itemBuilder: (_, i) {
                      final sch = allScheds[i];
                      return _ScheduleRow(
                        sch: sch, svc: svc,
                        isNext: sch.i == nextOHIdx || sch.i == nextUGIdx,
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}

class _ScheduleRow extends StatelessWidget {
  final Schedule sch;
  final TankService svc;
  final bool isNext;
  const _ScheduleRow({required this.sch, required this.svc, this.isNext = false});

  @override
  Widget build(BuildContext context) => Container(
    margin: const EdgeInsets.symmetric(vertical: 3),
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
    decoration: BoxDecoration(
      color: isNext
          ? accentGreen(context).withOpacity(0.08)
          : subtleBg(context),
      border: isNext
          ? Border(left: BorderSide(color: accentGreen(context), width: 3))
          : null,
      borderRadius: BorderRadius.circular(12),
    ),
    child: Row(children: [
      Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: sch.m == 'OH' ? accentGreen(context).withOpacity(0.1) : const Color(0xFF7c4dff).withOpacity(0.1),
          border: Border.all(color: sch.m == 'OH' ? accentGreen(context) : const Color(0xFF7c4dff)),
          borderRadius: BorderRadius.circular(6),
        ),
        child: Text(sch.m,
          style: TextStyle(
            color: sch.m == 'OH' ? accentGreen(context) : const Color(0xFF7c4dff),
            fontSize: 10, fontWeight: FontWeight.w700, letterSpacing: 0.5)),
      ),
      const SizedBox(width: 10),
      Text(_to12hr(sch.t), style: TextStyle(color: textColor(context), fontSize: 13, fontWeight: FontWeight.w600)),
      const SizedBox(width: 8),
      Text('${sch.d} min', style: TextStyle(color: labelColor(context), fontSize: 11)),
      if (isNext) ...[        
        const SizedBox(width: 6),
        Text('Next', style: TextStyle(color: accentGreen(context), fontSize: 10, fontWeight: FontWeight.w600)),
      ],
      const Spacer(),
      IconButton(
        icon: Icon(Icons.edit_outlined, color: accentBlue(context), size: 18),
        padding: EdgeInsets.zero, constraints: const BoxConstraints(),
        tooltip: 'Edit',
        onPressed: () => showModalBottomSheet(
          context: context, isScrollControlled: true,
          backgroundColor: cardBg(context),
          builder: (_) => ScheduleSheet(svc: svc, editSchedule: sch),
        ),
      ),
      const SizedBox(width: 8),
      IconButton(
        icon: Icon(Icons.delete_outline, color: accentRed(context), size: 18),
        padding: EdgeInsets.zero, constraints: const BoxConstraints(),
        onPressed: () => svc.sendControl({'cmd': 'sched_remove', 'index': sch.i}),
      ),
    ]),
  );
}

class _SettingRow extends StatelessWidget {
  final String label;
  final bool? value;
  final void Function(bool) onChange;
  final bool last;
  const _SettingRow(this.label, this.value, this.onChange, {this.last = false});

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(vertical: 8),
    decoration: BoxDecoration(
      border: last ? null : Border(bottom: BorderSide(color: cardBd(context)))),
    child: Row(children: [
      Expanded(child: Text(label, style: TextStyle(color: textColor(context), fontSize: 13))),
      Switch(
        value: value ?? false,
        onChanged: value == null ? null : onChange,
        activeColor: accentBlue(context),
      ),
    ]),
  );
}

class _InfoRow extends StatelessWidget {
  final String label;
  final String? value;
  final bool? loraOk;
  final bool last;
  const _InfoRow(this.label, this.value, {this.loraOk, this.last = false});

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(vertical: 7),
    decoration: BoxDecoration(
      border: last ? null : Border(bottom: BorderSide(color: cardBd(context)))),
    child: Row(children: [
      Text(label, style: TextStyle(color: labelColor(context), fontSize: 13)),
      const Spacer(),
      if (loraOk != null)
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
          decoration: BoxDecoration(
            color: loraOk! ? accentGreen(context).withOpacity(0.1) : accentRed(context).withOpacity(0.1),
            borderRadius: BorderRadius.circular(4),
          ),
          child: Text(loraOk! ? 'OK' : 'FAIL',
            style: TextStyle(color: loraOk! ? accentGreen(context) : accentRed(context), fontSize: 11, fontWeight: FontWeight.w700)),
        )
      else
        Text(value ?? '—', style: TextStyle(color: textColor(context), fontWeight: FontWeight.w500, fontSize: 13)),
    ]),
  );
}

class _LinkInfoRow extends StatelessWidget {
  final String label;
  final String url;
  final bool last;
  const _LinkInfoRow(this.label, this.url, {this.last = false});

  Future<void> _open() => launchUrl(Uri.parse(url), mode: LaunchMode.externalApplication);

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(vertical: 7),
    decoration: BoxDecoration(
      border: last ? null : Border(bottom: BorderSide(color: cardBd(context)))),
    child: Row(children: [
      Text(label, style: TextStyle(color: labelColor(context), fontSize: 13)),
      const Spacer(),
      GestureDetector(
        onTap: _open,
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          Text('GitHub Releases',
              style: TextStyle(
                color: accentBlue(context),
                fontSize: 13,
                fontWeight: FontWeight.w500,
                decoration: TextDecoration.underline,
              )),
          const SizedBox(width: 4),
          Icon(Icons.open_in_new, size: 13, color: accentBlue(context)),
        ]),
      ),
    ]),
  );
}
