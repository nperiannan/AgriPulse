import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'setup_screen.dart';
import 'device_list_screen.dart';
import 'dashboard_screen.dart';
import 'register_screen.dart';
import 'theme_data.dart';

class LoginScreen extends StatefulWidget {
  const LoginScreen({super.key});

  @override
  State<LoginScreen> createState() => _LoginScreenState();
}

class _LoginScreenState extends State<LoginScreen> {
  final _userCtrl = TextEditingController();
  final _passCtrl = TextEditingController();
  bool _loading  = false;
  String? _error;
  bool _obscure  = true;

  @override
  void dispose() {
    _userCtrl.dispose();
    _passCtrl.dispose();
    super.dispose();
  }

  Future<void> _login() async {
    final username = _userCtrl.text.trim();
    final password = _passCtrl.text;
    if (username.isEmpty || password.isEmpty) {
      setState(() => _error = 'Please enter username and password');
      return;
    }
    setState(() { _loading = true; _error = null; });
    final svc = context.read<TankService>();
    final ok = await svc.login(username, password);
    if (!mounted) return;
    if (ok) {
      await svc.loadSavedUrls();
      if (!mounted) return;
      if (svc.wifiUrl.isNotEmpty || svc.mobileUrl.isNotEmpty) {
        await svc.connectAuto();
        if (mounted) {
                Navigator.of(context).pushReplacement(
            MaterialPageRoute(builder: (_) => const DeviceListScreen()),
          );
        }
      } else {
        if (mounted) {
          Navigator.of(context).pushReplacement(
            MaterialPageRoute(builder: (_) => const SetupScreen()),
          );
        }
      }
    } else {
      setState(() {
        _loading = false;
        _error = svc.error ?? 'Invalid username or password';
      });
    }
  }

  void _connectDirect() {
    final ipCtrl = TextEditingController(text: 'http://192.168.4.1');
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: cardBg(ctx),
        title: Text('Direct IP Connection',
            style: TextStyle(color: textColor(ctx), fontSize: 16)),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Connect directly to the ESP32 controller.\nNo login required — works on LAN or AP hotspot.',
                style: TextStyle(color: labelColor(ctx), fontSize: 12)),
            const SizedBox(height: 12),
            TextField(
              controller: ipCtrl,
              style: TextStyle(color: textColor(ctx)),
              decoration: InputDecoration(
                labelText: 'Device IP',
                labelStyle: TextStyle(color: labelColor(ctx)),
                hintText: 'http://192.168.4.1',
                hintStyle: TextStyle(color: labelColor(ctx).withOpacity(0.5)),
                filled: true,
                fillColor: cardBg(ctx),
                border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
              ),
            ),
          ],
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () async {
              Navigator.pop(ctx);
              final svc = context.read<TankService>();
              final ip = ipCtrl.text.trim();
              await svc.saveDirectMode(true, ip);
              svc.connectDirect(ip);
              if (mounted) {
                Navigator.of(context).pushReplacement(
                  MaterialPageRoute(builder: (_) => const DashboardScreen()),
                );
              }
            },
            child: Text('Connect', style: TextStyle(color: accentBlue(ctx))),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: scaffoldBg(context),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 360),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Text('💧', style: TextStyle(fontSize: 52)),
                const SizedBox(height: 8),
                Text(
                  'Tank Monitor',
                  style: TextStyle(
                    fontSize: 24, fontWeight: FontWeight.bold,
                    color: accentBlue(context),
                  ),
                ),
                const SizedBox(height: 4),
                Text(
                  'Sign in to continue',
                  style: TextStyle(fontSize: 13, color: labelColor(context)),
                ),
                const SizedBox(height: 32),

                if (_error != null)
                  Container(
                    margin: const EdgeInsets.only(bottom: 16),
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                    decoration: BoxDecoration(
                      color: accentRed(context).withValues(alpha: 0.1),
                      border: Border.all(color: accentRed(context).withValues(alpha: 0.3)),
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Row(
                      children: [
                        Icon(Icons.error_outline, color: accentRed(context), size: 16),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(
                            _error!,
                            style: TextStyle(color: accentRed(context), fontSize: 13),
                          ),
                        ),
                      ],
                    ),
                  ),

                TextField(
                  controller: _userCtrl,
                  style: TextStyle(color: textColor(context)),
                  decoration: _inputDecoration(context, 'Username', Icons.person_outline),
                  textInputAction: TextInputAction.next,
                  autofillHints: const [AutofillHints.username],
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _passCtrl,
                  obscureText: _obscure,
                  style: TextStyle(color: textColor(context)),
                  decoration: _inputDecoration(context, 'Password', Icons.lock_outline).copyWith(
                    suffixIcon: IconButton(
                      icon: Icon(
                        _obscure ? Icons.visibility_off : Icons.visibility,
                        color: labelColor(context), size: 20,
                      ),
                      onPressed: () => setState(() => _obscure = !_obscure),
                    ),
                  ),
                  textInputAction: TextInputAction.done,
                  onSubmitted: (_) => _login(),
                  autofillHints: const [AutofillHints.password],
                ),
                const SizedBox(height: 24),

                SizedBox(
                  width: double.infinity,
                  child: FilledButton(
                    onPressed: _loading ? null : _login,
                    style: FilledButton.styleFrom(
                      backgroundColor: accentBlue(context),
                      foregroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(vertical: 14),
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                    ),
                    child: _loading
                        ? const SizedBox(
                            height: 18, width: 18,
                            child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white),
                          )
                        : const Text('Sign In', style: TextStyle(fontSize: 15)),
                  ),
                ),
                const SizedBox(height: 20),

                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    Text('New here? ',
                        style: TextStyle(color: labelColor(context), fontSize: 13)),
                    GestureDetector(
                      onTap: () => Navigator.of(context).push(
                        MaterialPageRoute(builder: (_) => const RegisterScreen()),
                      ),
                      child: Text('Create account',
                          style: TextStyle(
                              color: accentBlue(context),
                              fontSize: 13,
                              fontWeight: FontWeight.w600)),
                    ),
                  ],
                ),
                const SizedBox(height: 24),

                // ── Server Settings ──
                GestureDetector(
                  onTap: () => Navigator.of(context).push(
                    MaterialPageRoute(builder: (_) => const SetupScreen()),
                  ),
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(Icons.settings, color: labelColor(context), size: 14),
                      const SizedBox(width: 4),
                      Text('Server Settings',
                          style: TextStyle(color: labelColor(context), fontSize: 12)),
                    ],
                  ),
                ),
                const SizedBox(height: 12),

                // ── Direct IP (no login) ──
                GestureDetector(
                  onTap: () => _connectDirect(),
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(Icons.router, color: accentOrange(context), size: 14),
                      const SizedBox(width: 4),
                      Text('Connect directly via IP (no login)',
                          style: TextStyle(color: accentOrange(context), fontSize: 12)),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  InputDecoration _inputDecoration(BuildContext context, String hint, IconData icon) {
    return InputDecoration(
      hintText: hint,
      hintStyle: TextStyle(color: labelColor(context)),
      prefixIcon: Icon(icon, color: labelColor(context), size: 20),
      filled: true,
      fillColor: cardBg(context),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: cardBd(context)),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: cardBd(context)),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: accentBlue(context), width: 2),
      ),
      contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
    );
  }
}
