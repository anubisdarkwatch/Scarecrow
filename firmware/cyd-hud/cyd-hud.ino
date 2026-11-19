// ============================================================================
//  SCARECROW HUD  —  Cheap Yellow Display dashboard (ESP32-2432S024 / S028R)
//  Receive-only companion screen for the Evil Crow "Scarecrow" collector.
//  Joins the Scarecrow Wi-Fi AP and renders two views off the collector's JSON:
//    * LOCATE     — anchor + rover nodes, heat-coloured RSSI bars
//    * SIGNATURES — the live signature library (/sigs.json)
//  Switch views by tapping the screen; it also auto-cycles if left alone.
//
//  Display: TFT_eSPI (panel config via -D build flags: see build command).
//  Touch:   XPT2046 PENIRQ on GPIO36 — a tap toggles the view (no SPI needed).
// ============================================================================
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static TFT_eSPI tft = TFT_eSPI();

// ---- Match the collector's config.h ----------------------------------------
static const char* AP_SSID = "Scarecrow";
static const char* AP_PASS = "watchonly";
static const char* BASE    = "http://192.168.4.1";
#define TOUCH_IRQ 36          // XPT2046 PENIRQ: LOW while the panel is touched

// ---- Anubis Darkwatch palette (RGB565) -------------------------------------
#define C_BG    0x0820        // near-black navy
#define C_PANEL 0x10A3        // raised row panel
#define C_INK   0xF79E        // bright text
#define C_DIM   0x6B6D        // muted label
#define C_TEAL  0x2E7F        // accent
#define C_GOLD  0xFE62        // Darkwatch gold
#define C_CRIM  0xF9A6        // hot crimson
#define C_OK    0x3EB3
#define C_WARN  0xFB80

static int SW = 320, SH = 240;

// ---- data ------------------------------------------------------------------
struct Node { char label[14]; int rssi; int age; bool self; };
static Node nodes[9];
static int  nNodes = 0;
static float targetMhz = 433.92f;
static long  sweeps = 0;
static char  hotCh[24] = "scanning";

struct Sig { int id; int te; int count; int rssi; int age; char label[18]; };
static Sig  sigs[12];
static int  nSigs = 0;

static bool wifiOk = false;
enum View { V_LOCATE = 0, V_SIGS = 1 };
static View view = V_LOCATE;
static uint32_t lastPoll = 0, lastSigs = 0, lastState = 0, lastWifiTry = 0;
static uint32_t lastTouch = 0, lastAutoFlip = 0, lastChrome = 0;
static bool touchPrev = false, lastWifi = false;

// ---- colour helpers --------------------------------------------------------
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// RSSI -> heat colour: cold teal (weak) -> gold -> crimson (strong)
static uint16_t heat(float p) {
  if (p < 0) p = 0; if (p > 1) p = 1;
  if (p < 0.5f) { float t = p / 0.5f;  return rgb(46 + t * 200, 127 + t * 70, 127 - t * 90); }
  float t = (p - 0.5f) / 0.5f;          return rgb(246, 197 - t * 150, 37 + t * 20);
}
static float rssiP(int rssi) { float p = (rssi + 110) / 80.0f; return p < 0 ? 0 : (p > 1 ? 1 : p); }

// ---- chrome ----------------------------------------------------------------
void drawCrow(int x, int y) {               // tiny stylized crow mark
  tft.fillCircle(x + 6, y + 6, 6, C_TEAL);           // head
  tft.fillTriangle(x + 11, y + 4, x + 20, y + 6, x + 11, y + 9, C_GOLD); // beak
  tft.fillCircle(x + 5, y + 5, 1, C_BG);             // eye
}

void drawHeader() {
  tft.fillRect(0, 0, SW, 42, C_BG);
  drawCrow(8, 6);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_INK, C_BG); tft.drawString("SCARECROW", 34, 2);
  tft.setTextFont(2);
  tft.setTextColor(C_GOLD, C_BG); tft.drawString("DARKWATCH", 36, 25);
  // view tag, top-right
  tft.setTextDatum(TR_DATUM); tft.setTextFont(2);
  tft.setTextColor(wifiOk ? C_OK : C_WARN, C_BG);
  tft.drawString(wifiOk ? (view == V_LOCATE ? "LOCATE" : "SIGS") : "LINKING", SW - 10, 6);
  tft.setTextColor(C_DIM, C_BG);
  char l[36]; snprintf(l, sizeof(l), "%.2f MHz", targetMhz);
  tft.drawString(l, SW - 10, 24);
  // accent rule
  for (int x = 0; x < SW; x++) {
    uint16_t c = heat((float)x / SW);
    tft.drawFastVLine(x, 42, 2, c);
  }
}

void drawFooter() {
  tft.fillRect(0, SH - 16, SW, 16, C_BG);
  tft.setTextFont(2); tft.setTextDatum(BL_DATUM);
  tft.setTextColor(C_OK, C_BG);  tft.drawString("RX-ONLY", 10, SH - 1);
  tft.setTextDatum(BC_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString(view == V_LOCATE ? "o  -" : "-  o", SW / 2, SH - 1);   // view dots
  tft.setTextDatum(BR_DATUM); tft.setTextColor(C_DIM, C_BG);
  char r[28];
  if (view == V_LOCATE) snprintf(r, sizeof(r), "sweeps %ld", sweeps);
  else                  snprintf(r, sizeof(r), "%d sigs", nSigs);
  tft.drawString(r, SW - 10, SH - 1);
}

// ---- views -----------------------------------------------------------------
void drawLocate() {
  int top = 50, rowH = 34, bx = 12, bw = SW - 24;
  tft.fillRect(0, top, SW, SH - top - 16, C_BG);
  if (nNodes == 0) {
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("waiting for nodes...", SW / 2, top + 30);
    return;
  }
  for (int i = 0; i < nNodes && i < 5; i++) {
    int y = top + i * rowH;
    tft.fillRoundRect(bx - 4, y, bw + 8, rowH - 6, 4, C_PANEL);
    float p = rssiP(nodes[i].rssi);
    uint16_t hc = heat(p);
    tft.setTextFont(2); tft.setTextDatum(TL_DATUM);
    tft.setTextColor(nodes[i].self ? C_GOLD : C_INK, C_PANEL);
    tft.drawString(nodes[i].label, bx + 4, y + 3);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(hc, C_PANEL);
    char rs[12]; snprintf(rs, sizeof(rs), "%d dBm", nodes[i].rssi);
    tft.drawString(rs, bx + bw - 4, y + 3);
    int by = y + 20, bh = 7, iw = bw - 8;
    tft.fillRoundRect(bx + 4, by, iw, bh, 2, 0x2124);
    int fw = (int)(iw * p);
    if (fw > 0) tft.fillRoundRect(bx + 4, by, fw, bh, 2, hc);
    if (nodes[i].age > 5 && !nodes[i].self) {
      tft.setTextFont(1); tft.setTextDatum(TR_DATUM); tft.setTextColor(C_WARN, C_PANEL);
      tft.drawString("stale", bx + bw - 70, y + 6);
    }
  }
}

void drawSigs() {
  int top = 50, rowH = 30, bx = 12, bw = SW - 24;
  tft.fillRect(0, top, SW, SH - top - 16, C_BG);
  if (nSigs == 0) {
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("no signatures captured yet", SW / 2, top + 30);
    return;
  }
  for (int i = 0; i < nSigs && i < 6; i++) {
    int y = top + i * rowH;
    tft.fillRoundRect(bx - 4, y, bw + 8, rowH - 5, 4, C_PANEL);
    tft.setTextFont(2); tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_INK, C_PANEL);
    char nm[20]; snprintf(nm, sizeof(nm), "%.18s", sigs[i].label[0] ? sigs[i].label : "unlabeled");
    tft.drawString(nm, bx + 4, y + 2);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEAL, C_PANEL);
    char meta[22]; snprintf(meta, sizeof(meta), "te %d  x%d", sigs[i].te, sigs[i].count);
    tft.drawString(meta, bx + bw - 4, y + 2);
    // thin rssi heat line
    float p = rssiP(sigs[i].rssi);
    int by = y + 18, iw = bw - 8;
    tft.fillRoundRect(bx + 4, by, iw, 5, 2, 0x2124);
    int fw = (int)(iw * p);
    if (fw > 0) tft.fillRoundRect(bx + 4, by, fw, 5, 2, heat(p));
  }
}

void redraw() { if (view == V_LOCATE) drawLocate(); else drawSigs(); drawFooter(); }

// ---- polling ---------------------------------------------------------------
bool httpGet(const char* path, String& out) {
  HTTPClient http; http.setConnectTimeout(1200); http.setTimeout(1200);
  if (!http.begin(String(BASE) + path)) return false;
  int code = http.GET(); bool ok = (code == 200);
  if (ok) out = http.getString();
  http.end(); return ok;
}
void sortNodes() {
  for (int i = 0; i < nNodes; i++) for (int j = i + 1; j < nNodes; j++)
    if (nodes[j].rssi > nodes[i].rssi) { Node t = nodes[i]; nodes[i] = nodes[j]; nodes[j] = t; }
}
bool pollNodes() {
  String b; if (!httpGet("/nodes.json", b)) return false;
  JsonDocument d; if (deserializeJson(d, b)) return false;
  targetMhz = d["target"] | targetMhz;
  JsonArray a = d["nodes"].as<JsonArray>(); nNodes = 0;
  for (JsonObject n : a) { if (nNodes >= 9) break;
    const char* id = n["id"] | "?"; bool self = n["self"] | false;
    nodes[nNodes].rssi = n["rssi"] | -120; nodes[nNodes].age = n["age"] | 0; nodes[nNodes].self = self;
    if (self || !strcmp(id, "0")) snprintf(nodes[nNodes].label, 14, "EVIL CROW");
    else snprintf(nodes[nNodes].label, 14, "NODE %s", id);
    nNodes++;
  }
  sortNodes(); return true;
}
bool pollSigs() {
  String b; if (!httpGet("/sigs.json", b)) return false;
  JsonDocument d; if (deserializeJson(d, b)) return false;
  JsonArray a = d["sigs"].as<JsonArray>(); nSigs = 0;
  for (JsonObject s : a) { if (nSigs >= 12) break;
    sigs[nSigs].id = s["id"] | 0; sigs[nSigs].te = s["te"] | 0;
    sigs[nSigs].count = s["count"] | 0; sigs[nSigs].rssi = s["rssi"] | -120;
    sigs[nSigs].age = s["age"] | 0;
    snprintf(sigs[nSigs].label, 18, "%s", (const char*)(s["label"] | ""));
    nSigs++;
  }
  return true;
}
bool pollState() {
  String b; if (!httpGet("/state.json", b)) return false;
  JsonDocument d; if (deserializeJson(d, b)) return false;
  sweeps = d["sweeps"] | sweeps;
  int bb = -1; const char* bm = nullptr;
  for (JsonObject c : d["channels"].as<JsonArray>()) { int busy = c["busy"] | 0; if (busy > bb) { bb = busy; bm = c["mhz"] | "?"; } }
  if (bm && bb > 0) snprintf(hotCh, sizeof(hotCh), "busiest %s", bm); else snprintf(hotCh, sizeof(hotCh), "scanning");
  return true;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) { wifiOk = true; return; }
  wifiOk = false;
  if (millis() - lastWifiTry > 3000) { lastWifiTry = millis(); WiFi.disconnect(); WiFi.begin(AP_SSID, AP_PASS); }
}

void flipView() { view = (view == V_LOCATE) ? V_SIGS : V_LOCATE; drawHeader(); redraw(); lastAutoFlip = millis(); }

void setup() {
  Serial.begin(115200);
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);      // backlight (S028R)
  pinMode(27, OUTPUT); digitalWrite(27, HIGH);      // backlight (S024)
  pinMode(TOUCH_IRQ, INPUT);
  tft.init(); tft.setRotation(1);
  SW = tft.width(); SH = tft.height();
  tft.fillScreen(C_BG);
  drawHeader(); redraw();
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(AP_SSID, AP_PASS);
}

void loop() {
  ensureWifi();

  // touch: PENIRQ falling edge toggles view
  bool touched = (digitalRead(TOUCH_IRQ) == LOW);
  if (touched && !touchPrev && millis() - lastTouch > 300) { lastTouch = millis(); flipView(); }
  touchPrev = touched;

  // auto-cycle views every 8s so both are seen hands-free
  if (millis() - lastAutoFlip > 8000) flipView();

  if (wifiOk && millis() - lastPoll > 400) { lastPoll = millis();
    if (view == V_LOCATE && pollNodes()) drawLocate();
  }
  if (wifiOk && millis() - lastSigs > 1500) { lastSigs = millis();
    if (pollSigs() && view == V_SIGS) drawSigs();
  }
  if (wifiOk && millis() - lastState > 2000) { lastState = millis(); pollState(); }

  if (millis() - lastChrome > 1000 || wifiOk != lastWifi) {
    lastChrome = millis(); lastWifi = wifiOk; drawHeader(); drawFooter();
  }
  delay(5);
}
