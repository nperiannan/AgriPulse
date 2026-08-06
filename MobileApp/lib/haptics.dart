import 'package:flutter/services.dart';
import 'package:vibration/vibration.dart';

/// Centralized haptic feedback.
///
/// [tap] is a light selection click for general buttons; [motor] is a strong,
/// long vibration reserved for motor ON/OFF (and cancel) actions.
class AppHaptics {
  static bool _checked = false;
  static bool _hasVibrator = false;
  static bool _hasAmplitude = false;

  static Future<void> _ensure() async {
    if (_checked) return;
    _checked = true;
    try {
      _hasVibrator = await Vibration.hasVibrator();
      if (_hasVibrator) {
        _hasAmplitude = await Vibration.hasAmplitudeControl();
      }
    } catch (_) {
      _hasVibrator = false;
    }
  }

  /// Light feedback for general buttons (Sync NTP, WiFi, Reboot, Factory Reset,
  /// Flash, Rollback, History, dialog actions, etc.).
  static void tap() {
    HapticFeedback.selectionClick();
  }

  /// Medium feedback for confirming a potentially destructive dialog action.
  static void confirm() {
    HapticFeedback.mediumImpact();
  }

  /// Short, firm vibration for motor ON/OFF and cancel actions.
  static Future<void> motor() async {
    await _ensure();
    if (_hasVibrator) {
      try {
        Vibration.vibrate(duration: 220, amplitude: _hasAmplitude ? 255 : -1);
        return;
      } catch (_) {}
    }
    // Fallback when no controllable vibrator is available.
    HapticFeedback.heavyImpact();
  }
}
