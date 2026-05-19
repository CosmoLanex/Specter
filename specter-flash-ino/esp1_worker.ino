/*
 * ════════════════════════════════════════════════════════════
 *  ESP1 — SCAN & ALERT WORKER  "NUKRAX" v3.0
 * ════════════════════════════════════════════════════════════
 *  NRF24 no1 (HSPI): SCK=14 MISO=12 MOSI=13 CS=15 CE=16
 *  NRF24 no2 (VSPI): SCK=18 MISO=19 MOSI=23 CS=21 CE=22
 *  Slide switch: middle=GND  right=GPIO33
 *  UART to ESP2:  TX=GPIO17 → ESP2 RX=GPIO17
 *                 RX=GPIO32 ← ESP2 TX=GPIO32
 *
 *  REQUIRED LIBRARIES:
 *    • RF24  by TMRh20
 *    • ezButton  by ArduinoGetStarted
 * ════════════════════════════════════════════════════════════
 */

// ── Includes ─────────────────────────────────────────────────
#include "RF24.h"
#include <SPI.h>
#include <ezButton.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "esp_bluedroid_api.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>
#include "esp_gap_ble_api.h"

// ═══════════════════════════════════════════════════════════════
//  ██████████████████████████████████████████████████████████
//  ██  ORIGINAL JR.IO CODE — DO NOT MODIFY  ████████████████
//  ██  (preserved exactly from more.ino)    ████████████████
//  ██████████████████████████████████████████████████████████
// ═══════════════════════════════════════════════════════════════

SPIClass* sp = nullptr;
SPIClass* hp = nullptr;

RF24 radio(16, 15, 16000000);   //HSPI CAN SET SPI SPEED TO 16000000 BY DEFAULT ITS 10000000
RF24 radio1(22, 21, 16000000);  //VSPI CAN SET SPI SPEED TO 16000000 BY DEFAULT ITS 10000000

//HSPI=SCK = 14, MISO = 12, MOSI = 13, CS = 15 , CE = 16
//VSPI=SCK = 18, MISO =19, MOSI = 23 ,CS =21 ,CE = 22

unsigned int flag  = 0;   //HSPI// Flag variable to keep track of direction
unsigned int flagv = 0;   //VSPI// Flag variable to keep track of direction
int ch  = 45;             // Variable to store value of ch
int ch1 = 45;             // Variable to store value of ch

ezButton toggleSwitch(33);

void two() {
  if (flagv == 0) {  // If flag is 0, increment ch by 4 and ch1 by 1

    ch1 += 4;
  } else {  // If flag is not 0, decrement ch by 4 and ch1 by 1

    ch1 -= 4;
  }

  if (flag == 0) {  // If flag is 0, increment ch by 4 and ch1 by 1
    ch += 2;

  } else {  // If flag is not 0, decrement ch by 4 and ch1 by 1
    ch -= 2;
  }

  // Check if ch1 is greater than 79 and flag is 0
  if ((ch1 > 79) && (flagv == 0)) {
    flagv = 1;                             // Set flag to 1 to change direction
  } else if ((ch1 < 2) && (flagv == 1)) {  // Check if ch1 is less than 2 and flag is 1
    flagv = 0;                             // Set flag to 0 to change direction
  }

  // Check if ch is greater than 79 and flag is 0
  if ((ch > 79) && (flag == 0)) {
    flag = 1;                            // Set flag to 1 to change direction
  } else if ((ch < 2) && (flag == 1)) {  // Check if ch is less than 2 and flag is 1
    flag = 0;                            // Set flag to 0 to change direction
  }
  radio.setChannel(ch);
  radio1.setChannel(ch1);
  /*Serial.print("SP:");
  Serial.println(ch1);
  Serial.print("\tHP:");
  Serial.println(ch);*/
}

void one() {
  ////RANDOM CHANNEL
  radio1.setChannel(random(80));
  radio.setChannel(random(80));
  delayMicroseconds(random(60));//////REMOVE IF SLOW


 /*  YOU CAN DO -----SWEEP CHANNEL
  for (int i = 0; i < 79; i++) {
    radio.setChannel(i);
*/

}

void initSP() {
  sp = new SPIClass(VSPI);
  sp->begin();
  if (radio1.begin(sp)) {
    Serial.println("SP Started !!!");
    radio1.setAutoAck(false);
    radio1.stopListening();
    radio1.setRetries(0, 0);
    radio1.setPALevel(RF24_PA_MAX, true);
    radio1.setDataRate(RF24_2MBPS);
    radio1.setCRCLength(RF24_CRC_DISABLED);
    radio1.printPrettyDetails();
    radio1.startConstCarrier(RF24_PA_MAX, ch1);
  } else {
    Serial.println("SP couldn't start !!!");
  }
}

void initHP() {
  hp = new SPIClass(HSPI);
  hp->begin();
  if (radio.begin(hp)) {
    Serial.println("HP Started !!!");
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.printPrettyDetails();
    radio.startConstCarrier(RF24_PA_MAX, ch);
  } else {
    Serial.println("HP couldn't start !!!");
  }
}

// ═══════════════════════════════════════════════════════════════
//  ██  END OF ORIGINAL CODE  ███████████████████████████████
// ═══════════════════════════════════════════════════════════════

// ── UART ─────────────────────────────────────────────────────
#define LINK_TX   17
#define LINK_RX   32
#define LINK_BAUD 115200
HardwareSerial lnk(2);

// ── Protocol ─────────────────────────────────────────────────
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

// ── Radio state flags ─────────────────────────────────────────
bool hpOk = false;   // HSPI radio (radio)  ready
bool spOk = false;   // VSPI radio (radio1) ready

// ── JR.IO state ──────────────────────────────────────────────
volatile bool jrioRunning = false;

// ── Alert state ──────────────────────────────────────────────
bool bleAdvert = false;

// ── BLE scan data ────────────────────────────────────────────
#define MAX_BLE_RES 25
struct BleNode { char name[20]; char addr[18]; int8_t rssi; };
BleNode bleRes[MAX_BLE_RES];
int bleCount = 0;

BLEScan*        pScan   = nullptr;
BLEAdvertising* pAdvert = nullptr;

void stopAlerts(); // forward declaration

class BLECb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (bleCount >= MAX_BLE_RES) return;
    String n = d.getName().c_str();
    strncpy(bleRes[bleCount].name, (n.length()>0 ? n.c_str() : "<unnamed>"), 19);
    strncpy(bleRes[bleCount].addr, d.getAddress().toString().c_str(), 17);
    bleRes[bleCount].rssi = (int8_t)d.getRSSI();
    bleCount++;
  }
};

// ── NRF spectrum buffer ───────────────────────────────────────
#define SPEC_R  2
#define SPEC_CH 126
uint8_t spectrum[SPEC_R][SPEC_CH];

// ═══════════════════════════════════════════════════════════════
//  UART — RECEIVE COMMAND (non-blocking state machine)
// ═══════════════════════════════════════════════════════════════
enum RxSt { RX_IDLE, RX_CMD, RX_PLEN, RX_DATA, RX_CHK, RX_EOF };
RxSt    rxSt   = RX_IDLE;
uint8_t rxCmd  = 0, rxPlen = 0, rxIdx = 0, rxChk = 0;
uint8_t rxBuf[64];

bool pollCmd() {
  while (lnk.available()) {
    uint8_t b = lnk.read();
    switch (rxSt) {
      case RX_IDLE:  if (b == SOF_CMD) { rxSt = RX_CMD; rxChk = 0; } break;
      case RX_CMD:   rxCmd  = b; rxChk ^= b; rxSt = RX_PLEN; break;
      case RX_PLEN:  rxPlen = b; rxChk ^= b; rxIdx = 0;
                     rxSt = (b > 0 ? RX_DATA : RX_CHK); break;
      case RX_DATA:
        if (rxIdx < sizeof(rxBuf)) rxBuf[rxIdx] = b;
        rxChk ^= b; rxIdx++;
        if (rxIdx >= rxPlen) rxSt = RX_CHK;
        break;
      case RX_CHK:
        rxSt = RX_EOF;
        if (b != rxChk) { rxSt = RX_IDLE; Serial.println("[RX] CHK fail"); }
        break;
      case RX_EOF:
        rxSt = RX_IDLE;
        if (b == EOF_BYTE) return true;
        break;
    }
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════
//  UART — SEND RESPONSE
// ═══════════════════════════════════════════════════════════════
void sendResp(uint8_t type, const uint8_t* payload, uint16_t plen) {
  uint8_t chk = type;
  chk ^= (uint8_t)(plen >> 8);
  chk ^= (uint8_t)(plen & 0xFF);
  for (uint16_t i = 0; i < plen; i++) chk ^= payload[i];
  lnk.write(SOF_RESP);
  lnk.write(type);
  lnk.write((uint8_t)(plen >> 8));
  lnk.write((uint8_t)(plen & 0xFF));
  if (plen && payload) lnk.write(payload, plen);
  lnk.write(chk);
  lnk.write(EOF_BYTE);
  lnk.flush();
  Serial.printf("[TX] RESP 0x%02X  %d bytes\n", type, plen);
}

void sendOk() { sendResp(RESP_OK, nullptr, 0); }

// ═══════════════════════════════════════════════════════════════
//  INIT RADIOS FOR SCAN MODE (low power passive RX)
// ═══════════════════════════════════════════════════════════════
void initRadiosForScan() {
  // Free old SPI instances if they exist, then create fresh ones
  if (hp) { delete hp; hp = nullptr; }
  if (sp) { delete sp; sp = nullptr; }

  hp = new SPIClass(HSPI);
  hp->begin();   // default HSPI: SCK=14 MISO=12 MOSI=13 — matches circuit

  sp = new SPIClass(VSPI);
  sp->begin();   // default VSPI: SCK=18 MISO=19 MOSI=23 — matches circuit

  hpOk = false;
  if (radio.begin(hp)) {
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setDataRate(RF24_2MBPS);
    radio.setPALevel(RF24_PA_MIN);   // min power for passive scan
    radio.startListening();
    hpOk = true;
    Serial.println("[OK]  HSPI radio ready (scan mode)");
  } else {
    Serial.println("[ERR] HSPI radio failed");
  }

  spOk = false;
  if (radio1.begin(sp)) {
    radio1.setAutoAck(false);
    radio1.setRetries(0, 0);
    radio1.setCRCLength(RF24_CRC_DISABLED);
    radio1.setDataRate(RF24_2MBPS);
    radio1.setPALevel(RF24_PA_MIN);
    radio1.startListening();
    spOk = true;
    Serial.println("[OK]  VSPI radio ready (scan mode)");
  } else {
    Serial.println("[ERR] VSPI radio failed");
  }

  Serial.printf("[NRF] %d/2 radios operational\n", (int)hpOk + (int)spOk);
}

// ═══════════════════════════════════════════════════════════════
//  JR.IO — START
// ═══════════════════════════════════════════════════════════════
void startJrio() {
  jrioRunning = false;
  Serial.println("[JR.IO] Starting...");

  // Stop any active alert cleanly
  stopAlerts();
  delay(100);

  // Deinit BT and WiFi — exactly as original more.ino setup() sequence
  esp_bt_controller_deinit();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_disconnect();

  // Reset sweep variables to starting state
  flag = 0; flagv = 0; ch = 45; ch1 = 45;

  // Free existing SPI instances before original code creates new ones
  if (hp) { delete hp; hp = nullptr; }
  if (sp) { delete sp; sp = nullptr; }

  // ── Run exact original init functions ──────────────────────
  initHP();   // << ORIGINAL CODE — sets HSPI to constant carrier
  initSP();   // << ORIGINAL CODE — sets VSPI to constant carrier

  jrioRunning = true;
  Serial.println("[JR.IO] Active — SW HIGH=sweep  SW LOW=random");
  // No sendOk() here — would corrupt ESP2 state machine
}

// ═══════════════════════════════════════════════════════════════
//  JR.IO — STOP
// ═══════════════════════════════════════════════════════════════
void stopJrio() {
  jrioRunning = false;
  Serial.println("[JR.IO] Stopping...");

  // Stop constant carrier on both radios
  if (hpOk || radio.isChipConnected()) {
    radio.stopConstCarrier();
    Serial.println("[JR.IO] HSPI carrier OFF");
  }
  if (spOk || radio1.isChipConnected()) {
    radio1.stopConstCarrier();
    Serial.println("[JR.IO] VSPI carrier OFF");
  }

  // Restore radios to passive scan mode
  initRadiosForScan();

  // Re-initialize WiFi for scanning
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_start();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(300);

  // Re-initialize BLE controller from scratch
  // Required because esp_bt_controller_deinit() was called in startJrio()
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_bt_controller_init(&bt_cfg);
  esp_bt_controller_enable(ESP_BT_MODE_BLE);
  esp_bluedroid_init();
  esp_bluedroid_enable();

  // Re-attach Arduino BLE scan layer
  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  Serial.println("[JR.IO] Stopped — WiFi + BLE restored for scanning");
  // No sendOk() here — would corrupt ESP2 state machine
}

// ═══════════════════════════════════════════════════════════════
//  WIFI SCAN
// ═══════════════════════════════════════════════════════════════
void doWifiScan() {
  Serial.println("[WIFI] Scanning...");
  stopAlerts();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  int n = WiFi.scanNetworks(false, true);
  if (n < 0) n = 0;
  n = min(n, 30);
  Serial.printf("[WIFI] Found %d networks\n", n);

  // Payload: count(1) + n×35 bytes  [ssid(32) rssi(1) ch(1) enc(1)]
  uint16_t plen = 1 + (uint16_t)n * 35;
  uint8_t* buf  = (uint8_t*)malloc(plen);
  if (!buf) { Serial.println("[ERR] malloc fail"); return; }

  buf[0] = (uint8_t)n;
  for (int i = 0; i < n; i++) {
    uint8_t* p = buf + 1 + i * 35;
    memset(p, 0, 35);
    strncpy((char*)p, WiFi.SSID(i).c_str(), 31);
    p[32] = (uint8_t)(int8_t)WiFi.RSSI(i);
    p[33] = (uint8_t)WiFi.channel(i);
    p[34] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
  }
  WiFi.scanDelete();
  sendResp(RESP_WIFI, buf, plen);
  free(buf);
}

// ═══════════════════════════════════════════════════════════════
//  BLE SCAN
// ═══════════════════════════════════════════════════════════════
void doBleScan() {
  Serial.println("[BLE] Scanning 6s...");
  if (bleAdvert && pAdvert) { pAdvert->stop(); bleAdvert = false; }

  bleCount = 0;
  pScan->clearResults();
  pScan->start(6, false);

  Serial.printf("[BLE] Found %d devices\n", bleCount);

  // Payload: count(1) + n×39 bytes  [name(20) addr(18) rssi(1)]
  uint16_t plen = 1 + (uint16_t)bleCount * 39;
  uint8_t* buf  = (uint8_t*)malloc(plen);
  if (!buf) { Serial.println("[ERR] malloc fail"); return; }

  buf[0] = (uint8_t)bleCount;
  for (int i = 0; i < bleCount; i++) {
    uint8_t* p = buf + 1 + i * 39;
    memset(p, 0, 39);
    strncpy((char*)p,      bleRes[i].name, 19);
    strncpy((char*)p + 20, bleRes[i].addr, 17);
    p[38] = (uint8_t)(int8_t)bleRes[i].rssi;
  }
  sendResp(RESP_BLE, buf, plen);
  free(buf);
}

// ═══════════════════════════════════════════════════════════════
//  NRF24 SPECTRUM SCAN  (passive RX, uses radio & radio1)
// ═══════════════════════════════════════════════════════════════
void doNrfScan(uint8_t passes) {
  memset(spectrum, 0, sizeof(spectrum));
  Serial.printf("[NRF] %d passes × 63 steps × 2 radios\n", passes);

  for (uint8_t pass = 0; pass < passes; pass++) {
    for (uint8_t base = 0; base < 63; base++) {
      uint8_t c0 = base;       // HSPI: channels  0–62
      uint8_t c1 = base + 63;  // VSPI: channels 63–125

      if (hpOk) { radio.setChannel(c0);  radio.startListening(); }
      if (spOk) { radio1.setChannel(c1); radio1.startListening(); }

      delayMicroseconds(280);

      if (hpOk) { if (radio.testRPD())  spectrum[0][c0]++; radio.stopListening();  }
      if (spOk) { if (radio1.testRPD()) spectrum[1][c1]++; radio1.stopListening(); }
    }
    if (pass % 5 == 4) delay(1);
  }

  for (int r = 0; r < 2; r++) {
    Serial.printf("[NRF] Radio%d: ", r);
    for (int c = 0; c < SPEC_CH; c++)
      if (spectrum[r][c]) Serial.printf("ch%d(%d) ", c, spectrum[r][c]);
    Serial.println();
  }

  sendResp(RESP_NRF, (uint8_t*)spectrum, SPEC_R * SPEC_CH);
}

// ═══════════════════════════════════════════════════════════════
//  ALERT — STOP ALL
// ═══════════════════════════════════════════════════════════════
void stopAlerts() {
  if (bleAdvert && pAdvert) { pAdvert->stop(); bleAdvert = false; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  Serial.println("[ALERT] Stopped");
}

// ═══════════════════════════════════════════════════════════════
//  ALERT — WIFI SSID BLAST
// ═══════════════════════════════════════════════════════════════
void startAlertSsid(const char* msg) {
  stopAlerts();
  char ssid[33] = {};
  strncpy(ssid, msg, 32);
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.softAP(ssid, nullptr, 6, false, 4);
  Serial.printf("[ALERT-SSID] \"%s\"\n", ssid);
  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  ALERT — BLE BEACON
// ═══════════════════════════════════════════════════════════════
void startAlertBle(const char* msg) {
  stopAlerts();
  if (bleAdvert && pAdvert) { pAdvert->stop(); bleAdvert = false; }
  BLEDevice::init(msg);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  pAdvert = BLEDevice::getAdvertising();
  pAdvert->setScanResponse(true);
  pAdvert->setMinInterval(0x20);
  pAdvert->setMaxInterval(0x40);
  pAdvert->start();
  bleAdvert = true;
  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  Serial.printf("[ALERT-BLE] \"%s\"\n", msg);
  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  ALERT — BLAST ALL  (WiFi SSID + BLE simultaneously)
// ═══════════════════════════════════════════════════════════════
void startAlertAll(const char* msg) {
  stopAlerts();

  // WiFi SSID
  char ssid[33] = {};
  if (strlen(msg) <= 29) snprintf(ssid, sizeof(ssid), "!! %s", msg);
  else strncpy(ssid, msg, 32);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.softAP(ssid, nullptr, 6, false, 8);

  // BLE beacon
  if (bleAdvert && pAdvert) { pAdvert->stop(); bleAdvert = false; }
  BLEDevice::init(msg);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  pAdvert = BLEDevice::getAdvertising();
  pAdvert->setScanResponse(true);
  pAdvert->setMinInterval(0x20);
  pAdvert->setMaxInterval(0x40);
  pAdvert->start();
  bleAdvert = true;
  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  Serial.printf("[ALERT-ALL] SSID:\"%s\"  BLE:\"%s\"\n", ssid, msg);
  sendOk();
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("╔═══════════════════════════════╗");
  Serial.println("║  NUKRAX  ESP1  Worker  v3.0   ║");
  Serial.println("╚═══════════════════════════════╝");

  // ezButton debounce (used by JR.IO slide switch)
  toggleSwitch.setDebounceTime(50);

  // UART link to ESP2
  lnk.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  Serial.printf("[UART] TX=GPIO%d  RX=GPIO%d\n", LINK_TX, LINK_RX);

  // Init radios in passive scan mode (low power RX)
  initRadiosForScan();

  // WiFi in STA mode ready for scanning
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);

  // BLE init for scanning and alerts
  BLEDevice::init("ESP1-NukraxWorker");
  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new BLECb(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  Serial.println("[ESP1] Ready — waiting for commands from ESP2");
  Serial.println();
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {

  // ── JR.IO tight engine ─────────────────────────────────────
  // When active: runs at full speed, no delay.
  // Uses EXACT original loop() logic from more.ino.
  if (jrioRunning) {
    toggleSwitch.loop();              // << ORIGINAL — must call first
    int state = toggleSwitch.getState();
    if (state == HIGH) two();         // << ORIGINAL — sweep mode
    else               one();         // << ORIGINAL — random mode

    // Poll UART every iteration — catches stop command instantly
    if (pollCmd() && rxCmd == CMD_JRIO_STOP) {
      stopJrio();
    }
    yield();   // keep watchdog fed
    return;    // no delay at bottom while JR.IO is running
  }

  // ── Normal command processing ──────────────────────────────
  if (pollCmd()) {
    Serial.printf("[CMD] 0x%02X  payload=%d bytes\n", rxCmd, rxPlen);

    switch (rxCmd) {
      case CMD_WIFI_SCAN:  doWifiScan(); break;
      case CMD_BLE_SCAN:   doBleScan();  break;
      case CMD_NRF_SCAN:   doNrfScan(rxPlen > 0 ? rxBuf[0] : 10); break;

      case CMD_ALERT_SSID: {
        char msg[33] = {};
        strncpy(msg, (char*)rxBuf, min((int)rxPlen, 32));
        startAlertSsid(msg);
        break;
      }
      case CMD_ALERT_BLE: {
        char msg[33] = {};
        strncpy(msg, (char*)rxBuf, min((int)rxPlen, 32));
        startAlertBle(msg);
        break;
      }
      case CMD_ALERT_ALL: {
        char msg[33] = {};
        strncpy(msg, (char*)rxBuf, min((int)rxPlen, 32));
        startAlertAll(msg);
        break;
      }
      case CMD_STOP_ALERT:  stopAlerts(); break;  // no sendOk — ESP2 clears state itself
      case CMD_JRIO_START:  startJrio();  break;   // no sendOk — ESP2 sets state itself
      case CMD_JRIO_STOP:   stopJrio();   break;   // no sendOk — ESP2 sets state itself

      default:
        Serial.printf("[WARN] Unknown cmd 0x%02X\n", rxCmd);
        break;
    }
  }

  delay(5);
}
