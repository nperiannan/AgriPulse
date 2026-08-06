import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Dashboard concept identifiers.
enum DashboardConcept {
  hybrid,      // F — Arc gauges + grid motors  (default)
  nova,        // Elder-friendly next-gen: big glass tanks, plain language, every action visible
  clean,       // Minimal tank+motor unit cards
  console,     // Dense, everything above the fold
}

/// App theme mode.
enum AppThemeMode { light, dark, system }

/// Persistence keys.
const _kDashboardConcept = 'dashboard_concept';
const _kAppThemeMode     = 'app_theme_mode';
const _kUgMotorName      = 'ug_motor_name';
const _kOhMotorName      = 'oh_motor_name';
const _kPrefsVersion     = 'prefs_version';
const _kMotorNotify      = 'motor_notifications';
const _kOhFlowLPM        = 'oh_flow_lpm';
const _kUgFlowLPM        = 'ug_flow_lpm';
const _kShowOhWater      = 'show_oh_water_banner';
const _kShowUgWater      = 'show_ug_water_banner';

/// Current prefs schema version.
/// Bump this when adding new keys — the migration guard ensures smooth
/// upgrades without crashing existing installs.
const _currentPrefsVersion = 4;

/// Default estimated pump flow rates (litres/minute), used for the History
/// screen's water-volume estimate until the user calibrates their own pump.
const kDefaultOhFlowLPM = 30.0; // ½ HP motor · ¾" pipe, typical domestic rate
const kDefaultUgFlowLPM = 35.0;

class AppPreferences extends ChangeNotifier {
  DashboardConcept _concept = DashboardConcept.hybrid;
  AppThemeMode     _themeMode = AppThemeMode.dark;
  String _ugMotorName = 'UG Motor';
  String _ohMotorName = 'OH Motor';
  bool   _motorNotify = false;
  double _ohFlowLPM = kDefaultOhFlowLPM;
  double _ugFlowLPM = kDefaultUgFlowLPM;
  bool   _showOhWater = true;
  bool   _showUgWater = false;

  DashboardConcept get concept   => _concept;
  AppThemeMode     get themeMode => _themeMode;
  String get ugMotorName => _ugMotorName;
  String get ohMotorName => _ohMotorName;
  bool   get motorNotify => _motorNotify;
  double get ohFlowLPM => _ohFlowLPM;
  double get ugFlowLPM => _ugFlowLPM;
  bool   get showOhWater => _showOhWater;
  bool   get showUgWater => _showUgWater;

  /// Load saved preferences with migration guard.
  Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();

    // ── Migration guard ──────────────────────────────────────────────────
    final savedVersion = prefs.getInt(_kPrefsVersion) ?? 0;
    if (savedVersion < _currentPrefsVersion) {
      // First time or upgrading from an older version.
      // Set defaults for any keys that don't exist yet; never overwrite
      // values the user already saved.
      if (!prefs.containsKey(_kDashboardConcept)) {
        await prefs.setString(_kDashboardConcept, DashboardConcept.hybrid.name);
      }
      if (!prefs.containsKey(_kAppThemeMode)) {
        await prefs.setString(_kAppThemeMode, AppThemeMode.dark.name);
      }
      if (!prefs.containsKey(_kMotorNotify)) {
        await prefs.setBool(_kMotorNotify, false);
      }
      if (!prefs.containsKey(_kOhFlowLPM)) {
        await prefs.setDouble(_kOhFlowLPM, kDefaultOhFlowLPM);
      }
      if (!prefs.containsKey(_kUgFlowLPM)) {
        await prefs.setDouble(_kUgFlowLPM, kDefaultUgFlowLPM);
      }
      if (!prefs.containsKey(_kShowOhWater)) {
        await prefs.setBool(_kShowOhWater, true);
      }
      if (!prefs.containsKey(_kShowUgWater)) {
        await prefs.setBool(_kShowUgWater, false);
      }
      await prefs.setInt(_kPrefsVersion, _currentPrefsVersion);
    }

    // ── Read values ──────────────────────────────────────────────────────
    final conceptStr  = prefs.getString(_kDashboardConcept);
    final themeModeStr = prefs.getString(_kAppThemeMode);

    _concept = DashboardConcept.values.firstWhere(
      (e) => e.name == conceptStr,
      orElse: () => DashboardConcept.hybrid,
    );
    _themeMode = AppThemeMode.values.firstWhere(
      (e) => e.name == themeModeStr,
      orElse: () => AppThemeMode.dark,
    );
    _ugMotorName = prefs.getString(_kUgMotorName) ?? 'UG Motor';
    _ohMotorName = prefs.getString(_kOhMotorName) ?? 'OH Motor';
    _motorNotify = prefs.getBool(_kMotorNotify) ?? false;
    _ohFlowLPM = prefs.getDouble(_kOhFlowLPM) ?? kDefaultOhFlowLPM;
    _ugFlowLPM = prefs.getDouble(_kUgFlowLPM) ?? kDefaultUgFlowLPM;
    _showOhWater = prefs.getBool(_kShowOhWater) ?? true;
    _showUgWater = prefs.getBool(_kShowUgWater) ?? false;

    notifyListeners();
  }

  Future<void> setConcept(DashboardConcept c) async {
    if (_concept == c) return;
    _concept = c;
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kDashboardConcept, c.name);
  }

  Future<void> setThemeMode(AppThemeMode m) async {
    if (_themeMode == m) return;
    _themeMode = m;
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kAppThemeMode, m.name);
  }

  Future<void> setMotorName(String motor, String name) async {
    if (motor == 'UG') {
      _ugMotorName = name;
    } else {
      _ohMotorName = name;
    }
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      motor == 'UG' ? _kUgMotorName : _kOhMotorName, name);
  }

  Future<void> setMotorNotify(bool v) async {
    if (_motorNotify == v) return;
    _motorNotify = v;
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_kMotorNotify, v);
  }

  /// Sets the calibrated pump flow rate (litres/minute) used for the water-
  /// volume estimate on the History screen. [motor] is 'OH' or 'UG'.
  Future<void> setFlowLPM(String motor, double lpm) async {
    if (motor == 'UG') {
      _ugFlowLPM = lpm;
    } else {
      _ohFlowLPM = lpm;
    }
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setDouble(motor == 'UG' ? _kUgFlowLPM : _kOhFlowLPM, lpm);
  }

  /// Toggles whether the History screen shows the OH/UG water-pumped
  /// estimate banner. [motor] is 'OH' or 'UG'.
  Future<void> setShowWaterBanner(String motor, bool v) async {
    if (motor == 'UG') {
      if (_showUgWater == v) return;
      _showUgWater = v;
    } else {
      if (_showOhWater == v) return;
      _showOhWater = v;
    }
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(motor == 'UG' ? _kShowUgWater : _kShowOhWater, v);
  }

  /// Resolve effective ThemeMode for MaterialApp.
  ThemeMode get effectiveThemeMode {
    switch (_themeMode) {
      case AppThemeMode.light:  return ThemeMode.light;
      case AppThemeMode.dark:   return ThemeMode.dark;
      case AppThemeMode.system: return ThemeMode.system;
    }
  }
}
