import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'tank_service.dart';
import 'models.dart';
import 'theme_data.dart';
import 'app_preferences.dart';

// Accent colours are theme-independent (they read well on light & dark).
const _purple = Color(0xFF7C4DFF);
const _runFg = Color(0xFF08130A);

/// How many days back history can be selected — 2 years.
const _kMaxHistoryDays = 730;

/// A single motor run built from an ON event and its following OFF (or null if
/// the motor is still running).
class _Run {
  final String motor; // 'OH' | 'UG'
  final Map<String, dynamic> on;
  final Map<String, dynamic>? off;
  _Run({required this.motor, required this.on, this.off});
  int get onTs => on['ts'] as int? ?? 0;
  int? get offTs => off?['ts'] as int?;
  bool get running => off == null;
  String get reason => (on['rsnStr'] as String?) ?? '';
  int durSec(int nowTs) {
    final end = offTs ?? nowTs;
    final d = end - onTs;
    return d < 0 ? 0 : d;
  }

  /// Duration clipped to [periodFrom, periodTo) — used for period-scoped
  /// stats/water-volume so a run that started before (or is still running
  /// past) the selected window only counts the portion actually inside it.
  int clippedDurSec(int nowTs, int periodFrom, int periodTo) {
    final start = onTs < periodFrom ? periodFrom : onTs;
    final rawEnd = offTs ?? nowTs;
    final end = rawEnd > periodTo ? periodTo : rawEnd;
    final d = end - start;
    return d < 0 ? 0 : d;
  }
}

const _months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/// Selectable history time periods.
enum HistoryPeriod { today, week, month, year }

class EventHistoryScreen extends StatefulWidget {
  final bool initialGraph;
  const EventHistoryScreen({super.key, this.initialGraph = false});

  @override
  State<EventHistoryScreen> createState() => _EventHistoryScreenState();
}

class _EventHistoryScreenState extends State<EventHistoryScreen> {
  List<Map<String, dynamic>> _records = [];
  int _totalCount = 0;
  bool _loading = true;
  bool _showGraph = false;

  HistoryPeriod _period = HistoryPeriod.today;
  DateTime _anchor = DateTime.now(); // normalized per-period (see _normalize)

  // Theme-aware colour accessors (match the rest of the app, follow light/dark).
  Color get _bg => scaffoldBg(context);
  Color get _card => cardBg(context);
  Color get _card2 => subtleBg(context);
  Color get _bd => cardBd(context);
  Color get _txt => textColor(context);
  Color get _lbl => labelColor(context);
  Color get _green => accentGreen(context);
  Color get _blue => accentBlue(context);
  Color get _orange => kOrange;
  Color get _red => accentRed(context);

  @override
  void initState() {
    super.initState();
    _showGraph = widget.initialGraph;
    _load();
  }

  // ── Period range helpers ──────────────────────────────────────────────────

  DateTime get _earliestAllowed =>
      DateTime.now().subtract(const Duration(days: _kMaxHistoryDays));

  DateTime _normalize(DateTime d, HistoryPeriod p) {
    switch (p) {
      case HistoryPeriod.today:
      case HistoryPeriod.week:
      case HistoryPeriod.month:
      case HistoryPeriod.year:
        return DateTime(d.year, d.month, d.day);
    }
  }

  /// The selected window as [from, to) epoch-seconds, plus a little lead-in
  /// buffer on the fetch so a run that started just before the window (and
  /// ends inside it) can still be paired up correctly (see _runs()).
  ({DateTime start, DateTime end}) _periodBounds() {
    final now = DateTime.now();
    DateTime start, end;
    switch (_period) {
      // Today behaves like a single-day window anchored on `_anchor` (which
      // defaults to today) so it can be paged backward/forward one day at a
      // time — when the anchor IS today, the end is capped to "now" below so
      // it stays a live view; a past anchor gets the full historical day.
      case HistoryPeriod.today:
        start = DateTime(_anchor.year, _anchor.month, _anchor.day);
        end = DateTime(_anchor.year, _anchor.month, _anchor.day + 1);
        break;
      // Week/Month/Year are rolling windows ending on (and including) the
      // anchor date — e.g. Week = the 7 days up to and including the anchor —
      // rather than a calendar-aligned Mon–Sun/1st–31st/Jan–Dec window.
      case HistoryPeriod.week:
        start = DateTime(_anchor.year, _anchor.month, _anchor.day - 6);
        end = DateTime(_anchor.year, _anchor.month, _anchor.day + 1);
        break;
      case HistoryPeriod.month:
        start = DateTime(_anchor.year, _anchor.month - 1, _anchor.day);
        end = DateTime(_anchor.year, _anchor.month, _anchor.day + 1);
        break;
      case HistoryPeriod.year:
        start = DateTime(_anchor.year - 1, _anchor.month, _anchor.day);
        end = DateTime(_anchor.year, _anchor.month, _anchor.day + 1);
        break;
    }
    if (end.isAfter(now)) end = now;
    return (start: start, end: end);
  }

  int _epoch(DateTime d) => d.millisecondsSinceEpoch ~/ 1000;

  ({DateTime start, DateTime end}) _periodBoundsFor(HistoryPeriod p, DateTime anchor) {
    final saved = _anchor;
    final savedP = _period;
    _anchor = anchor;
    _period = p;
    final b = _periodBounds();
    _anchor = saved;
    _period = savedP;
    return b;
  }

  bool get _canGoBack {
    final prevAnchor = _normalize(_shiftAnchorFor(_period, _anchor, -1), _period);
    final prevBounds = _periodBoundsFor(_period, prevAnchor);
    return !prevBounds.start.isBefore(_earliestAllowed);
  }

  bool get _canGoForward => _periodBounds().end.isBefore(DateTime.now());

  /// True when the current anchor date is today (vs. a past day reached via
  /// the arrows or the date picker while still on the "Today" chip).
  bool get _anchorIsToday {
    final now = DateTime.now();
    return _anchor.year == now.year && _anchor.month == now.month && _anchor.day == now.day;
  }

  void _shiftPeriod(int dir) {
    final next = _normalize(_shiftAnchorFor(_period, _anchor, dir), _period);
    final bounds = _periodBoundsFor(_period, next);
    if (dir < 0 && bounds.start.isBefore(_earliestAllowed)) return; // hit 2-yr limit
    if (dir > 0 && bounds.start.isAfter(DateTime.now())) return; // don't go to the future
    setState(() => _anchor = next);
    _load();
  }

  /// Jump straight to a chosen date via the native calendar picker —
  /// available for every period (including Today) as an alternative to
  /// tapping the arrows repeatedly.
  Future<void> _pickDate() async {
    final picked = await showDatePicker(
      context: context,
      initialDate: _anchor,
      firstDate: _earliestAllowed,
      lastDate: DateTime.now(),
    );
    if (picked == null) return;
    setState(() => _anchor = _normalize(picked, _period));
    _load();
  }

  // Each arrow tap pages the whole rolling window by exactly one window
  // size — 1 day for Today, 7 days for Week, 1 month for Month, 1 year for Year.
  DateTime _shiftAnchorFor(HistoryPeriod p, DateTime anchor, int dir) {
    switch (p) {
      case HistoryPeriod.today:
        return anchor.add(Duration(days: dir));
      case HistoryPeriod.week:
        return anchor.add(Duration(days: 7 * dir));
      case HistoryPeriod.month:
        return DateTime(anchor.year, anchor.month + dir, anchor.day);
      case HistoryPeriod.year:
        return DateTime(anchor.year + dir, anchor.month, anchor.day);
    }
  }

  String _rangeLabel() {
    final b = _periodBounds();
    switch (_period) {
      case HistoryPeriod.today:
        if (_anchorIsToday) return 'Today';
        return '${_anchor.day} ${_months[_anchor.month - 1]} ${_anchor.year}';
      case HistoryPeriod.week:
      case HistoryPeriod.month:
      case HistoryPeriod.year:
        final endIncl = DateTime(_anchor.year, _anchor.month, _anchor.day);
        final sameMonth = b.start.month == endIncl.month && b.start.year == endIncl.year;
        final sameYear = b.start.year == endIncl.year;
        final startStr = sameMonth
            ? '${b.start.day}'
            : sameYear
                ? '${b.start.day} ${_months[b.start.month - 1]}'
                : '${b.start.day} ${_months[b.start.month - 1]} ${b.start.year}';
        return '$startStr – ${endIncl.day} ${_months[endIncl.month - 1]} ${endIncl.year}';
    }
  }

  String _periodChipLabel(HistoryPeriod p) {
    switch (p) {
      case HistoryPeriod.today: return 'Today';
      case HistoryPeriod.week: return 'Week';
      case HistoryPeriod.month: return 'Month';
      case HistoryPeriod.year: return 'Year';
    }
  }

  String _runsCountLabel() {
    switch (_period) {
      case HistoryPeriod.today: return _anchorIsToday ? 'RUNS TODAY' : 'RUNS THAT DAY';
      case HistoryPeriod.week: return 'RUNS · 7 DAYS';
      case HistoryPeriod.month: return 'RUNS · 1 MONTH';
      case HistoryPeriod.year: return 'RUNS · 1 YEAR';
    }
  }

  // ── Data loading ──────────────────────────────────────────────────────────

  Future<void> _load({bool showSpinner = true}) async {
    final svc = context.read<TankService>();
    if (showSpinner) setState(() => _loading = true);
    final bounds = _periodBounds();
    // Fetch a day of lead-in before the window so a run that started just
    // before it (and ends inside it) can still be paired with its ON event.
    final fetchFrom = bounds.start.subtract(const Duration(days: 1));
    final data = await svc.fetchHistory(
      triggerRefresh: showSpinner,
      from: _epoch(fetchFrom),
      to: _epoch(bounds.end),
    );
    if (!mounted) return;
    setState(() {
      _records = (data['records'] as List<dynamic>? ?? []).cast<Map<String, dynamic>>();
      _totalCount = data['count'] as int? ?? _records.length;
      _loading = false;
    });
    // Only auto-refresh while looking at a live "Today" view (anchor is
    // actually today); historical periods/days don't change once loaded.
    if (showSpinner && _period == HistoryPeriod.today && _anchorIsToday) {
      Future.delayed(const Duration(milliseconds: 2800), () {
        if (mounted && _period == HistoryPeriod.today && _anchorIsToday) _load(showSpinner: false);
      });
    }
  }

  Future<void> _clearHistory() async {
    final confirm = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: _card,
        title: Text('Clear all history?', style: TextStyle(color: _txt, fontSize: 16)),
        content: Text('This will erase all $_totalCount events.', style: TextStyle(color: _lbl, fontSize: 13)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: Text('Clear', style: TextStyle(color: _red))),
        ],
      ),
    );
    if (confirm != true) return;
    final svc = context.read<TankService>();
    await svc.clearHistory();
    if (mounted) await _load();
  }

  // ── Pair ON/OFF events into runs (newest-first), scoped to the period ────
  List<_Run> _runs() {
    // Sort ascending by timestamp explicitly rather than assuming a fixed
    // order from the source: the backend returns DESC (newest-first) when
    // no from/to is given, but ASC (oldest-first) when a date range is
    // requested (see web/backend handleDeviceHistory) — and direct mode's
    // order isn't guaranteed either. Blindly reversing here previously broke
    // ON/OFF pairing whenever the range-scoped ASC order was returned,
    // producing negative durations (shown as "0s"), swapped start/stop
    // times, and runs stuck showing as still "running".
    final chrono = List<Map<String, dynamic>>.from(_records)
      ..sort((a, b) => ((a['ts'] as int?) ?? 0).compareTo((b['ts'] as int?) ?? 0));
    Map<String, dynamic>? openOH, openUG;
    final runs = <_Run>[];
    for (final r in chrono) {
      switch (r['ev'] as String? ?? '') {
        case 'OH Motor ON':
          openOH = r;
          break;
        case 'OH Motor OFF':
          if (openOH != null) runs.add(_Run(motor: 'OH', on: openOH, off: r));
          openOH = null;
          break;
        case 'UG Motor ON':
          openUG = r;
          break;
        case 'UG Motor OFF':
          if (openUG != null) runs.add(_Run(motor: 'UG', on: openUG, off: r));
          openUG = null;
          break;
      }
    }
    if (openOH != null) runs.add(_Run(motor: 'OH', on: openOH, off: null));
    if (openUG != null) runs.add(_Run(motor: 'UG', on: openUG, off: null));

    // Keep only runs that overlap the selected period (the lead-in buffer
    // fetched in _load() lets a run starting just before the window, but
    // ending inside it, still show up — clipped for stats via clippedDurSec).
    final bounds = _periodBounds();
    final periodFrom = _epoch(bounds.start);
    final periodTo = _epoch(bounds.end);
    final nowTs = _nowTs();
    final filtered = runs.where((r) {
      final end = r.offTs ?? nowTs;
      return r.onTs < periodTo && end >= periodFrom;
    }).toList();

    filtered.sort((a, b) => b.onTs.compareTo(a.onTs)); // newest first
    return filtered;
  }

  int _nowTs() => DateTime.now().millisecondsSinceEpoch ~/ 1000;
  String _two(int n) => n.toString().padLeft(2, '0');
  String _hhmm(int ts) {
    final d = DateTime.fromMillisecondsSinceEpoch(ts * 1000).toLocal();
    final period = d.hour >= 12 ? 'PM' : 'AM';
    final h12 = d.hour % 12 == 0 ? 12 : d.hour % 12;
    return '$h12:${_two(d.minute)} $period';
  }

  String _durLong(int sec) {
    if (sec < 60) return '${sec}s';
    // Round (not truncate) to the nearest minute so e.g. a 118s run shows
    // "2m" — matching the minute-rounded HH:MM start/stop labels shown next
    // to it — instead of truncating down to "1m".
    final m = (sec + 30) ~/ 60, h = m ~/ 60, mm = m % 60;
    return h > 0 ? '${h}h ${_two(mm)}m' : '${m}m';
  }

  String _durShort(int sec) {
    if (sec < 60) return '${sec}s';
    final m = (sec + 30) ~/ 60, h = m ~/ 60, mm = m % 60;
    return h > 0 ? '${h}h ${mm}m' : '${m}m';
  }

  String _dayKey(int ts) {
    final d = DateTime.fromMillisecondsSinceEpoch(ts * 1000).toLocal();
    return '${d.year}-${d.month}-${d.day}';
  }

  String _dayHeader(int ts) {
    final d = DateTime.fromMillisecondsSinceEpoch(ts * 1000).toLocal();
    final now = DateTime.now();
    final diff = DateTime(now.year, now.month, now.day)
        .difference(DateTime(d.year, d.month, d.day))
        .inDays;
    final base = '${d.day} ${_months[d.month - 1]}'.toUpperCase();
    if (diff == 0) return 'TODAY · $base';
    if (diff == 1) return 'YESTERDAY · $base';
    return base;
  }

  ({String text, Color color}) _reason(_Run r) {
    final s = r.reason.toLowerCase();
    if (s.contains('manual')) {
      if (s.contains('touch') || s.contains('controller')) return (text: 'Manual · Controller', color: _blue);
      if (s.contains('web')) return (text: 'Manual · Web', color: _blue);
      if (s.contains('app')) return (text: 'Manual · App', color: _blue);
      return (text: 'Manual', color: _blue);
    }
    if (s.contains('schedul')) return (text: 'Scheduled', color: _orange);
    if (s.contains('full')) return (text: 'Auto · Tank full', color: _green);
    if (s.contains('auto') || s.contains('level')) return (text: 'Auto · Level', color: _green);
    if (s.contains('max')) return (text: 'Max runtime', color: _red);
    if (s.contains('lora') || s.contains('signal')) return (text: 'Signal lost', color: _red);
    if (s.contains('power restore')) return (text: 'Power restored', color: _green);
    if (s.contains('power')) return (text: 'Power cut', color: _red);
    if (r.reason.isEmpty) return (text: 'Auto', color: _green);
    return (text: r.reason, color: _orange);
  }

  @override
  Widget build(BuildContext context) {
    final runs = _runs();
    final issues = context.watch<TankService>().deliveryIssues;
    return Scaffold(
      backgroundColor: _bg,
      appBar: AppBar(
        backgroundColor: _bg,
        elevation: 0,
        leading: BackButton(color: _lbl),
        titleSpacing: 0,
        title: Text('History', style: TextStyle(color: _txt, fontSize: 16, fontWeight: FontWeight.w600)),
        actions: [
          IconButton(
            icon: Icon(Icons.delete_outline, color: _red, size: 20),
            onPressed: runs.isEmpty ? null : _clearHistory,
            tooltip: 'Clear history',
          ),
        ],
      ),
      body: _loading
          ? Center(child: CircularProgressIndicator(color: _green))
          : RefreshIndicator(
              color: _green,
              backgroundColor: _card,
              onRefresh: () => _load(),
              child: ListView(
                // Bottom inset accounts for the system nav bar (3-button or
                // gesture pill) so the last card never sits underneath it.
                padding: EdgeInsets.fromLTRB(12, 12, 12, 12 + MediaQuery.of(context).padding.bottom),
                children: [
                  _periodBar(),
                  const SizedBox(height: 12),
                  if (issues.isNotEmpty) ...[
                    _deliveryIssuesCard(issues),
                    const SizedBox(height: 12),
                  ],
                  Container(
                    padding: const EdgeInsets.all(14),
                    decoration: BoxDecoration(
                      color: _card,
                      borderRadius: BorderRadius.circular(16),
                      border: Border.all(color: _bd),
                    ),
                    child: Column(children: [
                      _headerRow(),
                      const SizedBox(height: 12),
                      if (_showGraph) _graphView(runs) else _tableSummary(runs),
                    ]),
                  ),
                  if (!_showGraph) ...[
                    const SizedBox(height: 12),
                    Container(
                      padding: const EdgeInsets.all(14),
                      decoration: BoxDecoration(
                        color: _card,
                        borderRadius: BorderRadius.circular(16),
                        border: Border.all(color: _bd),
                      ),
                      child: _tableRunsList(runs),
                    ),
                  ],
                ],
              ),
            ),
    );
  }

  // ── PERIOD SELECTOR (chips + resolved-range subrow) ───────────────────────
  Widget _periodBar() {
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        for (final p in HistoryPeriod.values) ...[
          if (p != HistoryPeriod.values.first) const SizedBox(width: 6),
          Expanded(child: _periodChip(p)),
        ],
      ]),
      const SizedBox(height: 10),
      Row(mainAxisAlignment: MainAxisAlignment.center, children: [
        _navArrow(Icons.chevron_left, _canGoBack ? () => _shiftPeriod(-1) : null),
        const SizedBox(width: 16),
        Column(mainAxisSize: MainAxisSize.min, children: [
          GestureDetector(
            onTap: _period == HistoryPeriod.today ? null : () => _openPeriodPicker(initialSegment: _period),
            child: Text(_rangeLabel(),
                style: TextStyle(color: _txt, fontSize: 13, fontWeight: FontWeight.w700)),
          ),
          const SizedBox(height: 3),
          GestureDetector(
            onTap: _pickDate,
            child: Row(mainAxisSize: MainAxisSize.min, children: [
              Text('choose date',
                  style: TextStyle(color: _lbl, fontSize: 10.5, decoration: TextDecoration.underline)),
              const SizedBox(width: 4),
              Icon(Icons.calendar_today, size: 11, color: _lbl),
            ]),
          ),
        ]),
        const SizedBox(width: 16),
        _navArrow(Icons.chevron_right, _canGoForward ? () => _shiftPeriod(1) : null),
      ]),
    ]);
  }

  Widget _navArrow(IconData icon, VoidCallback? onTap) => GestureDetector(
        onTap: onTap,
        child: Container(
          width: 32,
          height: 32,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            color: _card2,
            border: Border.all(color: _bd),
          ),
          child: Icon(icon, color: onTap != null ? _txt : _lbl.withValues(alpha: 0.3), size: 19),
        ),
      );

  Widget _periodChip(HistoryPeriod p) {
    final active = _period == p;
    return GestureDetector(
      onTap: () {
        if (p == HistoryPeriod.today) {
          setState(() {
            _period = HistoryPeriod.today;
            _anchor = DateTime.now();
          });
          _load();
        } else {
          _openPeriodPicker(initialSegment: p);
        }
      },
      child: Container(
        padding: const EdgeInsets.symmetric(vertical: 7),
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: active ? _blue : _card2,
          border: Border.all(color: active ? _blue : _bd),
          borderRadius: BorderRadius.circular(9),
        ),
        child: Text(_periodChipLabel(p),
            style: TextStyle(
                color: active ? Colors.white : _lbl, fontSize: 11.5, fontWeight: FontWeight.w700)),
      ),
    );
  }

  // ── Period picker bottom sheet ─────────────────────────────────────────────
  Future<void> _openPeriodPicker({required HistoryPeriod initialSegment}) async {
    HistoryPeriod segment = initialSegment;
    DateTime tempAnchor = _normalize(
      _period == initialSegment ? _anchor : DateTime.now(),
      segment,
    );

    await showModalBottomSheet(
      context: context,
      backgroundColor: _card,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (ctx) {
        return StatefulBuilder(builder: (ctx, setSheetState) {
          final bounds = _periodBoundsFor(segment, tempAnchor);
          final backBounds = _periodBoundsFor(segment, _normalize(_shiftAnchorFor(segment, tempAnchor, -1), segment));
          final backOk = !backBounds.start.isBefore(_earliestAllowed);
          final fwdOk = bounds.end.isBefore(DateTime.now());

          String label;
          switch (segment) {
            case HistoryPeriod.today:
              label = 'Today';
              break;
            case HistoryPeriod.week:
            case HistoryPeriod.month:
            case HistoryPeriod.year:
              final endIncl = DateTime(tempAnchor.year, tempAnchor.month, tempAnchor.day);
              final sameMonth = bounds.start.month == endIncl.month && bounds.start.year == endIncl.year;
              final sameYear = bounds.start.year == endIncl.year;
              final startStr = sameMonth
                  ? '${bounds.start.day}'
                  : sameYear
                      ? '${bounds.start.day} ${_months[bounds.start.month - 1]}'
                      : '${bounds.start.day} ${_months[bounds.start.month - 1]} ${bounds.start.year}';
              label = '$startStr – ${endIncl.day} ${_months[endIncl.month - 1]} ${endIncl.year}';
              break;
          }

          void shift(int dir) {
            final next = _normalize(_shiftAnchorFor(segment, tempAnchor, dir), segment);
            final b = _periodBoundsFor(segment, next);
            if (dir < 0 && b.start.isBefore(_earliestAllowed)) return;
            if (dir > 0 && b.start.isAfter(DateTime.now())) return;
            setSheetState(() => tempAnchor = next);
          }

          return Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 22),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Container(width: 36, height: 4, decoration: BoxDecoration(color: _bd, borderRadius: BorderRadius.circular(2))),
              const SizedBox(height: 14),
              Align(
                alignment: Alignment.centerLeft,
                child: Text('Select ${_periodChipLabel(segment).toLowerCase()}',
                    style: TextStyle(color: _txt, fontSize: 15, fontWeight: FontWeight.w700)),
              ),
              const SizedBox(height: 12),
              Row(children: [
                for (final seg in [HistoryPeriod.week, HistoryPeriod.month, HistoryPeriod.year]) ...[
                  if (seg != HistoryPeriod.week) const SizedBox(width: 6),
                  Expanded(
                    child: GestureDetector(
                      onTap: () => setSheetState(() {
                        segment = seg;
                        tempAnchor = _normalize(DateTime.now(), seg);
                      }),
                      child: Container(
                        padding: const EdgeInsets.symmetric(vertical: 8),
                        alignment: Alignment.center,
                        decoration: BoxDecoration(
                          color: segment == seg ? _blue : _card2,
                          border: Border.all(color: segment == seg ? _blue : _bd),
                          borderRadius: BorderRadius.circular(9),
                        ),
                        child: Text(_periodChipLabel(seg),
                            style: TextStyle(
                                color: segment == seg ? Colors.white : _lbl,
                                fontSize: 11.5,
                                fontWeight: FontWeight.w700)),
                      ),
                    ),
                  ),
                ],
              ]),
              const SizedBox(height: 16),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                decoration: BoxDecoration(color: _card2, borderRadius: BorderRadius.circular(12)),
                child: Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
                  _navArrow(Icons.chevron_left, backOk ? () => shift(-1) : null),
                  Text(label, style: TextStyle(color: _txt, fontSize: 13, fontWeight: FontWeight.w600)),
                  _navArrow(Icons.chevron_right, fwdOk ? () => shift(1) : null),
                ]),
              ),
              const SizedBox(height: 14),
              Container(
                padding: const EdgeInsets.all(10),
                decoration: BoxDecoration(
                  color: _orange.withValues(alpha: 0.08),
                  border: Border.all(color: _orange.withValues(alpha: 0.3)),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Icon(Icons.info_outline, color: _orange, size: 14),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      'History is available for the last 2 years (back to '
                      '${_months[_earliestAllowed.month - 1]} ${_earliestAllowed.year}).',
                      style: TextStyle(color: _lbl, fontSize: 10.5, height: 1.4),
                    ),
                  ),
                ]),
              ),
              const SizedBox(height: 16),
              GestureDetector(
                onTap: () {
                  setState(() {
                    _period = segment;
                    _anchor = tempAnchor;
                  });
                  Navigator.pop(ctx);
                  _load();
                },
                child: Container(
                  padding: const EdgeInsets.symmetric(vertical: 13),
                  alignment: Alignment.center,
                  decoration: BoxDecoration(color: _blue, borderRadius: BorderRadius.circular(12)),
                  child: const Text('Apply',
                      style: TextStyle(color: Colors.white, fontSize: 13.5, fontWeight: FontWeight.w700)),
                ),
              ),
            ]),
          );
        });
      },
    );
  }

  // ── DELIVERY ISSUES card (local, unacknowledged commands) ────────────────
  Widget _deliveryIssuesCard(List<DeliveryIssue> issues) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: _red.withOpacity(0.06),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: _red.withOpacity(0.35)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            Icon(Icons.error_outline, color: _red, size: 18),
            const SizedBox(width: 6),
            Text('DELIVERY ISSUES',
                style: TextStyle(color: _red, fontSize: 11, fontWeight: FontWeight.w800, letterSpacing: 0.8)),
            const Spacer(),
            GestureDetector(
              onTap: () => context.read<TankService>().clearDeliveryIssues(),
              child: Text('CLEAR',
                  style: TextStyle(color: _red.withOpacity(0.85), fontSize: 10.5, fontWeight: FontWeight.w700)),
            ),
          ]),
          const SizedBox(height: 6),
          Text('Commands the controller never acknowledged (no ack received).',
              style: TextStyle(color: _lbl, fontSize: 11)),
          const SizedBox(height: 10),
          ...issues.map(_deliveryIssueRow),
        ],
      ),
    );
  }

  Widget _deliveryIssueRow(DeliveryIssue e) {
    final d = e.time.toLocal();
    final dateStr = '${d.day} ${_months[d.month - 1]} · ${_two(d.hour)}:${_two(d.minute)}';
    final label = '${e.motor} motor ${e.start ? 'start' : 'stop'}';
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(children: [
        Container(
          width: 34, height: 34,
          decoration: BoxDecoration(
            color: _red.withOpacity(0.12),
            borderRadius: BorderRadius.circular(9),
          ),
          child: Icon(Icons.wifi_off_rounded, color: _red, size: 17),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(label,
                  style: TextStyle(color: _txt, fontSize: 13, fontWeight: FontWeight.w700)),
              const SizedBox(height: 1),
              Text('Not delivered — signal did not reach the controller',
                  style: TextStyle(color: _lbl, fontSize: 10.5)),
            ],
          ),
        ),
        const SizedBox(width: 8),
        Text(dateStr,
            style: TextStyle(color: _lbl, fontSize: 10.5, fontWeight: FontWeight.w600)),
      ]),
    );
  }

  Widget _headerRow() => Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text('HISTORY',
              style: TextStyle(color: _lbl, fontSize: 11, fontWeight: FontWeight.w700, letterSpacing: 0.8)),
          Container(
            padding: const EdgeInsets.all(3),
            decoration: BoxDecoration(color: _card2, borderRadius: BorderRadius.circular(9)),
            child: Row(children: [
              _toggleBtn('Table', !_showGraph, () => setState(() => _showGraph = false)),
              _toggleBtn('Graph', _showGraph, () => setState(() => _showGraph = true)),
            ]),
          ),
        ],
      );

  Widget _toggleBtn(String label, bool on, VoidCallback onTap) => GestureDetector(
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
          decoration: BoxDecoration(color: on ? _blue : Colors.transparent, borderRadius: BorderRadius.circular(7)),
          child: Text(label,
              style: TextStyle(
                  color: on ? Colors.white : _lbl,
                  fontSize: 11,
                  fontWeight: on ? FontWeight.w600 : FontWeight.w400)),
        ),
      );

  Widget _stat(String v, String k, Color? vColor) => Expanded(
        child: Container(
          padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 6),
          decoration: BoxDecoration(color: _card2, borderRadius: BorderRadius.circular(10)),
          child: Column(children: [
            Text(v, style: TextStyle(color: vColor ?? _txt, fontSize: 15, fontWeight: FontWeight.w800)),
            const SizedBox(height: 1),
            Text(k, textAlign: TextAlign.center, style: TextStyle(color: _lbl, fontSize: 8.5, letterSpacing: 0.4)),
          ]),
        ),
      );

  Widget _empty(String msg) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 44),
        child: Center(child: Text(msg, style: TextStyle(color: _lbl, fontSize: 14))),
      );

  Widget _legend(Color c, String t) => Row(mainAxisSize: MainAxisSize.min, children: [
        Container(width: 10, height: 10, decoration: BoxDecoration(color: c, borderRadius: BorderRadius.circular(3))),
        const SizedBox(width: 4),
        Text(t, style: TextStyle(color: _lbl, fontSize: 10)),
      ]);

  // ── WATER VOLUME banners (OH pumped / UG drawn, scoped to the period) ────
  Widget _waterBanners(List<_Run> runs) {
    final nowTs = _nowTs();
    final bounds = _periodBounds();
    final periodFrom = _epoch(bounds.start);
    final periodTo = _epoch(bounds.end);
    int ohSec = 0, ugSec = 0;
    for (final r in runs) {
      final d = r.clippedDurSec(nowTs, periodFrom, periodTo);
      if (r.motor == 'OH') {
        ohSec += d;
      } else {
        ugSec += d;
      }
    }
    if (ohSec == 0 && ugSec == 0) return const SizedBox.shrink();
    final prefs = context.watch<AppPreferences>();
    final showOh = ohSec > 0 && prefs.showOhWater;
    final showUg = ugSec > 0 && prefs.showUgWater;
    if (!showOh && !showUg) return const SizedBox.shrink();
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: Column(children: [
        if (showOh) _waterBanner('OH', ohSec, prefs.ohFlowLPM, _blue),
        if (showOh && showUg) const SizedBox(height: 8),
        if (showUg) _waterBanner('UG', ugSec, prefs.ugFlowLPM, _purple),
      ]),
    );
  }

  Widget _waterBanner(String motor, int sec, double lpm, Color color) {
    final minutes = sec / 60.0;
    final litres = (minutes * lpm).round();
    final label = motor == 'OH' ? 'OH WATER PUMPED (EST.)' : 'UG WATER DRAWN (EST.)';
    return GestureDetector(
      onTap: () => _calibrateDialog(motor, lpm),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.09),
          border: Border.all(color: color.withValues(alpha: 0.28)),
          borderRadius: BorderRadius.circular(12),
        ),
        child: Row(children: [
          const Text('💧', style: TextStyle(fontSize: 17)),
          const SizedBox(width: 10),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Row(crossAxisAlignment: CrossAxisAlignment.baseline, textBaseline: TextBaseline.alphabetic, children: [
                Text(_litresStr(litres), style: TextStyle(color: _txt, fontSize: 14.5, fontWeight: FontWeight.w800)),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(label,
                      style: TextStyle(color: _lbl, fontSize: 9.5, fontWeight: FontWeight.w700, letterSpacing: 0.3),
                      overflow: TextOverflow.ellipsis),
                ),
              ]),
              const SizedBox(height: 1),
              Text('${_durShort(sec)} runtime × ${lpm.toStringAsFixed(0)} L/min (est.)',
                  style: TextStyle(color: _lbl, fontSize: 9.5)),
            ]),
          ),
          const SizedBox(width: 6),
          Text('CALIBRATE ›', style: TextStyle(color: color, fontSize: 9.5, fontWeight: FontWeight.w700)),
        ]),
      ),
    );
  }

  String _litresStr(int litres) {
    if (litres >= 1000) {
      final thousands = litres / 1000.0;
      return '~${thousands.toStringAsFixed(thousands >= 10 ? 0 : 1)}k L';
    }
    return '~$litres L';
  }

  Future<void> _calibrateDialog(String motor, double currentLpm) async {
    final ctrl = TextEditingController(text: currentLpm.toStringAsFixed(0));
    final result = await showDialog<double>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: _card,
        title: Text('Calibrate $motor pump rate', style: TextStyle(color: _txt, fontSize: 15)),
        content: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text(
            "Enter your pump's real-world flow rate for a more accurate "
            'estimate. Tip: time how long it takes to fill a known volume '
            '(e.g. a 20 L bucket) and divide to get L/min.',
            style: TextStyle(color: _lbl, fontSize: 12, height: 1.4),
          ),
          const SizedBox(height: 14),
          TextField(
            controller: ctrl,
            keyboardType: const TextInputType.numberWithOptions(decimal: true),
            style: TextStyle(color: _txt),
            decoration: InputDecoration(
              suffixText: 'L/min',
              suffixStyle: TextStyle(color: _lbl),
              filled: true,
              fillColor: _card2,
              border: OutlineInputBorder(borderRadius: BorderRadius.circular(10), borderSide: BorderSide.none),
            ),
          ),
        ]),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () {
              final v = double.tryParse(ctrl.text.trim());
              Navigator.pop(ctx, v);
            },
            child: Text('Save', style: TextStyle(color: _blue)),
          ),
        ],
      ),
    );
    if (result != null && result > 0 && mounted) {
      await context.read<AppPreferences>().setFlowLPM(motor, result);
    }
  }

  // ── TABLE (T1★ Enhanced) ─────────────────────────────────────────────────
  /// Shared per-run aggregation (OH/UG seconds, max duration, day groups)
  /// used by both the summary card and the runs-list card below.
  ({int ohSec, int ugSec, int maxDur, Map<String, List<_Run>> groups, List<String> order}) _tableData(List<_Run> runs) {
    final nowTs = _nowTs();
    final bounds = _periodBounds();
    final periodFrom = _epoch(bounds.start);
    final periodTo = _epoch(bounds.end);

    int ohSec = 0, ugSec = 0, maxDur = 1;
    for (final r in runs) {
      final dur = r.clippedDurSec(nowTs, periodFrom, periodTo);
      if (dur > maxDur) maxDur = dur;
      if (r.motor == 'OH') {
        ohSec += dur;
      } else {
        ugSec += dur;
      }
    }

    final groups = <String, List<_Run>>{};
    final order = <String>[];
    for (final r in runs) {
      final k = _dayKey(r.onTs);
      if (!groups.containsKey(k)) {
        groups[k] = [];
        order.add(k);
      }
      groups[k]!.add(r);
    }

    return (ohSec: ohSec, ugSec: ugSec, maxDur: maxDur, groups: groups, order: order);
  }

  /// Card 1 content: run-count/OH/UG stats + water banners.
  Widget _tableSummary(List<_Run> runs) {
    final d = _tableData(runs);
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        _stat('${runs.length}', _runsCountLabel(), null),
        const SizedBox(width: 8),
        _stat(_durLong(d.ohSec), 'TOTAL OH RUN', _blue),
        const SizedBox(width: 8),
        _stat(_durLong(d.ugSec), 'TOTAL UG RUN', _purple),
      ]),
      const SizedBox(height: 10),
      _waterBanners(runs),
    ]);
  }

  /// Card 2 content: day-grouped run rows (or the empty-state message).
  Widget _tableRunsList(List<_Run> runs) {
    if (runs.isEmpty) return _empty('No motor runs in this period');
    final nowTs = _nowTs();
    final d = _tableData(runs);
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      for (final k in d.order) ...[
        _dateHeader(d.groups[k]!, nowTs),
        for (final r in d.groups[k]!) _runRow(r, d.maxDur, nowTs),
      ],
    ]);
  }

  Widget _dateHeader(List<_Run> dayRuns, int nowTs) {
    final subtotal = dayRuns.fold<int>(0, (a, r) => a + r.durSec(nowTs));
    return Padding(
      padding: const EdgeInsets.fromLTRB(2, 14, 2, 6),
      child: Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
        Text(_dayHeader(dayRuns.first.onTs),
            style: TextStyle(color: _lbl, fontSize: 10, fontWeight: FontWeight.w700, letterSpacing: 0.8)),
        Text('${dayRuns.length} runs · ${_durLong(subtotal)}', style: TextStyle(color: _lbl, fontSize: 10)),
      ]),
    );
  }

  Widget _runRow(_Run r, int maxDur, int nowTs) {
    final isOH = r.motor == 'OH';
    final c = isOH ? _blue : _purple;
    final dur = r.durSec(nowTs);
    final rsn = _reason(r);
    return Container(
      decoration: BoxDecoration(border: Border(top: BorderSide(color: _bd))),
      padding: const EdgeInsets.symmetric(vertical: 9),
      child: Row(children: [
        Container(width: 3, height: 36, decoration: BoxDecoration(color: c, borderRadius: BorderRadius.circular(2))),
        const SizedBox(width: 9),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              _motorChip(r.motor, c),
              const SizedBox(width: 6),
              Expanded(
                child: Text.rich(
                  TextSpan(children: [
                    TextSpan(
                        text: r.running ? '${_hhmm(r.onTs)} → ' : '${_hhmm(r.onTs)} → ${_hhmm(r.offTs!)}',
                        style: TextStyle(color: _txt, fontSize: 12.5, fontWeight: FontWeight.w600)),
                    if (r.running)
                      TextSpan(text: 'now', style: TextStyle(color: _green, fontSize: 12.5, fontWeight: FontWeight.w600)),
                  ]),
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            ]),
            const SizedBox(height: 4),
            r.running ? _pill('● RUNNING', _green, solid: true, bold: true) : _pill(rsn.text, rsn.color),
            const SizedBox(height: 5),
            ClipRRect(
              borderRadius: BorderRadius.circular(3),
              child: LinearProgressIndicator(
                value: (dur / maxDur).clamp(0.02, 1.0),
                minHeight: 4,
                backgroundColor: _card2,
                color: c,
              ),
            ),
          ]),
        ),
        const SizedBox(width: 10),
        Text(_durShort(dur), style: TextStyle(color: r.running ? _green : _txt, fontSize: 13, fontWeight: FontWeight.w800)),
      ]),
    );
  }

  Widget _motorChip(String m, Color c) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
        decoration: BoxDecoration(
            color: c.withValues(alpha: 0.12), border: Border.all(color: c), borderRadius: BorderRadius.circular(6)),
        child: Text(m, style: TextStyle(color: c, fontSize: 9, fontWeight: FontWeight.w800)),
      );

  Widget _pill(String text, Color color, {bool solid = false, bool bold = false}) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 1.5),
        decoration:
            BoxDecoration(color: solid ? color : color.withValues(alpha: 0.12), borderRadius: BorderRadius.circular(20)),
        child: Text(text,
            style: TextStyle(
                color: solid ? _runFg : color, fontSize: 9, fontWeight: bold ? FontWeight.w800 : FontWeight.w600)),
      );

  // ── GRAPH (per-run for Today, aggregated per day/month otherwise) ────────
  Widget _graphView(List<_Run> runs) {
    if (runs.isEmpty) return _empty('No motor runs to chart yet');
    if (_period == HistoryPeriod.today) {
      return _perRunGraph(runs);
    }
    return _aggregatedGraph(runs);
  }

  // Original per-run bar chart (kept for Today/Day where individual runs
  // are still meaningful at a glance).
  Widget _perRunGraph(List<_Run> runs) {
    final nowTs = _nowTs();
    final chrono = runs.reversed.toList(); // oldest-first
    final show = chrono.length > 12 ? chrono.sublist(chrono.length - 12) : chrono;

    final durs = show.map((r) => r.durSec(nowTs) / 60.0).toList(); // minutes
    int total = 0, longest = 0;
    for (final r in show) {
      final d = r.durSec(nowTs);
      total += d;
      if (d > longest) longest = d;
    }
    final avgMin = durs.isEmpty ? 0.0 : durs.reduce((a, b) => a + b) / durs.length;
    double maxDurMin = 0;
    for (final d in durs) {
      if (d > maxDurMin) maxDurMin = d;
    }
    final maxY = maxDurMin <= 0 ? 20.0 : (maxDurMin / 20).ceil() * 20.0;
    final step = maxY <= 80 ? 20.0 : (maxY / 4).ceilToDouble();
    final grids = <double>[];
    for (double v = step; v <= maxY + 0.1; v += step) {
      grids.add(v);
    }
    const plotH = 160.0;

    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        _stat(_durLong(total), 'TOTAL', null),
        const SizedBox(width: 8),
        _stat(_durShort(longest), 'LONGEST', null),
        const SizedBox(width: 8),
        _stat('${avgMin.round()}m', 'AVG', null),
      ]),
      const SizedBox(height: 10),
      _waterBanners(runs),
      Row(children: [
        _legend(_blue, 'OH'),
        const SizedBox(width: 14),
        _legend(_purple, 'UG'),
        const Spacer(),
        Text('minutes', style: TextStyle(color: _lbl, fontSize: 10)),
      ]),
      const SizedBox(height: 10),
      Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        SizedBox(
          width: 24,
          height: plotH,
          child: Stack(children: [
            for (final g in grids)
              Positioned(
                top: plotH * (1 - g / maxY) - 6,
                right: 4,
                child: Text(g.toInt().toString(), style: TextStyle(color: _lbl, fontSize: 8)),
              ),
          ]),
        ),
        Expanded(
          child: SizedBox(
            height: plotH,
            child: Stack(clipBehavior: Clip.none, children: [
              for (final g in grids)
                Positioned(top: plotH * (1 - g / maxY), left: 0, right: 0, child: _DashedLine(color: _bd)),
              if (avgMin > 0 && avgMin <= maxY)
                Positioned(
                  top: plotH * (1 - avgMin / maxY),
                  left: 0,
                  right: 0,
                  child: _DashedLine(color: _orange, label: 'avg ${avgMin.round()}', labelBg: _card),
                ),
              Positioned.fill(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    for (int i = 0; i < show.length; i++)
                      Expanded(
                        child: _bar(
                          show[i],
                          durs[i],
                          maxY,
                          plotH,
                          dayStart: i > 0 && _dayKey(show[i].onTs) != _dayKey(show[i - 1].onTs),
                        ),
                      ),
                  ],
                ),
              ),
            ]),
          ),
        ),
      ]),
      const SizedBox(height: 5),
      Row(children: [
        const SizedBox(width: 24),
        Expanded(
          child: Row(children: [
            for (int i = 0; i < show.length; i++)
              Expanded(
                child: Column(children: [
                  if (i == 0 || _dayKey(show[i].onTs) != _dayKey(show[i - 1].onTs))
                    Text(_dateShort(show[i].onTs),
                        textAlign: TextAlign.center,
                        style: TextStyle(color: _txt, fontSize: 8, fontWeight: FontWeight.w600)),
                  Text(_hhmm(show[i].onTs), style: TextStyle(color: _lbl, fontSize: 8)),
                ]),
              ),
          ]),
        ),
      ]),
    ]);
  }

  /// Aggregated bar chart used for Week/Month (per-day buckets) and Year
  /// (per-month buckets) — plotting every individual run would be unreadable
  /// at those scales.
  Widget _aggregatedGraph(List<_Run> runs) {
    final nowTs = _nowTs();
    final bounds = _periodBounds();
    final periodFrom = _epoch(bounds.start);
    final periodTo = _epoch(bounds.end);
    final byMonth = _period == HistoryPeriod.year;

    // Build ordered bucket start-dates spanning the whole period (so empty
    // days/months still get a zero-height slot instead of being skipped).
    final buckets = <DateTime>[];
    if (byMonth) {
      for (var d = bounds.start; d.isBefore(bounds.end); d = DateTime(d.year, d.month + 1, 1)) {
        buckets.add(d);
      }
    } else {
      for (var d = bounds.start; d.isBefore(bounds.end); d = d.add(const Duration(days: 1))) {
        buckets.add(d);
      }
    }

    final ohMin = List<double>.filled(buckets.length, 0);
    final ugMin = List<double>.filled(buckets.length, 0);
    for (final r in runs) {
      final dur = r.clippedDurSec(nowTs, periodFrom, periodTo);
      if (dur <= 0) continue;
      final d = DateTime.fromMillisecondsSinceEpoch(r.onTs * 1000).toLocal();
      final idx = byMonth
          ? (d.year - bounds.start.year) * 12 + (d.month - bounds.start.month)
          : d.difference(bounds.start).inDays;
      if (idx < 0 || idx >= buckets.length) continue;
      if (r.motor == 'OH') {
        ohMin[idx] += dur / 60.0;
      } else {
        ugMin[idx] += dur / 60.0;
      }
    }

    int totalSec = 0, longestSec = 0;
    for (final r in runs) {
      final dur = r.clippedDurSec(nowTs, periodFrom, periodTo);
      totalSec += dur;
      if (dur > longestSec) longestSec = dur;
    }
    final activeBuckets = List.generate(buckets.length, (i) => ohMin[i] + ugMin[i]).where((v) => v > 0).length;
    final avgMinPerBucket = activeBuckets == 0 ? 0.0 : (totalSec / 60.0) / activeBuckets;

    double maxDurMin = 0;
    for (var i = 0; i < buckets.length; i++) {
      if (ohMin[i] > maxDurMin) maxDurMin = ohMin[i];
      if (ugMin[i] > maxDurMin) maxDurMin = ugMin[i];
    }
    final maxY = maxDurMin <= 0 ? 20.0 : maxDurMin;
    const plotH = 150.0;

    // Thin out x-axis labels so they don't overlap on wide (month/year) charts.
    final labelEvery = buckets.length <= 8 ? 1 : (buckets.length / 6).ceil();

    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        _stat(_durLong(totalSec), 'TOTAL', null),
        const SizedBox(width: 8),
        _stat(_durShort(longestSec), 'LONGEST', null),
        const SizedBox(width: 8),
        _stat('${avgMinPerBucket.round()}m', byMonth ? 'AVG/MONTH' : 'AVG/DAY', null),
      ]),
      const SizedBox(height: 10),
      _waterBanners(runs),
      Row(children: [
        _legend(_blue, 'OH'),
        const SizedBox(width: 14),
        _legend(_purple, 'UG'),
        const Spacer(),
        Text(byMonth ? 'minutes/month' : 'minutes/day', style: TextStyle(color: _lbl, fontSize: 10)),
      ]),
      const SizedBox(height: 10),
      SizedBox(
        height: plotH,
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            for (var i = 0; i < buckets.length; i++)
              Expanded(
                child: Column(mainAxisAlignment: MainAxisAlignment.end, children: [
                  Container(
                    width: double.infinity,
                    margin: const EdgeInsets.symmetric(horizontal: 1),
                    height: (plotH * (ohMin[i] / maxY)).clamp(0.0, plotH),
                    decoration: BoxDecoration(
                      color: _blue,
                      borderRadius: const BorderRadius.vertical(top: Radius.circular(2)),
                    ),
                  ),
                  const SizedBox(height: 2),
                  Container(
                    width: double.infinity,
                    margin: const EdgeInsets.symmetric(horizontal: 1),
                    height: (plotH * (ugMin[i] / maxY)).clamp(0.0, plotH),
                    decoration: BoxDecoration(
                      color: _purple,
                      borderRadius: const BorderRadius.vertical(top: Radius.circular(2)),
                    ),
                  ),
                ]),
              ),
          ],
        ),
      ),
      const SizedBox(height: 4),
      Row(children: [
        for (var i = 0; i < buckets.length; i++)
          Expanded(
            child: Text(
              i % labelEvery == 0 ? (byMonth ? _months[buckets[i].month - 1] : '${buckets[i].day}') : '',
              textAlign: TextAlign.center,
              style: TextStyle(color: _lbl, fontSize: 8),
            ),
          ),
      ]),
    ]);
  }

  String _dateShort(int ts) {
    final d = DateTime.fromMillisecondsSinceEpoch(ts * 1000).toLocal();
    return '${d.day} ${_months[d.month - 1]}';
  }

  Widget _bar(_Run r, double durMin, double maxY, double plotH, {bool dayStart = false}) {
    final isOH = r.motor == 'OH';
    final grad = isOH
        ? const LinearGradient(
            begin: Alignment.topCenter, end: Alignment.bottomCenter, colors: [Color(0xFF82B1FF), Color(0xFF2962FF)])
        : const LinearGradient(
            begin: Alignment.topCenter, end: Alignment.bottomCenter, colors: [Color(0xFFB39DFF), Color(0xFF651FFF)]);
    final barH = (plotH * (durMin / maxY)).clamp(3.0, plotH - 18);
    return Container(
      decoration: dayStart ? BoxDecoration(border: Border(left: BorderSide(color: _bd))) : null,
      child: Column(mainAxisAlignment: MainAxisAlignment.end, children: [
        Text('${durMin.round()}',
            style: TextStyle(color: isOH ? _blue : _purple, fontSize: 9, fontWeight: FontWeight.w800)),
        const SizedBox(height: 3),
        // Cap the bar's width so a chart with only one or two runs (whose
        // Expanded slot spans most/all of the plot area) doesn't stretch
        // into one huge column — width scales with the slot but never
        // exceeds a sensible max, matching the slim bars seen with many runs.
        LayoutBuilder(
          builder: (context, constraints) {
            final w = (constraints.maxWidth * 0.62).clamp(8.0, 34.0);
            return Container(
              width: w,
              height: barH,
              decoration: BoxDecoration(gradient: grad, borderRadius: const BorderRadius.vertical(top: Radius.circular(6))),
            );
          },
        ),
      ]),
    );
  }
}

/// Horizontal dashed line used for chart gridlines and the average marker.
class _DashedLine extends StatelessWidget {
  final Color color;
  final String? label;
  final Color? labelBg;
  const _DashedLine({required this.color, this.label, this.labelBg});

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 1,
      child: Stack(clipBehavior: Clip.none, children: [
        CustomPaint(size: const Size(double.infinity, 1), painter: _DashPainter(color)),
        if (label != null)
          Positioned(
            right: 0,
            top: -13,
            child: Container(
              color: labelBg,
              padding: const EdgeInsets.symmetric(horizontal: 3),
              child: Text(label!, style: TextStyle(color: color, fontSize: 8)),
            ),
          ),
      ]),
    );
  }
}

class _DashPainter extends CustomPainter {
  final Color color;
  _DashPainter(this.color);
  @override
  void paint(Canvas canvas, Size size) {
    final p = Paint()
      ..color = color
      ..strokeWidth = 1;
    double x = 0;
    while (x < size.width) {
      canvas.drawLine(Offset(x, 0), Offset(x + 4, 0), p);
      x += 7;
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
