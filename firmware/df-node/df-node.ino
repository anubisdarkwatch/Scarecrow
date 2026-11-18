// ============================================================================
//  SCARECROW DF ROVER NODE
//  ESP32/S3 + one CC1101. Parks on the target frequency, reports RSSI (and
//  burst timing fingerprints) to the Evil Crow collector over ESP-NOW.
//  Receive-only: no SetTx anywhere.
//
//  On the M5StickS3 (BOARD_STICKS3_RFPACK) it also drives the 135x240 display
//  via M5Unified so the rover shows its own live RSSI. Generic ESP32 builds
//  stay headless (no M5 dependency).
// ============================================================================
#include "node_config.h"
#if BOARD == BOARD_STICKS3_RFPACK
  #define USE_M5 1
  #include <M5Unified.h>
#else
  #define USE_M5 0
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include "df_packet.h"

static uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint32_t seq = 0, lastBeacon = 0;
int16_t  lastRssi = -120;
uint16_t lastTe = 0;
uint32_t burstCount = 0;

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
  // node id
  cv.setTextColor(C_INK, C_BG); cv.setTextSize(2);
  char nid[10]; snprintf(nid, sizeof(nid), "NODE %c", NODE_ID);
  cv.drawString(nid, 6, 38);
  // big RSSI
  cv.setTextDatum(middle_center); cv.setTextSize(4);
  cv.setTextColor(lastRssi > -70 ? C_OK : C_INK, C_BG);
  char rs[8]; snprintf(rs, sizeof(rs), "%d", lastRssi);
  cv.drawString(rs, W/2, H/2 - 4);
  cv.setTextSize(1); cv.setTextColor(C_DIM, C_BG);
  cv.drawString("dBm", W/2, H/2 + 24);
  // strength bar
  int bw = W - 12, bx = 6, by = H/2 + 42;
  float p = (lastRssi + 110) / 80.0f; if (p < 0) p = 0; if (p > 1) p = 1;
  cv.drawRoundRect(bx, by, bw, 10, 3, C_DIM);
  cv.fillRoundRect(bx, by, (int)(bw * p), 10, 3, C_GOLD);
  // footer
  cv.setTextDatum(bottom_left); cv.setTextColor(C_OK, C_BG);
  cv.drawString("RX-ONLY", 6, H - 20);
  cv.setTextColor(C_DIM, C_BG); cv.setTextDatum(bottom_right);
  char ft[22]; snprintf(ft, sizeof(ft), "%.2f ch%d", TARGET_FREQ_MHZ, COLLECTOR_CHANNEL);
  cv.drawString(ft, W - 6, H - 20);
  cv.setTextDatum(bottom_left);
  char bl[22]; snprintf(bl, sizeof(bl), "bursts %lu  bat %d%%", (unsigned long)burstCount, M5.Power.getBatteryLevel());
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
  p.te = te; p.edges = edges; p.freq_x100 = TARGET_FREQ_X100;
  esp_now_send(BCAST, (uint8_t*)&p, sizeof(p));
}

void setup() {
#if USE_M5
  auto cfg = M5.config();
  M5.begin(cfg);                        // powers board + display (handles PMU/BL)
  M5.Display.setRotation(0);
  M5.Display.setBrightness(140);
  cv.createSprite(M5.Display.width(), M5.Display.height());
  drawScreen();                         // show immediately -> proof of life
#endif
  Serial.begin(NODE_SERIAL_BAUD);

  // Radio: single CC1101 in async OOK RX on the target frequency.
  ELECHOUSE_cc1101.addSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC_CS, 0);
  ELECHOUSE_cc1101.setModul(0);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setRxBW(TARGET_RXBW_KHZ);
  ELECHOUSE_cc1101.setMHZ(TARGET_FREQ_MHZ);
  ELECHOUSE_cc1101.SetRx();
  pinMode(CC_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(CC_GDO0), onEdge, CHANGE);

  // ESP-NOW on the collector's channel.
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(COLLECTOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) Serial.println("esp_now init failed");
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = COLLECTOR_CHANNEL; peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.printf("{\"t\":\"nodeboot\",\"node\":\"%c\",\"freq\":%.2f,\"ch\":%d,\"tx\":false}\n",
                NODE_ID, TARGET_FREQ_MHZ, COLLECTOR_CHANNEL);
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
  lastRssi = ELECHOUSE_cc1101.getRssi();
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
      int16_t s = ELECHOUSE_cc1101.getRssi();
      if (s > r) r = s;
      delayMicroseconds(300);
    }
    lastRssi = r;
    dfSend(DF_KIND_RSSI, r, 0, 0);
#if USE_M5
    drawScreen();
#endif
  }
}
