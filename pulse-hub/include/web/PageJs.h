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
  // Same as act(), but for the handful of state-changing endpoints that are
  // registered HTTP_GET (deletewifi) rather than POST.
  actGet:function(p,msg){return Api.get(p).then(function(d){
      if(d&&d.ok){if(msg)UI.toast(msg,'ok');}else UI.toast((d&&d.error)||'Failed','err');
      return d;
    }).catch(function(){UI.toast('Request failed','err');});},
  // Three themes cycled by one button: green (default) -> black -> light.
  // '' is green, so an existing stored preference keeps working unchanged.
  themes:['','black','light'],
  themeName:function(t){return t==='black'?'Black':t==='light'?'Light':'Green';},
  applyTheme:function(t){
    var d=document.documentElement;
    if(t)d.setAttribute('data-t',t);else d.removeAttribute('data-t');
    var b=UI.el('btnTheme');
    // Label shows what you'll get next, not what you're on — a button says
    // what it does.
    if(b){var nx=UI.themes[(UI.themes.indexOf(t)+1)%UI.themes.length];
      b.textContent=UI.themeName(nx);}
    try{localStorage.setItem('apt',t);}catch(e){}
  },
  theme:function(){
    var cur=document.documentElement.getAttribute('data-t')||'';
    var i=UI.themes.indexOf(cur); if(i<0)i=0;
    UI.applyTheme(UI.themes[(i+1)%UI.themes.length]);
  },
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
// Applied before first paint so the page never flashes the default theme.
// The button label is set later, once the DOM exists (see the wiring section).
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
      // Feeds the Protection tab's quick-calibrate helper (updates even while
      // that tab isn't visible — cheap, and it's fresh the moment you switch to it).
      var rv=UI.el('qc_live_v_r'),yv=UI.el('qc_live_v_y'),bv=UI.el('qc_live_v_b'),iv=UI.el('qc_live_i');
      if(rv)rv.textContent=UI.fmt(d.phases[0]&&d.phases[0].volts,1);
      if(yv)yv.textContent=UI.fmt(d.phases[1]&&d.phases[1].volts,1);
      if(bv)bv.textContent=UI.fmt(d.phases[2]&&d.phases[2].volts,1);
      if(iv)iv.textContent=UI.fmt(d.max_amps,2);
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
  namesLoaded:false,   // populate the rename inputs once, never again on poll —
                       // Zones.poll() runs every ~3s regardless of tab, and
                       // rebuilding these inputs mid-edit would erase typing.
  mmss:function(s){var m=Math.floor(s/60),r=s%60;return m+':'+(r<10?'0':'')+r;},
  esc:function(s){return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');},
  renderNames:function(zones){
    var h='';
    for(var i=0;i<zones.length;i++){
      h+='<div class="row"><span class="lb">'+(zones[i].kind==='diverter'?'Diverter':'Zone '+(i+1))
        +'</span><input class="inp w" id="zname_'+i+'" maxlength="16" value="'
        +Zones.esc(zones[i].name)+'"></div>';
    }
    UI.el('zoneNames').innerHTML=h;
  },
  saveNames:function(){
    var reqs=[];
    for(var i=0;i<8;i++){
      var el=UI.el('zname_'+i); if(!el)continue;
      var v=el.value.trim(); if(!v){UI.toast('Zone '+(i+1)+' name cannot be blank','err');return;}
      reqs.push(UI.act('/api/zones/cmd',{cmd:'rename',id:i,name:v},null));
    }
    Promise.all(reqs).then(function(){UI.toast('Zone names saved','ok');Zones.poll();});
  },
  poll:function(){
    return Api.get('/api/zones').then(function(d){
      var n=UI.el('zoneNote');
      // Simulated is a working mode, not a failure: the interlocks and timers
      // all run, nothing is energised. Say that rather than "layout only".
      if(!d.hardware_present){n.style.display='block';
        n.textContent='No valve board detected \u2014 running simulated. Zones, timers and '+
          'interlocks all work, but no valve is energised. Press Rescan after fitting the board.';}
      else if(d.bus_fault){n.style.display='block';
        n.textContent='Valve board stopped responding on I2C \u2014 commands are not reaching it.';}
      else n.style.display='none';

      // An unattended stop is the thing an operator most needs explained: they
      // come back to a dry field and every valve shut. Dry run and blocked
      // valve look identical from here, so name which one it was.
      var sb=UI.el('zoneStopBanner');
      var bad=(d.stop_cause>=3);   // dry run, blocked, supply, motor fault
      if(bad&&d.stop_reason){sb.style.display='block';
        sb.textContent='Watering stopped'+(d.stop_zone?(' on '+d.stop_zone):'')+' \u2014 '+d.stop_reason;}
      else sb.style.display='none';

      var badgeText=d.open_count+' of '+d.max_open+' open \u00b7 '+d.backend+
        (d.addr?(' @ 0x'+d.addr.toString(16).toUpperCase()):'');
      var badgeClass='badge '+(d.bus_fault?'b-err':d.open_count?'b-ok':'b-off');
      var b=UI.el('zoneBadge'); b.textContent=badgeText; b.className=badgeClass;
      var bc=UI.el('zoneBadgeCtl'); if(bc){bc.textContent=badgeText; bc.className=badgeClass;}
      var cs=UI.el('chkZonesSummary');
      if(cs)cs.textContent=d.open_count?d.open_count+' zone(s) running \u2014 open the Zones tab for detail.'
                                        :'All zones closed. Open the Zones tab to run one.';

      var full=d.open_count>=d.max_open;
      var h='';
      for(var i=0;i<d.zones.length;i++){var z=d.zones[i];
        var blocked=!z.open&&full;
        h+='<div class="z'+(z.open?' on':'')+'">'
          +'<div class="zn">'+z.name
            // The diverter sends borewell water to the well for storage rather
            // than to a field \u2014 worth marking so it isn't run expecting crop.
            +(z.kind==='diverter'?'<div class="dt">to well</div>':'')+'</div>'
          +'<div class="zs">'+(z.open
              ? (Zones.mmss(z.left_s)+' left'+(z.source==='program'?' \u00b7 program':''))
              : 'closed')+'</div>'
          +'<button class="btn-s zbtn'+(z.open?' btn-d':'')+'" data-zid="'+i+'" data-zact="'
            +(z.open?'stop':'run')+'"'+(blocked?' disabled title="Valve limit reached"':'')+'>'
            +(z.open?'Stop':'Run')+'</button>'
          +'</div>';}
      UI.el('zones').innerHTML=h;

      if(!Zones.namesLoaded){Zones.namesLoaded=true;Zones.renderNames(d.zones);}
    }).catch(function(){});
  },
  load:function(){return Zones.poll();},
  cmd:function(o){return UI.act('/api/zones/cmd',o,null).then(Zones.poll);}
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
      'Calibration saved').then(Motor.poll);},
  // Ratiometric single-point calibration: newScale = entered * oldScale / live.
  // Sound regardless of what the ADE7758's internal ADC full-scale/gain actually
  // is, because it only relies on the existing scale already being linear
  // (true for an RMS conversion) — no CT-ratio/burden formula to get wrong.
  // /api/calibration treats each field as independently optional server-side,
  // so posting just the one field being calibrated leaves the others alone.
  quickCal:function(enteredId,liveId,calField){
    var rawId={v_a:'cal_v_a',v_b:'cal_v_b',v_c:'cal_v_c',i:'cal_i'}[calField];
    var entered=parseFloat(UI.el(enteredId).value);
    var live=parseFloat(UI.el(liveId).textContent);
    var oldScale=parseFloat(UI.el(rawId).value);
    if(!entered||!live||!oldScale){
      UI.toast('Need a non-zero live reading, stored scale, and entered value','err');return;}
    var newScale=entered*oldScale/live;
    var o={};o[calField]=newScale;
    UI.act('/api/calibration',o,'Scale updated to '+newScale.toPrecision(6))
      .then(function(){Protection.load();});
  }
};

/* ---- Schedules ----
   GET /schedulelist always returns exactly MAX_SCHEDULES (10) slots, enabled
   or not — the backend has no concept of "add"/"remove", only "edit slot N
   and toggle it on". The UI mirrors that: 10 fixed rows, Save all posts every
   slot back in one shot (matches /updateAllSchedules' bulk-replace contract),
   rather than pretending schedules can be dynamically added/removed. */
var MAX_SCHEDULES=10;   // mirrors Scheduler.h — bump both together if it ever changes
var Sched={
  load:function(){
    Api.get('/schedulelist').then(function(d){
      // handleScheduleList() in HttpServer.cpp returns a bare JSON array
      // (doc.to<JsonArray>()), not {schedules:[...]} — reading d.schedules
      // here silently produced an empty list on every load.
      var a=Array.isArray(d)?d:[];
      var h='';
      for(var i=0;i<a.length;i++){var s=a[i]||{};
        h+='<div class="row">'
          +'<span class="lb">#'+(i+1)+'</span>'
          +'<input type="checkbox" id="sch_en_'+i+'"'+(s.enabled?' checked':'')+' title="Enabled">'
          +'<select id="sch_mt_'+i+'" class="inp">'
            +'<option value="0"'+(s.motorType==0?' selected':'')+'>Overhead tank</option>'
            +'<option value="1"'+(s.motorType==1?' selected':'')+'>Underground tank</option>'
          +'</select>'
          +'<input type="time" id="sch_time_'+i+'" class="inp" value="'+(s.time||'00:00')+'">'
          +'<input type="number" id="sch_dur_'+i+'" class="inp" min="1" max="240" value="'+(s.duration||10)+'"><span class="u">min</span>'
          +(s.running?' <span class="badge b-ok">running</span>':'')
          +'</div>';
      }
      UI.el('schedWrap').innerHTML=h||'<div class="hint">Unavailable.</div>';
    }).catch(function(){UI.el('schedWrap').innerHTML='<div class="hint">Unavailable.</div>';});
  },
  save:function(){
    var o={};
    for(var i=0;i<MAX_SCHEDULES;i++){
      var en=UI.el('sch_en_'+i);
      if(!en)continue;   // fewer than MAX_SCHEDULES rows rendered (load() failed/empty)
      if(en.checked) o['enabled'+i]='1';
      o['motorType'+i]=UI.el('sch_mt_'+i).value;
      o['time'+i]=UI.el('sch_time_'+i).value||'00:00';
      o['duration'+i]=UI.el('sch_dur_'+i).value||'10';
    }
    UI.act('/updateAllSchedules',o,'Schedules saved').then(Sched.load);
  }
};

/* ---- Network ---- */
var Net={
  load:function(){
    Api.get('/wifilist').then(function(d){
      var a=(d&&d.networks)||[];var h='';
      for(var i=0;i<a.length;i++){var n=a[i];
        // Mirrors WiFiManager.cpp's own backend guard (handleRemoveNetwork):
        // it refuses to drop the network currently providing the connection,
        // and refuses to drop the last saved network — either would strand
        // the device with no way back in. Disable here instead of letting
        // the request round-trip just to be told no.
        var lastOne = a.length<=1;
        var delDisabled = n.connected || lastOne;
        var delTitle = n.connected ? 'Currently connected — cannot remove'
                      : lastOne ? 'At least one saved network must remain' : 'Remove';
        h+='<div class="row wnet">'
          +'<span class="lb">'+n.ssid
            +(n.connected?' <span class="badge b-ok">connected'+(n.ip?(' '+n.ip):'')+'</span>':'')
            +'</span>'
          +'<span class="dt">#'+n.priority+'</span>'
          +'<button class="btn-s" data-act="up" data-ssid="'+n.ssid+'" data-pri="'+n.priority+'"'
            +(i===0?' disabled':'')+' title="Raise priority">&uarr;</button>'
          +'<button class="btn-s" data-act="down" data-ssid="'+n.ssid+'" data-pri="'+n.priority+'"'
            +(i===a.length-1?' disabled':'')+' title="Lower priority">&darr;</button>'
          +'<button class="btn-s btn-d" data-act="del" data-ssid="'+n.ssid+'"'
            +(delDisabled?' disabled':'')+' title="'+delTitle+'">Delete</button>'
          +'</div>';}
      UI.el('wifiNets').innerHTML=h||'<div class="hint">No saved networks.</div>';
    }).catch(function(){});
    Api.get('/mqttconfig').then(function(d){
      UI.el('mq_host').value=d.broker;UI.el('mq_port').value=d.port;
      var s=UI.el('mq_st');s.textContent=d.connected?'connected':'offline';
      s.className='badge '+(d.connected?'b-ok':'b-warn');
    }).catch(function(){});
    Api.get('/status').then(function(d){
      var u=UI.el('web_user_inp');
      if(u){u.placeholder=d.webUser||'user';}
    }).catch(function(){});
  },
  wifiAction:function(act,ssid,pri){
    if(act==='del'){
      // Re-render from the server straight away rather than optimistically
      // removing the row: the firmware can legitimately refuse the delete
      // (connected network, or the last one saved), and the refreshed list is
      // the honest answer either way.
      UI.actGet('/deletewifi?ssid='+encodeURIComponent(ssid),'Network removed').then(Net.load);
    } else if(act==='up'||act==='down'){
      var newPri=pri+(act==='up'?-1:1);
      if(newPri<1) return;
      UI.act('/setwifipriority',{ssid:ssid,priority:newPri},null).then(Net.load);
    }
  },
  bars:function(rssi){
    // -50 excellent, -60 good, -70 fair, below that weak.
    var n = rssi>=-55?4 : rssi>=-65?3 : rssi>=-75?2 : 1;
    return '▂▄▆█'.slice(0,n)+' '+rssi+'dBm';
  },
  scan:function(){
    var w=UI.el('scanWrap');
    w.innerHTML='<div class="hint">Scanning… the hub pauses briefly while its radio sweeps.</div>';
    // Longer timeout: a full scan blocks the radio for several seconds.
    Api.fetchT('/wifiscan',null,20000).then(function(r){return r.json();}).then(function(a){
      if(!a||!a.length){w.innerHTML='<div class="hint">No networks found. Try again — the hub’s own AP can mask weak signals.</div>';return;}
      a.sort(function(x,y){return y.rssi-x.rssi;});
      var h='<div class="hint" style="margin:6px 0 4px">Found '+a.length+' network'+(a.length>1?'s':'')+' — pick one to join.</div>';
      for(var i=0;i<a.length;i++){var n=a[i];
        h+='<div class="row"><span class="lb">'+n.ssid+(n.open?' <span class="badge b-warn">open</span>':'')+
           '</span><span class="dt">'+Net.bars(n.rssi)+' · ch'+n.ch+'</span>'+
           '<button class="btn-s" data-join="'+encodeURIComponent(n.ssid)+'" data-open="'+(n.open?1:0)+'">Join</button></div>';}
      w.innerHTML=h;
    }).catch(function(){w.innerHTML='<div class="hint">Scan failed or timed out.</div>';});
  },
  join:function(ssid,isOpen){
    var pass='';
    if(!isOpen){
      pass=window.prompt('Password for "'+ssid+'"');
      if(pass===null)return;           // cancelled
      if(pass.length<8){UI.toast('WPA2 passwords are at least 8 characters','err');return;}
    }
    UI.act('/addwifi',{ssid:ssid,password:pass},'Added '+ssid+' — connecting').then(function(){
      UI.el('scanWrap').innerHTML='';
      Net.load();
    });
  }
};

/* ---- Programs ---- */
var DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var Progs={
  zoneNames:[],
  hhmm:function(m){var h=Math.floor(m/60),mi=m%60;return (h<10?'0':'')+h+':'+(mi<10?'0':'')+mi;},
  load:function(){
    return Promise.all([Api.get('/api/programs'), Api.get('/api/zones')]).then(function(r){
      var d=r[0], zd=r[1];
      Progs.zoneNames=(zd.zones||[]).map(function(z){return z;});
      UI.el('pd_source').value=d.defaults.source;
      UI.el('pd_seasonal').value=d.defaults.seasonalPct;
      UI.el('pd_rain').value=d.defaults.rainDelayDays;

      var h='';
      for(var i=0;i<d.programs.length;i++){var p=d.programs[i];
        h+=Progs.card(p);
      }
      UI.el('progCards').innerHTML=h;
      // Set selects/checkboxes that can't be expressed as static HTML attrs cleanly.
      for(var j=0;j<d.programs.length;j++){var pp=d.programs[j];
        UI.el('pg_src_'+j).value=pp.source;
        UI.el('pg_daymode_'+j).value=pp.dayMode;
        Progs.showDayFields(j,pp.dayMode);
        for(var wd=0;wd<7;wd++){
          var cb=UI.el('pg_day_'+j+'_'+wd);
          if(cb)cb.checked=(pp.dayMask&(1<<wd))!==0;
        }
      }
    }).catch(function(){UI.el('progCards').innerHTML='<div class="hint">Unavailable.</div>';});
  },
  card:function(p){
    var running=p.running;
    var h='<div class="card">'
      +'<div class="ct">'+p.name+' '
      +(p.enabled?'<span class="badge b-ok">enabled</span>':'<span class="badge b-off">disabled</span>')
      +(running?' <span class="badge b-ok">running &mdash; zone '+(p.currentZone+1)+'</span>':'')
      +'</div>'
      +'<div class="row"><input type="checkbox" id="pg_en_'+p.id+'"'+(p.enabled?' checked':'')+'> <span class="lb">Enabled</span>'
        +'<input class="inp w" id="pg_name_'+p.id+'" value="'+p.name.replace(/"/g,'&quot;')+'" maxlength="15" style="max-width:160px"></div>'
      +'<div class="row"><span class="lb">Start times</span>'
        +'<input class="inp w" id="pg_starts_'+p.id+'" placeholder="05:30, 17:00" value="'
        +p.starts.map(Progs.hhmm).join(', ')+'"></div>'
      +'<div class="row"><span class="lb">Watering days</span>'
        +'<select class="inp" id="pg_daymode_'+p.id+'" onchange="Progs.showDayFields('+p.id+',this.value)">'
        +'<option value="0">Specific days</option><option value="1">Odd dates</option>'
        +'<option value="2">Even dates</option><option value="3">Every N days</option></select></div>'
      +'<div class="row" id="pg_days_'+p.id+'">';
    for(var wd=0;wd<7;wd++){
      h+='<label style="display:inline-flex;align-items:center;gap:3px;margin-right:6px">'
        +'<input type="checkbox" id="pg_day_'+p.id+'_'+wd+'">'+DAYS[wd]+'</label>';
    }
    h+='</div>'
      +'<div class="row" id="pg_interval_'+p.id+'" style="display:none">'
        +'<span class="lb">Every</span><input class="inp" type="number" min="1" max="30" id="pg_iv_'+p.id+'" value="'+((p.interval)||2)+'"><span class="u">days</span></div>'
      +'<div class="row"><span class="lb">Water source</span>'
        +'<select class="inp" id="pg_src_'+p.id+'"><option value="well">Well Motor</option><option value="bore">Bore Motor</option></select></div>'
      +'<div style="padding:8px 0 0;border-top:1px solid var(--bd)"><div class="hint" style="margin-bottom:6px">Zone run times &mdash; zones run one after another</div>';
    for(var z=0;z<Progs.zoneNames.length;z++){var zn=Progs.zoneNames[z];
      h+='<div class="row"><span class="lb">'+zn.name+(zn.kind==='diverter'?' (to well)':'')+'</span>'
        +'<input class="inp n" type="number" min="0" max="240" id="pg_zm_'+p.id+'_'+z+'" value="'+p.zoneMin[z]+'"><span class="u">min</span></div>';
    }
    h+='</div>'
      +'<div class="brow">'
        +'<button class="btn" id="btnProgSave_'+p.id+'">Save program</button>'
        +(running?'<button class="btn-s btn-d" id="btnProgStop_'+p.id+'">Stop</button>'
                 :'<button class="btn-s" id="btnProgRun_'+p.id+'">Run now</button>')
        +'<button class="btn-s btn-d" id="btnProgDelete_'+p.id+'">Delete</button>'
      +'</div></div>';
    return h;
  },
  showDayFields:function(id,mode){
    mode=String(mode);
    UI.el('pg_days_'+id).style.display=(mode==='0')?'block':'none';
    UI.el('pg_interval_'+id).style.display=(mode==='3')?'flex':'none';
  },
  save:function(id){
    var o={id:id};
    if(UI.el('pg_en_'+id).checked) o.enabled='1';
    o.name=UI.el('pg_name_'+id).value;
    o.starts=UI.el('pg_starts_'+id).value;
    o.dayMode=UI.el('pg_daymode_'+id).value;
    o.interval=UI.el('pg_iv_'+id).value;
    o.source=UI.el('pg_src_'+id).value;
    var mask=0;
    for(var wd=0;wd<7;wd++){var cb=UI.el('pg_day_'+id+'_'+wd);if(cb&&cb.checked)mask|=(1<<wd);}
    o.dayMask=mask;
    for(var z=0;z<Progs.zoneNames.length;z++){
      var el=UI.el('pg_zm_'+id+'_'+z); if(el)o['zm'+z]=el.value;
    }
    UI.act('/api/programs/save',o,'Saved').then(Progs.load);
  },
  run:function(id){UI.act('/api/programs/cmd',{cmd:'run',id:id},'Running').then(Progs.load);},
  stop:function(id){UI.act('/api/programs/cmd',{cmd:'stop',id:id},'Stopped').then(Progs.load);},
  del:function(id){
    if(!window.confirm('Delete this program? This cannot be undone.'))return;
    UI.act('/api/programs/cmd',{cmd:'delete',id:id},'Deleted').then(Progs.load);
  },
  add:function(){UI.act('/api/programs/cmd',{cmd:'create'},'Program added').then(Progs.load);}
};

/* ---- History ---- */
var Hist={
  load:function(){
    Api.get('/history').then(function(d){
      if(!d||!d.eeprom){
        UI.el('histWrap').innerHTML='<div class="hint">No EEPROM detected on this board — history logging is unavailable.</div>';
        return;
      }
      var a=d.records||[];
      if(!a.length){UI.el('histWrap').innerHTML='<div class="hint">No events recorded yet.</div>';return;}
      var h='<table><thead><tr><th>Time</th><th>Event</th><th>OH</th><th>UG</th><th>Reason</th></tr></thead><tbody>';
      for(var i=0;i<a.length;i++){var r=a[i];
        h+='<tr><td>'+r.time+'</td><td>'+r.ev+'</td><td>'+r.oh+'</td><td>'+r.ug+'</td><td>'+(r.rsnStr||'')+'</td></tr>';}
      h+='</tbody></table>';
      UI.el('histWrap').innerHTML=h;
    }).catch(function(){UI.el('histWrap').innerHTML='<div class="hint">Unavailable.</div>';});
  }
};

/* ---- System ---- */
var Sys={
  load:function(){Sys.info();Sys.logs();},
  row:function(label,value){return '<div class="row"><span class="lb">'+label+'</span><span>'+value+'</span></div>';},
  bytes:function(n){return n>=1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(0)+' KB';},
  hex:function(n){return '0x'+Number(n).toString(16).toUpperCase();},
  uptime:function(s){
    var d=Math.floor(s/86400);s%=86400;var h=Math.floor(s/3600);s%=3600;var m=Math.floor(s/60);
    var parts=[];if(d)parts.push(d+'d');if(h||d)parts.push(h+'h');parts.push(m+'m');
    return parts.join(' ');
  },
  info:function(){
    Api.get('/status').then(function(d){
      UI.el('sysFw').innerHTML=
        Sys.row('Firmware',d.fwVersion)+
        Sys.row('Transmitter FW',d.txFw)+
        Sys.row('Uptime',Sys.uptime(d.uptime))+
        Sys.row('Last restart',d.resetReason)+
        Sys.row('Chip',d.chipModel+' rev '+d.chipRev)+
        Sys.row('CPU',d.cpuCores+' cores @ '+d.cpuFreqMHz+' MHz')+
        Sys.row('MAC',d.macAddress)+
        Sys.row('ESP-IDF',d.sdkVersion)+
        Sys.row('Core split','WiFi/OTA/NTP on core 0, control loop on core 1');

      var heapPct=d.heapSize?Math.round(100*(d.heapSize-d.freeHeap)/d.heapSize):0;
      var flashPct=d.freeSketch?Math.round(100*d.sketchSize/d.freeSketch):0;
      UI.el('sysMem').innerHTML=
        Sys.row('Heap used',Sys.bytes(d.heapSize-d.freeHeap)+' / '+Sys.bytes(d.heapSize)+' ('+heapPct+'%)')+
        Sys.row('OTA slot used',Sys.bytes(d.sketchSize)+' / '+Sys.bytes(d.freeSketch)+' ('+flashPct+'%)')+
        Sys.row('Flash chip',Sys.bytes(d.flashSize)+' @ '+Math.round(d.flashSpeedHz/1000000)+' MHz')+
        Sys.row('PSRAM',d.psramSize?Sys.bytes(d.psramSize):'disabled (GPIO35 reused as relay pin)');

      var mode=(d.apEnabled&&d.staEnabled)?'AP + STA':d.apEnabled?'AP only':d.staEnabled?'STA only':'off';
      UI.el('sysRadio').innerHTML=
        Sys.row('WiFi mode',mode)+
        Sys.row('AP (hotspot)','AgriPulse @ '+d.apIP)+
        Sys.row('STA (uplink)',d.wifiConnected?(d.wifiSSID+' @ '+d.wifiIP):'not connected')+
        Sys.row('Bluetooth','disabled — not built into this firmware');

      var none=!d.rtcOk&&!d.lcdAddr&&!d.eepromOk;
      UI.el('sysPeriph').innerHTML=
        Sys.row('Board revision',d.boardRev+' &mdash; SDA GPIO'+d.i2cSda+', SCL GPIO'+d.i2cScl)+
        Sys.row('RTC (DS3231)',d.rtcOk?('OK @ '+Sys.hex(d.rtcAddr)):'<span style="color:var(--err)">not detected</span>')+
        Sys.row('LCD',d.lcdAddr?('OK @ '+Sys.hex(d.lcdAddr)):'<span style="color:var(--err)">not detected</span>')+
        Sys.row('EEPROM (history)',d.eepromOk?('OK @ '+Sys.hex(d.eepromAddr)):'<span style="color:var(--err)">not detected</span>')+
        // All three missing at once is a bus-level fault, not three dead chips.
        // Say so here rather than making someone correlate it from the boot log.
        (none?'<div class="hint" style="color:var(--warn)">Nothing is responding anywhere on the I2C bus '+
          '(a full 0x08&ndash;0x77 scan found zero devices), so this is the bus itself, not three failed parts. '+
          'Check: 4.7k&ohm; pull-ups from SDA and SCL to 3.3&nbsp;V (the ESP32 internal ones are ~45k&ohm; and are often '+
          'too weak over jumper wire), 3.3&nbsp;V and GND actually present at each module, and both signal wires seated.</div>':'');
    }).catch(function(){});
  },
  // On-demand I2C bus scan. Separate from the passive RTC/LCD/EEPROM status
  // above: those report what was found at boot, this re-probes right now, so
  // fixing a loose wire is confirmable without a reboot.
  i2cScan:function(){
    var w=UI.el('i2cScanResult');
    w.innerHTML='<div class="hint">Scanning…</div>';
    Api.get('/api/i2cscan').then(function(d){
      var h='';
      if(d.found>0){
        var addrs=d.addrs.map(function(a){return Sys.hex(a);}).join(', ');
        h='<div class="banner bn-warn" style="display:block;background:rgba(63,185,80,.12);'+
          'border-color:var(--ok);color:var(--ok)">'+d.found+' device(s) found: '+addrs+'</div>';
      } else if(d.clamped){
        h='<div class="banner" style="display:block">Bus is clamped LOW even with the internal '+
          'pull-up — something is holding a line down (stuck slave, or a short). A recovery pulse '+
          'was attempted automatically; power-cycle the board if this persists.</div>';
      } else if(!d.pullups){
        h='<div class="banner" style="display:block">Lines float LOW without a pull-up — no working '+
          'pull-up resistors on the bus. Check power at each module first: an unpowered breakout '+
          'supplies neither an ACK nor a pull-up.</div>';
      } else {
        h='<div class="banner" style="display:block">Pull-ups are present, nothing is clamping the bus, '+
          'but no device answered on SDA'+d.sda+'/SCL'+d.scl+'. Check 3.3V/GND at each module and that '+
          'both wires are actually seated.</div>';
      }
      w.innerHTML=h;
    }).catch(function(){w.innerHTML='<div class="hint">Scan failed.</div>';});
  },
  logs:function(){
    return Api.get('/logs').then(function(d){
      var a=(d&&d.logs)||[];var s='';
      for(var i=0;i<a.length;i++)s+=(typeof a[i]==='string'?a[i]:JSON.stringify(a[i]))+'\n';
      UI.el('logs').textContent=s||'(empty)';
    }).catch(function(){});
  }
};

UI.panels={'p-prot':Protection,'p-prog':Progs,'p-sched':Sched,'p-hist':Hist,'p-net':Net,'p-sys':Sys};

/* ---- wiring ---- */
function bind(id,fn){var e=UI.el(id);if(e)e.onclick=fn;}
document.querySelectorAll('.tabs button').forEach(function(b){
  b.onclick=function(){UI.show(b.dataset.p);};});
document.querySelectorAll('.sel button').forEach(function(b){
  b.onclick=function(){Motor.pick=b.dataset.m;Motor.paint();};});
// Delegated: wifiNets is re-rendered wholesale by Net.load(), so bind once on
// the (stable) container rather than re-binding after every render.
UI.el('wifiNets').addEventListener('click',function(e){
  var b=e.target.closest('button[data-act]');
  if(!b||b.disabled)return;
  Net.wifiAction(b.dataset.act,b.dataset.ssid,parseInt(b.dataset.pri,10));
});

bind('btnStart',function(){Motor.cmd('start',{motor:Motor.pick});});
bind('btnStop', function(){Motor.cmd('stop');});
bind('btnLock', function(){
  Motor.cmd('lockout',{on:UI.el('btnLock').textContent.indexOf('Engaged')===0?'0':'1'});});
bind('btnEnable',function(){
  Motor.cmd('enable',{on:UI.el('btnEnable').textContent.indexOf('Enabled')===0?'0':'1'});});
bind('btnSaveProt',Protection.save);
bind('btnSaveCal',Protection.saveCal);
bind('btnQcVR',function(){Protection.quickCal('qc_v_r','qc_live_v_r','v_a');});
bind('btnQcVY',function(){Protection.quickCal('qc_v_y','qc_live_v_y','v_b');});
bind('btnQcVB',function(){Protection.quickCal('qc_v_b','qc_live_v_b','v_c');});
bind('btnQcI', function(){Protection.quickCal('qc_i','qc_live_i','i');});
bind('btnSchedSave',Sched.save);
bind('btnSchedCancel',function(){UI.act('/cancelSchedule',{},'Active schedule cancelled').then(Sched.load);});
bind('btnSchedClear',function(){UI.act('/clearSchedules',{},'All schedules cleared').then(Sched.load);});
// Delegated, like wifiNets: the zone grid is re-rendered on every poll.
UI.el('zones').addEventListener('click',function(e){
  var b=e.target.closest('button[data-zact]');
  if(!b||b.disabled)return;
  if(b.dataset.zact==='run'){
    var m=parseInt(UI.el('zoneMins').value,10);
    if(!m||m<1){UI.toast('Enter a run time in minutes','err');return;}
    Zones.cmd({cmd:'run',id:b.dataset.zid,minutes:m});
  } else {
    Zones.cmd({cmd:'stop',id:b.dataset.zid});
  }
});
bind('btnWifiScan',Net.scan);
UI.el('scanWrap').addEventListener('click',function(e){
  var b=e.target.closest('button[data-join]');
  if(!b)return;
  Net.join(decodeURIComponent(b.dataset.join), b.dataset.open==='1');
});
bind('btnZonesStopAll',function(){Zones.cmd({cmd:'stopall'});});
bind('btnZoneRescan',function(){UI.act('/api/zones/cmd',{cmd:'rescan'},'Rescanned').then(Zones.poll);});
bind('btnZoneNamesSave',Zones.saveNames);
bind('btnProgDefaultsSave',function(){
  UI.act('/api/programs/cmd',{cmd:'defaults',source:UI.el('pd_source').value,
    seasonalPct:UI.el('pd_seasonal').value,rainDelayDays:UI.el('pd_rain').value},'Defaults saved').then(Progs.load);
});
UI.el('progCards').addEventListener('click',function(e){
  var b=e.target.closest('button[id]'); if(!b)return;
  var m=b.id.match(/^btnProg(Save|Run|Stop|Delete)_(\d+)$/); if(!m)return;
  var id=parseInt(m[2],10);
  if(m[1]==='Save')Progs.save(id);
  else if(m[1]==='Run')Progs.run(id);
  else if(m[1]==='Stop')Progs.stop(id);
  else Progs.del(id);
});
bind('btnProgAdd',Progs.add);
bind('btnHistRefresh',Hist.load);
bind('btnHistClear',function(){UI.act('/clearhistory',{},'History cleared').then(Hist.load);});
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
bind('btnI2CScan',Sys.i2cScan);
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
    .then(function(){return Zones.poll();})
    .then(function(){pollFails=0;})
    .catch(function(){pollFails++;})
    .then(function(){
      // Back off when the hub is struggling (it starves the AP while the Wi-Fi
      // task is attempting a station connection).
      var wait=POLL_MS*(pollFails>3?4:(pollFails>0?2:1));
      setTimeout(cycle,wait);
    });
}

// Sets the theme button's label to whatever comes next in the cycle.
UI.applyTheme(document.documentElement.getAttribute('data-t')||'');

tick();cycle();
setInterval(tick,1000);
)JS";

#endif // PAGE_JS_H
