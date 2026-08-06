import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'theme_data.dart';

class AdminScreen extends StatefulWidget {
  const AdminScreen({super.key});

  @override
  State<AdminScreen> createState() => _AdminScreenState();
}

class _AdminScreenState extends State<AdminScreen> {
  List<Map<String, dynamic>> _users = [];
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() => _loading = true);
    final svc = context.read<TankService>();
    final users = await svc.adminListUsers();
    if (!mounted) return;
    setState(() { _users = users; _loading = false; });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: scaffoldBg(context),
      appBar: AppBar(
        backgroundColor: cardBg(context),
        elevation: 0,
        title: Text(
          '👥 Users & Devices',
          style: TextStyle(color: accentBlue(context), fontWeight: FontWeight.w700, fontSize: 18),
        ),
        actions: [
          IconButton(
            icon: Icon(Icons.refresh, color: labelColor(context), size: 20),
            tooltip: 'Refresh',
            onPressed: _load,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _users.isEmpty
              ? Center(
                  child: Text('No users found',
                      style: TextStyle(color: labelColor(context), fontSize: 15)))
              : RefreshIndicator(
                  onRefresh: _load,
                  child: ListView.separated(
                    padding: const EdgeInsets.all(12),
                    itemCount: _users.length,
                    separatorBuilder: (_, __) => const SizedBox(height: 8),
                    itemBuilder: (_, i) => _userCard(_users[i]),
                  ),
                ),
    );
  }

  Widget _userCard(Map<String, dynamic> user) {
    final username = user['username'] as String? ?? '—';
    final isAdmin  = user['is_admin'] as bool? ?? false;
    final devices  = (user['devices'] as List<dynamic>? ?? [])
        .cast<Map<String, dynamic>>();

    return Container(
      decoration: BoxDecoration(
        color: cardBg(context),
        border: Border.all(color: cardBd(context)),
        borderRadius: BorderRadius.circular(12),
      ),
      child: ExpansionTile(
        tilePadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 4),
        collapsedBackgroundColor: Colors.transparent,
        backgroundColor: Colors.transparent,
        iconColor: labelColor(context),
        collapsedIconColor: labelColor(context),
        leading: CircleAvatar(
          backgroundColor: isAdmin
              ? const Color(0xFF391085)
              : const Color(0xFF003a8c),
          radius: 18,
          child: Text(
            username.isNotEmpty ? username[0].toUpperCase() : '?',
            style: TextStyle(
                color: textColor(context), fontWeight: FontWeight.w700, fontSize: 15),
          ),
        ),
        title: Row(
          children: [
            Expanded(
              child: Text(
                username,
                style: TextStyle(
                    color: textColor(context),
                    fontWeight: FontWeight.w600,
                    fontSize: 15),
                overflow: TextOverflow.ellipsis,
              ),
            ),
            if (isAdmin) ...[
              const SizedBox(width: 6),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
                decoration: BoxDecoration(
                  color: const Color(0xFF391085),
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: const Color(0xFF531DAB)),
                ),
                child: Text('admin',
                    style: TextStyle(color: accentOrange(context), fontSize: 10)),
              ),
            ],
          ],
        ),
        subtitle: Padding(
          padding: const EdgeInsets.only(top: 2),
          child: Text(
            devices.isEmpty
                ? 'No devices claimed'
                : '${devices.length} device${devices.length == 1 ? '' : 's'}',
            style: TextStyle(color: labelColor(context), fontSize: 12),
          ),
        ),
        children: devices.isEmpty
            ? [
                Padding(
                  padding: const EdgeInsets.fromLTRB(14, 0, 14, 12),
                  child: Text('No devices claimed',
                      style: TextStyle(color: labelColor(context), fontSize: 13)),
                ),
              ]
            : devices.map((d) => _deviceRow(d)).toList(),
      ),
    );
  }

  Widget _deviceRow(Map<String, dynamic> d) {
    final name      = (d['display_name'] as String? ?? '').isNotEmpty
        ? d['display_name'] as String
        : d['mac'] as String? ?? '—';
    final mac       = d['mac'] as String? ?? '—';
    final fw        = d['fw_version'] as String? ?? '';
    final online    = d['online'] as bool? ?? false;

    return Container(
      margin: const EdgeInsets.fromLTRB(14, 0, 14, 10),
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: scaffoldBg(context),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: cardBd(context)),
      ),
      child: Row(
        children: [
          Icon(Icons.memory, color: online ? accentGreen(context) : labelColor(context), size: 28),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Expanded(
                      child: Text(
                        name,
                        style: TextStyle(
                            color: textColor(context),
                            fontWeight: FontWeight.w500,
                            fontSize: 13),
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    const SizedBox(width: 6),
                    Container(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 6, vertical: 1),
                      decoration: BoxDecoration(
                        color: online
                            ? accentGreen(context).withValues(alpha: 0.12)
                            : subtleBg(context),
                        borderRadius: BorderRadius.circular(8),
                        border: Border.all(
                            color: online
                                ? accentGreen(context).withValues(alpha: 0.4)
                                : cardBd(context)),
                      ),
                      child: Text(
                        online ? '● Online' : '○ Offline',
                        style: TextStyle(
                            color: online ? accentGreen(context) : labelColor(context), fontSize: 10),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 3),
                Text(mac,
                    style: TextStyle(
                        color: labelColor(context),
                        fontSize: 10,
                        fontFamily: 'monospace')),
                if (fw.isNotEmpty)
                  Text('FW $fw',
                      style: TextStyle(color: labelColor(context), fontSize: 10)),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
