#include "HttpServer.h"
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#include "MotorControl.h"
#include "Buzzer.h"
#include "LoRaManager.h"
#include "WiFiManager.h"
#include "Display.h"
#include "Scheduler.h"
#include "History.h"
#include "MQTTManager.h"
#include "RTCManager.h"
#include <WebServer.h>   // Arduino ESP32 WebServer library
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>

static WebServer server(80);

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static bool isAuthenticated() {
    // Simple bearer token check via query param ?auth=<password>
    // For basic protection on the local network.
    return true; // Open – rely on network security (home WiFi)
}

static void sendJson(int code, const String& json) {
    server.send(code, "application/json", json);
}

static void sendOk() {
    sendJson(200, "{\"ok\":true}");
}

static void sendError(const char* msg) {
    String s = "{\"ok\":false,\"error\":\"";
    s += msg;
    s += "\"}";
    sendJson(400, s);
}

// ---------------------------------------------------------------------------
//  GET /  – Main status page
// ---------------------------------------------------------------------------

static void handleRoot() {
    String html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AgriPulse</title>
<style>
:root{
  --bg:#141414;--bg2:#1f1f1f;--bg3:#262626;--bd:#303030;--bd2:#434343;
  --tx:#e8e8e8;--tx2:#8c8c8c;--tx3:#d9d9d9;
  --blue:#1890ff;--green:#52c41a;--red:#ff4d4f;--orange:#fa8c16;--gold:#faad14;
  --pill-on-bg:#162312;--pill-on-c:#52c41a;--pill-on-b:#274916;
  --pill-off-bg:#2a1215;--pill-off-c:#ff4d4f;--pill-off-b:#58181c;
  --toast-bg:#262626;--toast-bd:#434343;
  --inp-bg:#262626;--arc-bg:#303030;
}
body.light{
  --bg:#f5f5f5;--bg2:#ffffff;--bg3:#fafafa;--bd:#d9d9d9;--bd2:#bfbfbf;
  --tx:#141414;--tx2:#8c8c8c;--tx3:#262626;
  --pill-on-bg:#f6ffed;--pill-on-c:#389e0d;--pill-on-b:#b7eb8f;
  --pill-off-bg:#fff1f0;--pill-off-c:#cf1322;--pill-off-b:#ffa39e;
  --toast-bg:#ffffff;--toast-bd:#d9d9d9;
  --inp-bg:#ffffff;--arc-bg:#e8e8e8;
}
*,*::before,*::after{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--tx);margin:0;padding:16px;min-height:100vh;transition:background .2s,color .2s}
.hdr{display:flex;align-items:center;justify-content:space-between;margin-bottom:20px;padding-bottom:12px;border-bottom:1px solid var(--bd)}
.hdr h1{margin:0;font-size:18px;color:var(--blue);display:flex;align-items:center;gap:6px}
.hdr-right{display:flex;align-items:center;gap:10px}
.ver{font-size:10px;color:var(--tx2)}
#clock{font-size:12px;color:var(--tx2)}
.theme-btn{background:none;border:1px solid var(--bd2);border-radius:20px;padding:3px 10px;font-size:13px;cursor:pointer;color:var(--tx);transition:background .2s}
.theme-btn:hover{background:var(--bg3)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.card{background:var(--bg2);border:1px solid var(--bd);border-radius:12px;padding:18px;text-align:center}
.card-full{background:var(--bg2);border:1px solid var(--bd);border-radius:12px;padding:16px;margin-bottom:12px}
.ctitle{font-size:11px;color:var(--tx2);text-transform:uppercase;letter-spacing:1px;margin-bottom:14px;text-align:left}
.card .ctitle{text-align:center}
.tcircle{position:relative;display:inline-block;width:110px;height:110px;margin-bottom:10px}
.tcircle svg{transform:rotate(-90deg)}
.arc-bg{fill:none;stroke:var(--arc-bg);stroke-width:9}
.arc-fg{fill:none;stroke-width:9;stroke-linecap:round;transition:stroke-dasharray .5s,stroke .5s}
.tlabel{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:15px;font-weight:700;text-align:center;line-height:1.2}
.s-full{color:#52c41a} .s-half{color:#1890ff} .s-low{color:#fa8c16} .s-empty{color:#ff4d4f} .s-unk{color:#faad14}
.tx-lost{display:none;background:#2a1215;border:1px solid #ff4d4f;border-radius:8px;padding:8px 14px;margin-bottom:10px;color:#ff4d4f;font-size:13px;text-align:center}
.mrow{display:flex;align-items:center;justify-content:space-between;background:var(--bg3);border-radius:8px;padding:7px 12px;margin-top:8px}
.mlabel{font-size:11px;color:var(--tx2)}
.pill{display:inline-flex;align-items:center;gap:4px;padding:2px 10px;border-radius:12px;font-size:12px;font-weight:600}
.pill-on{background:var(--pill-on-bg);color:var(--pill-on-c);border:1px solid var(--pill-on-b)}
.pill-off{background:var(--pill-off-bg);color:var(--pill-off-c);border:1px solid var(--pill-off-b)}
.pill-mon{background:var(--bg2);color:var(--tx2);border:1px solid var(--bd2)}
.brow{display:flex;gap:6px;margin-top:10px}
.btn{flex:1;padding:7px 0;border:none;border-radius:6px;font-size:13px;font-weight:600;cursor:pointer;transition:opacity .15s}
.btn:hover{opacity:.85} .btn:active{opacity:.7}
.btn-p{background:var(--blue);color:#fff} .btn-d{background:var(--red);color:#fff}
.btn-g{background:var(--bd2);color:var(--tx)} .btn-o{background:var(--orange);color:#fff}
.irow{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--bd);font-size:13px}
.irow:last-child{border-bottom:none}
.ilbl{color:var(--tx2)} .ival{color:var(--tx);font-weight:500;text-align:right;max-width:65%}
.trow{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid var(--bd);font-size:13px}
.trow:last-child{border-bottom:none} .tlbl{color:var(--tx3)}
.sw{position:relative;display:inline-block;width:40px;height:22px}
.sw input{opacity:0;width:0;height:0}
.sld{position:absolute;cursor:pointer;inset:0;background:var(--bd2);border-radius:22px;transition:.3s}
.sld::before{content:"";position:absolute;height:16px;width:16px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s}
input:checked+.sld{background:var(--blue)}
input:checked+.sld::before{transform:translateX(18px)}
.inp{background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:6px 10px;font-size:13px;width:100%}
.sched-table{width:100%;border-collapse:collapse;font-size:13px}
.sched-table th{color:var(--tx2);font-weight:500;padding:6px 4px;border-bottom:1px solid var(--bd);text-align:center}
.sched-table td{padding:6px 4px;border-bottom:1px solid var(--bd);text-align:center;vertical-align:middle}
.sched-table tr:last-child td{border-bottom:none}
.sched-table input[type=time],.sched-table input[type=number]{background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:5px;padding:4px 6px;font-size:12px;width:100%}
.acts{display:flex;gap:8px;flex-wrap:wrap}
.acts .btn{flex:none;padding:7px 14px}
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:var(--toast-bg);color:var(--tx);border:1px solid var(--toast-bd);padding:8px 20px;border-radius:20px;font-size:13px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:999;white-space:nowrap}
#toast.show{opacity:1} #toast.ok{border-color:var(--pill-on-b);color:var(--pill-on-c)} #toast.err{border-color:var(--pill-off-b);color:var(--pill-off-c)}
@media(max-width:400px){.grid{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="hdr">
  <h1>&#127793; AgriPulse <span class="ver">v)rawhtml" FW_VERSION R"rawhtml(</span></h1>
  <div class="hdr-right">
    <span id="clock">--:--:--</span>
    <button class="theme-btn" id="themeBtn" onclick="toggleTheme()">&#9788; Light</button>
  </div>
</div>

<!-- Tank Cards -->
<div id="txLostBanner" class="tx-lost">&#9888; Transmitter lost &mdash; no signal received</div>
<div class="grid">
  <div class="card">
    <div class="ctitle">Underground Tank</div>
    <div class="tcircle">
      <svg width="110" height="110" viewBox="0 0 110 110">
        <circle class="arc-bg" cx="55" cy="55" r="45"/>
        <circle class="arc-fg" id="ugArc" cx="55" cy="55" r="45" stroke-dasharray="0 283"/>
      </svg>
      <div class="tlabel s-unk" id="ugLbl">--</div>
    </div>
    <div class="mrow">
      <span class="mlabel">Motor</span>
      <span class="pill pill-mon" id="ugPill">--</span>
    </div>
    <div class="brow">
      <button class="btn btn-p" onclick="mc('/undergroundmotor?state=on','UG Motor ON')">ON</button>
      <button class="btn btn-d" onclick="mc('/undergroundmotor?state=off','UG Motor OFF')">OFF</button>
    </div>
  </div>
  <div class="card">
    <div class="ctitle">Overhead Tank</div>
    <div class="tcircle">
      <svg width="110" height="110" viewBox="0 0 110 110">
        <circle class="arc-bg" cx="55" cy="55" r="45"/>
        <circle class="arc-fg" id="ohArc" cx="55" cy="55" r="45" stroke-dasharray="0 283"/>
      </svg>
      <div class="tlabel s-unk" id="ohLbl">--</div>
    </div>
    <div class="mrow">
      <span class="mlabel">Motor</span>
      <span class="pill pill-mon" id="ohPill">--</span>
    </div>
    <div class="brow">
      <button class="btn btn-p" onclick="mc('/motor?state=on','OH Motor ON')">ON</button>
      <button class="btn btn-d" onclick="mc('/motor?state=off','OH Motor OFF')">OFF</button>
    </div>
  </div>
</div>

<!-- System Info -->
<div class="card-full">
  <div class="ctitle">System</div>
  <div class="irow"><span class="ilbl">Date &amp; Time</span><span class="ival" id="sysTime">--</span></div>
  <div class="irow"><span class="ilbl">NTP Last Sync</span><span class="ival" id="ntpSyncAge">--</span></div>
  <div class="irow"><span class="ilbl">NTP Drift</span><span class="ival" id="ntpDrift">--</span></div>
  <div class="irow"><span class="ilbl">WiFi</span><span class="ival" id="wifiInfo">--</span></div>
  <div class="irow"><span class="ilbl">LoRa</span><span class="ival" id="loraInfo">--</span></div>
  <div class="irow"><span class="ilbl">Last LoRa Packet</span><span class="ival" id="loraTime">--</span></div>
</div>

<!-- Scheduler -->
<div class="card-full">
  <div class="ctitle">Motor Scheduler</div>
  <div id="schedAlert" style="display:none;background:#fff3cd;border:1px solid #ffc107;color:#856404;border-radius:6px;padding:8px 12px;margin-bottom:10px;display:none;align-items:center;gap:10px">
    <span>&#9888; A scheduled run is active.</span>
    <button class="btn btn-d" style="padding:3px 12px;font-size:12px" onclick="cancelSchedule()">&#9724; Stop &amp; Cancel</button>
  </div>
  <table class="sched-table" id="schedTable">
    <thead><tr><th>On</th><th>Motor</th><th>Start Time</th><th>Duration (min)</th><th></th></tr></thead>
    <tbody id="schedBody">
      <tr><td colspan="5" style="color:var(--tx2);padding:12px">Loading...</td></tr>
    </tbody>
  </table>
  <div style="display:flex;gap:8px;margin-top:12px;flex-wrap:wrap">
    <button id="addSchedBtn" class="btn btn-p" onclick="addSchedRow()">+ Add Entry</button>
    <button class="btn btn-p" onclick="saveSchedules()">&#128190; Save</button>
    <button class="btn btn-d" onclick="if(confirm('Clear all schedules?'))clearSchedules()">Clear All</button>
  </div>
</div>

<!-- Settings -->
<div class="card-full">
  <div class="ctitle">Settings</div>
  <div class="trow">
    <span class="tlbl">OH Display Only</span>
    <label class="sw"><input type="checkbox" id="oh_disp" onchange="saveSetting()"><span class="sld"></span></label>
  </div>
  <div class="trow">
    <span class="tlbl">UG Display Only</span>
    <label class="sw"><input type="checkbox" id="ug_disp" onchange="saveSetting()"><span class="sld"></span></label>
  </div>
  <div class="trow">
    <span class="tlbl">Ignore UG for OH Motor</span>
    <label class="sw"><input type="checkbox" id="ug_ign" onchange="saveSetting()"><span class="sld"></span></label>
  </div>
  <div class="trow">
    <span class="tlbl">Buzzer Delay Before Motor Start</span>
    <label class="sw"><input type="checkbox" id="buzz_del" onchange="saveSetting()"><span class="sld"></span></label>
  </div>
  <div class="trow">
    <span class="tlbl">OH Motor Start Level</span>
    <select id="oh_start_lvl" onchange="saveMotorThresholds()" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:4px 8px;font-size:13px">
      <option value="1">EMPTY (0 0 0)</option>
      <option value="2">LOW (0 0 1)</option>
      <option value="3">HALF (0 1 1)</option>
    </select>
  </div>
  <div class="trow">
    <span class="tlbl">OH Motor Stop Level</span>
    <select id="oh_stop_lvl" onchange="saveMotorThresholds()" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:4px 8px;font-size:13px">
      <option value="2">LOW (0 0 1)</option>
      <option value="3">HALF (0 1 1)</option>
      <option value="4">FULL (1 1 1)</option>
    </select>
  </div>
  <div class="trow">
    <span class="tlbl">OH Max Motor Runtime (min)</span>
    <input id="oh_max_run" type="number" min="5" max="60" value="20" onchange="saveMotorThresholds()" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:4px 8px;font-size:13px;width:70px">
  </div>
  <div class="trow">
    <span class="tlbl">MQTT Watchdog Reboot (min)</span>
    <input id="mqtt_wd_min" type="number" min="10" max="60" value="15" onchange="saveMotorThresholds()" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:4px 8px;font-size:13px;width:70px">
  </div>
  <div class="trow">
    <span class="tlbl">LCD Backlight</span>
    <select id="lcd_bl_mode" onchange="setLcdMode(this.value)" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:4px 8px;font-size:13px">
      <option value="auto">Auto (night on)</option>
      <option value="always_on">Always On</option>
      <option value="always_off">Always Off</option>
    </select>
  </div>
  <div class="trow" style="gap:8px">
    <span class="tlbl" style="white-space:nowrap">MQTT Password</span>
    <div style="display:flex;gap:6px;flex:1">
      <input id="mqtt_pass_inp" class="inp" type="password" placeholder="New password" style="flex:1">
      <button class="btn btn-o" style="flex:none;padding:6px 14px;font-size:13px" onclick="setMqttPass()">Set</button>
    </div>
  </div>
</div>

<!-- MQTT Broker -->
<div class="card-full">
  <div class="ctitle">MQTT Broker</div>
  <div class="trow" style="gap:8px">
    <span class="tlbl" style="white-space:nowrap">Broker Host</span>
    <input id="mqtt_broker" class="inp" type="text" placeholder="hostname or IP" style="flex:1">
  </div>
  <div class="trow" style="gap:8px">
    <span class="tlbl" style="white-space:nowrap">Port</span>
    <input id="mqtt_port" class="inp" type="number" min="1" max="65535" placeholder="1883" style="width:100px">
  </div>
  <div style="display:flex;gap:6px;margin-top:10px">
    <button class="btn btn-p" onclick="saveMqttBroker()">Save &amp; Reconnect</button>
    <button class="btn btn-g" onclick="setMqttPreset('fqdn')">Use FQDN</button>
    <button class="btn btn-g" onclick="setMqttPreset('lan')">Use LAN IP</button>
  </div>
  <div id="mqttStatus" style="font-size:11px;color:var(--tx2);margin-top:6px"></div>
</div>

<!-- WiFi Networks -->
<div class="card-full">
  <div class="ctitle">WiFi Networks</div>
  <div id="apClientsRow" style="display:none;margin-bottom:8px;font-size:12px;color:var(--tx2)">
    <b>AgriPulse AP hotspot</b> &mdash; IP: <span id="apIP"></span> &nbsp;|&nbsp;
    Clients connected to AP: <span id="apClients">0</span>
    <div style="font-size:11px;color:var(--tx2);margin-top:2px">(This lists devices joined to the <em>AgriPulse</em> hotspot, not your router network)</div>
    <ul id="apClientList" style="margin:4px 0 0 16px;padding:0;list-style:disc"></ul>
  </div>
  <div id="wifiNets" style="margin-bottom:10px"></div>
  <div style="display:flex;gap:6px;flex-wrap:wrap">
    <input id="wSsid" class="inp" type="text"     placeholder="SSID"     style="flex:1;min-width:100px">
    <input id="wPass" class="inp" type="password" placeholder="Password" style="flex:1;min-width:100px">
    <button class="btn btn-o" style="flex:none;padding:7px 14px" onclick="scanWifi()">&#128268; Scan</button>
    <button class="btn btn-p" style="flex:none;padding:7px 14px" onclick="addWifi()">Add</button>
  </div>
  <!-- Scan results dropdown -->
  <div id="scanDrop" style="display:none;margin-top:6px;background:var(--inp-bg);border:1px solid var(--bd2);border-radius:6px;max-height:200px;overflow-y:auto"></div>
</div>

<!-- Actions -->
<div class="card-full">
  <div class="ctitle">Actions</div>
  <div class="acts">
    <button class="btn btn-p" onclick="postMc('/syncntp','NTP Synced')">&#128337; Sync NTP Time</button>
    <button class="btn btn-o" onclick="if(confirm('Reboot device?'))postMc('/reboot','Rebooting...')">&#8635; Reboot</button>
    <button class="btn btn-d" onclick="if(confirm('Factory reset? All settings will be lost!'))postMc('/factoryreset','Factory Reset')">Factory Reset</button>
  </div>
</div>

<!-- Event History -->
<div class="card-full">
  <div class="ctitle" style="display:flex;justify-content:space-between;align-items:center">
    <span>Event History</span>
    <div style="display:flex;gap:6px">
      <button class="btn btn-p"  style="padding:4px 10px;font-size:11px" onclick="loadHistory()">&#8635; Load</button>
      <button class="btn btn-d"  style="padding:4px 10px;font-size:11px" onclick="clearHistoryData()">&#128465; Clear</button>
    </div>
  </div>
  <div id="histStatus" style="font-size:11px;color:var(--tx2);margin-bottom:6px"></div>
  <div style="overflow-x:auto">
    <table id="histTable" style="width:100%;border-collapse:collapse;font-size:12px;display:none">
      <thead><tr>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:left;white-space:nowrap">Time</th>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:left">Event</th>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:left">Reason</th>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:center">OH</th>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:center">UG</th>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:center">OH&#9587;</th>
        <th style="color:var(--tx2);font-weight:500;padding:5px 6px;border-bottom:1px solid var(--bd);text-align:center">UG&#9587;</th>
      </tr></thead>
      <tbody id="histBody"></tbody>
    </table>
    <div id="histEmpty" style="font-size:12px;color:var(--tx2);padding:10px 0;display:none">No history records found.</div>
  </div>
</div>

<!-- About / Device Info -->
<div class="card-full">
  <div class="ctitle">About / Device Info</div>
  <div id="aboutBox" style="font-size:12px;line-height:1.7">Loading&hellip;</div>
</div>

<!-- Logs -->
<div class="card-full">
  <div class="ctitle" style="display:flex;justify-content:space-between;align-items:center">
    <span>System Logs</span>
    <div style="display:flex;gap:6px">
      <button class="btn btn-p" style="padding:4px 10px;font-size:11px" onclick="copyLogs()">&#128203; Copy</button>
      <button class="btn btn-o" style="padding:4px 10px;font-size:11px" onclick="exportLogs()">&#11015; Export</button>
      <button class="btn btn-o" style="padding:4px 10px;font-size:11px" onclick="loadLogs()">&#8635; Refresh</button>
      <button class="btn btn-d" style="padding:4px 10px;font-size:11px" onclick="clearLogs()">&#128465; Clear</button>
    </div>
  </div>
  <div class="trow" style="margin-bottom:6px">
    <span class="tlbl">Log Level</span>
    <select id="log_level_sel" onchange="setLogLevel(this.value)" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:6px;padding:4px 8px;font-size:13px">
      <option value="info">Info (Info + Warn + Error)</option>
      <option value="debug">Debug (All)</option>
    </select>
  </div>
  <div id="logBox" style="font-family:monospace;font-size:11px;background:var(--inp-bg);border:1px solid var(--bd2);border-radius:6px;padding:10px;max-height:220px;overflow-y:auto;color:var(--tx2);white-space:pre-wrap;word-break:break-all">Loading...</div>
</div>

<div id="toast"></div>
<script>
const C=283;
// ---- Theme (default: light) ----
(function(){
  var t=localStorage.getItem('theme')||'light';
  if(t==='light'){document.body.classList.add('light');document.getElementById('themeBtn').textContent='\u262d Dark';}
})();
function toggleTheme(){
  var light=document.body.classList.toggle('light');
  localStorage.setItem('theme',light?'light':'dark');
  document.getElementById('themeBtn').textContent=light?'\u262d Dark':'\u2600 Light';
}
// ---- Tank circle arcs ----
function arc(id,lbl,state){
  var a=document.getElementById(id),l=document.getElementById(lbl);
  if(!a||!l)return;
  l.style.color='';
  if(state==='FULL'){
    a.setAttribute('stroke-dasharray',C+' '+C);a.style.stroke='#52c41a';
    l.className='tlabel s-full';l.textContent='FULL';
  } else if(state==='HALF'){
    a.setAttribute('stroke-dasharray',(C*0.5)+' '+C);a.style.stroke='#1890ff';
    l.className='tlabel s-half';l.textContent='HALF';
  } else if(state==='LOW'){
    a.setAttribute('stroke-dasharray',(C*0.2)+' '+C);a.style.stroke='#fa8c16';
    l.className='tlabel s-low';l.textContent='LOW';
  } else if(state==='EMPTY'){
    a.setAttribute('stroke-dasharray','0 '+C);a.style.stroke='#ff4d4f';
    l.className='tlabel s-empty';l.textContent='EMPTY';
  } else {
    a.setAttribute('stroke-dasharray','0 '+C);a.style.stroke='#faad14';
    l.className='tlabel s-unk';l.textContent='?';
  }
}
// ---- Motor pill ----
function pill(id,running,disp){
  var e=document.getElementById(id);if(!e)return;
  if(disp){e.className='pill pill-mon';e.textContent='MONITOR';}
  else if(running==='ON'){e.className='pill pill-on';e.innerHTML='&#9679; ON';}
  else{e.className='pill pill-off';e.innerHTML='&#9679; OFF';}
}
// ---- Toast ----
function toast(msg,type){
  var t=document.getElementById('toast');
  t.textContent=msg;t.className='show'+(type?' '+type:'');
  clearTimeout(t._t);t._t=setTimeout(function(){t.className='';},2500);
}
// ---- GET fetch ----
function mc(url,label){
  fetch(url).then(function(r){return r.json();}).then(function(d){
    toast(d.ok?(label||'OK'):d.error,d.ok?'ok':'err');refreshStatus();
  }).catch(function(){toast('No response','err');});
}
// ---- POST fetch ----
function postMc(url,label){
  fetch(url,{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    toast(d.ok?(label||'OK'):d.error,d.ok?'ok':'err');refreshStatus();
  }).catch(function(){toast('No response','err');});
}
// ---- Status poll ----
function refreshStatus(){
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    arc('ohArc','ohLbl',d.ohState);
    arc('ugArc','ugLbl',d.ugState);
    // Transmitter lost banner
    var tb=document.getElementById('txLostBanner');
    if(tb)tb.style.display=d.txLost?'block':'none';
    pill('ohPill',d.oh_motor,d.ohDisplayOnly);
    pill('ugPill',d.ug_motor,d.ugDisplayOnly);
    var wi=document.getElementById('wifiInfo');
    if(wi)wi.textContent=d.wifiConnected?(d.wifiSSID+' | '+d.wifiIP):'Disconnected';
    var lo=document.getElementById('loraInfo');
    if(lo)lo.textContent=d.loraOk?('OK | '+d.loraRSSI+' dBm | SNR '+d.loraSNR+' dB'):'Error';
    var lt=document.getElementById('loraTime');
    if(lt)lt.textContent=d.lastLoraReceived;
    var ab=document.getElementById('aboutBox');
    if(ab&&d.fwVersion!==undefined){
      var fmtB=function(n){n=n||0;return n>=1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(1)+' KB';};
      var hx=function(n){return '0x'+(n||0).toString(16).toUpperCase();};
      var okB=function(b){return b?'<span style="color:var(--green)">OK</span>':'<span style="color:var(--red)">not found</span>';};
      var ramUsed=(d.heapSize||0)-(d.freeHeap||0);
      ab.innerHTML=
        '<b>Firmware</b><br>Controller: '+d.fwVersion+'<br>Transmitter: '+(d.txFw||'?')+'<br><br>'
        +'<b>I2C devices</b><br>'
        +'LCD: '+hx(d.lcdAddr)+'<br>'
        +'EEPROM: '+hx(d.eepromAddr)+' '+okB(d.eepromOk)+'<br>'
        +'RTC (DS3231): '+hx(d.rtcAddr)+' '+okB(d.rtcOk)+'<br><br>'
        +'<b>Memory</b><br>'
        +'RAM: '+fmtB(ramUsed)+' used / '+fmtB(d.heapSize)+' ('+fmtB(d.freeHeap)+' free)<br>'
        +'Flash (sketch): '+fmtB(d.sketchSize)+' used / '+fmtB((d.sketchSize||0)+(d.freeSketch||0))+' app partition<br>'
        +'Flash chip: '+fmtB(d.flashSize);
    }
    var ck=document.getElementById('clock');
    if(ck&&d.time)ck.textContent=d.time;
    var st=document.getElementById('sysTime');
    if(st&&d.time)st.textContent=d.time;
    var na=document.getElementById('ntpSyncAge');
    if(na){
      if(!d.ntpSynced){na.textContent='Never';na.style.color='var(--err,#e74)';}else{
        var s=d.ntpSyncAge;
        var txt=s<60?s+'s ago':s<3600?Math.floor(s/60)+'m ago':Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m ago';
        na.textContent=txt;na.style.color='';
      }
    }
    var nd=document.getElementById('ntpDrift');
    if(nd){
      if(!d.ntpSynced){nd.textContent='--';}else{
        var drift=d.ntpDriftSec;
        var absDrift=Math.abs(drift);
        var driftTxt=(drift>=0?'+':'')+drift+' sec '+(absDrift>10?'⚠':'✓');
        nd.textContent=driftTxt;
        nd.style.color=absDrift>30?'var(--err,#e74)':absDrift>10?'#fa0':'';
      }
    }
    var o=document.getElementById('oh_disp'),u=document.getElementById('ug_disp');
    var i=document.getElementById('ug_ign'),b=document.getElementById('buzz_del');
    if(o)o.checked=d.ohDisplayOnly;if(u)u.checked=d.ugDisplayOnly;
    if(i)i.checked=d.ugIgnore;if(b)b.checked=d.buzzerDelay;
    var lcdSel=document.getElementById('lcd_bl_mode');
    if(lcdSel){
      var modes=['auto','always_on','always_off'];
      lcdSel.value=modes[d.lcdBlMode]||'auto';
    }
    var lls=document.getElementById('log_level_sel');
    if(lls&&d.logLevel)lls.value=d.logLevel;
    // Motor thresholds
    var osl=document.getElementById('oh_start_lvl');
    if(osl&&d.ohStartLevel!==undefined)osl.value=String(d.ohStartLevel);
    var osl2=document.getElementById('oh_stop_lvl');
    if(osl2&&d.ohStopLevel!==undefined)osl2.value=String(d.ohStopLevel);
    var omr=document.getElementById('oh_max_run');
    if(omr&&d.ohMaxRunMin!==undefined)omr.value=String(d.ohMaxRunMin);
    var mwd=document.getElementById('mqtt_wd_min');
    if(mwd&&d.mqttWatchdogMin!==undefined)mwd.value=String(d.mqttWatchdogMin);
    // Scheduler active alert
    var sa=document.getElementById('schedAlert');
    if(sa)sa.style.display=d.schedRunning?'flex':'none';
  }).catch(function(){});
  refreshWifiList();
}
// ---- Settings ----
function saveSetting(){
  var body=new URLSearchParams();
  if(document.getElementById('oh_disp').checked)body.append('oh_disp_only','1');
  if(document.getElementById('ug_disp').checked)body.append('ug_disp_only','1');
  if(document.getElementById('ug_ign').checked)body.append('ug_ignore','1');
  if(document.getElementById('buzz_del').checked)body.append('buzzer_delay','1');
  body.append('oh_start_level',document.getElementById('oh_start_lvl').value);
  body.append('oh_stop_level',document.getElementById('oh_stop_lvl').value);
  body.append('oh_max_run',document.getElementById('oh_max_run').value);
  body.append('mqtt_watchdog_min',document.getElementById('mqtt_wd_min').value);
  fetch('/setconfig',{method:'POST',body:body})
    .then(function(){toast('Settings saved','ok');refreshStatus();})
    .catch(function(){toast('Save failed','err');});
}
function saveMotorThresholds(){saveSetting();}
// ---- WiFi ----
function addWifi(){
  var ssid=document.getElementById('wSsid').value.trim();
  var pass=document.getElementById('wPass').value;
  if(!ssid){toast('SSID required','err');return;}
  fetch('/addwifi',{method:'POST',body:new URLSearchParams({ssid:ssid,password:pass})})
    .then(function(r){return r.json();}).then(function(d){
      document.getElementById('wSsid').value='';
      document.getElementById('wPass').value='';
      refreshWifiList();
      toast(d.ok?'Network saved':'Failed',d.ok?'ok':'err');
    }).catch(function(){toast('Failed','err');});
}
function delWifi(ssid){
  fetch('/deletewifi?ssid='+encodeURIComponent(ssid)).then(function(){refreshWifiList();});
}
function scanWifi(){
  var btn=event.target;btn.disabled=true;btn.textContent='Scanning\u2026';
  var drop=document.getElementById('scanDrop');
  drop.style.display='block';drop.innerHTML='<div style="padding:10px;font-size:12px;color:var(--tx2)">Scanning\u2026</div>';
  fetch('/wifiscan').then(function(r){return r.json();}).then(function(nets){
    btn.disabled=false;btn.textContent='\ud83d\udd08 Scan';
    if(!nets.length){drop.innerHTML='<div style="padding:10px;font-size:12px;color:var(--tx2)">No networks found</div>';return;}
    nets.sort(function(a,b){return b.rssi-a.rssi;});
    var h='';
    nets.forEach(function(n){
      var bars=n.rssi>=-60?4:n.rssi>=-70?3:n.rssi>=-80?2:1;
      var sig='';for(var i=1;i<=4;i++)sig+='<span style="opacity:'+(i<=bars?'1':'0.25')+'">&#9646;</span>';
      var lock=n.open?'':'&#128274; ';
      h+='<div onclick="pickSsid(\''+n.ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')" '
        +'style="padding:8px 12px;cursor:pointer;border-bottom:1px solid var(--bd);display:flex;align-items:center;gap:8px;font-size:13px" '
        +'onmouseover="this.style.background=\'var(--inp-bg2,#eee)\'" onmouseout="this.style.background=\'\'">'
        +'<span style="letter-spacing:1px;font-size:10px;color:var(--tx2)">'+sig+'</span>'
        +'<span style="flex:1">'+lock+n.ssid+'</span>'
        +'<span style="font-size:11px;color:var(--tx2)">ch'+n.ch+' '+n.rssi+'dBm</span>'
        +'</div>';
    });
    drop.innerHTML=h;
  }).catch(function(){
    btn.disabled=false;btn.textContent='\ud83d\udd08 Scan';
    drop.innerHTML='<div style="padding:10px;font-size:12px;color:var(--tx2)">Scan failed</div>';
  });
}
function pickSsid(ssid){
  document.getElementById('wSsid').value=ssid;
  document.getElementById('scanDrop').style.display='none';
  document.getElementById('wPass').focus();
}
function setWifiPri(ssid,pri){
  fetch('/setwifipriority',{method:'POST',body:new URLSearchParams({ssid:ssid,priority:String(pri)})})
    .then(function(){refreshWifiList();});
}
function refreshWifiList(){
  fetch('/wifilist').then(function(r){return r.json();}).then(function(data){
    var nets=data.networks||data;
    var h='';
    if(Array.isArray(nets)){
      nets.forEach(function(n,i){
        h+='<div class="irow" style="gap:4px">';
        h+='<span style="font-size:11px;color:var(--tx2);min-width:22px">#'+(n.priority||i+1)+'</span>';
        h+='<span class="ilbl">'+n.ssid+(n.connected?' &#10003;':'')+(n.connected&&n.ip?' | '+n.ip:'')+'</span>';
        h+='<button class="btn btn-p" style="flex:none;padding:2px 7px;font-size:11px" title="Higher priority" onclick="setWifiPri(\''+n.ssid+'\','+(Math.max(1,(n.priority||i+1)-1))+')">&#8679;</button>';
        h+='<button class="btn btn-p" style="flex:none;padding:2px 7px;font-size:11px" title="Lower priority" onclick="setWifiPri(\''+n.ssid+'\','+(( n.priority||i+1)+1)+')">&#8681;</button>';
        h+='<button class="btn btn-d" style="flex:none;padding:3px 10px;font-size:11px" onclick="delWifi(\''+n.ssid+'\')" >Remove</button>';
        h+='</div>';
      });
    }
    document.getElementById('wifiNets').innerHTML=h||'<span style="color:var(--tx2);font-size:12px">No saved networks</span>';
    // AP clients
    var ar=document.getElementById('apClientsRow');
    if(ar&&data.apEnabled){
      ar.style.display='block';
      document.getElementById('apIP').textContent=data.apIP||'';
      var cl=data.apClients||[];
      document.getElementById('apClients').textContent=cl.length;
      var ul=document.getElementById('apClientList');
      ul.innerHTML=cl.map(function(c){
        var mac=typeof c==='string'?c:(c.mac||'?');
        var ip=typeof c==='string'?'':(c.ip||'');
        return '<li>'+(ip?'<b>'+ip+'</b> &mdash; ':'')+mac+'</li>';
      }).join('');
    } else if(ar){ar.style.display='none';}
  }).catch(function(){});
}
// ---- LCD mode ----
function setLcdMode(mode){
  fetch('/setlcdmode',{method:'POST',body:new URLSearchParams({mode:mode})})
    .then(function(r){return r.json();}).then(function(d){
      toast(d.ok?'LCD mode saved':'Failed',d.ok?'ok':'err');
    }).catch(function(){toast('Failed','err');});
}
// ---- Log level ----
function setLogLevel(level){
  fetch('/setloglevel',{method:'POST',body:new URLSearchParams({level:level})})
    .then(function(r){return r.json();}).then(function(d){
      toast(d.ok?'Log level: '+level:'Failed',d.ok?'ok':'err');
    }).catch(function(){toast('Failed','err');});
}
// ---- MQTT password ----
function setMqttPass(){
  var p=document.getElementById('mqtt_pass_inp').value;
  if(!p){toast('Password required','err');return;}
  fetch('/setmqttpass',{method:'POST',body:new URLSearchParams({pass:p})})
    .then(function(r){return r.json();}).then(function(d){
      if(d.ok){document.getElementById('mqtt_pass_inp').value='';toast('MQTT password updated','ok');}
      else toast(d.error||'Failed','err');
    }).catch(function(){toast('Failed','err');});
}
// ---- Scheduler ----
function buildSchedRow(i,s){
  var mSel='<select id="sm'+i+'" style="background:var(--inp-bg);color:var(--tx);border:1px solid var(--bd2);border-radius:5px;padding:3px 6px;font-size:12px;width:100%">'
    +'<option value="0"'+(s.motorType===0?' selected':'')+'>OH</option>'
    +'<option value="1"'+(s.motorType===1?' selected':'')+'>UG</option>'
    +'</select>';
  return '<tr id="sr'+i+'">'
    +'<td style="text-align:center"><label class="sw"><input type="checkbox" id="se'+i+'"'+(s.enabled?' checked':'')+'><span class="sld"></span></label></td>'
    +'<td>'+mSel+'</td>'
    +'<td><input type="time" id="st'+i+'" value="'+s.time+'" class="inp" style="padding:4px;font-size:12px;width:100%"></td>'
    +'<td><input type="number" id="sd'+i+'" value="'+s.duration+'" min="1" max="240" class="inp" style="padding:4px;font-size:12px;width:100%"></td>'
    +'<td style="text-align:center"><button class="btn btn-d" style="padding:3px 8px;font-size:11px" onclick="delSchedRow('+i+')">&#10005;</button></td>'
    +'</tr>';
}
function updateAddBtn(){
  var n=document.querySelectorAll('#schedBody tr[id^="sr"]').length;
  var btn=document.getElementById('addSchedBtn');
  if(btn){btn.disabled=(n>=10);btn.style.opacity=(n>=10)?'0.5':'1';}
}
function loadSchedules(){
  fetch('/schedulelist').then(function(r){return r.json();}).then(function(list){
    var h='';
    var ri=0;
    list.forEach(function(s){
      if(!s.enabled&&(!s.time||s.time==='00:00'))return;
      h+=buildSchedRow(ri,s);
      ri++;
    });
    if(ri===0)h='<tr><td colspan="5" style="color:var(--tx2);padding:12px;text-align:center">No schedules. Click \"+ Add Entry\".</td></tr>';
    document.getElementById('schedBody').innerHTML=h;
    updateAddBtn();
  }).catch(function(){});
}
function addSchedRow(){
  var n=document.querySelectorAll('#schedBody tr[id^="sr"]').length;
  if(n>=10){toast('Max 10 schedules','err');return;}
  var s={enabled:true,motorType:0,time:'06:00',duration:2};
  var tmp=document.createElement('tbody');
  tmp.innerHTML=buildSchedRow(n,s);
  document.getElementById('schedBody').appendChild(tmp.firstChild);
  updateAddBtn();
}
function delSchedRow(i){
  var row=document.getElementById('sr'+i);
  if(row)row.remove();
  document.querySelectorAll('#schedBody tr[id^="sr"]').forEach(function(r,ni){
    r.id='sr'+ni;
    r.querySelectorAll('[id]').forEach(function(el){el.id=el.id.replace(/\d+$/,String(ni));});
    var btn=r.querySelector('button');
    if(btn)btn.setAttribute('onclick','delSchedRow('+ni+')');
  });
  updateAddBtn();
  saveSchedules();
}
function saveSchedules(){
  var body=new URLSearchParams();
  var rows=document.querySelectorAll('#schedBody tr[id^="sr"]');
  var n=rows.length;
  for(var i=0;i<10;i++){
    if(i<n){
      var en=document.getElementById('se'+i);
      var mt=document.getElementById('sm'+i);
      var t=document.getElementById('st'+i);
      var d=document.getElementById('sd'+i);
      if(en&&en.checked)body.append('enabled'+i,'1');
      body.append('motorType'+i,(mt?mt.value:'0'));
      body.append('time'+i,(t?t.value:'00:00'));
      body.append('duration'+i,(d?d.value:'2'));
    } else {
      body.append('time'+i,'00:00');
      body.append('duration'+i,'2');
      body.append('motorType'+i,'0');
    }
  }
  fetch('/updateAllSchedules',{method:'POST',body:body})
    .then(function(r){return r.json();}).then(function(d){
      toast(d.ok?'Schedules saved':'Save failed',d.ok?'ok':'err');
    }).catch(function(){toast('Save failed','err');});
}
function cancelSchedule(){
  fetch('/cancelSchedule',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    toast(d.ok?'Schedule cancelled':'Failed',d.ok?'ok':'err');
    refreshStatus();
  }).catch(function(){toast('Failed','err');});
}
function clearSchedules(){
  fetch('/clearSchedules',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    toast(d.ok?'Schedules cleared':'Failed',d.ok?'ok':'err');
    loadSchedules();
  }).catch(function(){toast('Failed','err');});
}
function loadLogs(){
  fetch('/logs').then(function(r){return r.json();}).then(function(arr){
    var box=document.getElementById('logBox');
    if(!box)return;
    if(!arr||arr.length===0){box.textContent='No logs yet.';return;}
    box.textContent=arr.join('\n');
    box.scrollTop=box.scrollHeight;
  }).catch(function(){document.getElementById('logBox').textContent='Failed to load logs.';});
}
function copyLogs(){
  var t=document.getElementById('logBox').textContent;
  if(!t||t==='No logs yet.'){toast('Nothing to copy','err');return;}
  navigator.clipboard.writeText(t).then(function(){toast('Logs copied','ok');}).catch(function(){toast('Copy failed','err');});
}
function exportLogs(){
  var t=document.getElementById('logBox').textContent;
  if(!t||t==='No logs yet.'){toast('Nothing to export','err');return;}
  var a=document.createElement('a');
  a.href='data:text/plain;charset=utf-8,'+encodeURIComponent(t);
  a.download='agripulse_logs.txt';
  a.click();
}
function clearLogs(){
  if(!confirm('Clear all logs?'))return;
  fetch('/clearlogs',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    if(d.ok){document.getElementById('logBox').textContent='No logs yet.';toast('Logs cleared','ok');}
    else toast('Failed','err');
  }).catch(function(){toast('Failed','err');});
}
// ---- History ----
function loadHistory(){
  document.getElementById('histStatus').textContent='Loading...';
  fetch('/history').then(function(r){return r.json();}).then(function(d){
    var eeprom=d.eeprom?'EEPROM OK':'No EEPROM';
    document.getElementById('histStatus').textContent=eeprom+' \u2022 '+d.count+' total records (showing latest '+Math.min(d.count,100)+')';
    var rows=d.records||[];
    if(rows.length===0){
      document.getElementById('histTable').style.display='none';
      document.getElementById('histEmpty').style.display='block';
      return;
    }
    document.getElementById('histEmpty').style.display='none';
    document.getElementById('histTable').style.display='table';
    var stColor=function(s){return s==='FULL'?'var(--green)':s==='HALF'?'var(--blue)':s==='LOW'?'var(--orange)':s==='EMPTY'?'var(--red)':'var(--gold)';};
    var mColor=function(b){return b?'var(--green)':'var(--tx2)';};
    var mTxt=function(b){return b?'ON':'--';};
    var td=function(h,x){return '<td style="padding:4px 6px;border-bottom:1px solid var(--bd);'+(x||'')+'">'+h+'</td>';};
    var hhmm=function(t){return t?t.split(' ').slice(0,2).join(' '):'';};
    var fmtDur=function(sec){if(sec<0||isNaN(sec))return'';if(sec<60)return sec+'s';var m=Math.floor(sec/60),s=sec%60;return m+'m'+(s?(' '+s+'s'):'');};
    // Pair each motor ON with its following OFF into a single "run" row.
    var chrono=rows.slice().reverse(); // oldest-first
    var openOH=null,openUG=null,items=[];
    chrono.forEach(function(r){
      if(r.ev==='OH Motor ON'){openOH=r;}
      else if(r.ev==='OH Motor OFF'){items.push({run:true,m:'OH',on:openOH,off:r});openOH=null;}
      else if(r.ev==='UG Motor ON'){openUG=r;}
      else if(r.ev==='UG Motor OFF'){items.push({run:true,m:'UG',on:openUG,off:r});openUG=null;}
      else{items.push({run:false,r:r});}
    });
    if(openOH)items.push({run:true,m:'OH',on:openOH,off:null});
    if(openUG)items.push({run:true,m:'UG',on:openUG,off:null});
    items.reverse(); // newest-first
    document.getElementById('histBody').innerHTML=items.map(function(it){
      if(!it.run){
        var r=it.r;
        return '<tr>'+td(r.time,'white-space:nowrap;color:var(--tx2)')+td(r.ev)+td(r.rsnStr||'','color:var(--tx2)')
          +td(r.oh,'text-align:center;color:'+stColor(r.oh))+td(r.ug,'text-align:center;color:'+stColor(r.ug))
          +td(mTxt(r.ohM),'text-align:center;color:'+mColor(r.ohM))+td(mTxt(r.ugM),'text-align:center;color:'+mColor(r.ugM))+'</tr>';
      }
      var on=it.on,off=it.off,ref=off||on||{};
      var when=(on&&off)?(hhmm(on.time)+' \u2192 '+hhmm(off.time)):hhmm(ref.time);
      var dur=(on&&off)?fmtDur(off.ts-on.ts):(on&&!off?'running':'');
      var reason=(on?(on.rsnStr||'?'):'\u2014')+' \u2192 '+(off?(off.rsnStr||'?'):'still on');
      var durHtml=dur?(' <span style="color:var(--tx2)">('+dur+')</span>'):'';
      return '<tr>'+td(when,'white-space:nowrap;color:var(--tx2)')
        +td('<b>'+it.m+' Motor</b>'+durHtml)
        +td(reason,'color:var(--tx2)')
        +td(ref.oh||'','text-align:center;color:'+stColor(ref.oh))
        +td(ref.ug||'','text-align:center;color:'+stColor(ref.ug))
        +td(off?'\u25A0':'\u25B6','text-align:center;color:'+(off?'var(--tx2)':'var(--green)'))
        +td('','text-align:center')+'</tr>';
    }).join('');
  }).catch(function(){document.getElementById('histStatus').textContent='Failed to load';});
}
function clearHistoryData(){
  if(!confirm('Clear all event history from EEPROM?'))return;
  fetch('/clearhistory',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    if(d.ok){document.getElementById('histBody').innerHTML='';document.getElementById('histTable').style.display='none';document.getElementById('histEmpty').style.display='block';document.getElementById('histStatus').textContent='Cleared';toast('History cleared','ok');}
    else toast('Failed','err');
  }).catch(function(){toast('Failed','err');});
}
// ---- MQTT broker config ----
function loadMqttConfig(){
  fetch('/mqttconfig').then(function(r){return r.json();}).then(function(d){
    document.getElementById('mqtt_broker').value=d.broker||'';
    document.getElementById('mqtt_port').value=d.port||1883;
    document.getElementById('mqttStatus').textContent='Current: '+d.broker+':'+d.port+(d.connected?' (Connected)':' (Disconnected)');
    document.getElementById('mqttStatus').style.color=d.connected?'var(--green)':'var(--red)';
  }).catch(function(){});
}
function saveMqttBroker(){
  var b=document.getElementById('mqtt_broker').value.trim();
  var p=document.getElementById('mqtt_port').value;
  if(!b){toast('Broker host required','err');return;}
  fetch('/setmqttbroker',{method:'POST',body:new URLSearchParams({broker:b,port:p})})
    .then(function(r){return r.json();}).then(function(d){
      toast(d.ok?'MQTT broker saved, reconnecting...':'Failed',d.ok?'ok':'err');
      setTimeout(loadMqttConfig,3000);
    }).catch(function(){toast('Failed','err');});
}
function setMqttPreset(type){
  if(type==='fqdn'){
    document.getElementById('mqtt_broker').value='nperiannan-nas.freemyip.com';
    document.getElementById('mqtt_port').value='1883';
  }else{
    document.getElementById('mqtt_broker').value='150.230.129.215';
    document.getElementById('mqtt_port').value='1883';
  }
}
setInterval(refreshStatus,5000);
setInterval(loadLogs,10000);
refreshStatus();
loadSchedules();
loadLogs();
loadMqttConfig();
</script>
</body></html>)rawhtml";

    server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
//  GET /status  – JSON status for BLE app and AJAX
// ---------------------------------------------------------------------------

static void handleStatus() {
    StaticJsonDocument<1024> doc;
    doc["ugState"]           = tankStateStr(ugTankState);
    doc["ohState"]           = tankStateStr(ohTankState);
    doc["ohLastKnown"]       = tankStateStr(ohLastKnownState);
    doc["undergroundTankLevel"] = (ugTankState == TANK_STATE_FULL) ? 100
                                : (ugTankState == TANK_STATE_HALF) ? 50
                                : (ugTankState == TANK_STATE_LOW)  ? 20
                                : (ugTankState == TANK_STATE_EMPTY) ? 0 : 50;
    doc["overheadTankLevel"]    = (ohTankState == TANK_STATE_FULL) ? 100
                                : (ohTankState == TANK_STATE_HALF) ? 50
                                : (ohTankState == TANK_STATE_LOW)  ? 20
                                : (ohTankState == TANK_STATE_EMPTY) ? 0 : 50;
    doc["oh_motor"]          = ohMotorRunning ? "ON" : "OFF";
    doc["ug_motor"]          = ugMotorRunning ? "ON" : "OFF";
    doc["overheadMotorStatus"]     = ohMotorRunning ? "ON" : "OFF";
    doc["undergroundMotorStatus"]  = ugMotorRunning ? "ON" : "OFF";
    doc["ohDisplayOnly"]     = ohDisplayOnly;
    doc["ugDisplayOnly"]     = ugDisplayOnly;
    doc["ugIgnore"]          = ugIgnoreForOH;
    doc["buzzerDelay"]       = buzzerDelayEnabled;
    doc["manualAutoStop"]    = manualAutoStop;
    doc["ohStartLevel"]      = (int)ohStartLevel;
    doc["ohStopLevel"]       = (int)ohStopLevel;
    doc["ohMaxRunMin"]       = (int)ohMaxRunMin;
    doc["mqttWatchdogMin"]   = (int)mqttWatchdogMin;
    doc["loraOk"]            = isLoraOperational();
    doc["txLost"]            = isTransmitterLost();
    doc["loraRSSI"]          = getLoraRSSI();
    doc["loraSNR"]           = getLoraSNR();
    doc["lastLoraReceived"]  = lastLoraReceivedTime > 0
                               ? String((millis() - lastLoraReceivedTime) / 1000) + "s ago"
                               : "Never";
    doc["wifiConnected"]     = (WiFi.status() == WL_CONNECTED);
    doc["wifiSSID"]          = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
    doc["wifiIP"]            = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    doc["time"]              = getFormattedTime();
    doc["fwVersion"]         = FW_VERSION;
    doc["bleEnabled"]        = false;
    doc["bleConnected"]      = false;
    doc["lcdBlMode"]         = (int)lcdBacklightMode;
    doc["logLevel"]          = getLogLevel() == DEBUG ? "debug" : "info";
    doc["buzzerActive"]      = isBuzzerActive();
    doc["ohBuzzer"]          = isOHBuzzerPending();
    doc["ugBuzzer"]          = isUGBuzzerPending();
    doc["ohCd"]              = getOHStartCountdown();
    doc["ugCd"]              = getUGStartCountdown();
    doc["ohRsn"]             = (int)getOHLastReason();
    doc["ugRsn"]             = (int)getUGLastReason();
    doc["ohRej"]             = (int)getOHRejectCode();
    doc["ugRej"]             = (int)getUGRejectCode();
    doc["txFw"]              = TRANSMITTER_FW_VERSION;
    doc["ntpSynced"]         = hasNtpSynced();
    doc["ntpDriftSec"]       = getNtpDriftSeconds();
    doc["ntpSyncAge"]        = (uint32_t)getNtpSyncAgeSeconds();
    // Any schedule currently in its run window?
    bool anySchedRunning = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (schedules[i].isRunning) { anySchedRunning = true; break; }
    }
    doc["schedRunning"]      = anySchedRunning;
    // --- About / device info ---
    doc["lcdAddr"]           = getLcdAddress();
    doc["eepromAddr"]        = histEepromFound ? getEepromAddress() : 0;
    doc["eepromOk"]          = histEepromFound;
    doc["rtcAddr"]           = 0x68;
    doc["rtcOk"]             = ds3231Found;
    doc["freeHeap"]          = (uint32_t)ESP.getFreeHeap();
    doc["heapSize"]          = (uint32_t)ESP.getHeapSize();
    doc["sketchSize"]        = (uint32_t)ESP.getSketchSize();
    doc["freeSketch"]        = (uint32_t)ESP.getFreeSketchSpace();
    doc["flashSize"]         = (uint32_t)ESP.getFlashChipSize();
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  Motor endpoints
// ---------------------------------------------------------------------------

static void handleOHMotor() {
    String state = server.arg("state");
    if (state == "on")  turnOnOHMotor();
    else if (state == "off") turnOffOHMotor();
    else { sendError("Invalid state param"); return; }
    sendOk();
}

static void handleUGMotor() {
    String state = server.arg("state");
    if (state == "on")  turnOnUGMotor();
    else if (state == "off") turnOffUGMotor();
    else { sendError("Invalid state param"); return; }
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /wifiscan  – run a fresh scan and return visible APs as JSON
// ---------------------------------------------------------------------------

static void handleWifiScan() {
    int found = WiFi.scanNetworks(false, true);   // blocking, show hidden
    String json = "[";
    for (int i = 0; i < found; i++) {
        if (i > 0) json += ",";
        // Escape SSID for JSON
        String s = WiFi.SSID(i);
        s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
        bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        json += "{\"ssid\":\"" + s + "\","
                "\"rssi\":"    + String(WiFi.RSSI(i)) + ","
                "\"ch\":"      + String(WiFi.channel(i)) + ","
                "\"open\":"    + (open ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
//  GET /wifilist
// ---------------------------------------------------------------------------

static void handleWifiList() {
    server.send(200, "application/json", getStoredNetworksJson());
}

// ---------------------------------------------------------------------------
//  POST /addwifi
// ---------------------------------------------------------------------------

static void handleAddWifi() {
    String ssid = server.arg("ssid");
    String pass = server.arg("password");
    if (ssid.isEmpty()) { sendError("SSID required"); return; }
    addWifiNetwork(ssid, pass);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setwifipriority
// ---------------------------------------------------------------------------

static void handleSetWifiPriority() {
    String ssid = server.arg("ssid");
    int    pri  = server.arg("priority").toInt();
    if (ssid.isEmpty() || pri < 1) { sendError("ssid and priority required"); return; }
    setWifiPriority(ssid, pri);
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /deletewifi
// ---------------------------------------------------------------------------

static void handleDeleteWifi() {
    String ssid = server.arg("ssid");
    if (ssid.isEmpty()) { sendError("SSID required"); return; }
    removeWifiNetwork(ssid);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setconfig
// ---------------------------------------------------------------------------

static void handleSetConfig() {
    ohDisplayOnly    = server.hasArg("oh_disp_only");
    ugDisplayOnly    = server.hasArg("ug_disp_only");
    ugIgnoreForOH    = server.hasArg("ug_ignore");
    buzzerDelayEnabled = server.hasArg("buzzer_delay");
    manualAutoStop     = server.hasArg("manual_auto_stop");
    if (server.hasArg("oh_start_level")) {
        uint8_t v = (uint8_t)server.arg("oh_start_level").toInt();
        if (v >= TANK_STATE_EMPTY && v <= TANK_STATE_HALF) ohStartLevel = (TankState)v;
    }
    if (server.hasArg("oh_stop_level")) {
        uint8_t v = (uint8_t)server.arg("oh_stop_level").toInt();
        if (v >= TANK_STATE_LOW && v <= TANK_STATE_FULL) ohStopLevel = (TankState)v;
    }
    // Validate: stop must be > start
    if (ohStopLevel <= ohStartLevel) {
        ohStartLevel = TANK_STATE_EMPTY;
        ohStopLevel  = TANK_STATE_FULL;
    }
    if (server.hasArg("oh_max_run")) {
        int v = server.arg("oh_max_run").toInt();
        if (v >= 5 && v <= 60) ohMaxRunMin = (uint8_t)v;
    }
    if (server.hasArg("mqtt_watchdog_min")) {
        int v = server.arg("mqtt_watchdog_min").toInt();
        if (v >= MQTT_WATCHDOG_MIN_MIN && v <= MQTT_WATCHDOG_MAX_MIN) mqttWatchdogMin = (uint8_t)v;
    }
    saveMotorConfig();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /buzzertest
// ---------------------------------------------------------------------------

static void handleBuzzerTest() {
    startBuzzer(BUZZER_SHORT_BEEPS);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /reboot
// ---------------------------------------------------------------------------

static void handleReboot() {
    sendOk();
    delay(500);
    esp_restart();
}

// ---------------------------------------------------------------------------
//  POST /factoryreset
// ---------------------------------------------------------------------------

static void handleFactoryReset() {
    preferences.begin(NVS_MOTOR_NS,  false); preferences.clear(); preferences.end();
    preferences.begin(NVS_WIFI_NS,   false); preferences.clear(); preferences.end();
    preferences.begin(NVS_BLE_NS,    false); preferences.clear(); preferences.end();
    preferences.begin("scheduler",   false); preferences.clear(); preferences.end();
    Log(INFO, "[Web] Factory reset performed – all NVS namespaces cleared");
    sendOk();
    delay(500);
    esp_restart();
}

// ---------------------------------------------------------------------------
//  GET /systeminfo
// ---------------------------------------------------------------------------

static void handleSystemInfo() {
    StaticJsonDocument<256> doc;
    doc["fwVersion"]  = FW_VERSION;
    doc["freeHeap"]   = (int)ESP.getFreeHeap();
    doc["uptime"]     = (uint32_t)(millis() / 1000);
    doc["bleConn"]    = false;
    doc["apMode"]     = isAPMode;
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  POST /setbleenabled
// ---------------------------------------------------------------------------

static void handleSetBleEnabled() {
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /schedulelist
// ---------------------------------------------------------------------------

static void handleScheduleList() {
    StaticJsonDocument<768> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["enabled"]   = schedules[i].enabled;
        obj["motorType"] = schedules[i].motorType;
        obj["time"]      = schedules[i].time;
        obj["duration"]  = schedules[i].duration;
        obj["running"]   = schedules[i].isRunning;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  POST /updateAllSchedules
// ---------------------------------------------------------------------------

static void handleUpdateAllSchedules() {
    // Track which motor types had a running schedule that is now being disabled
    bool ohWasRunning = false, ugWasRunning = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!schedules[i].isRunning) continue;
        bool willBeDisabled = !server.hasArg("enabled" + String(i));
        String newTime = server.arg("time" + String(i));
        if (newTime.length() < 5) newTime = "00:00";
        bool cleared = (newTime == "00:00" || willBeDisabled);
        if (cleared) {
            if (schedules[i].motorType == 1) ugWasRunning = true;
            else                             ohWasRunning = true;
        }
    }
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        bool enabled   = server.hasArg("enabled" + String(i));
        uint8_t mtype  = (uint8_t)constrain(server.arg("motorType" + String(i)).toInt(), 0, 1);
        String time    = server.arg("time" + String(i));
        int duration   = server.arg("duration" + String(i)).toInt();
        if (time.length() < 5 || time.indexOf(':') == -1) time = "00:00";
        duration = constrain(duration, 1, 240);
        schedules[i].enabled   = enabled;
        schedules[i].motorType = mtype;
        schedules[i].time      = time;
        schedules[i].duration  = (uint16_t)duration;
        if (!enabled) schedules[i].isRunning = false;
    }
    saveSchedules();
    // Cancel motors whose running schedule was deleted/disabled
    if (ohWasRunning) turnOffOHMotor();
    if (ugWasRunning) turnOffUGMotor();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /cancelSchedule – stop any running scheduled motor, cancel buzzer
// ---------------------------------------------------------------------------

static void handleCancelSchedule() {
    bool ohRunning = false, ugRunning = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!schedules[i].isRunning) continue;
        if (schedules[i].motorType == 1) ugRunning = true;
        else                             ohRunning = true;
        schedules[i].isRunning = false;
        schedules[i].startTime = 0;
    }
    // Always stop both to also cancel a pending buzzer-delay start
    if (ohRunning || ugRunning) {
        if (ohRunning) turnOffOHMotor();
        if (ugRunning) turnOffUGMotor();
    } else {
        // Cancel buzzer even if isRunning wasn't set yet (fired < 10s ago)
        turnOffOHMotor();
        turnOffUGMotor();
    }
    Log(INFO, "[Sched] Active schedule cancelled via web");
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /clearSchedules
// ---------------------------------------------------------------------------

static void handleClearSchedules() {
    clearAllSchedules();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /syncntp
// ---------------------------------------------------------------------------

static void handleSyncNtp() {
    if (WiFi.status() != WL_CONNECTED) {
        server.send(200, "application/json", "{\"ok\":false,\"error\":\"Not connected to WiFi\"}");
        return;
    }
    synchronizeTime();
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /history  – returns last 100 events from EEPROM
// ---------------------------------------------------------------------------

static void handleHistory() {
    server.send(200, "application/json", getHistoryJson(100));
}

// ---------------------------------------------------------------------------
//  POST /clearhistory
// ---------------------------------------------------------------------------

static void handleClearHistory() {
    clearHistory();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setlcdmode  – set LCD backlight mode (auto / always_on / always_off)
// ---------------------------------------------------------------------------

static void handleSetLcdMode() {
    String mode = server.arg("mode");
    if      (mode == "always_on")  lcdBacklightMode = LCD_BL_ALWAYS_ON;
    else if (mode == "always_off") lcdBacklightMode = LCD_BL_ALWAYS_OFF;
    else                           lcdBacklightMode = LCD_BL_AUTO;
    saveMotorConfig();
    applyBacklightMode();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setloglevel  – set log level (info / debug)
// ---------------------------------------------------------------------------

static void handleSetLogLevel() {
    String level = server.arg("level");
    if (level == "debug") {
        setLogLevel(DEBUG);
        Log(INFO, "[Web] Log level set to DEBUG");
    } else {
        setLogLevel(INFO);
        Log(INFO, "[Web] Log level set to INFO");
    }
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setmqttpass  – update MQTT password in NVS and reconnect
// ---------------------------------------------------------------------------

static void handleSetMqttPass() {
    String newPass = server.arg("pass");
    if (newPass.isEmpty()) { sendError("pass required"); return; }
    Preferences prefs;
    prefs.begin(MQTT_NVS_NS, false);
    prefs.putString("pass", newPass);
    prefs.end();
    Log(INFO, "[Web] MQTT password updated via HTTP");
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /mqttconfig  – return current MQTT broker, port, connection status
// ---------------------------------------------------------------------------

static void handleMqttConfig() {
    StaticJsonDocument<256> doc;
    doc["broker"]    = getMQTTBroker();
    doc["port"]      = getMQTTPort();
    doc["connected"] = isMQTTConnected();
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  POST /setmqttbroker  – update MQTT broker host & port in NVS, reconnect
// ---------------------------------------------------------------------------

static void handleSetMqttBroker() {
    String broker = server.arg("broker");
    int    port   = server.arg("port").toInt();
    if (broker.isEmpty()) { sendError("broker required"); return; }
    if (port < 1 || port > 65535) port = MQTT_PORT_DEFAULT;
    Preferences prefs;
    prefs.begin(MQTT_NVS_NS, false);
    prefs.putString("broker", broker);
    prefs.putInt("port", port);
    prefs.end();
    reloadMQTTConfig();
    Log(INFO, "[Web] MQTT broker set to " + broker + ":" + String(port));
    sendOk();
}

// ---------------------------------------------------------------------------
//  Setup & loop
// ---------------------------------------------------------------------------

void setupWebServer() {
    // Note: server is an Arduino ESP32 WebServer instance
    server.on("/",                   HTTP_GET,  handleRoot);
    server.on("/status",             HTTP_GET,  handleStatus);
    server.on("/motor",              HTTP_GET,  handleOHMotor);
    server.on("/undergroundmotor",   HTTP_GET,  handleUGMotor);
    server.on("/wifiscan",            HTTP_GET,  handleWifiScan);
    server.on("/wifilist",           HTTP_GET,  handleWifiList);
    server.on("/addwifi",            HTTP_POST, handleAddWifi);
    server.on("/setwifipriority",    HTTP_POST, handleSetWifiPriority);
    server.on("/deletewifi",         HTTP_GET,  handleDeleteWifi);
    server.on("/setconfig",          HTTP_POST, handleSetConfig);
    server.on("/reboot",             HTTP_POST, handleReboot);
    server.on("/factoryreset",       HTTP_POST, handleFactoryReset);
    server.on("/systeminfo",         HTTP_GET,  handleSystemInfo);
    server.on("/setbleenabled",      HTTP_POST, handleSetBleEnabled);
    server.on("/setlcdmode",         HTTP_POST, handleSetLcdMode);
    server.on("/setloglevel",        HTTP_POST, handleSetLogLevel);
    server.on("/setmqttpass",        HTTP_POST, handleSetMqttPass);
    server.on("/mqttconfig",         HTTP_GET,  handleMqttConfig);
    server.on("/setmqttbroker",      HTTP_POST, handleSetMqttBroker);
    server.on("/schedulelist",       HTTP_GET,  handleScheduleList);
    server.on("/updateAllSchedules", HTTP_POST, handleUpdateAllSchedules);
    server.on("/cancelSchedule",     HTTP_POST, handleCancelSchedule);
    server.on("/clearSchedules",     HTTP_POST, handleClearSchedules);
    server.on("/syncntp",            HTTP_POST, handleSyncNtp);
    server.on("/history",            HTTP_GET,  handleHistory);
    server.on("/clearhistory",       HTTP_POST, handleClearHistory);
    server.on("/logs",               HTTP_GET,  [](){
        sendJson(200, getLogsJson(50));
    });
    server.on("/clearlogs",          HTTP_POST, [](){ clearLogs(); sendOk(); });
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();
    Log(INFO, "[Web] Server started");
}

void handleWebClients() {
    server.handleClient();
}
