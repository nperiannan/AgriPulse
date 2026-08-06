import 'package:flutter/material.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'app_preferences.dart';
import 'notification_service.dart';
import 'push_service.dart';
import 'theme_data.dart';
import 'login_screen.dart';
import 'setup_screen.dart';
import 'device_list_screen.dart';
import 'dashboard_screen.dart';

/// Background/terminated FCM handler. Notification messages are displayed by
/// the OS automatically; this just needs to exist and be a top-level function.
@pragma('vm:entry-point')
Future<void> _firebaseBackgroundHandler(RemoteMessage message) async {}

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await NotificationService.init();
  await PushService.init(); // no-op if Firebase isn't configured
  if (PushService.enabled) {
    FirebaseMessaging.onBackgroundMessage(_firebaseBackgroundHandler);
  }
  runApp(
    MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => TankService()),
        ChangeNotifierProvider(create: (_) => AppPreferences()),
      ],
      child: const TankMonitorApp(),
    ),
  );
}

class TankMonitorApp extends StatelessWidget {
  const TankMonitorApp({super.key});

  @override
  Widget build(BuildContext context) {
    final prefs = context.watch<AppPreferences>();
    return MaterialApp(
      title: 'Tank Monitor',
      debugShowCheckedModeBanner: false,
      theme: lightTheme,
      darkTheme: darkTheme,
      themeMode: prefs.effectiveThemeMode,
      home: const _Startup(),
    );
  }
}

/// Loads the saved token on first frame and routes to the right screen.
/// Token missing  → LoginScreen
/// Token present, no saved URL → SetupScreen
/// Token present, URL saved → DeviceListScreen (auto-navigates to Dashboard for single device)
class _Startup extends StatefulWidget {
  const _Startup();

  @override
  State<_Startup> createState() => _StartupState();
}

class _StartupState extends State<_Startup> {
  bool _ready = false;
  Widget _home = const _Splash();

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    final svc = context.read<TankService>();
    final prefs = context.read<AppPreferences>();
    await Future.wait([svc.loadToken(), svc.loadSavedUrls(), prefs.load()]);
    svc.motorNotifyEnabled = prefs.motorNotify;
    prefs.addListener(() {
      svc.motorNotifyEnabled = prefs.motorNotify;
    });

    // Direct mode: skip login/device list, go straight to dashboard
    if (svc.directMode && svc.directIp.isNotEmpty) {
      svc.connectDirect(svc.directIp);
      setState(() { _home = const DashboardScreen(); _ready = true; });
      return;
    }

    if (svc.authToken == null) {
      setState(() { _home = const LoginScreen(); _ready = true; });
      return;
    }
    if (svc.wifiUrl.isNotEmpty || svc.mobileUrl.isNotEmpty) {
      setState(() { _home = const DeviceListScreen(); _ready = true; });
    } else {
      setState(() { _home = const SetupScreen(); _ready = true; });
    }
  }

  @override
  Widget build(BuildContext context) {
    if (!_ready) return const _Splash();
    return _home;
  }
}

class _Splash extends StatelessWidget {
  const _Splash();
  @override
  Widget build(BuildContext context) {
    return const Scaffold(
      body: Center(child: Text('💧', style: TextStyle(fontSize: 48))),
    );
  }
}
