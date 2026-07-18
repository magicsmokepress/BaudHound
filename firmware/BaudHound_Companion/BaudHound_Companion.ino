/*
 * ESP32-WROOM Companion Debug Target
 * ----------------------------------
 * A device-under-test simulator for the ESP32-S3 Wireless Serial Console
 * (mobile_TTL_Com_term). It emits console-style UART traffic and answers
 * commands, so you can exercise the console's features against a known source:
 *   - passive baud autodetect  (steady heartbeat stream)
 *   - wake-probe / ~probe       (quiet mode: silent until it receives Enter)
 *   - RX/TX auto-swap           (just reverse the two data wires and watch it recover)
 *   - auto re-detect / repoll   (change this device's baud live, console re-locks)
 *
 * Wiring (3.3V logic, common ground):
 *   WROOM GPIO17 (TX2) -> S3 console RX pin (default GPIO16)
 *   WROOM GPIO16 (RX2) <- S3 console TX pin (default GPIO17)
 *   GND               <-> GND
 *
 * WROOM USB Serial = local debug/control @115200 (watch + drive the companion).
 * Onboard LED (GPIO2) = link activity.  BOOT button (GPIO0) = cycle link baud.
 *
 * Commands (type from the S3 console, or on the WROOM's own USB serial):
 *   help              list commands
 *   status            print a status line
 *   chatty on|off     periodic heartbeat lines (default on)
 *   quiet on|off      prompt-only mode: silent until Enter  (tests wake-probe)
 *   baud <n>          switch link baud to <n>               (tests re-detect)
 *   led on|off        drive the onboard LED
 *   <Enter>           reprints the "device>" prompt (the wake response)
 */

#define LINK      Serial2
#define LINK_RX   16
#define LINK_TX   17
#define LED_PIN   2
#define BTN_PIN   0

const uint32_t BAUDS[] = {9600, 19200, 38400, 57600, 115200, 230400};
const int NB = sizeof(BAUDS) / sizeof(BAUDS[0]);
int  baudIdx = 4;                 // start at 115200

bool chatty = true;               // emit periodic heartbeat
bool quiet  = false;              // set true to boot silent (wake-probe test)
uint32_t hbEvery = 1000, lastHb = 0, seq = 0, lastAct = 0;
String bufLink, bufUsb;

void openLink(uint32_t b){ LINK.end(); delay(5); LINK.begin(b, SERIAL_8N1, LINK_RX, LINK_TX); }
void prompt(Stream& o){ o.print("device> "); }

void banner(Stream& o){
  o.println();
  o.println("=== CompanionOS v1.0  (ESP32-WROOM debug target) ===");
  o.printf ("link @ %u 8N1   type 'help'\r\n", BAUDS[baudIdx]);
}

void processCmd(Stream& o, String line){
  line.trim();
  lastAct = millis();
  if(line.length() == 0){ prompt(o); return; }        // bare Enter = wake / prompt
  Serial.printf("[cmd] %s\r\n", line.c_str());        // mirror to WROOM USB debug

  if(line == "help"){
    o.println("commands: help, status, chatty on|off, quiet on|off, baud <n>, led on|off");
  } else if(line == "status"){
    o.printf("uptime=%lus seq=%lu heap=%u baud=%u chatty=%d quiet=%d\r\n",
             millis()/1000, seq, (unsigned)ESP.getFreeHeap(), BAUDS[baudIdx], chatty, quiet);
  } else if(line == "chatty on"){  chatty = true;  o.println("chatty on");
  } else if(line == "chatty off"){ chatty = false; o.println("chatty off");
  } else if(line == "quiet on"){   quiet  = true;  o.println("quiet on");
  } else if(line == "quiet off"){  quiet  = false; o.println("quiet off");
  } else if(line == "led on"){   digitalWrite(LED_PIN, HIGH); o.println("led on");
  } else if(line == "led off"){  digitalWrite(LED_PIN, LOW);  o.println("led off");
  } else if(line.startsWith("baud ")){
    uint32_t nb = (uint32_t)line.substring(5).toInt();
    if(nb >= 300 && nb <= 1000000){
      o.printf("switching link to %u baud...\r\n", nb); delay(30); LINK.flush();
      openLink(nb); banner(LINK);                     // console should now re-detect
    } else o.println("bad baud");
  } else {
    o.printf("unknown command: %s\r\n", line.c_str());
  }
  prompt(o);
}

// read a stream, echo typed chars back, process on CR (LF ignored)
void feed(Stream& in, String& buf, Stream& out){
  while(in.available()){
    char c = in.read();
    lastAct = millis();
    if(c == '\n') continue;                           // ignore LF (CR is the terminator)
    if(c == '\r'){ out.print("\r\n"); processCmd(out, buf); buf = ""; continue; }
    out.write(c);                                     // echo so the console shows input
    if(buf.length() < 200) buf += c;
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  openLink(BAUDS[baudIdx]);
  Serial.printf("\nCompanion debug target up. Link @ %u 8N1 on GPIO%d(TX)/%d(RX)\n",
                BAUDS[baudIdx], LINK_TX, LINK_RX);
  if(!quiet){ banner(LINK); prompt(LINK); }           // quiet mode stays silent until Enter
}

void loop(){
  feed(LINK,   bufLink, LINK);      // commands from the console
  feed(Serial, bufUsb,  Serial);    // local control via WROOM USB

  // periodic heartbeat (suppressed in quiet mode) -> feeds passive autodetect
  if(chatty && !quiet && millis() - lastHb >= hbEvery){
    lastHb = millis(); seq++; lastAct = millis();
    LINK.printf("[%5lu] hb seq=%lu heap=%u temp=%dC link=OK\r\n",
                millis()/1000, seq, (unsigned)ESP.getFreeHeap(), 25 + (int)(seq % 6));
    Serial.printf("hb seq=%lu\r\n", seq);
  }

  // BOOT button: cycle link baud to test the console's auto re-detect
  static bool prevBtn = HIGH; static uint32_t lastBtn = 0;
  bool b = digitalRead(BTN_PIN);
  if(prevBtn == HIGH && b == LOW && millis() - lastBtn > 250){
    lastBtn = millis();
    baudIdx = (baudIdx + 1) % NB;
    LINK.printf("\r\n[button] switching to %u baud\r\n", BAUDS[baudIdx]);
    delay(20); openLink(BAUDS[baudIdx]);
    if(!quiet){ banner(LINK); prompt(LINK); }
    Serial.printf("baud -> %u\r\n", BAUDS[baudIdx]);
  }
  prevBtn = b;

  // onboard LED = link activity (recent rx/tx or heartbeat)
  digitalWrite(LED_PIN, (millis() - lastAct < 40) ? HIGH : LOW);
}
