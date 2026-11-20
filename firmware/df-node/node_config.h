// ============================================================================
//  node_config.h  —  Scarecrow DF rover node configuration
//  A rover is any ESP32/ESP32-S3 + a single CC1101. It parks on the target
//  frequency, measures RSSI (and burst timing), and reports to the Evil Crow
//  collector over ESP-NOW. No web UI, no transmit path.
// ============================================================================
#pragma once

// ---- Radio driver backend selectors ----------------------------------------
#define RADIO_DRIVER_ELECHOUSE 1   // ELECHOUSE_CC1101_SRC_DRV (classic ESP32)
#define RADIO_DRIVER_RADIOLIB  2   // jgromes/RadioLib (ESP32-S3 / C-series friendly)

// ---- Pick your board -------------------------------------------------------
#define BOARD_STICKS3_RFPACK  1   // M5Stack StickS3 + Pingequa RF Pack S3  [ELECHOUSE]
#define BOARD_GENERIC_CC1101  2   // Any ESP32 + a bare CC1101 you wire      [ELECHOUSE]
#define BOARD_TEMBED_CC1101   3   // LilyGo T-Embed CC1101 (ESP32-S3)        [RadioLib]
#define BOARD  BOARD_TEMBED_CC1101

#if BOARD == BOARD_STICKS3_RFPACK
  #define RADIO_DRIVER RADIO_DRIVER_ELECHOUSE
  // Verified: BruceDevices/firmware boards/m5stack-sticks3/connections.md
  #define CC_SCK   5
  #define CC_MISO  4
  #define CC_MOSI  6
  #define CC_CS    2
  #define CC_GDO0  3
  // RF Pack S3 has an SN74 that arbitrates the shared MISO between CC1101 and
  // nRF24 by chip-select. Hold the rivals HIGH so the CC1101 owns MISO, or its
  // registers read back as zero (dead radio).
  #define HAS_SN74_BUS 1
  #define BUS_NRF_CS   8
  #define BUS_SD_CS    7
#elif BOARD == BOARD_GENERIC_CC1101
  #define RADIO_DRIVER RADIO_DRIVER_ELECHOUSE
  // Standard ESP32 VSPI wiring — change these to match how you wired the CC1101
  // SPI header (SCK/MISO/MOSI/CSN) + GDO0 on whatever ESP32 you're using.
  #define CC_SCK   18
  #define CC_MISO  19
  #define CC_MOSI  23
  #define CC_CS    5
  #define CC_GDO0  4
  #define HAS_SN74_BUS 0
#elif BOARD == BOARD_TEMBED_CC1101
  // Verified against LilyGo factory firmware (Xinyuan-LilyGO/T-Embed-CC1101:
  // examples/utilities.h + examples/cc1101_recv_irq.ino). The onboard CC1101
  // lives on the board's "LORA" SPI pins (HSPI). Two things the generic wiring
  // guides omit, and which otherwise look exactly like a dead radio:
  //   * GPIO15 (PWR_EN) must be driven HIGH to power the CC1101.
  //   * the antenna RF switch (SW1/SW0) selects the band (both HIGH = 434 MHz).
  #define RADIO_DRIVER RADIO_DRIVER_RADIOLIB
  #define CC_SCK    11   // BOARD_SPI_SCK
  #define CC_MISO   10   // BOARD_SPI_MISO
  #define CC_MOSI   9    // BOARD_SPI_MOSI
  #define CC_CS     12   // BOARD_LORA_CS
  #define CC_GDO0   3    // BOARD_LORA_IO0  (raw OOK data + edge IRQ)
  #define CC_GDO2   38   // BOARD_LORA_IO2
  #define CC_PWR_EN 15   // BOARD_PWR_EN: HIGH powers the CC1101 (+ WS2812)
  #define CC_SW0    48   // BOARD_LORA_SW0  antenna RF switch
  #define CC_SW1    47   // BOARD_LORA_SW1
  #define CC_DISPLAY_CS 41  // park HIGH before init (shared SPI bus)
  #define CC_SD_CS      13  // park HIGH before init (shared SPI bus)
  #define HAS_SN74_BUS 0
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
