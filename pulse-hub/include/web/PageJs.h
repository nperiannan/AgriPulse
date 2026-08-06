#ifndef PAGE_JS_H
#define PAGE_JS_H

// Served as /app.js.
//
// Organised as one module per panel, each with its own load/render, over a
// shared Api helper. Adding a panel means adding an object and one entry in
// UI.panels — the same shape as the backend's api/ modules.

static const char PAGE_JS[] PROGMEM = R"JS(
var Api={
  // Every request is time-boxed. The hub serves one connection at a time, so a
  // request left hanging would otherwise stall the whole poll chain.
  fetchT:function(u,opt,ms){
    var c=new AbortController();
    var t=setTimeout(function(){c.abort();},ms||6000);
    opt=opt||{};opt.signal=c.signal;
    return fetch(u,opt).then(function(r){clearTimeout(t);
      if(r.status===401)throw new Error('unauthorised');return r;},
      function(e){clearTimeout(t);throw e;});
  },
  get:function(u){return Api.fetchT(u).then(function(r){return r.json();});},
  post:function(u,o){return Api.fetchT(u,{method:'POST',body:new URLSearchParams(o)},8000)
    .then(function(r){return r.json();});}
};

var UI={
  el:function(i){return document.getElementById(i);},
  toast:function(m,k){var t=UI.el('toast');t.textContent=m;
    t.style.background=(k==='err')?'var(--err)':'var(--acc)';t.style.display='block';
    clearTimeout(t._h);t._h=setTimeout(function(){t.style.display='none';},2600);},
  act:function(p,o,msg){return Api.post(p,o).then(function(d){
      if(d&&d.ok){if(msg)UI.toast(msg,'ok');}else UI.toast((d&&d.error)||'Failed','err');
      return d;
    }).catch(function(){UI.toast('Request failed','err');});},
  theme:function(){var d=document.documentElement;
    var n=d.getAttribute('data-t')==='light'?'':'light';
    d.setAttribute('data-t',n);try{localStorage.setItem('apt',n);}catch(e){}},
  fmt:function(v,d){return (v===undefined||v===null)?'--':Number(v).toFixed(d);},
  panels:{},
  show:function(id){
    var bs=document.querySelectorAll('.tabs button');
    for(var i=0;i<bs.length;i++)bs[i].classList.toggle('on',bs[i].dataset.p===id);
    var ps=document.querySelectorAll('.pane');
    for(var j=0;j<ps.length;j++)ps[j].classList.toggle('on',ps[j].id===id);
    var m=UI.panels[id]; if(m&&m.load)m.load();
  }
};
try{var _t=localStorage.getItem('apt');if(_t)document.documentElement.setAttribute('data-t',_t);}catch(e){}

/* ---- Power ---- */
var Power={
  poll:function(){
    return Api.get('/api/power').then(function(d){
      var h='';
      for(var i=0;i<d.phases.length;i++){var p=d.phases[i];
        h+='<tr><td>'+p.name+'</td><td'+(p.present?'':' class="dead"')+'>'+UI.fmt(p.volts,0)+
           '<span class="u">V</span></td><td>'+UI.fmt(p.amps,2)+'<span class="u">A</span></td><td>'+
           UI.fmt(p.hz,1)+'<span class="u">Hz</span></td></tr>';}
      UI.el('phBody').innerHTML=h;
      var mb=UI.el('meterBadge');
      mb.textContent=d.healthy?'meter ok':'meter fault';
      mb.className='badge '+(d.healthy?'b-ok':'b-err');
      var s=UI.el('seqB');
      s.textContent=d.seq_ok?'correct':'not verified';
      s.className='badge '+(d.seq_ok?'b-ok':'b-warn');
      var ib=UI.el('imbB');
      ib.textContent=d.imbalance>0?(d.imbalance*100).toFixed(1)+'%':'n/a';
      ib.className='badge '+(d.imbalance>0.15?'b-err':'b-off');
      var cb=UI.el('calBanner');
      if(!d.calibrated){cb.style.display='block';
        cb.textContent='Meter uncalibrated \u2014 enter the CT ratio under Protection. '+
          'Overload and dry-run trips stay disarmed, and the motor will not start, '+
          'because a start could not be verified.';}
      else cb.style.display='none';
    }).catch(function(){});
  }
};

/* ---- Motor ---- */
var Motor={
  pick:'well',
  poll:function(){
    return Api.get('/api/motor').then(function(d){
      UI.el('mState').textContent=d.state;
      var b=UI.el('mBadge');
      b.textContent=d.running?'running':(d.enabled?'idle':'disabled');
      b.className='badge '+(d.running?'b-ok':(d.state_id>=11?'b-err':'b-off'));
      UI.el('mRun').textContent=d.run_s>0?('for '+d.run_s+' s'):'';
      Motor.pick=d.selected;Motor.paint();
      UI.el('btnStart').disabled=!d.can_start;
      UI.el('btnStop').disabled=!d.running;
      UI.el('btnLock').textContent=d.lockout?'Engaged \u2014 release':'Clear \u2014 engage';
      UI.el('btnEnable').textContent=d.enabled?'Enabled \u2014 disable':'Disabled \u2014 enable';
      var h='';
      for(var i=0;i<d.checks.length;i++){var c=d.checks[i];
        h+='<li><span class="ic" style="color:'+(c.ok?'var(--ok)':'var(--err)')+'">'+
           (c.ok?'\u2713':'\u2717')+'</span><span>'+c.name+'<div class="dt">'+c.detail+'</div></span></li>';}
      UI.el('chk').innerHTML=h;
      var fb=UI.el('faultBanner');
      if(d.state_id===12){fb.style.display='block';
        fb.textContent='WELDED CONTACTOR \u2014 the motor did not stop when commanded. '+
          'Isolate the supply and inspect the contactor. This cannot be cleared remotely.';}
      else if(d.last_trip&&d.last_trip!=='ok'){fb.style.display='block';
        fb.textContent='Last trip: '+d.last_trip;}
      else fb.style.display='none';
    }).catch(function(){});
  },
  paint:function(){
    UI.el('selWell').classList.toggle('on',Motor.pick==='well');
    UI.el('selBore').classList.toggle('on',Motor.pick==='bore');
  },
  cmd:function(c,extra){
    var o={cmd:c};if(extra)for(var k in extra)o[k]=extra[k];
    return UI.act('/api/motor/cmd',o,null).then(Motor.poll);
  }
};

/* ---- Zones ---- */
var Zones={
  load:function(){
    Api.get('/api/zones').then(function(d){
      var n=UI.el('zoneNote');
      if(!d.hardware_present){n.style.display='block';
        n.textContent=d.note+' \u2014 zones are shown for layout only.';}
      else n.style.display='none';
      var h='';
      for(var i=0;i<d.zones.length;i++){var z=d.zones[i];
        h+='<div class="z'+(z.open?' on':'')+'"><div class="zn">'+z.name+'</div><div class="zs">'+
           (d.hardware_present?(z.open?'open':'closed'):'no valve board')+'</div></div>';}
      UI.el('zones').innerHTML=h;
    }).catch(function(){});
  }
};

/* ---- Protection ---- */
var F=['v_low','v_high','i_low','i_high','f_low','f_high','imbalance','inrush_s','dryrun_s'];
var Protection={
  load:function(){
    Api.get('/api/protection').then(function(d){
      F.forEach(function(k){if(UI.el(k))UI.el(k).value=d[k];});
      UI.el('cal_v_a').value=d.cal_v_a;UI.el('cal_v_b').value=d.cal_v_b;
      UI.el('cal_v_c').value=d.cal_v_c;UI.el('cal_i').value=d.cal_i;
      UI.el('calFlag').value=d.calibrated?'1':'0';
    }).catch(function(){});
  },
  save:function(){var o={};F.forEach(function(k){o[k]=UI.el(k).value;});
    UI.act('/api/protection',o,'Thresholds saved');},
  saveCal:function(){
    UI.act('/api/calibration',{v_a:UI.el('cal_v_a').value,v_b:UI.el('cal_v_b').value,
      v_c:UI.el('cal_v_c').value,i:UI.el('cal_i').value,calibrated:UI.el('calFlag').value},
      'Calibration saved').then(Motor.poll);}
};

/* ---- Schedules ---- */
var Sched={
  load:function(){
    Api.get('/schedulelist').then(function(d){
      var a=(d&&d.schedules)||[];
      var h='<div class="hint">Schedules currently target the motor. Per-zone scheduling '+
             'arrives with the valve board.</div>';
      for(var i=0;i<a.length;i++){
        if(!a[i].enabled&&!a[i].time)continue;
        h+='<div class="row"><span class="lb">'+(a[i].time||'--:--')+' for '+(a[i].duration||0)+
           ' min</span><span class="badge '+(a[i].enabled?'b-ok':'b-off')+'">'+
           (a[i].enabled?'on':'off')+'</span></div>';}
      UI.el('schedWrap').innerHTML=h;
    }).catch(function(){UI.el('schedWrap').innerHTML='<div class="hint">Unavailable.</div>';});
  }
};

/* ---- Network ---- */
var Net={
  load:function(){
    Api.get('/wifilist').then(function(d){
      var a=(d&&d.networks)||[];var h='';
      for(var i=0;i<a.length;i++){var s=a[i].ssid||a[i];
        h+='<div class="row"><span class="lb">'+s+'</span></div>';}
      UI.el('wifiNets').innerHTML=h||'<div class="hint">No saved networks.</div>';
    }).catch(function(){});
    Api.get('/mqttconfig').then(function(d){
      UI.el('mq_host').value=d.broker;UI.el('mq_port').value=d.port;
      var s=UI.el('mq_st');s.textContent=d.connected?'connected':'offline';
      s.className='badge '+(d.connected?'b-ok':'b-warn');
    }).catch(function(){});
  }
};

/* ---- System ---- */
var Sys={
  load:function(){Sys.info();Sys.logs();},
  info:function(){
    Api.get('/systeminfo').then(function(d){
      var h='';for(var k in d){h+='<div class="row"><span class="lb">'+k+'</span><span>'+d[k]+'</span></div>';}
      UI.el('sysInfo').innerHTML=h;
    }).catch(function(){});
  },
  logs:function(){
    return Api.get('/logs').then(function(d){
      var a=(d&&d.logs)||[];var s='';
      for(var i=0;i<a.length;i++)s+=(typeof a[i]==='string'?a[i]:JSON.stringify(a[i]))+'\n';
      UI.el('logs').textContent=s||'(empty)';
    }).catch(function(){});
  }
};

UI.panels={'p-prot':Protection,'p-sched':Sched,'p-net':Net,'p-sys':Sys};

/* ---- wiring ---- */
function bind(id,fn){var e=UI.el(id);if(e)e.onclick=fn;}
document.querySelectorAll('.tabs button').forEach(function(b){
  b.onclick=function(){UI.show(b.dataset.p);};});
document.querySelectorAll('.sel button').forEach(function(b){
  b.onclick=function(){Motor.pick=b.dataset.m;Motor.paint();};});

bind('btnStart',function(){Motor.cmd('start',{motor:Motor.pick});});
bind('btnStop', function(){Motor.cmd('stop');});
bind('btnLock', function(){
  Motor.cmd('lockout',{on:UI.el('btnLock').textContent.indexOf('Engaged')===0?'0':'1'});});
bind('btnEnable',function(){
  Motor.cmd('enable',{on:UI.el('btnEnable').textContent.indexOf('Enabled')===0?'0':'1'});});
bind('btnSaveProt',Protection.save);
bind('btnSaveCal',Protection.saveCal);
bind('btnAddWifi',function(){
  UI.act('/addwifi',{ssid:UI.el('wSsid').value,password:UI.el('wPass').value},'Network added').then(Net.load);});
bind('btnSaveMqtt',function(){
  UI.act('/setmqttbroker',{broker:UI.el('mq_host').value,port:UI.el('mq_port').value},'MQTT saved');});
bind('btnWebPass',function(){var p=UI.el('web_pass_inp').value;
  if(p.length<8){UI.toast('At least 8 characters','err');return;}
  UI.act('/setwebpass',{user:UI.el('web_user_inp').value,pass:p},'Web login updated \u2014 sign in again');
  UI.el('web_pass_inp').value='';});
bind('btnApPass',function(){var p=UI.el('ap_pass_inp').value;
  if(p.length<8){UI.toast('At least 8 characters','err');return;}
  UI.act('/setappass',{ap:p},'AP password updated');UI.el('ap_pass_inp').value='';});
bind('btnOtaPass',function(){var p=UI.el('ota_pass_inp').value;
  if(p.length<8){UI.toast('At least 8 characters','err');return;}
  UI.act('/setappass',{ota:p},'OTA password updated \u2014 active after reboot');
  UI.el('ota_pass_inp').value='';});
bind('btnNtp',function(){UI.act('/syncntp',{},'Time synced');});
bind('btnReboot',function(){UI.act('/reboot',{},'Rebooting');});
bind('btnLogRefresh',Sys.logs);
bind('btnLogClear',function(){UI.act('/clearlogs',{},'Cleared').then(Sys.logs);});

function tick(){UI.el('clock').textContent=new Date().toTimeString().substr(0,8);}

// One request at a time, chained, with a gap after each full cycle.
// Independent setInterval timers pile requests up faster than a single-connection
// server can drain them, which collapses into connection resets.
var POLL_MS=3000, pollFails=0;
function cycle(){
  Power.poll()
    .then(function(){return Motor.poll();})
    .then(function(){pollFails=0;})
    .catch(function(){pollFails++;})
    .then(function(){
      // Back off when the hub is struggling (it starves the AP while the Wi-Fi
      // task is attempting a station connection).
      var wait=POLL_MS*(pollFails>3?4:(pollFails>0?2:1));
      setTimeout(cycle,wait);
    });
}

Zones.load();tick();cycle();
setInterval(tick,1000);
)JS";

#endif // PAGE_JS_H
