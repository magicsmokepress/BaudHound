/*
 * BaudHound - ESP32-S3 Wireless Com Terminal  (AP + captive portal + web setup)
 * ----------------------------------------------------------------------------
 * Open SoftAP "BaudHound-XXXX" (always up). Join it -> captive page. Settings
 * page joins your WiFi and configures the COM port. RGB LEDs show net + link status.
 *
 *   Console : http://192.168.4.1/ (on the AP); on your LAN use the IP shown in Settings
 *   Telnet  : telnet <ip> 23
 *
 * Board  : "ESP32S3 Dev Module"  (USB CDC On Boot: Enabled, for the debug Serial)
 * Wiring : chosen RX pin <- target TX, TX pin -> target RX, GND<->GND  (3.3V)
 * Libs   : ESPAsyncWebServer + AsyncTCP.  WiFi/DNSServer/Preferences built in.
 * LEDs   : built-in WS2812 (core >= 3.x provides rgbLedWrite()) + optional external RGB.
 *
 * Onboard WS2812 (network status):
 *   red flash  = serial activity (RX/TX on the wire)
 *   blue       = joined to WiFi
 *   green      = AP up / not joined
 * External RGB 37/39/40 (serial-link status, one at a time):
 *   green blink = scanning / probing for baud
 *   blue        = locked & receiving
 *   red         = RX/TX orientation swapped (while idle)
 */

#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include "driver/gpio.h"

// ---------------- fixed defaults ----------------
#define FW_VERSION "1.3"
#define RGB_LED_PIN 48                      // onboard WS2812 (WiFi / activity status)
// External common-cathode RGB LED: common leg -> a real GND pin (NOT a GPIO).
#define SWAP_LED   37                       // RGB red   = RX/TX swapped
#define PROBE_LED  39                       // RGB green  = probing / scanning
#define LOCK_LED   40                       // RGB blue   = receiving / locked
// ------------------------------------------------

struct Cfg {
  char     ssid[33] = "";
  char     pass[65] = "";
  uint8_t  uart  = 1;        // 1 = UART1 (default), 0 = UART0
  int16_t  rx    = 16;
  int16_t  tx    = 17;
  uint32_t baud  = 0;        // 0 = autodetect
  uint8_t  dbits = 8;
  char     parity= 'N';
  uint8_t  sbits = 1;
  bool     probe = false;    // send a wake char (Enter) during autodetect
} cfg;

Preferences      prefs;
AsyncWebServer   http(80);
AsyncEventSource sse("/stream");
WiFiServer       telnet(23);
DNSServer        dns;

HardwareSerial*  com = &Serial1;
WiFiClient tclients[4];
String     tbuf[4];
// telnet input IAC parser state, per client (handles IAC IAC literal, WILL/WONT/DO/DONT, and SB..SE subneg)
enum { T_NORMAL=0, T_IAC, T_OPT, T_SB, T_SB_IAC };
uint8_t    tstate[4] = {0};

volatile bool scanning = false, applyWifi = false, applySerial = false;
uint32_t lastRxMs = 0, lastTxMs = 0;
bool     anyClient = false;
bool     joining = false; uint32_t joinStart = 0, lastJoinTry = 0;
bool     comSawData = false;
bool     rxtxSwapped = false;
uint32_t gPrint = 0, gTotal = 0, lastAutoDetect = 0;   // garbled-stream watch
String   webCmd; volatile bool webCmdReady = false;    // web command deferred to loop()
int      repollFails = 0;                              // consecutive failed auto re-detects
char     apName[32] = "BaudHound";
String   lineBuf;

const uint32_t BAUD_LIST[] = {9600,14400,19200,28800,38400,57600,74880,115200,230400,460800,921600};
const int      BAUD_N = sizeof(BAUD_LIST)/sizeof(BAUD_LIST[0]);

// ---------- persistence ----------
void cfgLoad(){ prefs.begin("console",true);
  if(prefs.isKey("cfg")) prefs.getBytes("cfg",&cfg,sizeof(cfg)); prefs.end(); }
void cfgSave(){ prefs.begin("console",false);
  prefs.putBytes("cfg",&cfg,sizeof(cfg)); prefs.end(); }

// ---------- helpers ----------
uint32_t serialConfig(int d,char p,int s){
  uint32_t stop=(s>=2)?3:1, par=(p=='E')?2:(p=='O')?3:0;
  return 0x8000000UL | (stop<<4) | ((uint32_t)(d-5)<<2) | par;
}
String jsonEsc(const String& s){ String o; for(char c:s){ if(c=='"'||c=='\\')o+='\\'; o+=c; } return o; }

// ---------- RGB LED ----------
inline void led(uint8_t r,uint8_t g,uint8_t b){       // onboard WS2812; throttled, writes on change
  static uint8_t lr=255,lg=255,lb=255; static uint32_t last=0;
  uint32_t now=millis();
  if(r==lr && g==lg && b==lb) return;
  if(now-last < 15) return;
  rgbLedWrite(RGB_LED_PIN,r,g,b); lr=r; lg=g; lb=b; last=now;
}
void updateLed(){
  uint32_t now=millis();
  bool recentRx = lastRxMs && (now-lastRxMs < 3000);
  // External RGB (37=red, 39=green, 40=blue): serial-link status, one color at a time.
  uint32_t pp = cfg.probe ? 250 : 600;
  bool cr=false, cg=false, cb=false;
  if(scanning)         cg = (now%pp)<(pp/2);          // green blink = probing / scanning
  else if(recentRx)    cb = true;                     // blue = receiving / locked
  else if(rxtxSwapped) cr = true;                     // red = RX/TX swapped (while idle)
  digitalWrite(SWAP_LED,  cr?HIGH:LOW);               // 37 red
  digitalWrite(PROBE_LED, cg?HIGH:LOW);               // 39 green
  digitalWrite(LOCK_LED,  cb?HIGH:LOW);               // 40 blue
  // Onboard WS2812 (GPIO48): green = AP up / not joined, blue = joined to WiFi, red flash = serial data.
  bool tx=(now-lastTxMs)<60, rx=(now-lastRxMs)<60;
  if(tx||rx)                           led(90,0,0);   // red   = serial activity
  else if(WiFi.status()==WL_CONNECTED) led(0,0,90);   // blue  = joined to WiFi
  else                                 led(0,90,0);   // green = AP only / not joined
}

// ---------- UART ----------
void openLink(){
  com->end();
  com = (cfg.uart==0)?&Serial0:&Serial1;
  com->begin(cfg.baud?cfg.baud:115200, serialConfig(cfg.dbits,cfg.parity,cfg.sbits), cfg.rx, cfg.tx);
  if(cfg.rx>=0) gpio_pullup_en((gpio_num_t)cfg.rx);  // idle RX high WITHOUT detaching UART (core 3.x periman)
}
void telnetSend(WiFiClient& c, const String& s){     // escape 0xFF (IAC) so a target's binary bytes aren't read as telnet commands
  for(size_t i=0;i<s.length();i++){ uint8_t b=(uint8_t)s[i];
    c.write(b); if(b==0xFF) c.write((uint8_t)0xFF); }
}
void broadcast(const String& line){                  // status + target lines to all viewers
  // NB: do NOT stamp lastRxMs here — that would light the RX/activity LED on our own
  // bracketed status output. Real wire RX is stamped only in loop()'s read path.
  sse.send(line.c_str(),"message");
  for(auto& c:tclients) if(c&&c.connected()){ telnetSend(c,line); c.print("\r\n"); }
}
void toTarget(const String& s){ lastTxMs=millis(); com->print(s); com->print("\r\n"); }

uint32_t detectBaud(bool active=false){
  const uint32_t WIN=300; const int MINB=16; const int PASSES=3;
  uint32_t good[BAUD_N]={0}, total[BAUD_N]={0};
  bool saw=false; scanning=true;
  // Sweep the baud list several times, accumulating samples, so bursty/periodic
  // data (e.g. one line per second) is reliably caught. Lock early once confident.
  for(int pass=0; pass<PASSES; pass++){
    for(int i=0;i<BAUD_N;i++){
      com->updateBaudRate(BAUD_LIST[i]); delay(3);
      while(com->available()) com->read();
      if(active) com->write('\r');                 // nudge a request/response device
      uint32_t t0=millis();
      while(millis()-t0<WIN){
        if(com->available()){ uint8_t b=com->read(); total[i]++; saw=true;
          if(b==9||b==10||b==13||(b>=32&&b<=126)) good[i]++; }
        updateLed();
        yield();   // feed IDLE/task-WDT; detection blocks loop() by design.
        // ponytail: blocking ~pass*bauds*WIN. Upgrade path = cooperative state machine across loop() iters if transports must stay live mid-scan.
      }
      if(total[i]>=MINB && (float)good[i]/total[i]>=0.92f){     // clear winner -> stop now
        scanning=false; comSawData=true;
        Serial.printf("  locked %u baud (%lu/%lu printable)\n",
                      BAUD_LIST[i],(unsigned long)good[i],(unsigned long)total[i]);
        return BAUD_LIST[i];
      }
    }
  }
  scanning=false; comSawData=saw;
  uint32_t best=0; float bestScore=0.88f;         // else: best ratio with enough samples
  for(int i=0;i<BAUD_N;i++){
    if(total[i]<MINB) continue;
    float sc=(float)good[i]/total[i];
    Serial.printf("  %7u baud: %lu bytes %.0f%%\n",BAUD_LIST[i],(unsigned long)total[i],sc*100);
    if(sc>bestScore){ bestScore=sc; best=BAUD_LIST[i]; }
  }
  return best;
}

void swapRxTx(){ int16_t t=cfg.rx; cfg.rx=cfg.tx; cfg.tx=t; rxtxSwapped=!rxtxSwapped; openLink(); }

// scan current orientation; if the line is silent, flip RX/TX and scan again
uint32_t autoBaud(bool active=false){
  uint32_t d=detectBaud(active);
  if(!d && !comSawData){
    swapRxTx(); broadcast("[no signal - trying swapped RX/TX]");
    d=detectBaud(active);
    if(d || comSawData) cfgSave();   // saw something after flip: keep swapped pins
    else swapRxTx();                 // still silent: restore original, don't persist
  }
  return d;
}

void handleInput(String line){
  String t=line; t.trim();
  if(t.startsWith("~baud")){
    String arg=t.substring(5); arg.trim(); repollFails=0;
    if(arg.length()==0){                              // bare ~baud -> autodetect
      broadcast("[detecting baud...]");
      uint32_t d=autoBaud(cfg.probe);
      if(d){ com->updateBaudRate(d); broadcast("[baud = "+String(d)+"]"); }
      else   broadcast("[no signal / undetectable]");
    } else if(arg=="auto"){                           // ~baud auto -> autodetect mode
      cfg.baud=0; cfgSave(); openLink(); broadcast("[baud = auto]");
    } else {                                          // ~baud <n> -> pin a fixed baud
      long n=arg.toInt();
      if(n>=300 && n<=2000000){ cfg.baud=(uint32_t)n; cfgSave(); openLink();
        broadcast("[baud fixed = "+String(n)+"]"); }
      else broadcast("[bad baud]");
    }
    return; }
  if(t.startsWith("~probe")){                 // toggle persistent wake-probe (on/off/bare-toggle)
    String a=t.substring(6); a.trim();
    if(a=="on") cfg.probe=true; else if(a=="off") cfg.probe=false; else cfg.probe=!cfg.probe;
    cfgSave();
    broadcast(String("[wake probe ")+(cfg.probe?"ON":"OFF")+"]");
    if(cfg.probe){                            // just enabled -> probe now
      uint32_t d=autoBaud(true);
      if(d){ com->updateBaudRate(d); broadcast("[baud = "+String(d)+"]"); }
      else   broadcast("[no response]");
    }
    return; }
  if(t=="~swap"){ swapRxTx(); cfgSave();
    broadcast("[swapped: RX="+String(cfg.rx)+" TX="+String(cfg.tx)+"]"); return; }
  if(t=="~version"||t=="~info"){
    broadcast(String("[BaudHound v")+FW_VERSION+"  uart"+cfg.uart+" rx"+cfg.rx+" tx"+cfg.tx
      +" baud="+(cfg.baud?String(cfg.baud):String("auto"))+" probe="+(cfg.probe?"on":"off")
      +" swapped="+(rxtxSwapped?"yes":"no")+"]"); return; }
  if(t=="~ledtest"){
    broadcast("[ledtest] external RGB 0.7s each: GPIO37(red), GPIO39(green), GPIO40(blue); then onboard R/G/B/W");
    int pins[]={SWAP_LED,PROBE_LED,LOCK_LED};
    for(int i=0;i<3;i++){ digitalWrite(pins[i],HIGH); delay(700); digitalWrite(pins[i],LOW); delay(150); }
    led(120,0,0); delay(700); led(0,120,0); delay(700);
    led(0,0,120); delay(700); led(90,90,90); delay(700); led(0,0,0);
    broadcast("[ledtest] done"); return; }
  toTarget(line);
}

void startJoin(){ WiFi.begin(cfg.ssid,cfg.pass); joining=true; joinStart=lastJoinTry=millis(); }

// ---------- pages ----------
const char CONSOLE_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>BaudHound - Wireless Com Terminal</title>
<style>body{margin:0;font:14px/1.4 monospace;background:#111;color:#ddd}
#top{display:flex;justify-content:space-between;align-items:center;background:#000;padding:6px 10px}
#top a{color:#8bf;text-decoration:none;font-size:20px}
#top select{background:#222;color:#8bf;border:1px solid #333;padding:3px;margin-right:8px;font:13px monospace}
#log{padding:8px;white-space:pre-wrap;word-break:break-all;height:calc(100vh - 124px);overflow:auto}
#hints{display:flex;flex-wrap:wrap;gap:6px;padding:5px 8px;background:#000;border-top:1px solid #222}
#hints span{background:#222;color:#8bf;padding:2px 9px;border-radius:10px;font-size:12px;cursor:pointer}
#hints span:active{background:#345}
#bar{display:flex;gap:6px;padding:8px;background:#000}
#in{flex:1;background:#222;color:#0f0;border:1px solid #333;padding:6px;font:inherit}
button{background:#2a4;color:#fff;border:0;padding:6px 12px}
.mini{background:#444;padding:4px 10px;margin-right:8px}</style>
<div id=top><b>&#128062; BaudHound</b><span>
<select id=baudsel onchange="setBaud(this.value)"><option value=auto>Auto</option>
<option>9600</option><option>14400</option><option>19200</option><option>28800</option><option>38400</option><option>57600</option><option>74880</option><option>115200</option><option>230400</option><option>460800</option><option>921600</option></select>
<button class=mini onclick=clearLog()>Clear</button><a href="/settings" title="Settings">&#9881;</a></span></div>
<div id=hints></div>
<div id=log></div>
<div id=bar><input id=in placeholder="type + Enter (~baud to auto-detect)" autofocus>
<button onclick=send()>Send</button></div>
<script>
const log=document.getElementById('log'),in_=document.getElementById('in');
function add(t){const a=log.scrollTop+log.clientHeight>=log.scrollHeight-4;
 log.textContent+=t;if(a)log.scrollTop=log.scrollHeight}
new EventSource('/stream').onmessage=e=>add(e.data+"\n");
function clearLog(){log.textContent=''}
const hb=document.getElementById('hints');
['~baud','~swap','~probe','~probe on','~probe off','~ledtest','~version'].forEach(c=>{
 const s=document.createElement('span');s.textContent=c;
 s.onclick=()=>{in_.value=c;in_.focus()};hb.appendChild(s);});
let hist=[];try{hist=JSON.parse(localStorage.getItem('sc_hist')||'[]')}catch(e){}
let hi=hist.length;
function send(){const c=in_.value;in_.value='';
 if(c.length){add("> "+c+"\n");hist.push(c);if(hist.length>50)hist.shift();
  try{localStorage.setItem('sc_hist',JSON.stringify(hist))}catch(e){}}
 hi=hist.length;
 fetch('/send?c='+encodeURIComponent(c),{method:'POST'})}
function setBaud(v){add("> ~baud "+v+"\n");fetch('/send?c='+encodeURIComponent('~baud '+v),{method:'POST'})}
fetch('/status').then(r=>r.json()).then(s=>{document.getElementById('baudsel').value=(s.baud==0?'auto':String(s.baud))}).catch(e=>{});
in_.addEventListener('keydown',e=>{
 if(e.key==='Enter'){send();}
 else if(e.key==='ArrowUp'){e.preventDefault();
  if(hi>0){hi--;in_.value=hist[hi];
   setTimeout(()=>in_.setSelectionRange(in_.value.length,in_.value.length),0);}}
 else if(e.key==='ArrowDown'){e.preventDefault();
  if(hi<hist.length-1){hi++;in_.value=hist[hi];}else{hi=hist.length;in_.value='';}}});
</script>)HTML";

const char SETTINGS_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>BaudHound Settings</title>
<style>body{margin:0;font:15px/1.5 system-ui,sans-serif;background:#111;color:#ddd}
.wrap{max-width:520px;margin:0 auto;padding:14px}h2{color:#8bf;margin:18px 0 8px}a{color:#8bf}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:12px;margin-bottom:14px}
label{display:block;margin:8px 0 2px;color:#aaa;font-size:13px}
input,select{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #333;padding:8px;border-radius:6px}
.row{display:flex;gap:8px}.row>div{flex:1}
button{margin-top:10px;background:#2a6;color:#fff;border:0;padding:9px 14px;border-radius:6px;cursor:pointer}
#nets div{padding:6px;border-bottom:1px solid #262626;cursor:pointer}#nets div:hover{background:#242424}
small{color:#888}</style>
<div class=wrap>
<a href="/">&larr; Console</a>
<h2>WiFi</h2>
<div class=card>
 <div id=stat><small>loading&hellip;</small></div>
 <button onclick=scan()>Scan networks</button><div id=nets></div>
 <label>SSID</label><input id=ssid>
 <label>Password</label><input id=pass type=password>
 <button onclick=joinWifi()>Save &amp; Join</button>
</div>
<h2>Serial / COM</h2>
<div class=card>
 <div class=row>
  <div><label>Port</label><select id=uart><option value=1>UART1</option><option value=0>UART0</option></select></div>
  <div><label>RX pin</label><input id=rx type=number></div>
  <div><label>TX pin</label><input id=tx type=number></div>
 </div>
 <label>Baud</label>
 <select id=baud><option value=0>Auto-detect</option>
  <option>9600</option><option>14400</option><option>19200</option><option>28800</option><option>38400</option><option>57600</option>
  <option>74880</option><option>115200</option><option>230400</option><option>460800</option><option>921600</option></select>
 <div class=row>
  <div><label>Data</label><select id=dbits><option>8</option><option>7</option><option>6</option><option>5</option></select></div>
  <div><label>Parity</label><select id=parity><option>N</option><option>E</option><option>O</option></select></div>
  <div><label>Stop</label><select id=sbits><option>1</option><option>2</option></select></div>
 </div>
 <label>Wake probe (send Enter during autodetect to wake a silent console)</label>
 <select id=probe><option value=0>Off</option><option value=1>On</option></select>
 <button onclick=applySerial()>Apply &amp; Save</button>
 <button onclick=swapRxTx() style="background:#a63">Swap RX/TX</button>
</div></div>
<script>
function g(id){return document.getElementById(id)}
function load(){fetch('/status').then(r=>r.json()).then(s=>{
 g('stat').innerHTML=(s.sta?('Joined <b>'+s.ssid+'</b> &mdash; '+s.ip):'Not joined (AP only)')
  +'<br><small>AP: '+s.ap+' @ '+s.apip+'</small>';
 g('ssid').value=s.ssid;g('uart').value=s.uart;g('rx').value=s.rx;g('tx').value=s.tx;
 g('baud').value=s.baud;g('dbits').value=s.dbits;g('parity').value=s.parity;g('sbits').value=s.sbits;
 g('probe').value=s.probe;})}
function scan(){g('nets').innerHTML='<small>scanning&hellip;</small>';pollScan()}
function pollScan(){fetch('/scan').then(r=>r.json()).then(s=>{
  if(s.scanning){setTimeout(pollScan,1200);return}          // async scan still running -> poll again
  var l=s.nets||[];g('nets').innerHTML='';
  l.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{let d=document.createElement('div');
   d.textContent=(n.enc?'\uD83D\uDD12 ':'\uD83D\uDD13 ')+n.ssid+'  ('+n.rssi+')';
   d.onclick=()=>{g('ssid').value=n.ssid;g('pass').focus()};g('nets').appendChild(d)})
  if(!l.length)g('nets').innerHTML='<small>no networks found</small>'
 }).catch(e=>{g('nets').innerHTML='<small>scan failed</small>'})}
function joinWifi(){fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
 body:'ssid='+encodeURIComponent(g('ssid').value)+'&pass='+encodeURIComponent(g('pass').value)})
 .then(()=>{alert('Joining '+g('ssid').value+'\u2026 (AP stays up)');setTimeout(load,4000)})}
function applySerial(){let p=['uart','rx','tx','baud','dbits','parity','sbits','probe']
 .map(k=>k+'='+encodeURIComponent(g(k).value)).join('&');
 fetch('/serial',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p})
 .then(()=>alert('Applied'+(g('baud').value=='0'?' (auto-detecting\u2026)':'')))}
function swapRxTx(){fetch('/swap',{method:'POST'}).then(()=>{load();alert('Swapped RX/TX')})}
load();
</script>)HTML";

// ---------- handlers ----------
void handleStatus(AsyncWebServerRequest* r){
  bool sta=WiFi.status()==WL_CONNECTED; String j="{";
  j+="\"sta\":"+String(sta?"true":"false");
  j+=",\"ssid\":\""+jsonEsc(cfg.ssid)+"\"";
  j+=",\"ip\":\""+(sta?WiFi.localIP().toString():String("-"))+"\"";
  j+=",\"ap\":\""+String(apName)+"\",\"apip\":\""+WiFi.softAPIP().toString()+"\"";
  j+=",\"uart\":"+String(cfg.uart)+",\"rx\":"+String(cfg.rx)+",\"tx\":"+String(cfg.tx);
  j+=",\"baud\":"+String(cfg.baud)+",\"dbits\":"+String(cfg.dbits);
  j+=",\"parity\":\""+String(cfg.parity)+"\",\"sbits\":"+String(cfg.sbits);
  j+=",\"probe\":"+String(cfg.probe?1:0)+"}";
  r->send(200,"application/json",j);
}
// Non-blocking: never call the synchronous WiFi.scanNetworks() here — this runs on the
// AsyncTCP task and would stall all HTTP for seconds (and trip the async_tcp WDT). Instead
// kick an async scan and let the client poll; scanComplete()/SSID()/etc. are cheap reads.
void handleScan(AsyncWebServerRequest* r){
  int n=WiFi.scanComplete();
  if(n==WIFI_SCAN_RUNNING){ r->send(200,"application/json","{\"scanning\":true}"); return; }
  if(n==WIFI_SCAN_FAILED){ WiFi.scanNetworks(true);   // async=true -> returns immediately
    r->send(200,"application/json","{\"scanning\":true}"); return; }
  String j="{\"nets\":[";
  for(int i=0;i<n;i++){ if(i)j+=",";
    j+="{\"ssid\":\""+jsonEsc(WiFi.SSID(i))+"\",\"rssi\":"+String(WiFi.RSSI(i))
      +",\"enc\":"+(WiFi.encryptionType(i)==WIFI_AUTH_OPEN?"false":"true")+"}"; }
  j+="]}"; WiFi.scanDelete();                          // free; next /scan (no scan running) starts a fresh one
  r->send(200,"application/json",j);
}

void setup(){
  Serial.begin(115200);
  led(0,0,0);
  pinMode(PROBE_LED,  OUTPUT); digitalWrite(PROBE_LED,  LOW);
  pinMode(LOCK_LED,   OUTPUT); digitalWrite(LOCK_LED,   LOW);
  pinMode(SWAP_LED,   OUTPUT); digitalWrite(SWAP_LED,   LOW);
  cfgLoad();
  openLink();

  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apName,sizeof(apName),"BaudHound-%02X%02X",mac[4],mac[5]);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName);              // open AP (no password)
  if(cfg.ssid[0]) startJoin();

  dns.start(53,"*",WiFi.softAPIP());

  Serial.printf("\n== BaudHound (Wireless Com Terminal) v%s ==\n",FW_VERSION);
  Serial.printf("AP: %s (open) @ %s\n",apName,WiFi.softAPIP().toString().c_str());
  Serial.printf("Console: http://%s/   Telnet :23\n",WiFi.softAPIP().toString().c_str());

  if(cfg.baud==0){ Serial.println("Auto-detecting baud...");
    uint32_t d=autoBaud(cfg.probe);
    if(d){ com->updateBaudRate(d); Serial.printf("Detected %u\n",d); }
    else   Serial.println("No signal; using 115200"); }

  http.on("/",         HTTP_GET,  [](AsyncWebServerRequest* r){ r->send_P(200,"text/html",CONSOLE_HTML); });
  http.on("/settings", HTTP_GET,  [](AsyncWebServerRequest* r){ r->send_P(200,"text/html",SETTINGS_HTML); });
  http.on("/status",   HTTP_GET,  handleStatus);
  http.on("/scan",     HTTP_GET,  handleScan);
  http.on("/send",     HTTP_POST, [](AsyncWebServerRequest* r){
    if(r->hasParam("c")){ webCmd=r->getParam("c")->value(); webCmdReady=true; }  // run in loop(), not async task
    r->send(200,"text/plain","ok"); });
  http.on("/wifi",     HTTP_POST, [](AsyncWebServerRequest* r){
    if(r->hasParam("ssid",true)) strlcpy(cfg.ssid,r->getParam("ssid",true)->value().c_str(),sizeof(cfg.ssid));
    if(r->hasParam("pass",true)) strlcpy(cfg.pass,r->getParam("pass",true)->value().c_str(),sizeof(cfg.pass));
    cfgSave(); applyWifi=true; r->send(200,"text/plain","ok"); });
  http.on("/serial",   HTTP_POST, [](AsyncWebServerRequest* r){
    auto P=[&](const char*k,long d)->long{ return r->hasParam(k,true)?r->getParam(k,true)->value().toInt():d; };
    cfg.uart=P("uart",cfg.uart)==0?0:1;
    cfg.rx=P("rx",cfg.rx); cfg.tx=P("tx",cfg.tx);
    cfg.baud=P("baud",cfg.baud);
    cfg.dbits=constrain((int)P("dbits",cfg.dbits),5,8);
    cfg.sbits=P("sbits",cfg.sbits)==2?2:1;
    if(r->hasParam("parity",true)){ char c=r->getParam("parity",true)->value()[0]; cfg.parity=(c=='E'||c=='O')?c:'N'; }
    cfg.probe=P("probe",cfg.probe?1:0)!=0;
    cfgSave(); applySerial=true; r->send(200,"text/plain","ok"); });
  http.on("/swap",     HTTP_POST, [](AsyncWebServerRequest* r){
    swapRxTx(); cfgSave(); r->send(200,"text/plain","ok"); });
  sse.onConnect([](AsyncEventSourceClient* c){ c->send("[web client connected]"); });
  http.addHandler(&sse);
  http.onNotFound([](AsyncWebServerRequest* r){        // captive portal -> bounce to console at the IP (no DNS needed)
    r->send(200,"text/html",
      "<!doctype html><meta http-equiv=refresh content='0;url=http://192.168.4.1/'>"
      "<body style='background:#111;color:#8bf;font:16px sans-serif;text-align:center;padding-top:40px'>"
      "<a href='http://192.168.4.1/'>Open BaudHound &#8594;</a></body>"); });
  http.begin();

  telnet.begin(); telnet.setNoDelay(true);
}

void loop(){
  dns.processNextRequest();
  if(webCmdReady){ webCmdReady=false; handleInput(webCmd); }   // web commands run here (single-threaded UART)
  if(applyWifi){ applyWifi=false; startJoin(); }
  if(applySerial){ applySerial=false; openLink();
    if(cfg.baud==0){ uint32_t d=autoBaud(cfg.probe); if(d) com->updateBaudRate(d); } }

  // WiFi join state machine (drives the AP-only LED colors)
  if(WiFi.status()==WL_CONNECTED) joining=false;
  else {
    if(joining && millis()-joinStart > 20000) joining=false;                 // attempt timed out
    if(cfg.ssid[0] && !joining && millis()-lastJoinTry > 30000) startJoin(); // auto-retry
  }

  if(telnet.hasClient()){
    bool placed=false;
    for(int i=0;i<4;i++) if(!tclients[i]||!tclients[i].connected()){
      tclients[i]=telnet.available(); tbuf[i]=""; tstate[i]=T_NORMAL;
      tclients[i].println("[connected]"); placed=true; break; }
    if(!placed) telnet.available().stop();
  }
  for(int i=0;i<4;i++){ WiFiClient& c=tclients[i];
    if(!c||!c.connected()) continue;
    while(c.available()){ uint8_t b=c.read();
      switch(tstate[i]){
        case T_IAC:                                        // prev byte was IAC (0xFF)
          if(b==0xFF){ if(tbuf[i].length()<512) tbuf[i]+=(char)0xFF; tstate[i]=T_NORMAL; } // IAC IAC = literal 0xFF
          else if(b==250)            tstate[i]=T_SB;       // SB  -> subnegotiation
          else if(b>=251 && b<=254)  tstate[i]=T_OPT;      // WILL/WONT/DO/DONT -> one option byte follows
          else                       tstate[i]=T_NORMAL;   // other 2-byte command
          break;
        case T_OPT:  tstate[i]=T_NORMAL; break;            // swallow the option byte
        case T_SB:   if(b==0xFF) tstate[i]=T_SB_IAC; break;// skip subneg payload until IAC SE
        case T_SB_IAC: tstate[i]=(b==240)?T_NORMAL:T_SB; break; // IAC SE ends it; IAC IAC stays in SB
        default:                                           // T_NORMAL
          if(b==0xFF)                       tstate[i]=T_IAC;
          else if(b=='\n'){ handleInput(tbuf[i]); tbuf[i]=""; }
          else if(b!='\r' && tbuf[i].length()<512) tbuf[i]+=(char)b;
      }
    }
  }
  // USB terminal (native USB CDC) <-> target : raw transparent passthrough
  if(Serial){ while(Serial.available()){ com->write((uint8_t)Serial.read()); lastTxMs=millis(); } }

  int budget=512;                             // bound per-loop reads so a fast/binary
  while(com->available() && budget-->0){       // stream can't starve the loop (watchdog)
    char ch=com->read();
    if(Serial) Serial.write(ch);              // mirror raw bytes to USB terminal
    lastRxMs=millis();
    gTotal++; if(ch==9||ch==10||ch==13||((uint8_t)ch>=32&&(uint8_t)ch<=126)) gPrint++;
    if(ch=='\n'){ broadcast(lineBuf); lineBuf=""; }
    else if(ch!='\r'&&lineBuf.length()<512) lineBuf+=ch; }

  // Idle flush: surface a prompt-without-newline (e.g. "login: ") on web+telnet once the
  // stream pauses, instead of withholding it until the next '\n'. Text only; raw binary -> USB.
  // 40ms idle >> inter-byte gap at 9600+ baud, so this only fires between bursts, not mid-line.
  if(lineBuf.length() && millis()-lastRxMs > 40){ broadcast(lineBuf); lineBuf=""; }

  // Auto-repoll: in Auto mode, if the stream looks garbled (probe moved to a
  // port running a different baud), rescan and lock onto the new speed.
  if(cfg.baud==0 && gTotal>=64){
    if(repollFails<3 && gPrint*100/gTotal < 50 && millis()-lastAutoDetect>4000){
      broadcast("[garbled stream - re-detecting baud]");
      uint32_t d=autoBaud(cfg.probe);
      if(d){ com->updateBaudRate(d); repollFails=0; }
      else if(++repollFails>=3)
        broadcast("[undetectable stream - autodetect paused. Pin a fixed baud in Settings, or run ~baud to retry.]");
      lastAutoDetect=millis();
    }
    gPrint=gTotal=0;
  }

  anyClient=(sse.count()>0);
  for(auto& c:tclients) if(c&&c.connected()) anyClient=true;
  updateLed();
}