// ============================================================================
//  df_packet.h  —  shared ESP-NOW packet format for Scarecrow direction finding
//  Identical copy lives in firmware/scarecrow/ (collector) and firmware/df-node/
//  (rover). Keep them byte-for-byte in sync.
// ============================================================================
#pragma once
#include <stdint.h>

#define DF_MAGIC 0x5343        // 'S','C'  — Scarecrow DF
#define DF_KIND_RSSI  0        // periodic RSSI beacon on the target freq
#define DF_KIND_BURST 1        // a burst was captured (carries te fingerprint)

struct __attribute__((packed)) DfPacket {
  uint16_t magic;              // DF_MAGIC
  char     node;               // node id: 'A', 'B', ...
  uint8_t  kind;               // DF_KIND_*
  uint32_t seq;                // per-node sequence counter
  uint32_t t_ms;               // node's millis() at send
  int16_t  rssi;              // dBm (beacon = live floor; burst = burst peak)
  uint16_t te;                 // burst base symbol time, us (kind=BURST)
  uint16_t edges;              // burst edge count (kind=BURST)
  uint16_t freq_x100;          // monitored freq * 100 (43392 = 433.92 MHz)
};
