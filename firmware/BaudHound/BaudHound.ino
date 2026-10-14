/*
 * BaudHound - ESP32-S3 Wireless Com Terminal  (AP + captive portal + web setup)
 * ----------------------------------------------------------------------------
 * Open SoftAP "BaudHound-XXXX" (always up). Join it -> captive page. Settings
 * page joins your WiFi and configures the COM port. RGB LEDs show net + link status.
 *
 *   Console : http://192.168.4.1/ (on the AP); on your LAN use the IP shown in Settings
 *   Telnet  : telnet <ip> 23        (plain; paused while a TLS session is open)
 *   TLS     : telnet inside TLS on port 992, self-signed certificate made on first boot.
 *             One TLS session at a time, and while it is open nothing else sees or types:
 *             plain telnet clients are dropped and refused, the web console is paused.
 *   Find it : UDP broadcast "BAUDHOUND?" to port 47023; each device answers with a JSON line
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
#include <AsyncUDP.h>
#include "driver/gpio.h"
#include <lwip/sockets.h>
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

// ---------------- fixed defaults ----------------
#define FW_VERSION "1.4"
#define RGB_LED_PIN 48                      // onboard WS2812 (WiFi / activity status)
// External common-cathode RGB LED: common leg -> a real GND pin (NOT a GPIO).
#define SWAP_LED   37                       // RGB red   = RX/TX swapped
#define PROBE_LED  39                       // RGB green  = probing / scanning
#define LOCK_LED   40                       // RGB blue   = receiving / locked
#define DISCOVERY_PORT 47023                // UDP: answers "BAUDHOUND?" so a desktop app can find it
#define TLS_PORT   992                      // telnets: the telnet stream inside TLS
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
  bool     discover = true;  // answer discovery broadcasts (appended: older saved configs keep the default)
} cfg;

Preferences      prefs;
AsyncWebServer   http(80);
AsyncEventSource sse("/stream");
WiFiServer       telnet(23);
DNSServer        dns;
AsyncUDP         discovery;

HardwareSerial*  com = &Serial1;
// One telnet input stream (a plain client or the TLS session): IAC parser state plus the line
// being typed. Handles IAC IAC literal, WILL/WONT/DO/DONT, and SB..SE subnegotiation.
enum { T_NORMAL=0, T_IAC, T_OPT, T_SB, T_SB_IAC };
struct LineIn {
  String  buf;
  uint8_t state=T_NORMAL;
  bool    cr=false;            // last data byte was CR: a following LF or NUL belongs to the same Enter
  void reset(){ buf=""; state=T_NORMAL; cr=false; }
};
WiFiClient tclients[4];
LineIn     tin[4];

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
volatile bool locked=false;                            // a TLS session is open: it alone sees and types
mbedtls_entropy_context  tlsEntropy;
mbedtls_ctr_drbg_context tlsDrbg;
mbedtls_pk_context       tlsKey;
mbedtls_x509_crt         tlsCrt;
mbedtls_ssl_config       tlsConf;
bool     tlsOk=false;                                  // key, cert and config are ready
int      tlsListen=-1;
char     tlsFp[64]="";                                 // "SHA256:<base64>" of the certificate, as OpenSSH prints keys
struct TlsSession {
  int      fd=-1;
  bool     ready=false;                                // handshake done
  uint32_t start=0;
  char     peer[16]="";
  mbedtls_ssl_context ssl;
  LineIn   in;
} tls;

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
void tlsSendLine(const String& line);
void broadcast(const String& line){                  // status + target lines to all viewers
  // NB: do NOT stamp lastRxMs here — that would light the RX/activity LED on our own
  // bracketed status output. Real wire RX is stamped only in loop()'s read path.
  if(locked){ tlsSendLine(line); return; }           // TLS session open: nobody else sees the line
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
      +" swapped="+(rxtxSwapped?"yes":"no")+" discovery="+(cfg.discover?"on":"off")+"]");
    if(tlsOk) broadcast(String("[TLS :")+TLS_PORT+" "+tlsFp+(locked?String(" - session from ")+tls.peer:String(""))+"]");
    return; }
  if(t=="~ledtest"){
    broadcast("[ledtest] external RGB 0.7s each: GPIO37(red), GPIO39(green), GPIO40(blue); then onboard R/G/B/W");
    int pins[]={SWAP_LED,PROBE_LED,LOCK_LED};
    for(int i=0;i<3;i++){ digitalWrite(pins[i],HIGH); delay(700); digitalWrite(pins[i],LOW); delay(150); }
    led(120,0,0); delay(700); led(0,120,0); delay(700);
    led(0,0,120); delay(700); led(90,90,90); delay(700); led(0,0,0);
    broadcast("[ledtest] done"); return; }
  toTarget(line);
}

// Feed one received byte of a telnet stream. Enter arrives as CR LF, CR NUL (RFC 854), bare CR
// (xterm, BaudHoundTerm) or bare LF (nc); any of them ends the line once.
void telnetFeed(LineIn& in, uint8_t b){
  switch(in.state){
    case T_IAC:                                        // prev byte was IAC (0xFF)
      if(b==0xFF){ if(in.buf.length()<512) in.buf+=(char)0xFF; in.state=T_NORMAL; } // IAC IAC = literal 0xFF
      else if(b==250)            in.state=T_SB;        // SB  -> subnegotiation
      else if(b>=251 && b<=254)  in.state=T_OPT;       // WILL/WONT/DO/DONT -> one option byte follows
      else                       in.state=T_NORMAL;    // other 2-byte command
      return;
    case T_OPT:    in.state=T_NORMAL; return;          // swallow the option byte
    case T_SB:     if(b==0xFF) in.state=T_SB_IAC; return; // skip subneg payload until IAC SE
    case T_SB_IAC: in.state=(b==240)?T_NORMAL:T_SB; return; // IAC SE ends it; IAC IAC stays in SB
  }
  if(b==0xFF){ in.state=T_IAC; return; }
  if((b=='\n'||b==0) && in.cr){ in.cr=false; return; }
  in.cr=(b=='\r');
  if(b=='\r'||b=='\n'){ String line=in.buf; in.buf=""; handleInput(line); }
  else if(b==0) {}                                     // stray NUL: not text
  else if(b==8||b==127){ if(in.buf.length()) in.buf.remove(in.buf.length()-1); }  // BS / DEL edit the line
  else if(in.buf.length()<512) in.buf+=(char)b;
}

// ---------- TLS console ----------
// mbedTLS over a raw non-blocking lwIP socket. The key and self-signed certificate are made once
// and kept in NVS, so the fingerprint a client pinned stays valid across reboots.
int tlsBioSend(void* ctx,const unsigned char* b,size_t n){
  int r=send(*(int*)ctx,b,n,MSG_NOSIGNAL);
  if(r<0) return (errno==EAGAIN||errno==EWOULDBLOCK)?MBEDTLS_ERR_SSL_WANT_WRITE:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return r;
}
int tlsBioRecv(void* ctx,unsigned char* b,size_t n){
  int r=recv(*(int*)ctx,b,n,0);
  if(r<0) return (errno==EAGAIN||errno==EWOULDBLOCK)?MBEDTLS_ERR_SSL_WANT_READ:MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return r;                                            // 0 = peer closed
}

bool tlsMakeCert(){                                    // first boot: P-256 key + self-signed cert into NVS
  Serial.println("TLS: generating key and certificate...");
  if(mbedtls_pk_setup(&tlsKey,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) return false;
  if(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(tlsKey),mbedtls_ctr_drbg_random,&tlsDrbg)) return false;
  static uint8_t buf[1024];
  int kl=mbedtls_pk_write_key_der(&tlsKey,buf,sizeof(buf));   // DER is written at the END of buf
  if(kl<=0) return false;
  prefs.begin("tls",false); prefs.putBytes("key",buf+sizeof(buf)-kl,kl); prefs.end();

  mbedtls_x509write_cert c; mbedtls_x509write_crt_init(&c);
  char dn[64]; snprintf(dn,sizeof(dn),"CN=%s,O=BaudHound",apName);
  uint8_t serial[8]; mbedtls_ctr_drbg_random(&tlsDrbg,serial,sizeof(serial)); serial[0]=(serial[0]&0x7F)|0x01;
  mbedtls_x509write_crt_set_version(&c,MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&c,MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&c,&tlsKey);
  mbedtls_x509write_crt_set_issuer_key(&c,&tlsKey);
  int bad=mbedtls_x509write_crt_set_subject_name(&c,dn)
        | mbedtls_x509write_crt_set_issuer_name(&c,dn)
        | mbedtls_x509write_crt_set_serial_raw(&c,serial,sizeof(serial))
        | mbedtls_x509write_crt_set_validity(&c,"20260101000000","20491231235959");
  int cl=bad?-1:mbedtls_x509write_crt_der(&c,buf,sizeof(buf),mbedtls_ctr_drbg_random,&tlsDrbg);
  mbedtls_x509write_crt_free(&c);
  if(cl<=0) return false;
  prefs.begin("tls",false); prefs.putBytes("crt",buf+sizeof(buf)-cl,cl); prefs.end();
  mbedtls_pk_free(&tlsKey); mbedtls_pk_init(&tlsKey);  // reload both from NVS, the same path as every later boot
  return true;
}

bool tlsLoad(){
  static uint8_t buf[1024];
  prefs.begin("tls",true);
  size_t kl=prefs.isKey("key")?prefs.getBytes("key",buf,sizeof(buf)):0;
  prefs.end();
  if(!kl || mbedtls_pk_parse_key(&tlsKey,buf,kl,NULL,0,mbedtls_ctr_drbg_random,&tlsDrbg)) return false;
  prefs.begin("tls",true);
  size_t cl=prefs.isKey("crt")?prefs.getBytes("crt",buf,sizeof(buf)):0;
  prefs.end();
  if(!cl || mbedtls_x509_crt_parse_der(&tlsCrt,buf,cl)) return false;
  uint8_t h[32]; mbedtls_sha256(buf,cl,h,0);
  size_t n=0; strcpy(tlsFp,"SHA256:");
  mbedtls_base64_encode((uint8_t*)tlsFp+7,sizeof(tlsFp)-8,&n,h,sizeof(h));
  tlsFp[7+n]=0; while(n && tlsFp[7+n-1]=='='){ tlsFp[7+n-1]=0; n--; }  // unpadded, as OpenSSH prints it
  return true;
}

void tlsBegin(){
  mbedtls_entropy_init(&tlsEntropy); mbedtls_ctr_drbg_init(&tlsDrbg);
  mbedtls_pk_init(&tlsKey); mbedtls_x509_crt_init(&tlsCrt); mbedtls_ssl_config_init(&tlsConf);
  if(mbedtls_ctr_drbg_seed(&tlsDrbg,mbedtls_entropy_func,&tlsEntropy,(const uint8_t*)apName,strlen(apName))) return;
  if(!tlsLoad()){
    mbedtls_pk_free(&tlsKey); mbedtls_pk_init(&tlsKey); mbedtls_x509_crt_free(&tlsCrt); mbedtls_x509_crt_init(&tlsCrt);
    if(!tlsMakeCert() || !tlsLoad()){ Serial.println("TLS: no certificate, TLS console disabled"); return; }
  }
  if(mbedtls_ssl_config_defaults(&tlsConf,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)) return;
  mbedtls_ssl_conf_rng(&tlsConf,mbedtls_ctr_drbg_random,&tlsDrbg);
  mbedtls_ssl_conf_min_tls_version(&tlsConf,MBEDTLS_SSL_VERSION_TLS1_2);
  if(mbedtls_ssl_conf_own_cert(&tlsConf,&tlsCrt,&tlsKey)) return;

  tlsListen=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);
  if(tlsListen<0) return;
  int one=1; setsockopt(tlsListen,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
  sockaddr_in a={}; a.sin_family=AF_INET; a.sin_port=htons(TLS_PORT); a.sin_addr.s_addr=htonl(INADDR_ANY);
  if(bind(tlsListen,(sockaddr*)&a,sizeof(a))<0 || listen(tlsListen,2)<0){ close(tlsListen); tlsListen=-1; return; }
  fcntl(tlsListen,F_SETFL,fcntl(tlsListen,F_GETFL,0)|O_NONBLOCK);
  tlsOk=true;
  Serial.printf("TLS console :%d  %s\n",TLS_PORT,tlsFp);
}

void lockOn(){                                         // TLS session is up: it becomes the only viewer
  String msg=String("[locked: TLS session from ")+tls.peer+" - web console paused]";
  sse.send(msg.c_str(),"message");
  for(auto& c:tclients) if(c&&c.connected()){ c.print("[closed: a TLS session started]\r\n"); c.stop(); }
  locked=true;
  broadcast(String("[TLS session open - ")+apName+" v" FW_VERSION ", plain telnet and web console are paused]");
}

void tlsDrop(){
  if(tls.fd<0) return;
  bool was=tls.ready;
  mbedtls_ssl_close_notify(&tls.ssl);                  // best effort; the socket is closed regardless
  mbedtls_ssl_free(&tls.ssl);
  close(tls.fd); tls.fd=-1; tls.ready=false; tls.in.reset();
  if(was){ locked=false; broadcast("[TLS session ended - web console live]"); }
}

void tlsWrite(const uint8_t* p,size_t n){
  if(tls.fd<0 || !tls.ready) return;
  uint32_t t0=millis();
  while(n){
    int r=mbedtls_ssl_write(&tls.ssl,p,n);
    if(r>0){ p+=r; n-=r; continue; }
    if((r==MBEDTLS_ERR_SSL_WANT_WRITE||r==MBEDTLS_ERR_SSL_WANT_READ) && millis()-t0<2000){ delay(1); continue; }
    tlsDrop(); return;                                 // stalled 2 s or failed: the client is gone
  }
}

void tlsSendLine(const String& line){                  // same framing as plain telnet: IAC escaped, CR LF
  if(tls.fd<0 || !tls.ready) return;
  static uint8_t out[1040]; size_t n=0;
  for(size_t i=0;i<line.length() && n<sizeof(out)-4;i++){ uint8_t b=(uint8_t)line[i];
    out[n++]=b; if(b==0xFF) out[n++]=0xFF; }
  out[n++]='\r'; out[n++]='\n';
  tlsWrite(out,n);
}

void tlsPoll(){
  if(!tlsOk) return;
  sockaddr_in peer; socklen_t pl=sizeof(peer);
  int fd=accept(tlsListen,(sockaddr*)&peer,&pl);
  if(fd>=0){
    if(tls.fd>=0){ close(fd); }                        // one TLS session at a time: refused before any handshake
    else {
      fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
      // A laptop that leaves Wi-Fi sends no FIN. Keepalive frees the lock ~25 s later instead of never.
      int on=1,idle=10,intvl=5,cnt=3;
      setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof(on));
      setsockopt(fd,IPPROTO_TCP,TCP_KEEPIDLE,&idle,sizeof(idle));
      setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&intvl,sizeof(intvl));
      setsockopt(fd,IPPROTO_TCP,TCP_KEEPCNT,&cnt,sizeof(cnt));
      setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on));
      tls.fd=fd; tls.ready=false; tls.start=millis(); tls.in.reset();
      inet_ntoa_r(peer.sin_addr,tls.peer,sizeof(tls.peer));
      mbedtls_ssl_init(&tls.ssl);
      if(mbedtls_ssl_setup(&tls.ssl,&tlsConf)){ mbedtls_ssl_free(&tls.ssl); close(fd); tls.fd=-1; }
      else mbedtls_ssl_set_bio(&tls.ssl,&tls.fd,tlsBioSend,tlsBioRecv,NULL);
    }
  }
  if(tls.fd<0) return;
  if(!tls.ready){                                      // handshake, one step per loop() so nothing else stalls
    int r=mbedtls_ssl_handshake(&tls.ssl);
    if(r==0){ tls.ready=true; Serial.printf("TLS session from %s\n",tls.peer); lockOn(); }
    else if((r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE) || millis()-tls.start>15000) tlsDrop();
    return;
  }
  uint8_t b[128];
  int r=mbedtls_ssl_read(&tls.ssl,b,sizeof(b));       // one record per loop(), like the other inputs' budgets
  if(r>0){ for(int i=0;i<r && tls.fd>=0;i++) telnetFeed(tls.in,b[i]); }   // a command may end the session
  else if(r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE) tlsDrop();   // 0, close_notify or error
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
function locked(r){if(r.status==423)add("[locked: a TLS session is active - input refused]\n")}
function send(){const c=in_.value;in_.value='';
 if(c.length){add("> "+c+"\n");hist.push(c);if(hist.length>50)hist.shift();
  try{localStorage.setItem('sc_hist',JSON.stringify(hist))}catch(e){}}
 hi=hist.length;
 fetch('/send?c='+encodeURIComponent(c),{method:'POST'}).then(locked)}
function setBaud(v){add("> ~baud "+v+"\n");fetch('/send?c='+encodeURIComponent('~baud '+v),{method:'POST'}).then(locked)}
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
 <label>Answer discovery (lets BaudHoundTerm find this device on the network, UDP 47023)</label>
 <select id=discover><option value=1>On</option><option value=0>Off</option></select>
 <button onclick=applySerial()>Apply &amp; Save</button>
 <button onclick=swapRxTx() style="background:#a63">Swap RX/TX</button>
</div></div>
<script>
function g(id){return document.getElementById(id)}
function load(){fetch('/status').then(r=>r.json()).then(s=>{
 g('stat').innerHTML=(s.sta?('Joined <b>'+s.ssid+'</b> &mdash; '+s.ip):'Not joined (AP only)')
  +'<br><small>AP: '+s.ap+' @ '+s.apip+'</small>'
  +(s.tls?'<br><small>TLS console :'+s.tls+' - '+s.tlsfp+'</small>':'')
  +(s.locked?'<br><b style="color:#e96">A TLS session is active: changes are refused until it ends.</b>':'');
 g('ssid').value=s.ssid;g('uart').value=s.uart;g('rx').value=s.rx;g('tx').value=s.tx;
 g('baud').value=s.baud;g('dbits').value=s.dbits;g('parity').value=s.parity;g('sbits').value=s.sbits;
 g('probe').value=s.probe;g('discover').value=s.discover;})}
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
 .then(r=>{if(r.status==423){alert('Locked: a TLS session is active');return}
  alert('Joining '+g('ssid').value+'\u2026 (AP stays up)');setTimeout(load,4000)})}
function applySerial(){let p=['uart','rx','tx','baud','dbits','parity','sbits','probe','discover']
 .map(k=>k+'='+encodeURIComponent(g(k).value)).join('&');
 fetch('/serial',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p})
 .then(r=>alert(r.status==423?'Locked: a TLS session is active':'Applied'+(g('baud').value=='0'?' (auto-detecting\u2026)':'')))}
function swapRxTx(){fetch('/swap',{method:'POST'}).then(r=>{load();alert(r.status==423?'Locked: a TLS session is active':'Swapped RX/TX')})}
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
  j+=",\"probe\":"+String(cfg.probe?1:0);
  j+=",\"discover\":"+String(cfg.discover?1:0);
  j+=",\"fw\":\"" FW_VERSION "\",\"rate\":"+String(com->baudRate());
  j+=",\"tls\":"+String(tlsOk?TLS_PORT:0)+",\"tlsfp\":\""+String(tlsFp)+"\"";
  j+=",\"locked\":"+String(locked?"true":"false")+"}";
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

// Discovery: a client broadcasts "BAUDHOUND?" to UDP 47023 and every device on that segment
// answers the sender with one JSON object. Runs on the AsyncUDP task, so it still answers while
// loop() is blocked in a baud scan; it only reads state. Replies are capped per second because a
// spoofed source address would otherwise turn the device into a small reflector.
void onDiscovery(AsyncUDPPacket& p){
  static const char Q[]="BAUDHOUND?";
  if(!cfg.discover || p.length()<sizeof(Q)-1 || memcmp(p.data(),Q,sizeof(Q)-1)!=0) return;
  static uint32_t win=0; static uint8_t sent=0; uint32_t now=millis();
  if(now-win>1000){ win=now; sent=0; }
  if(++sent>8) return;
  bool sta=WiFi.status()==WL_CONNECTED; String j="{\"baudhound\":1";
  j+=",\"name\":\""+String(apName)+"\",\"fw\":\"" FW_VERSION "\"";
  j+=",\"ip\":\""+p.localIP().toString()+"\"";
  j+=",\"apip\":\""+WiFi.softAPIP().toString()+"\"";
  j+=",\"sta\":"+String(sta?"true":"false");
  j+=",\"ssid\":\""+jsonEsc(sta?String(cfg.ssid):String(""))+"\"";
  j+=",\"lanip\":\""+(sta?WiFi.localIP().toString():String(""))+"\"";
  j+=",\"telnet\":23,\"http\":80";
  j+=",\"baud\":"+String(cfg.baud)+",\"rate\":"+String(com->baudRate());
  j+=",\"scanning\":"+String(scanning?"true":"false");
  j+=",\"tls\":"+String(tlsOk?TLS_PORT:0)+",\"tlsfp\":\""+String(tlsFp)+"\"";
  j+=",\"locked\":"+String(locked?"true":"false")+"}";
  p.write((const uint8_t*)j.c_str(),j.length());
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
  tlsBegin();                       // first boot generates the key (a second or two)

  Serial.printf("\n== BaudHound (Wireless Com Terminal) v%s ==\n",FW_VERSION);
  Serial.printf("AP: %s (open) @ %s\n",apName,WiFi.softAPIP().toString().c_str());
  Serial.printf("Console: http://%s/   Telnet :23   TLS :%d\n",WiFi.softAPIP().toString().c_str(),TLS_PORT);

  if(cfg.baud==0){ Serial.println("Auto-detecting baud...");
    uint32_t d=autoBaud(cfg.probe);
    if(d){ com->updateBaudRate(d); Serial.printf("Detected %u\n",d); }
    else   Serial.println("No signal; using 115200"); }

  http.on("/",         HTTP_GET,  [](AsyncWebServerRequest* r){ r->send_P(200,"text/html",CONSOLE_HTML); });
  http.on("/settings", HTTP_GET,  [](AsyncWebServerRequest* r){ r->send_P(200,"text/html",SETTINGS_HTML); });
  http.on("/status",   HTTP_GET,  handleStatus);
  http.on("/scan",     HTTP_GET,  handleScan);
  http.on("/send",     HTTP_POST, [](AsyncWebServerRequest* r){
    if(locked){ r->send(423,"text/plain","locked: a TLS session is active"); return; }
    if(r->hasParam("c")){ webCmd=r->getParam("c")->value(); webCmdReady=true; }  // run in loop(), not async task
    r->send(200,"text/plain","ok"); });
  http.on("/wifi",     HTTP_POST, [](AsyncWebServerRequest* r){
    if(locked){ r->send(423,"text/plain","locked: a TLS session is active"); return; }
    if(r->hasParam("ssid",true)) strlcpy(cfg.ssid,r->getParam("ssid",true)->value().c_str(),sizeof(cfg.ssid));
    if(r->hasParam("pass",true)) strlcpy(cfg.pass,r->getParam("pass",true)->value().c_str(),sizeof(cfg.pass));
    cfgSave(); applyWifi=true; r->send(200,"text/plain","ok"); });
  http.on("/serial",   HTTP_POST, [](AsyncWebServerRequest* r){
    if(locked){ r->send(423,"text/plain","locked: a TLS session is active"); return; }
    auto P=[&](const char*k,long d)->long{ return r->hasParam(k,true)?r->getParam(k,true)->value().toInt():d; };
    cfg.uart=P("uart",cfg.uart)==0?0:1;
    cfg.rx=P("rx",cfg.rx); cfg.tx=P("tx",cfg.tx);
    cfg.baud=P("baud",cfg.baud);
    cfg.dbits=constrain((int)P("dbits",cfg.dbits),5,8);
    cfg.sbits=P("sbits",cfg.sbits)==2?2:1;
    if(r->hasParam("parity",true)){ char c=r->getParam("parity",true)->value()[0]; cfg.parity=(c=='E'||c=='O')?c:'N'; }
    cfg.probe=P("probe",cfg.probe?1:0)!=0;
    cfg.discover=P("discover",cfg.discover?1:0)!=0;
    cfgSave(); applySerial=true; r->send(200,"text/plain","ok"); });
  http.on("/swap",     HTTP_POST, [](AsyncWebServerRequest* r){
    if(locked){ r->send(423,"text/plain","locked: a TLS session is active"); return; }
    swapRxTx(); cfgSave(); r->send(200,"text/plain","ok"); });
  sse.onConnect([](AsyncEventSourceClient* c){
    c->send(locked?"[locked: a TLS session is active - web console paused]":"[web client connected]"); });
  http.addHandler(&sse);
  http.onNotFound([](AsyncWebServerRequest* r){        // captive portal -> bounce to console at the IP (no DNS needed)
    r->send(200,"text/html",
      "<!doctype html><meta http-equiv=refresh content='0;url=http://192.168.4.1/'>"
      "<body style='background:#111;color:#8bf;font:16px sans-serif;text-align:center;padding-top:40px'>"
      "<a href='http://192.168.4.1/'>Open BaudHound &#8594;</a></body>"); });
  http.begin();

  telnet.begin(); telnet.setNoDelay(true);
  if(discovery.listen(DISCOVERY_PORT)) discovery.onPacket(onDiscovery);   // cfg.discover is checked per packet, so the toggle is live
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
    if(locked){ WiFiClient c=telnet.accept(); c.print("[locked: a TLS session is active]\r\n"); c.stop(); }
    else {
      bool placed=false;
      for(int i=0;i<4;i++) if(!tclients[i]||!tclients[i].connected()){
        tclients[i]=telnet.accept(); tin[i].reset();
        tclients[i].println("[connected]"); placed=true; break; }
      if(!placed) telnet.accept().stop();
    }
  }
  for(int i=0;i<4;i++){ WiFiClient& c=tclients[i];
    if(!c||!c.connected()) continue;
    while(c.available() && !locked) telnetFeed(tin[i],c.read());
  }
  tlsPoll();
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