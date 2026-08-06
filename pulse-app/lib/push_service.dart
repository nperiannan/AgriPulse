import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'notification_service.dart';

/// Firebase Cloud Messaging integration for background push notifications.
///
/// Entirely optional: if the app has no Firebase configuration bundled
/// (no `google-services.json`), [init] fails gracefully and [enabled] stays
/// false, so the app behaves exactly as before.
class PushService {
  static bool enabled = false;
  static String? _token;
  static void Function(String token)? _onToken;

  /// Initializes Firebase + FCM handlers. Safe no-op if Firebase isn't set up.
  static Future<void> init() async {
    try {
      await Firebase.initializeApp();
    } catch (_) {
      // No Firebase config bundled — push disabled, app continues normally.
      enabled = false;
      return;
    }
    try {
      final messaging = FirebaseMessaging.instance;
      await messaging.requestPermission(alert: true, badge: true, sound: true);

      // Foreground messages: Android doesn't auto-display these, so mirror them
      // into a local notification.
      FirebaseMessaging.onMessage.listen((RemoteMessage m) {
        final n = m.notification;
        if (n != null) {
          NotificationService.showMotorNotification(
            title: n.title ?? 'Tank Monitor',
            body: n.body ?? '',
          );
        }
      });

      FirebaseMessaging.instance.onTokenRefresh.listen((t) {
        _token = t;
        _onToken?.call(t);
      });

      _token = await messaging.getToken();
      enabled = true;
      final t = _token;
      if (t != null) _onToken?.call(t);
    } catch (_) {
      enabled = false;
    }
  }

  /// Registers a callback invoked with the current token (and on every refresh).
  static void onToken(void Function(String token) cb) {
    _onToken = cb;
    final t = _token;
    if (t != null) cb(t);
  }

  static String? get token => _token;
}
