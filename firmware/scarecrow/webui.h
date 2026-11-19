// ============================================================================
//  webui.h  —  read-only WiFi dashboard for Scarecrow  (Anubis Darkwatch)
//  Two views: MONITOR (occupancy + events) and FOX HUNT (live RSSI instrument).
//  AP + AsyncWebServer + Server-Sent Events. Self-contained page from PROGMEM.
//  Nothing here can transmit on the CC1101s — it displays and tunes RX only.
// ============================================================================
#pragma once
#include "config.h"
#if USE_WEB
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

extern String buildStateJson();     // provided by the main sketch
extern void   setWatchFreq(float);  // retune Module B (fox target)
extern float  getWatchFreq();
extern String buildSigsJson();      // signature library
extern void   sigLabel(int, String);
extern void   sigForget(int);
#if USE_DF
extern String buildNodesJson();     // direction-finding node table
#endif

static AsyncWebServer  server(80);
static AsyncEventSource events("/events");

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Scarecrow · Anubis Darkwatch</title><style>
:root{
 --bg:#06070b; --bg2:#0b0d15; --panel:#0e1018; --panel2:#121521;
 --ink:#e9ecf4; --dim:#6b7186; --line:#1d2130;
 --acc:#9b8cff; --acc2:#6f5bff; --gold:#e0b84c; --ok:#35d6a4; --hot:#ff4d5e;
 --glow:0 0 24px rgba(155,140,255,.25);
}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{background:
   radial-gradient(1200px 600px at 50% -10%,rgba(111,91,255,.10),transparent 60%),
   radial-gradient(900px 500px at 90% 110%,rgba(224,184,76,.06),transparent 60%),
   var(--bg);
 color:var(--ink);font:14px/1.5 ui-monospace,"SFMono-Regular",Menlo,Consolas,monospace;
 -webkit-font-smoothing:antialiased}
.scan{position:fixed;inset:0;pointer-events:none;z-index:0;opacity:.5;
 background:repeating-linear-gradient(0deg,transparent 0 3px,rgba(255,255,255,.012) 3px 4px)}
.app{position:relative;z-index:1;max-width:760px;margin:0 auto;padding:0 14px 40px}

/* header / brand */
header{display:flex;align-items:center;gap:13px;padding:18px 4px 14px}
.mark{width:42px;height:42px;flex:0 0 auto;filter:drop-shadow(var(--glow))}
.brand h1{margin:0;font-size:20px;letter-spacing:.34em;font-weight:700}
.brand .sub{color:var(--gold);font-size:10px;letter-spacing:.42em;margin-top:2px}
.spacer{flex:1}
.live{display:flex;align-items:center;gap:7px;color:var(--dim);font-size:11px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dim)}
.dot.on{background:var(--ok);box-shadow:0 0 10px var(--ok)}
.statline{display:flex;gap:14px;flex-wrap:wrap;color:var(--dim);font-size:11px;
 padding:0 4px 10px;letter-spacing:.06em}
.statline b{color:var(--acc)} .statline .tx{color:var(--ok)}

/* tabs */
.tabs{display:flex;gap:6px;padding:4px;background:var(--panel);border:1px solid var(--line);
 border-radius:12px;margin:6px 0 16px}
.tab{flex:1;text-align:center;padding:10px;border-radius:9px;cursor:pointer;
 letter-spacing:.14em;font-size:12px;color:var(--dim);transition:.15s;user-select:none}
.tab.sel{background:linear-gradient(180deg,var(--panel2),#0b0e18);color:var(--ink);
 box-shadow:inset 0 0 0 1px var(--line),var(--glow)}
.view{display:none} .view.sel{display:block}

/* cards */
.card{background:linear-gradient(180deg,var(--panel),#0a0c13);border:1px solid var(--line);
 border-radius:14px;padding:16px 16px;margin:14px 0}
.card h2{margin:0 0 12px;font-size:11px;letter-spacing:.18em;color:var(--dim);
 text-transform:uppercase;display:flex;align-items:center;gap:8px}
.card h2::before{content:"";width:6px;height:6px;border-radius:50%;background:var(--acc);
 box-shadow:0 0 8px var(--acc)}

/* occupancy table */
table{width:100%;border-collapse:collapse}
td,th{text-align:left;padding:6px 6px;border-bottom:1px solid var(--line);font-size:13px}
th{color:var(--dim);font-weight:400;font-size:11px;letter-spacing:.1em}
.bar{height:7px;background:#05060a;border-radius:4px;overflow:hidden;box-shadow:inset 0 0 0 1px var(--line)}
.bar>i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--ok),var(--acc));transition:width .25s}
#selftest div{font-size:12px;color:var(--dim);margin:3px 0}
#selftest b{color:var(--ink);letter-spacing:.08em}
#wf{width:100%;height:150px;display:block;border-radius:6px;background:#06070b;
 box-shadow:inset 0 0 0 1px var(--line)}
.wfleg{height:6px;border-radius:3px;margin:9px 0 3px;
 background:linear-gradient(90deg,#06070b,#2e6e8c,#e0b84c,#ff4d5e)}
.wflab{display:flex;justify-content:space-between;color:var(--dim);font-size:9px;letter-spacing:.04em}
#log{max-height:46vh;overflow:auto}
#log div{padding:3px 0;border-bottom:1px solid #141825;white-space:pre-wrap;word-break:break-all;font-size:12.5px}
.burst{color:var(--acc)} .busy{color:var(--gold)} .alm{color:var(--hot)} .k{color:var(--dim)}
#alarm{display:none;margin:0 0 10px;padding:11px 14px;border-radius:12px;background:var(--hot);
 color:#17000a;font-weight:700;letter-spacing:.08em}
#alarm.on{display:block;animation:pulse 1s infinite}@keyframes pulse{50%{opacity:.55}}

/* fox hunt */
.fox-freq{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:6px}
.fox-freq .big{font-size:30px;letter-spacing:.04em;color:var(--ink)}
.fox-freq .u{color:var(--dim);font-size:13px}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 2px}
.chip{padding:6px 10px;border:1px solid var(--line);border-radius:999px;color:var(--dim);
 cursor:pointer;font-size:12px}.chip:hover{color:var(--ink);border-color:var(--acc)}
.chip.sel{color:var(--acc);border-color:var(--acc);box-shadow:var(--glow)}
.setrow{display:flex;gap:8px;margin-top:10px}
.setrow input{flex:1;background:#05060a;border:1px solid var(--line);border-radius:9px;
 color:var(--ink);padding:10px;font:inherit}
.btn{background:linear-gradient(180deg,var(--acc),var(--acc2));color:#0a0b0f;border:0;
 border-radius:9px;padding:10px 16px;font:inherit;font-weight:700;letter-spacing:.06em;cursor:pointer}
.btn.ghost{background:transparent;color:var(--acc);border:1px solid var(--acc)}
.gaugewrap{display:flex;flex-direction:column;align-items:center;padding:6px 0 2px}
#gauge{width:100%;max-width:360px;height:auto}
.readout{display:flex;justify-content:center;align-items:baseline;gap:8px;margin-top:-18px}
.readout .dbm{font-size:46px;letter-spacing:.02em}
.readout .unit{color:var(--dim)}
.trend{text-align:center;margin:8px 0 2px;font-size:20px;letter-spacing:.18em;font-weight:700;min-height:26px}
.trend.warm{color:var(--ok)} .trend.cold{color:var(--hot)} .trend.hold{color:var(--dim)}
.metrics{display:flex;justify-content:space-between;gap:10px;margin-top:10px}
.metric{flex:1;text-align:center;background:#090b12;border:1px solid var(--line);border-radius:10px;padding:10px 6px}
.metric .v{font-size:18px}.metric .l{color:var(--dim);font-size:10px;letter-spacing:.14em;margin-top:3px}
.foxbar{height:12px;background:#05060a;border-radius:7px;overflow:hidden;margin-top:12px;box-shadow:inset 0 0 0 1px var(--line)}
.foxbar>i{display:block;height:100%;width:0;background:linear-gradient(90deg,#2b3350,var(--gold),var(--hot));transition:width .12s}
.foot{color:var(--dim);font-size:11px;text-align:center;margin-top:18px;letter-spacing:.06em}
.foot .tx{color:var(--ok)}
/* signatures */
.siglist{display:flex;flex-direction:column;gap:9px;margin-top:4px}
.sigrow{background:#090b12;border:1px solid var(--line);border-radius:12px;padding:12px}
.sigmain{display:flex;align-items:center;gap:8px}
.siglabel{flex:1;background:#05060a;border:1px solid var(--line);border-radius:8px;color:var(--ink);padding:9px;font:inherit}
.signew{color:#06070b;background:var(--gold);border-radius:6px;padding:3px 8px;font-size:10px;font-weight:700;letter-spacing:.12em}
.sigmeta{color:var(--dim);font-size:12px;margin:9px 2px 10px}.sigmeta b{color:var(--ink)}
.sigacts{display:flex;gap:8px}.sigacts .btn{flex:1;padding:8px}
.new{color:var(--gold)}
.toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%) translateY(20px);
 background:var(--gold);color:#06070b;font-weight:700;padding:11px 18px;border-radius:11px;
 opacity:0;transition:.25s;z-index:9;pointer-events:none;letter-spacing:.04em;box-shadow:var(--glow)}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
/* locate */
.ndname{flex:1;letter-spacing:.08em}.ndrssi{color:var(--dim);font-size:12px}
.sigrow.hot{border-color:var(--gold);box-shadow:0 0 16px rgba(224,184,76,.22)}
.sigrow.hot .ndname{color:var(--gold)}
</style></head><body>
<div class="scan"></div>
<div class="app">
 <header>
  <svg class="mark" viewBox="0 0 48 48" fill="none">
   <defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
     <stop offset="0" stop-color="#b9adff"/><stop offset="1" stop-color="#6f5bff"/></linearGradient></defs>
   <!-- RF arcs -->
   <path d="M33 14a13 13 0 0 1 0 20" stroke="url(#g)" stroke-width="2" opacity=".55" stroke-linecap="round"/>
   <path d="M37 10a19 19 0 0 1 0 28" stroke="url(#g)" stroke-width="2" opacity=".28" stroke-linecap="round"/>
   <!-- crow head -->
   <path d="M7 31c0-7 5-13 12-14l12-4c1 0 2 1 1 2l-4 5 3 1c1 0 1 2 0 2l-6 2c-1 5-5 9-11 9-4 0-7-2-7-4z"
     fill="url(#g)"/>
   <circle cx="15" cy="27" r="1.7" fill="#06070b"/>
  </svg>
  <div class="brand"><h1>SCARECROW</h1><div class="sub">ANUBIS&nbsp;DARKWATCH</div></div>
  <div class="spacer"></div>
  <div class="live"><span id="dot" class="dot"></span><span id="livetxt">link</span></div>
 </header>
 <div class="statline">
  <span class="tx">RX-ONLY</span>
  <span>fox <b id="watch">--</b></span>
  <span><span id="sw">0</span> sweeps</span>
 </div>

 <div class="tabs">
  <div class="tab sel" data-v="mon">MONITOR</div>
  <div class="tab" data-v="fox">FOX HUNT</div>
  <div class="tab" data-v="sig">SIGNATURES</div>
  <div class="tab" data-v="loc">LOCATE</div>
 </div>

 <!-- MONITOR -->
 <div class="view sel" id="mon">
  <p id="alarm">⚠ BROADBAND INTERFERENCE</p>
  <div class="card"><h2>Radio self-test</h2><div id="selftest"></div></div>
  <div class="card"><h2>Spectrum waterfall</h2>
   <canvas id="wf"></canvas>
   <div class="wfleg"></div>
   <div class="wflab"><span>weak</span><span id="wfband"></span><span>strong</span></div>
  </div>
  <div class="card"><h2>Channel occupancy</h2>
   <table id="occ"><thead><tr><th>MHz</th><th>RSSI</th><th>busy</th><th>floor</th><th>live</th></tr></thead>
   <tbody></tbody></table></div>
  <div class="card"><h2>Event stream</h2><div id="log"></div></div>
 </div>

 <!-- FOX HUNT -->
 <div class="view" id="fox">
  <div class="card">
   <h2>Fox target</h2>
   <div class="fox-freq"><span class="big" id="foxf">433.92</span><span class="u">MHz</span></div>
   <div class="chips" id="chips"></div>
   <div class="setrow">
    <input id="freqin" inputmode="decimal" placeholder="e.g. 433.92">
    <button class="btn" onclick="setFreq()">SET</button>
   </div>
  </div>
  <div class="card">
   <h2>Signal strength</h2>
   <div class="gaugewrap">
    <svg id="gauge" viewBox="0 0 300 172">
     <path id="track" d="M24 158 A126 126 0 0 1 276 158" fill="none" stroke="#10131d" stroke-width="16" stroke-linecap="round"/>
     <path id="val" d="M24 158 A126 126 0 0 1 276 158" fill="none" stroke="url(#gg)" stroke-width="16" stroke-linecap="round"/>
     <defs><linearGradient id="gg" x1="0" y1="0" x2="1" y2="0">
       <stop offset="0" stop-color="#2b3350"/><stop offset=".6" stop-color="#e0b84c"/><stop offset="1" stop-color="#ff4d5e"/></linearGradient></defs>
     <circle id="peak" cx="24" cy="158" r="4.5" fill="#fff" opacity=".9"/>
     <line id="needle" x1="150" y1="158" x2="150" y2="48" stroke="#e9ecf4" stroke-width="3" stroke-linecap="round"/>
     <circle cx="150" cy="158" r="7" fill="#e9ecf4"/>
    </svg>
    <div class="readout"><span class="dbm" id="dbm">—</span><span class="unit">dBm</span></div>
   </div>
   <div class="trend hold" id="trend">— HOLD —</div>
   <div class="foxbar"><i id="foxbar"></i></div>
   <div class="metrics">
    <div class="metric"><div class="v" id="peakv">—</div><div class="l">PEAK dBm</div></div>
    <div class="metric"><div class="v" id="nowv">—</div><div class="l">NOW dBm</div></div>
    <div class="metric"><div class="v" id="hz">0</div><div class="l">UPDATES/s</div></div>
   </div>
   <div class="setrow" style="margin-top:14px">
    <button class="btn ghost" style="flex:1" onclick="resetPeak()">RESET PEAK</button>
    <button class="btn ghost" id="audbtn" style="flex:1" onclick="toggleAudio()">🔊 AUDIO: OFF</button>
   </div>
   <p class="foot">Sweep the antenna &amp; your body; walk toward rising dBm. Peak = hottest bearing.</p>
  </div>
 </div>
 <!-- SIGNATURES -->
 <div class="view" id="sig">
  <div class="card"><h2>Signature library</h2>
   <p class="foot" style="text-align:left;margin:-4px 0 10px">
     Emitters grouped by base symbol timing (<b>te</b>). <b class="new">NEW</b> =
     first seen this session. Label the ones you recognise; unlabeled strangers
     that keep returning are worth a look. Saved to SD when present.</p>
   <div id="siglist" class="siglist"></div>
   <p class="foot" id="sigempty">No signatures yet — capture a burst on the fox frequency.</p>
  </div>
 </div>
 <!-- LOCATE -->
 <div class="view" id="loc">
  <div class="card"><h2>Node locator</h2>
   <p class="foot" id="locmsg" style="text-align:left;margin:-4px 0 10px"></p>
   <div id="nodelist" class="siglist"></div>
  </div>
 </div>
 <p class="foot"><span class="tx">No transmit path in this firmware.</span> Display &amp; tune only.</p>
</div>
<div id="toast" class="toast"></div>
<script>
const $=s=>document.querySelector(s),occBody=$('#occ tbody');
// tabs
document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{
 document.querySelectorAll('.tab').forEach(x=>x.classList.remove('sel'));
 document.querySelectorAll('.view').forEach(x=>x.classList.remove('sel'));
 t.classList.add('sel');$('#'+t.dataset.v).classList.add('sel');});

// ---- monitor: occupancy ----
function row(mhz,busy,floor,rssi){let tr=document.getElementById('c'+mhz);
 if(!tr){tr=document.createElement('tr');tr.id='c'+mhz;
  tr.innerHTML=`<td>${mhz}</td><td class="r">--</td><td class="b">0</td><td class="f">--</td>
  <td style="width:30%"><div class="bar"><i></i></div></td>`;occBody.appendChild(tr);}
 if(rssi!=null){tr.querySelector('.r').textContent=rssi;
  tr.querySelector('.bar>i').style.width=Math.max(0,Math.min(100,(rssi+110)/80*100))+'%';}
 tr.querySelector('.b').textContent=busy;tr.querySelector('.f').textContent=floor;}

// ---- spectrum waterfall (one row per sweep, newest on top) ----
let wfctx=null,WFn=0;
function wfColor(r){let p=Math.max(0,Math.min(1,(r+110)/70));           // -110 floor .. -40 hot
 const S=[[6,7,11],[46,110,140],[224,184,76],[255,77,94]];let g=p*3,i=g|0,t=g-i;if(i>2){i=2;t=1;}
 const a=S[i],b=S[i+1]||a;return`rgb(${a[0]+(b[0]-a[0])*t|0},${a[1]+(b[1]-a[1])*t|0},${a[2]+(b[2]-a[2])*t|0})`;}
function wfInit(keys){const c=$('#wf');c.width=c.clientWidth||320;c.height=150;wfctx=c.getContext('2d');WFn=keys.length;
 const b=$('#wfband');if(b)b.textContent=parseFloat(keys[0]).toFixed(0)+'–'+parseFloat(keys[keys.length-1]).toFixed(0)+' MHz';}
function drawWaterfall(ro){if(!ro)return;const keys=Object.keys(ro).sort((a,b)=>parseFloat(a)-parseFloat(b));
 if(!wfctx||WFn!==keys.length)wfInit(keys);if(!wfctx||!wfctx.canvas.width)return;
 const c=wfctx.canvas,n=keys.length,cw=c.width/n,rh=3;
 wfctx.drawImage(c,0,rh);                                              // scroll down
 for(let i=0;i<n;i++){wfctx.fillStyle=wfColor(parseFloat(ro[keys[i]]));wfctx.fillRect(i*cw|0,0,Math.ceil(cw),rh);}}
function add(cls,txt){const d=document.createElement('div');d.className=cls;
 d.textContent=new Date().toLocaleTimeString()+'  '+txt;$('#log').prepend(d);
 while($('#log').childElementCount>200)$('#log').lastChild.remove();}

// ---- fox hunt ----
const VAL=$('#val'),LEN=VAL.getTotalLength();VAL.style.strokeDasharray=LEN;VAL.style.strokeDashoffset=LEN;
let peak=-200, ewF=-100, ewS=-100, fcount=0;
const RLO=-110, RHI=-30;
function pct(r){return Math.max(0,Math.min(1,(r-RLO)/(RHI-RLO)));}
function foxUpdate(r){
 const p=pct(r);
 VAL.style.strokeDashoffset=LEN*(1-p);
 // needle: weak=left(180deg) strong=right(0deg)
 const ang=Math.PI*(1-p), cx=150,cy=158,L=108;
 $('#needle').setAttribute('x2',cx+L*Math.cos(ang));
 $('#needle').setAttribute('y2',cy-L*Math.sin(ang));
 $('#dbm').textContent=r; $('#nowv').textContent=r;
 $('#foxbar').style.width=(p*100)+'%';
 if(r>peak){peak=r;$('#peakv').textContent=r;
   const pa=Math.PI*(1-pct(peak));
   $('#peak').setAttribute('cx',cx+126*Math.cos(pa));$('#peak').setAttribute('cy',cy-126*Math.sin(pa));}
 // warmer / colder
 ewF=ewF*0.6+r*0.4; ewS=ewS*0.95+r*0.05;
 const t=$('#trend');
 if(ewF>ewS+0.8){t.className='trend warm';t.textContent='▲ WARMER ▲';}
 else if(ewF<ewS-0.8){t.className='trend cold';t.textContent='▼ COLDER ▼';}
 else{t.className='trend hold';t.textContent='— HOLD —';}
 fcount++; if(audioOn)beep(r);
}
setInterval(()=>{$('#hz').textContent=fcount;fcount=0;},1000);
function resetPeak(){peak=-200;$('#peakv').textContent='—';
 $('#peak').setAttribute('cx',24);$('#peak').setAttribute('cy',158);}

// ---- signature library ----
const SIG={};
function sigMeta(d){return `te <b>${d.te}µs</b> &middot; edges ${d.nmin}`
 +(d.nmax&&d.nmax!=d.nmin?'–'+d.nmax:'')+` &middot; seen <b>${d.count}×</b> &middot; <b>${d.rssi}</b>dBm`;}
function renderSig(d){
 const s=SIG[d.id]=Object.assign({nmin:d.nmin,nmax:d.nmax,count:0,label:''},SIG[d.id]||{},d);
 $('#sigempty').style.display='none';
 let row=document.getElementById('sig'+d.id);
 if(!row){row=document.createElement('div');row.className='sigrow';row.id='sig'+d.id;
  row.innerHTML=`<div class="sigmain"><input class="siglabel" id="lbl${d.id}" placeholder="unlabeled #${d.id}">
   <span class="signew" style="display:none">NEW</span></div>
   <div class="sigmeta"></div>
   <div class="sigacts"><button class="btn ghost" onclick="labelSig(${d.id})">SAVE</button>
   <button class="btn ghost" onclick="forgetSig(${d.id})">FORGET</button></div>`;
  $('#siglist').prepend(row);
  if(s.label)row.querySelector('.siglabel').value=s.label;}
 row.querySelector('.sigmeta').innerHTML=sigMeta(s);
 if(d.isNew)row.querySelector('.signew').style.display='inline';}
function labelSig(id){const v=document.getElementById('lbl'+id).value;
 fetch('/label?id='+id+'&name='+encodeURIComponent(v)).then(()=>toast('saved'));}
function forgetSig(id){fetch('/forget?id='+id).then(()=>{
 const r=document.getElementById('sig'+id);if(r)r.remove();delete SIG[id];});}
function toast(m){const t=$('#toast');t.textContent=m;t.classList.add('show');
 clearTimeout(window._tt);window._tt=setTimeout(()=>t.classList.remove('show'),2200);}
fetch('/sigs.json').then(r=>r.json()).then(d=>d.sigs.forEach(renderSig)).catch(()=>{});

// ---- locate (direction finding) ----
const NODES={};let curTarget='--';
// A rover counts as active if we've heard from it in the last 12s.
function activeRovers(){return Object.values(NODES).filter(n=>n.id!=='0'&&(Date.now()-(n.seen||0))<12000).length;}
function updateLocateMode(){const m=$('#locmsg');if(!m)return;const r=activeRovers();
 if(r===0){m.innerHTML='<b>Standalone.</b> This Evil Crow (anchor) is your single-point '
   +'fox meter on <b>'+curTarget+'</b> — the bar below rises as you near the source. '
   +'Add one or more rover nodes over ESP-NOW for multi-point direction finding.';}
 else{m.innerHTML='<b>'+r+' rover'+(r>1?'s':'')+' active.</b> Hottest node is nearest the source. '
   +'Target <b>'+curTarget+'</b>; <b>0</b> is this Evil Crow (anchor). Walk a rover and the bars '
   +'re-rank live. Amplified nodes read hotter — calibrate before trusting the order across mixed radios.';}}
function renderNode(d){NODES[d.id]=Object.assign(NODES[d.id]||{},d,{seen:Date.now()});const s=NODES[d.id];
 let row=document.getElementById('nd'+d.id);
 if(!row){row=document.createElement('div');row.className='sigrow';row.id='nd'+d.id;
  row.innerHTML=`<div class="sigmain"><b class="ndname"></b><span class="ndrssi"></span></div>
   <div class="foxbar" style="margin-top:9px"><i></i></div>`;$('#nodelist').appendChild(row);}
 row.querySelector('.ndname').textContent=d.id==='0'?'EVIL CROW · anchor':('NODE '+d.id);
 row.querySelector('.ndrssi').textContent=s.rssi+' dBm'+(s.age>5?' · stale '+s.age+'s':'');
 row.querySelector('.foxbar>i').style.width=Math.max(0,Math.min(100,(s.rssi+110)/80*100))+'%';
 rankNodes();updateLocateMode();}
function rankNodes(){const L=$('#nodelist');
 [...L.children].sort((a,b)=>NODES[b.id.slice(2)].rssi-NODES[a.id.slice(2)].rssi)
  .forEach((r,i,arr)=>{r.style.order=i;r.classList.toggle('hot',i===0&&arr.length>1);});}
// Drop rovers we stop hearing from (anchor 0 always stays); keep the mode honest.
function sweepNodes(){const now=Date.now();
 for(const id in NODES){if(id==='0')continue;const age=(now-NODES[id].seen)/1000;
  const row=document.getElementById('nd'+id);
  if(age>30){if(row)row.remove();delete NODES[id];continue;}
  if(row)row.querySelector('.ndrssi').textContent=NODES[id].rssi+' dBm'+(age>6?' · stale '+Math.round(age)+'s':'');}
 rankNodes();updateLocateMode();}
setInterval(sweepNodes,3000);
fetch('/nodes.json').then(r=>r.json()).then(d=>{if(d.target)curTarget=d.target+' MHz';
 d.nodes.forEach(renderNode);updateLocateMode();}).catch(()=>updateLocateMode());

const PRESETS=[315,390,418,433.92,868.3,915];
const chips=$('#chips');
PRESETS.forEach(f=>{const c=document.createElement('span');c.className='chip';c.textContent=f;
 c.onclick=()=>sendFreq(f);chips.appendChild(c);});
function markChip(f){document.querySelectorAll('.chip').forEach(c=>
 c.classList.toggle('sel',parseFloat(c.textContent)===parseFloat(f)));}
function setFreq(){const v=parseFloat($('#freqin').value);if(v)sendFreq(v);}
function sendFreq(f){fetch('/fox?mhz='+f).then(()=>{});}
function setWatchUI(f){$('#watch').textContent=f+' MHz';$('#foxf').textContent=f;markChip(f);
 curTarget=f+' MHz';updateLocateMode();}

// ---- audio beacon (classic fox-hunt beep: faster+higher = closer) ----
let actx,audioOn=false,lastBeep=0;
function toggleAudio(){audioOn=!audioOn;$('#audbtn').textContent='🔊 AUDIO: '+(audioOn?'ON':'OFF');
 if(audioOn&&!actx)actx=new (window.AudioContext||window.webkitAudioContext)();
 if(audioOn&&actx.state==='suspended')actx.resume();}
function beep(r){if(!actx)return;const p=pct(r);
 const interval=700-p*640;                 // ms between beeps: far=700, near=60
 const now=performance.now();if(now-lastBeep<interval)return;lastBeep=now;
 const o=actx.createOscillator(),g=actx.createGain();
 o.frequency.value=300+p*1100;             // pitch 300..1400 Hz
 o.connect(g);g.connect(actx.destination);
 const t=actx.currentTime;g.gain.setValueAtTime(0.0001,t);
 g.gain.exponentialRampToValueAtTime(0.25,t+0.01);
 g.gain.exponentialRampToValueAtTime(0.0001,t+0.08);
 o.start(t);o.stop(t+0.09);}

// ---- stream ----
function handle(e){let o;try{o=JSON.parse(e)}catch(_){return}
 if(o.t==='fox'){foxUpdate(o.rssi);}
 else if(o.t==='sweep'){$('#sw').textContent=o.n;for(const m in o.occ)
   row(m,o.occ[m],(window._fl&&window._fl[m])??'--',o.rssi?o.rssi[m]:null);drawWaterfall(o.rssi);}
 else if(o.t==='busy'){add('busy',`busy ${o.mhz}  ${o.rssi}dBm (floor ${o.floor})`);
   (window._fl=window._fl||{})[(+o.mhz).toFixed(2)]=o.floor;}
 else if(o.t==='burst'){add('burst',`burst ${o.mhz}  ${o.edges} edges  ${o.dur_us}us  sig ${o.sig}  ${o.rssi}dBm`);}
 else if(o.t==='alarm'){add('alm',`ALARM ${o.kind}  ${o.hot} hot  peak ${o.peak_mhz} ${o.peak_rssi}dBm`);
   $('#alarm').classList.add('on');clearTimeout(window._at);
   window._at=setTimeout(()=>$('#alarm').classList.remove('on'),6000);}
 else if(o.t==='watch'){setWatchUI(o.mhz);}
 else if(o.t==='newsig'){renderSig({id:o.id,te:o.te,nmin:o.edges,nmax:o.edges,count:1,rssi:o.rssi,isNew:true});
   toast('NEW signal · te '+o.te+'µs');}
 else if(o.t==='sig'){renderSig({id:o.id,te:o.te,count:o.count,rssi:o.rssi});}
 else if(o.t==='sigupd'){const i=document.getElementById('lbl'+o.id);if(i)i.value=o.label;
   const r=document.getElementById('sig'+o.id);if(r)r.querySelector('.signew').style.display='none';}
 else if(o.t==='sigdel'){const r=document.getElementById('sig'+o.id);if(r)r.remove();delete SIG[o.id];}
 else if(o.t==='node'){renderNode(o);}
 else if(o.t==='selftest'){let st=document.getElementById('st'+o.mod)||(()=>{
    let d=document.createElement('div');d.id='st'+o.mod;$('#selftest').appendChild(d);return d;})();
    let ok=o.version>0&&o.version<255;
    st.innerHTML=`<b>MOD ${o.mod}</b> &middot; ver ${o.version} `+
     `<span style="color:${ok?'var(--ok)':'var(--hot)'}">${ok?'SPI ok':'SPI DEAD'}</span> &middot; `+
     `marc ${o.marcstate} <span style="color:${o.in_rx?'var(--ok)':'var(--hot)'}">${o.in_rx?'RX':'NOT RX'}</span>`+
     ` &middot; raw ${o.rssi_raw}`;}
 else if(o.t==='boot'){setWatchUI(o.watch_mhz);}}

fetch('/state.json').then(r=>r.json()).then(s=>{setWatchUI(s.watch_mhz);
 $('#sw').textContent=s.sweeps;window._fl={};
 s.channels.forEach(c=>{window._fl[c.mhz]=c.floor;row(c.mhz,c.busy,c.floor,c.rssi);});});
const es=new EventSource('/events');
es.onopen=()=>{$('#dot').classList.add('on');$('#livetxt').textContent='live';};
es.onerror=()=>{$('#dot').classList.remove('on');$('#livetxt').textContent='reconnecting';};
es.onmessage=ev=>handle(ev.data);
</script></body></html>
)HTML";

inline void webSetup() {
#if USE_DF
  WiFi.mode(WIFI_AP_STA);            // STA iface present for reliable ESP-NOW rx
#else
  WiFi.mode(WIFI_AP);
#endif
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/state.json", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "application/json", buildStateJson());
  });
  server.on("/fox", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("mhz")) setWatchFreq(r->getParam("mhz")->value().toFloat());
    r->send(200, "application/json",
      String("{\"ok\":true,\"watch_mhz\":") + String(getWatchFreq(), 2) + "}");
  });
  server.on("/sigs.json", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "application/json", buildSigsJson());
  });
  server.on("/label", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("id") && r->hasParam("name"))
      sigLabel(r->getParam("id")->value().toInt(), r->getParam("name")->value());
    r->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/forget", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("id")) sigForget(r->getParam("id")->value().toInt());
    r->send(200, "application/json", "{\"ok\":true}");
  });
#if USE_DF
  server.on("/nodes.json", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "application/json", buildNodesJson());
  });
#endif
  events.onConnect([](AsyncEventSourceClient *c){
    c->send("{\"t\":\"hello\"}", NULL, millis(), 2000);
  });
  server.addHandler(&events);
  server.begin();
}

inline void webPush(const String& line) { events.send(line.c_str(), "message", millis()); }
inline String webApIp() { return WiFi.softAPIP().toString(); }

#else   // USE_WEB == 0
inline void   webSetup() {}
inline void   webPush(const String&) {}
inline String webApIp() { return String("disabled"); }
#endif
