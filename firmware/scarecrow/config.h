// ============================================================================
//  SCARECROW  —  receive-only sub-GHz sensor firmware for Evil Crow RF V2
//  The crow that only watches. No transmit path exists in this binary.
//
//  Target : ESP32-PICO-D4 + 2x CC1101 (Evil Crow RF V2)
//  Library: ELECHOUSE_CC1101_SRC_DRV (dual-module fork — same one the stock
//           Evil Crow firmware uses: addSpiPin / setModul)
//  License: CC-BY-4.0 (match upstream)
// ============================================================================
#pragma once

// ---------------------------------------------------------------------------
//  PIN MAP  — verified against joelsernamoreno/EvilCrowRF-V2 firmware.ino
//  CC1101 modules share one SPI bus; SD card is on a SEPARATE VSPI bus.
// ---------------------------------------------------------------------------
#define CC_SCK    14
#define CC_MISO   12
#define CC_MOSI   13

#define CC1_CS     5
#define CC1_GDO0   4      // module 1 async data / RX interrupt pin
#define CC1_GDO2   2      // (stock fw calls this tx_pin1 — unused here)

#define CC2_CS    27
#define CC2_GDO0  26      // module 2 async data / RX interrupt pin
#define CC2_GDO2  25      // (stock fw calls this tx_pin2 — unused here)

#define SD_SCLK   18
#define SD_MISO   19
#define SD_MOSI   23
#define SD_SS     22
#define USE_SD     1      // set 0 to run serial-only (no card needed)

// ---------------------------------------------------------------------------
//  ROLES
//    Module A (CC1 / "scanner")  : hops a channel plan, measures RSSI only.
//                                  Builds occupancy + detects jam/interference.
//    Module B (CC2 / "listener") : parks on ONE watch frequency in async OOK
//                                  mode, captures raw burst timing fingerprints.
//  Two radios, two jobs, simultaneously — the thing a Flipper can't do.
// ---------------------------------------------------------------------------

// ---- Module A: channel plan (MHz). Edit freely; keep inside legal RX bands --
// Covers the common US ISM device clusters. Add/trim as you like.
static const float SCAN_PLAN[] = {
  315.00,                 // US keyfobs, TPMS, garage
  390.00,                 // GM/older fobs
  418.00, 433.92,         // EU/US remotes, weather, sensors
  868.30,                 // EU ISM (SRD) — RX is fine even in US
  902.00, 915.00, 928.00  // US 902-928 ISM (LoRa/Meshtastic, sensors)
};
static const int   SCAN_PLAN_LEN = sizeof(SCAN_PLAN) / sizeof(SCAN_PLAN[0]);

#define SCAN_DWELL_MS        6     // (legacy) nominal dwell per channel
#define SCAN_SETTLE_US     800     // FS/RSSI settle after retune before sampling
#define SCAN_SAMPLES        10     // peak-hold: samples per channel per sweep
#define SCAN_SAMPLE_GAP_US 450     // spacing between peak-hold samples (~5ms total)
#define SCAN_RXBW_KHZ      203     // wide BW = better occupancy sensing
#define SCAN_REPORT_MS    2000     // emit a "sweep" summary this often

// ---- Jammer / interference detection ----
// Per-channel noise floor is tracked as an EWMA. An instantaneous reading
// this many dB over the floor = "busy". Many adjacent channels busy at once,
// sustained, with no clean burst edges = broadband jam signature.
#define BUSY_MARGIN_DB      14
#define FLOOR_EWMA_SHIFT     5     // floor += (sample-floor)>>SHIFT  (bigger=slower)
#define JAM_MIN_CHANNELS     4     // this many channels hot simultaneously
#define JAM_MIN_SWEEPS       3     // for this many consecutive sweeps -> alarm

// ---- Module B: raw burst listener ----
#define WATCH_FREQ_MHZ   433.92f   // park here; change to your area's hot freq
#define FOX_MS             80      // fox-hunt RSSI stream interval (~12 Hz)
#define WATCH_RXBW_KHZ     58      // narrower = cleaner OOK slicing
#define BURST_MIN_EDGES    16      // ignore noise flecks below this
#define BURST_MAX_EDGES   512      // ring buffer size (per burst)
#define BURST_GAP_US     8000      // >this idle = end of burst
#define BURST_MIN_US       100     // ignore sub-100us glitches

// ---- Output ----
#define SERIAL_BAUD     115200
// All telemetry is newline-delimited JSON (NDJSON) on Serial, one event/line,
// so it pipes straight into jq / a Cartographer ingest / a notebook.

// ---- Web dashboard (WiFi AP) ----
// Receive-only still holds: WiFi is the ESP32's own 2.4GHz radio for the UI,
// nothing to do with the CC1101s. The dashboard is read-only — no control,
// no TX, nothing to actuate.
#define USE_WEB        1              // set 0 to drop WiFi entirely
#define AP_SSID        "Scarecrow"
#define AP_PASS        "watchonly"    // >=8 chars; change it
#define AP_CHANNEL     6
#define UI_MAX_LOG     200            // rows kept in the browser log

// ---- Direction finding (ESP-NOW rover nodes) ----
// Receive RSSI/burst reports from Scarecrow DF rover nodes and rank them in the
// LOCATE view (hottest node = nearest the source). Rovers must use the same
// WiFi channel as AP_CHANNEL above. Still receive-only — ESP-NOW is WiFi.
#define USE_DF         1              // 0 to drop DF collector entirely
