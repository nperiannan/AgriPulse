import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'models.dart';
import 'tank_service.dart';
import 'login_screen.dart';
import 'setup_screen.dart';
import 'dashboard_screen.dart';
import 'claim_screen.dart';
import 'admin_screen.dart';
import 'theme_data.dart';

class DeviceListScreen extends StatefulWidget {
  /// When [autoNavigate] is true (default), a single device skips straight
  /// to the dashboard on load — useful for the initial app launch flow.
  /// Set to false when navigating here intentionally (e.g. "Switch device")
  /// so the user can see and manage their device cards.
  const DeviceListScreen({super.key, this.autoNavigate = true});

  final bool autoNavigate;

  @override
  State<DeviceListScreen> createState() => _DeviceListScreenState();
}

class _DeviceListScreenState extends State<DeviceListScreen> {
  List<Device> _devices = [];
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() => _loading = true);
    final svc = context.read<TankService>();
    final devices = await svc.listDevices();
    if (!mounted) return;

    // Auto-navigate when there's exactly one device — only on initial launch
    if (widget.autoNavigate && devices.length == 1) {
      await svc.connectToDevice(devices.first);
      if (mounted) {
        Navigator.of(context).pushReplacement(
          MaterialPageRoute(builder: (_) => const DashboardScreen()),
        );
      }
      return;
    }
    setState(() { _devices = devices; _loading = false; });
  }

  Future<void> _selectDevice(Device d) async {
    final svc = context.read<TankService>();
    await svc.connectToDevice(d);
    if (!mounted) return;
    Navigator.of(context).pushReplacement(
      MaterialPageRoute(builder: (_) => const DashboardScreen()),
    );
  }

  Future<void> _rename(Device d) async {
    final nameCtrl = TextEditingController(text: d.displayName);
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        backgroundColor: cardBg(context),
        title: Text('Rename device', style: TextStyle(color: textColor(context))),
        content: TextField(
          controller: nameCtrl,
          autofocus: true,
          decoration: InputDecoration(
            hintText: 'Display name',
            hintStyle: TextStyle(color: labelColor(context)),
            filled: true, fillColor: subtleBg(context),
            border: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
                borderSide: BorderSide(color: cardBd(context))),
            enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
                borderSide: BorderSide(color: cardBd(context))),
            focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8),
                borderSide: BorderSide(color: accentBlue(context), width: 2)),
            contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
          ),
          style: TextStyle(color: textColor(context)),
          onSubmitted: (_) => Navigator.pop(context, true),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          TextButton(
            onPressed: () => Navigator.pop(context, true),
            child: Text('Rename', style: TextStyle(color: accentBlue(context))),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    final newName = nameCtrl.text.trim();
    if (newName.isEmpty || newName == d.displayName) return;
    final svc = context.read<TankService>();
    final ok = await svc.renameDevice(d.mac, newName);
    if (!mounted) return;
    if (ok) {
      _load();
    } else {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(svc.error ?? 'Rename failed')),
      );
    }
  }

  Future<void> _unclaim(Device d) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        backgroundColor: cardBg(context),
        title: Text('Remove device?', style: TextStyle(color: textColor(context))),
        content: Text(
          'Remove "${d.displayName}" from your account?',
          style: TextStyle(color: labelColor(context), fontSize: 13),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          TextButton(
            onPressed: () => Navigator.pop(context, true),
            child: Text('Remove', style: TextStyle(color: accentRed(context))),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    final svc = context.read<TankService>();
    final ok = await svc.unclaimDevice(d.mac);
    if (!mounted) return;
    if (ok) {
      _load();
    } else {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(svc.error ?? 'Remove failed')),
      );
    }
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

  @override
  Widget build(BuildContext context) {
    final svc = context.watch<TankService>();
    if (svc.unauthorized) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        Navigator.of(context).pushAndRemoveUntil(
          MaterialPageRoute(builder: (_) => const LoginScreen()),
          (_) => false,
        );
      });
    }
    return Scaffold(
      backgroundColor: scaffoldBg(context),
      appBar: AppBar(
        backgroundColor: cardBg(context),
        elevation: 0,
        automaticallyImplyLeading: false,
        title: Text('💧 My Devices',
            style: TextStyle(color: accentBlue(context), fontWeight: FontWeight.w700, fontSize: 18)),
        actions: [
          if (svc.isAdmin)
            IconButton(
              icon: Icon(Icons.admin_panel_settings, color: accentOrange(context), size: 22),
              tooltip: 'Admin: Users & Devices',
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute(builder: (_) => const AdminScreen()),
              ),
            ),
          IconButton(
            icon: Icon(Icons.settings_ethernet, color: labelColor(context), size: 20),
            tooltip: 'Server settings',
            onPressed: () => Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const SetupScreen()),
            ),
          ),
          IconButton(
            icon: Icon(Icons.logout, color: labelColor(context), size: 20),
            tooltip: 'Sign out',
            onPressed: _logout,
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton(
        backgroundColor: accentBlue(context),
        tooltip: 'Claim device',
        onPressed: () async {
          await Navigator.of(context).push(
            MaterialPageRoute(builder: (_) => const ClaimScreen()),
          );
          _load(); // refresh after returning
        },
        child: Icon(Icons.add, color: textColor(context)),
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _devices.isEmpty
              ? _emptyState()
              : RefreshIndicator(
                  onRefresh: _load,
                  child: ListView.separated(
                    padding: const EdgeInsets.all(12),
                    itemCount: _devices.length,
                    separatorBuilder: (_, __) => const SizedBox(height: 8),
                    itemBuilder: (_, i) => _deviceCard(_devices[i]),
                  ),
                ),
    );
  }

  Widget _emptyState() {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.devices_other, size: 64, color: labelColor(context)),
          const SizedBox(height: 16),
          Text('No devices yet',
              style: TextStyle(color: textColor(context), fontSize: 18, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          Text('Tap + to claim your first device',
              style: TextStyle(color: labelColor(context), fontSize: 13)),
        ],
      ),
    );
  }

  Widget _deviceCard(Device d) {
    final (typeIcon, typeLabel) = _deviceTypeInfo(d.typeId);
    return GestureDetector(
      onTap: () => _selectDevice(d),
      onLongPress: () => _showDeviceOptions(d),
      child: Container(
        padding: const EdgeInsets.all(14),
        decoration: BoxDecoration(
          color: cardBg(context),
          border: Border.all(color: cardBd(context)),
          borderRadius: BorderRadius.circular(12),
        ),
        child: Row(
          children: [
            Icon(typeIcon, color: d.online ? accentGreen(context) : labelColor(context), size: 36),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          d.displayName.isNotEmpty ? d.displayName : d.mac,
                          style: TextStyle(
                              color: textColor(context), fontWeight: FontWeight.w600, fontSize: 15),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                      const SizedBox(width: 8),
                      Container(
                        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                        decoration: BoxDecoration(
                          color: d.online
                              ? accentGreen(context).withValues(alpha: 0.12)
                              : subtleBg(context),
                          borderRadius: BorderRadius.circular(10),
                          border: Border.all(
                              color: d.online
                                  ? accentGreen(context).withValues(alpha: 0.4)
                                  : cardBd(context)),
                        ),
                        child: Text(
                          d.online ? '● Online' : '○ Offline',
                          style: TextStyle(
                              color: d.online ? accentGreen(context) : labelColor(context), fontSize: 11),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 3),
                  Text(typeLabel,
                      style: TextStyle(color: accentBlue(context), fontSize: 12, fontWeight: FontWeight.w500)),
                  const SizedBox(height: 2),
                  Text(d.mac,
                      style: TextStyle(
                          color: labelColor(context), fontSize: 11, fontFamily: 'monospace')),
                  if (d.fwVersion.isNotEmpty)
                    Text('FW ${d.fwVersion}',
                        style: TextStyle(color: labelColor(context), fontSize: 11)),
                ],
              ),
            ),
            Icon(Icons.chevron_right, color: labelColor(context), size: 20),
          ],
        ),
      ),
    );
  }

  /// Returns (icon, label) for the given device type ID.
  (IconData, String) _deviceTypeInfo(String typeId) {
    switch (typeId) {
      case 'tank_monitor': return (Icons.water_drop_outlined, 'Tank Monitor');
      case 'smart_ps':     return (Icons.bolt_outlined,        'Smart PS');
      default:             return (Icons.memory,               typeId.isNotEmpty ? typeId : 'Unknown');
    }
  }

  void _showDeviceOptions(Device d) {
    showModalBottomSheet(
      context: context,
      backgroundColor: cardBg(context),
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
      ),
      builder: (_) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 16, horizontal: 20),
              child: Text(
                d.displayName.isNotEmpty ? d.displayName : d.mac,
                style: TextStyle(
                    color: textColor(context), fontWeight: FontWeight.w600, fontSize: 16),
              ),
            ),
            Divider(color: cardBd(context), height: 1),
            ListTile(
              leading: Icon(Icons.drive_file_rename_outline, color: textColor(context)),
              title: Text('Rename', style: TextStyle(color: textColor(context))),
              onTap: () { Navigator.pop(context); _rename(d); },
            ),
            ListTile(
              leading: Icon(Icons.link_off, color: accentRed(context)),
              title: Text('Remove from account', style: TextStyle(color: accentRed(context))),
              onTap: () { Navigator.pop(context); _unclaim(d); },
            ),
            const SizedBox(height: 8),
          ],
        ),
      ),
    );
  }
}
