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
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"

// ── UART ─────────────────────────────────────────────────────
#define LINK_TX   17
#define LINK_RX   32
#define LINK_BAUD 115200
HardwareSerial lnk(2);

// Slide switch — JR.IO mode selector (on ESP1 board only)
#define SLIDE_SW  33   // INPUT_PULLUP: HIGH=sweep mode  LOW=random mode

// Protocol (must match ESP2)
#define SOF_CMD  0xAA
#define SOF_RESP 0xBB
#define EOF_BYTE 0x55

#define CMD_WIFI_SCAN    0x01
#define CMD_BLE_SCAN     0x02
#define CMD_NRF_SCAN     0x03
#define CMD_ALERT_SSID   0x04
#define CMD_ALERT_BLE    0x05
#define CMD_ALERT_ALL    0x06
#define CMD_STOP_ALERT   0x07
#define CMD_JRIO_START   0x08
#define CMD_JRIO_STOP    0x09

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
bool bleAdvert=false;

// ── JR.IO state ──────────────────────────────────────────────
volatile bool jrioRunning = false;
int jCh=45, jCh1=45, jFlag=0, jFlagV=0;

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
  // Stop any active alert before scanning
  stopAlerts();

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
//  ALERT: BLAST ALL  (WiFi SSID + BLE beacon simultaneously)
// ═══════════════════════════════════════════════════════════════
//
//  HOW IT REACHES DEVICES WITHOUT ANY CONNECTION:
//
//  ① WiFi SSID Blast — ESP1 becomes an open WiFi access point
//    whose NETWORK NAME IS the alert message.  Every phone,
//    laptop and tablet that has WiFi on sees it instantly in
//    their WiFi list.  No connection, no app — the message IS
//    the SSID.  iOS/Android show new networks in their status
//    bar notification.  Range: 50–100 m.
//
//  ② BLE Advertising — ESP1 continuously broadcasts BLE
//    advertisement packets (GAP ADV_IND) in all directions.
//    The alert message is placed in the "Complete Local Name"
//    field of every packet.  Any BLE-enabled device passively
//    receives these even while locked — visible in Bluetooth
//    settings or any BLE scanner app.  TX power is set to
//    maximum (+9 dBm) for maximum range.  Range: 30–50 m.
//
//  Both run at the same time using ESP32's built-in
//  WiFi+BT coexistence (TBTT time-sharing).
//  No connection from any device is ever needed or expected.
//
void startAlertAll(const char* msg){
  stopAlerts();

  // ── ① WiFi SSID blast ────────────────────────────────────
  // Truncate to 32 chars (SSID limit).
  // Prefix "!! " grabs attention in the network list.
  char ssid[33]={};
  if(strlen(msg)<=29){
    snprintf(ssid,sizeof(ssid),"!! %s",msg);
  } else {
    strncpy(ssid,msg,32);
  }
  WiFi.mode(WIFI_AP_STA);           // AP+STA allows WiFi+BT coexistence
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // max TX power
  WiFi.softAP(ssid,               // SSID = alert message
              nullptr,            // no password — open network
              6,                  // channel 6 (most monitored)
              false,              // visible SSID
              8);                 // max 8 clients (unused but keeps AP alive)
  Serial.printf("[ALERT-SSID] Broadcasting SSID: \"%s\"\n",ssid);

  // ── ② BLE beacon ─────────────────────────────────────────
  // Reinitialise BLE device with alert as the device name.
  // The name appears in every single ADV_IND packet — no
  // connection or pairing needed for the name to be visible.
  if(bleAdvert&&pAdvert){ pAdvert->stop(); bleAdvert=false; }

  BLEDevice::init(msg);

  // Set BLE TX power to maximum (+9 dBm)
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,  ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  pAdvert=BLEDevice::getAdvertising();
  pAdvert->setScanResponse(true);      // include name in scan response too
  pAdvert->setMinInterval(0x20);       // 20ms — fast advertising for max reach
  pAdvert->setMaxInterval(0x40);       // 40ms
  pAdvert->start();
  bleAdvert=true;

  // Restore scan callbacks for future BLE scan commands
  pScan=BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(),true);
  pScan->setActiveScan(true);
  pScan->setInterval(100); pScan->setWindow(99);

  Serial.printf("[ALERT-BLE]  Beacon name: \"%s\"  txPwr=+9dBm\n",msg);
  Serial.println("[ALERT] Both WiFi SSID + BLE active — no connection required");
  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  STOP ALL ALERTS
// ═══════════════════════════════════════════════════════════════
void stopAlerts(){
  if(bleAdvert&&pAdvert){ pAdvert->stop(); bleAdvert=false; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA); WiFi.disconnect(true);
  delay(100);
  Serial.println("[ALERT] All alerts stopped");
}

// ═══════════════════════════════════════════════════════════════
//  JR.IO — 2.4 GHz DISRUPTOR ENGINE
// ═══════════════════════════════════════════════════════════════
/*
 *  Uses both NRF24L01+PA/LNA modules in constant-carrier TX mode
 *  at maximum power (RF24_PA_MAX = +20 dBm with PA/LNA board).
 *
 *  TWO MODES selected by slide switch (GPIO33, INPUT_PULLUP):
 *    Switch HIGH (open/off) → jrioTwo() : staggered sweep
 *      HSPI hops ±2 channels, VSPI hops ±4 channels — they
 *      sweep the 2.4 GHz band at different rates so they are
 *      rarely on the same channel, maximising coverage.
 *
 *    Switch LOW  (closed to GND) → jrioOne() : random hop
 *      Both radios jump to independent random channels every
 *      ~60 µs — more chaotic, harder for adaptive systems.
 *
 *  startConstCarrier(level, ch) sets CE high and puts the
 *  nRF24L01 into continuous-wave transmission.  Subsequent
 *  setChannel(ch) calls change frequency while CE stays high.
 *
 *  No packets are sent — pure unmodulated 2.4 GHz carrier.
 *  Range: limited only by PA/LNA amplifier (100–200 m).
 */

// ── Mode 1: Random hop ───────────────────────────────────────
void jrioOne(){
  if(r0ok) r0.setChannel(random(80));
  if(r1ok) r1.setChannel(random(80));
  delayMicroseconds(random(60));
}

// ── Mode 2: Staggered sweep ──────────────────────────────────
void jrioTwo(){
  // VSPI — faster sweep (±4)
  if(jFlagV==0){ jCh1+=4; if(jCh1>79){jCh1=79;jFlagV=1;} }
  else          { jCh1-=4; if(jCh1<2) {jCh1=2; jFlagV=0;} }
  // HSPI — slower sweep (±2)
  if(jFlag==0) { jCh+=2;  if(jCh>79) {jCh=79;  jFlag=1;}  }
  else         { jCh-=2;  if(jCh<2)  {jCh=2;   jFlag=0;}  }
  if(r0ok) r0.setChannel(jCh);
  if(r1ok) r1.setChannel(jCh1);
}

// ── Start JR.IO ──────────────────────────────────────────────
void startJrio(){
  Serial.println("[JR.IO] Starting...");
  jrioRunning=false; // ensure clean state

  // Stop any ongoing alert (WiFi AP / BLE advert)
  stopAlerts();
  delay(100);

  // Reset sweep counters
  jCh=45; jCh1=45; jFlag=0; jFlagV=0;

  // Switch HSPI radio (r0) to constant carrier TX mode
  if(r0ok){
    r0.stopListening();
    r0.setAutoAck(false);
    r0.setRetries(0,0);
    r0.setCRCLength(RF24_CRC_DISABLED);
    r0.setDataRate(RF24_2MBPS);
    r0.setPALevel(RF24_PA_MAX,true);  // true = LNA gain on
    r0.startConstCarrier(RF24_PA_MAX, jCh);
    Serial.printf("[JR.IO] HSPI carrier ON  ch=%d\n",jCh);
  }

  // Switch VSPI radio (r1) to constant carrier TX mode
  if(r1ok){
    r1.stopListening();
    r1.setAutoAck(false);
    r1.setRetries(0,0);
    r1.setCRCLength(RF24_CRC_DISABLED);
    r1.setDataRate(RF24_2MBPS);
    r1.setPALevel(RF24_PA_MAX,true);
    r1.startConstCarrier(RF24_PA_MAX, jCh1);
    Serial.printf("[JR.IO] VSPI carrier ON  ch=%d\n",jCh1);
  }

  jrioRunning=true;
  Serial.println("[JR.IO] Active — SW HIGH=sweep  SW LOW=random");
  sendOk(); // notify ESP2 we are live
}

// ── Stop JR.IO ───────────────────────────────────────────────
void stopJrio(){
  jrioRunning=false;

  // Bring radios out of constant carrier, restore scan config
  if(r0ok){
    r0.stopConstCarrier();
    r0.setAutoAck(false);
    r0.setCRCLength(RF24_CRC_DISABLED);
    r0.setDataRate(RF24_2MBPS);
    r0.setPALevel(RF24_PA_MIN);
    r0.startListening();
    Serial.println("[JR.IO] HSPI restored");
  }
  if(r1ok){
    r1.stopConstCarrier();
    r1.setAutoAck(false);
    r1.setCRCLength(RF24_CRC_DISABLED);
    r1.setDataRate(RF24_2MBPS);
    r1.setPALevel(RF24_PA_MIN);
    r1.startListening();
    Serial.println("[JR.IO] VSPI restored");
  }

  // Re-enable WiFi for scanning
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  Serial.println("[JR.IO] Stopped — all radios back to scan mode");
  sendOk();
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

  // Slide switch — JR.IO mode select
  pinMode(SLIDE_SW, INPUT_PULLUP);
  Serial.printf("[SW]   Slide switch GPIO%d: %s\n",
                SLIDE_SW, digitalRead(SLIDE_SW)==HIGH?"SWEEP mode":"RANDOM mode");

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
  // ── JR.IO tight engine loop ────────────────────────────────
  // While JR.IO is active this runs as fast as possible.
  // UART is still polled every iteration so stop cmd is instant.
  if(jrioRunning){
    if(digitalRead(SLIDE_SW)==HIGH) jrioTwo();  // sweep
    else                             jrioOne();  // random
    // Poll UART without blocking — catches stop command
    if(pollCmd()&&rxCmd==CMD_JRIO_STOP){ stopJrio(); return; }
    yield(); // keep watchdog fed
    return;  // skip normal delay at bottom
  }

  // ── Normal command processing ──────────────────────────────
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
      case CMD_ALERT_ALL:{
        char msg[33]={}; strncpy(msg,(char*)rxBuf,min((int)rxPlen,32));
        startAlertAll(msg); break;
      }
      case CMD_STOP_ALERT:   stopAlerts(); sendOk(); break;
      case CMD_JRIO_START:   startJrio();  break;
      case CMD_JRIO_STOP:    stopJrio();   break;
      default: Serial.printf("[WARN] Unknown cmd 0x%02X\n",rxCmd); break;
    }
  }

  delay(5);
}
