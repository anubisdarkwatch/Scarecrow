// ============================================================================
//  SCARECROW  —  receive-only sub-GHz sensor for Evil Crow RF V2
//  Module A hops a channel plan (occupancy + jammer detection).
//  Module B parks on a watch freq and captures raw burst fingerprints.
//  Output: NDJSON on Serial @115200 (+ optional SD mirror).
//
//  >>> There is intentionally NO transmit code. The radios are only ever
//  >>> placed in RX or IDLE. Do not add SetTx() — that is the whole point.
// ============================================================================
#include <Arduino.h>
#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include "config.h"
#if USE_SD
  #include <SPI.h>
  #include <SD.h>
  SPIClass sdSPI(VSPI);
  static bool sdOk = false;
  File logFile;
#endif
#include "webui.h"   // read-only WiFi dashboard (stubs out when USE_WEB=0)

// ------- module index helpers (dual-module fork uses setModul 0/1) ----------
#define MOD_A 0   // CC1 — scanner
#define MOD_B 1   // CC2 — listener

// ------- CC1101 status registers (read via SpiReadStatus; lib adds burst bit) -
#define REG_PARTNUM  0x30   // expect 0x00
#define REG_VERSION  0x31   // expect ~0x14 (nonzero, not 0xFF) if SPI is alive
#define REG_RSSI     0x34   // raw RSSI byte
#define REG_MARC     0x35   // MARCSTATE; 0x0D = RX
uint32_t lastSelfTest = 0;
uint32_t lastFox = 0;
float    watchFreq = WATCH_FREQ_MHZ;   // Module B park freq — runtime-tunable
void moduleSelfTest(byte modul, const char* name);   // defined after emit()
void setWatchFreq(float f);                          // defined after emit()
float getWatchFreq() { return watchFreq; }

// ---------------------------------------------------------------------------
//  Module B: raw edge capture via GPIO interrupt on GDO0 (async OOK data out)
// ---------------------------------------------------------------------------
volatile uint32_t edgeBuf[BURST_MAX_EDGES];
volatile uint16_t edgeCount = 0;
volatile uint32_t lastEdgeUs = 0;
volatile bool     burstOverflow = false;

void IRAM_ATTR onEdge() {
  uint32_t now = micros();
  uint32_t d   = now - lastEdgeUs;
  lastEdgeUs   = now;
  if (d < BURST_MIN_US) return;              // glitch filter
  if (edgeCount < BURST_MAX_EDGES) {
    edgeBuf[edgeCount++] = d;
  } else {
    burstOverflow = true;
  }
}

// ---------------------------------------------------------------------------
//  Scanner state
// ---------------------------------------------------------------------------
int16_t  floorDb[SCAN_PLAN_LEN];             // EWMA noise floor per channel
int16_t  lastRssi[SCAN_PLAN_LEN];            // most recent live RSSI per channel
bool     seeded = false;
uint8_t  jamStreak = 0;
uint32_t lastReport = 0;
uint32_t busyAccum[SCAN_PLAN_LEN];           // occupancy counter per channel
uint32_t sweeps = 0;

// ---------------------------------------------------------------------------
//  Output helpers
// ---------------------------------------------------------------------------
// ui=true also pushes the line to the live web dashboard. The per-channel
// "busy" firehose stays on serial/SD only; the UI gets curated events.
void emit(const String& line, bool ui = false) {
  Serial.println(line);
#if USE_SD
  if (sdOk) { logFile.println(line); logFile.flush(); }
#endif
  if (ui) webPush(line);
}

// Snapshot for the dashboard's initial load / reconnect.
String buildStateJson() {
  String s = String("{\"watch_mhz\":") + String(watchFreq, 2)
           + ",\"sweeps\":" + sweeps + ",\"channels\":[";
  for (int i = 0; i < SCAN_PLAN_LEN; i++) {
    if (i) s += ",";
    s += "{\"mhz\":\"" + String(SCAN_PLAN[i], 2) + "\",\"busy\":" + busyAccum[i]
       + ",\"floor\":" + floorDb[i] + ",\"rssi\":" + lastRssi[i] + "}";
  }
  s += "]}";
  return s;
}

// Read each module's identity + state directly. Decides between "SPI dead",
// "not in RX", and "RX fine but quiet" without any guesswork.
void moduleSelfTest(byte modul, const char* name) {
  ELECHOUSE_cc1101.setModul(modul);
  byte pn  = ELECHOUSE_cc1101.SpiReadStatus(REG_PARTNUM);
  byte ver = ELECHOUSE_cc1101.SpiReadStatus(REG_VERSION);
  byte ms  = ELECHOUSE_cc1101.SpiReadStatus(REG_MARC) & 0x1F;
  byte rr  = ELECHOUSE_cc1101.SpiReadStatus(REG_RSSI);
  emit(String("{\"t\":\"selftest\",\"mod\":\"") + name
     + "\",\"partnum\":" + pn + ",\"version\":" + ver
     + ",\"marcstate\":" + ms + ",\"rssi_raw\":" + rr
     + ",\"in_rx\":" + (ms == 0x0D ? "true" : "false") + "}", true);
}

// Retune Module B's park/fox frequency at runtime (from the web UI).
void setWatchFreq(float f) {
  if (f < 280.0 || f > 960.0) return;          // CC1101 sub-GHz sanity clamp
  watchFreq = f;
  noInterrupts(); edgeCount = 0; interrupts();  // drop any partial burst
  ELECHOUSE_cc1101.setModul(MOD_B);
  ELECHOUSE_cc1101.setMHZ(watchFreq);
  ELECHOUSE_cc1101.SetRx();
  ELECHOUSE_cc1101.setModul(MOD_A);
  emit(String("{\"t\":\"watch\",\"mhz\":") + String(watchFreq, 2) + "}", true);
}

// Fast RSSI on the fox/watch frequency, pushed to the UI only (not serial/SD).
void foxTick() {
  if (millis() - lastFox < FOX_MS) return;
  lastFox = millis();
  ELECHOUSE_cc1101.setModul(MOD_B);
  int16_t fr = -200;
  for (uint8_t k = 0; k < 4; k++) {            // brief peak-hold for stability
    int16_t r = ELECHOUSE_cc1101.getRssi();
    if (r > fr) fr = r;
    delayMicroseconds(200);
  }
  ELECHOUSE_cc1101.setModul(MOD_A);
  webPush(String("{\"t\":\"fox\",\"mhz\":") + String(watchFreq, 2)
        + ",\"rssi\":" + fr + "}");            // UI-only live stream
}

// ---------------------------------------------------------------------------
//  Radio bring-up — RX ONLY
// ---------------------------------------------------------------------------
void initScanner() {                          // Module A: RSSI sampling
  ELECHOUSE_cc1101.setModul(MOD_A);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);              // async free-running RX: RSSI tracks
                                              // live (packet mode latches it — the
                                              // v0.4 bug where MOD A froze at 209)
  ELECHOUSE_cc1101.setModulation(2);          // ASK/OOK (widest listen)
  ELECHOUSE_cc1101.setRxBW(SCAN_RXBW_KHZ);
  ELECHOUSE_cc1101.setMHZ(SCAN_PLAN[0]);
  ELECHOUSE_cc1101.SetRx();
}

void initListener() {                         // Module B: raw OOK data out
  ELECHOUSE_cc1101.setModul(MOD_B);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);              // async serial — data on GDO0
  ELECHOUSE_cc1101.setModulation(2);          // ASK/OOK
  ELECHOUSE_cc1101.setRxBW(WATCH_RXBW_KHZ);
  ELECHOUSE_cc1101.setMHZ(watchFreq);
  ELECHOUSE_cc1101.SetRx();
  pinMode(CC2_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(CC2_GDO0), onEdge, CHANGE);
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  // Register both CC1101s on the shared SPI bus (dual-module fork API).
  ELECHOUSE_cc1101.addSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC1_CS, MOD_A);
  ELECHOUSE_cc1101.addSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC2_CS, MOD_B);

#if USE_SD
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_SS);
  sdOk = SD.begin(SD_SS, sdSPI);
  if (sdOk) logFile = SD.open("/scarecrow.ndjson", FILE_APPEND);
#endif

  initScanner();
  initListener();
  webSetup();

  for (int i = 0; i < SCAN_PLAN_LEN; i++) { floorDb[i] = -110; lastRssi[i] = -127; busyAccum[i] = 0; }

  moduleSelfTest(MOD_A, "A");
  moduleSelfTest(MOD_B, "B");
  ELECHOUSE_cc1101.setModul(MOD_A);   // leave scanner selected

  emit(String("{\"t\":\"boot\",\"fw\":\"scarecrow-0.7\",\"tx\":false,")
     + "\"channels\":" + SCAN_PLAN_LEN
     + ",\"watch_mhz\":" + String(watchFreq, 2)
     + ",\"ap_ip\":\"" + webApIp() + "\""
     + ",\"sd\":" +
#if USE_SD
       String(sdOk ? "true" : "false")
#else
       "false"
#endif
     + "}", true);
}

// ---------------------------------------------------------------------------
//  Scanner sweep — read RSSI on every channel, update floor, flag busy/jam
// ---------------------------------------------------------------------------
void doSweep() {
  ELECHOUSE_cc1101.setModul(MOD_A);
  int hotThisSweep = 0;
  int16_t peak = -127; int peakCh = 0;

  for (int i = 0; i < SCAN_PLAN_LEN; i++) {
    ELECHOUSE_cc1101.setMHZ(SCAN_PLAN[i]);
    ELECHOUSE_cc1101.SetRx();
    delayMicroseconds(SCAN_SETTLE_US);          // let FS + RSSI settle after hop
    // Peak-hold: a single 6ms sample rarely overlaps a ~100ms transient burst.
    // Sampling repeatedly and keeping the max catches brief on-channel signals
    // between hops, so the scanner flags fob presses, not just steady emitters.
    int16_t rssi = -200;
    for (uint8_t k = 0; k < SCAN_SAMPLES; k++) {
      int16_t r = ELECHOUSE_cc1101.getRssi();
      if (r > rssi) rssi = r;
      delayMicroseconds(SCAN_SAMPLE_GAP_US);
    }
    lastRssi[i] = rssi;                         // live value for the dashboard

    if (!seeded) { floorDb[i] = rssi; }        // first pass seeds the floor
    bool busy = (rssi > floorDb[i] + BUSY_MARGIN_DB);

    if (busy) {
      hotThisSweep++;
      busyAccum[i]++;
      emit(String("{\"t\":\"busy\",\"mhz\":") + String(SCAN_PLAN[i], 2)
         + ",\"rssi\":" + rssi + ",\"floor\":" + floorDb[i] + "}");
    } else {
      // only adapt the floor on quiet reads, so a real signal can't drag it up
      floorDb[i] += (rssi - floorDb[i]) >> FLOOR_EWMA_SHIFT;
    }
    if (rssi > peak) { peak = rssi; peakCh = i; }
  }
  seeded = true;
  sweeps++;

  // Broadband jam heuristic: many channels hot at once, several sweeps running.
  if (hotThisSweep >= JAM_MIN_CHANNELS) {
    if (++jamStreak >= JAM_MIN_SWEEPS) {
      emit(String("{\"t\":\"alarm\",\"kind\":\"broadband_interference\",")
         + "\"hot\":" + hotThisSweep + ",\"peak_mhz\":" + String(SCAN_PLAN[peakCh], 2)
         + ",\"peak_rssi\":" + peak + "}", true);
      jamStreak = JAM_MIN_SWEEPS;              // latch; keep firing while present
    }
  } else {
    jamStreak = 0;
  }
}

// ---------------------------------------------------------------------------
//  Burst reaper — when Module B's edge train goes quiet, summarise it
// ---------------------------------------------------------------------------
void reapBurst() {
  if (edgeCount == 0) return;
  if (micros() - lastEdgeUs < BURST_GAP_US) return;   // still in progress

  noInterrupts();
  uint16_t n = edgeCount;
  bool ovf   = burstOverflow;
  static uint32_t snap[BURST_MAX_EDGES];
  for (uint16_t i = 0; i < n; i++) snap[i] = edgeBuf[i];
  edgeCount = 0; burstOverflow = false;
  interrupts();

  if (n < BURST_MIN_EDGES) return;            // too short — noise

  // Cheap timing fingerprint: total duration + a rolling hash of the pulse
  // train. Same remote pressed twice => same-ish hash. No decode, no store of
  // payload — just "a thing with this signature appeared".
  uint32_t dur = 0; uint32_t h = 2166136261UL;
  uint32_t shortest = 0xFFFFFFFF, longest = 0;
  for (uint16_t i = 1; i < n; i++) {           // skip snap[0]: idle gap before the
                                               // burst, not part of its duration
    uint32_t d = snap[i];
    dur += d;
    if (d < shortest) shortest = d;
    if (d > longest)  longest  = d;
    h = (h ^ (d & 0xFF)) * 16777619UL;        // FNV-1a over quantised widths
  }

  ELECHOUSE_cc1101.setModul(MOD_B);
  int16_t rssi = ELECHOUSE_cc1101.getRssi();

  emit(String("{\"t\":\"burst\",\"mhz\":") + String(watchFreq, 2)
     + ",\"rssi\":" + rssi
     + ",\"edges\":" + n
     + ",\"dur_us\":" + dur
     + ",\"min_us\":" + shortest + ",\"max_us\":" + longest
     + ",\"sig\":\"" + String(h, HEX) + "\""
     + ",\"ovf\":" + (ovf ? "true" : "false") + "}", true);
}

// ---------------------------------------------------------------------------
//  Periodic occupancy report
// ---------------------------------------------------------------------------
void maybeReport() {
  if (millis() - lastReport < SCAN_REPORT_MS) return;
  lastReport = millis();
  String s = String("{\"t\":\"sweep\",\"n\":") + sweeps + ",\"occ\":{";
  for (int i = 0; i < SCAN_PLAN_LEN; i++) {
    if (i) s += ",";
    s += "\"" + String(SCAN_PLAN[i], 2) + "\":" + busyAccum[i];
  }
  s += "},\"rssi\":{";
  for (int i = 0; i < SCAN_PLAN_LEN; i++) {    // live RSSI — should vary if RX is alive
    if (i) s += ",";
    s += "\"" + String(SCAN_PLAN[i], 2) + "\":" + lastRssi[i];
  }
  s += "}}";
  emit(s, true);
}

// ---------------------------------------------------------------------------
//  LOOP — scanner and listener run concurrently; listener is interrupt-driven
//  so a full sweep never blocks burst capture on the other radio.
// ---------------------------------------------------------------------------
void loop() {
  doSweep();        // ~ SCAN_PLAN_LEN * SCAN_DWELL_MS per call
  reapBurst();      // non-blocking; drains Module B's ISR buffer
  foxTick();        // fast RSSI on the fox freq -> UI
  maybeReport();

  if (millis() - lastSelfTest > 5000) {   // module health every 5s
    lastSelfTest = millis();
    moduleSelfTest(MOD_A, "A");
    moduleSelfTest(MOD_B, "B");
    ELECHOUSE_cc1101.setModul(MOD_A);
  }
}
