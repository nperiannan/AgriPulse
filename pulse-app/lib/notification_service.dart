import 'package:flutter_local_notifications/flutter_local_notifications.dart';

class NotificationService {
  static final _plugin = FlutterLocalNotificationsPlugin();
  static bool _initialized = false;

  static Future<void> init() async {
    if (_initialized) return;
    const androidSettings = AndroidInitializationSettings('@mipmap/ic_launcher');
    const settings = InitializationSettings(android: androidSettings);
    await _plugin.initialize(settings);
    // Pre-create the channel so background/terminated FCM notifications (which
    // the OS renders directly) display reliably on Android O+.
    const channel = AndroidNotificationChannel(
      'motor_status',
      'Motor Status',
      description: 'Notifications when motors turn on or off',
      importance: Importance.high,
    );
    await _plugin
        .resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()
        ?.createNotificationChannel(channel);
    _initialized = true;
  }

  static Future<void> showMotorNotification({
    required String title,
    required String body,
  }) async {
    if (!_initialized) return;
    const details = AndroidNotificationDetails(
      'motor_status',
      'Motor Status',
      channelDescription: 'Notifications when motors turn on or off',
      importance: Importance.high,
      priority: Priority.high,
    );
    await _plugin.show(
      DateTime.now().millisecondsSinceEpoch ~/ 1000,
      title,
      body,
      const NotificationDetails(android: details),
    );
  }
}
