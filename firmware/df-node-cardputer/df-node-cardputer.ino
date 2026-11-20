// ============================================================================
//  SCARECROW DF NODE  —  M5Stack Cardputer ADV + Cap CC1101
//  ESP32-S3 (Stamp-S3A) + TI CC1101 (M5 Cap, SKU U219). Parks on the watch
//  frequency, reports RSSI + burst-timing fingerprints to the Evil Crow
//  collector over ESP-NOW, and shows a live readout on the 240x135 screen.
//  RECEIVE-ONLY: there is no transmit call anywhere in this firmware.
//
//  Cap CC1101 wiring on the Cardputer-Adv (from M5Stack's Cap CC1101 docs):
//    CS   G5     SCK  G40    MOSI G14    MISO G39    GDO0 G15
//    RF_SW0 = host G13 ;  RF_SW1 = CC1101 GDO2 (driven via IOCFG2 register)
//    Band select:  315 -> SW0=0,SW1=0   433 -> SW0=0,SW1=1   868/915 -> SW0=1,SW1=1
//    SPI bus is shared with the microSD (CS G12) — we park it HIGH.
// ============================================================================
#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <RadioLib.h>
#include <M5Unified.h>
#include <M5Cardputer.h>     // TCA8418 keyboard (Cardputer-Adv), wraps M5Unified
#include <Preferences.h>
#include "df_packet.h"

#define BTN_G0 0                 // G0 button: cycles screen rotation, saved to flash
static Preferences prefs;
static uint8_t hudRot = 1;       // 240x135 landscape by default

// ---- identity & link -------------------------------------------------------
#define NODE_ID            'C'       // UNIQUE per node ('A'=T-Embed, 'C'=Cardputer)
#define COLLECTOR_CHANNEL  6         // MUST equal the Evil Crow's AP_CHANNEL
#define TARGET_FREQ_MHZ    433.92f
#define TARGET_RXBW_KHZ    58
#define RSSI_BEACON_MS     100
#define RSSI_SAMPLES       6
#define BURST_GAP_US       8000
#define BURST_MIN_EDGES    16
#define BURST_MAX_EDGES    512
#define BURST_MIN_US       100

// ---- Cap CC1101 pins (Cardputer-Adv) ---------------------------------------
#define CC_SCK   40
#define CC_MISO  39
#define CC_MOSI  14
#define CC_CS    5
#define CC_GDO0  15        // raw OOK data envelope + edge IRQ
#define RF_SW0   13        // host pin, band select low bit
#define CC_SD_CS 12        // microSD CS on the shared bus — park HIGH
// CC1101 register (RF_SW1 = GDO2 static level via IOCFG2)
#define CC_REG_IOCFG2      0x00
#define IOCFG2_LOW         0x2F      // GDO2 = constant 0  ("HW to 0")
#define IOCFG2_HIGH        0x6F      // GDO2 = constant 1  (0x2F | GDOx_INV)

// ---- radio (CC1101 on its own HSPI; GDO2 is NC to RadioLib) -----------------
// Subclass to reach the protected register write — needed to hold the CC1101's
// GDO2 at a static level (that pin is the cap's RF_SW1 band-select line).
class CC1101X : public CC1101 {
public:
  CC1101X(Module *m) : CC1101(m) {}
  void writeReg(uint8_t reg, uint8_t val) { getMod()->SPIsetRegValue(reg, val); }
};
static SPIClass ccSPI(HSPI);
static CC1101X radio(new Module(CC_CS, CC_GDO0, RADIOLIB_NC, RADIOLIB_NC, ccSPI));

static uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint32_t seq = 0, lastBeacon = 0;
float    watchMhz = TARGET_FREQ_MHZ;
int16_t  lastRssi = -120;
uint16_t lastTe = 0;
uint32_t burstCount = 0;
uint8_t  nodeVer = 0;               // 0x14 once the CC1101 answers; 0 = dead
int      radioInitRC = -999;
volatile uint32_t txOk = 0, txFail = 0;
int      lastSendRC = -1;
bool     espnowOk = false;
bool     haveDisplay = false;

// ---- views + local signature store -----------------------------------------
enum View { V_RSSI = 0, V_SIGS, V_DIAG, V_COUNT };
uint8_t  view = V_RSSI;

#define NSIG 12
struct NSig { bool used; uint16_t te; uint32_t count; int16_t rssi; uint32_t lastMs; };
NSig nsigs[NSIG] = {};
void sigAdd(uint16_t te, int16_t rssi) {
  int slot = -1, freeSlot = -1;
  for (int i = 0; i < NSIG; i++) {
    if (!nsigs[i].used) { if (freeSlot < 0) freeSlot = i; continue; }
    if (abs((int)nsigs[i].te - (int)te) <= 90) { slot = i; break; }
  }
  if (slot < 0) slot = (freeSlot >= 0) ? freeSlot : 0;   // reuse oldest-ish if full
  if (!nsigs[slot].used) { nsigs[slot].used = true; nsigs[slot].count = 0; }
  nsigs[slot].te = te; nsigs[slot].rssi = rssi;
  nsigs[slot].count++; nsigs[slot].lastMs = millis();
}

void onDfSent(const wifi_tx_info_t *info, esp_now_send_status_t st) {
  if (st == ESP_NOW_SEND_SUCCESS) txOk++; else txFail++;
}

// Drive the cap's SP3T RF switch for the requested band. RF_SW0 is a host GPIO;
// RF_SW1 is the CC1101's GDO2, held at a static level by writing IOCFG2.
static void setBand(float mhz) {
  bool sw1;
  if (mhz < 350)      { digitalWrite(RF_SW0, LOW);  sw1 = false; }  // 315
  else if (mhz < 600) { digitalWrite(RF_SW0, LOW);  sw1 = true;  }  // 433
  else                { digitalWrite(RF_SW0, HIGH); sw1 = true;  }  // 868/915
  radio.writeReg(CC_REG_IOCFG2, sw1 ? IOCFG2_HIGH : IOCFG2_LOW);
}

static void radioSetup() {
  pinMode(CC_SD_CS, OUTPUT); digitalWrite(CC_SD_CS, HIGH);  // park SD off the bus
  pinMode(CC_CS,    OUTPUT); digitalWrite(CC_CS,    HIGH);
  pinMode(RF_SW0,   OUTPUT); digitalWrite(RF_SW0,   LOW);
  delay(10);
  ccSPI.begin(CC_SCK, CC_MISO, CC_MOSI);
  radioInitRC = radio.begin(TARGET_FREQ_MHZ);
  radio.setFrequency(TARGET_FREQ_MHZ);
  radio.setOOK(true);
  radio.setRxBandwidth((float)TARGET_RXBW_KHZ);
  radio.setFrequencyDeviation(5.2);
  radio.receiveDirectAsync();                   // async raw OOK on GDO0
  setBand(TARGET_FREQ_MHZ);                      // RF switch after async config
  nodeVer = (radioInitRC == RADIOLIB_ERR_NONE) ? 0x14 : 0;
}
static inline int16_t radioRssi() { return (int16_t)lroundf(radio.getRSSI()); }
static void radioRetune(float mhz) {
  radio.setFrequency(mhz);
  radio.receiveDirectAsync();
  setBand(mhz);
}

// ---- 240x135 display (M5Unified) -------------------------------------------
#define C_BG    0x0841
#define C_INK   0xE73C
#define C_DIM   0x6B4D
#define C_ACC   0x9C7F
#define C_GOLD  0xE5A6
#define C_OK    0x3EB3
M5Canvas cv(&M5.Display);

static const char* viewName() {
  return view == V_RSSI ? "SIGNAL" : view == V_SIGS ? "SIGS" : "DIAG";
}
void drawScreen() {
  if (!haveDisplay) return;
  int W = cv.width(), H = cv.height();
  cv.fillSprite(C_BG);
  // header
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(C_ACC, C_BG);  cv.drawString("SCARECROW", 6, 5);
  cv.setTextColor(C_GOLD, C_BG); cv.drawString("DARKWATCH", 6, 17);
  cv.setTextDatum(top_right);
  cv.setTextColor(C_INK, C_BG);
  char nid[10]; snprintf(nid, sizeof(nid), "NODE %c", NODE_ID);
  cv.drawString(nid, W - 6, 5);
  cv.setTextColor((nodeVer == 0x14) ? C_OK : C_GOLD, C_BG);
  cv.drawString(nodeVer == 0x14 ? "cc:ok" : "cc:--", W - 6, 17);
  cv.drawFastHLine(0, 30, W, C_DIM);

  if (view == V_RSSI) {
    cv.setTextDatum(middle_center); cv.setTextSize(4);
    cv.setTextColor(lastRssi > -70 ? C_OK : C_INK, C_BG);
    char rs[8]; snprintf(rs, sizeof(rs), "%d", lastRssi);
    cv.drawString(rs, W / 2, H / 2 - 2);
    cv.setTextSize(1); cv.setTextColor(C_DIM, C_BG);
    cv.drawString("dBm", W / 2, H / 2 + 22);
    int bw = W - 12, bx = 6, by = H - 34;
    float p = (lastRssi + 110) / 80.0f; if (p < 0) p = 0; if (p > 1) p = 1;
    cv.drawRoundRect(bx, by, bw, 9, 3, C_DIM);
    cv.fillRoundRect(bx, by, (int)(bw * p), 9, 3, C_GOLD);
  } else if (view == V_SIGS) {
    cv.setTextDatum(top_left); cv.setTextSize(1);
    int y = 36, shown = 0;
    for (int i = 0; i < NSIG && y < H - 12; i++) {
      if (!nsigs[i].used) continue;
      cv.setTextColor(nsigs[i].rssi > -70 ? C_OK : C_INK, C_BG);
      char l[40]; snprintf(l, sizeof(l), "te%u  x%lu  %ddBm",
        nsigs[i].te, (unsigned long)nsigs[i].count, nsigs[i].rssi);
      cv.drawString(l, 6, y); y += 13; shown++;
    }
    if (!shown) { cv.setTextColor(C_DIM, C_BG); cv.drawString("no bursts captured yet", 6, 40); }
  } else { // V_DIAG
    cv.setTextDatum(top_left); cv.setTextSize(1); cv.setTextColor(C_INK, C_BG);
    char l[40]; int y = 36;
    snprintf(l, sizeof(l), "band   %.2f MHz", watchMhz);      cv.drawString(l, 6, y); y += 13;
    snprintf(l, sizeof(l), "cc1101 ver 0x%02X", nodeVer);     cv.drawString(l, 6, y); y += 13;
    snprintf(l, sizeof(l), "esp-now %s  ch%d", espnowOk?"up":"--", COLLECTOR_CHANNEL); cv.drawString(l, 6, y); y += 13;
    snprintf(l, sizeof(l), "tx %lu  fail %lu", (unsigned long)txOk, (unsigned long)txFail); cv.drawString(l, 6, y); y += 13;
    snprintf(l, sizeof(l), "batt %d%%  bursts %lu", M5.Power.getBatteryLevel(), (unsigned long)burstCount); cv.drawString(l, 6, y);
  }
  // footer: view name + hint
  cv.setTextDatum(bottom_left); cv.setTextColor(C_OK, C_BG);
  cv.drawString("RX-ONLY", 6, H - 4);
  cv.setTextDatum(bottom_center); cv.setTextColor(C_GOLD, C_BG);
  cv.drawString(viewName(), W / 2, H - 4);
  cv.setTextDatum(bottom_right); cv.setTextColor(C_DIM, C_BG);
  cv.drawString(",/ or G0", W - 6, H - 4);
  cv.pushSprite(0, 0);
}
void cycleView() { view = (view + 1) % V_COUNT; drawScreen(); }

// ---- burst capture (GDO0 edge interrupt) -----------------------------------
volatile uint32_t edgeBuf[BURST_MAX_EDGES];
volatile uint16_t edgeCount = 0;
volatile uint32_t lastEdgeUs = 0;
void IRAM_ATTR onEdge() {
  uint32_t now = micros(), d = now - lastEdgeUs; lastEdgeUs = now;
  if (d < BURST_MIN_US) return;
  if (edgeCount < BURST_MAX_EDGES) edgeBuf[edgeCount++] = d;
}
uint16_t computeTe(uint32_t *a, uint16_t n) {
  uint16_t hist[40] = {0};
  for (uint16_t i = 1; i < n; i++) { uint32_t d = a[i]; if (d < 2000) hist[d/50]++; }
  uint16_t best = 2, bc = 0;
  for (uint16_t b = 2; b < 40; b++) if (hist[b] > bc) { bc = hist[b]; best = b; }
  return best * 50 + 25;
}

void dfSend(uint8_t kind, int16_t rssi, uint16_t te, uint16_t edges) {
  DfPacket p;
  p.magic = DF_MAGIC; p.node = NODE_ID; p.kind = kind;
  p.seq = seq++; p.t_ms = millis(); p.rssi = rssi;
  p.te = te; p.edges = edges; p.freq_x100 = (uint16_t)(watchMhz * 100);
  lastSendRC = esp_now_send(BCAST, (uint8_t*)&p, sizeof(p));
}

void onCfgRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(CfgPacket)) return;
  CfgPacket c; memcpy(&c, data, sizeof(c));
  if (c.magic != DF_MAGIC || c.kind != DF_KIND_CFG) return;
  if (c.watch_mhz < 280 || c.watch_mhz > 960) return;
  if (fabsf(c.watch_mhz - watchMhz) > 0.05f) { watchMhz = c.watch_mhz; radioRetune(watchMhz); }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);        // init board + TCA8418 keyboard
  pinMode(BTN_G0, INPUT_PULLUP);
  prefs.begin("node", false);
  hudRot = prefs.getUChar("rot", 1);
  haveDisplay = (M5.Display.width() > 0);
  if (haveDisplay) {
    M5.Display.setRotation(hudRot);
    M5.Display.setBrightness(150);
    splash();
    cv.createSprite(M5.Display.width(), M5.Display.height());
    drawScreen();
  }
  Serial.begin(115200);
  delay(150);

  radioSetup();
  Serial.printf("# cardputer-adv node %c initRC=%d ver=0x%02X (alive when 0x14)\n",
                NODE_ID, radioInitRC, nodeVer);

  pinMode(CC_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(CC_GDO0), onEdge, CHANGE);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(COLLECTOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
  espnowOk = (esp_now_init() == ESP_OK);
  esp_now_register_send_cb(onDfSent);
  esp_now_register_recv_cb(onCfgRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = COLLECTOR_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);
  if (haveDisplay) drawScreen();

  Serial.printf("{\"t\":\"nodeboot\",\"node\":\"%c\",\"freq\":%.2f,\"ch\":%d,"
                "\"cc1101_ver\":%u,\"espnow_init\":%d,\"tx\":false}\n",
                NODE_ID, TARGET_FREQ_MHZ, COLLECTOR_CHANNEL, nodeVer, espnowOk ? 1 : 0);
}

void reapBurst() {
  if (edgeCount == 0) return;
  if (micros() - lastEdgeUs < BURST_GAP_US) return;
  noInterrupts();
  uint16_t n = edgeCount;
  static uint32_t snap[BURST_MAX_EDGES];
  for (uint16_t i = 0; i < n; i++) snap[i] = edgeBuf[i];
  edgeCount = 0;
  interrupts();
  if (n < BURST_MIN_EDGES) return;
  lastTe = computeTe(snap, n);
  lastRssi = radioRssi();
  burstCount++;
  sigAdd(lastTe, lastRssi);
  dfSend(DF_KIND_BURST, lastRssi, lastTe, n);
}

// Darkwatch boot splash: radiating RF rings + crow mark + title, ~1.3s.
void splash() {
  if (!haveDisplay) return;
  auto &d = M5.Display;
  int W = d.width(), H = d.height();
  d.fillScreen(C_BG);
  int cx = W / 2, cy = H / 2 - 10;
  for (int rd = 5; rd < (H > W ? W : H); rd += 6) {       // radiating rings
    d.drawCircle(cx, cy, rd, rd < H / 3 ? C_GOLD : C_ACC);
    delay(20);
  }
  d.fillCircle(cx - 10, cy, 8, C_ACC);                    // crow mark
  d.fillTriangle(cx - 2, cy - 5, cx + 13, cy - 1, cx - 2, cy + 5, C_GOLD);
  d.fillCircle(cx - 12, cy - 2, 1, C_BG);
  d.setTextDatum(middle_center);
  d.setTextColor(C_INK, C_BG); d.setTextSize(2);
  d.drawString("SCARECROW", cx, cy + 32);
  d.setTextSize(1); d.setTextColor(C_GOLD, C_BG);
  char s[26]; snprintf(s, sizeof(s), "ANUBIS DARKWATCH  NODE %c", NODE_ID);
  d.drawString(s, cx, cy + 50);
  delay(550);
}

void cycleRotation() {
  if (!haveDisplay) return;
  hudRot = (hudRot + 1) & 3;
  prefs.putUChar("rot", hudRot);
  M5.Display.setRotation(hudRot);
  cv.deleteSprite();
  cv.createSprite(M5.Display.width(), M5.Display.height());
  drawScreen();
}

void loop() {
  M5Cardputer.update();
  // Keyboard: ',' = left (prev screen), '/' = right (next screen) — the Cardputer
  // arrow-key cluster. (';'/'.'' are up/down, reserved for future scrolling.)
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    for (auto c : M5Cardputer.Keyboard.keysState().word) {
      if (c == '/')      { view = (view + 1) % V_COUNT; drawScreen(); }
      else if (c == ',') { view = (view + V_COUNT - 1) % V_COUNT; drawScreen(); }
    }
  }
  // G0 button: short tap = next screen, hold (>900ms) = rotate + save
  static bool bPrev = false; static uint32_t bT0 = 0; static bool bLong = false;
  bool bNow = (digitalRead(BTN_G0) == LOW);
  if (bNow && !bPrev) { bT0 = millis(); bLong = false; }
  if (bNow && !bLong && millis() - bT0 > 900) { bLong = true; cycleRotation(); }
  if (!bNow && bPrev && !bLong) cycleView();
  bPrev = bNow;
  reapBurst();
  if (millis() - lastBeacon >= RSSI_BEACON_MS) {
    lastBeacon = millis();
    int16_t r = -200;
    for (uint8_t k = 0; k < RSSI_SAMPLES; k++) {
      int16_t s = radioRssi(); if (s > r) r = s;
      delayMicroseconds(300);
    }
    lastRssi = r;
    dfSend(DF_KIND_RSSI, r, 0, 0);
    static uint8_t diagN = 0;
    if (++diagN >= 10) { diagN = 0;
      Serial.printf("{\"t\":\"rssi\",\"node\":\"%c\",\"rssi\":%d,\"bursts\":%lu,\"ver\":%u}\n",
        NODE_ID, lastRssi, (unsigned long)burstCount, nodeVer);
    }
    drawScreen();
  }
}
