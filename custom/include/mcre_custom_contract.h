#pragma once

#include <stdint.h>

// Stable contract version between injector hooks and overlay modules.
// Bump only when function signatures or required behavior changes.
#define MCRE_CUSTOM_API_VERSION 1U

namespace mesh {
class Packet;
}

struct McreRxMeta {
  int len;
  float score;
  int last_snr;
  int last_rssi;
};

struct McreTxMeta {
  int len;
};

uint32_t mcreCustomApiVersion();
