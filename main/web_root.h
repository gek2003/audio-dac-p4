/* Автогенерировано tools/gen_web_root.py — вручную не править.
   Правки страницы: tools/web_page.html, затем из корня проекта:
   python tools/gen_web_root.py */
#define PAGE_HTML \
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' conte" \
    "nt='width=device-width,initial-scale=1'><title>DAC remote</title><style>body" \
    "{background:#14181c;color:#e8e6e3;font-family:sans-serif;max-width:640px;mar" \
    "gin:0 auto;padding:16px}h1{font-size:20px;margin:8px 0}#st{color:#9aa4ad;fon" \
    "t-size:13px;margin-bottom:10px}.row{display:flex;gap:8px;align-items:center;" \
    "margin:10px 0}button{background:#3a4148;color:#e8e6e3;border:0;border-radius" \
    ":8px;padding:14px 10px;font-size:18px;flex:1;cursor:pointer}button:active{ba" \
    "ckground:#4a525a}input[type=text]{flex:4;padding:10px;background:#1d2227;col" \
    "or:#e8e6e3;border:1px solid #3a4148;border-radius:8px;min-width:0}#bar{heigh" \
    "t:28px;background:#1d2227;border-radius:8px;position:relative;overflow:hidde" \
    "n;cursor:pointer}#fill{height:100%;background:#4a7ba6;width:0}#tm{position:a" \
    "bsolute;top:4px;left:8px;font-size:12px}input[type=range]{flex:3;min-width:0" \
    "}li{padding:10px 6px;border-bottom:1px solid #232a30;cursor:pointer;word-bre" \
    "ak:break-all}ul{list-style:none;padding:0;margin:8px 0}.lbl{color:#9aa4ad;fo" \
    "nt-size:13px;margin-top:16px}.dim{color:#5a646c}</style></head><body><h1>DAC" \
    " remote</h1><div id='st' class='dim'>...</div><div id='bar'><div id='fill'><" \
    "/div><div id='tm'></div></div><div class='row'><button onclick=\"volrel(-5)" \
    "\">&minus;</button><input type='range' id='vl' min='0' max='100' value='80'>" \
    "<button onclick=\"volrel(5)\">+</button><button id='mu' onclick=\"api('/api/" \
    "mute')\">M</button></div><div class='row'><button onclick=\"api('/api/prev')" \
    ".then(tick)\">|&lt;</button><button id='pp' onclick=\"api('/api/play').then(" \
    "tick)\">&gt;</button><button onclick=\"api('/api/next').then(tick)\">&gt;|</" \
    "button></div><div class='row'><input type='text' id='url' placeholder='http:" \
    "//... radio or file'><button onclick=\"playUrl()\">GO</button></div><div cla" \
    "ss='lbl'>SD / USB / Phono</div><div class='row'><button id='btsd' onclick='s" \
    "etRoot(\"sd\")'>SD</button><button id='btusb' onclick='setRoot(\"usb\")'>USB" \
    "</button><button onclick=\"api('/api/phono').then(tick)\">PHONO</button></di" \
    "v><ul id='fl'></ul><div class='lbl'>Phono subsonic HPF</div><div class='row'" \
    "><button id='hp0' onclick=\"api('/api/hpf?v=0').then(tick)\">OFF</button><bu" \
    "tton id='hp1' onclick=\"api('/api/hpf?v=1').then(tick)\">GENTLE</button><but" \
    "ton id='hp2' onclick=\"api('/api/hpf?v=2').then(tick)\">STRICT</button></div" \
    "><div class='lbl'>DSD mode</div><div class='row'><button id='dsd0' onclick=" \
    "\"api('/api/dsd?v=0').then(tick)\">DoP</button><button id='dsd1' onclick=\"a" \
    "pi('/api/dsd?v=1').then(tick)\">Native</button></div><div class='dim' id='ds" \
    "dhint'>Native: нужна проводка DATA2/DATA_CLK (GPIO49/50); применяется со сле" \
    "дующего DSD-трека</div><div class='lbl'>Radio stations</div><ul id='stl'></u" \
    "l><div class='lbl'>Firmware</div><div class='row'><input type='text' id='ota" \
    "url' placeholder='http://... firmware.bin'><button onclick='otaFromUrl()'>UR" \
    "L</button><button onclick='otaFromSd()'>SD</button></div><div id='otast' cla" \
    "ss='dim'></div><script>let scrubbing=false,stations=[],curPath='',curOff=0;c" \
    "onst $=id=>document.getElementById(id);$('vl').addEventListener('input',()=>" \
    "{scrubbing=true});$('vl').addEventListener('change',()=>{scrubbing=false;tic" \
    "k()});let busy=false,lastApi=0;function api(u){if(busy||Date.now()-lastApi<3" \
    "00)return Promise.resolve({err:'busy'});busy=true;lastApi=Date.now();return " \
    "fetch(u,{method:'POST'}).then(r=>r.json()).finally(()=>{busy=false})}functio" \
    "n esc(s){return s==null?'':String(s).replace(/&/g,'&amp;').replace(/</g,'&lt" \
    ";')}function tm(ms){const s=Math.floor(ms/1000);const m=Math.floor(s/60)%60," \
    "sec=s%60,h=Math.floor(s/3600);return (h?h+':':'')+Math.floor(s/60)+':'+Strin" \
    "g(sec).padStart(2,'0')}function volrel(d){const v=$('vl');api('/api/volume?v" \
    "='+Math.max(0,Math.min(100,+v.value+d))).then(tick)}function playUrl(){const" \
    " u=$('url').value.trim();if(!u)return;api('/api/src?u='+encodeURIComponent(u" \
    ")).then(tick)}$('vl').addEventListener('change',()=>api('/api/volume?v='+$('" \
    "vl').value));$('bar').addEventListener('click',e=>{const r=$('bar').getBound" \
    "ingClientRect();fetch('/api/status').then(r=>r.json()).then(j=>{if(!j.seekab" \
    "le||!j.total_ms)return;const k=(e.clientX-r.left)/r.width;api('/api/seek?ms=" \
    "'+Math.floor(k*j.total_ms)).then(tick)})})function tick(){fetch('/api/status" \
    "').then(r=>r.json()).then(j=>{const st=j.state=='playing'?'PLAYING':j.state=" \
    "='paused'?'PAUSED':'STOPPED';$('st').textContent=st+' | '+j.ip+' | '+j.ssid+" \
    "' | '+j.vol_db+' dB'+(j.mute?' [MUTE]':'')+' | fs='+(j.fs||'-');$('pp').inne" \
    "rHTML=j.state=='playing'?'II':'&gt;';if(!scrubbing){$('vl').value=j.vol_pct;" \
    "$('fill').style.width=(j.total_ms?100*j.pos_ms/j.total_ms:0)+'%';$('tm').tex" \
    "tContent=j.total_ms?tm(j.pos_ms)+' / '+tm(j.total_ms):'';const hpf=(typeof j" \
    ".hpf==='number')?j.hpf:1;$('hp0').style.background=hpf==0?'#4a7ba6':'#3a4148" \
    "';$('hp1').style.background=hpf==1?'#4a7ba6':'#3a4148';$('hp2').style.backgr" \
    "ound=hpf==2?'#4a7ba6':'#3a4148';const dsdm=(typeof j.dsd==='number')?j.dsd:0" \
    ";$('dsd0').style.background=dsdm==0?'#4a7ba6':'#3a4148';$('dsd1').style.back" \
    "ground=dsdm==1?'#4a7ba6':'#3a4148';}})}setInterval(tick,2000);tick();let cur" \
    "Root='sd';function setRoot(r){curRoot=r;$('btsd').style.background=r=='sd'?'" \
    "#4a7ba6':'#3a4148';$('btusb').style.background=r=='usb'?'#4a7ba6':'#3a4148';" \
    "loadFiles('',0,false)}function loadFiles(p,off,append){curPath=p;curOff=off;" \
    "fetch('/api/files?root='+curRoot+(p?'&path='+encodeURIComponent(p):'')+'&off" \
    "='+off+'&lim=100').then(r=>r.json()).then(j=>{const el=$('fl');if(!append){e" \
    "l.innerHTML='';}if(j.err){el.innerHTML=\"<li class='dim'>\"+j.err+'</li>';re" \
    "turn}if(!append&&p){const li=document.createElement('li');li.textContent='.." \
    " (up)';li.onclick=()=>{const i=p.lastIndexOf('/');loadFiles(i>0?p.slice(0,i)" \
    ":'',0,false)};el.appendChild(li)}if(!j.items||!j.items.length){if(!append)el" \
    ".innerHTML+=\"<li class='dim'>empty</li>\";return}j.items.forEach(it=>{const" \
    " li=document.createElement('li');li.textContent=(it.dir?'[DIR] ':'')+it.name" \
    ";const full=p?(p.endsWith('/')?p:p+'/'+it.name):it.name;li.onclick=()=>it.di" \
    "r?loadFiles(full,0,false):api('/api/playfile?p='+encodeURIComponent(full)+'&" \
    "root='+curRoot).then(()=>{});el.appendChild(li)})const got=off+j.items.lengt" \
    "h;if(j.count>got){const li=document.createElement('li');li.className='dim';l" \
    "i.textContent='... ещё '+(j.count-got);li.onclick=()=>loadFiles(curPath,got," \
    "true);el.appendChild(li)}})}setRoot('sd');function loadStations(){fetch('/ap" \
    "i/stations').then(r=>r.json()).then(j=>{stations=j.items||[];const el=$('stl" \
    "');el.innerHTML='';if(!stations.length){el.innerHTML=\"<li class='dim'>empty" \
    "</li>\";return}stations.forEach((it,idx)=>{const li=document.createElement('" \
    "li');li.textContent=it.name;li.onclick=()=>fetch('/api/station?i='+idx,{meth" \
    "od:'POST'}).then(tick);el.appendChild(li)})})}loadStations();function otaFro" \
    "mUrl(){const u=$('otaurl').value.trim();if(!u)return;api('/api/ota?u='+encod" \
    "eURIComponent(u)).then(j=>{$('otast').textContent=j.err?j.err+' '+j.msg:'sta" \
    "rted';tick()})}function otaFromSd(){api('/api/ota?file=firmware.bin').then(j" \
    "=>{$('otast').textContent=j.err?j.err+' '+j.msg:'started (firmware.bin)';tic" \
    "k()})}function otaTick(){fetch('/api/status').then(r=>r.json()).then(j=>{con" \
    "st o=j.ota||{};$('otast').textContent=o.state=='idle'?'':(o.state+' '+(o.pct" \
    ">=0?o.pct+'% ':'')+(o.msg||''))});}setInterval(otaTick,2000);otaTick();</scr" \
    "ipt></body></html>"
