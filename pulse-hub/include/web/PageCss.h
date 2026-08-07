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
 --tx:#11201a;--tx2:#5b6f65;--ok:#1a7f37;--warn:#9a6700;--err:#cf222e;--acc:#1a7f37;
}
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
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:13px}
.card.full{grid-column:1/-1}
.ct{font-size:11.5px;font-weight:650;text-transform:uppercase;letter-spacing:.6px;color:var(--tx2);margin:0 0 9px}
.badge{display:inline-block;padding:2px 9px;border-radius:11px;font-size:11px;font-weight:600}
.b-ok{background:rgba(63,185,80,.16);color:var(--ok)}
.b-warn{background:rgba(210,153,34,.16);color:var(--warn)}
.b-err{background:rgba(248,81,73,.16);color:var(--err)}
.b-off{background:var(--card2);color:var(--tx2)}
.banner{padding:9px 12px;border-radius:7px;font-size:12.5px;margin-bottom:11px;display:none}
.bn-warn{background:rgba(210,153,34,.12);border:1px solid var(--warn);color:var(--warn)}
.bn-err{background:rgba(248,81,73,.12);border:1px solid var(--err);color:var(--err)}
table.ph{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
table.ph th{font-size:10px;text-transform:uppercase;letter-spacing:.5px;color:var(--tx2);
 text-align:right;padding:4px 5px;font-weight:600}
table.ph th:first-child{text-align:left}
table.ph td{padding:6px 5px;text-align:right;border-top:1px solid var(--bd);font-size:15px}
table.ph td:first-child{text-align:left;font-weight:600;font-size:13px}
.u{font-size:10px;color:var(--tx2);margin-left:2px}
.dead{color:var(--err)}
.mrow{display:flex;align-items:center;gap:9px;margin-bottom:10px;flex-wrap:wrap}
.mrow .big{font-size:19px;font-weight:650}
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
ul.chk{list-style:none;margin:0;padding:0}
ul.chk li{display:flex;gap:8px;padding:5px 0;border-top:1px solid var(--bd);font-size:12.5px}
ul.chk li:first-child{border-top:none}
.ic{flex:none;width:14px;font-weight:700}
.dt{color:var(--tx2);font-size:11px}
.zg{display:grid;gap:8px;grid-template-columns:repeat(auto-fill,minmax(126px,1fr))}
.z{background:var(--card2);border:1px solid var(--bd);border-radius:8px;padding:10px;text-align:center}
.z.on{border-color:var(--acc);background:rgba(46,160,67,.13)}
.z .zn{font-size:12.5px;font-weight:600;margin-bottom:4px}
.z .zs{font-size:10.5px;color:var(--tx2);font-variant-numeric:tabular-nums}
.z .zbtn{margin-top:7px;width:100%;padding:5px 0}
.z .zbtn:disabled{opacity:.38;cursor:not-allowed}
.row{display:flex;align-items:center;gap:8px;padding:7px 0;border-top:1px solid var(--bd);flex-wrap:wrap}
.row:first-child{border-top:none}
.lb{flex:1;min-width:128px;font-size:12.5px}
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
pre.log{background:var(--card2);border:1px solid var(--bd);border-radius:7px;padding:9px;font-size:11px;
 max-height:300px;overflow:auto;margin:0;white-space:pre-wrap}
.hist{max-height:440px;overflow:auto}
.hist table{width:100%;border-collapse:collapse;font-size:12.5px}
.hist th{font-size:10px;text-transform:uppercase;letter-spacing:.5px;color:var(--tx2);
 text-align:left;padding:4px 6px;font-weight:600;position:sticky;top:0;background:var(--card)}
.hist td{padding:5px 6px;border-top:1px solid var(--bd);text-align:left;white-space:nowrap}
.hist td:nth-child(2){white-space:normal}
)CSS";

#endif // PAGE_CSS_H
