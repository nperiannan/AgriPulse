import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'theme_data.dart';

class WifiManagementScreen extends StatefulWidget {
  const WifiManagementScreen({super.key});

  @override
  State<WifiManagementScreen> createState() => _WifiManagementScreenState();
}

class _WifiManagementScreenState extends State<WifiManagementScreen> {
  List<Map<String, dynamic>> _savedNetworks = [];
  List<Map<String, dynamic>> _apClients = [];
  List<Map<String, dynamic>> _scanResults = [];
  bool _apEnabled = false;
  String _apIp = '';
  bool _loading = true;
  bool _scanning = false;

  @override
  void initState() {
    super.initState();
    _loadWifiList();
  }

  Future<void> _loadWifiList({bool showSpinner = true}) async {
    final svc = context.read<TankService>();

    // Show the cached list instantly; only spin when we have nothing yet.
    if (showSpinner && _savedNetworks.isEmpty && svc.wifiCache == null) {
      setState(() => _loading = true);
    }
    final data = await svc.fetchWifiList();
    if (!mounted) return;
    // Cloud mode response wraps in {type: "wifi_list", data: {...}}
    final payload = data.containsKey('data')
        ? data['data'] as Map<String, dynamic>? ?? data
        : data;
    setState(() {
      _savedNetworks = (payload['networks'] as List<dynamic>? ?? [])
          .cast<Map<String, dynamic>>();
      _apClients = (payload['apClients'] as List<dynamic>? ?? [])
          .cast<Map<String, dynamic>>();
      _apEnabled = payload['apEnabled'] as bool? ?? false;
      _apIp = payload['apIP'] as String? ?? '';
      _loading = false;
    });
    // Pick up the background refresh a moment later (no spinner).
    if (showSpinner) {
      Future.delayed(const Duration(milliseconds: 2800), () {
        if (mounted) _loadWifiList(showSpinner: false);
      });
    }
  }

  Future<void> _scan() async {
    final svc = context.read<TankService>();

    setState(() => _scanning = true);
    final results = await svc.scanWifi();
    if (!mounted) return;
    // Filter out already-saved SSIDs
    final savedSsids = _savedNetworks.map((n) => n['ssid'] as String).toSet();
    setState(() {
      _scanResults = results.where((r) => !savedSsids.contains(r['ssid'])).toList();
      _scanning = false;
    });
  }

  Future<void> _addNetwork(String ssid) async {
    final password = await _showPasswordDialog(ssid);
    if (password == null || !mounted) return;

    final svc = context.read<TankService>();
    final ok = await svc.addWifi(ssid, password);
    if (ok && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Added "$ssid"')),
      );
      await _loadWifiList();
    }
  }

  Future<String?> _showPasswordDialog(String ssid) {
    final ctrl = TextEditingController();
    bool obscure = true;
    return showDialog<String>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, setDialogState) => AlertDialog(
          backgroundColor: cardBg(ctx),
          title: Text('Connect to $ssid', style: TextStyle(color: textColor(ctx), fontSize: 16)),
          content: TextField(
            controller: ctrl,
            obscureText: obscure,
            style: TextStyle(color: textColor(ctx)),
            decoration: InputDecoration(
              labelText: 'Password',
              labelStyle: TextStyle(color: labelColor(ctx)),
              suffixIcon: IconButton(
                icon: Icon(obscure ? Icons.visibility_off : Icons.visibility,
                    color: labelColor(ctx), size: 20),
                onPressed: () => setDialogState(() => obscure = !obscure),
              ),
            ),
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
            TextButton(
              onPressed: () => Navigator.pop(ctx, ctrl.text),
              child: const Text('Add'),
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _deleteNetwork(String ssid) async {
    final confirm = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: cardBg(ctx),
        title: Text('Remove "$ssid"?', style: TextStyle(color: textColor(ctx), fontSize: 16)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          TextButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: Text('Remove', style: TextStyle(color: accentRed(ctx))),
          ),
        ],
      ),
    );
    if (confirm != true) return;

    final svc = context.read<TankService>();
    final ok = await svc.deleteWifi(ssid);
    if (ok && mounted) await _loadWifiList();
  }

  Future<void> _changePriority(String ssid, int newPriority) async {
    final svc = context.read<TankService>();
    final ok = await svc.setWifiPriority(ssid, newPriority);
    if (ok && mounted) await _loadWifiList();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: scaffoldBg(context),
      appBar: AppBar(
        backgroundColor: cardBg(context),
        elevation: 0,
        leading: BackButton(color: labelColor(context)),
        title: Text('WiFi Management',
            style: TextStyle(color: textColor(context), fontSize: 16, fontWeight: FontWeight.w600)),
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              onRefresh: _loadWifiList,
              child: ListView(
                padding: const EdgeInsets.all(16),
                children: [
                  _buildCurrentConnection(),
                  const SizedBox(height: 20),
                  _buildSavedNetworks(),
                  const SizedBox(height: 20),
                  _buildScanSection(),
                  const SizedBox(height: 20),
                  _buildApSection(),
                ],
              ),
            ),
    );
  }

  Widget _buildCurrentConnection() {
    final connected = _savedNetworks.where((n) => n['connected'] == true).toList();
    if (connected.isEmpty) {
      return _section(
        'CURRENT CONNECTION',
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Text('Not connected to any WiFi',
              style: TextStyle(color: labelColor(context), fontSize: 13)),
        ),
      );
    }
    final net = connected.first;
    return _section(
      'CURRENT CONNECTION',
      child: ListTile(
        leading: Icon(Icons.wifi, color: accentGreen(context), size: 28),
        title: Text(net['ssid'] as String? ?? '',
            style: TextStyle(color: textColor(context), fontWeight: FontWeight.w600)),
        subtitle: Text('${net['ip'] ?? ''} • Connected',
            style: TextStyle(color: accentGreen(context), fontSize: 12)),
      ),
    );
  }

  Widget _buildSavedNetworks() {
    return _section(
      'SAVED NETWORKS',
      child: _savedNetworks.isEmpty
          ? Padding(
              padding: const EdgeInsets.all(12),
              child: Text('No saved networks',
                  style: TextStyle(color: labelColor(context), fontSize: 13)),
            )
          : Column(
              children: List.generate(_savedNetworks.length, (i) {
                final net = _savedNetworks[i];
                final ssid = net['ssid'] as String? ?? '';
                final isConn = net['connected'] as bool? ?? false;
                final priority = net['priority'] as int? ?? (i + 1);
                return ListTile(
                  dense: true,
                  leading: CircleAvatar(
                    radius: 14,
                    backgroundColor: accentBlue(context).withValues(alpha: 0.2),
                    child: Text('$priority',
                        style: TextStyle(color: accentBlue(context), fontSize: 12, fontWeight: FontWeight.bold)),
                  ),
                  title: Text(ssid, style: TextStyle(
                    color: textColor(context),
                    fontWeight: isConn ? FontWeight.w600 : FontWeight.normal,
                  )),
                  subtitle: isConn
                      ? Text('Connected', style: TextStyle(color: accentGreen(context), fontSize: 11))
                      : null,
                  trailing: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      if (i > 0)
                        IconButton(
                          icon: Icon(Icons.arrow_upward, color: labelColor(context), size: 18),
                          onPressed: () => _changePriority(ssid, priority - 1),
                          tooltip: 'Move up',
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints(minWidth: 32),
                        ),
                      if (i < _savedNetworks.length - 1)
                        IconButton(
                          icon: Icon(Icons.arrow_downward, color: labelColor(context), size: 18),
                          onPressed: () => _changePriority(ssid, priority + 1),
                          tooltip: 'Move down',
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints(minWidth: 32),
                        ),
                      IconButton(
                        icon: Icon(Icons.delete_outline,
                            color: (isConn || _savedNetworks.length <= 1) ? labelColor(context).withValues(alpha: 0.35) : accentRed(context),
                            size: 18),
                        onPressed: (isConn || _savedNetworks.length <= 1) ? null : () => _deleteNetwork(ssid),
                        tooltip: isConn
                            ? "Can't remove — currently providing internet access"
                            : (_savedNetworks.length <= 1 ? "Can't remove the last saved network" : null),
                        padding: EdgeInsets.zero,
                        constraints: const BoxConstraints(minWidth: 32),
                      ),
                    ],
                  ),
                );
              }),
            ),
    );
  }

  Widget _buildScanSection() {
    return _section(
      'AVAILABLE NETWORKS',
      trailing: _scanning
          ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
          : IconButton(
              icon: Icon(Icons.refresh, color: accentBlue(context), size: 20),
              onPressed: _scan,
              tooltip: 'Scan',
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints(minWidth: 32),
            ),
      child: _scanResults.isEmpty
          ? Padding(
              padding: const EdgeInsets.all(12),
              child: Text(
                _scanning ? 'Scanning...' : 'Tap refresh to scan for networks',
                style: TextStyle(color: labelColor(context), fontSize: 13),
              ),
            )
          : Column(
              children: _scanResults.map((net) {
                final ssid = net['ssid'] as String? ?? '';
                final rssi = net['rssi'] as int? ?? -100;
                final open = net['open'] as bool? ?? false;
                return ListTile(
                  dense: true,
                  leading: Icon(
                    rssi > -50 ? Icons.wifi : rssi > -70 ? Icons.wifi_2_bar : Icons.wifi_1_bar,
                    color: labelColor(context),
                    size: 20,
                  ),
                  title: Text(ssid, style: TextStyle(color: textColor(context))),
                  subtitle: Text('${rssi} dBm', style: TextStyle(color: labelColor(context), fontSize: 11)),
                  trailing: Icon(open ? Icons.lock_open : Icons.lock,
                      color: labelColor(context), size: 16),
                  onTap: () => _addNetwork(ssid),
                );
              }).toList(),
            ),
    );
  }

  Widget _buildApSection() {
    return _section(
      'AP HOTSPOT',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          ListTile(
            dense: true,
            leading: Icon(Icons.router, color: accentOrange(context), size: 22),
            title: Text('TankMonitor', style: TextStyle(color: textColor(context))),
            subtitle: Text(
              _apEnabled ? 'IP: $_apIp • Active' : 'Inactive',
              style: TextStyle(color: _apEnabled ? accentGreen(context) : labelColor(context), fontSize: 12),
            ),
          ),
          if (_apClients.isNotEmpty) ...[
            Padding(
              padding: const EdgeInsets.only(left: 16, bottom: 4),
              child: Text('Connected clients (${_apClients.length})',
                  style: TextStyle(color: labelColor(context), fontSize: 12, fontWeight: FontWeight.w600)),
            ),
            ..._apClients.map((c) => ListTile(
                  dense: true,
                  contentPadding: const EdgeInsets.only(left: 24),
                  leading: Icon(Icons.devices, color: labelColor(context), size: 18),
                  title: Text(c['mac'] as String? ?? '', style: TextStyle(color: textColor(context), fontSize: 13)),
                  subtitle: Text(c['ip'] as String? ?? '', style: TextStyle(color: labelColor(context), fontSize: 11)),
                )),
          ] else if (_apEnabled)
            Padding(
              padding: const EdgeInsets.only(left: 16, bottom: 8),
              child: Text('No clients connected',
                  style: TextStyle(color: labelColor(context), fontSize: 12)),
            ),
        ],
      ),
    );
  }

  Widget _section(String title, {required Widget child, Widget? trailing}) {
    return Container(
      decoration: BoxDecoration(
        color: cardBg(context),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: cardBd(context)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 8, 4),
            child: Row(
              children: [
                Text(title, style: TextStyle(
                  color: labelColor(context), fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 0.8)),
                const Spacer(),
                if (trailing != null) trailing,
              ],
            ),
          ),
          child,
          const SizedBox(height: 4),
        ],
      ),
    );
  }
}
