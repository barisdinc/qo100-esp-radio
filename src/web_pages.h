#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------- radio UI
static const char PAGE_RADIO[] PROGMEM = R"HTML(<!doctype html>
<html lang="tr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>QO-100 Radyo</title>
<style>
:root{--bg:#0f1419;--card:#1a2129;--fg:#e6edf3;--mut:#8b98a5;--acc:#3fb950;--warn:#f0883e;--line:#2d3640}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif}
main{max-width:560px;margin:auto;padding:16px}
h1{font-size:18px;margin:4px 0 14px;display:flex;justify-content:space-between;align-items:center}
.dot{width:10px;height:10px;border-radius:50%;background:#e5534b;display:inline-block;margin-right:6px}
.dot.on{background:var(--acc)}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:12px}
.freq{font:600 34px ui-monospace,monospace;text-align:center;letter-spacing:1px}
.off{text-align:center;color:var(--mut);font-size:14px;margin-top:2px}
.row{display:flex;gap:6px;flex-wrap:wrap;justify-content:center;margin-top:10px}
button{background:#25303b;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:10px 0;font-size:15px;cursor:pointer;flex:1;min-width:56px}
button:active{background:#34414f}button.on{background:var(--acc);color:#000;border-color:var(--acc)}
input[type=range]{width:100%}
label{display:flex;justify-content:space-between;color:var(--mut);font-size:14px;margin-top:8px}
.bar{height:14px;background:#0b0f13;border-radius:7px;overflow:hidden;margin-top:6px;position:relative}
.bar i{display:block;height:100%;background:linear-gradient(90deg,#3fb950,#d29922,#f85149);width:0}
.bar b{position:absolute;top:0;bottom:0;width:2px;background:#fff}
select,input[type=text],input[type=number]{width:100%;background:#0b0f13;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:9px;font-size:15px}
.small{color:var(--mut);font-size:12px;text-align:center}
a{color:#58a6ff}
</style></head><body><main>
<h1><span><span id="dot" class="dot"></span>QO-100 Radyo</span><span id="sm" style="font:14px monospace">--</span></h1>

<div class="card">
 <div class="freq" id="fq">10489.777000</div>
 <div class="off" id="off">MHz · USB</div>
 <div class="row">
  <button onclick="step(-1000)">-1k</button><button onclick="step(-100)">-100</button><button onclick="step(-10)">-10</button>
  <button onclick="step(10)">+10</button><button onclick="step(100)">+100</button><button onclick="step(1000)">+1k</button>
 </div>
 <label><span>İnce ayar (±3 kHz)</span><span id="ftv">0 Hz</span></label>
 <input type="range" id="ft" min="-3000" max="3000" step="10" value="0">
 <div class="row">
  <input type="number" id="fin" step="0.000001" placeholder="MHz, ör. 10489.777">
  <button style="flex:0 0 70px" onclick="setMHz()">Git</button>
  <button style="flex:0 0 90px" onclick="center()">Merkez</button>
 </div>
</div>

<div class="card">
 <div class="row" style="margin-top:0">
  <button id="mute" onclick="send({mute:st.mute?0:1})">Ses açık</button>
 </div>
 <label><span>Ses seviyesi</span><span id="volv"></span></label>
 <input type="range" id="vol" min="0" max="100">
</div>

<div class="card">
 <label style="margin-top:0"><span>Squelch eşiği</span><span id="sqv"></span></label>
 <input type="range" id="sq" min="-140" max="-40" step="1">
 <div class="bar"><i id="smb"></i><b id="sqm"></b></div>
 <div class="row"><button id="ssq" onclick="send({srvsql:st.srvsql?0:1})">Sunucu squelch: kapalı</button></div>
 <div class="small">En solda squelch kapalıdır. Sinyal eşiğin altına düşünce ses kesilir.</div>
</div>

<div class="card">
 <label style="margin-top:0"><span>WebSDR sunucusu</span></label>
 <select id="srv" onchange="send({server:this.value})"></select>
 <div class="small" id="info" style="margin-top:10px"></div>
 <div class="small" style="margin-top:6px"><a href="/wifi">WiFi ayarları</a></div>
</div>
</main>
<script>
const CENTER=10489777000;let st={},busy=0,drag=0;
const $=id=>document.getElementById(id);
function fmt(hz){return (hz/1e6).toFixed(6)}
function send(o){busy=Date.now();fetch('/api/set?'+new URLSearchParams(o)).then(r=>r.json()).then(show).catch(()=>{})}
function step(d){send({freq:st.freq+d})}
function center(){send({freq:CENTER})}
function setMHz(){const v=parseFloat($('fin').value);if(v>10489&&v<10491)send({freq:Math.round(v*1e6)*1000})}
function show(s){st=s;
 $('dot').className='dot'+(s.conn?' on':'');
 $('fq').textContent=fmt(s.freq);
 const off=s.freq-CENTER;$('off').textContent='MHz · USB · '+(off>=0?'+':'')+off+' Hz';
 if(!drag){$('ft').value=Math.max(-3000,Math.min(3000,off));$('ftv').textContent=off+' Hz'}
 $('mute').textContent=s.mute?'SESSİZ (aç)':'Ses açık (kapat)';$('mute').className=s.mute?'':'on';
 if(document.activeElement!==$('vol'))$('vol').value=s.vol;$('volv').textContent=s.vol+'%';
 if(document.activeElement!==$('sq'))$('sq').value=Math.round(s.sql/10);
 $('sqv').textContent=s.sql<=-1400?'kapalı':(s.sql/10).toFixed(0)+' dB';
 $('ssq').textContent='Sunucu squelch: '+(s.srvsql?'açık':'kapalı');$('ssq').className=s.srvsql?'on':'';
 const db=s.smeter;$('sm').textContent=(s.conn?db.toFixed(1)+' dB':'bağlı değil')+(s.open?'':' · SQL');
 $('smb').style.width=Math.max(0,Math.min(100,(db+140))) +'%';
 $('sqm').style.left=Math.max(0,Math.min(100,(s.sql/10+140)))+'%';
 const sel=$('srv');if(sel.options.length!=s.servers.length){sel.innerHTML='';s.servers.forEach((n,i)=>sel.add(new Option(n,i)))}
 if(document.activeElement!==sel)sel.value=s.server;
 $('info').textContent='IP '+s.ip+' · RSSI '+s.rssi+' dBm · tampon '+s.buf+' · boşalma '+s.under+' · heap '+s.heap;
}
$('ft').oninput=e=>{drag=1;$('ftv').textContent=e.target.value+' Hz'};
$('ft').onchange=e=>{drag=0;send({freq:CENTER+parseInt(e.target.value)})};
$('vol').onchange=e=>send({vol:e.target.value});
$('sq').onchange=e=>send({sql:e.target.value<=-140?-1400:e.target.value*10});
function poll(){if(Date.now()-busy>300)fetch('/api/status').then(r=>r.json()).then(show).catch(()=>{});}
setInterval(poll,700);poll();
</script></body></html>)HTML";

// ---------------------------------------------------------------- WiFi setup
static const char PAGE_WIFI_HEAD[] PROGMEM = R"HTML(<!doctype html>
<html lang="tr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>QO-100 WiFi Ayarı</title>
<style>
body{margin:0;background:#0f1419;color:#e6edf3;font:16px system-ui,sans-serif}
main{max-width:480px;margin:auto;padding:16px}
.card{background:#1a2129;border:1px solid #2d3640;border-radius:10px;padding:14px;margin-bottom:12px}
input,button{width:100%;box-sizing:border-box;padding:11px;margin-top:8px;font-size:16px;border-radius:8px;border:1px solid #2d3640;background:#0b0f13;color:#e6edf3}
button{background:#3fb950;color:#000;font-weight:600;border:0}
li{padding:8px 4px;border-bottom:1px solid #2d3640;cursor:pointer;list-style:none;display:flex;justify-content:space-between}
ul{padding:0;margin:0}small{color:#8b98a5}a{color:#58a6ff}
</style></head><body><main>
<h2>QO-100 Radyo · WiFi</h2>
<div class="card"><small>Bulunan ağlar (seçmek için dokunun):</small><ul>)HTML";

static const char PAGE_WIFI_TAIL[] PROGMEM = R"HTML(</ul></div>
<div class="card"><form method="POST" action="/save">
<input name="ssid" id="ssid" placeholder="Ağ adı (SSID)" required maxlength="32">
<input name="pass" type="password" placeholder="Şifre" maxlength="64">
<button type="submit">Kaydet ve yeniden başlat</button></form></div>
<small><a href="/wifi">Yeniden tara</a> · <a href="/">Radyo</a></small>
</main><script>
document.querySelectorAll('li').forEach(l=>l.onclick=()=>{document.getElementById('ssid').value=l.dataset.s})
</script></body></html>)HTML";

static const char PAGE_SAVED[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Kaydedildi</title></head>
<body style="background:#0f1419;color:#e6edf3;font:16px system-ui;padding:24px">
<h2>Kaydedildi</h2><p>Cihaz yeniden başlıyor ve ağa bağlanmayı deneyecek.
Bağlanırsa arayüze <b>http://qo100.local</b> adresinden (ya da modemin verdiği IP'den) ulaşabilirsiniz.
Bağlanamazsa tekrar <b>qo100</b> erişim noktası açılır.</p></body></html>)HTML";
