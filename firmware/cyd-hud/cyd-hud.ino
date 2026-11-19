// ============================================================================
//  SCARECROW HUD  —  Cheap Yellow Display (ESP32-2432S028R) dashboard
//  Receive-only companion screen for the Evil Crow "Scarecrow" collector.
//  Joins the Scarecrow Wi-Fi AP, polls the collector's JSON endpoints, and
//  renders the live direction-finding view (anchor + rover nodes).
//
//  Display: TFT_eSPI (config comes from -D build flags for the CYD panel).
// ============================================================================
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static TFT_eSPI tft = TFT_eSPI();

// ---- Match these to the collector's config.h --------------------------------
static const char* AP_SSID = "Scarecrow";
static const char* AP_PASS = "watchonly";
static const char* BASE    = "http://192.168.4.1";
#define TFT_BL_PIN 21

// ---- Anubis Darkwatch palette (RGB565) -------------------------------------
#define C_BG   0x0841
#define C_INK  0xE73C
#define C_DIM  0x6B4D
#define C_ACC  0x9C7F
#define C_GOLD 0xE5A6
#define C_OK   0x3EB3
#define C_WARN 0xFB80

static int SW = 320, SH = 240;

struct Node { char label[14]; int rssi; int age; bool self; };
static Node nodes[9];
static int  nNodes = 0;
static float targetMhz = 433.92f;
static long  sweeps = 0;
static char  hotCh[24] = "--";
static bool  wifiOk = false;
static uint32_t lastNodes = 0, lastState = 0, lastWifiTry = 0;

// ---- drawing ---------------------------------------------------------------
void drawHeader() {
  tft.fillRect(0, 0, SW, 44, C_BG);
  tft.setTextFont(2); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_ACC, C_BG);  tft.drawString("SCARECROW", 10, 6);
  tft.setTextColor(C_GOLD, C_BG); tft.drawString("DARKWATCH", 10, 24);
  tft.fillCircle(4, 12, 2, C_ACC);
}

void drawStatusBar() {
  tft.fillRect(0, 44, SW, 22, C_BG);
  tft.setTextFont(2); tft.setTextDatum(TL_DATUM); tft.setTextColor(C_DIM, C_BG);
  char l[40];
  snprintf(l, sizeof(l), "%.2f MHz   sweeps %ld", targetMhz, sweeps);
  tft.drawString(l, 10, 46);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(wifiOk ? C_OK : C_WARN, C_BG);
  tft.drawString(wifiOk ? "LINKED" : "LINKING", SW - 10, 46);
  tft.drawFastHLine(10, 66, SW - 20, C_DIM);
}

void drawNodes() {
  int top = 72, rowH = 30, bx = 14, bw = SW - 28;
  tft.fillRect(0, top, SW, SH - top - 16, C_BG);
  if (nNodes == 0) {
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("waiting for nodes...", SW / 2, top + 30);
    return;
  }
  for (int i = 0; i < nNodes && i < 5; i++) {
    int y = top + i * rowH;
    bool hot = (i == 0);                        // sorted hottest-first
    uint16_t col = nodes[i].self ? C_GOLD : (hot ? C_OK : C_INK);
    tft.setTextFont(2); tft.setTextDatum(TL_DATUM); tft.setTextColor(col, C_BG);
    tft.drawString(nodes[i].label, bx, y);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(C_INK, C_BG);
    char rs[12]; snprintf(rs, sizeof(rs), "%d dBm", nodes[i].rssi);
    tft.drawString(rs, SW - bx, y);
    int by = y + 17, bh = 8;
    float p = (nodes[i].rssi + 110) / 80.0f; if (p < 0) p = 0; if (p > 1) p = 1;
    tft.drawRoundRect(bx, by, bw, bh, 2, C_DIM);
    int fw = (int)((bw - 4) * p);
    if (fw > 0) tft.fillRoundRect(bx + 2, by + 2, fw, bh - 4, 1, hot ? C_GOLD : C_ACC);
    if (nodes[i].age > 5 && !nodes[i].self) {
      tft.setTextFont(1); tft.setTextDatum(TR_DATUM); tft.setTextColor(C_WARN, C_BG);
      tft.drawString("stale", SW - bx - 72, y + 3);
    }
  }
}

void drawFooter() {
  tft.fillRect(0, SH - 14, SW, 14, C_BG);
  tft.setTextFont(2); tft.setTextDatum(BL_DATUM); tft.setTextColor(C_OK, C_BG);
  tft.drawString("RX-ONLY", 10, SH - 1);
  tft.setTextDatum(BR_DATUM); tft.setTextColor(C_DIM, C_BG);
  tft.drawString(hotCh, SW - 10, SH - 1);
}

// ---- polling ---------------------------------------------------------------
bool httpGet(const char* path, String& out) {
  HTTPClient http;
  http.setConnectTimeout(1200); http.setTimeout(1200);
  if (!http.begin(String(BASE) + path)) return false;
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) out = http.getString();
  http.end();
  return ok;
}

void sortNodesByRssi() {
  for (int i = 0; i < nNodes; i++)
    for (int j = i + 1; j < nNodes; j++)
      if (nodes[j].rssi > nodes[i].rssi) { Node t = nodes[i]; nodes[i] = nodes[j]; nodes[j] = t; }
}

bool pollNodes() {
  String body;
  if (!httpGet("/nodes.json", body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  targetMhz = doc["target"] | targetMhz;
  JsonArray arr = doc["nodes"].as<JsonArray>();
  nNodes = 0;
  for (JsonObject n : arr) {
    if (nNodes >= 9) break;
    const char* id = n["id"] | "?";
    bool self = n["self"] | false;
    nodes[nNodes].rssi = n["rssi"] | -120;
    nodes[nNodes].age  = n["age"] | 0;
    nodes[nNodes].self = self;
    if (self || strcmp(id, "0") == 0) snprintf(nodes[nNodes].label, 14, "EVIL CROW");
    else snprintf(nodes[nNodes].label, 14, "NODE %s", id);
    nNodes++;
  }
  sortNodesByRssi();
  return true;
}

bool pollState() {
  String body;
  if (!httpGet("/state.json", body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  sweeps = doc["sweeps"] | sweeps;
  int bestBusy = -1; const char* bestMhz = nullptr;
  for (JsonObject c : doc["channels"].as<JsonArray>()) {
    int busy = c["busy"] | 0;
    if (busy > bestBusy) { bestBusy = busy; bestMhz = c["mhz"] | "?"; }
  }
  if (bestMhz && bestBusy > 0) snprintf(hotCh, sizeof(hotCh), "busiest %s MHz", bestMhz);
  else snprintf(hotCh, sizeof(hotCh), "scanning");
  return true;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) { wifiOk = true; return; }
  wifiOk = false;
  if (millis() - lastWifiTry > 3000) {
    lastWifiTry = millis();
    WiFi.disconnect();
    WiFi.begin(AP_SSID, AP_PASS);
  }
}

void setup() {
  Serial.begin(115200);
  // Drive BOTH backlight candidates: 2432S028R uses GPIO21, 2432S024 uses GPIO27.
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);
  pinMode(27, OUTPUT); digitalWrite(27, HIGH);
  tft.init();
  tft.setRotation(1);                 // landscape 320x240
  SW = tft.width(); SH = tft.height();
  tft.fillScreen(C_BG);
  drawHeader(); drawStatusBar(); drawNodes(); drawFooter();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(AP_SSID, AP_PASS);
}

void loop() {
  ensureWifi();

  if (wifiOk && millis() - lastNodes > 400) {
    lastNodes = millis();
    if (pollNodes()) drawNodes();
  }
  if (wifiOk && millis() - lastState > 2000) {
    lastState = millis();
    pollState();
  }
  static uint32_t lastChrome = 0; static bool lastWifi = false;
  if (millis() - lastChrome > 1000 || wifiOk != lastWifi) {
    lastChrome = millis(); lastWifi = wifiOk;
    drawStatusBar(); drawFooter();
  }
  delay(5);
}
