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
#if USE_DF
  #include <esp_now.h>
  #include <esp_wifi.h>
  #include "df_packet.h"
#endif

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
#if USE_DF
int16_t  lastFoxRssi = -120;           // EC's own watch RSSI, shared with DF
#endif
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
#if USE_DF
  cfgBroadcast();                 // push the new band to rover nodes right away
#endif
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
#if USE_DF
  lastFoxRssi = fr;                            // feed EC's own node reading
#endif
  webPush(String("{\"t\":\"fox\",\"mhz\":") + String(watchFreq, 2)
        + ",\"rssi\":" + fr + "}");            // UI-only live stream
}

// ===========================================================================
//  SIGNATURE LIBRARY
//  Recognise returning emitters and flag first-seen ones. The raw FNV sig is
//  jitter-sensitive (changes every press), so we fingerprint on `te` — the
//  base symbol time (most common pulse width), which is stable per remote —
//  and match with tolerance. Heuristic grouping by timing, not a unique ID:
//  remotes that share a base timing can land in one entry.
// ===========================================================================
#define SIG_MAX        48
#define SIG_TE_TOL_US  90     // two bursts are "same" if |te| within this

struct Sig {
  bool     used;
  uint16_t te;                 // base pulse width (us)
  uint16_t nmin, nmax;         // edge-count range seen
  uint32_t count;
  int16_t  lastRssi;
  uint32_t firstMs, lastMs;
  char     label[18];
};
Sig  sigs[SIG_MAX];
bool     sigsDirty = false;
uint32_t lastSigSave = 0;

// base symbol time = mode of the pulse-width histogram (50us buckets)
uint16_t computeTe(uint32_t *a, uint16_t n) {
  uint16_t hist[40] = {0};
  for (uint16_t i = 1; i < n; i++) { uint32_t d = a[i]; if (d < 2000) hist[d/50]++; }
  uint16_t best = 2, bc = 0;
  for (uint16_t b = 2; b < 40; b++) if (hist[b] > bc) { bc = hist[b]; best = b; }
  return best * 50 + 25;
}
int sigFind(uint16_t te) {
  for (int i = 0; i < SIG_MAX; i++)
    if (sigs[i].used && abs((int)te - (int)sigs[i].te) <= SIG_TE_TOL_US) return i;
  return -1;
}
int sigIngest(uint16_t te, uint16_t n, int16_t rssi, bool *isNew) {
  int id = sigFind(te);
  if (id < 0) {
    for (int i = 0; i < SIG_MAX; i++) if (!sigs[i].used) { id = i; break; }
    if (id < 0) return -1;                 // table full
    sigs[id] = Sig{}; sigs[id].used = true; sigs[id].te = te;
    sigs[id].nmin = n; sigs[id].nmax = n; sigs[id].firstMs = millis();
    *isNew = true;
  } else *isNew = false;
  sigs[id].count++; sigs[id].lastRssi = rssi; sigs[id].lastMs = millis();
  if (n < sigs[id].nmin) sigs[id].nmin = n;
  if (n > sigs[id].nmax) sigs[id].nmax = n;
  return id;
}

void sigsSave() {
  sigsDirty = false; lastSigSave = millis();
#if USE_SD
  if (!sdOk) return;
  SD.remove("/sigs.csv");
  File f = SD.open("/sigs.csv", FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < SIG_MAX; i++) if (sigs[i].used)
    f.printf("%u,%u,%u,%lu,%d,%s\n", sigs[i].te, sigs[i].nmin, sigs[i].nmax,
             (unsigned long)sigs[i].count, sigs[i].lastRssi, sigs[i].label);
  f.close();
#endif
}
void sigsLoad() {
#if USE_SD
  if (!sdOk) return;
  File f = SD.open("/sigs.csv", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n'); ln.trim(); if (!ln.length()) continue;
    int c1=ln.indexOf(','),c2=ln.indexOf(',',c1+1),c3=ln.indexOf(',',c2+1),
        c4=ln.indexOf(',',c3+1),c5=ln.indexOf(',',c4+1);
    if (c5<0) continue;
    for (int i=0;i<SIG_MAX;i++) if(!sigs[i].used){
      sigs[i]=Sig{}; sigs[i].used=true;
      sigs[i].te=ln.substring(0,c1).toInt();
      sigs[i].nmin=ln.substring(c1+1,c2).toInt();
      sigs[i].nmax=ln.substring(c2+1,c3).toInt();
      sigs[i].count=ln.substring(c3+1,c4).toInt();
      sigs[i].lastRssi=ln.substring(c4+1,c5).toInt();
      ln.substring(c5+1).toCharArray(sigs[i].label,sizeof(sigs[i].label));
      break;
    }
  }
  f.close();
#endif
}

String buildSigsJson() {
  String s = "{\"sigs\":[";
  bool first = true;
  for (int i = 0; i < SIG_MAX; i++) if (sigs[i].used) {
    if (!first) s += ","; first = false;
    s += "{\"id\":" + String(i) + ",\"te\":" + sigs[i].te
       + ",\"nmin\":" + sigs[i].nmin + ",\"nmax\":" + sigs[i].nmax
       + ",\"count\":" + sigs[i].count + ",\"rssi\":" + sigs[i].lastRssi
       + ",\"age\":" + ((millis() - sigs[i].lastMs) / 1000)
       + ",\"label\":\"" + String(sigs[i].label) + "\"}";
  }
  s += "]}"; return s;
}
void sigLabel(int id, String name) {
  if (id < 0 || id >= SIG_MAX || !sigs[id].used) return;
  String clean; for (uint16_t i=0;i<name.length() && clean.length()<17;i++){
    char ch=name[i]; if(isalnum(ch)||ch==' '||ch=='-'||ch=='_') clean+=ch; }
  clean.toCharArray(sigs[id].label, sizeof(sigs[id].label));
  sigsSave();
  webPush(String("{\"t\":\"sigupd\",\"id\":")+id+",\"label\":\""+sigs[id].label+"\"}");
}
void sigForget(int id) {
  if (id < 0 || id >= SIG_MAX) return;
  sigs[id].used = false; sigsSave();
  webPush(String("{\"t\":\"sigdel\",\"id\":")+id+"}");
}

// ===========================================================================
//  DIRECTION FINDING — collector for ESP-NOW rover nodes
//  Rovers broadcast {node, rssi, te} on the target freq. We rank them; hottest
//  node is nearest the source. The Evil Crow itself is node "0" (the anchor),
//  reported from its own Module-B watch RSSI.
// ===========================================================================
#if USE_DF
struct DfNode { bool used; char id; int16_t rssi; uint16_t te; uint32_t lastMs; uint32_t seq; bool dirty; };
DfNode dfNodes[8];
uint32_t lastDfTick = 0;
portMUX_TYPE dfMux = portMUX_INITIALIZER_UNLOCKED;

void onDfRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(DfPacket)) return;
  DfPacket p; memcpy(&p, data, sizeof(p));
  if (p.magic != DF_MAGIC) return;
  portENTER_CRITICAL(&dfMux);
  int idx = -1;
  for (int i = 0; i < 8; i++) if (dfNodes[i].used && dfNodes[i].id == p.node) { idx = i; break; }
  if (idx < 0) for (int i = 0; i < 8; i++) if (!dfNodes[i].used) { dfNodes[i].used = true; dfNodes[i].id = p.node; idx = i; break; }
  if (idx >= 0) {
    dfNodes[idx].rssi = p.rssi;
    if (p.kind == DF_KIND_BURST) dfNodes[idx].te = p.te;
    dfNodes[idx].lastMs = millis(); dfNodes[idx].seq = p.seq; dfNodes[idx].dirty = true;
  }
  portEXIT_CRITICAL(&dfMux);
}

String buildNodesJson() {
  String s = String("{\"target\":") + String(watchFreq, 2)
           + ",\"nodes\":[{\"id\":\"0\",\"self\":true,\"rssi\":" + lastFoxRssi + ",\"age\":0}";
  for (int i = 0; i < 8; i++) if (dfNodes[i].used)
    s += ",{\"id\":\"" + String(dfNodes[i].id) + "\",\"rssi\":" + dfNodes[i].rssi
       + ",\"te\":" + dfNodes[i].te + ",\"age\":" + ((millis() - dfNodes[i].lastMs) / 1000) + "}";
  s += "]}"; return s;
}

// Broadcast the current watch band so every rover node retunes to follow it.
void cfgBroadcast() {
  static uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  CfgPacket c; c.magic = DF_MAGIC; c.kind = DF_KIND_CFG; c.watch_mhz = watchFreq;
  esp_now_send(bcast, (uint8_t*)&c, sizeof(c));
}

uint32_t lastCfgBcast = 0;
void dfTick() {
  if (millis() - lastCfgBcast > 2000) { lastCfgBcast = millis(); cfgBroadcast(); }
  if (millis() - lastDfTick < 250) return;
  lastDfTick = millis();
  webPush(String("{\"t\":\"node\",\"id\":\"0\",\"self\":true,\"rssi\":") + lastFoxRssi + ",\"age\":0}");
  for (int i = 0; i < 8; i++) {
    bool d = false; int16_t rssi = 0; uint16_t te = 0; uint32_t age = 0; char id = 0;
    portENTER_CRITICAL(&dfMux);
    if (dfNodes[i].used && dfNodes[i].dirty) {
      d = true; id = dfNodes[i].id; rssi = dfNodes[i].rssi; te = dfNodes[i].te;
      age = (millis() - dfNodes[i].lastMs) / 1000; dfNodes[i].dirty = false;
    }
    portEXIT_CRITICAL(&dfMux);
    if (d) webPush(String("{\"t\":\"node\",\"id\":\"") + id + "\",\"rssi\":" + rssi
                 + ",\"te\":" + te + ",\"age\":" + age + "}");
  }
}
#endif

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
  sigsLoad();                 // restore saved signature library (if SD present)

  initScanner();
  initListener();
  webSetup();
#if USE_DF
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDfRecv);              // AP channel = AP_CHANNEL
    static uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, bcast, 6);
    peer.channel = AP_CHANNEL; peer.encrypt = false;
    esp_now_add_peer(&peer);                         // so we can push band config out
  }
#endif

  for (int i = 0; i < SCAN_PLAN_LEN; i++) { floorDb[i] = -110; lastRssi[i] = -127; busyAccum[i] = 0; }

  moduleSelfTest(MOD_A, "A");
  moduleSelfTest(MOD_B, "B");
  ELECHOUSE_cc1101.setModul(MOD_A);   // leave scanner selected

  emit(String("{\"t\":\"boot\",\"fw\":\"scarecrow-0.9-df\",\"tx\":false,")
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

  // --- signature library: fingerprint on base timing, flag first-seen ---
  uint16_t te = computeTe(snap, n);
  bool isNew = false;
  int id = sigIngest(te, n, rssi, &isNew);
  if (id >= 0) {
    sigsDirty = true;
    if (isNew)   // noteworthy -> serial/SD/UI
      emit(String("{\"t\":\"newsig\",\"id\":") + id + ",\"te\":" + te
         + ",\"edges\":" + n + ",\"rssi\":" + rssi + "}", true);
    else         // routine re-sighting -> UI only
      webPush(String("{\"t\":\"sig\",\"id\":") + id + ",\"count\":" + sigs[id].count
            + ",\"rssi\":" + rssi + ",\"te\":" + te + "}");
  }
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
  if (sigsDirty && millis() - lastSigSave > 3000) sigsSave();  // debounced persist
#if USE_DF
  dfTick();         // rank rover nodes -> LOCATE view
#endif

  if (millis() - lastSelfTest > 5000) {   // module health every 5s
    lastSelfTest = millis();
    moduleSelfTest(MOD_A, "A");
    moduleSelfTest(MOD_B, "B");
    ELECHOUSE_cc1101.setModul(MOD_A);
  }
}
