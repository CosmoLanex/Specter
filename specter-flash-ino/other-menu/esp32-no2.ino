/*
 * ════════════════════════════════════════════════════════════
 *  ESP2 — UI CONTROLLER  "NUKRAX" v3.0
 *  Display + Buttons + Command dispatch to ESP1
 * ════════════════════════════════════════════════════════════
 *  OLED SSD1306 (I2C 4-pin):  SDA=GPIO4  SCL=GPIO5
 *  Button UP     → GPIO 25
 *  Button DOWN   → GPIO 27
 *  Button MIDDLE → GPIO 26
 *  UART to ESP1:  TX=GPIO32 → ESP1 RX=GPIO32
 *                 RX=GPIO17 ← ESP1 TX=GPIO17
 *                 Shared GND mandatory
 * ════════════════════════════════════════════════════════════
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Display ──────────────────────────────────────────────────
#define OLED_SDA  4
#define OLED_SCL  5
#define OLED_ADDR 0x3C
#define SCR_W 128
#define SCR_H  64
Adafruit_SSD1306 oled(SCR_W, SCR_H, &Wire, -1);

// ── Buttons ──────────────────────────────────────────────────
#define PIN_UP   25
#define PIN_DN   27
#define PIN_MID  26
#define DEBOUNCE   180
#define LONG_MS    850

struct Btn {
  uint8_t  pin;
  uint32_t downAt, edgeAt;
  bool     held, longFired;
};
Btn bU={PIN_UP,0,0,false,false};
Btn bD={PIN_DN,0,0,false,false};
Btn bM={PIN_MID,0,0,false,false};

bool tap(Btn &b){
  bool lo=(digitalRead(b.pin)==LOW); uint32_t n=millis();
  if(lo&&!b.held&&n-b.edgeAt>DEBOUNCE){b.held=true;b.downAt=n;b.edgeAt=n;b.longFired=false;return true;}
  if(!lo){b.held=false;b.longFired=false;} return false;
}
bool hold(Btn &b){
  bool lo=(digitalRead(b.pin)==LOW);
  if(lo&&b.held&&!b.longFired&&millis()-b.downAt>=LONG_MS){b.longFired=true;return true;}
  if(!lo){b.held=false;b.longFired=false;} return false;
}

// ── UART ─────────────────────────────────────────────────────
#define LINK_TX   32
#define LINK_RX   17
#define LINK_BAUD 115200
HardwareSerial lnk(2);

// Protocol
#define SOF_CMD  0xAA   // start-of-frame for commands
#define SOF_RESP 0xBB   // start-of-frame for responses
#define EOF_BYTE 0x55

#define CMD_WIFI_SCAN    0x01
#define CMD_BLE_SCAN     0x02
#define CMD_NRF_SCAN     0x03
#define CMD_ALERT_SSID   0x04
#define CMD_ALERT_BLE    0x05
#define CMD_ALERT_PORTAL 0x06
#define CMD_STOP_ALERT   0x07

#define RESP_WIFI   0xA1
#define RESP_BLE    0xA2
#define RESP_NRF    0xA3
#define RESP_OK     0xA4

// ── Received data structures ──────────────────────────────────
#define MAX_WIFI 30
#define MAX_BLE  25
#define SPEC_R   2
#define SPEC_CH  126

struct RxWifi { char ssid[32]; int8_t rssi; uint8_t ch; uint8_t enc; };
struct RxBle  { char name[20]; char addr[18]; int8_t rssi; };

RxWifi wRes[MAX_WIFI]; int wCount=0;
RxBle  bRes[MAX_BLE];  int bCount=0;
uint8_t nrfSpec[SPEC_R][SPEC_CH];
bool nrfReady=false;

// ── App states ────────────────────────────────────────────────
enum St {
  ST_SPLASH,
  ST_MENU,
  // Per-feature inactive status screen
  ST_WIFI_STATUS, ST_BLE_STATUS, ST_NRF_STATUS, ST_ALERT_STATUS,
  // Scanning/waiting
  ST_WIFI_SCAN,   ST_BLE_SCAN,   ST_NRF_SCAN,
  // Results
  ST_WIFI_RES,    ST_BLE_RES,    ST_NRF_RES,
  // Action overlay on results
  ST_ACTION,
  // Alert sub-flow
  ST_ALERT_MENU, ST_ALERT_PRESET, ST_ALERT_COMPOSE,
  ST_ALERT_METHOD, ST_ALERT_ACTIVE
};
St st = ST_SPLASH;
St prevResultSt = ST_WIFI_RES; // which scan's result we came from

// ── Menu ─────────────────────────────────────────────────────
const char* menuItems[]={"  WIFI  SCAN","  BLE   SCAN","  NRF   SCAN","  ALERT MODE"};
int mCur=0;

// ── Result navigation ─────────────────────────────────────────
int resCur=0, resScroll=0;
#define RES_ROWS 4

// ── Action overlay ────────────────────────────────────────────
const char* actLabels[]={"SCAN  AGAIN","DEACTIVATE ","BACK TO MENU"};
int actCur=0;

// ── Alert ─────────────────────────────────────────────────────
const char* presets[]={
  "!! EMERGENCY !!","WiFi Down-Use LTE","Meeting in 5 min!",
  "Please Evacuate!","Network Outage","Security Alert!","Custom Message..."
};
#define PRE_COUNT 7
#define PRE_CUSTOM 6
int preCur=0, preScroll=0;

const char CSET[]=" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!?.,:-@#";
#define CSET_LEN 46
#define CSET_DEL 46
char composeBuf[33]={};
int  composeLen=0, composeIdx=0;

char alertMsg[33]={};
int  methodCur=0;
bool alertActive=false;

// ── Scan wait timer ───────────────────────────────────────────
uint32_t scanStart=0;
#define SCAN_TIMEOUT 20000

// ═══════════════════════════════════════════════════════════════
//  UART  ── SEND COMMAND
// ═══════════════════════════════════════════════════════════════
void sendCmd(uint8_t cmd, const uint8_t* payload=nullptr, uint8_t plen=0){
  uint8_t chk=cmd;
  for(uint8_t i=0;i<plen;i++) chk^=payload[i];
  lnk.write(SOF_CMD); lnk.write(cmd); lnk.write(plen);
  if(plen&&payload) lnk.write(payload,plen);
  lnk.write(chk); lnk.write(EOF_BYTE);
  lnk.flush();
}

// ═══════════════════════════════════════════════════════════════
//  UART  ── RECEIVE RESPONSE  (non-blocking state machine)
// ═══════════════════════════════════════════════════════════════
enum RxSt { RX_IDLE, RX_TYPE, RX_LEN_HI, RX_LEN_LO, RX_DATA, RX_CHK, RX_EOF };
RxSt rxSt=RX_IDLE;
uint8_t  rxType=0;
uint16_t rxExpect=0, rxIdx=0;
uint8_t  rxBuf[1100];
uint8_t  rxChk=0;

// Returns true when a complete valid response has been parsed into rxBuf/rxType
bool pollUART(){
  while(lnk.available()){
    uint8_t b=lnk.read();
    switch(rxSt){
      case RX_IDLE:    if(b==SOF_RESP){rxSt=RX_TYPE;rxChk=0;} break;
      case RX_TYPE:    rxType=b; rxChk^=b; rxSt=RX_LEN_HI; break;
      case RX_LEN_HI:  rxExpect=(uint16_t)b<<8; rxChk^=b; rxSt=RX_LEN_LO; break;
      case RX_LEN_LO:  rxExpect|=b; rxChk^=b; rxIdx=0; rxSt=(rxExpect>0?RX_DATA:RX_CHK); break;
      case RX_DATA:
        if(rxIdx<sizeof(rxBuf)) rxBuf[rxIdx]=b;
        rxChk^=b; rxIdx++;
        if(rxIdx>=rxExpect) rxSt=RX_CHK;
        break;
      case RX_CHK:
        rxSt=RX_EOF;
        if(b!=rxChk){ rxSt=RX_IDLE; Serial.println("[RX] CHK fail"); }
        break;
      case RX_EOF:
        rxSt=RX_IDLE;
        if(b==EOF_BYTE) return true;
        break;
    }
  }
  return false;
}

// Parse whichever response just arrived
void parseResponse(){
  switch(rxType){

    case RESP_WIFI:{
      wCount=min((int)rxBuf[0], MAX_WIFI);
      for(int i=0;i<wCount;i++){
        uint8_t* p=rxBuf+1+i*35;
        memcpy(wRes[i].ssid,p,32); wRes[i].ssid[31]='\0';
        wRes[i].rssi=(int8_t)p[32];
        wRes[i].ch=p[33]; wRes[i].enc=p[34];
      }
      resCur=0; resScroll=0;
      st=ST_WIFI_RES;
      break;
    }

    case RESP_BLE:{
      bCount=min((int)rxBuf[0], MAX_BLE);
      for(int i=0;i<bCount;i++){
        uint8_t* p=rxBuf+1+i*39;
        memcpy(bRes[i].name,p,20); bRes[i].name[19]='\0';
        memcpy(bRes[i].addr,p+20,18); bRes[i].addr[17]='\0';
        bRes[i].rssi=(int8_t)p[38];
      }
      resCur=0; resScroll=0;
      st=ST_BLE_RES;
      break;
    }

    case RESP_NRF:{
      for(int r=0;r<SPEC_R;r++)
        for(int c=0;c<SPEC_CH;c++)
          nrfSpec[r][c]=rxBuf[r*SPEC_CH+c];
      nrfReady=true;
      resCur=0; resScroll=0;
      st=ST_NRF_RES;
      break;
    }

    case RESP_OK:
      alertActive=true;
      st=ST_ALERT_ACTIVE;
      break;
  }
}

// ═══════════════════════════════════════════════════════════════
//  GLITCH ANIMATION  ── NUKRAX INTRO
// ═══════════════════════════════════════════════════════════════

void gNoise(uint8_t n){
  for(uint8_t i=0;i<n;i++){
    int x=random(128),y=random(64),w=random(4,70),h=random(1,4);
    oled.fillRect(x,y,min(w,128-x),h,random(2)?SSD1306_WHITE:SSD1306_BLACK);
  }
}

void gPixels(uint8_t n){
  for(uint8_t i=0;i<n;i++)
    oled.drawPixel(random(128),random(64),SSD1306_WHITE);
}

void gScanLine(){
  int y=random(64);
  oled.drawFastHLine(0,y,128,random(2)?SSD1306_WHITE:SSD1306_BLACK);
}

void gVertBar(){
  int x=random(128),w=random(1,5);
  oled.fillRect(x,0,w,64,random(2)?SSD1306_WHITE:SSD1306_BLACK);
}

void drawNukrax(int ox, int oy, bool inv=false){
  oled.setTextSize(3);
  oled.setTextColor(inv?SSD1306_BLACK:SSD1306_WHITE);
  oled.setCursor(8+ox, 14+oy);
  oled.print("NUKRAX");
}

void drawCornerMarkers(){
  // High-tech corner brackets
  oled.drawFastHLine(0,0,6,SSD1306_WHITE);  oled.drawFastVLine(0,0,5,SSD1306_WHITE);
  oled.drawFastHLine(122,0,6,SSD1306_WHITE);oled.drawFastVLine(127,0,5,SSD1306_WHITE);
  oled.drawFastHLine(0,63,6,SSD1306_WHITE); oled.drawFastVLine(0,59,5,SSD1306_WHITE);
  oled.drawFastHLine(122,63,6,SSD1306_WHITE);oled.drawFastVLine(127,59,5,SSD1306_WHITE);
}

void drawSubtitle(bool visible){
  if(!visible) return;
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(14,46); oled.print("RF INTELLIGENCE SYS");
  oled.setCursor(34,56); oled.print("v3.0  [ARMED]");
}

void nukraxIntro(){
  // ── Phase 1: Static noise burst (0–550ms) ──────────────────
  uint32_t t0=millis();
  while(millis()-t0<550){
    oled.clearDisplay();
    gNoise(22);
    oled.display(); delay(28);
  }

  // ── Phase 2: Text emerges through glitch (550–1400ms) ──────
  while(millis()-t0<1400){
    oled.clearDisplay();
    gNoise(10);
    // Ghost echo of text (offset copy)
    drawNukrax(random(-6,6), random(-3,3), false);
    // Second copy for echo
    oled.setTextSize(3); oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(8+random(-3,3), 14+random(-2,2));
    oled.print("NUKRAX");
    // Main text
    drawNukrax(random(-4,4), random(-2,2), false);
    // Occasional horizontal tear
    if(random(3)==0) gScanLine();
    if(random(5)==0){
      oled.invertDisplay(true); oled.display(); delay(35);
      oled.invertDisplay(false);
    }
    oled.display(); delay(40);
  }

  // ── Phase 3: Hold with active glitch effects (1400–3500ms) ─
  uint32_t p3=millis();
  while(millis()-t0<3500){
    oled.clearDisplay();
    drawCornerMarkers();

    // Main text mostly stable
    int sx=(random(6)==0)?random(-4,4):0;
    int sy=(random(8)==0)?random(-2,2):0;
    drawNukrax(sx, sy);

    // Shadow/echo effect offset by 2px
    oled.setTextSize(3); oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(10+sx, 16+sy);
    oled.print("NUKRAX");

    // Re-draw main over shadow
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(8+sx, 14+sy);
    oled.print("NUKRAX");

    // Decorative lines
    oled.drawFastHLine(0,12,128,SSD1306_WHITE);
    oled.drawFastHLine(0,43,128,SSD1306_WHITE);

    // Subtitle flicker
    drawSubtitle(random(4)!=0);

    // Noise effects
    gPixels(4);
    if(random(4)==0) gScanLine();
    if(random(8)==0) gVertBar();

    // Blinking cursor after NUKRAX
    uint32_t elapsed=millis()-p3;
    if((elapsed/350)%2==0){
      oled.fillRect(116,14,3,22,SSD1306_WHITE);
    }

    // Brief flash
    if(random(20)==0){
      oled.invertDisplay(true); oled.display(); delay(30);
      oled.invertDisplay(false);
    }

    oled.display(); delay(50);
  }

  // ── Phase 4: Dissolve out (3500–4100ms) ─────────────────────
  for(uint8_t frame=0; frame<18; frame++){
    oled.clearDisplay();
    uint8_t barCount=(uint8_t)(5+frame*2);
    gNoise(barCount);
    // Text shifts increasingly
    if(frame<10) drawNukrax(random(-frame*2,frame*2), random(-frame,frame));
    oled.display(); delay(30);
  }

  // Final wipe to black
  for(int x=0;x<128;x+=8){
    oled.fillRect(x,0,8,64,SSD1306_BLACK);
    oled.display(); delay(12);
  }
  delay(120);
}

// ═══════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════
uint8_t toBars(int rssi){
  if(rssi>=-50)return 4; if(rssi>=-65)return 3;
  if(rssi>=-75)return 2; if(rssi>=-85)return 1; return 0;
}
void sigBars(int x,int y,uint8_t b,bool inv){
  uint16_t c=inv?SSD1306_BLACK:SSD1306_WHITE;
  for(uint8_t i=0;i<4;i++){
    int h=2+i*2,bx=x+i*4,by=y+8-h;
    if(i<b) oled.fillRect(bx,by,3,h,c);
    else    oled.drawRect(bx,by,3,h,c);
  }
}
void hdr(const char* t, bool dot=false){
  oled.fillRect(0,0,128,12,SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK); oled.setTextSize(1);
  int x=(128-(int)strlen(t)*6)/2;
  oled.setCursor(max(0,x),2); oled.print(t);
  if(dot){ oled.fillCircle(122,6,3,SSD1306_BLACK); } // live dot
  oled.setTextColor(SSD1306_WHITE);
}
void statusBadge(bool active){
  if(active){
    oled.fillRect(76,2,48,8,SSD1306_BLACK);
    oled.drawRect(76,2,48,8,SSD1306_BLACK);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(78,3); oled.print("[ACTIVE]");
  } else {
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(70,3); oled.print("[INACTIVE]");
  }
  oled.setTextColor(SSD1306_WHITE);
}
void bottomHint(const char* t){
  oled.drawFastHLine(0,55,128,SSD1306_WHITE);
  oled.setCursor(2,57); oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1); oled.print(t);
}
void spinnerFrame(const char* title, const char* line2=""){
  static uint8_t f=0; const char* sp="|/-\\";
  oled.clearDisplay(); hdr(title);
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  // Radar circle
  float a=(f*15.0f)*3.14159f/180.0f;
  oled.drawCircle(64,38,16,SSD1306_WHITE);
  oled.drawLine(64,38,64+(int)(16*cos(a)),38+(int)(16*sin(a)),SSD1306_WHITE);
  // Corner scan brackets
  oled.drawFastHLine(10,20,12,SSD1306_WHITE); oled.drawFastVLine(10,20,8,SSD1306_WHITE);
  oled.drawFastHLine(106,20,12,SSD1306_WHITE);oled.drawFastVLine(117,20,8,SSD1306_WHITE);
  oled.drawFastHLine(10,56,12,SSD1306_WHITE); oled.drawFastVLine(10,48,8,SSD1306_WHITE);
  oled.drawFastHLine(106,56,12,SSD1306_WHITE);oled.drawFastVLine(117,48,8,SSD1306_WHITE);
  oled.setCursor(45,13); oled.print("SCANNING"); oled.print(sp[f%4]);
  if(strlen(line2)>0){oled.setCursor(2,57);oled.print(line2);}
  f++; oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN: MAIN MENU
// ═══════════════════════════════════════════════════════════════
void drawMenu(){
  oled.clearDisplay();
  // Title
  oled.fillRect(0,0,128,11,SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK); oled.setTextSize(1);
  oled.setCursor(26,2); oled.print("NUKRAX  SYSTEM");
  if(alertActive){oled.fillCircle(122,5,3,SSD1306_BLACK);}

  oled.setTextColor(SSD1306_WHITE);
  for(int i=0;i<4;i++){
    int y=12+i*13; bool sel=(i==mCur);
    if(sel) oled.fillRect(0,y,128,12,SSD1306_WHITE);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    oled.setCursor(4,y+2);
    if(sel) oled.print("> "); else oled.print("  ");
    oled.print(menuItems[i]);
    // Alert active indicator
    if(i==3&&alertActive){oled.setCursor(115,y+2);oled.print("*");}
  }

  oled.drawFastHLine(0,63,128,SSD1306_WHITE);
  oled.setTextColor(SSD1306_WHITE); oled.setCursor(2,63); // below screen, won't show
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN: FEATURE STATUS (inactive entry screen)
// ═══════════════════════════════════════════════════════════════
void drawStatusScreen(const char* name, bool active){
  oled.clearDisplay();
  hdr(name); statusBadge(active);

  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.drawFastHLine(0,13,128,SSD1306_WHITE);

  oled.setCursor(8,20);
  oled.print("Status: ");
  if(active){ oled.print("RUNNING"); }
  else       { oled.print("STANDBY"); }

  oled.setCursor(8,32);
  if(!active){ oled.print("Press OK to ACTIVATE"); }
  else        { oled.print("System is active"); }

  oled.setCursor(8,44);
  oled.print("Hold OK: back to menu");

  bottomHint(active?"OK:action  U/D:scroll":"OK:activate");
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN: WIFI RESULTS
// ═══════════════════════════════════════════════════════════════
void drawWifiRes(){
  oled.clearDisplay();
  char t[22]; snprintf(t,sizeof(t),"WIFI  %d FOUND",wCount);
  hdr(t);
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);

  if(!wCount){
    oled.setCursor(14,28); oled.print("No networks found");
    bottomHint("OK:options"); oled.display(); return;
  }
  for(int i=0;i<RES_ROWS;i++){
    int idx=resScroll+i; if(idx>=wCount) break;
    int y=13+i*11; bool sel=(idx==resCur);
    if(sel) oled.fillRect(0,y,128,10,SSD1306_WHITE);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    oled.setCursor(2,y+1);
    oled.print(sel?">":"");
    oled.print(wRes[idx].enc?"*":" ");
    // SSID truncated to 12 chars
    char ss[13]={}; strncpy(ss,strlen(wRes[idx].ssid)?wRes[idx].ssid:"<hidden>",12);
    oled.print(ss);
    // Channel
    char ch[5]; snprintf(ch,sizeof(ch),"C%02d",wRes[idx].ch);
    oled.setCursor(96,y+1); oled.print(ch);
    // Signal bars
    sigBars(110,y+1,toBars(wRes[idx].rssi),sel);
    oled.setTextColor(SSD1306_WHITE);
  }
  if(resScroll>0)           oled.fillTriangle(122,13,126,13,124,10,SSD1306_WHITE);
  if(resScroll+RES_ROWS<wCount) oled.fillTriangle(122,62,126,62,124,65,SSD1306_WHITE);
  bottomHint("U/D:scroll  OK:options");
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN: BLE RESULTS
// ═══════════════════════════════════════════════════════════════
void drawBleRes(){
  oled.clearDisplay();
  char t[22]; snprintf(t,sizeof(t),"BLE   %d FOUND",bCount);
  hdr(t);
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);

  if(!bCount){
    oled.setCursor(14,28); oled.print("No BLE devices found");
    bottomHint("OK:options"); oled.display(); return;
  }
  for(int i=0;i<RES_ROWS;i++){
    int idx=resScroll+i; if(idx>=bCount) break;
    int y=13+i*11; bool sel=(idx==resCur);
    if(sel) oled.fillRect(0,y,128,10,SSD1306_WHITE);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    oled.setCursor(2,y+1);
    oled.print(sel?">":"");
    char nm[15]={}; strncpy(nm,bRes[idx].name,14);
    oled.print(nm);
    char rs[6]; snprintf(rs,sizeof(rs),"%ddBm",bRes[idx].rssi);
    oled.setCursor(92,y+1); oled.print(rs);
    oled.setTextColor(SSD1306_WHITE);
  }
  if(resScroll>0)           oled.fillTriangle(122,13,126,13,124,10,SSD1306_WHITE);
  if(resScroll+RES_ROWS<bCount) oled.fillTriangle(122,62,126,62,124,65,SSD1306_WHITE);
  bottomHint("U/D:scroll  OK:options");
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN: NRF SPECTRUM
// ═══════════════════════════════════════════════════════════════
int nrfPan=0;
void drawNrfRes(){
  oled.clearDisplay(); hdr("2.4GHz SPECTRUM");
  uint8_t merged[SPEC_CH]={};
  uint8_t peak=1;
  for(int c=0;c<SPEC_CH;c++){
    for(int r=0;r<SPEC_R;r++) if(nrfSpec[r][c]>merged[c]) merged[c]=nrfSpec[r][c];
    if(merged[c]>peak) peak=merged[c];
  }
  int vEnd=min(SPEC_CH,nrfPan+SCR_W);
  for(int c=nrfPan;c<vEnd;c++){
    uint8_t h=(uint16_t)merged[c]*49/peak;
    if(h) oled.drawLine(c-nrfPan,62,c-nrfPan,62-h,SSD1306_WHITE);
  }
  // WiFi markers
  const int mk[]={12,37,62}; const char* ml[]={"W1","W6","W11"};
  for(int i=0;i<3;i++){
    int px=mk[i]-nrfPan;
    if(px>=0&&px<SCR_W){
      oled.drawLine(px,13,px,55,SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
      oled.setCursor(max(0,px-5),14); oled.print(ml[i]);
      oled.setTextColor(SSD1306_WHITE);
    }
  }
  bottomHint("U/D:pan  OK:options");
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN: ACTION OVERLAY (on top of results)
// ═══════════════════════════════════════════════════════════════
void drawActionOverlay(){
  // Draw a semi-visible results screen behind
  // Then draw the overlay box
  oled.fillRect(4,30,120,34,SSD1306_BLACK);
  oled.drawRect(4,30,120,34,SSD1306_WHITE);
  oled.drawFastHLine(4,39,120,SSD1306_WHITE);

  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(30,32); oled.print("-- OPTIONS --");

  for(int i=0;i<3;i++){
    int y=41+i*7; bool sel=(i==actCur);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    if(sel) oled.fillRect(6,y-1,116,8,SSD1306_WHITE);
    oled.setCursor(12,y);
    oled.print(sel?"> ":"  ");
    oled.print(actLabels[i]);
    oled.setTextColor(SSD1306_WHITE);
  }
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  ALERT SCREENS
// ═══════════════════════════════════════════════════════════════
void drawAlertStatus(){
  oled.clearDisplay(); hdr("ALERT  MODE", alertActive);
  statusBadge(alertActive);
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.drawFastHLine(0,13,128,SSD1306_WHITE);

  if(alertActive){
    oled.setCursor(2,18); oled.print("Broadcasting:");
    oled.setCursor(2,28); oled.print(alertMsg);
    oled.setCursor(2,40);
    const char* mnames[]={"","WiFi SSID","BLE Beacon","Portal"};
    oled.print("Via: "); oled.print(mnames[min(methodCur+1,3)]);
  } else {
    oled.setCursor(4,22); oled.print("Send alerts to nearby");
    oled.setCursor(4,32); oled.print("mobile devices.");
    oled.setCursor(4,44); oled.print("Press OK to configure");
  }
  bottomHint(alertActive?"OK:stop/change":"OK:start");
  oled.display();
}

void drawAlertMenu(){
  oled.clearDisplay(); hdr("ALERT  SETUP");
  const char* opts[]={"Preset Message","Custom Message","Choose Method","Send Now"};
  for(int i=0;i<4;i++){
    int y=13+i*12; bool sel=(i==actCur);
    if(sel) oled.fillRect(0,y,128,11,SSD1306_WHITE);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    oled.setCursor(6,y+2); oled.print(sel?"> ":"  "); oled.print(opts[i]);
  }
  bottomHint("U/D:pick  OK:select");
  oled.setTextColor(SSD1306_WHITE); oled.display();
}

void drawPresets(){
  oled.clearDisplay(); hdr("SELECT  MESSAGE");
  for(int i=0;i<3;i++){
    int idx=preScroll+i; if(idx>=PRE_COUNT) break;
    int y=13+i*17; bool sel=(idx==preCur);
    if(sel) oled.fillRect(0,y,128,16,SSD1306_WHITE);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    oled.setCursor(4,y+4); oled.print(sel?"> ":"  ");
    char tmp[18]={}; strncpy(tmp,presets[idx],17); oled.print(tmp);
    oled.setTextColor(SSD1306_WHITE);
  }
  if(preScroll>0)             oled.fillTriangle(122,13,126,13,124,10,SSD1306_WHITE);
  if(preScroll+3<PRE_COUNT)   oled.fillTriangle(122,62,126,62,124,65,SSD1306_WHITE);
  bottomHint("U/D:pick  OK:select");
  oled.display();
}

void drawCompose(){
  oled.clearDisplay(); hdr("CUSTOM  MSG");
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);

  // Message preview with cursor
  char prev[22]={}; int s=max(0,composeLen-20);
  strncpy(prev,composeBuf+s,20);
  int pl=strlen(prev); prev[pl]='_'; prev[pl+1]='\0';
  oled.setCursor(2,14); oled.print(prev);

  // Current char (large)
  oled.setTextSize(2);
  char cc=CSET[min(composeIdx,(int)CSET_LEN-1)];
  if(composeIdx==CSET_DEL){ oled.setCursor(54,28); oled.print("[<]"); }
  else { oled.setCursor(58,28); oled.print(cc); }
  oled.setTextSize(1);

  // Prev/next hint
  char cp=CSET[((composeIdx-1+CSET_LEN+1)%(CSET_LEN+1))%CSET_LEN];
  char cn=CSET[(composeIdx+1)%(CSET_LEN)];
  oled.setCursor(22,44); oled.print(cp);
  oled.print(" < "); oled.print(cc); oled.print(" > "); oled.print(cn);

  oled.setCursor(2,55); oled.print("OK:add  HOLD OK:done  U:del");
  oled.display();
}

void drawMethodSelect(){
  oled.clearDisplay(); hdr("BROADCAST  METHOD");
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(2,14); oled.print("Msg:"); oled.print(alertMsg);

  const char* methods[]={"WiFi SSID Blast","BLE Beacon","Captive Portal"};
  const char* desc[]={"Visible in WiFi list","Visible in BT scan","Auto-opens browser"};
  for(int i=0;i<3;i++){
    int y=24+i*13; bool sel=(i==methodCur);
    if(sel) oled.fillRect(0,y,128,12,SSD1306_WHITE);
    oled.setTextColor(sel?SSD1306_BLACK:SSD1306_WHITE);
    oled.setCursor(4,y+2); oled.print(sel?">":""); oled.print(methods[i]);
    oled.setTextColor(SSD1306_WHITE);
  }
  bottomHint("U/D:pick  OK:launch");
  oled.display();
}

void drawAlertActive(){
  static uint32_t lt=0; static bool fl=false;
  if(millis()-lt>500){ fl=!fl; lt=millis(); }
  oled.clearDisplay();
  if(fl) oled.fillRect(0,0,128,12,SSD1306_WHITE);
  else   oled.drawRect(0,0,128,12,SSD1306_WHITE);
  oled.setTextColor(fl?SSD1306_BLACK:SSD1306_WHITE);
  oled.setCursor(8,2); oled.print("!! BROADCASTING !!");
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);

  const char* mnames[]={"WiFi SSID","BLE Beacon","Captive Portal"};
  oled.setCursor(2,16); oled.print("Via: "); oled.print(mnames[min(methodCur,2)]);
  oled.setCursor(2,26); oled.print("Msg: ");
  char tmp[17]={}; strncpy(tmp,alertMsg,16); oled.print(tmp);
  if(methodCur==2){ oled.setCursor(2,38); oled.print("Connect: RF-ALERT WiFi"); }
  bottomHint("OK:stop broadcast");
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  SCAN LAUNCHERS
// ═══════════════════════════════════════════════════════════════
void launchWifi(){
  sendCmd(CMD_WIFI_SCAN);
  scanStart=millis(); wCount=0; st=ST_WIFI_SCAN;
}
void launchBle(){
  sendCmd(CMD_BLE_SCAN);
  scanStart=millis(); bCount=0; st=ST_BLE_SCAN;
}
void launchNrf(){
  uint8_t p=10; sendCmd(CMD_NRF_SCAN,&p,1);
  scanStart=millis(); nrfReady=false; nrfPan=0; st=ST_NRF_SCAN;
}
void launchAlert(){
  uint8_t buf[33];
  uint8_t cmd=CMD_ALERT_SSID;
  if(methodCur==1) cmd=CMD_ALERT_BLE;
  if(methodCur==2) cmd=CMD_ALERT_PORTAL;
  uint8_t len=strlen(alertMsg);
  memcpy(buf,alertMsg,len);
  sendCmd(cmd,buf,len);
  st=ST_ALERT_ACTIVE;
}
void stopAlert(){
  sendCmd(CMD_STOP_ALERT);
  alertActive=false; st=ST_ALERT_STATUS;
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);
  lnk.begin(LINK_BAUD,SERIAL_8N1,LINK_RX,LINK_TX);

  Wire.begin(OLED_SDA,OLED_SCL);
  if(!oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){
    Serial.println("[ERR] OLED not found"); while(1) delay(500);
  }
  oled.setTextSize(1); oled.cp437(true);

  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DN, INPUT_PULLUP);
  pinMode(PIN_MID,INPUT_PULLUP);

  nukraxIntro();
  st=ST_MENU; drawMenu();
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop(){
  bool up=tap(bU), dn=tap(bD), mid=tap(bM);
  bool midH=hold(bM), upH=hold(bU);

  // Always poll UART for incoming data from ESP1
  if(pollUART()) parseResponse();

  switch(st){

  // ── Main menu ──────────────────────────────────────────────
  case ST_MENU:
    if(up&&mCur>0){ mCur--; drawMenu(); }
    if(dn&&mCur<3){ mCur++; drawMenu(); }
    if(mid){
      switch(mCur){
        case 0: st=ST_WIFI_STATUS;  drawStatusScreen("WIFI SCAN",false); break;
        case 1: st=ST_BLE_STATUS;   drawStatusScreen("BLE  SCAN",false); break;
        case 2: st=ST_NRF_STATUS;   drawStatusScreen("NRF  SCAN",false); break;
        case 3: st=ST_ALERT_STATUS; drawAlertStatus(); break;
      }
    }
    if(alertActive) drawAlertActive(); // keep active screen refreshed
    break;

  // ── Feature status screens ─────────────────────────────────
  case ST_WIFI_STATUS:
    if(mid){ launchWifi(); }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;
  case ST_BLE_STATUS:
    if(mid){ launchBle(); }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;
  case ST_NRF_STATUS:
    if(mid){ launchNrf(); }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;
  case ST_ALERT_STATUS:
    if(alertActive&&mid){ stopAlert(); }
    else if(!alertActive&&mid){ actCur=0; st=ST_ALERT_MENU; drawAlertMenu(); }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;

  // ── Scanning / waiting for ESP1 ───────────────────────────
  case ST_WIFI_SCAN:
    spinnerFrame("WIFI SCANNING","ESP1 scanning...");
    if(millis()-scanStart>SCAN_TIMEOUT){
      oled.clearDisplay(); hdr("WIFI TIMEOUT");
      oled.setCursor(4,24); oled.print("No response from ESP1");
      oled.setCursor(4,36); oled.print("Check UART wiring");
      bottomHint("OK:back");
      oled.display();
      while(!tap(bM)) delay(20);
      st=ST_WIFI_STATUS; drawStatusScreen("WIFI SCAN",false);
    }
    delay(80);
    break;

  case ST_BLE_SCAN:
    spinnerFrame("BLE SCANNING","ESP1 scanning 6s...");
    if(millis()-scanStart>SCAN_TIMEOUT){
      oled.clearDisplay(); hdr("BLE TIMEOUT");
      oled.setCursor(4,28); oled.print("No response from ESP1");
      bottomHint("OK:back"); oled.display();
      while(!tap(bM)) delay(20);
      st=ST_BLE_STATUS; drawStatusScreen("BLE  SCAN",false);
    }
    delay(80);
    break;

  case ST_NRF_SCAN:
    spinnerFrame("NRF24 SCANNING","2.4GHz sweep...");
    if(millis()-scanStart>SCAN_TIMEOUT){
      oled.clearDisplay(); hdr("NRF TIMEOUT");
      oled.setCursor(4,28); oled.print("No response from ESP1");
      bottomHint("OK:back"); oled.display();
      while(!tap(bM)) delay(20);
      st=ST_NRF_STATUS; drawStatusScreen("NRF  SCAN",false);
    }
    delay(80);
    break;

  // ── Results screens ────────────────────────────────────────
  case ST_WIFI_RES:
    if(up&&resCur>0){ resCur--; if(resCur<resScroll)resScroll=resCur; drawWifiRes(); }
    if(dn&&resCur<wCount-1){ resCur++; if(resCur>=resScroll+RES_ROWS)resScroll=resCur-RES_ROWS+1; drawWifiRes(); }
    if(mid){ actCur=0; prevResultSt=ST_WIFI_RES; drawWifiRes(); drawActionOverlay(); st=ST_ACTION; }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;

  case ST_BLE_RES:
    if(up&&resCur>0){ resCur--; if(resCur<resScroll)resScroll=resCur; drawBleRes(); }
    if(dn&&resCur<bCount-1){ resCur++; if(resCur>=resScroll+RES_ROWS)resScroll=resCur-RES_ROWS+1; drawBleRes(); }
    if(mid){ actCur=0; prevResultSt=ST_BLE_RES; drawBleRes(); drawActionOverlay(); st=ST_ACTION; }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;

  case ST_NRF_RES:
    if(up&&nrfPan>0){ nrfPan=max(0,nrfPan-8); drawNrfRes(); }
    if(dn&&nrfPan<SPEC_CH-SCR_W){ nrfPan=min(SPEC_CH-SCR_W,nrfPan+8); drawNrfRes(); }
    if(mid){ actCur=0; prevResultSt=ST_NRF_RES; drawNrfRes(); drawActionOverlay(); st=ST_ACTION; }
    if(midH){ st=ST_MENU; drawMenu(); }
    break;

  // ── Action overlay ─────────────────────────────────────────
  case ST_ACTION:
    if(up&&actCur>0){ actCur--; if(prevResultSt==ST_WIFI_RES)drawWifiRes(); else if(prevResultSt==ST_BLE_RES)drawBleRes(); else drawNrfRes(); drawActionOverlay(); }
    if(dn&&actCur<2){ actCur++; if(prevResultSt==ST_WIFI_RES)drawWifiRes(); else if(prevResultSt==ST_BLE_RES)drawBleRes(); else drawNrfRes(); drawActionOverlay(); }
    if(mid){
      switch(actCur){
        case 0: // Scan again
          if(prevResultSt==ST_WIFI_RES) launchWifi();
          else if(prevResultSt==ST_BLE_RES) launchBle();
          else launchNrf();
          break;
        case 1: // Deactivate → back to status
          if(prevResultSt==ST_WIFI_RES){ st=ST_WIFI_STATUS; drawStatusScreen("WIFI SCAN",false); }
          else if(prevResultSt==ST_BLE_RES){ st=ST_BLE_STATUS; drawStatusScreen("BLE  SCAN",false); }
          else{ st=ST_NRF_STATUS; drawStatusScreen("NRF  SCAN",false); }
          break;
        case 2: // Back to menu
          st=ST_MENU; drawMenu();
          break;
      }
    }
    break;

  // ── Alert flow ─────────────────────────────────────────────
  case ST_ALERT_MENU:
    if(up&&actCur>0){ actCur--; drawAlertMenu(); }
    if(dn&&actCur<3){ actCur++; drawAlertMenu(); }
    if(mid){
      switch(actCur){
        case 0: preCur=0; preScroll=0; st=ST_ALERT_PRESET; drawPresets(); break;
        case 1: memset(composeBuf,0,sizeof(composeBuf)); composeLen=0; composeIdx=0; st=ST_ALERT_COMPOSE; drawCompose(); break;
        case 2: methodCur=0; st=ST_ALERT_METHOD; drawMethodSelect(); break;
        case 3: launchAlert(); break;
      }
    }
    if(midH){ st=ST_ALERT_STATUS; drawAlertStatus(); }
    break;

  case ST_ALERT_PRESET:
    if(up&&preCur>0){ preCur--; if(preCur<preScroll)preScroll=preCur; drawPresets(); }
    if(dn&&preCur<PRE_COUNT-1){ preCur++; if(preCur>=preScroll+3)preScroll=preCur-2; drawPresets(); }
    if(mid){
      if(preCur==PRE_CUSTOM){ memset(composeBuf,0,sizeof(composeBuf)); composeLen=0; composeIdx=0; st=ST_ALERT_COMPOSE; drawCompose(); }
      else{ strncpy(alertMsg,presets[preCur],32); methodCur=0; st=ST_ALERT_METHOD; drawMethodSelect(); }
    }
    if(midH){ st=ST_ALERT_MENU; drawAlertMenu(); }
    break;

  case ST_ALERT_COMPOSE:
    if(up){ composeIdx=(composeIdx+1)%(CSET_LEN+1); drawCompose(); }
    if(dn){ composeIdx=(composeIdx-1+CSET_LEN+1)%(CSET_LEN+1); drawCompose(); }
    if(upH&&composeLen>0){ composeLen--; composeBuf[composeLen]='\0'; drawCompose(); }
    if(mid){
      if(composeIdx==CSET_DEL){ if(composeLen>0){composeLen--;composeBuf[composeLen]='\0';} }
      else if(composeLen<32){ composeBuf[composeLen++]=CSET[composeIdx]; composeBuf[composeLen]='\0'; }
      drawCompose();
    }
    if(midH&&composeLen>0){ strncpy(alertMsg,composeBuf,32); methodCur=0; st=ST_ALERT_METHOD; drawMethodSelect(); }
    break;

  case ST_ALERT_METHOD:
    if(up&&methodCur>0){ methodCur--; drawMethodSelect(); }
    if(dn&&methodCur<2){ methodCur++; drawMethodSelect(); }
    if(mid){ launchAlert(); }
    if(midH){ st=ST_ALERT_MENU; drawAlertMenu(); }
    break;

  case ST_ALERT_ACTIVE:
    drawAlertActive();
    if(mid){ stopAlert(); st=ST_ALERT_STATUS; drawAlertStatus(); }
    break;

  default: break;
  }

  delay(10);
}
