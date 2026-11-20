// ============================================================================
//  SCARECROW HUD  —  Cheap Yellow Display dashboard (ESP32-2432S024 / S028R)
//  Receive-only companion screen AND remote control for the Evil Crow collector.
//  Joins the Scarecrow Wi-Fi AP and renders:
//    * LOCATE     — anchor + rover nodes, heat-coloured RSSI bars
//    * SIGNATURES — the live signature library (/sigs.json)
//    * CONTROL    — change the mesh band right from the handheld (hits /fox)
//  Touch: a quick TAP cycles the view; a HOLD performs the view's action
//  (in CONTROL, a hold steps the band and pushes it to the Evil Crow + nodes).
//
//  Board variants (pick one with a -D build flag; default BOARD_CYD28):
//    BOARD_CYD28  ESP32-2432S024 / S028R — 2.4"/2.8" ILI9341 320x240,
//                 resistive touch sensed via XPT2046 PENIRQ on GPIO36.
//    BOARD_CYD32  ESP32-2432S032C(-I)    — 3.2" IPS ST7789 240x320 (inverted),
//                 GT911 capacitive touch on I2C (SDA 33 / SCL 32), BL GPIO27.
//  Display panel is configured by TFT_eSPI -D build flags (see build command);
//  this file only differs per board in backlight pins + how touch is sensed.
//  Touch gesture is the same everywhere: quick TAP cycles view, HOLD = action.
// ============================================================================
#if !defined(BOARD_CYD32) && !defined(BOARD_CYD28)
#define BOARD_CYD28
#endif

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#ifdef BOARD_CYD32
#include <Wire.h>
#endif

static TFT_eSPI tft = TFT_eSPI();
static Preferences prefs;
static uint8_t hudRot = 1;                 // screen rotation; BOOT button cycles + saves it
#define BOOT_BTN 0

static const char* AP_SSID = "Scarecrow";
static const char* AP_PASS = "watchonly";
static const char* BASE    = "http://192.168.4.1";
#define TOUCH_IRQ 36

// ---- touch: resistive PENIRQ (CYD28) or GT911 capacitive (CYD32) -----------
#ifdef BOARD_CYD32
static uint8_t gtAddr = 0;                       // GT911 capacitive controller, I2C
static bool gtTouched() {                        // true while a finger is down
  if (!gtAddr) return false;
  Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(gtAddr, (uint8_t)1) != 1) return false;
  uint8_t st = Wire.read();
  if (st & 0x80) {                               // buffer ready -> clear it for the next report
    Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission();
  }
  return (st & 0x80) && (st & 0x0F);
}
static void gtInit() {
  pinMode(25, OUTPUT); pinMode(21, OUTPUT);      // reset pulse with INT low -> address 0x5D
  digitalWrite(25, LOW); digitalWrite(21, LOW); delay(10); digitalWrite(25, HIGH); delay(10);
  pinMode(21, INPUT); delay(50);
  Wire.begin(33, 32, 400000);
  for (uint8_t a : {0x5D, 0x14}) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { gtAddr = a; break; } }
  Serial.printf("# GT911 %s\n", gtAddr ? "found" : "NOT found");
}
#endif
// Unified "is the screen being touched now?" — GT911 reports in bursts, so latch
// briefly so a continuous press doesn't read as rapid tap-release-tap.
static bool touchDown() {
#ifdef BOARD_CYD32
  static uint32_t lastT = 0;
  if (gtTouched()) { lastT = millis(); return true; }
  return (millis() - lastT < 80);
#else
  return digitalRead(TOUCH_IRQ) == LOW;
#endif
}

// ---- Anubis Darkwatch palette (RGB565) -------------------------------------
#define C_BG    0x0820
#define C_PANEL 0x10A3
#define C_INK   0xF79E
#define C_DIM   0x6B6D
#define C_TEAL  0x2E7F
#define C_GOLD  0xFE62
#define C_CRIM  0xF9A6
#define C_OK    0x3EB3
#define C_WARN  0xFB80

static int SW = 320, SH = 240;

struct Node { char label[14]; int rssi; int age; bool self; };
static Node nodes[9];
static int  nNodes = 0;
static float targetMhz = 433.92f;
static long  sweeps = 0;
static char  hotCh[24] = "scanning";

struct Sig { int id; int te; int count; int rssi; int age; char label[18]; };
static Sig  sigs[12];
static int  nSigs = 0;

// mesh bands the CONTROL view steps through (pushed to the Evil Crow)
static const float BANDS[] = {315.00f, 433.92f, 868.30f, 915.00f};
static const int   NBANDS = 4;
static int bandIdx = 1;

static bool wifiOk = false;
enum View { V_LOCATE = 0, V_SIGS = 1, V_CONTROL = 2 };
static View view = V_LOCATE;
static uint32_t lastPoll = 0, lastSigs = 0, lastState = 0, lastWifiTry = 0, lastChrome = 0;
static bool lastWifi = false;

// ---- colour helpers --------------------------------------------------------
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static uint16_t heat(float p) {
  if (p < 0) p = 0; if (p > 1) p = 1;
  if (p < 0.5f) { float t = p / 0.5f;  return rgb(46 + t * 200, 127 + t * 70, 127 - t * 90); }
  float t = (p - 0.5f) / 0.5f;          return rgb(246, 197 - t * 150, 37 + t * 20);
}
static float rssiP(int rssi) { float p = (rssi + 110) / 80.0f; return p < 0 ? 0 : (p > 1 ? 1 : p); }
static const char* viewTag() { return view == V_LOCATE ? "LOCATE" : view == V_SIGS ? "SIGS" : "CTRL"; }

// ---- chrome ----------------------------------------------------------------
void drawCrow(int x, int y) {
  tft.fillCircle(x + 6, y + 6, 6, C_TEAL);
  tft.fillTriangle(x + 11, y + 4, x + 20, y + 6, x + 11, y + 9, C_GOLD);
  tft.fillCircle(x + 5, y + 5, 1, C_BG);
}
void drawHeader() {
  tft.fillRect(0, 0, SW, 42, C_BG);
  drawCrow(8, 6);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(4);
  tft.setTextColor(C_INK, C_BG); tft.drawString("SCARECROW", 34, 2);
  tft.setTextFont(2); tft.setTextColor(C_GOLD, C_BG); tft.drawString("DARKWATCH", 36, 25);
  tft.setTextDatum(TR_DATUM); tft.setTextFont(2);
  tft.setTextColor(wifiOk ? C_OK : C_WARN, C_BG);
  tft.drawString(wifiOk ? "LINKED" : "LINKING", SW - 10, 6);   // link status, not view (dots show view)
  tft.setTextColor(C_DIM, C_BG);
  char l[36]; snprintf(l, sizeof(l), "%.2f MHz", targetMhz);
  tft.drawString(l, SW - 10, 24);
  for (int x = 0; x < SW; x++) tft.drawFastVLine(x, 42, 2, heat((float)x / SW));
}
void drawFooter() {
  tft.fillRect(0, SH - 16, SW, 16, C_BG);
  tft.setTextFont(2); tft.setTextDatum(BL_DATUM);
  tft.setTextColor(C_OK, C_BG); tft.drawString("RX-ONLY", 10, SH - 1);
  // three view dots
  tft.setTextDatum(BC_DATUM); tft.setTextColor(C_DIM, C_BG);
  char d[8]; snprintf(d, sizeof(d), "%c %c %c", view==V_LOCATE?'o':'-', view==V_SIGS?'o':'-', view==V_CONTROL?'o':'-');
  tft.drawString(d, SW / 2, SH - 1);
  tft.setTextDatum(BR_DATUM); tft.setTextColor(C_DIM, C_BG);
  char r[28];
  if (view == V_LOCATE)      snprintf(r, sizeof(r), "sweeps %ld", sweeps);
  else if (view == V_SIGS)   snprintf(r, sizeof(r), "%d sigs", nSigs);
  else                       snprintf(r, sizeof(r), "tap:view hold:set");
  tft.drawString(r, SW - 10, SH - 1);
}

// ---- views -----------------------------------------------------------------
void drawLocate() {
  int top = 50, rowH = 34, bx = 12, bw = SW - 24;
  tft.fillRect(0, top, SW, SH - top - 16, C_BG);
  if (nNodes == 0) {
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("waiting for nodes...", SW / 2, top + 30); return;
  }
  for (int i = 0; i < nNodes && i < 5; i++) {
    int y = top + i * rowH;
    tft.fillRoundRect(bx - 4, y, bw + 8, rowH - 6, 4, C_PANEL);
    float p = rssiP(nodes[i].rssi); uint16_t hc = heat(p);
    tft.setTextFont(2); tft.setTextDatum(TL_DATUM);
    tft.setTextColor(nodes[i].self ? C_GOLD : C_INK, C_PANEL);
    tft.drawString(nodes[i].label, bx + 4, y + 3);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(hc, C_PANEL);
    char rs[12]; snprintf(rs, sizeof(rs), "%d dBm", nodes[i].rssi);
    tft.drawString(rs, bx + bw - 4, y + 3);
    int by = y + 20, bh = 7, iw = bw - 8;
    tft.fillRoundRect(bx + 4, by, iw, bh, 2, 0x2124);
    int fw = (int)(iw * p); if (fw > 0) tft.fillRoundRect(bx + 4, by, fw, bh, 2, hc);
    if (nodes[i].age > 5 && !nodes[i].self) {
      tft.setTextFont(1); tft.setTextDatum(TR_DATUM); tft.setTextColor(C_WARN, C_PANEL);
      tft.drawString("stale", bx + bw - 70, y + 6);
    }
  }
  // Self-only = no rover node present: say so, don't leave a blank where NODE A was.
  if (nNodes == 1 && nodes[0].self) {
    tft.setTextFont(1); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("standalone - no rover - single-point fox meter", SW / 2, top + rowH + 12);
  }
}
void drawSigs() {
  int top = 50, rowH = 30, bx = 12, bw = SW - 24;
  tft.fillRect(0, top, SW, SH - top - 16, C_BG);
  if (nSigs == 0) {
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("no signatures captured yet", SW / 2, top + 30); return;
  }
  for (int i = 0; i < nSigs && i < 6; i++) {
    int y = top + i * rowH;
    tft.fillRoundRect(bx - 4, y, bw + 8, rowH - 5, 4, C_PANEL);
    tft.setTextFont(2); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_INK, C_PANEL);
    char nm[20]; snprintf(nm, sizeof(nm), "%.18s", sigs[i].label[0] ? sigs[i].label : "unlabeled");
    tft.drawString(nm, bx + 4, y + 2);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEAL, C_PANEL);
    char meta[22]; snprintf(meta, sizeof(meta), "te %d  x%d", sigs[i].te, sigs[i].count);
    tft.drawString(meta, bx + bw - 4, y + 2);
    float p = rssiP(sigs[i].rssi); int by = y + 18, iw = bw - 8;
    tft.fillRoundRect(bx + 4, by, iw, 5, 2, 0x2124);
    int fw = (int)(iw * p); if (fw > 0) tft.fillRoundRect(bx + 4, by, fw, 5, 2, heat(p));
  }
}
void drawControl() {
  int top = 50;
  tft.fillRect(0, top, SW, SH - top - 16, C_BG);
  tft.setTextFont(2); tft.setTextDatum(TC_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("MESH BAND - sets Evil Crow + all nodes", SW / 2, top + 2);
  tft.setTextFont(6); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_GOLD, C_BG);
  char b[12]; snprintf(b, sizeof(b), "%.2f", targetMhz);
  tft.drawString(b, SW / 2, top + 52);
  tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("MHz", SW / 2, top + 86);
  int cw = SW / NBANDS, cy = top + 120;
  for (int i = 0; i < NBANDS; i++) {
    bool cur = fabsf(BANDS[i] - targetMhz) < 0.3f;
    tft.fillRoundRect(cw * i + 6, cy - 12, cw - 12, 26, 5, cur ? C_TEAL : C_PANEL);
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM);
    tft.setTextColor(cur ? C_BG : C_INK, cur ? C_TEAL : C_PANEL);
    char bb[8]; snprintf(bb, sizeof(bb), "%d", (int)BANDS[i]);
    tft.drawString(bb, cw * i + cw / 2, cy);
  }
  tft.setTextFont(2); tft.setTextDatum(BC_DATUM); tft.setTextColor(C_OK, C_BG);
  tft.drawString("HOLD screen to change band", SW / 2, SH - 20);
}

void redraw() {
  if (view == V_LOCATE) drawLocate();
  else if (view == V_SIGS) drawSigs();
  else drawControl();
  drawFooter();
}

// ---- polling ---------------------------------------------------------------
bool httpGet(const char* path, String& out) {
  HTTPClient http; http.setConnectTimeout(1200); http.setTimeout(1200);
  if (!http.begin(String(BASE) + path)) return false;
  int code = http.GET(); bool ok = (code == 200);
  if (ok) out = http.getString();
  http.end(); return ok;
}
void sendBand(float mhz) {                      // push a band to the Evil Crow
  String b; char p[40]; snprintf(p, sizeof(p), "/fox?mhz=%.2f", mhz);
  httpGet(p, b);
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
    sigs[nSigs].count = s["count"] | 0; sigs[nSigs].rssi = s["rssi"] | -120; sigs[nSigs].age = s["age"] | 0;
    snprintf(sigs[nSigs].label, 18, "%s", (const char*)(s["label"] | "")); nSigs++;
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

void cycleView() { view = (View)((view + 1) % 3); drawHeader(); redraw(); }
void viewAction() {                             // long-press action, per view
  if (view == V_CONTROL) {
    bandIdx = (bandIdx + 1) % NBANDS;
    sendBand(BANDS[bandIdx]);
    targetMhz = BANDS[bandIdx];                 // optimistic; confirmed on next poll
    drawHeader(); drawControl();
  }
}

// BOOT button steps the screen through its 4 rotations and remembers the choice,
// so any CYD panel (which mount differently) can be set upright in the field.
void cycleRotation() {
  hudRot = (hudRot + 1) & 3;
  prefs.putUChar("rot", hudRot);
  tft.setRotation(hudRot);
  SW = tft.width(); SH = tft.height();
  tft.fillScreen(C_BG); drawHeader(); redraw(); drawFooter();
}
// Short Darkwatch intro at power-on: radiating RF rings, the crow mark, title,
// and a heat sweep. Pure eye-candy, ~1.5s, runs once before the HUD comes up.
void splash() {
  tft.fillScreen(C_BG);
  int cx = SW / 2, cy = SH / 2 - 12;
  for (int rd = 6; rd < 78; rd += 6) {        // radiating "listening" rings
    tft.drawCircle(cx, cy, rd, rd < 30 ? C_GOLD : C_TEAL);
    delay(26);
  }
  tft.fillCircle(cx - 14, cy, 11, C_TEAL);    // crow mark (scaled up)
  tft.fillTriangle(cx - 4, cy - 7, cx + 18, cy - 2, cx - 4, cy + 7, C_GOLD);
  tft.fillCircle(cx - 16, cy - 3, 2, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4); tft.setTextColor(C_INK, C_BG);
  tft.drawString("SCARECROW", cx, cy + 46);
  tft.setTextFont(2); tft.setTextColor(C_GOLD, C_BG);
  tft.drawString("ANUBIS DARKWATCH", cx, cy + 70);
  for (int x = 0; x < SW; x += 3) {           // heat sweep underline
    tft.drawFastVLine(x, cy + 86, 2, heat((float)x / SW));
    delay(3);
  }
  delay(650);
}
void setup() {
  Serial.begin(115200);
#ifdef BOARD_CYD32
  pinMode(27, OUTPUT); digitalWrite(27, HIGH);   // 3.2" backlight (GPIO21 is the GT911 INT)
  gtInit();                                      // capacitive touch over I2C
#else
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);   // 2.8" backlight
  pinMode(27, OUTPUT); digitalWrite(27, HIGH);   // 2.4" backlight
  pinMode(TOUCH_IRQ, INPUT);                     // resistive PENIRQ
#endif
  pinMode(BOOT_BTN, INPUT);                       // BOOT button cycles screen rotation
  prefs.begin("hud", false);
  hudRot = prefs.getUChar("rot", 1);              // remembered orientation (default 1 = landscape)
  tft.init(); tft.setRotation(hudRot);
  SW = tft.width(); SH = tft.height();
  splash();
  tft.fillScreen(C_BG);
  drawHeader(); redraw();
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(AP_SSID, AP_PASS);
}

void loop() {
  ensureWifi();

  // touch: quick tap = cycle view, hold (>700ms) = view action
  static bool tp = false; static uint32_t t0 = 0; static bool acted = false;
  // BOOT button: cycle + save screen rotation (debounced edge)
  static bool bPrev = false;
  bool bNow = (digitalRead(BOOT_BTN) == LOW);
  if (bNow && !bPrev) cycleRotation();
  bPrev = bNow;

  bool touched = touchDown();
  if (touched && !tp) { t0 = millis(); acted = false; }
  if (touched && !acted && millis() - t0 > 700) { acted = true; viewAction(); }
  if (!touched && tp) { uint32_t dur = millis() - t0; if (!acted && dur >= 40 && dur < 500) cycleView(); }
  tp = touched;

  bool wantNodes = (view == V_LOCATE || view == V_CONTROL);
  if (wifiOk && wantNodes && millis() - lastPoll > 400) { lastPoll = millis();
    if (pollNodes()) { if (view == V_LOCATE) drawLocate(); else drawControl(); }
  }
  if (wifiOk && view == V_SIGS && millis() - lastSigs > 1200) { lastSigs = millis();
    if (pollSigs()) drawSigs();
  }
  if (wifiOk && millis() - lastState > 2000) { lastState = millis(); pollState(); }

  if (millis() - lastChrome > 1000 || wifiOk != lastWifi) {
    lastChrome = millis(); lastWifi = wifiOk; drawHeader(); drawFooter();
  }
  delay(5);
}
