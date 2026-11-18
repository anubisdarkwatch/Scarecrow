// ============================================================================
//  node_config.h  —  Scarecrow DF rover node configuration
//  A rover is any ESP32/ESP32-S3 + a single CC1101. It parks on the target
//  frequency, measures RSSI (and burst timing), and reports to the Evil Crow
//  collector over ESP-NOW. No web UI, no transmit path.
// ============================================================================
#pragma once

// ---- Pick your board -------------------------------------------------------
#define BOARD_STICKS3_RFPACK  1   // M5Stack StickS3 + Pingequa RF Pack S3 (CC1101)
#define BOARD_GENERIC_CC1101  2   // ESP32 + any bare/Flipper-GPIO CC1101 you wire:
                                  //   Rabbit Labs Flux Capacitor / Slim-Line, EByte E07, etc.
#define BOARD  BOARD_STICKS3_RFPACK

#if BOARD == BOARD_STICKS3_RFPACK
  // Verified: BruceDevices/firmware boards/m5stack-sticks3/connections.md
  #define CC_SCK   5
  #define CC_MISO  4
  #define CC_MOSI  6
  #define CC_CS    2
  #define CC_GDO0  3
#elif BOARD == BOARD_GENERIC_CC1101
  // Standard ESP32 VSPI wiring — change these to match how you wired the
  // Rabbit Labs board's CC1101 SPI header (SCK/MISO/MOSI/CSN) + GDO0.
  // Flux Capacitor note: its E07 front-end has a TX/RX switch; we never TX,
  // so it sits in RX and the LNA path is active. Nothing to drive for RX-only.
  #define CC_SCK   18
  #define CC_MISO  19
  #define CC_MOSI  23
  #define CC_CS    5
  #define CC_GDO0  4
#endif

// ---- Identity & link -------------------------------------------------------
#define NODE_ID            'A'      // UNIQUE per node: 'A', 'B', 'C', ...
#define COLLECTOR_CHANNEL  6        // MUST equal the Evil Crow's AP_CHANNEL
#define TARGET_FREQ_MHZ    433.92f  // frequency this node watches for DF
#define TARGET_FREQ_X100   43392    // keep in sync with TARGET_FREQ_MHZ
#define TARGET_RXBW_KHZ    58

// ---- Timing ----------------------------------------------------------------
#define RSSI_BEACON_MS     100      // RSSI report cadence (~10 Hz)
#define RSSI_SAMPLES       6        // peak-hold samples per beacon
#define BURST_GAP_US       8000
#define BURST_MIN_EDGES    16
#define BURST_MAX_EDGES    512
#define BURST_MIN_US       100

#define NODE_SERIAL_BAUD   115200
