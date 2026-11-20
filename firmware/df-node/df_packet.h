// ============================================================================
//  df_packet.h  —  shared ESP-NOW packet format for Scarecrow direction finding
//  Identical copy lives in firmware/scarecrow/ (collector) and firmware/df-node/
//  (rover). Keep them byte-for-byte in sync.
// ============================================================================
#pragma once
#include <stdint.h>

#define DF_MAGIC 0x5343        // 'S','C'  — Scarecrow DF
#define DF_KIND_RSSI  0        // node->collector: periodic RSSI beacon
#define DF_KIND_BURST 1        // node->collector: a burst was captured (te fingerprint)
#define DF_KIND_CFG   2        // collector->node: band/watch-freq to follow

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

// Collector -> node broadcast: the band every rover should tune to, so the
// whole DF mesh follows the fox frequency picked in the web UI.
struct __attribute__((packed)) CfgPacket {
  uint16_t magic;              // DF_MAGIC
  uint8_t  kind;               // DF_KIND_CFG
  float    watch_mhz;          // band for all nodes to follow
};
