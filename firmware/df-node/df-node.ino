// ============================================================================
//  SCARECROW DF ROVER NODE
//  ESP32/S3 + one CC1101. Parks on the target frequency, reports RSSI (and
//  burst timing fingerprints) to the Evil Crow collector over ESP-NOW.
//  Receive-only: no SetTx anywhere.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include "node_config.h"
#include "df_packet.h"

static uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint32_t seq = 0, lastBeacon = 0;

// ---- burst capture (GDO0 edge interrupt, same approach as Scarecrow) -------
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
  Serial.begin(NODE_SERIAL_BAUD);

  // Radio: single CC1101 in async OOK RX on the target frequency.
  ELECHOUSE_cc1101.addSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC_CS, 0);
  ELECHOUSE_cc1101.setModul(0);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);        // async free-running -> live RSSI
  ELECHOUSE_cc1101.setModulation(2);    // ASK/OOK
  ELECHOUSE_cc1101.setRxBW(TARGET_RXBW_KHZ);
  ELECHOUSE_cc1101.setMHZ(TARGET_FREQ_MHZ);
  ELECHOUSE_cc1101.SetRx();
  pinMode(CC_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(CC_GDO0), onEdge, CHANGE);

  // ESP-NOW on the collector's channel (STA mode, no AP association).
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(COLLECTOR_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) { Serial.println("esp_now init failed"); }
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
  uint16_t te = computeTe(snap, n);
  int16_t rssi = ELECHOUSE_cc1101.getRssi();
  dfSend(DF_KIND_BURST, rssi, te, n);
  Serial.printf("{\"t\":\"burst\",\"node\":\"%c\",\"te\":%u,\"edges\":%u,\"rssi\":%d}\n",
                NODE_ID, te, n, rssi);
}

void loop() {
  reapBurst();
  if (millis() - lastBeacon >= RSSI_BEACON_MS) {
    lastBeacon = millis();
    int16_t r = -200;
    for (uint8_t k = 0; k < RSSI_SAMPLES; k++) {   // peak-hold
      int16_t s = ELECHOUSE_cc1101.getRssi();
      if (s > r) r = s;
      delayMicroseconds(300);
    }
    dfSend(DF_KIND_RSSI, r, 0, 0);
  }
}
