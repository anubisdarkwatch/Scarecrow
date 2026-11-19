// ============================================================================
//  SCARECROW DF ROVER NODE
//  An ESP32 + one CC1101. Parks on the target frequency, reports RSSI (and
//  burst-timing fingerprints) to the Evil Crow collector over ESP-NOW.
//  Receive-only: there is no transmit call anywhere in this firmware.
//
//  Radio backend is chosen per board in node_config.h:
//    * ELECHOUSE fork  -> classic ESP32 boards (StickS3 RF Pack, bare wiring)
//    * RadioLib        -> LilyGo T-Embed CC1101 (ESP32-S3, onboard CC1101)
//  On the M5StickS3 it also drives the 135x240 display via M5Unified; every
//  other board stays headless for this radio-first bring-up.
// ============================================================================
#include "node_config.h"

// Headless mode: no onboard display, so the CC1101 gets the HSPI bus to itself
// (the factory-proven wiring). The CYD HUD is the display now. Comment out to
// bring the T-Embed's own screen back (shared-bus build).
#define TEMBED_HEADLESS 1

#if BOARD == BOARD_STICKS3_RFPACK
  #define USE_M5 1
  #include <M5Unified.h>
#else
  #define USE_M5 0
#endif
#if BOARD == BOARD_TEMBED_CC1101 && !defined(TEMBED_HEADLESS)
  #define USE_TFT 1                   // onboard ST7789V, shares HSPI with the CC1101
  #include <TFT_eSPI.h>
#else
  #define USE_TFT 0
#endif
#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#if RADIO_DRIVER == RADIO_DRIVER_RADIOLIB
  #include <RadioLib.h>
#else
  #include "ELECHOUSE_CC1101_SRC_DRV.h"
#endif
#include "df_packet.h"

static uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint32_t seq = 0, lastBeacon = 0;
float    watchMhz = TARGET_FREQ_MHZ;  // runtime band — follows the collector
int16_t  lastRssi = -120;
uint16_t lastTe = 0;
uint32_t burstCount = 0;
uint8_t  nodeVer = 0;                 // 0x14 once the CC1101 answers; 0 = dead
int      radioInitRC = -999;          // raw driver init return (0 = OK on RadioLib)
volatile uint32_t txOk = 0, txFail = 0;
int      lastSendRC = -1;             // last esp_now_send() return (0 = ESP_OK)
bool     espnowOk = false;

void onDfSent(const wifi_tx_info_t *info, esp_now_send_status_t st) {
  if (st == ESP_NOW_SEND_SUCCESS) txOk++; else txFail++;
}

// ---- radio abstraction -----------------------------------------------------
// Both backends expose the same three things: radioSetup(), radioRssi() (live
// dBm), and nodeVer/radioInitRC for the self-test readout.
#if RADIO_DRIVER == RADIO_DRIVER_RADIOLIB
#if USE_TFT
// Display owns HSPI, so the radio shares the DEFAULT (FSPI) bus.
static CC1101 radio = new Module(CC_CS, CC_GDO0, RADIOLIB_NC, CC_GDO2);
#define RADIO_SPI SPI
#else
// Headless: give the CC1101 its own HSPI bus, exactly like LilyGo's factory
// cc1101 example (SPIClass(HSPI)) — the configuration proven to receive here.
static SPIClass ccSPI(HSPI);
static CC1101 radio = new Module(CC_CS, CC_GDO0, RADIOLIB_NC, CC_GDO2, ccSPI);
#define RADIO_SPI ccSPI
#endif

static void radioSetup() {
#ifdef CC_PWR_EN
  pinMode(CC_PWR_EN, OUTPUT); digitalWrite(CC_PWR_EN, HIGH);  // power the CC1101
#endif
  // Shared SPI bus: park every rival chip-select HIGH before we talk to the radio.
#ifdef CC_DISPLAY_CS
  pinMode(CC_DISPLAY_CS, OUTPUT); digitalWrite(CC_DISPLAY_CS, HIGH);
#endif
#ifdef CC_SD_CS
  pinMode(CC_SD_CS, OUTPUT); digitalWrite(CC_SD_CS, HIGH);
#endif
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);
#ifdef CC_SW0
  // Antenna RF switch -> 434 MHz path (SW1=HIGH, SW0=HIGH per LilyGo factory fw).
  pinMode(CC_SW1, OUTPUT); pinMode(CC_SW0, OUTPUT);
  digitalWrite(CC_SW1, HIGH); digitalWrite(CC_SW0, HIGH);
#endif
  delay(10);
  RADIO_SPI.begin(CC_SCK, CC_MISO, CC_MOSI);
  radioInitRC = radio.begin(TARGET_FREQ_MHZ);      // checks VERSION reg internally
  radio.setFrequency(TARGET_FREQ_MHZ);             // force carrier to 433.92
  radio.setOOK(true);                              // OOK/ASK, like the fobs we hunt
  radio.setRxBandwidth((uint32_t)TARGET_RXBW_KHZ);
  radio.setFrequencyDeviation(5.2);
  radio.receiveDirectAsync();                      // async raw OOK: GDO0 = data envelope
  nodeVer = (radioInitRC == RADIOLIB_ERR_NONE) ? 0x14 : 0;
}
static inline int16_t radioRssi() { return (int16_t)lroundf(radio.getRSSI()); }
static void radioRetune(float mhz) {
#ifdef CC_SW0
  // T-Embed antenna RF switch: 315 / 434 / 868-915 band paths.
  if (mhz < 350)      { digitalWrite(CC_SW1, HIGH); digitalWrite(CC_SW0, LOW);  }
  else if (mhz < 600) { digitalWrite(CC_SW1, HIGH); digitalWrite(CC_SW0, HIGH); }
  else                { digitalWrite(CC_SW1, LOW);  digitalWrite(CC_SW0, HIGH); }
#endif
  radio.setFrequency(mhz);
  radio.receiveDirectAsync();
}

#else  // ---- ELECHOUSE fork (classic ESP32) ----
static void radioSetup() {
#if HAS_SN74_BUS
  // RF Pack S3: deassert rival CS so the SN74 routes the CC1101's MISO to us.
  pinMode(BUS_NRF_CS, OUTPUT); digitalWrite(BUS_NRF_CS, HIGH);
  pinMode(BUS_SD_CS,  OUTPUT); digitalWrite(BUS_SD_CS,  HIGH);
#endif
  ELECHOUSE_cc1101.addSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC_CS, 0);
  ELECHOUSE_cc1101.setModul(0);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);                   // async free-running
  ELECHOUSE_cc1101.setModulation(2);               // ASK/OOK
  ELECHOUSE_cc1101.setRxBW(TARGET_RXBW_KHZ);
  ELECHOUSE_cc1101.setMHZ(TARGET_FREQ_MHZ);
  ELECHOUSE_cc1101.SetRx();
  nodeVer = ELECHOUSE_cc1101.SpiReadStatus(0x31);  // VERSION: proves SPI alive
  radioInitRC = nodeVer;
}
static inline int16_t radioRssi() { return (int16_t)ELECHOUSE_cc1101.getRssi(); }
static void radioRetune(float mhz) {
  ELECHOUSE_cc1101.setMHZ(mhz);
  ELECHOUSE_cc1101.SetRx();
}
#endif

// ---- T-Embed onboard display (TFT_eSPI, ST7789V 320x170 on HSPI) -----------
#if USE_TFT
TFT_eSPI tft = TFT_eSPI();
#define T_BG   0x0841
#define T_INK  0xE73C
#define T_DIM  0x6B4D
#define T_ACC  0x9C7F
#define T_GOLD 0xE5A6
#define T_OK   0x3EB3
static int TW = 320, TH = 170;

void tftChrome() {                      // static frame, drawn once
  tft.fillScreen(T_BG);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(2);
  tft.setTextColor(T_ACC, T_BG);  tft.drawString("SCARECROW", 8, 6);
  tft.setTextColor(T_GOLD, T_BG); tft.drawString("DARKWATCH", 8, 24);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(T_INK, T_BG);
  char nid[10]; snprintf(nid, sizeof(nid), "NODE %c", NODE_ID);
  tft.drawString(nid, TW - 8, 6);
  uint16_t vcol = (nodeVer == 0x14) ? T_OK : T_GOLD;
  tft.setTextColor(vcol, T_BG);
  char st[24]; snprintf(st, sizeof(st), "cc:%s  now:%s",
    nodeVer == 0x14 ? "ok" : "--", espnowOk ? "ok" : "--");
  tft.drawString(st, TW - 8, 26);
  tft.setTextDatum(BL_DATUM); tft.setTextFont(1); tft.setTextColor(T_OK, T_BG);
  tft.drawString("RX-ONLY - no transmit path", 8, TH - 6);
  tft.setTextDatum(BR_DATUM); tft.setTextColor(T_DIM, T_BG);
  char ft[24]; snprintf(ft, sizeof(ft), "%.2f MHz  ch%d", TARGET_FREQ_MHZ, COLLECTOR_CHANNEL);
  tft.drawString(ft, TW - 8, TH - 6);
}

void tftRssi() {                        // dynamic region, redrawn each beacon
  tft.fillRect(0, 44, TW, 66, T_BG);
  tft.setTextDatum(MC_DATUM); tft.setTextFont(7);
  tft.setTextColor(lastRssi > -70 ? T_OK : T_INK, T_BG);
  char rs[8]; snprintf(rs, sizeof(rs), "%d", lastRssi);
  tft.drawString(rs, TW / 2, 74);
  tft.setTextFont(2); tft.setTextColor(T_DIM, T_BG);
  char sub[32]; snprintf(sub, sizeof(sub), "dBm    bursts %lu", (unsigned long)burstCount);
  tft.drawString(sub, TW / 2, 104);   // diag: bursts = raw OOK pulses heard on GDO0
  int bx = 16, by = 126, bw = TW - 32, bh = 16;
  float p = (lastRssi + 110) / 80.0f; if (p < 0) p = 0; if (p > 1) p = 1;
  int fillw = (int)((bw - 4) * p);
  tft.drawRoundRect(bx, by, bw, bh, 4, T_DIM);
  tft.fillRect(bx + 2, by + 2, bw - 4, bh - 4, T_BG);     // clear old fill
  if (fillw > 0) tft.fillRoundRect(bx + 2, by + 2, fillw, bh - 4, 3, T_GOLD);
}

void tftSetup() {
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);   // park radio CS off the bus
  tft.init();
  tft.setRotation(3);                                  // landscape 320x170
  TW = tft.width(); TH = tft.height();
  pinMode(CC_PWR_EN, OUTPUT); digitalWrite(CC_PWR_EN, HIGH);
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);         // backlight
  tftChrome();
  tftRssi();
}
#endif

#if USE_M5
M5Canvas cv(&M5.Display);
// Anubis Darkwatch palette (RGB565)
#define C_BG    0x0841
#define C_INK   0xE73C
#define C_DIM   0x6B4D
#define C_ACC   0x9C7F
#define C_GOLD  0xE5A6
#define C_OK    0x3EB3
void drawScreen() {
  int W = cv.width(), H = cv.height();
  cv.fillSprite(C_BG);
  cv.setTextColor(C_ACC, C_BG); cv.setTextDatum(top_left);
  cv.setTextSize(1); cv.drawString("SCARECROW", 6, 6);
  cv.setTextColor(C_GOLD, C_BG); cv.drawString("DARKWATCH", 6, 18);
  cv.setTextColor(C_INK, C_BG); cv.setTextSize(2);
  char nid[10]; snprintf(nid, sizeof(nid), "NODE %c", NODE_ID);
  cv.drawString(nid, 6, 38);
  cv.setTextDatum(middle_center); cv.setTextSize(4);
  cv.setTextColor(lastRssi > -70 ? C_OK : C_INK, C_BG);
  char rs[8]; snprintf(rs, sizeof(rs), "%d", lastRssi);
  cv.drawString(rs, W/2, H/2 - 4);
  cv.setTextSize(1); cv.setTextColor(C_DIM, C_BG);
  cv.drawString("dBm", W/2, H/2 + 24);
  int bw = W - 12, bx = 6, by = H/2 + 42;
  float p = (lastRssi + 110) / 80.0f; if (p < 0) p = 0; if (p > 1) p = 1;
  cv.drawRoundRect(bx, by, bw, 10, 3, C_DIM);
  cv.fillRoundRect(bx, by, (int)(bw * p), 10, 3, C_GOLD);
  cv.setTextDatum(bottom_left); cv.setTextColor(C_OK, C_BG);
  cv.drawString("RX-ONLY", 6, H - 20);
  cv.setTextColor(C_DIM, C_BG); cv.setTextDatum(bottom_right);
  char ft[22]; snprintf(ft, sizeof(ft), "%.2f ch%d", TARGET_FREQ_MHZ, COLLECTOR_CHANNEL);
  cv.drawString(ft, W - 6, H - 20);
  cv.setTextDatum(bottom_left);
  uint16_t vcol = (nodeVer > 0 && nodeVer < 255) ? C_OK : C_GOLD;
  cv.setTextColor(vcol, C_BG);
  char bl[30]; snprintf(bl, sizeof(bl), "v%u e%d s%d tx%lu b%d%%",
    nodeVer, espnowOk ? 1 : 0, lastSendRC, (unsigned long)txOk, M5.Power.getBatteryLevel());
  cv.drawString(bl, 6, H - 6);
  cv.pushSprite(0, 0);
}
#endif

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

// Collector pushes the band to follow; retune the CC1101 to match.
void onCfgRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(CfgPacket)) return;
  CfgPacket c; memcpy(&c, data, sizeof(c));
  if (c.magic != DF_MAGIC || c.kind != DF_KIND_CFG) return;
  if (c.watch_mhz < 280 || c.watch_mhz > 960) return;
  if (fabsf(c.watch_mhz - watchMhz) > 0.05f) { watchMhz = c.watch_mhz; radioRetune(watchMhz); }
}

void setup() {
#if USE_M5
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Power.setExtOutput(true);
  delay(50);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(140);
  cv.createSprite(M5.Display.width(), M5.Display.height());
  drawScreen();
#endif
  Serial.begin(NODE_SERIAL_BAUD);
  delay(200);

  radioSetup();                        // backend-specific CC1101 bring-up
  Serial.printf("# node %c board=%d driver=%s\n", NODE_ID, BOARD,
    (RADIO_DRIVER == RADIO_DRIVER_RADIOLIB) ? "RadioLib" : "ELECHOUSE");
  Serial.printf("# CC1101 pins SCK=%d MISO=%d MOSI=%d CS=%d GDO0=%d\n",
    CC_SCK, CC_MISO, CC_MOSI, CC_CS, CC_GDO0);
  Serial.printf("# CC1101 initRC=%d ver=0x%02X (alive when ver=0x14)\n",
    radioInitRC, nodeVer);

  pinMode(CC_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(CC_GDO0), onEdge, CHANGE);

  // ESP-NOW — set channel BEFORE init, broadcast peer on the collector's channel.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(COLLECTOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
  espnowOk = (esp_now_init() == ESP_OK);
  if (!espnowOk) Serial.println("# esp_now init failed");
  esp_now_register_send_cb(onDfSent);
  esp_now_register_recv_cb(onCfgRecv);               // follow the collector's band
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = COLLECTOR_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);

#if USE_TFT
  tftSetup();                          // bring up the panel last; it shares HSPI
#endif

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
  dfSend(DF_KIND_BURST, lastRssi, lastTe, n);
  Serial.printf("{\"t\":\"burst\",\"node\":\"%c\",\"te\":%u,\"edges\":%u,\"rssi\":%d}\n",
                NODE_ID, lastTe, n, lastRssi);
}

void loop() {
#if USE_M5
  M5.update();
#endif
  reapBurst();
  if (millis() - lastBeacon >= RSSI_BEACON_MS) {
    lastBeacon = millis();
    int16_t r = -200;
    for (uint8_t k = 0; k < RSSI_SAMPLES; k++) {
      int16_t s = radioRssi();
      if (s > r) r = s;
      delayMicroseconds(300);
    }
    lastRssi = r;
    dfSend(DF_KIND_RSSI, r, 0, 0);
    static uint8_t diagN = 0;                       // ~1 Hz serial diagnostic
    if (++diagN >= 10) { diagN = 0;
      Serial.printf("{\"t\":\"rssi\",\"node\":\"%c\",\"rssi\":%d,\"bursts\":%lu,\"ver\":%u}\n",
        NODE_ID, lastRssi, (unsigned long)burstCount, nodeVer);
    }
#if USE_M5
    drawScreen();
#endif
#if USE_TFT
    tftRssi();
#endif
  }
}
