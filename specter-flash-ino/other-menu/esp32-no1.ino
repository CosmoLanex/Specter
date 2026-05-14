/*
 * ════════════════════════════════════════════════════════════
 *  ESP1 — SCAN & ALERT WORKER  "NUKRAX" v3.0
 *  Does ALL scanning (WiFi, BLE, NRF24) and alert broadcasting
 *  Reports everything back to ESP2 via UART
 * ════════════════════════════════════════════════════════════
 *  NRF24 no1 (HSPI): SCK=14 MISO=12 MOSI=13 CS=15 CE=16
 *  NRF24 no2 (VSPI): SCK=18 MISO=19 MOSI=23 CS=21 CE=22
 *  UART to ESP2:      TX=GPIO17 → ESP2 RX=GPIO17
 *                     RX=GPIO32 ← ESP2 TX=GPIO32
 *                     Shared GND mandatory
 * ════════════════════════════════════════════════════════════
 */

#include <SPI.h>
#include <RF24.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>

// ── UART ─────────────────────────────────────────────────────
#define LINK_TX   17
#define LINK_RX   32
#define LINK_BAUD 115200
HardwareSerial lnk(2);

// Protocol (must match ESP2)
#define SOF_CMD  0xAA
#define SOF_RESP 0xBB
#define EOF_BYTE 0x55

#define CMD_WIFI_SCAN    0x01
#define CMD_BLE_SCAN     0x02
#define CMD_NRF_SCAN     0x03
#define CMD_ALERT_SSID   0x04
#define CMD_ALERT_BLE    0x05
#define CMD_ALERT_PORTAL 0x06
#define CMD_STOP_ALERT   0x07

#define RESP_WIFI  0xA1
#define RESP_BLE   0xA2
#define RESP_NRF   0xA3
#define RESP_OK    0xA4

// ── NRF24 modules ────────────────────────────────────────────
#define R0_SCK  14
#define R0_MISO 12
#define R0_MOSI 13
#define R0_CS   15
#define R0_CE   16

#define R1_SCK  18
#define R1_MISO 19
#define R1_MOSI 23
#define R1_CS   21
#define R1_CE   22

SPIClass hspi(HSPI), vspi(VSPI);
RF24 r0(R0_CE,R0_CS), r1(R1_CE,R1_CS);
bool r0ok=false, r1ok=false;

#define SPEC_R  2
#define SPEC_CH 126
uint8_t spectrum[SPEC_R][SPEC_CH];

// ── Alert state ──────────────────────────────────────────────
WebServer alertSrv(80);
DNSServer  dnsSrv;
bool portalUp=false, bleAdvert=false;

// ── BLE scan callback ─────────────────────────────────────────
#define MAX_BLE_RES 25
struct BleNode { char name[20]; char addr[18]; int8_t rssi; };
BleNode bleRes[MAX_BLE_RES];
int bleCount=0;

class BLECb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if(bleCount>=MAX_BLE_RES) return;
    String n=d.getName().c_str();
    strncpy(bleRes[bleCount].name, (n.length()>0?n.c_str():"<unnamed>"), 19);
    strncpy(bleRes[bleCount].addr, d.getAddress().toString().c_str(), 17);
    bleRes[bleCount].rssi=(int8_t)d.getRSSI();
    bleCount++;
  }
};

BLEScan*        pScan   = nullptr;
BLEAdvertising* pAdvert = nullptr;

void stopAlerts(); // forward declaration

// ═══════════════════════════════════════════════════════════════
//  UART  ── RECEIVE COMMAND (non-blocking)
// ═══════════════════════════════════════════════════════════════
enum RxSt { RX_IDLE, RX_CMD, RX_PLEN, RX_DATA, RX_CHK, RX_EOF };
RxSt  rxSt=RX_IDLE;
uint8_t rxCmd=0, rxPlen=0, rxIdx=0, rxChk=0;
uint8_t rxBuf[64];

// Returns true when a complete valid command is ready in rxCmd/rxBuf
bool pollCmd(){
  while(lnk.available()){
    uint8_t b=lnk.read();
    switch(rxSt){
      case RX_IDLE: if(b==SOF_CMD){rxSt=RX_CMD;rxChk=0;} break;
      case RX_CMD:  rxCmd=b; rxChk^=b; rxSt=RX_PLEN; break;
      case RX_PLEN: rxPlen=b; rxChk^=b; rxIdx=0; rxSt=(b>0?RX_DATA:RX_CHK); break;
      case RX_DATA:
        if(rxIdx<sizeof(rxBuf)) rxBuf[rxIdx]=b;
        rxChk^=b; rxIdx++;
        if(rxIdx>=rxPlen) rxSt=RX_CHK;
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

// ═══════════════════════════════════════════════════════════════
//  UART  ── SEND RESPONSE
// ═══════════════════════════════════════════════════════════════
void sendResp(uint8_t type, const uint8_t* payload, uint16_t plen){
  uint8_t chk=type;
  chk^=(uint8_t)(plen>>8); chk^=(uint8_t)(plen&0xFF);
  for(uint16_t i=0;i<plen;i++) chk^=payload[i];

  lnk.write(SOF_RESP);
  lnk.write(type);
  lnk.write((uint8_t)(plen>>8));
  lnk.write((uint8_t)(plen&0xFF));
  if(plen) lnk.write(payload,plen);
  lnk.write(chk);
  lnk.write(EOF_BYTE);
  lnk.flush();
  Serial.printf("[TX] RESP 0x%02X  %d bytes\n",type,plen);
}

void sendOk(){ sendResp(RESP_OK,nullptr,0); }

// ═══════════════════════════════════════════════════════════════
//  WIFI SCAN
// ═══════════════════════════════════════════════════════════════
/*
 *  WiFi network struct sent to ESP2 (35 bytes each):
 *    ssid[32]  rssi(1)  channel(1)  encrypted(1)
 */
void doWifiScan(){
  Serial.println("[WIFI] Scanning...");
  // Stop any AP alert first
  if(portalUp){ dnsSrv.stop(); alertSrv.stop(); portalUp=false; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA); WiFi.disconnect(true); delay(200);

  int n=WiFi.scanNetworks(false,true);
  if(n<0) n=0;
  n=min(n,30);
  Serial.printf("[WIFI] Found %d networks\n",n);

  // Build payload: count(1) + n×35 bytes
  uint16_t plen=1+(uint16_t)n*35;
  uint8_t* buf=(uint8_t*)malloc(plen);
  if(!buf){ Serial.println("[ERR] malloc fail"); return; }

  buf[0]=(uint8_t)n;
  for(int i=0;i<n;i++){
    uint8_t* p=buf+1+i*35;
    memset(p,0,35);
    String ss=WiFi.SSID(i);
    strncpy((char*)p, ss.c_str(), 31);
    p[32]=(uint8_t)(int8_t)WiFi.RSSI(i);
    p[33]=(uint8_t)WiFi.channel(i);
    p[34]=(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN)?1:0;
  }
  WiFi.scanDelete();
  sendResp(RESP_WIFI,buf,plen);
  free(buf);
}

// ═══════════════════════════════════════════════════════════════
//  BLE SCAN
// ═══════════════════════════════════════════════════════════════
/*
 *  BLE device struct sent to ESP2 (39 bytes each):
 *    name[20]  addr[18]  rssi(1)
 */
void doBleScan(){
  Serial.println("[BLE] Scanning 6s...");
  if(bleAdvert&&pAdvert){ pAdvert->stop(); bleAdvert=false; }

  bleCount=0;
  pScan->clearResults();
  pScan->start(6,false);   // 6 second blocking scan

  Serial.printf("[BLE] Found %d devices\n",bleCount);

  uint16_t plen=1+(uint16_t)bleCount*39;
  uint8_t* buf=(uint8_t*)malloc(plen);
  if(!buf){ Serial.println("[ERR] malloc fail"); return; }

  buf[0]=(uint8_t)bleCount;
  for(int i=0;i<bleCount;i++){
    uint8_t* p=buf+1+i*39;
    memset(p,0,39);
    strncpy((char*)p,    bleRes[i].name, 19);
    strncpy((char*)p+20, bleRes[i].addr, 17);
    p[38]=(uint8_t)(int8_t)bleRes[i].rssi;
  }
  sendResp(RESP_BLE,buf,plen);
  free(buf);
}

// ═══════════════════════════════════════════════════════════════
//  NRF24 SPECTRUM SCAN
// ═══════════════════════════════════════════════════════════════
/*
 *  Both radios scan simultaneously — staggered across the band:
 *    radio0: channels  0–62  (lower 2.4GHz)
 *    radio1: channels 63–125 (upper 2.4GHz)
 *  testRPD() = carrier > -64dBm detected during listen window.
 *  Payload sent: raw uint8_t[2][126] = 252 bytes
 */
void doNrfScan(uint8_t passes){
  memset(spectrum,0,sizeof(spectrum));
  Serial.printf("[NRF] Scanning %d passes × 63 steps × 2 radios\n",passes);

  for(uint8_t pass=0;pass<passes;pass++){
    for(uint8_t base=0;base<63;base++){
      uint8_t c0=base, c1=base+63;
      if(r0ok){ r0.setChannel(c0); r0.startListening(); }
      if(r1ok){ r1.setChannel(c1); r1.startListening(); }
      delayMicroseconds(280);
      if(r0ok){ if(r0.testRPD()) spectrum[0][c0]++; r0.stopListening(); }
      if(r1ok){ if(r1.testRPD()) spectrum[1][c1]++; r1.stopListening(); }
    }
    if(pass%5==4) delay(1);
  }

  // Log active channels
  for(int r=0;r<2;r++){
    Serial.printf("[NRF] Radio%d active: ",r);
    for(int c=0;c<SPEC_CH;c++) if(spectrum[r][c]) Serial.printf("ch%d(%d) ",c,spectrum[r][c]);
    Serial.println();
  }

  sendResp(RESP_NRF,(uint8_t*)spectrum,SPEC_R*SPEC_CH);
}

// ═══════════════════════════════════════════════════════════════
//  ALERT: WiFi SSID BLAST
// ═══════════════════════════════════════════════════════════════
// Alert message becomes the WiFi network name.
// Any phone scanning WiFi sees it instantly — no connection needed.
void startAlertSsid(const char* msg){
  stopAlerts();
  char ssid[33]={}; strncpy(ssid,msg,32);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid,nullptr,6,false,4);
  Serial.printf("[ALERT-SSID] Broadcasting: \"%s\"\n",ssid);
  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  ALERT: BLE BEACON
// ═══════════════════════════════════════════════════════════════
// ESP1 advertises with the alert as its BLE device name.
// Visible in BLE scanner apps (nRF Connect, LightBlue, etc.)
void startAlertBle(const char* msg){
  stopAlerts();
  BLEDevice::init(msg);
  pAdvert=BLEDevice::getAdvertising();
  pAdvert->setScanResponse(true);
  pAdvert->start();
  bleAdvert=true;
  Serial.printf("[ALERT-BLE] Beacon: \"%s\"\n",msg);

  // Re-init scanner for future BLE scans
  pScan=BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(),true);
  pScan->setActiveScan(true);
  pScan->setInterval(100); pScan->setWindow(99);

  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  ALERT: CAPTIVE PORTAL
// ═══════════════════════════════════════════════════════════════
// ESP1 creates WiFi AP "RF-ALERT".
// When a phone connects, the OS auto-detects the captive portal
// and opens a browser showing the full styled alert.
// Works on iOS, Android, Windows, macOS — zero apps needed.
char portalMsg[33]={};

const char PORTAL_HTML[] PROGMEM = R"(
<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="8">
<title>RF Scout Alert</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a18;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     display:flex;justify-content:center;align-items:center;min-height:100vh;padding:16px}
.c{background:#101028;border-radius:18px;padding:30px 22px;max-width:400px;width:100%;
   text-align:center;box-shadow:0 0 50px rgba(255,70,0,.4),0 0 100px rgba(255,70,0,.1)}
.ico{font-size:68px;margin-bottom:10px;animation:p 1.4s ease-in-out infinite}
@keyframes p{0%,100%{transform:scale(1)}50%{transform:scale(1.1)}}
h1{color:#ff4400;font-size:20px;font-weight:800;letter-spacing:3px;margin-bottom:18px;
   text-transform:uppercase}
.msg{font-size:28px;font-weight:800;color:#ffe000;background:#0d0d30;
     border:2px solid #ff4400;border-radius:12px;padding:16px 10px;margin:0 0 20px;
     word-break:break-word;line-height:1.3;text-shadow:0 0 20px rgba(255,224,0,.5)}
.live{display:inline-flex;align-items:center;gap:8px;background:#18183a;
      border-radius:999px;padding:7px 14px;font-size:13px;color:#999}
.dot{width:8px;height:8px;border-radius:50%;background:#ff4400;
     animation:b 1s step-end infinite}
@keyframes b{50%{opacity:0}}
.ft{margin-top:18px;font-size:11px;color:#333}
</style>
</head>
<body>
<div class="c">
  <div class="ico">&#9888;</div>
  <h1>NUKRAX&#160;ALERT</h1>
  <div class="msg">%MSG%</div>
  <div class="live"><span class="dot"></span>Live from ESP32 · NUKRAX sys</div>
  <div class="ft">Auto-refreshes every 8s &bull; Disconnect when done</div>
</div>
</body></html>
)";

void handleRoot(){
  String html=FPSTR(PORTAL_HTML);
  html.replace("%MSG%",String(portalMsg));
  alertSrv.send(200,"text/html",html);
}
void handleRedir(){
  alertSrv.sendHeader("Location","http://192.168.4.1",true);
  alertSrv.send(302,"text/plain","");
}

void startAlertPortal(const char* msg){
  stopAlerts();
  strncpy(portalMsg,msg,32);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("RF-ALERT",nullptr,1,false,8);
  delay(300);
  IPAddress ip(192,168,4,1);
  dnsSrv.setErrorReplyCode(DNSReplyCode::NoError);
  dnsSrv.start(53,"*",ip);
  alertSrv.on("/",HTTP_GET,handleRoot);
  alertSrv.on("/generate_204",HTTP_GET,handleRedir);       // Android
  alertSrv.on("/hotspot-detect.html",HTTP_GET,handleRoot); // iOS
  alertSrv.on("/ncsi.txt",HTTP_GET,handleRoot);            // Windows
  alertSrv.on("/connecttest.txt",HTTP_GET,handleRoot);     // Win10
  alertSrv.onNotFound(handleRedir);
  alertSrv.begin();
  portalUp=true;
  Serial.printf("[ALERT-PORTAL] \"%s\"  SSID=RF-ALERT\n",msg);
  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  STOP ALL ALERTS
// ═══════════════════════════════════════════════════════════════
void stopAlerts(){
  if(portalUp){ dnsSrv.stop(); alertSrv.stop(); portalUp=false; }
  if(bleAdvert&&pAdvert){ pAdvert->stop(); bleAdvert=false; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA); WiFi.disconnect(true);
  delay(100);
  Serial.println("[ALERT] Stopped");
}

// ═══════════════════════════════════════════════════════════════
//  RADIO INIT
// ═══════════════════════════════════════════════════════════════
bool initRadio(RF24& radio, SPIClass& spi, uint8_t sck,uint8_t miso,uint8_t mosi, uint8_t idx){
  spi.begin(sck,miso,mosi);
  delay(20);
  if(!radio.begin(&spi)){ Serial.printf("[ERR] Radio%d init failed\n",idx); return false; }
  radio.setPALevel(RF24_PA_MIN);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.setAutoAck(false);
  radio.setPayloadSize(1);
  radio.powerUp();
  radio.startListening();
  Serial.printf("[OK]  Radio%d ready  CE=%d CS=%d\n",idx,(idx==0?R0_CE:R1_CE),(idx==0?R0_CS:R1_CS));
  return true;
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup(){
  Serial.begin(115200); delay(400);
  Serial.println();
  Serial.println("╔═══════════════════════════════╗");
  Serial.println("║  NUKRAX  ESP1  Worker  v3.0   ║");
  Serial.println("╚═══════════════════════════════╝");

  // UART link
  lnk.begin(LINK_BAUD,SERIAL_8N1,LINK_RX,LINK_TX);
  Serial.printf("[UART] TX=GPIO%d  RX=GPIO%d  %dbaud\n",LINK_TX,LINK_RX,LINK_BAUD);

  // NRF24 radios
  r0ok=initRadio(r0,hspi,R0_SCK,R0_MISO,R0_MOSI,0);
  r1ok=initRadio(r1,vspi,R1_SCK,R1_MISO,R1_MOSI,1);
  Serial.printf("[NRF] %d/2 radios operational\n",(int)r0ok+(int)r1ok);

  // WiFi + BLE
  WiFi.mode(WIFI_STA); WiFi.disconnect(true);
  BLEDevice::init("ESP1-NukraxWorker");
  pScan=BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(),true);
  pScan->setActiveScan(true);
  pScan->setInterval(100); pScan->setWindow(99);

  Serial.println("[ESP1] Ready — waiting for commands from ESP2");
  Serial.println();
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop(){
  // Keep portal alive (DNS + HTTP)
  if(portalUp){ dnsSrv.processNextRequest(); alertSrv.handleClient(); }

  if(pollCmd()){
    Serial.printf("[CMD] 0x%02X  payload=%d bytes\n",rxCmd,rxPlen);

    switch(rxCmd){
      case CMD_WIFI_SCAN:    doWifiScan(); break;
      case CMD_BLE_SCAN:     doBleScan();  break;
      case CMD_NRF_SCAN:     doNrfScan(rxPlen>0?rxBuf[0]:10); break;
      case CMD_ALERT_SSID:{
        char msg[33]={}; strncpy(msg,(char*)rxBuf,min((int)rxPlen,32));
        startAlertSsid(msg); break;
      }
      case CMD_ALERT_BLE:{
        char msg[33]={}; strncpy(msg,(char*)rxBuf,min((int)rxPlen,32));
        startAlertBle(msg); break;
      }
      case CMD_ALERT_PORTAL:{
        char msg[33]={}; strncpy(msg,(char*)rxBuf,min((int)rxPlen,32));
        startAlertPortal(msg); break;
      }
      case CMD_STOP_ALERT: stopAlerts(); sendOk(); break;
      default: Serial.printf("[WARN] Unknown cmd 0x%02X\n",rxCmd); break;
    }
  }

  delay(5);
}
