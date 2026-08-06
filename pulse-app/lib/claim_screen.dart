import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'theme_data.dart';

class ClaimScreen extends StatefulWidget {
  const ClaimScreen({super.key});

  @override
  State<ClaimScreen> createState() => _ClaimScreenState();
}

class _ClaimScreenState extends State<ClaimScreen> {
  final _macCtrl  = TextEditingController();
  final _nameCtrl = TextEditingController();
  bool _loading  = false;
  String? _error;
  String _typeId = 'tank_monitor'; // default

  static const _deviceTypes = [
    _DeviceTypeOption(id: 'tank_monitor', label: 'Tank Monitor', icon: Icons.water_drop_outlined),
    _DeviceTypeOption(id: 'smart_ps',     label: 'Smart PS',     icon: Icons.bolt_outlined),
  ];

  @override
  void dispose() {
    _macCtrl.dispose();
    _nameCtrl.dispose();
    super.dispose();
  }

  Future<void> _claim() async {
    final mac  = _macCtrl.text.trim().toUpperCase();
    final name = _nameCtrl.text.trim();
    if (mac.isEmpty || name.isEmpty) {
      setState(() => _error = 'MAC address and device name are required');
      return;
    }
    setState(() { _loading = true; _error = null; });
    final svc = context.read<TankService>();
    final ok = await svc.claimDevice(mac, name, typeId: _typeId);
    if (!mounted) return;
    if (ok) {
      Navigator.of(context).pop(true);
    } else {
      setState(() {
        _loading = false;
        _error = svc.error ?? 'Failed to claim device';
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: scaffoldBg(context),
      appBar: AppBar(
        backgroundColor: cardBg(context),
        elevation: 0,
        leading: BackButton(color: labelColor(context).withValues(alpha: 0.8)),
        title: Text('Claim Device',
            style: TextStyle(color: textColor(context), fontWeight: FontWeight.w600, fontSize: 17)),
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Add a device to your account by entering its MAC address.',
              style: TextStyle(color: labelColor(context), fontSize: 13),
            ),
            const SizedBox(height: 6),
            Text(
              'You can find the MAC address on the device LCD screen or physical label.',
              style: TextStyle(color: labelColor(context), fontSize: 13),
            ),
            const SizedBox(height: 24),

            // ── Device type ──────────────────────────────────────────────
            Text('Device Type', style: TextStyle(color: labelColor(context), fontSize: 12)),
            const SizedBox(height: 8),
            Row(
              children: _deviceTypes.map((t) => Expanded(
                child: Padding(
                  padding: EdgeInsets.only(right: t == _deviceTypes.last ? 0 : 8),
                  child: GestureDetector(
                    onTap: () => setState(() => _typeId = t.id),
                    child: Container(
                      padding: const EdgeInsets.symmetric(vertical: 12),
                      decoration: BoxDecoration(
                        color: _typeId == t.id
                            ? accentBlue(context).withValues(alpha: 0.12)
                            : cardBg(context),
                        border: Border.all(
                          color: _typeId == t.id ? accentBlue(context) : cardBd(context),
                          width: _typeId == t.id ? 2 : 1,
                        ),
                        borderRadius: BorderRadius.circular(8),
                      ),
                      child: Column(
                        children: [
                          Icon(t.icon,
                            color: _typeId == t.id ? accentBlue(context) : labelColor(context), size: 26),
                          const SizedBox(height: 4),
                          Text(t.label,
                            style: TextStyle(
                              color: _typeId == t.id ? accentBlue(context) : labelColor(context),
                              fontSize: 12, fontWeight: FontWeight.w600)),
                        ],
                      ),
                    ),
                  ),
                ),
              )).toList(),
            ),
            const SizedBox(height: 24),

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
                      child: Text(_error!,
                          style: TextStyle(color: accentRed(context), fontSize: 13)),
                    ),
                  ],
                ),
              ),

            Text('MAC address', style: TextStyle(color: labelColor(context), fontSize: 12)),
            const SizedBox(height: 6),
            TextField(
              controller: _macCtrl,
              decoration: _inputDec('e.g. AA:BB:CC:DD:EE:FF', Icons.memory_outlined),
              textCapitalization: TextCapitalization.characters,
              textInputAction: TextInputAction.next,
              style: TextStyle(color: textColor(context), fontFamily: 'monospace'),
            ),
            const SizedBox(height: 16),

            Text('Device name', style: TextStyle(color: labelColor(context), fontSize: 12)),
            const SizedBox(height: 6),
            TextField(
              controller: _nameCtrl,
              decoration: _inputDec('e.g. Rooftop Tank', Icons.label_outline),
              textInputAction: TextInputAction.done,
              onSubmitted: (_) => _claim(),
              style: TextStyle(color: textColor(context)),
            ),
            const SizedBox(height: 28),

            SizedBox(
              width: double.infinity,
              child: FilledButton(
                onPressed: _loading ? null : _claim,
                style: FilledButton.styleFrom(
                  backgroundColor: accentBlue(context),
                  padding: const EdgeInsets.symmetric(vertical: 14),
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                ),
                child: _loading
                    ? const SizedBox(
                        height: 18, width: 18,
                        child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white),
                      )
                    : const Text('Claim Device', style: TextStyle(fontSize: 15)),
              ),
            ),
          ],
        ),
      ),
    );
  }

  InputDecoration _inputDec(String hint, IconData icon) => InputDecoration(
        hintText: hint,
        hintStyle: TextStyle(color: labelColor(context)),
        prefixIcon: Icon(icon, color: labelColor(context), size: 20),
        filled: true,
        fillColor: cardBg(context),
        border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: BorderSide(color: cardBd(context))),
        enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: BorderSide(color: cardBd(context))),
        focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: BorderSide(color: accentBlue(context), width: 2)),
        contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      );
}

class _DeviceTypeOption {
  final String id;
  final String label;
  final IconData icon;
  const _DeviceTypeOption({required this.id, required this.label, required this.icon});
}
