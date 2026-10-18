#pragma once
// =============================================================================
//  DashboardPage — the web page served at http://rootsense.local
//  Plain HTML + CSS + JavaScript (no internet or libraries needed). It polls
//  /api/status every 4 s and sends changes with small POST requests.
// =============================================================================
#include <Arduino.h>

static const char DASHBOARD_HTML[] PROGMEM = R"ROOTSENSE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RootSense</title>
<style>
:root{--bg:#f3f5f1;--card:#fff;--ink:#1c2a21;--muted:#637267;--line:#dde4dc;--accent:#2e7d4f;--on-accent:#fff;
--water:#2f6fb3;--warn:#a15c00;--bad:#b42318;--track:#e7ece6;--band:rgba(46,125,79,.18)}
@media (prefers-color-scheme:dark){:root{--bg:#111613;--card:#1a211c;--ink:#e5eee7;--muted:#9aaa9f;--line:#2a342d;
--accent:#55b97f;--on-accent:#0b1a11;--water:#6aa8ff;--warn:#f0b44c;--bad:#ff8a80;--track:#253029;--band:rgba(85,185,127,.2)}}
*{box-sizing:border-box}
body{margin:0;font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--ink)}
header,main{max-width:1000px;margin:0 auto;padding:0 16px}
header{display:flex;flex-wrap:wrap;gap:10px;align-items:center;justify-content:space-between;padding-top:18px;padding-bottom:12px}
h1{font-size:21px;margin:0}
h2{font-size:18px;margin:0}
h3{font-size:15px;margin:0 0 8px}
.chips{display:flex;flex-wrap:wrap;gap:6px}
.chip{font-size:12px;padding:3px 10px;border-radius:999px;border:1px solid var(--line);background:var(--card);color:var(--muted)}
.chip.ok{color:var(--accent)}.chip.water{color:var(--water)}.chip.bad{color:var(--bad);border-color:var(--bad)}
main{display:grid;gap:14px;padding-bottom:40px}
.zones{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;min-width:0}
.zhead{display:flex;justify-content:space-between;align-items:flex-start;gap:8px}
.eyebrow{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted)}
.reading{display:flex;align-items:baseline;gap:6px;margin-top:10px}
.big{font-size:44px;font-weight:650;letter-spacing:-1px;font-variant-numeric:tabular-nums}
.unit{color:var(--muted)}
.gauge{position:relative;height:14px;border-radius:7px;background:var(--track);overflow:hidden;margin:8px 0 3px}
.gauge .band{position:absolute;top:0;bottom:0;background:var(--band)}
.gauge .fill{position:absolute;top:3px;bottom:3px;left:0;border-radius:4px;background:var(--water);transition:width .4s}
.scale{display:flex;justify-content:space-between;font-size:12px;color:var(--muted)}
.status{margin:10px 0;padding:9px 11px;border-radius:10px;background:var(--track)}
.status.ok{color:var(--accent)}.status.water{color:var(--water)}.status.warn{color:var(--warn)}
.status.bad{color:var(--bad);background:transparent;border:1px solid var(--bad)}
.tip{font-size:13px;color:var(--muted);margin:0 0 10px}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button{font:inherit;padding:7px 12px;border-radius:9px;border:1px solid var(--line);background:var(--card);color:var(--ink);cursor:pointer}
button:hover{border-color:var(--muted)}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--on-accent)}
button.danger{border-color:var(--bad);color:var(--bad)}
input,select{font:inherit;padding:6px 8px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--ink);max-width:100%}
input[type=number]{width:90px}
label{display:block;margin:8px 0}
.switch{display:flex;align-items:center;gap:6px;margin:0;font-size:14px;white-space:nowrap}
.switch input{width:18px;height:18px}
details{border-top:1px solid var(--line);margin-top:12px;padding-top:8px}
summary{cursor:pointer;color:var(--muted);font-weight:600}
.hint{font-size:13px;color:var(--muted)}
.steps{padding-left:20px;margin:8px 0}.steps li{margin:6px 0}
.kv{display:grid;grid-template-columns:auto 1fr;gap:3px 12px;font-size:13px;margin-top:10px}
.kv dt{color:var(--muted)}.kv dd{margin:0;font-variant-numeric:tabular-nums}
.caption{margin-top:14px}
.spark{display:block;width:100%;height:64px;margin-top:4px}
.log{font:12px/1.55 ui-monospace,Menlo,Consolas,monospace;max-height:320px;overflow:auto;margin:0;padding:0;list-style:none}
.log li{padding:3px 0;border-bottom:1px dashed var(--line)}.log time{color:var(--muted);margin-right:8px}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);max-width:92vw;background:var(--ink);color:var(--bg);
padding:10px 16px;border-radius:10px;opacity:0;pointer-events:none;transition:opacity .2s}
#toast.show{opacity:1}#toast.bad{background:var(--bad);color:#fff}
</style>
</head>
<body>
<header>
  <h1>&#127793; RootSense</h1>
  <div class="chips" id="chips"><span class="chip">Connecting&hellip;</span></div>
</header>
<main id="main"></main>
<div id="toast" role="status" aria-live="polite"></div>
<script>
const $=(s,el=document)=>el.querySelector(s);
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let plants=[],st=null,built=false,hist=null;
function ago(s){if(s==null||s<0)return'never';if(s<90)return Math.round(s)+' s ago';if(s<5400)return Math.round(s/60)+' min ago';
 if(s<172800)return Math.round(s/3600)+' h ago';return Math.round(s/86400)+' days ago'}
function toast(msg,bad){const t=$('#toast');t.textContent=msg;t.className='show'+(bad?' bad':'');clearTimeout(t.tm);t.tm=setTimeout(()=>t.className='',4000)}
async function getJSON(p){const r=await fetch(p,{cache:'no-store'});if(!r.ok)throw new Error(r.status);return r.json()}
async function post(p,data){
 try{const r=await fetch(p,{method:'POST',body:new URLSearchParams(data)});const j=await r.json();
  toast(j.ok?(j.msg||'Done'):(j.error||'Something went wrong'),!j.ok);await refresh(true);return j}
 catch(e){toast('Could not reach the plant controller',true)}}
function plantOptions(sel){let h='',g=null;for(const p of plants){if(p.group!==g){if(g!==null)h+='</optgroup>';g=p.group;h+='<optgroup label="'+esc(g)+'">'}
 h+='<option value="'+esc(p.key)+'"'+(p.key===sel?' selected':'')+'>'+esc(p.name)+' ('+p.below+'-'+p.target+'%)</option>'}return h+'</optgroup>'}
function zoneCard(z){const i=z.n,id=k=>'z'+i+'-'+k;return `
<section class="card" id="zone${i}">
 <div class="zhead"><div><div class="eyebrow">Zone ${i}</div><h2 id="${id('name')}"></h2></div>
  <label class="switch"><input type="checkbox" id="${id('en')}" onchange="post('/api/zone',{z:${i},enabled:this.checked?1:0})">Automatic</label></div>
 <div class="reading"><span class="big" id="${id('pct')}">&ndash;</span><span class="unit">% moisture</span></div>
 <div class="gauge"><div class="band" id="${id('band')}"></div><div class="fill" id="${id('fill')}"></div></div>
 <div class="scale"><span>dry</span><span id="${id('range')}"></span><span>just watered</span></div>
 <p class="status" id="${id('status')}"></p>
 <p class="tip" id="${id('tip')}"></p>
 <div class="row">
  <button class="primary" onclick="post('/api/water',{z:${i},ml:$('#${id('ml')}').value})">Water now</button>
  <input type="number" id="${id('ml')}" value="50" min="5" max="500" step="5" aria-label="millilitres"> ml
  <button class="danger" id="${id('clear')}" hidden onclick="post('/api/clear',{z:${i}})">Clear alert</button>
 </div>
 <div class="eyebrow caption">Last 24 hours</div>
 <svg class="spark" id="${id('spark')}" viewBox="0 0 288 64" preserveAspectRatio="none" role="img" aria-label="Moisture over the last 24 hours"></svg>
 <dl class="kv" id="${id('kv')}"></dl>
 <details><summary>Plant &amp; pot</summary>
  <label>Plant<br><select id="${id('plant')}">${plantOptions(z.plant)}</select></label>
  <label>Soil volume of the pot<br><input type="number" id="${id('pot')}" min="100" max="20000" step="50"> ml</label>
  <p class="hint">A 15 cm (6&Prime;) pot holds about 1500 ml of soil, a 20 cm (8&Prime;) pot about 3500 ml. Changing the pot size restarts the learning of how much water this pot needs.</p>
  <button onclick="savePlant(${i})">Save</button>
 </details>
 <details><summary>Calibrate the sensor</summary>
  <ol class="steps">
   <li>Pull the sensor out, wipe it clean and dry, and hold it in the air. <button onclick="post('/api/calibrate',{z:${i},point:'dry'})">Save DRY</button></li>
   <li>Push it back into the soil up to the line, water the pot thoroughly and wait about 30 min for it to drain. <button onclick="post('/api/calibrate',{z:${i},point:'wet'})">Save WET</button></li>
  </ol>
  <p class="hint" id="${id('cal')}"></p>
 </details>
 <details><summary>Measure the pump flow</summary>
  <ol class="steps">
   <li>Put this pump's outlet tube into a measuring cup (start with the tube empty, like normal use).</li>
   <li><button onclick="post('/api/pumptest',{z:${i},s:10})">Run pump 10 s</button></li>
   <li>Water that came out: <input type="number" id="${id('meas')}" min="1" max="3000"> ml <button onclick="saveFlow(${i})">Save</button></li>
  </ol>
 </details>
 <details><summary>Advanced</summary>
  <label>Soak time after each pulse <input type="number" id="${id('soak')}" min="2" max="120"> min</label>
  <label>Rest after a watering cycle <input type="number" id="${id('cool')}" min="10" max="1440"> min</label>
  <label>Daily water limit <input type="number" id="${id('limit')}" min="20" max="5000" step="10"> ml</label>
  <label>Max pulses per cycle <input type="number" id="${id('pulses')}" min="1" max="10"></label>
  <button onclick="saveAdv(${i})">Save</button>
 </details>
</section>`}
function systemCards(){return `
<section class="card"><div class="zhead"><h3>System</h3>
 <div class="row"><button class="danger" onclick="post('/api/stop',{})">Stop pump</button><button onclick="if(confirm('Restart the controller?'))post('/api/restart',{})">Restart</button></div></div>
 <dl class="kv" id="sys"></dl>
 <details><summary>Wi-Fi</summary>
  <p class="hint">Saving restarts the controller and joins this network. If it can't connect, it opens its own hotspot again.</p>
  <label>Network name<br><input id="ssid" autocomplete="off"></label>
  <label>Password<br><input id="pass" type="password" autocomplete="off"></label>
  <button onclick="post('/api/wifi',{ssid:$('#ssid').value,pass:$('#pass').value})">Save &amp; restart</button>
 </details>
 <details><summary>Quiet hours</summary>
  <p class="hint">No new watering starts during these hours (pumps can be noisy). Needs Wi-Fi for the clock.</p>
  <label class="switch"><input type="checkbox" id="q-en"> On</label>
  <div class="row">from <input type="number" id="q-start" min="0" max="23"> to <input type="number" id="q-end" min="0" max="23"> o'clock</div>
  <p><button onclick="post('/api/quiet',{enabled:$('#q-en').checked?1:0,start:$('#q-start').value,end:$('#q-end').value})">Save</button></p>
 </details>
</section>
<section class="card"><h3>Activity</h3><ul class="log" id="log"></ul></section>`}
function savePlant(i){post('/api/zone',{z:i,plant:$('#z'+i+'-plant').value,pot:$('#z'+i+'-pot').value})}
function saveFlow(i){const v=$('#z'+i+'-meas').value;if(!v){toast('Enter the measured ml first',true);return}post('/api/flow',{z:i,ml:v,s:10})}
function saveAdv(i){post('/api/zone',{z:i,soak:$('#z'+i+'-soak').value,cooldown:$('#z'+i+'-cool').value,limit:$('#z'+i+'-limit').value,pulses:$('#z'+i+'-pulses').value})}
function setVal(sel,v){const e=$(sel);if(e&&document.activeElement!==e)e.value=v}
function fillInputs(){for(const z of st.zones){const i=z.n;setVal('#z'+i+'-pot',z.pot);setVal('#z'+i+'-soak',z.soak);setVal('#z'+i+'-cool',z.cooldown);
 setVal('#z'+i+'-limit',z.limit);setVal('#z'+i+'-pulses',z.maxPulses);const s=$('#z'+i+'-plant');if(s&&document.activeElement!==s)s.value=z.plant}
 setVal('#q-start',st.quiet.start);setVal('#q-end',st.quiet.end);$('#q-en').checked=st.quiet.enabled;
 if(document.activeElement!==$('#ssid'))$('#ssid').value=st.wifi.saved||''}
function kv(rows){return rows.map(r=>'<dt>'+esc(r[0])+'</dt><dd>'+esc(r[1])+'</dd>').join('')}
function chips(){const c=[];
 c.push(st.tank.present?(st.tank.water?['ok','Tank: water OK']:['bad','Tank EMPTY']):['','No tank switch']);
 c.push(st.pump.running?['water','Pump on: zone '+st.pump.zone]:['','Pumps idle']);
 c.push(st.wifi.connected?['ok','Wi-Fi: '+st.wifi.ssid]:(st.wifi.ap?['','Setup hotspot']:['bad','Wi-Fi offline']));
 if(st.time)c.push(['',st.time]);
 $('#chips').innerHTML=c.map(x=>'<span class="chip '+x[0]+'">'+esc(x[1])+'</span>').join('')}
function render(){
 if(!built){$('#main').innerHTML='<div class="zones">'+st.zones.map(zoneCard).join('')+'</div>'+systemCards();built=true;fillInputs()}
 chips();
 for(const z of st.zones){const i=z.n,g=k=>$('#z'+i+'-'+k);
  g('name').textContent=z.plantName;g('en').checked=z.enabled;
  g('pct').textContent=z.moisture==null?'\u2014':Math.round(z.moisture);
  const pct=v=>Math.max(0,Math.min(100,v))+'%';
  g('fill').style.width=z.moisture==null?'0%':pct(z.moisture);
  g('band').style.left=pct(z.below);g('band').style.width=pct(z.target-z.below);
  g('range').textContent='waters below '+z.below+'%, up to '+z.target+'%';
  const s=g('status');s.textContent=z.status;s.className='status '+z.level;
  g('tip').textContent=z.tip;g('clear').hidden=z.state!=='fault';
  g('kv').innerHTML=kv([['Sensor',z.health==='ok'?Math.round(z.mv)+' mV':z.health+' ('+Math.round(z.mv)+' mV)'],
   ['Water, last 24 h',Math.round(z.ml24h)+' of '+z.limit+' ml'],['Last watered',ago(z.lastWateredAgo)],
   ['Pump flow',z.flow.toFixed(1)+' ml/s'],['Learned',z.mlPerPct.toFixed(1)+' ml raises it 1%']]);
  g('cal').textContent=(z.calDry&&z.calWet)?'Calibrated: dry '+z.dryMv+' mV, wet '+z.wetMv+' mV.':
   'Not fully calibrated yet - readings are approximate. Dry: '+(z.calDry?z.dryMv+' mV':'not done')+', wet: '+(z.calWet?z.wetMv+' mV':'not done')+'.'}
 $('#sys').innerHTML=kv([['Dashboard',st.wifi.ip?'http://'+st.wifi.ip:'-'],['Wi-Fi',st.wifi.connected?st.wifi.ssid+' ('+st.wifi.rssi+' dBm)':(st.wifi.ap?'hotspot '+st.wifi.apName:'not connected')],
  ['Clock',st.time||'not set (needs Wi-Fi)'],['Up for',ago(st.uptime).replace(' ago','')],['Quiet hours',st.quiet.enabled?st.quiet.start+':00 - '+st.quiet.end+':00'+(st.quiet.now?' (now)':''):'off']]);
 $('#log').innerHTML=st.events.map(e=>'<li><time>'+esc(e.t)+'</time>'+esc(e.msg)+'</li>').join('');
 drawHistory()}
function drawHistory(){if(!hist||!st)return;for(const z of st.zones){const svg=$('#z'+z.n+'-spark');const v=hist.zones[z.n-1]||[];if(!svg)continue;
 const n=hist.size,W=288,H=64,top=110,x=k=>W*(k+(n-v.length))/(n-1||1),y=m=>H-H*Math.min(m,top)/top;
 let d='',pen=false;v.forEach((m,k)=>{if(m==null){pen=false;return}d+=(pen?'L':'M')+x(k).toFixed(1)+' '+y(m).toFixed(1)+' ';pen=true});
 svg.innerHTML='<rect x="0" width="'+W+'" y="'+y(z.target)+'" height="'+(y(z.below)-y(z.target))+'" fill="var(--band)"/>'+
  '<path d="'+d+'" fill="none" stroke="var(--water)" stroke-width="2" vector-effect="non-scaling-stroke"/>'}}
async function refresh(force){try{st=await getJSON('/api/status');render();if(force)fillInputs()}
 catch(e){$('#chips').innerHTML='<span class="chip bad">Controller offline - retrying&hellip;</span>'}}
async function loadHistory(){try{hist=await getJSON('/api/history');drawHistory()}catch(e){}}
async function start(){try{plants=await getJSON('/api/plants')}catch(e){setTimeout(start,3000);return}
 await refresh();loadHistory();setInterval(refresh,4000);setInterval(loadHistory,60000)}
start();
</script>
</body>
</html>
)ROOTSENSE";
