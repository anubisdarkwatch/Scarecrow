// ============================================================================
//  SCARECROW — NM-CYD-C5 + NM-RF-HAT BRING-UP  (ESP32-C5)
//  Step 1: prove the three hard parts before porting Scarecrow:
//    1) ST7789 240x320 display + Darkwatch splash
//    2) CC1101 on the RF-HAT answers over SPI (chip version 0x14)
//    3) GDO0 (GPIO8) raw OOK edges captured off the bus (live RX envelope)
//  RECEIVE-ONLY. No transmit call anywhere.
//
//  The C5 has ONE general-purpose SPI, so the display and CC1101 SHARE it
//  (pins 6/7/2). We use Arduino_GFX's shared-interface HWSPI bus and hand the
//  SAME SPIClass to RadioLib, so one driver arbitrates the bus (begin/end
//  transaction per op). The OOK envelope itself rides GDO0 as an interrupt,
//  off the bus entirely.
//
//  Pinout (from CYM reference firmware, JimGat/CYM):
//    Display ST7789 : SCK 6  MOSI 7  MISO 2  CS 23  DC 24  RST -1  BL 25
//    CC1101 (DIP 1) : shared SPI 6/7/2 · CSN GPIO9 · GDO0 GPIO8 (interrupt)
//  >>> Set RF-HAT DIP switch position 1 to CC1101 so the radio powers up. <<<
// ============================================================================
#include <Arduino.h>
#include <math.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>
#include <RadioLib.h>
#include <Preferences.h>

#define BOOT_BTN 28                // NM-CYD-C5 BOOT button is GPIO28 (per CYM board header)
Preferences prefs;
uint8_t scrRot = 0;
bool    scrInv = true;             // IPS panels usually want inversion on

// ---- shared SPI bus + display ----------------------------------------------
#define PIN_SCK  6
#define PIN_MOSI 7
#define PIN_MISO 2
#define LCD_CS   23
#define LCD_DC   24
#define LCD_BL   25
SPIClass sharedSPI(FSPI);
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, PIN_SCK, PIN_MOSI, PIN_MISO, &sharedSPI, true /*shared*/);
Arduino_GFX *gfx = new Arduino_ST7789(bus, -1 /*RST tied to board*/, 0 /*rotation*/, true /*IPS*/, 240, 320);

// ---- CC1101 on the RF-HAT (same SPI bus) -----------------------------------
#define CC_CS    9        // CSN  (RF-HAT IO27)
#define CC_GDO0  8        // GDO0 (RF-HAT IO22) — raw OOK envelope + edge IRQ
#define WATCH_MHZ      433.92f
#define RX_BW_KHZ      58
static CC1101 radio = new Module(CC_CS, CC_GDO0, RADIOLIB_NC, RADIOLIB_NC, sharedSPI);

int     radioRC = -999;
uint8_t ver = 0;          // 0x14 when the CC1101 answers
int16_t rssi = -120;
volatile uint32_t edges = 0, lastEdgeUs = 0;
void IRAM_ATTR onEdge() { edges++; lastEdgeUs = micros(); }

bool radioInit() {                 // (re)bring up the CC1101 — retried live so you
  radioRC = radio.begin(WATCH_MHZ);// can flip DIP#1 and watch it come alive
  if (radioRC != RADIOLIB_ERR_NONE) return false;
  radio.setOOK(true);
  radio.setRxBandwidth((float)RX_BW_KHZ);
  radio.setFrequencyDeviation(5.2);
  radio.receiveDirectAsync();
  pinMode(CC_GDO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(CC_GDO0), onEdge, CHANGE);
  return true;
}

// ---- Darkwatch palette (RGB565) --------------------------------------------
#define C_BG    0x0841
#define C_INK   0xE73C
#define C_DIM   0x6B4D
#define C_ACC   0x9C7F
#define C_GOLD  0xE5A6
#define C_OK    0x3EB3
#define C_HOT   0xF9EB

int W() { return gfx->width(); }
int H() { return gfx->height(); }
void ctext(const char *s, int y, uint16_t c, uint8_t size) {   // centered text
  gfx->setTextSize(size); gfx->setTextColor(c);
  int w = (int)strlen(s) * 6 * size;
  gfx->setCursor((W() - w) / 2, y);
  gfx->print(s);
}
void ltext(const char *s, int x, int y, uint16_t c, uint8_t size) {
  gfx->setTextSize(size); gfx->setTextColor(c); gfx->setCursor(x, y); gfx->print(s);
}

void splash() {
  gfx->fillScreen(C_BG);
  int cx = W() / 2, cy = H() / 2 - 24;
  for (int r = 6; r < H() / 2; r += 7) {               // radiating rings
    gfx->drawCircle(cx, cy, r, r < H() / 6 ? C_GOLD : C_ACC);
    delay(22);
  }
  gfx->fillCircle(cx - 14, cy, 11, C_ACC);             // crow mark
  gfx->fillTriangle(cx - 3, cy - 8, cx + 18, cy - 1, cx - 3, cy + 8, C_GOLD);
  gfx->fillCircle(cx - 16, cy - 3, 2, C_BG);
  ctext("SCARECROW", cy + 44, C_INK, 3);
  ctext("ANUBIS DARKWATCH", cy + 74, C_GOLD, 1);
  ctext("NM-CYD-C5  BRING-UP", cy + 90, C_ACC, 1);
  delay(800);
}

int yEdges = 0, yRssi = 0;          // dynamic-field anchors (set by drawStatic)

// Full layout — drawn once (boot, rotate, invert). No per-frame full clears.
void drawStatic() {
  gfx->fillScreen(C_BG);
  ltext("SCARECROW", 8, 8, C_ACC, 2);
  ltext("DARKWATCH", 8, 26, C_GOLD, 1);
  gfx->drawFastHLine(0, 42, W(), C_DIM);

  int y = 54; char l[40];
  ltext("display ST7789 240x320 OK", 8, y, C_INK, 1); y += 20;
  snprintf(l, sizeof(l), "cc1101  ver 0x%02X %s", ver, ver == 0x14 ? "OK" : "- DIP1?");
  ltext(l, 8, y, ver == 0x14 ? C_OK : C_HOT, 1); y += 20;
  snprintf(l, sizeof(l), "band    %.2f MHz", WATCH_MHZ); ltext(l, 8, y, C_INK, 1); y += 20;
  ltext("gdo0 edges", 8, y, C_INK, 1); yEdges = y; y += 28;
  yRssi = y;
  ltext("dBm RSSI", 8, yRssi + 50, C_DIM, 1);

  ltext("BOOT tap:rotate  hold:invert", 8, H() - 30, C_DIM, 1);
  ltext("RX-ONLY", 8, H() - 16, C_OK, 1);
}

// Only the two changing fields — tiny region repaints, no flicker.
void drawDynamic() {
  char l[20];
  gfx->fillRect(95, yEdges, W() - 100, 8, C_BG);
  snprintf(l, sizeof(l), "%lu", (unsigned long)edges); ltext(l, 97, yEdges, C_INK, 1);
  gfx->fillRect(0, yRssi, W(), 44, C_BG);
  snprintf(l, sizeof(l), "%d", rssi); ctext(l, yRssi + 18, rssi > -75 ? C_OK : C_INK, 5);
}

void setup() {
  Serial.begin(115200);
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);   // park radio CS
  pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, HIGH); // backlight on (GPIO25)

  pinMode(BOOT_BTN, INPUT_PULLUP);
  prefs.begin("c5", false);
  scrRot = prefs.getUChar("rot", 0);
  scrInv = prefs.getUChar("inv", 1);
  gfx->begin(40000000);                                // inits shared SPI + panel
  gfx->setRotation(scrRot);
  gfx->invertDisplay(scrInv);
  splash();

  ver = radioInit() ? 0x14 : 0;                        // CC1101 on the shared bus

  Serial.printf("# C5 bring-up: display OK, cc1101 rc=%d ver=0x%02X\n", radioRC, ver);
  drawStatic(); drawDynamic();
}

void loop() {
  // BOOT button: tap = cycle rotation, hold (>800ms) = toggle invert — saved
  static bool bPrev = false; static uint32_t bT0 = 0; static bool bLong = false;
  bool bNow = (digitalRead(BOOT_BTN) == LOW);
  if (bNow && !bPrev) { bT0 = millis(); bLong = false; }
  if (bNow && !bLong && millis() - bT0 > 800) {
    bLong = true; scrInv = !scrInv; gfx->invertDisplay(scrInv);
    prefs.putUChar("inv", scrInv); drawStatic(); drawDynamic();
  }
  if (!bNow && bPrev && !bLong) {
    scrRot = (scrRot + 1) & 3; gfx->setRotation(scrRot);
    prefs.putUChar("rot", scrRot); drawStatic(); drawDynamic();
  }
  bPrev = bNow;

  // keep trying the CC1101 until it answers (flip DIP#1 and watch it go green)
  static uint32_t tR = 0;
  if (ver != 0x14 && millis() - tR > 1000) {
    tR = millis();
    if (radioInit()) { ver = 0x14; drawStatic(); }
  }

  static uint32_t t = 0;
  if (millis() - t > 300) {
    t = millis();
    if (ver == 0x14) rssi = (int16_t)lroundf(radio.getRSSI());
    drawDynamic();                 // only the changing fields -> no flicker
  }
}
