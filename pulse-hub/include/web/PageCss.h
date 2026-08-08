#ifndef PAGE_CSS_H
#define PAGE_CSS_H

// Served as /app.css. Kept apart from the markup so the look can be reworked
// without touching structure, and so the browser caches it between loads —
// the hub often serves this over its own hotspot with no internet.

static const char PAGE_CSS[] PROGMEM = R"CSS(
/* Three themes. Green is the default and stays exactly as approved.
   Black is a true black ground for night use at the panel and for OLED
   screens; it keeps the same green accent so the product still reads as one
   thing. Light is the daylight/outdoor option. */
:root{
 --bg:#0e1512;--card:#161f1b;--card2:#1d2823;--bd:#28352e;--bd2:#3a4c43;
 --tx:#e6efe9;--tx2:#8ea297;--ok:#3fb950;--warn:#d29922;--err:#f85149;--acc:#2ea043;
}
[data-t=black]{
 --bg:#000;--card:#0a0a0a;--card2:#141414;--bd:#242424;--bd2:#3a3a3a;
 --tx:#f2f2f2;--tx2:#909090;--ok:#3fb950;--warn:#d29922;--err:#ff5f56;--acc:#2ea043;
}
[data-t=light]{
 --bg:#f3f6f4;--card:#fff;--card2:#eef2ef;--bd:#dbe3dd;--bd2:#c3cfc8;
 --tx:#11201a;--tx2:#5b6f65;--ok:#2f9e63;--warn:#c9891f;--err:#d64545;--acc:#2f9e63;
}
/* Fixed, NOT theme-varying like --ok/--warn/--err above: R/Y/B is the
   physical wire-colour convention this install uses (see ApiPower.cpp) -
   a red phase should read red on every one of the 3 themes, not shift with
   dashboard mode the way a semantic status colour does. */
:root{--ph-r:#e0453a;--ph-y:#c99416;--ph-b:#2f66d8}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);
 font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.hdr{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:11px 15px;
 background:var(--card);border-bottom:1px solid var(--bd);position:sticky;top:0;z-index:20;flex-wrap:wrap}
.hdr h1{margin:0;font-size:16px;font-weight:650}
.ver{font-size:11px;color:var(--tx2);font-weight:400}
.hdr-r{display:flex;align-items:center;gap:9px;font-size:12px;color:var(--tx2)}
.wrap{padding:13px;max-width:1080px;margin:0 auto}
.grid{display:grid;gap:13px;grid-template-columns:repeat(auto-fit,minmax(290px,1fr))}
/* Dashboard: exactly 2 columns for Supply/Motor, not auto-fit's dynamic
   count. With only 2 real cards in that row, auto-fit was creating extra
   empty implicit column tracks on a wide screen - Supply/Motor looked
   squeezed into the left portion of the row while .card.full items below
   (which span every track, empty ones included) stretched the true full
   width, making the row above look narrower than it actually needed to be. */
.grid.g2{grid-template-columns:1fr 1fr}
@media (max-width:640px){.grid.g2{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:13px}
.card.full{grid-column:1/-1}
.ct{font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.6px;color:var(--tx2);margin:0 0 9px}
/* Standard card header: title left, at most one action button right, same
   size/position on every card that uses it — the Network tab's cards used to
   each place their button somewhere different. */
.card-hd{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:9px}
.card-hd .ct{margin:0}
/* Long explanatory paragraphs used to sit permanently in the card body; this
   collapses them into a small hover/focus tooltip next to the title instead. */
.info-dot{width:15px;height:15px;border-radius:50%;border:1px solid var(--bd2);color:var(--tx2);
  font-size:10px;font-weight:700;display:inline-flex;align-items:center;justify-content:center;
  cursor:help;flex:none;position:relative;margin-left:6px;vertical-align:middle}
.info-dot .tip{display:none;position:absolute;top:20px;left:0;z-index:5;width:220px;
  background:var(--card2);border:1px solid var(--bd2);border-radius:8px;padding:9px 11px;
  font-size:11.5px;font-weight:400;text-transform:none;letter-spacing:0;line-height:1.5;
  color:var(--tx);box-shadow:0 6px 18px rgba(0,0,0,.35)}
.info-dot:hover .tip,.info-dot:focus .tip{display:block}
/* Compact table for the raw per-phase calibration values. */
.raw-table{width:100%;border-collapse:collapse;font-size:12.5px}
.raw-table th{text-align:left;font-size:10px;text-transform:uppercase;letter-spacing:.5px;
  color:var(--tx2);padding:0 0 6px;font-weight:600}
.raw-table th:last-child,.raw-table td.num{text-align:right}
.raw-table td{padding:6px 0;border-top:1px solid var(--bd);vertical-align:middle}
.raw-table input{width:100px;text-align:right}
.badge{display:inline-block;padding:2px 9px;border-radius:11px;font-size:11px;font-weight:600}
.b-ok{background:rgba(63,185,80,.16);color:var(--ok)}
.b-warn{background:rgba(210,153,34,.16);color:var(--warn)}
.b-err{background:rgba(248,81,73,.16);color:var(--err)}
.b-off{background:var(--card2);color:var(--tx2)}
/* Dashboard field map: the plumbing itself (motors -> routing valves ->
   farm trunk -> per-zone branch), not an abstract tile grid. Fully dynamic —
   FieldMap.render() rebuilds it from live zone + motor data every poll, so
   it scales to however many zones/valves actually exist — see FieldMap in
   app.js, not a fixed shape hardcoded here. */
.fm-map{background:var(--card2);border:1px solid var(--bd);border-radius:8px;padding:10px;overflow-x:auto}
.banner{padding:9px 12px;border-radius:7px;font-size:12.5px;margin-bottom:11px;display:none}
.bn-warn{background:rgba(210,153,34,.12);border:1px solid var(--warn);color:var(--warn)}
.bn-err{background:rgba(248,81,73,.12);border:1px solid var(--err);color:var(--err)}

/* ---- Lamp: shared glowing-badge motif — phases, the motor itself, and
   (smaller, as .dot below) each precondition. Off/absent is always the same
   dim .bulb base; "live" picks up whichever semantic/phase color applies.
   Fixed rgba glow per color rather than color-mix(), matching how .b-ok
   etc. above already accept one fixed tint rather than one per theme. ---- */
.bulb{border-radius:50%;display:flex;align-items:center;justify-content:center;font-weight:800;
  font-variant-numeric:tabular-nums;background:var(--card2);color:var(--tx2);border:2.5px solid var(--bd2);
  transition:background .2s,border-color .2s,box-shadow .2s;
  /* Fixed-size badge, never a flex child that shrinks to fit its siblings -
     without this, .motorLamp's flex row squeezes the bulb's WIDTH down to
     fit the long state text beside it while its fixed height stays put,
     turning the circle into an egg. */
  flex-shrink:0}
.bulb.sm{width:52px;height:52px;font-size:12px}
.bulb.md{width:72px;height:72px;font-size:14px}
.bulb.live.r{background:var(--ph-r);border-color:var(--ph-r);color:#fff;
  box-shadow:0 0 0 5px rgba(224,69,58,.18),0 0 14px 2px rgba(224,69,58,.45)}
.bulb.live.y{background:var(--ph-y);border-color:var(--ph-y);color:#fff;
  box-shadow:0 0 0 5px rgba(201,148,22,.18),0 0 14px 2px rgba(201,148,22,.45)}
.bulb.live.b{background:var(--ph-b);border-color:var(--ph-b);color:#fff;
  box-shadow:0 0 0 5px rgba(47,102,216,.18),0 0 14px 2px rgba(47,102,216,.45)}
.bulb.live.ok{background:var(--ok);border-color:var(--ok);color:#fff;
  box-shadow:0 0 0 5px rgba(63,185,80,.18),0 0 14px 2px rgba(63,185,80,.45)}
.bulb.live.warn{background:var(--warn);border-color:var(--warn);color:#fff;
  box-shadow:0 0 0 5px rgba(210,153,34,.18),0 0 14px 2px rgba(210,153,34,.45)}
.bulb.live.err{background:var(--err);border-color:var(--err);color:#fff;
  box-shadow:0 0 0 5px rgba(248,81,73,.18),0 0 14px 2px rgba(248,81,73,.45)}

/* Real on/off toggle switch, for the two motor-drive controls that were a
   text button whose own click handler had to parse its current label back
   out to know what to send - "Engaged — release" read as ambiguous about
   whether it WAS engaged or would BECOME engaged. Track+knob shows state
   at a glance independent of the label beside it. warn variant is for
   lockout specifically: ON there means "motor deliberately blocked", which
   is an attention state, not the same "good" green as drive-enabled ON. */
.tgl{position:relative;width:42px;height:23px;border-radius:12px;border:none;
  background:var(--bd2);cursor:pointer;flex:none;padding:0;transition:background .2s}
.tgl .tk{position:absolute;top:2.5px;left:2.5px;width:18px;height:18px;border-radius:50%;
  background:#fff;transition:left .2s;box-shadow:0 1px 2px rgba(0,0,0,.35)}
.tgl.on{background:var(--ok)}
.tgl.on .tk{left:21.5px}
.tgl-warn.on{background:var(--err)}
.tglState{font-size:12px;font-weight:600;min-width:64px}

/* Well/Bore identity colors for the selector buttons - previously both used
   the same shared "selected" accent, so the only way to tell them apart at
   a glance was reading the text. */
.sel button.on.well{background:var(--acc);border-color:var(--acc)}
.sel button.on.bore{background:var(--warn);border-color:var(--warn)}

/* Supply card: three lamps, amps/Hz as small stats beneath each. */
.lampRow{display:flex;gap:10px;justify-content:space-between}
.lampCard{flex:1;text-align:center}
.lampCard .bulb{margin:0 auto 7px}
.lampCard .pl{font-size:10.5px;font-weight:700;letter-spacing:.05em;color:var(--tx2);margin-bottom:7px}
.miniStats{display:flex;flex-direction:column;gap:2px;font-size:10.5px;color:var(--tx2);font-variant-numeric:tabular-nums}
.miniStats b{color:var(--tx);font-weight:700}
.foot{display:flex;gap:9px;margin-top:14px;padding-top:12px;border-top:1px solid var(--bd)}
.foot .chip{flex:1;background:var(--card2);border:1px solid var(--bd);border-radius:8px;padding:8px 11px}
.foot .chip .l{font-size:9.5px;color:var(--tx2);text-transform:uppercase;letter-spacing:.04em;margin-bottom:2px}
.foot .chip .v{font-size:12.5px;font-weight:700;color:var(--tx)}
.foot .chip .v.warn{color:var(--warn)}
.foot .chip .v.off{color:var(--tx2);font-weight:600}

/* Motor card: same lamp, bigger — it's the primary read here. */
.motorLamp{display:flex;align-items:center;gap:14px;margin-bottom:14px}
.motorLamp .info .name{font-size:16px;font-weight:800;letter-spacing:-.01em}
.motorLamp .info .sub{font-size:11.5px;color:var(--tx2);margin-top:2px}

/* Start Preconditions: dot per check instead of a checkmark/cross glyph -
   same colour language as the lamps above, just small. Two-column so a
   full-width card doesn't leave one narrow list with dead space beside it. */
.checkGrid{display:grid;grid-template-columns:1fr 1fr;gap:0 26px}
@media (max-width:520px){.checkGrid{grid-template-columns:1fr}}
.checkRow{display:flex;align-items:center;gap:10px;padding:8px 0;border-top:1px solid var(--bd)}
.checkRow:nth-child(-n+2){border-top:none}
.checkRow .dot{width:10px;height:10px;border-radius:50%;flex:none;background:var(--bd2)}
.checkRow.pass .dot{background:var(--ok);box-shadow:0 0 0 4px rgba(63,185,80,.18)}
.checkRow.fail .dot{background:var(--err);box-shadow:0 0 0 4px rgba(248,81,73,.18)}
.checkRow .body{flex:1;min-width:0}
.checkRow .name{font-size:12.5px;font-weight:700}
.checkRow.fail .name{color:var(--err)}
.checkRow .detail{font-size:11px;color:var(--tx2);margin-top:1px}

/* Field map flow animation: every flowing pipe shares ONE dash period and
   animates exactly one period, so the loop is mathematically seamless -
   different per-line periods animated to the same fixed offset is what
   caused a visible jump at the seam during design review. */
.flowline{stroke-dasharray:6 4;animation:flow .7s linear infinite}
@keyframes flow{to{stroke-dashoffset:-10}}
@media (prefers-reduced-motion: reduce){.flowline{animation:none}}

.sel{display:flex;gap:6px;margin-bottom:9px}
.sel button{flex:1;padding:8px;border-radius:7px;border:1px solid var(--bd2);background:var(--card2);
 color:var(--tx2);font-size:12.5px;font-weight:600;cursor:pointer}
.sel button.on{background:var(--acc);border-color:var(--acc);color:#fff}
.brow{display:flex;gap:8px;margin-top:9px}
.btn{flex:1;padding:10px;border:none;border-radius:7px;font-size:13px;font-weight:650;cursor:pointer;
 color:#fff;background:var(--acc)}
.btn:disabled{opacity:.38;cursor:not-allowed}
.btn-d{background:var(--err)}
.btn-s{background:var(--card2);color:var(--tx);border:1px solid var(--bd2);flex:none;
 padding:7px 12px;font-size:12px;font-weight:600;border-radius:7px;cursor:pointer}
.dt{color:var(--tx2);font-size:11px}
.zg{display:grid;gap:8px;grid-template-columns:repeat(auto-fill,minmax(126px,1fr))}
.z{background:var(--card2);border:1px solid var(--bd);border-radius:8px;padding:10px;text-align:center}
.z.on{border-color:var(--acc);background:rgba(46,160,67,.13)}
.z.inactive{opacity:.55}
.z .zn{font-size:12.5px;font-weight:600;margin-bottom:4px}
.z .zs{font-size:10.5px;color:var(--tx2);font-variant-numeric:tabular-nums}
.z .zbtn{margin-top:7px;width:100%;padding:5px 0}
.z .zbtn:disabled{opacity:.38;cursor:not-allowed}
.row{display:flex;align-items:center;gap:8px;padding:7px 0;border-top:1px solid var(--bd);flex-wrap:wrap}
.row:first-child{border-top:none}
.lb{flex:1;min-width:128px;font-size:12.5px}
/* min-width:0 is what actually matters here: it lets the value shrink below
   its own single-line width instead of the whole span being forced onto a
   new physical row by .row's flex-wrap. No nowrap/ellipsis — truncating hid
   real values (SSID+IP, MAC) that people need to actually read; wrapping its
   own text onto a second line, still on the same row as the label, is the
   fix that keeps everything both visible and aligned. */
.rv{flex:1;min-width:0;font-size:12.5px;text-align:right}
.reserve{min-height:46px}
.inp{background:var(--card2);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;
 padding:6px 9px;font-size:12.5px;width:90px}
.inp.w{width:100%;min-width:110px}
select.inp{width:auto}
.hint{font-size:11px;color:var(--tx2);flex-basis:100%}
.tabs{display:flex;gap:4px;margin-bottom:12px;flex-wrap:wrap}
.tabs button{padding:7px 13px;border-radius:7px;border:1px solid var(--bd);background:var(--card);
 color:var(--tx2);font-size:12.5px;font-weight:600;cursor:pointer}
.tabs button.on{background:var(--acc);border-color:var(--acc);color:#fff}
.pane{display:none}.pane.on{display:block}
#toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);padding:10px 18px;border-radius:8px;
 font-size:13px;font-weight:600;display:none;z-index:60;color:#fff}
/* Bore-start destination picker — the only modal in the app, so kept minimal
   rather than pulling in a whole dialog system for one use. */
.modalOverlay{position:fixed;inset:0;background:rgba(0,0,0,.55);display:flex;
 align-items:center;justify-content:center;z-index:50;padding:16px}
.modalBox{background:var(--card);border:1px solid var(--bd2);border-radius:10px;
 padding:16px;width:min(420px,100%);max-height:86vh;overflow:auto}
/* One row per ad-hoc zone step (Start-button modal) — deliberately not
   .row/.lb, which assume one label-and-value pair; a step packs zone +
   minutes + simultaneous/sequential + remove into one compact line. */
.stepRow{display:flex;gap:6px;align-items:center;margin:7px 0;flex-wrap:wrap}
.stepRow select.stepZone{flex:2;min-width:120px}
.stepRow input.stepMin{flex:1;min-width:64px}
.stepRow select.stepMode{flex:1;min-width:120px}
.stepRow button{flex:none;padding:6px 10px}
pre.log{background:var(--card2);border:1px solid var(--bd);border-radius:7px;padding:11px 13px;font-size:11.5px;
 line-height:1.55;min-height:140px;max-height:300px;overflow:auto;margin:0;white-space:pre-wrap;color:var(--tx2)}
.hist{max-height:440px;overflow:auto}
.hist table{width:100%;border-collapse:collapse;font-size:12.5px}
.hist th{font-size:10px;text-transform:uppercase;letter-spacing:.5px;color:var(--tx2);
 text-align:left;padding:4px 6px;font-weight:600;position:sticky;top:0;background:var(--card)}
.hist td{padding:5px 6px;border-top:1px solid var(--bd);text-align:left;white-space:nowrap}
.hist td:nth-child(2){white-space:normal}
)CSS";

#endif // PAGE_CSS_H
