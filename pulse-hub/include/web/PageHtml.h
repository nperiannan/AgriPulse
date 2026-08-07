#ifndef PAGE_HTML_H
#define PAGE_HTML_H

#include "Config.h"

// Served as /. Structure only — styling is /app.css, behaviour is /app.js.
//
// Laid out the way irrigation controllers conventionally are: a Control tab
// with the things an operator touches daily, and the engineering detail
// (thresholds, calibration) kept behind its own tab.

static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AgriPulse</title>
<link rel="stylesheet" href="/app.css?v=)HTML" FW_VERSION R"HTML(">
</head>
<body>
<div class="hdr">
  <h1>&#127793; AgriPulse <span class="ver" id="fw"></span></h1>
  <div class="hdr-r">
    <span id="clock">--:--:--</span>
    <span id="meterBadge" class="badge b-off">meter</span>
    <button class="btn-s" id="btnTheme" onclick="UI.theme()" title="Switch theme">Theme</button>
  </div>
</div>

<div class="wrap">
  <div id="calBanner" class="banner bn-warn"></div>
  <div id="faultBanner" class="banner bn-err"></div>

  <div class="tabs">
    <button class="on" data-p="p-ctl">Control</button>
    <button data-p="p-zones">Zones</button>
    <button data-p="p-prog">Programs</button>
    <button data-p="p-prot">Protection</button>
    <button data-p="p-hist">History</button>
    <button data-p="p-net">Network</button>
    <button data-p="p-sys">System</button>
  </div>

  <div id="p-ctl" class="pane on">
   <div class="grid">
    <div class="card">
      <div class="ct">Supply &mdash; 3 Phase</div>
      <table class="ph">
        <thead><tr><th>Phase</th><th>Volts</th><th>Amps</th><th>Hz</th></tr></thead>
        <tbody id="phBody"><tr><td colspan="4" style="text-align:left;color:var(--tx2)">Reading&hellip;</td></tr></tbody>
      </table>
      <div class="row" style="margin-top:7px"><span class="lb">Phase sequence</span><span id="seqB" class="badge b-off">--</span></div>
      <div class="row"><span class="lb">Current imbalance</span><span id="imbB" class="badge b-off">--</span></div>
    </div>

    <div class="card">
      <div class="ct">Motor</div>
      <div class="mrow">
        <span id="mState" class="big">--</span>
        <span id="mBadge" class="badge b-off">--</span>
        <span id="mRun" class="dt"></span>
      </div>
      <div class="sel">
        <button id="selWell" data-m="well">Well Motor</button>
        <button id="selBore" data-m="bore">Bore Motor</button>
      </div>
      <div class="hint" style="margin-bottom:8px">
        One starter feeds both through the changeover, so only one runs at a time.
      </div>
      <div class="brow">
        <button id="btnStart" class="btn">START</button>
        <button id="btnStop" class="btn btn-d">STOP</button>
      </div>
      <div class="row" style="margin-top:9px">
        <span class="lb">Maintenance lockout</span><button id="btnLock" class="btn-s">--</button>
      </div>
      <div class="row">
        <span class="lb">Drive enabled</span><button id="btnEnable" class="btn-s">--</button>
        <div class="hint">Disabling stops the motor from taking any command &mdash; there is no fallback control path.</div>
      </div>
    </div>

    <div class="card">
      <div class="ct">Start Preconditions</div>
      <ul id="chk" class="chk"><li><span>Loading&hellip;</span></li></ul>
    </div>

    <div class="card">
      <div class="ct">Zones <span id="zoneBadgeCtl" class="badge b-off">--</span></div>
      <div id="chkZonesSummary" class="hint">Open the Zones tab to run or stop a valve.</div>
    </div>
   </div>
  </div>

  <div id="p-zones" class="pane">
   <div class="grid">
    <div class="card full">
      <div class="ct">Zones <span id="zoneBadge" class="badge b-off">--</span></div>
      <div id="zoneStopBanner" class="banner bn-err"></div>
      <div id="zoneNote" class="banner bn-warn"></div>
      <div class="row" style="border:none;padding-top:0">
        <span class="lb">Run for</span>
        <input id="zoneMins" class="inp" type="number" min="1" max="240" value="10"><span class="u">min</span>
        <button class="btn-s btn-d" id="btnZonesStopAll">Stop all</button>
        <button class="btn-s" id="btnZoneRescan">Rescan board</button>
      </div>
      <div class="zg" id="zones"></div>
      <div class="hint" style="margin-top:8px">
        At most 3 valves open together (5 HP head limit). The valve opens before the pump starts and
        closes only after it stops, so the pump is never run against a closed system.
      </div>
      <details style="margin-top:10px">
        <summary class="hint" style="cursor:pointer">Rename zones</summary>
        <div id="zoneNames" style="margin-top:6px"></div>
        <div class="brow"><button class="btn-s" id="btnZoneNamesSave">Save names</button></div>
      </details>
    </div>
    <div id="zoneProgArea" class="grid full" style="grid-column:1/-1;display:contents">
      <div class="ct" style="grid-column:1/-1;margin:6px 0 -4px">Scheduled Programs</div>
      <div id="zoneProgList" style="display:contents"></div>
    </div>
   </div>
  </div>

  <div id="p-prot" class="pane">
   <div class="grid">
    <div class="card">
      <div class="ct">Trip Thresholds</div>
      <div class="row"><span class="lb">Voltage min / max</span>
        <input id="v_low" class="inp" type="number" step="1"><input id="v_high" class="inp" type="number" step="1">
        <div class="hint">Phase to neutral.</div></div>
      <div class="row"><span class="lb">Current min / max</span>
        <input id="i_low" class="inp" type="number" step="0.1"><input id="i_high" class="inp" type="number" step="0.1">
        <div class="hint">The minimum catches dry run and closed discharge &mdash; on a centrifugal pump those show as LOW current, not high.</div></div>
      <div class="row"><span class="lb">Frequency min / max</span>
        <input id="f_low" class="inp" type="number" step="0.5"><input id="f_high" class="inp" type="number" step="0.5"></div>
      <div class="row"><span class="lb">Max phase imbalance</span>
        <input id="imbalance" class="inp" type="number" step="0.01">
        <div class="hint">Fraction of mean current. Sustained imbalance means a failing phase.</div></div>
      <div class="row"><span class="lb">Inrush blanking (s)</span>
        <input id="inrush_s" class="inp" type="number" step="1">
        <div class="hint">Overload stays disarmed this long after start, to ride through DOL inrush.</div></div>
      <div class="row"><span class="lb">Dry-run blanking (s)</span>
        <input id="dryrun_s" class="inp" type="number" step="1">
        <div class="hint">Longer: a healthy start looks like a dry run until flow establishes.</div></div>
      <div class="brow"><button class="btn" id="btnSaveProt">Save thresholds</button></div>
    </div>

    <div class="card">
      <div class="ct">Meter Calibration</div>
      <div class="hint" style="margin-bottom:9px">
        Quick calibrate: with the supply live (motor running for current), type what a trusted
        multimeter/clamp meter reads right now and hit Apply &mdash; it back-computes the scale
        factor from the live reading below, no CT-ratio math needed. The raw scale fields still
        show the actual stored value and can be edited directly if you already know it.
      </div>
      <div class="row"><span class="lb">Phase R volts &mdash; live: <b id="qc_live_v_r">--</b> V</span>
        <input id="qc_v_r" class="inp" type="number" step="0.1" placeholder="multimeter">
        <button class="btn-s" id="btnQcVR">Apply</button></div>
      <div class="row"><span class="lb">Phase Y volts &mdash; live: <b id="qc_live_v_y">--</b> V</span>
        <input id="qc_v_y" class="inp" type="number" step="0.1" placeholder="multimeter">
        <button class="btn-s" id="btnQcVY">Apply</button></div>
      <div class="row"><span class="lb">Phase B volts &mdash; live: <b id="qc_live_v_b">--</b> V</span>
        <input id="qc_v_b" class="inp" type="number" step="0.1" placeholder="multimeter">
        <button class="btn-s" id="btnQcVB">Apply</button></div>
      <div class="row"><span class="lb">Current &mdash; live: <b id="qc_live_i">--</b> A</span>
        <input id="qc_i" class="inp" type="number" step="0.01" placeholder="clamp meter">
        <button class="btn-s" id="btnQcI">Apply</button>
        <div class="hint">One shared scale is applied to all three phases.</div></div>

      <div class="row" style="margin-top:6px"><span class="lb" style="font-weight:650">Raw scale values</span></div>
      <div class="row"><span class="lb">Volt scale R</span><input id="cal_v_a" class="inp w" type="text"></div>
      <div class="row"><span class="lb">Volt scale Y</span><input id="cal_v_b" class="inp w" type="text"></div>
      <div class="row"><span class="lb">Volt scale B</span><input id="cal_v_c" class="inp w" type="text"></div>
      <div class="row"><span class="lb">Current scale</span><input id="cal_i" class="inp w" type="text">
        <div class="hint">Amps per count, from the CT ratio and the 10 &ohm; burden.</div></div>
      <div class="row"><span class="lb">Mark calibrated</span>
        <select id="calFlag" class="inp">
          <option value="0">No &mdash; trips disarmed</option>
          <option value="1">Yes &mdash; arm trips</option>
        </select></div>
      <div class="brow"><button class="btn" id="btnSaveCal">Save calibration</button></div>
    </div>
   </div>
  </div>

  <div id="p-prog" class="pane">
   <div class="grid">
    <div class="card full">
      <div class="ct">Defaults &mdash; apply to every program</div>
      <div class="row"><span class="lb">Water source</span>
        <select id="pd_source" class="inp"><option value="well">Well Motor</option><option value="bore">Bore Motor</option></select></div>
      <div class="row"><span class="lb">Seasonal adjust</span>
        <input id="pd_seasonal" class="inp" type="number" min="10" max="200" step="5"><span class="u">% of programmed time</span></div>
      <div class="row"><span class="lb">Rain delay</span>
        <input id="pd_rain" class="inp" type="number" min="0" max="14"><span class="u">days &mdash; skip all watering</span></div>
      <div class="brow" style="justify-content:space-between">
        <button class="btn-s" id="btnProgAdd">+ Add program</button>
        <button class="btn-s" id="btnProgDefaultsSave">Save defaults</button>
      </div>
    </div>
    <div id="progArea" class="grid full" style="grid-column:1/-1;display:contents">
      <div class="ct" style="grid-column:1/-1;margin:6px 0 -4px">Saved Programs</div>
      <div id="progList" style="display:contents"></div>
      <div id="progEditors" class="grid full" style="grid-column:1/-1"></div>
    </div>
   </div>
  </div>

  <div id="p-hist" class="pane">
    <div class="card full">
      <div class="ct">History</div>
      <div class="hint" style="margin-bottom:9px">
        Motor on/off, tank-state changes, boot events and protection trips &mdash; most recent
        first, up to the last 100 of up to 8191 stored in EEPROM.
      </div>
      <div id="histWrap" class="hist">Loading&hellip;</div>
      <div class="brow" style="margin-top:9px">
        <button class="btn-s" id="btnHistRefresh">Refresh</button>
        <button class="btn-s btn-d" id="btnHistClear">Clear</button>
      </div>
    </div>
  </div>

  <div id="p-net" class="pane">
   <div class="grid">
    <div class="card">
      <div class="ct">Wi-Fi</div>
      <div class="hint" style="margin-bottom:7px">Saved networks, tried in this order.</div>
      <div id="wifiNets"></div>
      <div class="brow" style="margin-bottom:4px">
        <button class="btn" id="btnWifiScan">Scan for networks</button>
      </div>
      <div id="scanWrap" class="reserve"></div>
      <details style="margin-top:8px">
        <summary class="hint" style="cursor:pointer">Add a hidden network by name</summary>
        <div class="row" style="border:none"><input id="wSsid" class="inp w" placeholder="SSID"></div>
        <div class="row" style="border:none"><input id="wPass" class="inp w" type="password" placeholder="Password"></div>
        <div class="brow"><button class="btn" id="btnAddWifi">Add network</button></div>
      </details>
    </div>
    <div class="card">
      <div class="ct">MQTT</div>
      <div class="row"><span class="lb">Broker</span><input id="mq_host" class="inp w"></div>
      <div class="row"><span class="lb">Port</span><input id="mq_port" class="inp" type="number"></div>
      <div class="row"><span class="lb">Status</span><span id="mq_st" class="badge b-off">--</span></div>
      <div class="brow"><button class="btn" id="btnSaveMqtt">Save &amp; reconnect</button></div>
    </div>
    <div class="card">
      <div class="ct">Passwords</div>
      <div class="row"><span class="lb">Web login</span>
        <input id="web_user_inp" class="inp" placeholder="user">
        <input id="web_pass_inp" class="inp" type="password" placeholder="min 8">
        <button class="btn-s" id="btnWebPass">Set</button>
        <div class="hint">Logs in to this page and the /status API. Username field shows the current one as a placeholder — leave blank to keep it. Applies immediately; you'll need to sign in again.</div></div>
      <div class="row"><span class="lb">AP hotspot</span>
        <input id="ap_pass_inp" class="inp" type="password" placeholder="min 8">
        <button class="btn-s" id="btnApPass">Set</button>
        <div class="hint">Wi-Fi password for this device's own "AgriPulse" hotspot. Applies immediately — anything already joined to it (including this browser, if you're on it) will be dropped.</div></div>
      <div class="row"><span class="lb">OTA upload</span>
        <input id="ota_pass_inp" class="inp" type="password" placeholder="min 8">
        <button class="btn-s" id="btnOtaPass">Set</button>
        <div class="hint">Must match the --auth used when uploading. Applies after reboot.</div></div>
    </div>
   </div>
  </div>

  <div id="p-sys" class="pane">
   <div class="grid">
    <div class="card">
      <div class="ct">Firmware</div>
      <div id="sysFw"></div>
      <div class="brow" style="margin-top:9px">
        <button class="btn-s" id="btnNtp">Sync time</button>
        <button class="btn-s" id="btnReboot">Reboot</button>
      </div>
    </div>
    <div class="card">
      <div class="ct">Memory &amp; Flash</div>
      <div id="sysMem"></div>
    </div>
    <div class="card">
      <div class="ct">Radio</div>
      <div id="sysRadio"></div>
    </div>
    <div class="card">
      <div class="ct">I2C Peripherals</div>
      <div id="sysPeriph"></div>
      <div class="brow"><button class="btn-s" id="btnI2CScan">Scan I2C bus</button></div>
      <div id="i2cScanResult" class="reserve"></div>
    </div>
    <div class="card full">
      <div class="ct">Log</div>
      <pre id="logs" class="log">Loading&hellip;</pre>
      <div class="brow" style="margin-top:8px">
        <button class="btn-s" id="btnLogRefresh">Refresh</button>
        <button class="btn-s" id="btnLogClear">Clear</button>
      </div>
    </div>
   </div>
  </div>
</div>
<div id="toast"></div>
<script src="/app.js?v=)HTML" FW_VERSION R"HTML("></script>
</body>
</html>)HTML";

#endif // PAGE_HTML_H
