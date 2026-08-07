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
  lastZonesData:null,   // last /api/zones response — the manager section
                        // (below) reads this instead of its own fetch, and
                        // only ever (re)renders on an explicit open/refresh,
                        // never on poll()'s 3s tick, so it can never erase
                        // someone mid-remap or mid-add.
  mmss:function(s){var m=Math.floor(s/60),r=s%60;return m+':'+(r<10?'0':'')+r;},
  esc:function(s){return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');},
  chLabel:function(ch,d){
    var b=Math.floor(ch/8), local=ch%8;
    var board=(d.boards||[]).filter(function(x){return x.board===b;})[0];
    return board?('ch '+ch+' (Expansion Board #'+(b+1)+' relay '+(local+1)
                  +(board.present?'':' — not detected')+')')
                :('ch '+ch+' (no board declared)');
  },
  poll:function(){
    return Api.get('/api/zones').then(function(d){
      Zones.lastZonesData=d;
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

      var boardSummary=d.fully_simulated?'simulated'
        :(d.boards||[]).map(function(x){return x.present?(x.backend+' @'+x.addr):(x.addr+' missing');}).join(', ')||'no board';
      var badgeText=d.open_count+' of '+d.max_open+' open \u00b7 '+boardSummary;
      var badgeClass='badge '+(d.bus_fault?'b-err':d.open_count?'b-ok':'b-off');
      var b=UI.el('zoneBadge'); b.textContent=badgeText; b.className=badgeClass;
      var bc=UI.el('zoneBadgeCtl'); if(bc){bc.textContent=badgeText; bc.className=badgeClass;}
      var cs=UI.el('chkZonesSummary');
      if(cs)cs.textContent=d.open_count?d.open_count+' zone(s) running \u2014 open the Zones tab for detail.'
                                        :'All zones closed. Open the Zones tab to run one.';

      var full=d.open_count>=d.max_open;
      var h='';
      for(var i=0;i<d.zones.length;i++){var z=d.zones[i];
        var blocked=(!z.open&&full)||!z.active;
        var usedBy=Zones.programLinks[z.id];   // [{name,win}, ...], populated by Zones.load()
        // Start/stop time shown inline now, not hover-only \u2014 a tooltip is
        // invisible on touch and easy to miss even on desktop. First entry's
        // window is the primary line; anything past that collapses to "+N
        // more" (a zone in several enabled programs, or one program with
        // several start times) rather than overflowing the tile.
        var usedTitle=usedBy?Zones.esc(usedBy.map(function(u){return u.name+' '+u.win;}).join(', ')):'';
        var usedLine=null;
        if(usedBy&&usedBy.length){
          usedLine=Zones.esc(usedBy[0].name)+(usedBy[0].win?(' '+usedBy[0].win):'');
          if(usedBy.length>1)usedLine+=' +'+(usedBy.length-1)+' more';
        }
        h+='<div class="z'+(z.open?' on':'')+(z.active?'':' inactive')+'"'
            +(usedBy?' title="'+usedTitle+'"':'')+'>'
          +'<div class="zn">'+z.name
            // The diverter sends borewell water to the well for storage rather
            // than to a field \u2014 worth marking so it isn't run expecting crop.
            +(z.kind==='diverter'?'<div class="dt">to well</div>':'')
            +(z.active?'':'<div class="dt" style="color:var(--err)">inactive \u2014 no board</div>')
            +'</div>'
          +'<div class="zs">'+(z.open
              ? (Zones.mmss(z.left_s)+' left'+(z.source==='program'?' \u00b7 program':''))
              : (usedLine?('in '+usedLine):'closed'))+'</div>'
          +'<button class="btn-s zbtn'+(z.open?' btn-d':'')+'" data-zid="'+z.id+'" data-zact="'
            +(z.open?'stop':'run')+'"'+(blocked?' disabled title="'
              +(z.active?'Valve limit reached':'Zone is inactive \u2014 see Manage zones')+'"':'')+'>'
            +(z.open?'Stop':'Run')+'</button>'
          +'</div>';}
      UI.el('zones').innerHTML=h;
    }).catch(function(){});
  },
  // --- Manage zones: add / rename / remap / delete -----------------------
  // Deliberately NOT wired into poll()'s 3s tick \u2014 see lastZonesData above.
  // Opens fresh each time the <details> is expanded, and after every action
  // inside it, so it is always current without ever fighting a live edit.
  loadManager:function(){
    return Api.get('/api/zones').then(function(d){
      Zones.lastZonesData=d;
      Zones.renderManager(d);
    });
  },
  renderManager:function(d){
    var bh=d.fully_simulated
      ? 'No valve board detected \u2014 every channel below is a simulated placeholder for bench testing.'
      : (d.boards&&d.boards.length
          ? d.boards.map(function(x){return 'Expansion Board #'+(x.board+1)+' ('+x.addr
              +(x.present?', '+x.backend:', not detected')+', ch '+(x.board*8)+'-'+(x.board*8+7)+')';}).join(' \u00b7 ')
          : 'No board detected and nothing is simulated \u2014 this should not happen; press Rescan.');
    UI.el('zoneBoards').textContent=bh;

    var lh='';
    for(var i=0;i<d.zones.length;i++){var z=d.zones[i];
      lh+='<div class="row"><span class="lb">'+Zones.esc(z.name)
        +(z.kind==='diverter'?' <span class="dt">(diverter)</span>':'')
        +(z.active?'':' <span class="badge b-err">inactive</span>')
        +'</span><span class="dt">'+Zones.chLabel(z.channel,d)+'</span>'
        +'<button class="btn-s" data-zmrename="'+z.id+'">Rename</button>'
        +'<button class="btn-s" data-zmremap="'+z.id+'">Remap</button>'
        +'<button class="btn-s btn-d" data-zmdel="'+z.id+'"'
          +(z.open?' disabled title="Stop it first"':'')+'>Delete</button>'
        +'</div>';}
    UI.el('zoneMgrList').innerHTML=lh||'<div class="hint">No zones yet.</div>';

    // Channel picker for the "+ Add zone" row: every channel not already
    // claimed by an existing zone.
    var taken={}; d.zones.forEach(function(z){taken[z.channel]=true;});
    var opts='';
    for(var ch=0; ch<d.valve_channels; ch++){
      if(taken[ch])continue;
      opts+='<option value="'+ch+'">'+Zones.chLabel(ch,d)+'</option>';
    }
    var sel=UI.el('zn_channel');
    sel.innerHTML=opts||'<option value="">No free channels</option>';
  },
  addZone:function(){
    var name=UI.el('zn_name').value.trim();
    if(!name){UI.toast('Enter a zone name','err');return;}
    var kind=UI.el('zn_kind').value;
    var ch=UI.el('zn_channel').value;
    if(ch===''){UI.toast('No free channel to assign','err');return;}
    UI.act('/api/zones/cmd',{cmd:'create',name:name,kind:kind,channel:ch},'Zone added').then(function(r){
      if(!r||!r.ok)return;
      UI.el('zn_name').value='';
      Zones.loadManager();
      Zones.load();
    });
  },
  renameZone:function(id){
    var d=Zones.lastZonesData; if(!d)return;
    var z=d.zones.filter(function(x){return x.id===id;})[0]; if(!z)return;
    var name=window.prompt('New name for "'+z.name+'"', z.name);
    if(name===null)return;
    name=name.trim();
    if(!name){UI.toast('Name cannot be blank','err');return;}
    UI.act('/api/zones/cmd',{cmd:'rename',id:id,name:name},'Renamed').then(function(){
      Zones.loadManager();
      Zones.load();
    });
  },
  remapZone:function(id){
    var d=Zones.lastZonesData; if(!d)return;
    var z=d.zones.filter(function(x){return x.id===id;})[0]; if(!z)return;
    if(z.open){UI.toast('Stop the zone before remapping it','err');return;}
    var taken={}; d.zones.forEach(function(x){if(x.id!==id)taken[x.channel]=true;});
    var free=[];
    for(var ch=0; ch<d.valve_channels; ch++) if(!taken[ch]) free.push(ch);
    if(!free.length){UI.toast('No free channel to remap to','err');return;}
    var list=free.map(function(ch){return ch+': '+Zones.chLabel(ch,d);}).join('\n');
    var ans=window.prompt('Remap "'+z.name+'" (currently ch '+z.channel+') to which channel number?\n\n'+list, String(z.channel));
    if(ans===null)return;
    var ch=parseInt(ans,10);
    if(isNaN(ch)||free.indexOf(ch)<0){UI.toast('Not a free channel number','err');return;}
    UI.act('/api/zones/cmd',{cmd:'remap',id:id,channel:ch},'Remapped').then(function(){
      Zones.loadManager();
      Zones.load();
    });
  },
  deleteZone:function(id){
    var d=Zones.lastZonesData; if(!d)return;
    var z=d.zones.filter(function(x){return x.id===id;})[0]; if(!z)return;
    if(!window.confirm('Delete zone "'+z.name+'"? This cannot be undone; any program using it will just skip it.'))return;
    UI.act('/api/zones/cmd',{cmd:'delete',id:id},'Deleted').then(function(){
      Zones.loadManager();
      Zones.load();
    });
  },
  programLinks:{},   // {zoneId: ['Program A 05:30\u201305:45', ...]} \u2014 only enabled programs
  // One card per ENABLED program \u2014 same shape as the Programs tab's summary
  // card (Progs.render()), so "what will actually run" is visible without
  // switching tabs. Reuses Progs' helpers (zoneName/hhmm/dayModeShort/esc)
  // rather than duplicating them; Progs.zoneName() falls back to "Zone N"
  // if Progs.zoneNames hasn't been populated yet (Programs tab never opened).
  renderProgramCards:function(pd){
    var progs=(pd.programs||[]).filter(function(p){return p.enabled;});
    var h='';
    for(var i=0;i<progs.length;i++){var p=progs[i];
      var zoneNames=[];
      for(var z=0;z<p.zoneMin.length;z++){if(p.zoneMin[z]>0)zoneNames.push(Progs.zoneName(z));}
      var next=Progs.nextRunText(p);
      h+='<div class="card">'
        +'<div class="ct">'+Progs.esc(p.name)+' <span class="badge b-ok">enabled</span>'
        +(p.running?' <span class="badge b-ok">running</span>':'')
        +'</div>'
        +'<div class="row"><span class="lb">Status</span><span>'+(next||'enabled, no start time set')+'</span></div>'
        +'<div class="row"><span class="lb">Start times</span><span>'
          +(p.starts&&p.starts.length?p.starts.map(Progs.hhmm).join(', '):'&mdash;')+'</span></div>'
        +'<div class="row"><span class="lb">Days</span><span>'+Progs.dayModeShort(p)+'</span></div>'
        +'<div class="row"><span class="lb">Zones</span><span>'+(zoneNames.length?zoneNames.join(', '):'&mdash;')+'</span></div>'
        +'<div class="brow">'
          +'<button class="btn-s" data-zptoggle="'+p.id+'">Disable</button>'
          +'<button class="btn-s" data-zpedit="'+p.id+'">Edit</button>'
          +'<button class="btn-s btn-d" data-zpdel="'+p.id+'">Delete</button>'
        +'</div></div>';
    }
    UI.el('zoneProgList').innerHTML=h||'<div class="hint">No enabled programs \u2014 enable or add one on the Programs tab.</div>';
  },
  progToggle:function(id){
    UI.act('/api/programs/cmd',{cmd:'toggle',id:id},null).then(function(){Zones.load();if(Progs.lastData)Progs.load();});
  },
  progEdit:function(id){
    // Switching tabs re-triggers Progs.load() (see UI.show); toggleEdit()'s
    // own render() runs before that fetch lands and will no-op harmlessly \u2014
    // openIds[id] is already set by then, so the load's own render() opens
    // the editor once the data actually arrives.
    UI.show('p-prog');
    Progs.toggleEdit(id);
  },
  progDelete:function(id){
    if(!window.confirm('Delete this program? This cannot be undone.'))return;
    UI.act('/api/programs/cmd',{cmd:'delete',id:id},'Deleted').then(function(){
      Progs.openIds={};
      Zones.load();
      if(Progs.lastData)Progs.load();
    });
  },
  load:function(){
    // The one-time /api/programs fetch lives here (Zones tab open / rename
    // save), never in the continuous poll cycle \u2014 that cycle already caused
    // real problems once (see PageJs.h header) when it grew unbounded.
    return Api.get('/api/programs').then(function(pd){
      Zones.programLinks={};
      Zones.renderProgramCards(pd);
      (pd.programs||[]).forEach(function(p){
        if(!p.enabled)return;
        // Sequence order matches the firmware exactly: ascending zone id,
        // skipping 0-minute zones, back to back with no gap (nextZoneAfter()
        // in Programs.cpp) \u2014 so the cumulative offset computed here is the
        // real clock time that zone will actually run at, not a guess.
        var seq=[]; (p.zoneMin||[]).forEach(function(min,zid){if(min>0)seq.push(zid);});
        if(!seq.length)return;   // nothing added to this program yet - genuinely nothing to link
        var offset={}, acc=0;
        seq.forEach(function(zid){offset[zid]=acc; acc+=p.zoneMin[zid];});
        // A program can be enabled with zones added but no start time typed in
        // yet \u2014 that's a real, common in-progress state, not "nothing to show".
        // [null] keeps the zone linked (win stays null -> "no start time set")
        // instead of the whole program silently vanishing from the Zones tab.
        var starts=(p.starts&&p.starts.length)?p.starts:[null];
        starts.forEach(function(startMin){
          seq.forEach(function(zid){
            var win=null;
            if(startMin!==null){
              var s=(startMin+offset[zid])%1440, e=(s+p.zoneMin[zid])%1440;
              win=Progs.hhmm(s)+'\u2013'+Progs.hhmm(e);
            }
            if(!Zones.programLinks[zid])Zones.programLinks[zid]=[];
            Zones.programLinks[zid].push({name:p.name,win:win});
          });
        });
      });
    }).catch(function(){}).then(Zones.poll);
  },
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
    I2cExp.load();
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
    // Submit/poll: /wifiscan just queues the request on the hub's WiFi task
    // and returns instantly, so it never fights the hub's own background
    // reconnect scan for the radio. Poll /wifiscanresult until ready.
    Api.fetchT('/wifiscan',null,6000).then(function(){
      var deadline=Date.now()+15000;
      (function poll(){
        Api.fetchT('/wifiscanresult',null,6000).then(function(r){return r.json();}).then(function(d){
          if(d&&d.ready){Net.renderScan(d.networks||[]);return;}
          if(Date.now()>deadline){w.innerHTML='<div class="hint">Scan failed or timed out.</div>';UI.toast('Scan failed or timed out','err');return;}
          setTimeout(poll,600);
        }).catch(function(){w.innerHTML='<div class="hint">Scan failed or timed out.</div>';UI.toast('Scan failed or timed out','err');});
      })();
    }).catch(function(){w.innerHTML='<div class="hint">Scan failed or timed out.</div>';UI.toast('Scan failed or timed out','err');});
  },
  renderScan:function(a){
    var w=UI.el('scanWrap');
    if(!a||!a.length){w.innerHTML='<div class="hint">No networks found. Try again — the hub’s own AP can mask weak signals.</div>';UI.toast('No networks found','err');return;}
    a.sort(function(x,y){return y.rssi-x.rssi;});
    var h='<div class="hint" style="margin:6px 0 4px">Found '+a.length+' network'+(a.length>1?'s':'')+' — pick one to join.</div>';
    for(var i=0;i<a.length;i++){var n=a[i];
      h+='<div class="row"><span class="lb">'+n.ssid+(n.open?' <span class="badge b-warn">open</span>':'')+
         '</span><span class="dt">'+Net.bars(n.rssi)+' · ch'+n.ch+'</span>'+
         '<button class="btn-s" data-join="'+encodeURIComponent(n.ssid)+'" data-open="'+(n.open?1:0)+'">Join</button></div>';}
    w.innerHTML=h;
    UI.toast('Found '+a.length+' network'+(a.length>1?'s':''),'ok');
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

/* ---- I2C expansion board declarations (Network tab) ---- */
var I2cExp={
  load:function(){
    return Api.get('/api/i2cexp').then(function(d){
      var lh='';
      // Board numbering is stable (declared order) — #1 always means
      // declared[0], whether or not it happens to answer right now. This is
      // also what zones map to (see Zones.chLabel), so the number shown here
      // is exactly the one that appears in the zone channel picker.
      (d.declared||[]).forEach(function(b){
        lh+='<div class="row"><span class="lb">Expansion Board #'+(b.board+1)+' &mdash; 0x'
          +b.addr.toString(16).toUpperCase()
          +(b.present?' <span class="badge b-ok">'+b.backend+'</span>':' <span class="badge b-err">not detected</span>')
          +'</span><button class="btn-s btn-d" data-i2cdel="'+b.addr+'">Remove</button></div>';
      });
      UI.el('i2cExpList').innerHTML=lh||'<div class="hint">None declared yet — every expansion (relay) '+
        'board is auto-detected on its own, but declaring its address here guarantees it can never be '+
        'mistaken for the LCD.</div>';

      // Suggestions only, not a hard restriction — the input still takes
      // free-text hex so an address can be declared before its board is even
      // powered up, which is the exact bench-testing case this exists for.
      var opts='';
      (d.detected||[]).forEach(function(a){opts+='<option value="0x'+a.toString(16).toUpperCase()+'">';});
      UI.el('i2cexp_suggest').innerHTML=opts;
    }).catch(function(){});
  },
  add:function(){
    var raw=UI.el('i2cexp_addr').value.trim();
    if(!raw){UI.toast('Enter an address, e.g. 0x20','err');return;}
    UI.act('/api/i2cexp/cmd',{cmd:'add',addr:raw},'Declared').then(function(r){
      // Refresh the list either way — on failure this is what surfaces
      // "oh, it's already declared" instead of leaving a stale "None
      // declared yet" on screen contradicting the error toast.
      I2cExp.load();
      if(r&&r.ok)UI.el('i2cexp_addr').value='';
    });
  },
  remove:function(addr){
    UI.act('/api/i2cexp/cmd',{cmd:'remove',addr:addr},'Removed').then(I2cExp.load);
  }
};

/* ---- Programs ---- */
var DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var Progs={
  zoneNames:[],
  lastData:null,
  openIds:{},   // {id:true} — which programs currently show their full editor
  hhmm:function(m){var h=Math.floor(m/60),mi=m%60;return (h<10?'0':'')+h+':'+(mi<10?'0':'')+mi;},
  // {zoneId: minutes} for zones actually in this program (server still stores
  // all 8 as zoneMin[], 0 = not used — this just presents that as an explicit
  // add/remove list instead of eight always-visible boxes).
  minMap:function(p){var m={};for(var z=0;z<p.zoneMin.length;z++){if(p.zoneMin[z]>0)m[z]=p.zoneMin[z];}return m;},
  zoneName:function(z){var zn=Progs.zoneNames[z];return zn?(zn.name+(zn.kind==='diverter'?' (to well)':'')):('Zone '+(z+1));},
  // firstStart (minutes since midnight, may be undefined if no start time is
  // set yet) drives the "05:30 → 05:45" preview per zone — computed the same
  // way the firmware actually sequences them: ascending zone id, back to back.
  zoneRowsHtml:function(pid,map,firstStart){
    var h='', acc=0;
    Object.keys(map).map(Number).sort(function(a,b){return a-b;}).forEach(function(z){
      var win='';
      if(firstStart!==undefined&&firstStart!==null){
        win=Progs.hhmm((firstStart+acc)%1440)+' &rarr; '+Progs.hhmm((firstStart+acc+map[z])%1440);
      }
      h+='<div class="row" data-zrow="'+z+'"><span class="lb">'+Progs.zoneName(z)+'</span>'
        +'<input class="inp n" type="number" min="1" max="240" id="pg_zm_'+pid+'_'+z+'" value="'+map[z]+'">'
        +'<span class="u">min</span>'
        +(win?'<span class="dt" style="min-width:96px;text-align:right">'+win+'</span>':'')
        +'<button class="btn-s btn-d" data-zremove="'+z+'" data-pid="'+pid+'">Remove</button></div>';
      acc+=map[z];
    });
    var opts='';
    for(var z=0;z<Progs.zoneNames.length;z++){
      if(map[z]!==undefined)continue;
      opts+='<option value="'+z+'">'+Progs.zoneName(z)+'</option>';
    }
    h+=opts?('<div class="row"><select class="inp" id="pg_addzone_'+pid+'">'+opts+'</select>'
      +'<button class="btn-s" data-zadd="1" data-pid="'+pid+'">+ Add zone</button></div>')
      :'<div class="hint">Every zone is already in this program.</div>';
    return h;
  },
  readZoneBox:function(pid){
    var box=UI.el('pg_zonesbox_'+pid), m={};
    if(!box)return m;
    box.querySelectorAll('[data-zrow]').forEach(function(row){
      var z=parseInt(row.dataset.zrow,10);
      var inp=row.querySelector('input[type=number]');
      var v=inp?parseInt(inp.value,10):1;
      m[z]=(v>0?v:1);
    });
    return m;
  },
  load:function(){
    return Promise.all([Api.get('/api/programs'), Api.get('/api/zones')]).then(function(r){
      var d=r[0], zd=r[1];
      Progs.zoneNames=(zd.zones||[]).map(function(z){return z;});
      Progs.lastData=d;
      UI.el('pd_source').value=d.defaults.source;
      UI.el('pd_seasonal').value=d.defaults.seasonalPct;
      UI.el('pd_rain').value=d.defaults.rainDelayDays;
      Progs.render();
    }).catch(function(){UI.el('progList').innerHTML='<div class="hint">Unavailable.</div>';});
  },
  esc:function(s){return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;');},
  dayModeShort:function(p){
    switch(Number(p.dayMode)){
      case 0:
        var names=[]; for(var wd=0;wd<7;wd++){if(p.dayMask&(1<<wd))names.push(DAYS[wd]);}
        return names.length===7?'Every day':(names.length?names.join(','):'No days set');
      case 1: return 'Odd dates';
      case 2: return 'Even dates';
      case 3: return 'Every '+(p.interval||2)+' days';
      default: return '?';
    }
  },
  // Each program is its own small card (matches Program B/C in the original
  // design) — not a text row in a shared list. The full editor for a program
  // only appears once you press Edit on it.
  render:function(){
    var d=Progs.lastData; if(!d)return;
    var lh='';
    for(var i=0;i<d.programs.length;i++){var p=d.programs[i];
      var zoneNames=[];
      for(var z=0;z<p.zoneMin.length;z++){if(p.zoneMin[z]>0)zoneNames.push(Progs.zoneName(z));}
      var next=Progs.nextRunText(p);
      lh+='<div class="card">'
        +'<div class="ct">'+Progs.esc(p.name)+' '
        +(p.enabled?'<span class="badge b-ok">enabled</span>':'<span class="badge b-off">disabled</span>')
        +(p.running?' <span class="badge b-ok">running</span>':'')
        +'</div>'
        +'<div class="row"><span class="lb">Status</span><span>'+(p.enabled?(next||'enabled, no start time set'):'disabled')+'</span></div>'
        +'<div class="row"><span class="lb">Start times</span><span>'
          +(p.starts&&p.starts.length?p.starts.map(Progs.hhmm).join(', '):'&mdash;')+'</span></div>'
        +'<div class="row"><span class="lb">Days</span><span>'+Progs.dayModeShort(p)+'</span></div>'
        +'<div class="row"><span class="lb">Zones</span><span>'+(zoneNames.length?zoneNames.join(', '):'&mdash;')+'</span></div>'
        +'<div class="brow">'
          +'<button class="btn-s" data-ptoggle="'+p.id+'">'+(p.enabled?'Disable':'Enable')+'</button>'
          +'<button class="btn-s" data-pedit="'+p.id+'">'+(Progs.openIds[p.id]?'Close':'Edit')+'</button>'
          +'<button class="btn-s btn-d" data-pdel="'+p.id+'">Delete</button>'
        +'</div></div>';
    }
    UI.el('progList').innerHTML=lh||'<div class="hint">No programs yet — press + Add program.</div>';

    var eh='';
    for(var j=0;j<d.programs.length;j++){
      if(Progs.openIds[d.programs[j].id]) eh+=Progs.card(d.programs[j]);
    }
    UI.el('progEditors').innerHTML=eh;
    for(var k=0;k<d.programs.length;k++){var pp=d.programs[k];
      if(!Progs.openIds[pp.id])continue;
      UI.el('pg_src_'+pp.id).value=pp.source;
      UI.el('pg_daymode_'+pp.id).value=pp.dayMode;
      Progs.showDayFields(pp.id,pp.dayMode);
      for(var wd=0;wd<7;wd++){
        var cb=UI.el('pg_day_'+pp.id+'_'+wd);
        if(cb)cb.checked=(pp.dayMask&(1<<wd))!==0;
      }
    }
  },
  toggleEdit:function(id){
    if(Progs.openIds[id])delete Progs.openIds[id]; else Progs.openIds[id]=true;
    Progs.render();
  },
  toggleEnabled:function(id){UI.act('/api/programs/cmd',{cmd:'toggle',id:id},null).then(Progs.load);},
  findProgram:function(id){
    id=parseInt(id,10);
    if(!Progs.lastData)return null;
    for(var i=0;i<Progs.lastData.programs.length;i++){if(Progs.lastData.programs[i].id===id)return Progs.lastData.programs[i];}
    return null;
  },
  // Whether calendar date `d` is a watering day under this program's rule.
  // Mirrors isWateringDay() in Programs.cpp, except WDAY_INTERVAL — that needs
  // lastRunEpoch, which isn't sent to the client, so it's handled separately
  // as a static "every N days" label rather than a specific predicted date.
  dayMatches:function(p,d){
    switch(Number(p.dayMode)){
      case 0: return (p.dayMask&(1<<d.getDay()))!==0;
      case 1: return (d.getDate()%2)===1;
      case 2: return (d.getDate()%2)===0;
      default: return false;
    }
  },
  nextRunText:function(p){
    if(!p.enabled||!p.starts||!p.starts.length)return null;
    if(Number(p.dayMode)===3)return 'every '+(p.interval||2)+' day(s)';
    var starts=p.starts.slice().sort(function(a,b){return a-b;});
    var now=new Date(), nowMin=now.getHours()*60+now.getMinutes();
    for(var add=0;add<8;add++){
      var d=new Date(now.getFullYear(),now.getMonth(),now.getDate()+add);
      if(!Progs.dayMatches(p,d))continue;
      for(var i=0;i<starts.length;i++){
        if(add>0||starts[i]>nowMin){
          var label=add===0?'today':add===1?'tomorrow':d.toLocaleDateString(undefined,{weekday:'short'});
          return label+' '+Progs.hhmm(starts[i]);
        }
      }
    }
    return null;
  },
  // Returns bare inner text/HTML — caller supplies the wrapping element (the
  // #pg_total_ID span in card(), refreshed live by refreshBoxAndTotal()).
  totalRunHtml:function(map,firstStart,endMin){
    var total=0; Object.keys(map).forEach(function(k){total+=map[k];});
    if(firstStart!==undefined&&firstStart!==null&&endMin!==undefined&&endMin!==null&&endMin>firstStart){
      var win=endMin-firstStart;
      return total+' of '+win+' min used'+(total<win?' — '+(win-total)+' min free':'');
    }
    if(!total)return '&mdash;';
    var ends=(firstStart!==undefined&&firstStart!==null)?(' &middot; ends '+Progs.hhmm((firstStart+total)%1440)):'';
    return total+' min'+ends;
  },
  parseHHMM:function(s){
    var parts=String(s||'').split(':');
    if(parts.length<2)return null;
    var h=parseInt(parts[0],10), m=parseInt(parts[1],10);
    return (isNaN(h)||isNaN(m))?null:(h*60+m);
  },
  // Best-effort reconstruction for re-opening a saved program: end time isn't
  // stored (it's a client-side planning aid, not a field the firmware knows
  // about), so infer it from start + however much zone time is already saved.
  guessEndTime:function(p){
    if(!p.starts||!p.starts.length)return '';
    var total=0; (p.zoneMin||[]).forEach(function(m){total+=m;});
    return total?Progs.hhmm((p.starts[0]+total)%1440):'';
  },
  // n integers summing exactly to `total`, as equal as integer division
  // allows — front-loaded remainder so nothing is lost to rounding.
  splitEven:function(total,n){
    var base=Math.floor(total/n), rem=total-base*n, out=[];
    for(var i=0;i<n;i++)out.push(base+(i<rem?1:0));
    return out;
  },
  // Re-renders both the zone box and the Total-run line for one program from
  // whatever is currently in the DOM — called after every add/remove/edit so
  // neither goes stale relative to the other.
  refreshBoxAndTotal:function(pid,map){
    var startEl=UI.el('pg_start_'+pid), endEl=UI.el('pg_end_'+pid);
    var startMin=startEl?Progs.parseHHMM(startEl.value):null;
    var endMin=endEl?Progs.parseHHMM(endEl.value):null;
    UI.el('pg_zonesbox_'+pid).innerHTML=Progs.zoneRowsHtml(pid,map,startMin);
    var totalEl=UI.el('pg_total_'+pid);
    if(totalEl)totalEl.innerHTML=Progs.totalRunHtml(map,startMin,endMin);
  },
  card:function(p){
    var running=p.running;
    var next=Progs.nextRunText(p);
    var h='<div class="card">'
      +'<div class="ct">'+p.name+' '
      +(p.enabled?'<span class="badge b-ok">enabled</span>':'<span class="badge b-off">disabled</span>')
      +(running?' <span class="badge b-ok">running &mdash; zone '+(p.currentZone+1)+'</span>':'')
      +(next?' <span class="badge b-off">next: '+next+'</span>':'')
      +'</div>'
      +'<div class="row"><input type="checkbox" id="pg_en_'+p.id+'"'+(p.enabled?' checked':'')+'> <span class="lb">Enabled</span>'
        +'<input class="inp w" id="pg_name_'+p.id+'" value="'+p.name.replace(/"/g,'&quot;')+'" maxlength="15" style="max-width:160px"></div>'
      +'<div class="row"><span class="lb">Start time <b style="color:var(--err)">*</b></span>'
        +'<input class="inp" type="time" id="pg_start_'+p.id+'" value="'
        +(p.starts&&p.starts.length?Progs.hhmm(p.starts[0]):'')+'"></div>'
      +'<div class="row"><span class="lb">End time</span>'
        +'<input class="inp" type="time" id="pg_end_'+p.id+'" value="'+Progs.guessEndTime(p)+'">'
        +'<div class="hint">Optional. Set before adding zones to split the window between them automatically; leave blank to type each zone’s minutes yourself.</div></div>'
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
      +'<div style="padding:8px 0 0;border-top:1px solid var(--bd)"><div class="hint" style="margin-bottom:6px">Zones in this program &mdash; run one after another, in the order added</div>'
      +'<div id="pg_zonesbox_'+p.id+'">'+Progs.zoneRowsHtml(p.id,Progs.minMap(p),p.starts&&p.starts[0])+'</div>'
      +'</div>'
      +'<div class="row"><span class="lb">Total run</span><span id="pg_total_'+p.id+'">'
        +Progs.totalRunHtml(Progs.minMap(p),p.starts&&p.starts[0],Progs.parseHHMM(Progs.guessEndTime(p)))+'</span></div>'
      +'<div class="brow">'
        +'<button class="btn" id="btnProgSave_'+p.id+'">Save program</button>'
        +(running?'<button class="btn-s btn-d" id="btnProgStop_'+p.id+'">Stop</button>'
                 :'<button class="btn-s" id="btnProgRun_'+p.id+'">Run now</button>')
      +'</div></div>';
    return h;
  },
  showDayFields:function(id,mode){
    mode=String(mode);
    UI.el('pg_days_'+id).style.display=(mode==='0')?'block':'none';
    UI.el('pg_interval_'+id).style.display=(mode==='3')?'flex':'none';
  },
  save:function(id){
    var startVal=UI.el('pg_start_'+id).value;
    if(!startVal){UI.toast('Start time is required','err');return;}
    var o={id:id};
    if(UI.el('pg_en_'+id).checked) o.enabled='1';
    o.name=UI.el('pg_name_'+id).value;
    o.starts=startVal;   // backend still accepts a CSV of up to 4; one value is one program run/day
    o.dayMode=UI.el('pg_daymode_'+id).value;
    o.interval=UI.el('pg_iv_'+id).value;
    o.source=UI.el('pg_src_'+id).value;
    var mask=0;
    for(var wd=0;wd<7;wd++){var cb=UI.el('pg_day_'+id+'_'+wd);if(cb&&cb.checked)mask|=(1<<wd);}
    o.dayMask=mask;
    var m=Progs.readZoneBox(id);
    for(var z=0;z<Progs.zoneNames.length;z++) o['zm'+z]=(m[z]!==undefined?m[z]:0);
    UI.act('/api/programs/save',o,'Saved').then(Progs.load);
  },
  run:function(id){UI.act('/api/programs/cmd',{cmd:'run',id:id},'Running').then(Progs.load);},
  stop:function(id){UI.act('/api/programs/cmd',{cmd:'stop',id:id},'Stopped').then(Progs.load);},
  del:function(id){
    if(!window.confirm('Delete this program? This cannot be undone.'))return;
    UI.act('/api/programs/cmd',{cmd:'delete',id:id},'Deleted').then(function(){
      // Deleting shifts every later id down server-side, so any open editor
      // keyed by a stale id would now show the wrong program. Safer to just
      // close everything than risk that.
      Progs.openIds={};
      Progs.load();
    });
  },
  add:function(){
    UI.act('/api/programs/cmd',{cmd:'create'},'Program added').then(function(d){
      if(d&&d.id!==undefined)Progs.openIds[d.id]=true;   // open it immediately, nothing to configure otherwise
      return Progs.load();
    });
  }
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
      var h='<table><thead><tr><th>Time</th><th>Event</th><th>Detail</th><th>Reason</th></tr></thead><tbody>';
      for(var i=0;i<a.length;i++){var r=a[i];
        // Zone events carry zoneId instead of oh/ug tank state (see History.h) —
        // "Zone N" rather than a name, since a renamed zone shouldn't rewrite
        // what an old record meant at the time.
        var detail=(r.zoneId!==undefined)?('Zone '+(r.zoneId+1)):(r.oh+' / '+r.ug);
        h+='<tr><td>'+r.time+'</td><td>'+r.ev+'</td><td>'+detail+'</td><td>'+(r.rsnStr||'')+'</td></tr>';}
      h+='</tbody></table>';
      UI.el('histWrap').innerHTML=h;
    }).catch(function(){UI.el('histWrap').innerHTML='<div class="hint">Unavailable.</div>';});
  }
};

/* ---- System ---- */
var Sys={
  load:function(){Sys.info();Sys.logs();},
  // The value span gets its own shrinkable/truncating flex slot (.rv) instead
  // of the default flex item, which — once label+value together outgrow the
  // card — used to wrap the WHOLE value onto its own line (STA/Bluetooth rows
  // with a long SSID+IP or long text were the ones that actually hit this).
  // min-width:0 lets it shrink below its own content width so the row always
  // stays on one line; a title attribute keeps the full text reachable on
  // hover if it does truncate. Tags are stripped for the title since value
  // can carry inline HTML (e.g. the red "not detected" spans).
  row:function(label,value){
    var plain=String(value).replace(/<[^>]*>/g,'').replace(/"/g,'&quot;');
    return '<div class="row"><span class="lb">'+label+'</span><span class="rv" title="'+plain+'">'+value+'</span></div>';
  },
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
        Sys.row('Uptime',Sys.uptime(d.uptime))+
        Sys.row('Chip',d.chipModel+' rev '+d.chipRev)+
        Sys.row('CPU',d.cpuCores+' cores @ '+d.cpuFreqMHz+' MHz')+
        Sys.row('Core 0','WiFi / OTA / NTP')+
        Sys.row('Core 1','Control loop')+
        Sys.row('MAC',d.macAddress)+
        Sys.row('Bluetooth MAC',d.btMacAddress)+
        Sys.row('ESP-IDF',d.sdkVersion);

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
        Sys.row('LoRa',d.loraOk?(d.loraFreqMHz+' MHz &middot; RSSI '+Math.round(d.loraRSSI)+' dBm'):'not detected')+
        Sys.row('Bluetooth','disabled — not built into this firmware');

      var none=!d.rtcOk&&!d.lcdAddr&&!d.eepromOk;
      var fixedRows=
        Sys.row('RTC (DS3231)',d.rtcOk?('OK @ '+Sys.hex(d.rtcAddr)):'<span style="color:var(--err)">not detected</span>')+
        Sys.row('LCD',d.lcdAddr?('OK @ '+Sys.hex(d.lcdAddr)):'<span style="color:var(--err)">not detected</span>')+
        Sys.row('EEPROM (history)',d.eepromOk?('OK @ '+Sys.hex(d.eepromAddr)):'<span style="color:var(--err)">not detected</span>')+
        // All three missing at once is a bus-level fault, not three dead chips.
        // Say so here rather than making someone correlate it from the boot log.
        (none?'<div class="hint" style="color:var(--warn)">Nothing is responding anywhere on the I2C bus '+
          '(a full 0x08&ndash;0x77 scan found zero devices), so this is the bus itself, not three failed parts. '+
          'Check: 4.7k&ohm; pull-ups from SDA and SCL to 3.3&nbsp;V (the ESP32 internal ones are ~45k&ohm; and are often '+
          'too weak over jumper wire), 3.3&nbsp;V and GND actually present at each module, and both signal wires seated.</div>':'');
      UI.el('sysPeriph').innerHTML=fixedRows;

      // Declared expansion (relay) boards — configured on the Network tab,
      // shown here read-only alongside the other I2C peripherals. Board
      // numbering is stable (declared order), so "#1" always means the same
      // physical board even if it's the one currently unplugged.
      Api.get('/api/i2cexp').then(function(e){
        var eh='';
        (e.declared||[]).forEach(function(b){
          eh+=Sys.row('Expansion Board #'+(b.board+1),
            b.present?('OK @ '+Sys.hex(b.addr)+' ('+b.backend+')')
                     :('<span style="color:var(--err)">not detected</span> (declared @ '+Sys.hex(b.addr)+')'));
        });
        UI.el('sysPeriph').innerHTML=fixedRows+eh;
      }).catch(function(){});
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
        UI.toast(d.found+' device(s) found: '+addrs,'ok');
      } else if(d.clamped){
        h='<div class="banner" style="display:block">Bus is clamped LOW even with the internal '+
          'pull-up — something is holding a line down (stuck slave, or a short). A recovery pulse '+
          'was attempted automatically; power-cycle the board if this persists.</div>';
        UI.toast('Bus clamped LOW — stuck slave or short?','err');
      } else if(!d.pullups){
        h='<div class="banner" style="display:block">Lines float LOW without a pull-up — no working '+
          'pull-up resistors on the bus. Check power at each module first: an unpowered breakout '+
          'supplies neither an ACK nor a pull-up.</div>';
        UI.toast('No pull-ups on the bus — check module power','err');
      } else {
        h='<div class="banner" style="display:block">Pull-ups are present, nothing is clamping the bus, '+
          'but no device answered on SDA'+d.sda+'/SCL'+d.scl+'. Check 3.3V/GND at each module and that '+
          'both wires are actually seated.</div>';
        UI.toast('No devices answered on SDA'+d.sda+'/SCL'+d.scl,'err');
      }
      w.innerHTML=h;
    }).catch(function(){w.innerHTML='<div class="hint">Scan failed.</div>';UI.toast('I2C scan failed','err');});
  },
  logs:function(){
    return Api.get('/logs').then(function(d){
      var a=(d&&d.logs)||[];var s='';
      for(var i=0;i<a.length;i++)s+=(typeof a[i]==='string'?a[i]:JSON.stringify(a[i]))+'\n';
      UI.el('logs').textContent=s||'(empty)';
    }).catch(function(){});
  }
};

UI.panels={'p-prot':Protection,'p-prog':Progs,'p-zones':Zones,'p-hist':Hist,'p-net':Net,'p-sys':Sys};

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
bind('btnZoneRescan',function(){UI.act('/api/zones/cmd',{cmd:'rescan'},'Rescanned').then(function(){
  Zones.poll();
  if(UI.el('zoneMgrDetails').open)Zones.loadManager();
});});
bind('btnZoneAdd',Zones.addZone);
UI.el('zoneMgrDetails').addEventListener('toggle',function(){
  if(this.open)Zones.loadManager();
});
UI.el('zoneMgrList').addEventListener('click',function(e){
  var rn=e.target.closest('button[data-zmrename]'); if(rn){Zones.renameZone(parseInt(rn.dataset.zmrename,10));return;}
  var rm=e.target.closest('button[data-zmremap]');  if(rm){Zones.remapZone(parseInt(rm.dataset.zmremap,10));return;}
  var dl=e.target.closest('button[data-zmdel]');    if(dl&&!dl.disabled){Zones.deleteZone(parseInt(dl.dataset.zmdel,10));return;}
});
UI.el('zoneProgArea').addEventListener('click',function(e){
  var tgl=e.target.closest('button[data-zptoggle]'); if(tgl){Zones.progToggle(tgl.dataset.zptoggle);return;}
  var edit=e.target.closest('button[data-zpedit]');   if(edit){Zones.progEdit(edit.dataset.zpedit);return;}
  var del=e.target.closest('button[data-zpdel]');     if(del){Zones.progDelete(del.dataset.zpdel);return;}
});
bind('btnProgDefaultsSave',function(){
  UI.act('/api/programs/cmd',{cmd:'defaults',source:UI.el('pd_source').value,
    seasonalPct:UI.el('pd_seasonal').value,rainDelayDays:UI.el('pd_rain').value},'Defaults saved').then(Progs.load);
});
UI.el('progArea').addEventListener('click',function(e){
  var tgl=e.target.closest('button[data-ptoggle]'); if(tgl){Progs.toggleEnabled(tgl.dataset.ptoggle);return;}
  var edit=e.target.closest('button[data-pedit]');   if(edit){Progs.toggleEdit(edit.dataset.pedit);return;}
  var pdel=e.target.closest('button[data-pdel]');    if(pdel){Progs.del(pdel.dataset.pdel);return;}

  var addBtn=e.target.closest('button[data-zadd]');
  if(addBtn){
    var pid=addBtn.dataset.pid, sel=UI.el('pg_addzone_'+pid);
    if(!sel||sel.value==='')return;
    var newZ=parseInt(sel.value,10);
    var m=Progs.readZoneBox(pid);
    var startMin=Progs.parseHHMM(UI.el('pg_start_'+pid).value);
    var endMin=Progs.parseHHMM(UI.el('pg_end_'+pid).value);
    if(startMin!==null&&endMin!==null&&endMin>startMin){
      // Window is set: re-split it evenly across every zone including the new
      // one, overwriting whatever was there — this is the "add a zone, existing
      // ones re-divide" behaviour, not a one-time default.
      var ids=Object.keys(m).map(Number); ids.push(newZ); ids.sort(function(a,b){return a-b;});
      var shares=Progs.splitEven(endMin-startMin,ids.length);
      m={}; ids.forEach(function(zid,idx){m[zid]=shares[idx];});
    } else {
      m[newZ]=10;   // no window set - manual entry, same as before
    }
    Progs.refreshBoxAndTotal(pid,m);
    return;
  }
  var rmBtn=e.target.closest('button[data-zremove]');
  if(rmBtn){
    var pid2=rmBtn.dataset.pid;
    var m2=Progs.readZoneBox(pid2); delete m2[parseInt(rmBtn.dataset.zremove,10)];
    Progs.refreshBoxAndTotal(pid2,m2);
    return;
  }
  var b=e.target.closest('button[id]'); if(!b)return;
  var m=b.id.match(/^btnProg(Save|Run|Stop)_(\d+)$/); if(!m)return;
  var id=parseInt(m[2],10);
  if(m[1]==='Save')Progs.save(id);
  else if(m[1]==='Run')Progs.run(id);
  else Progs.stop(id);
});
// Live cap: typing a zone's minutes past what the start/end window leaves
// free clamps it back down immediately, rather than silently allowing an
// over-committed program through to Save.
UI.el('progArea').addEventListener('input',function(e){
  var zm=e.target.id&&e.target.id.match(/^pg_zm_(\d+)_(\d+)$/);
  if(zm){
    var pid=zm[1];
    var startMin=Progs.parseHHMM(UI.el('pg_start_'+pid).value);
    var endMin=Progs.parseHHMM(UI.el('pg_end_'+pid).value);
    if(startMin===null||endMin===null||endMin<=startMin)return;   // no window - nothing to cap against
    var win=endMin-startMin;
    var box=Progs.readZoneBox(pid);
    var sum=0; Object.keys(box).forEach(function(k){sum+=box[k];});
    if(sum>win){
      var edited=parseInt(e.target.value,10)||0;
      var others=sum-edited;
      e.target.value=Math.max(1,win-others);
      UI.toast('Zones can’t exceed the '+win+' min window','err');
    }
    var totalEl=UI.el('pg_total_'+pid);
    if(totalEl)totalEl.innerHTML=Progs.totalRunHtml(Progs.readZoneBox(pid),startMin,endMin);
    return;
  }
  var se=e.target.id&&e.target.id.match(/^pg_(start|end)_(\d+)$/);
  if(se){
    // Start/end changing doesn't touch already-set zone minutes on its own
    // (only Add re-splits) — just refresh the displayed windows and total so
    // they reflect the new clock times.
    Progs.refreshBoxAndTotal(se[2],Progs.readZoneBox(se[2]));
  }
});
bind('btnProgAdd',Progs.add);
bind('btnHistRefresh',Hist.load);
bind('btnHistClear',function(){UI.act('/clearhistory',{},'History cleared').then(Hist.load);});
bind('btnAddWifi',function(){
  UI.act('/addwifi',{ssid:UI.el('wSsid').value,password:UI.el('wPass').value},'Network added').then(Net.load);});
bind('btnI2cExpAdd',I2cExp.add);
UI.el('i2cExpList').addEventListener('click',function(e){
  var b=e.target.closest('button[data-i2cdel]');
  if(!b)return;
  I2cExp.remove(b.dataset.i2cdel);
});
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
