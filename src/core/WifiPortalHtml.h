#pragma once
#include <Arduino.h>

// Portal de configuración (AP siempre activo + http://companion.local en la LAN).
// Cada sección es una tarjeta; la API está en NetManager::_setupServer().
static const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desktop Companion</title>
<style>
:root{--bg:#101012;--surface:#19191C;--border:#2a2a2f;--text:#F4EFE6;--muted:#848488;--coral:#E58C8A;--blue:#A4B2E6;--green:#3fb950;--red:#f85149;--amber:#f0a03c;}
*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;}
body{background:var(--bg);color:var(--text);padding:16px;}
.wrap{max-width:480px;margin:0 auto;}
h1{font-size:18px;color:var(--coral);margin:4px 0 16px;letter-spacing:.5px;}
.card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:14px;}
.card-title{font-size:12px;font-weight:800;color:var(--coral);text-transform:uppercase;letter-spacing:1px;margin-bottom:12px;display:flex;justify-content:space-between;align-items:center;}
.card-title .tag{font-size:11px;color:var(--muted);text-transform:none;letter-spacing:0;font-weight:600;}
.row{display:flex;justify-content:space-between;gap:12px;padding:7px 0;font-size:13px;border-bottom:1px solid #222226;}
.row:last-child{border-bottom:none;}
.lbl{color:var(--muted);white-space:nowrap;}
.val{font-weight:700;font-family:monospace;text-align:right;word-break:break-all;}
.ok{color:var(--green);}.bad{color:var(--red);}.wait{color:var(--blue);}
label{display:block;font-size:11px;font-weight:700;color:var(--blue);margin:12px 0 6px;text-transform:uppercase;letter-spacing:.5px;}
input,select{width:100%;background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:11px 12px;color:var(--text);font-size:15px;outline:none;}
input:focus,select:focus{border-color:var(--coral);}
.two{display:flex;gap:10px;}.two>div{flex:1;}
.btn{display:block;width:100%;padding:12px;font-size:14px;font-weight:700;border-radius:8px;border:none;cursor:pointer;margin-top:12px;}
.btn-primary{background:var(--coral);color:var(--bg);}
.btn-sub{background:#232328;border:1px solid var(--border);color:var(--text);}
.btn-danger{background:rgba(248,81,73,.12);border:1px solid var(--red);color:var(--red);}
.list{display:none;background:var(--bg);border:1px solid var(--border);border-radius:8px;max-height:220px;overflow-y:auto;margin-top:8px;}
.item{padding:10px 12px;border-bottom:1px solid #1c1c20;cursor:pointer;display:flex;justify-content:space-between;gap:10px;font-size:14px;}
.item:hover{background:#1d1d22;color:var(--coral);}
.item .sig{color:var(--blue);font-weight:700;white-space:nowrap;}
.msg{padding:12px;color:var(--muted);text-align:center;font-size:13px;}
.banner{display:none;margin-top:12px;padding:12px;border-radius:8px;font-size:13px;line-height:1.5;border:1px solid var(--border);background:var(--bg);}
.banner.ok{border-color:var(--green);color:var(--text);}
.banner.bad{border-color:var(--red);color:var(--text);}
.banner.wait{border-color:var(--blue);color:var(--text);}
.banner a{color:var(--coral);font-weight:700;}
.hint{font-size:12px;color:var(--muted);margin-top:8px;line-height:1.4;}
input[type=range]{padding:0;height:6px;-webkit-appearance:none;appearance:none;background:#2a2a2f;border:none;margin:14px 0 4px;}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:var(--coral);cursor:pointer;}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;background:var(--coral);border:none;cursor:pointer;}
.bar{height:8px;background:var(--bg);border:1px solid var(--border);border-radius:4px;overflow:hidden;margin:4px 0 10px;}
.bar>div{height:100%;background:var(--coral);width:0;transition:width .3s;}
.crumbs{font-family:monospace;font-size:13px;margin:6px 0 8px;word-break:break-all;}
.crumbs a{color:var(--blue);cursor:pointer;text-decoration:none;}
.files{background:var(--bg);border:1px solid var(--border);border-radius:8px;max-height:340px;overflow-y:auto;}
.frow{display:flex;align-items:center;gap:8px;padding:9px 10px;border-bottom:1px solid #1c1c20;font-size:14px;}
.frow:last-child{border-bottom:none;}
.frow .fn{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:default;}
.frow .fn.dir{cursor:pointer;color:var(--blue);font-weight:700;}
.frow .fs{color:var(--muted);font-size:12px;font-family:monospace;white-space:nowrap;}
.ib{background:none;border:1px solid var(--border);color:var(--text);border-radius:6px;padding:4px 7px;font-size:13px;cursor:pointer;}
.ib:hover{border-color:var(--coral);}
.grid3{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-top:10px;}
.grid3 .btn{margin-top:0;padding:10px 4px;font-size:13px;}
.prog{font-size:12px;color:var(--muted);margin-top:8px;font-family:monospace;white-space:pre-line;}
.bggrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:10px;}
.bgi{cursor:pointer;border:2px solid var(--border);border-radius:10px;padding:4px;text-align:center;background:var(--bg);}
.bgi canvas{width:100%;aspect-ratio:3/2;border-radius:6px;display:block;background:#111;}
.bgi span{display:block;font-size:12px;color:var(--muted);margin-top:4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.bgi.on{border-color:var(--coral);}
.bgi.on span{color:var(--text);font-weight:700;}
.modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.85);z-index:50;align-items:center;justify-content:center;flex-direction:column;padding:16px;}
.modal canvas{max-width:100%;border-radius:8px;border:1px solid var(--border);}
.modal .cap{margin-top:10px;font-size:13px;color:var(--muted);font-family:monospace;}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#1e1e22;border:1px solid var(--coral);padding:10px 20px;border-radius:20px;font-size:13px;font-weight:700;opacity:0;transition:opacity .3s;pointer-events:none;max-width:90%;text-align:center;}
.toast.show{opacity:1;}
</style>
</head>
<body>
<div class="wrap">
<h1>🤖 Desktop Companion</h1>

<div class="card">
  <div class="card-title">Estado <span id="stUp" class="tag"></span></div>
  <div class="row"><span class="lbl">Wi-Fi</span><span id="stSta" class="val">--</span></div>
  <div class="row"><span class="lbl">Red</span><span id="stSsid" class="val">--</span></div>
  <div class="row"><span class="lbl">IP</span><span id="stIp" class="val">--</span></div>
  <div class="row"><span class="lbl">Señal</span><span id="stRssi" class="val">--</span></div>
  <div class="row"><span class="lbl">Punto de acceso</span><span id="stAp" class="val">--</span></div>
</div>

<div class="card">
  <div class="card-title">Red Wi-Fi <span class="tag">solo 2.4 GHz</span></div>
  <button type="button" class="btn btn-sub" style="margin-top:0" onclick="scanWifi()">🔍 Buscar redes</button>
  <div id="list" class="list"></div>
  <label for="ssid">Nombre de red (SSID)</label>
  <input type="text" id="ssid" placeholder="Tu red Wi-Fi" autocomplete="off" autocapitalize="off">
  <label for="pass">Contraseña</label>
  <input type="password" id="pass" placeholder="Vacía si la red es abierta">
  <button type="button" class="btn btn-primary" onclick="saveWifi()">💾 Guardar y conectar</button>
  <div id="wifiResult" class="banner"></div>
  <button type="button" class="btn btn-danger" onclick="forgetWifi()">Olvidar red</button>
</div>

<div class="card">
  <div class="card-title">Ubicación del tiempo <span id="cityNow" class="tag"></span></div>
  <div class="two">
    <div style="flex:3"><input type="text" id="cityQ" placeholder="Busca tu ciudad"></div>
    <div style="flex:1"><button type="button" class="btn btn-sub" style="margin-top:0;padding:11px" onclick="searchCity()">Buscar</button></div>
  </div>
  <div id="cityList" class="list"></div>
  <div class="two">
    <div><label for="lat">Latitud</label><input type="text" id="lat" inputmode="decimal"></div>
    <div><label for="lon">Longitud</label><input type="text" id="lon" inputmode="decimal"></div>
  </div>
  <label for="city">Nombre</label>
  <input type="text" id="city" maxlength="40">
  <button type="button" class="btn btn-primary" onclick="saveLocation()">💾 Guardar ubicación</button>
  <div class="hint">La búsqueda usa Open-Meteo desde tu navegador: necesita internet. Si estás en el AP sin internet, escribe las coordenadas a mano.</div>
</div>

<div class="card">
  <div class="card-title">Hora</div>
  <label for="tzSel">Zona horaria</label>
  <select id="tzSel" onchange="tzChanged()"></select>
  <div id="tzCustomBox" style="display:none">
    <label for="tzCustom">Cadena POSIX</label>
    <input type="text" id="tzCustom" placeholder="CET-1CEST,M3.5.0,M10.5.0/3" autocapitalize="off">
  </div>
  <div id="tzHint" class="hint"></div>
  <button type="button" class="btn btn-primary" onclick="saveTz()">💾 Guardar zona horaria</button>
</div>

<div class="card">
  <div class="card-title">Pantalla <span id="brVal" class="tag"></span></div>
  <label for="br">Brillo</label>
  <input type="range" id="br" min="5" max="255" step="1">
</div>

<div class="card">
  <div class="card-title">Fondo de inicio <span id="bgCur" class="tag"></span></div>
  <div id="bgGrid" class="bggrid"><div class="msg">Cargando...</div></div>
  <div class="hint">Toca uno para usarlo al momento. Se listan los .565 de /bg; súbelos con "Subir fondo" en la tarjeta microSD. También se elige en la pantalla: Ajustes → Fondo.</div>
</div>

<div class="card">
  <div class="card-title">Punto de acceso <span id="apSec" class="tag"></span></div>
  <label for="apSsid">Nombre del AP</label>
  <input type="text" id="apSsid" maxlength="32" autocapitalize="off">
  <label for="apPass">Clave del AP</label>
  <input type="password" id="apPass" placeholder="Vacía = mantener la actual (mín. 8 caracteres)">
  <label style="display:flex;align-items:center;gap:8px;text-transform:none;font-size:13px;color:var(--text)"><input type="checkbox" id="apOpen" style="width:auto"> Red abierta, sin clave</label>
  <button type="button" class="btn btn-sub" onclick="saveAp()">💾 Guardar AP</button>
  <div class="hint">El AP se reinicia al guardar: si estás conectado a él, vuelve a unirte con el nombre y la clave nuevos.</div>
</div>

<div class="card">
  <div class="card-title">microSD <span id="sdInfo" class="tag">--</span></div>
  <div id="sdNone" style="display:none">
    <div class="msg">No se detecta la tarjeta (¿está metida? ¿FAT32?)</div>
    <button type="button" class="btn btn-sub" onclick="sdMount()">🔌 Volver a montar</button>
  </div>
  <div id="sdBox" style="display:none">
    <div class="bar"><div id="sdBar"></div></div>
    <div id="crumbs" class="crumbs"></div>
    <div id="files" class="files"></div>
    <div class="grid3">
      <button type="button" class="btn btn-primary" onclick="$('bgInput').click()">🖼 Subir fondo</button>
      <button type="button" class="btn btn-sub" onclick="$('fileInput').click()">⬆ Archivos</button>
      <button type="button" class="btn btn-sub" onclick="sdMkdir()">📁 Carpeta</button>
      <button type="button" class="btn btn-sub" onclick="$('icoInput').click()">◫ Subir icono</button>
      <button type="button" class="btn btn-sub" onclick="$('fotoInput').click()">📷 Subir fotos</button>
    </div>
    <input type="file" id="bgInput" accept="image/*" multiple style="display:none">
    <input type="file" id="fileInput" multiple style="display:none">
    <input type="file" id="fotoInput" accept="image/*" multiple style="display:none">
    <input type="file" id="icoInput" accept=".png,.svg,image/png,image/svg+xml" multiple style="display:none">
    <div id="sdProg" class="prog"></div>
    <div class="hint">"Subir fondo" convierte tus PNG/JPG a 480x320 (.565) en el navegador antes de subirlos. Van a la carpeta abierta (por defecto /bg). "Subir icono" convierte PNG/SVG (icon_    <div class="hint">"Subir fondo" convierte tus PNG/JPG a 480x320 (.565) en el navegador antes de subirlos. Van a la carpeta abierta (por defecto /bg).</div>lt;id    <div class="hint">"Subir fondo" convierte tus PNG/JPG a 480x320 (.565) en el navegador antes de subirlos. Van a la carpeta abierta (por defecto /bg).</div>gt;, blanco sobre transparente, 48x48) a /icons/icon_    <div class="hint">"Subir fondo" convierte tus PNG/JPG a 480x320 (.565) en el navegador antes de subirlos. Van a la carpeta abierta (por defecto /bg).</div>lt;id    <div class="hint">"Subir fondo" convierte tus PNG/JPG a 480x320 (.565) en el navegador antes de subirlos. Van a la carpeta abierta (por defecto /bg).</div>gt;.a4 y la pantalla lo usa en lugar del compilado. "Subir fotos" las convierte igual que los fondos y las deja en /fotos para el salvapantallas (Ajustes → REPOSO → FOTOS).</div>
    <button type="button" class="btn btn-danger" onclick="sdFormat()">⚠ Formatear tarjeta (FAT32)</button>
  </div>
</div>

<div class="card">
  <div class="card-title">Sistema</div>
  <button type="button" class="btn btn-danger" style="margin-top:0" onclick="reboot()">🔄 Reiniciar companion</button>
</div>
</div>
<div id="toast" class="toast"></div>
<div id="modal" class="modal" onclick="this.style.display='none'"><canvas id="prev" width="480" height="320"></canvas><div id="prevCap" class="cap"></div></div>

<script>
function $(id){return document.getElementById(id);}
function toast(m){var t=$('toast');t.textContent=m;t.classList.add('show');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),2800);}
function setVal(id,txt,cls){var e=$(id);e.textContent=txt;e.className='val'+(cls?' '+cls:'');}
function post(url,data){var f=new FormData();for(var k in data)f.append(k,data[k]);
  return fetch(url,{method:'POST',body:f}).then(r=>r.json()).then(d=>{if(!d.ok)throw new Error(d.error||'Error');return d;});}
function banner(cls,html){var b=$('wifiResult');b.className='banner '+cls;b.innerHTML=html;b.style.display='block';}
function esc(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}
function fmtUp(s){var d=Math.floor(s/86400),h=Math.floor(s/3600)%24,m=Math.floor(s/60)%60;return 'encendido '+(d?d+'d ':'')+h+'h '+m+'m';}

// ---------- Estado ----------
var ssidFilled=false,lastStatus=null;
function fetchStatus(){
  return fetch('/api/status').then(r=>r.json()).then(d=>{
    lastStatus=d;
    if(d.connected) setVal('stSta','Conectado','ok');
    else if(d.connecting) setVal('stSta','Conectando...','wait');
    else setVal('stSta',d.saved_ssid?('Sin conexión'+(d.error?' ('+d.error+')':'')):'Sin configurar','bad');
    setVal('stSsid',d.saved_ssid||'--');
    setVal('stIp',d.connected?d.ip+'  ·  '+d.host:'--');
    setVal('stRssi',d.connected?d.rssi+' dBm':'--');
    setVal('stAp',d.ap_ssid+'  ·  '+d.ap_ip+'  ('+d.ap_clients+')');
    $('stUp').textContent=fmtUp(d.uptime);
    if(!ssidFilled&&d.saved_ssid){$('ssid').value=d.saved_ssid;ssidFilled=true;}
    return d;
  });
}

// ---------- Wi-Fi ----------
function scanWifi(){
  var l=$('list');l.style.display='block';
  l.innerHTML='<div class="msg">Buscando redes...</div>';
  fetch('/api/scan').then(r=>r.json()).then(d=>{
    if(!d.length){l.innerHTML='<div class="msg">No se encontraron redes</div>';return;}
    l.innerHTML='';
    d.forEach(w=>{
      var it=document.createElement('div');it.className='item';
      var n=document.createElement('span');n.textContent=(w.secure?'🔒 ':'')+w.ssid;
      var s=document.createElement('span');s.className='sig';s.textContent=Math.min(Math.max(2*(w.rssi+100),0),100)+'%';
      it.appendChild(n);it.appendChild(s);
      it.onclick=()=>{$('ssid').value=w.ssid;l.style.display='none';$('pass').value='';$('pass').focus();};
      l.appendChild(it);
    });
  }).catch(()=>{l.innerHTML='<div class="msg bad">Error al escanear</div>';});
}

var watchTimer=null;
function watchConnect(ssid){
  clearInterval(watchTimer);
  var t0=Date.now(),fails=0;
  banner('wait','⏳ Conectando a <b>'+esc(ssid)+'</b>...');
  watchTimer=setInterval(()=>{
    fetchStatus().then(d=>{
      fails=0;
      if(d.connected&&d.saved_ssid===ssid){
        clearInterval(watchTimer);
        banner('ok','✅ Conectado a <b>'+esc(ssid)+'</b><br>IP: <b>'+d.ip+'</b><br>Desde tu red: <a href="http://'+d.host+'">http://'+d.host+'</a> o <a href="http://'+d.ip+'">http://'+d.ip+'</a>');
      }else if(d.error&&Date.now()-t0>4000&&!d.connecting){
        clearInterval(watchTimer);
        banner('bad','❌ No se pudo conectar: <b>'+esc(d.error)+'</b>');
      }else if(Date.now()-t0>25000){
        clearInterval(watchTimer);
        banner('bad','❌ No conecta. Revisa el nombre y la contraseña.');
      }
    }).catch(()=>{
      if(++fails>=3){
        clearInterval(watchTimer);
        banner('wait','📶 Tu dispositivo se ha desconectado del AP (pasa al cambiar de canal). Mira la pantalla del companion: allí aparece si ha conectado y su IP.');
      }
    });
  },1500);
}

function saveWifi(){
  var s=$('ssid').value.trim();
  if(!s){toast('Introduce el nombre de la red');return;}
  post('/api/wifi',{ssid:s,pass:$('pass').value}).then(()=>watchConnect(s)).catch(e=>banner('bad','❌ '+esc(e.message)));
}

function forgetWifi(){
  if(!confirm('¿Olvidar la red guardada? El AP seguirá activo.'))return;
  post('/api/forget',{}).then(()=>{$('ssid').value='';$('pass').value='';$('wifiResult').style.display='none';toast('Red olvidada');fetchStatus();});
}

// ---------- Ubicación ----------
function searchCity(){
  var q=$('cityQ').value.trim();if(!q)return;
  var l=$('cityList');l.style.display='block';l.innerHTML='<div class="msg">Buscando...</div>';
  fetch('https://geocoding-api.open-meteo.com/v1/search?count=6&language=es&name='+encodeURIComponent(q))
  .then(r=>r.json()).then(d=>{
    var res=d.results||[];
    if(!res.length){l.innerHTML='<div class="msg">Sin resultados</div>';return;}
    l.innerHTML='';
    res.forEach(c=>{
      var it=document.createElement('div');it.className='item';
      var n=document.createElement('span');n.textContent=c.name+(c.admin1?', '+c.admin1:'');
      var s=document.createElement('span');s.className='sig';s.textContent=c.country_code||'';
      it.appendChild(n);it.appendChild(s);
      it.onclick=()=>{
        $('lat').value=c.latitude.toFixed(4);$('lon').value=c.longitude.toFixed(4);$('city').value=c.name;
        l.style.display='none';
        var z=TZ.find(t=>t[2]===c.timezone);
        if(z&&z[1]!==$('tzSel').value){$('tzSel').value=z[1];tzChanged();$('tzHint').textContent='Zona sugerida para '+c.name+': '+z[0]+'. Pulsa guardar para aplicarla.';}
      };
      l.appendChild(it);
    });
  }).catch(()=>{l.innerHTML='<div class="msg bad">Sin internet: escribe latitud y longitud a mano</div>';});
}
$('cityQ').addEventListener('keydown',e=>{if(e.key==='Enter')searchCity();});

function saveLocation(){
  post('/api/config',{lat:$('lat').value.trim(),lon:$('lon').value.trim(),city:$('city').value.trim()})
  .then(()=>{$('cityNow').textContent=$('city').value;toast('Ubicación guardada');}).catch(e=>toast(e.message));
}

// ---------- Hora ----------
// [nombre, POSIX, IANA]
var TZ=[
 ['España peninsular / Baleares','CET-1CEST,M3.5.0,M10.5.0/3','Europe/Madrid'],
 ['Canarias','WET0WEST,M3.5.0/1,M10.5.0','Atlantic/Canary'],
 ['Portugal','WET0WEST,M3.5.0/1,M10.5.0','Europe/Lisbon'],
 ['Reino Unido / Irlanda','GMT0BST,M3.5.0/1,M10.5.0','Europe/London'],
 ['Europa central (París, Berlín, Roma)','CET-1CEST,M3.5.0,M10.5.0/3','Europe/Paris'],
 ['Europa del este (Atenas, Helsinki)','EET-2EEST,M3.5.0/3,M10.5.0/4','Europe/Athens'],
 ['México (CDMX)','CST6','America/Mexico_City'],
 ['Colombia / Perú / Ecuador','<-05>5','America/Bogota'],
 ['Venezuela / Bolivia','<-04>4','America/Caracas'],
 ['Chile','<-04>4<-03>,M9.1.6/24,M4.1.6/24','America/Santiago'],
 ['Argentina / Uruguay','<-03>3','America/Argentina/Buenos_Aires'],
 ['EE. UU. Este (Nueva York)','EST5EDT,M3.2.0,M11.1.0','America/New_York'],
 ['EE. UU. Centro (Chicago)','CST6CDT,M3.2.0,M11.1.0','America/Chicago'],
 ['EE. UU. Pacífico (Los Ángeles)','PST8PDT,M3.2.0,M11.1.0','America/Los_Angeles'],
 ['UTC','UTC0','UTC']
];
(function(){var s=$('tzSel');var seen={};TZ.forEach(t=>{if(seen[t[1]+t[0]])return;seen[t[1]+t[0]]=1;var o=document.createElement('option');o.value=t[1];o.textContent=t[0];s.appendChild(o);});
  var o=document.createElement('option');o.value='custom';o.textContent='Otra (POSIX a mano)';s.appendChild(o);})();
function tzChanged(){$('tzCustomBox').style.display=$('tzSel').value==='custom'?'block':'none';$('tzHint').textContent='';}
function saveTz(){
  var v=$('tzSel').value==='custom'?$('tzCustom').value.trim():$('tzSel').value;
  if(!v){toast('Escribe la zona POSIX');return;}
  post('/api/config',{tz:v}).then(()=>{$('tzHint').textContent='';toast('Zona horaria guardada');}).catch(e=>toast(e.message));
}

// ---------- Brillo ----------
var brT=0;
function brLabel(v){$('brVal').textContent=Math.round(v/255*100)+'%';}
$('br').addEventListener('input',e=>{
  var v=e.target.value;brLabel(v);
  if(Date.now()-brT>120){brT=Date.now();post('/api/brightness',{v:v}).catch(()=>{});}
});
$('br').addEventListener('change',e=>{post('/api/config',{brightness:e.target.value}).then(()=>toast('Brillo guardado')).catch(()=>{});});

// ---------- AP ----------
function saveAp(){
  var s=$('apSsid').value.trim(),p=$('apPass').value,open=$('apOpen').checked;
  if(!s){toast('El AP necesita nombre');return;}
  if(!open&&p&&p.length<8){toast('La clave del AP necesita 8 caracteres');return;}
  var data={ap_ssid:s};
  if(open)data.ap_pass='';else if(p)data.ap_pass=p;
  post('/api/config',data).then(()=>{toast('AP guardado: '+s);$('apPass').value='';$('apOpen').checked=false;
    if(open)$('apSec').textContent='abierto';else if(p)$('apSec').textContent='🔒 con clave';}).catch(e=>toast(e.message));
}

function reboot(){
  if(!confirm('¿Reiniciar el companion?'))return;
  post('/api/reboot',{}).catch(()=>{});toast('Reiniciando...');
}

// ---------- microSD ----------
var sdPath='/';
function fmtSize(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(0)+' KB';if(b<1073741824)return (b/1048576).toFixed(1)+' MB';return (b/1073741824).toFixed(2)+' GB';}
function joinPath(dir,name){return (dir.endsWith('/')?dir:dir+'/')+name;}
function q(p){return encodeURIComponent(p);}

function sdInfo(){
  return fetch('/api/sd/info').then(r=>r.json()).then(d=>{
    $('sdNone').style.display=d.mounted?'none':'block';
    $('sdBox').style.display=d.mounted?'block':'none';
    if(!d.mounted){$('sdInfo').textContent='sin tarjeta';return d;}
    $('sdInfo').textContent=d.type+' · '+fmtSize(d.used)+' / '+fmtSize(d.total);
    $('sdBar').style.width=(d.total?Math.max(1,d.used/d.total*100):0)+'%';
    return d;
  });
}

function sdList(path){
  if(path!==undefined)sdPath=path;
  var c=$('crumbs');c.innerHTML='';
  var parts=sdPath.split('/').filter(x=>x),acc='';
  var root=document.createElement('a');root.textContent='/';root.onclick=()=>sdList('/');c.appendChild(root);
  parts.forEach((p,i)=>{acc+='/'+p;var a=document.createElement('a');var target=acc;a.textContent=p+(i<parts.length-1?'/':'');a.onclick=()=>sdList(target);c.appendChild(a);});
  var box=$('files');box.innerHTML='<div class="msg">Cargando...</div>';
  fetch('/api/sd/list?path='+q(sdPath)).then(r=>r.json()).then(list=>{
    if(!Array.isArray(list)){box.innerHTML='<div class="msg bad">'+esc(list.error||'Error')+'</div>';return;}
    list.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name));
    box.innerHTML='';
    if(sdPath!=='/'){list.unshift({name:'..',dir:true,up:true});}
    if(!list.length){box.innerHTML='<div class="msg">Carpeta vacía</div>';return;}
    list.forEach(f=>{
      var row=document.createElement('div');row.className='frow';
      var full=f.up?sdPath.substring(0,sdPath.lastIndexOf('/'))||'/':joinPath(sdPath,f.name);
      var n=document.createElement('span');n.className='fn'+(f.dir?' dir':'');
      n.textContent=(f.dir?'📁 ':(f.name.endsWith('.565')?'🖼 ':'📄 '))+f.name;
      if(f.dir)n.onclick=()=>sdList(full);
      row.appendChild(n);
      if(!f.up){
        if(!f.dir){var s=document.createElement('span');s.className='fs';s.textContent=fmtSize(f.size);row.appendChild(s);}
        if(f.name.endsWith('.565'))row.appendChild(ib('👁','Ver',()=>preview565(full)));
        if(!f.dir)row.appendChild(ib('⬇','Descargar',()=>{location.href='/api/sd/file?dl=1&path='+q(full);}));
        row.appendChild(ib('✎','Renombrar',()=>sdRename(full,f.name)));
        row.appendChild(ib('🗑','Borrar',()=>sdDelete(full,f.dir)));
      }
      box.appendChild(row);
    });
  }).catch(()=>{box.innerHTML='<div class="msg bad">Error al leer la tarjeta</div>';});
}
function ib(txt,title,fn){var b=document.createElement('button');b.type='button';b.className='ib';b.textContent=txt;b.title=title;b.onclick=fn;return b;}
function sdRefresh(){sdInfo();sdList();}

function sdMkdir(){
  var n=prompt('Nombre de la carpeta nueva:');if(!n)return;
  if(/[\\/]/.test(n)){toast('Sin barras en el nombre');return;}
  post('/api/sd/mkdir',{path:joinPath(sdPath,n.trim())}).then(()=>{toast('Carpeta creada');sdList();}).catch(e=>toast(e.message));
}
function sdDelete(path,isDir){
  if(!confirm('¿Borrar '+path+(isDir?' y todo su contenido':'')+'?'))return;
  post('/api/sd/delete',{path:path}).then(()=>{toast('Borrado');sdRefresh();bgList();}).catch(e=>toast(e.message));
}
function sdRename(path,name){
  var n=prompt('Nuevo nombre:',name);if(!n||n===name)return;
  if(/[\\/]/.test(n)){toast('Sin barras en el nombre');return;}
  post('/api/sd/rename',{from:path,to:joinPath(path.substring(0,path.lastIndexOf('/'))||'/',n.trim())}).then(()=>{toast('Renombrado');sdList();bgList();}).catch(e=>toast(e.message));
}
function sdMount(){post('/api/sd/mount',{}).then(()=>{toast('Tarjeta montada');sdRefresh();}).catch(e=>toast(e.message));}
function sdFormat(){
  var t=prompt('Se BORRARÁ TODO el contenido de la microSD.\nEscribe FORMATEAR para confirmar:');
  if(t!=='FORMATEAR'){if(t!==null)toast('Cancelado');return;}
  $('sdProg').textContent='Formateando... (puede tardar hasta un minuto)';
  post('/api/sd/format',{confirm:'FORMATEAR'}).then(()=>{$('sdProg').textContent='✅ Formateada en FAT32. Creada /bg';toast('Tarjeta formateada');sdList('/');sdInfo();})
  .catch(e=>{$('sdProg').textContent='❌ '+e.message;});
}

// Sube un Blob con progreso
function sdUpload(dir,name,blob,label){
  return new Promise((ok,ko)=>{
    var fd=new FormData();fd.append('file',blob,name);
    var x=new XMLHttpRequest();x.open('POST','/api/sd/upload?path='+q(dir));
    x.upload.onprogress=e=>{if(e.lengthComputable)$('sdProg').textContent=label+': '+Math.round(e.loaded/e.total*100)+'%';};
    x.onload=()=>{try{var d=JSON.parse(x.responseText);d.ok?ok(d):ko(new Error(d.error));}catch(e){ko(new Error('Respuesta no válida'));}};
    x.onerror=()=>ko(new Error('Error de red'));
    x.send(fd);
  });
}

$('fileInput').addEventListener('change',async e=>{
  var fs=[...e.target.files];e.target.value='';
  for(var i=0;i<fs.length;i++){
    try{await sdUpload(sdPath,fs[i].name,fs[i],'['+(i+1)+'/'+fs.length+'] '+fs[i].name);}
    catch(err){$('sdProg').textContent='❌ '+fs[i].name+': '+err.message;return;}
  }
  $('sdProg').textContent='✅ '+fs.length+' archivo(s) subidos';sdRefresh();
});

// Imagen -> 480x320 RGB565 little-endian (recorte centrado tipo "cover" + tramado Bayer)
var BAYER=[0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5];
function imgTo565(img){
  var W=480,H=320,c=document.createElement('canvas');c.width=W;c.height=H;
  var x=c.getContext('2d'),s=Math.max(W/img.width,H/img.height),w=img.width*s,h=img.height*s;
  x.imageSmoothingQuality='high';x.drawImage(img,(W-w)/2,(H-h)/2,w,h);
  var d=x.getImageData(0,0,W,H).data,out=new Uint8Array(W*H*2);
  for(var y=0,i=0;y<H;y++)for(var xx=0;xx<W;xx++,i++){
    var t=BAYER[(y&3)*4+(xx&3)]/16-0.5;
    var r=Math.min(255,Math.max(0,d[i*4]+t*8)),g=Math.min(255,Math.max(0,d[i*4+1]+t*4)),b=Math.min(255,Math.max(0,d[i*4+2]+t*8));
    var v=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
    out[i*2]=v&255;out[i*2+1]=v>>8;
  }
  return out;
}
function loadImg(file){return new Promise((ok,ko)=>{var u=URL.createObjectURL(file),im=new Image();im.onload=()=>{URL.revokeObjectURL(u);ok(im);};im.onerror=()=>ko(new Error('No se pudo leer la imagen'));im.src=u;});}

$('bgInput').addEventListener('change',async e=>{
  var fs=[...e.target.files];e.target.value='';
  var dir=sdPath==='/'?'/bg':sdPath;
  if(dir==='/bg')await post('/api/sd/mkdir',{path:'/bg'}).catch(()=>{});   // por si no existe
  for(var i=0;i<fs.length;i++){
    var base=fs[i].name.replace(/\.[^.]+$/,'').toLowerCase().replace(/[^a-z0-9_-]+/g,'_')||'fondo';
    var label='['+(i+1)+'/'+fs.length+'] '+base+'.565';
    try{
      $('sdProg').textContent=label+': convirtiendo...';
      var data=imgTo565(await loadImg(fs[i]));
      await sdUpload(dir,base+'.565',new Blob([data]),label);
    }catch(err){$('sdProg').textContent='❌ '+label+': '+err.message;return;}
  }
  $('sdProg').textContent='✅ '+fs.length+' fondo(s) en '+dir;
  sdList(dir);sdInfo();bgList();
});

// Icono PNG/SVG -> máscara alpha 4 bits (.a4) para /icons: icon_* a 48x48, sb_* a 20x20. Blanco sobre transparente = icono.
function imgToA4(img,S){
  var c=document.createElement('canvas');c.width=S;c.height=S;
  var x=c.getContext('2d'),w=img.naturalWidth||img.width||S,h=img.naturalHeight||img.height||S,s=Math.min(S/w,S/h);
  x.imageSmoothingQuality='high';x.drawImage(img,(S-w*s)/2,(S-h*s)/2,w*s,h*s);
  var d=x.getImageData(0,0,S,S).data,opaque=true;
  for(var i=3;i<d.length;i+=4)if(d[i]<255){opaque=false;break;}
  var out=new Uint8Array(4+S*S/2);out[0]=65;out[1]=52;out[2]=S;out[3]=S;
  for(var i=0;i<S*S;i++){
    var lum=(d[i*4]*299+d[i*4+1]*587+d[i*4+2]*114)/1000,a=opaque?lum:d[i*4+3]*lum/255;
    var v=Math.min(15,Math.round(a*15/255));
    out[4+(i>>1)]|=(i&1)?v:(v<<4);
  }
  return out;
}
$('icoInput').addEventListener('change',async e=>{
  var fs=[...e.target.files];e.target.value='';
  await post('/api/sd/mkdir',{path:'/icons'}).catch(()=>{});
  for(var i=0;i<fs.length;i++){
    var id=fs[i].name.replace(/\.[^.]+$/,'').toLowerCase().replace(/[^a-z0-9_]+/g,'');
    if(!/^(icon|sb)_/.test(id))id='icon_'+id;   // sin prefijo = icono de la home
    var label='['+(i+1)+'/'+fs.length+'] '+id+'.a4';
    try{await sdUpload('/icons',id+'.a4',new Blob([imgToA4(await loadImg(fs[i]),id.startsWith('sb_')?20:48)]),label);}
    catch(err){$('sdProg').textContent='❌ '+label+': '+err.message;return;}
  }
  $('sdProg').textContent='✅ '+fs.length+' icono(s) en /icons';sdInfo();if(sdPath==='/icons')sdList();
});

// Fotos del salvapantallas: igual que los fondos (480x320 .565) pero a /fotos
$('fotoInput').addEventListener('change',async e=>{
  var fs=[...e.target.files];e.target.value='';
  await post('/api/sd/mkdir',{path:'/fotos'}).catch(()=>{});
  for(var i=0;i<fs.length;i++){
    var base=fs[i].name.replace(/\.[^.]+$/,'').toLowerCase().replace(/[^a-z0-9_-]+/g,'_')||'foto';
    var label='['+(i+1)+'/'+fs.length+'] '+base+'.565';
    try{
      $('sdProg').textContent=label+': convirtiendo...';
      await sdUpload('/fotos',base+'.565',new Blob([imgTo565(await loadImg(fs[i]))]),label);
    }catch(err){$('sdProg').textContent='❌ '+label+': '+err.message;return;}
  }
  $('sdProg').textContent='✅ '+fs.length+' foto(s) en /fotos';sdInfo();if(sdPath==='/fotos')sdList();
});

function preview565(path){
  fetch('/api/sd/file?path='+q(path)).then(r=>r.arrayBuffer()).then(buf=>{
    if(buf.byteLength!==480*320*2){toast('No es un fondo 480x320 ('+fmtSize(buf.byteLength)+')');return;}
    var u=new Uint8Array(buf),cv=$('prev'),x=cv.getContext('2d'),im=x.createImageData(480,320);
    for(var i=0;i<480*320;i++){var v=u[i*2]|(u[i*2+1]<<8);
      im.data[i*4]=((v>>11)&31)*255/31;im.data[i*4+1]=((v>>5)&63)*255/63;im.data[i*4+2]=(v&31)*255/31;im.data[i*4+3]=255;}
    x.putImageData(im,0,0);$('prevCap').textContent=path+'  ·  toca para cerrar';$('modal').style.display='flex';
  }).catch(()=>toast('No se pudo leer'));
}

// ---------- Fondo de inicio ----------
var bgHome='';
function bgMark(){document.querySelectorAll('.bgi').forEach(e=>e.classList.toggle('on',e.dataset.path===bgHome));
  $('bgCur').textContent=bgHome.replace(/^.*\//,'').replace(/\.565$/,'');}
function bgThumb(path,cv){
  return fetch('/api/sd/thumb?path='+q(path)).then(r=>r.arrayBuffer()).then(buf=>{
    if(buf.byteLength!==120*80*2)return;
    var u=new Uint8Array(buf),x=cv.getContext('2d'),im=x.createImageData(120,80);
    for(var i=0;i<120*80;i++){var v=u[i*2]|(u[i*2+1]<<8);
      im.data[i*4]=((v>>11)&31)*255/31;im.data[i*4+1]=((v>>5)&63)*255/63;im.data[i*4+2]=(v&31)*255/31;im.data[i*4+3]=255;}
    x.putImageData(im,0,0);
  }).catch(()=>{});
}
function bgList(){
  var g=$('bgGrid');
  return fetch('/api/sd/list?path=/bg').then(r=>r.json()).then(async list=>{
    if(!Array.isArray(list)){g.innerHTML='<div class="msg">'+esc(list.error||'Sin microSD o sin carpeta /bg')+'</div>';return;}
    list=list.filter(f=>!f.dir&&f.name.endsWith('.565')&&f.size===480*320*2).sort((a,b)=>a.name.localeCompare(b.name));
    if(!list.length){g.innerHTML='<div class="msg">No hay fondos en /bg</div>';return;}
    g.innerHTML='';var jobs=[];
    list.forEach(f=>{
      var path='/bg/'+f.name,d=document.createElement('div');d.className='bgi';d.dataset.path=path;
      var cv=document.createElement('canvas');cv.width=120;cv.height=80;d.appendChild(cv);
      var s=document.createElement('span');s.textContent=f.name.replace(/\.565$/,'');d.appendChild(s);
      d.onclick=()=>{if(path===bgHome)return;post('/api/config',{bg_home:path}).then(()=>{bgHome=path;bgMark();toast('Fondo cambiado');}).catch(e=>toast(e.message));};
      g.appendChild(d);jobs.push([path,cv]);
    });
    bgMark();
    for(var j of jobs)await bgThumb(j[0],j[1]);   // de una en una: el ESP32 sirve una petición cada vez
  }).catch(()=>{g.innerHTML='<div class="msg">Error al leer la tarjeta</div>';});
}

// ---------- Inicio ----------
fetch('/api/config').then(r=>r.json()).then(c=>{
  $('lat').value=c.lat;$('lon').value=c.lon;$('city').value=c.city;$('cityNow').textContent=c.city;
  var known=TZ.some(t=>t[1]===c.tz);
  $('tzSel').value=known?c.tz:'custom';if(!known)$('tzCustom').value=c.tz;tzChanged();
  $('br').value=c.brightness;brLabel(c.brightness);
  bgHome=c.bg_home||'';bgMark();
  $('apSsid').value=c.ap_ssid;$('apSec').textContent=c.ap_secure?'🔒 con clave':'abierto';
});
sdInfo().then(d=>{if(d.mounted){sdList('/');bgList();}}).catch(()=>{});
fetchStatus().catch(()=>{});setInterval(()=>fetchStatus().catch(()=>{}),3000);
</script>
</body>
</html>
)rawhtml";
