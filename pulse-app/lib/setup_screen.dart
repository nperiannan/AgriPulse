import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'dashboard_screen.dart';
import 'theme_data.dart';

class SetupScreen extends StatefulWidget {
  const SetupScreen({super.key});

  @override
  State<SetupScreen> createState() => _SetupScreenState();
}

class _SetupScreenState extends State<SetupScreen> {
  final _mobileCtrl = TextEditingController();
  final _directIpCtrl = TextEditingController();
  final _form       = GlobalKey<FormState>();
  bool  _connecting = false;
  bool  _directMode = false;

  @override
  void initState() {
    super.initState();
    final svc = context.read<TankService>();
    _mobileCtrl.text = svc.mobileUrl.isNotEmpty ? svc.mobileUrl : defaultMobileUrl;
    _directMode      = svc.directMode;
    _directIpCtrl.text = svc.directIp.isNotEmpty
        ? svc.directIp
        : 'http://192.168.4.1';
  }

  @override
  void dispose() {
    _mobileCtrl.dispose();
    _directIpCtrl.dispose();
    super.dispose();
  }

  Future<void> _connect() async {
    if (!_form.currentState!.validate()) return;
    setState(() => _connecting = true);

    final svc = context.read<TankService>();

    if (_directMode) {
      // Direct IP mode — no auth, poll ESP32 HTTP directly
      final ip = _directIpCtrl.text.trim();
      await svc.saveDirectMode(true, ip);
      svc.connectDirect(ip);
      await Future.delayed(const Duration(seconds: 2));
      if (!mounted) return;
      setState(() => _connecting = false);
      Navigator.of(context).pushReplacement(
        MaterialPageRoute(builder: (_) => const DashboardScreen()),
      );
      return;
    }

    // Cloud mode
    await svc.saveDirectMode(false, '');
    await svc.saveUrls(
      wifi:   _mobileCtrl.text.trim(),
      mobile: _mobileCtrl.text.trim(),
    );
    await svc.connectAuto(); // auto-picks based on current network

    await Future.delayed(const Duration(seconds: 2));
    if (!mounted) return;
    setState(() => _connecting = false);
    Navigator.of(context).pushReplacement(
      MaterialPageRoute(builder: (_) => const DashboardScreen()),
    );
  }

  @override
  Widget build(BuildContext context) {
    final canGoBack = Navigator.of(context).canPop();
    return Scaffold(
      backgroundColor: scaffoldBg(context),
      appBar: AppBar(
        backgroundColor: cardBg(context),
        elevation: 0,
        leading: canGoBack
            ? BackButton(
                color: labelColor(context),
                onPressed: () => Navigator.of(context).pop(),
              )
            : null,
        automaticallyImplyLeading: false,
        title: Text('Server Settings',
            style: TextStyle(color: textColor(context), fontSize: 16, fontWeight: FontWeight.w600)),
      ),
      body: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(28),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const SizedBox(height: 12),
              Text(
                '💧 Tank Monitor',
                style: TextStyle(
                  fontSize: 28, fontWeight: FontWeight.w700,
                  color: accentBlue(context),
                ),
              ),
              const SizedBox(height: 8),
              Text(
                'Choose how to connect to your device.',
                style: TextStyle(color: labelColor(context), fontSize: 13),
              ),
              const SizedBox(height: 24),

              // ── Connection mode toggle ───────────────────────────────────
              Row(
                children: [
                  Expanded(
                    child: _ModeButton(
                      icon: Icons.cloud,
                      label: 'Cloud',
                      subtitle: 'Via web backend',
                      selected: !_directMode,
                      onTap: () => setState(() => _directMode = false),
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: _ModeButton(
                      icon: Icons.router,
                      label: 'Direct IP',
                      subtitle: 'ESP32 on LAN',
                      selected: _directMode,
                      onTap: () => setState(() => _directMode = true),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 24),

              Form(
                key: _form,
                child: _directMode ? _buildDirectFields() : _buildCloudFields(),
              ),

              const SizedBox(height: 20),
              Container(
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: accentBlue(context).withOpacity(0.08),
                  border: Border.all(color: accentBlue(context).withOpacity(0.35)),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Row(children: [
                  Icon(Icons.info_outline, color: accentBlue(context), size: 16),
                  const SizedBox(width: 8),
                  Expanded(child: Text(
                    _directMode
                        ? 'Connects directly to the ESP32 controller.\n'
                          'Works without internet. Use 192.168.4.1 when\n'
                          'connected to the AgriPulse AP hotspot.'
                        : 'Uses this URL for all connections (WiFi and mobile data).\n'
                          'Server is in the cloud — accessible from anywhere.',
                    style: TextStyle(color: labelColor(context), fontSize: 12),
                  )),
                ]),
              ),
              const SizedBox(height: 32),

              SizedBox(
                width: double.infinity,
                height: 48,
                child: ElevatedButton(
                  onPressed: _connecting ? null : _connect,
                  style: ElevatedButton.styleFrom(
                    backgroundColor: accentBlue(context),
                    foregroundColor: Colors.white,
                    shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(8)),
                  ),
                  child: _connecting
                      ? const SizedBox(width: 20, height: 20,
                          child: CircularProgressIndicator(
                            color: Colors.white, strokeWidth: 2))
                      : const Text('Save & Connect',
                          style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildCloudFields() {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      _FieldLabel(icon: Icons.cloud, color: accentBlue(context),
        text: 'Server URL'),
      const SizedBox(height: 6),
      _UrlField(controller: _mobileCtrl, hint: 'http://nperiannan-nas.freemyip.com:1880', label: 'Server URL'),
      const SizedBox(height: 6),
      _DefaultChip(
        label: 'Use default  nperiannan-nas.freemyip.com:1880',
        onTap: () => setState(() => _mobileCtrl.text = defaultMobileUrl),
      ),
      const SizedBox(height: 8),
      Text(
        'ℹ️ Resolves to 150.230.129.215 (Oracle Cloud — static IP)',
        style: TextStyle(color: labelColor(context), fontSize: 11),
      ),
    ]);
  }

  Widget _buildDirectFields() {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      _FieldLabel(icon: Icons.router, color: accentOrange(context),
        text: 'Device IP Address'),
      const SizedBox(height: 6),
      _UrlField(
        controller: _directIpCtrl,
        hint: 'http://192.168.4.1',
        label: 'ESP32 IP',
      ),
      const SizedBox(height: 6),
      _DefaultChip(
        label: 'AP hotspot default  192.168.4.1',
        onTap: () => setState(() => _directIpCtrl.text = 'http://192.168.4.1'),
      ),
    ]);
  }
}

class _FieldLabel extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String text;
  const _FieldLabel({required this.icon, required this.color, required this.text});
  @override
  Widget build(BuildContext context) => Row(children: [
    Icon(icon, color: color, size: 15),
    const SizedBox(width: 6),
    Text(text, style: TextStyle(color: labelColor(context), fontSize: 12)),
  ]);
}

class _UrlField extends StatelessWidget {
  final TextEditingController controller;
  final String hint;
  final String label;
  const _UrlField({required this.controller, required this.hint, required this.label});

  @override
  Widget build(BuildContext context) => TextFormField(
    controller: controller,
    style: TextStyle(color: textColor(context)),
    keyboardType: TextInputType.url,
    autocorrect: false,
    decoration: InputDecoration(
      labelText: label,
      hintText: hint,
      labelStyle: TextStyle(color: labelColor(context)),
      hintStyle: TextStyle(color: labelColor(context).withOpacity(0.5)),
      filled: true,
      fillColor: cardBg(context),
      border: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: cardBd(context))),
      enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: cardBd(context))),
      focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: accentBlue(context))),
      errorBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
        borderSide: BorderSide(color: accentRed(context))),
    ),
    validator: (v) {
      if (v == null || v.trim().isEmpty) return 'Required';
      final uri = Uri.tryParse(v.trim());
      if (uri == null || !uri.scheme.startsWith('http')) {
        return 'Must start with http:// or https://';
      }
      return null;
    },
  );
}

class _DefaultChip extends StatelessWidget {
  final String label;
  final VoidCallback onTap;
  const _DefaultChip({required this.label, required this.onTap});

  @override
  Widget build(BuildContext context) => GestureDetector(
    onTap: onTap,
    child: Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: accentBlue(context).withOpacity(0.08),
        border: Border.all(color: accentBlue(context).withOpacity(0.4)),
        borderRadius: BorderRadius.circular(20),
      ),
      child: Text('↩ $label',
        style: TextStyle(color: accentBlue(context), fontSize: 11)),
    ),
  );
}

class _ModeButton extends StatelessWidget {
  final IconData icon;
  final String label;
  final String subtitle;
  final bool selected;
  final VoidCallback onTap;
  const _ModeButton({
    required this.icon, required this.label, required this.subtitle,
    required this.selected, required this.onTap,
  });

  @override
  Widget build(BuildContext context) => GestureDetector(
    onTap: onTap,
    child: Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 14),
      decoration: BoxDecoration(
        color: selected
            ? accentBlue(context).withOpacity(0.12)
            : cardBg(context),
        border: Border.all(
          color: selected ? accentBlue(context) : cardBd(context),
          width: selected ? 2 : 1,
        ),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Column(
        children: [
          Icon(icon, color: selected ? accentBlue(context) : labelColor(context), size: 28),
          const SizedBox(height: 6),
          Text(label, style: TextStyle(
            color: selected ? accentBlue(context) : textColor(context),
            fontSize: 14, fontWeight: FontWeight.w600)),
          Text(subtitle, style: TextStyle(
            color: labelColor(context), fontSize: 11)),
        ],
      ),
    ),
  );
}