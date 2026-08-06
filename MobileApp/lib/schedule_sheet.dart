import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'models.dart';
import 'tank_service.dart';
import 'theme_data.dart';

class ScheduleSheet extends StatefulWidget {
  final TankService svc;
  /// If non-null, the sheet is in edit mode for this schedule.
  final Schedule? editSchedule;
  const ScheduleSheet({super.key, required this.svc, this.editSchedule});

  @override
  State<ScheduleSheet> createState() => _ScheduleSheetState();
}

class _ScheduleSheetState extends State<ScheduleSheet> {
  late int    _motor;
  late TimeOfDay _time;
  late final TextEditingController _durCtrl;
  bool _submitting = false;

  @override
  void initState() {
    super.initState();
    final e = widget.editSchedule;
    _motor = e != null ? (e.m == 'OH' ? 0 : 1) : 0;
    if (e != null) {
      final parts = e.t.split(':');
      _time = TimeOfDay(hour: int.parse(parts[0]), minute: int.parse(parts[1]));
    } else {
      _time = TimeOfDay.now();
    }
    _durCtrl = TextEditingController(text: e != null ? '${e.d}' : '30');
  }

  @override
  void dispose() {
    _durCtrl.dispose();
    super.dispose();
  }

  bool get _isEdit => widget.editSchedule != null;

  Future<void> _pickTime() async {
    final picked = await showTimePicker(
      context: context,
      initialTime: _time,
      builder: (ctx, child) => Theme(
        data: ThemeData.dark().copyWith(
          colorScheme: const ColorScheme.dark(primary: kBlue),
        ),
        child: child!,
      ),
    );
    if (picked != null) setState(() => _time = picked);
  }

  Future<void> _submit() async {
    final dur = int.tryParse(_durCtrl.text) ?? 0;
    if (dur < 1 || dur > 480) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Duration must be 1–480 minutes')));
      return;
    }
    setState(() => _submitting = true);
    final hh = _time.hour.toString().padLeft(2, '0');
    final mm = _time.minute.toString().padLeft(2, '0');

    if (_isEdit) {
      // Delete old schedule then re-add with new values
      await widget.svc.sendControl({'cmd': 'sched_remove', 'index': widget.editSchedule!.i});
    }
    await widget.svc.sendControl({
      'cmd': 'sched_add',
      'motor': _motor,
      'time': '$hh:$mm',
      'duration': dur,
    });
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final bottom = MediaQuery.of(context).viewInsets.bottom;
    return Padding(
      padding: EdgeInsets.fromLTRB(20, 20, 20, 20 + bottom),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Handle
          Center(child: Container(
            width: 36, height: 4,
            decoration: BoxDecoration(
              color: const Color(0xFF434343),
              borderRadius: BorderRadius.circular(2)),
          )),
          const SizedBox(height: 16),
          Text(_isEdit ? 'Edit Schedule' : 'Add Schedule',
            style: TextStyle(color: textColor(context), fontSize: 17, fontWeight: FontWeight.w700)),
          const SizedBox(height: 20),

          // Motor selector
          Text('Motor', style: TextStyle(color: labelColor(context), fontSize: 12)),
          const SizedBox(height: 6),
          Row(children: [
            _MotorChip(label: 'OH — Overhead',   value: 0, group: _motor, onTap: () => setState(() => _motor = 0)),
            const SizedBox(width: 10),
            _MotorChip(label: 'UG — Underground', value: 1, group: _motor, onTap: () => setState(() => _motor = 1)),
          ]),
          const SizedBox(height: 16),

          // Time picker
          Text('Start Time', style: TextStyle(color: labelColor(context), fontSize: 12)),
          const SizedBox(height: 6),
          GestureDetector(
            onTap: _pickTime,
            child: Container(
              width: double.infinity,
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
              decoration: BoxDecoration(
                color: scaffoldBg(context),
                border: Border.all(color: cardBd(context)),
                borderRadius: BorderRadius.circular(8),
              ),
              child: Text(
                _tod12hr(_time),
                style: TextStyle(color: textColor(context), fontSize: 15),
              ),
            ),
          ),
          const SizedBox(height: 16),

          // Duration
          Text('Duration (minutes)', style: TextStyle(color: labelColor(context), fontSize: 12)),
          const SizedBox(height: 6),
          TextField(
            controller: _durCtrl,
            style: TextStyle(color: textColor(context)),
            keyboardType: TextInputType.number,
            inputFormatters: [FilteringTextInputFormatter.digitsOnly],
            decoration: InputDecoration(
              filled: true,
              fillColor: scaffoldBg(context),
              suffixText: 'min',
              suffixStyle: TextStyle(color: labelColor(context)),
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
                borderSide: BorderSide(color: accentBlue(context)),
              ),
            ),
          ),
          const SizedBox(height: 24),

          SizedBox(
            width: double.infinity,
            height: 46,
            child: ElevatedButton(
              onPressed: _submitting ? null : _submit,
              style: ElevatedButton.styleFrom(
                backgroundColor: accentBlue(context), foregroundColor: textColor(context),
                shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
              ),
              child: _submitting
                  ? const SizedBox(width: 20, height: 20,
                      child: CircularProgressIndicator(color: Colors.white, strokeWidth: 2))
                  : Text(_isEdit ? 'Save Changes' : 'Add Schedule',
                      style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
            ),
          ),
        ],
      ),
    );
  }
}

class _MotorChip extends StatelessWidget {
  final String label;
  final int value;
  final int group;
  final VoidCallback onTap;
  const _MotorChip({required this.label, required this.value, required this.group, required this.onTap});

  @override
  Widget build(BuildContext context) {
    final selected = value == group;
    return Expanded(
      child: GestureDetector(
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(vertical: 10),
          decoration: BoxDecoration(
            color: selected ? kBlue.withOpacity(0.15) : scaffoldBg(context),
            border: Border.all(color: selected ? kBlue : kDarkCardBd),
            borderRadius: BorderRadius.circular(8),
          ),
          child: Center(child: Text(label,
            style: TextStyle(
              color: selected ? kBlue : kDarkLabel,
              fontSize: 12, fontWeight: FontWeight.w600))),
        ),
      ),
    );
  }
}

/// Converts [TimeOfDay] to "H:MM AM/PM" for display.
String _tod12hr(TimeOfDay t) {
  final period = t.hour >= 12 ? 'PM' : 'AM';
  final h = t.hour % 12 == 0 ? 12 : t.hour % 12;
  final m = t.minute.toString().padLeft(2, '0');
  return '$h:$m $period';
}
