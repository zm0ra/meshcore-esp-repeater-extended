#include "mcre_custom_state.h"

#include <Arduino.h>
#include <Mesh.h>
#include <Packet.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "MyMesh.h"

static McreRuntimeState g_mcre_state = {};
static uint32_t g_start_ms = 0;

static uint32_t mcreNowEpoch() {
  if (g_mcre_state.mesh_instance) {
    MyMesh* mesh = static_cast<MyMesh*>(g_mcre_state.mesh_instance);
    if (mesh->getRTCClock()) {
      return mesh->getRTCClock()->getCurrentTime();
    }
  }
  uint32_t t = (uint32_t)time(nullptr);
  if (t > 0) return t;
  return 0;
}

static void mcreRefreshNeighborCount() {
  uint8_t count = 0;
  for (int i = 0; i < MCRE_MAX_NEIGHBORS; i++) {
    if (g_mcre_state.neighbors[i].heard_timestamp > 0) {
      count++;
    }
  }
  g_mcre_state.neighbors_count = count;
}

static McreNeighborInfo* mcreFindOrAllocNeighbor(const uint8_t pub_key[PUB_KEY_SIZE]) {
  int empty_idx = -1;
  uint32_t oldest_ts = UINT32_MAX;
  int oldest_idx = 0;

  for (int i = 0; i < MCRE_MAX_NEIGHBORS; i++) {
    McreNeighborInfo* n = &g_mcre_state.neighbors[i];
    if (memcmp(n->pub_key, pub_key, PUB_KEY_SIZE) == 0) {
      return n;
    }
    if (n->heard_timestamp == 0) {
      if (empty_idx < 0) empty_idx = i;
      continue;
    }
    if (n->heard_timestamp < oldest_ts) {
      oldest_ts = n->heard_timestamp;
      oldest_idx = i;
    }
  }

  if (empty_idx >= 0) {
    return &g_mcre_state.neighbors[empty_idx];
  }
  return &g_mcre_state.neighbors[oldest_idx];
}

static void mcreUpdateNeighborFromAdvert(const mesh::Packet* pkt, const McreRxMeta& meta) {
  if (!pkt) return;
  if (pkt->getPayloadType() != PAYLOAD_TYPE_ADVERT) return;

  int ofs = 0;
  if (pkt->payload_len < (PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + 1)) return;

  uint8_t pub_key[PUB_KEY_SIZE];
  memcpy(pub_key, &pkt->payload[ofs], PUB_KEY_SIZE);
  ofs += PUB_KEY_SIZE;

  uint32_t advert_ts = 0;
  memcpy(&advert_ts, &pkt->payload[ofs], 4);
  ofs += 4;

  ofs += SIGNATURE_SIZE;
  if (ofs >= pkt->payload_len) return;

  const uint8_t* app_data = &pkt->payload[ofs];
  const uint8_t app_data_len = (uint8_t)(pkt->payload_len - ofs);

  AdvertDataParser parser(app_data, app_data_len);
  if (!parser.isValid()) return;
  if (parser.getType() != ADV_TYPE_REPEATER) return;

  McreNeighborInfo* n = mcreFindOrAllocNeighbor(pub_key);
  memset(n, 0, sizeof(*n));
  memcpy(n->pub_key, pub_key, PUB_KEY_SIZE);
  mesh::Utils::toHex(n->id_prefix, pub_key, 4);

  if (parser.hasName()) {
    StrHelper::strncpy(n->name, parser.getName(), sizeof(n->name));
  }

  if (parser.hasLatLon()) {
    n->has_location = 1;
    n->lat_e6 = parser.getIntLat();
    n->lon_e6 = parser.getIntLon();
  }

  float pkt_snr = pkt->getSNR();
  int snr_x4 = (int)(pkt_snr * 4.0f);
  if (snr_x4 > 127) snr_x4 = 127;
  if (snr_x4 < -128) snr_x4 = -128;
  n->snr_x4 = (int8_t)snr_x4;

  n->advert_timestamp = advert_ts;
  n->heard_timestamp = mcreNowEpoch();

  // Fallback for packets where pkt->_snr is missing/0 but metadata has value.
  if (n->snr_x4 == 0 && meta.last_snr != 0) {
    int inferred = meta.last_snr * 4;
    if (inferred > 127) inferred = 127;
    if (inferred < -128) inferred = -128;
    n->snr_x4 = (int8_t)inferred;
  }

  mcreRefreshNeighborCount();
  int snr_int = (int)n->snr_x4 / 4;
  int snr_frac = abs((int)n->snr_x4 % 4) * 25;
  if (n->has_location) {
    mcreStateLogf("ADV RX %s snr=%d.%02d lat_e6=%ld lon_e6=%ld",
                  n->id_prefix, snr_int, snr_frac,
                  (long)n->lat_e6, (long)n->lon_e6);
  } else {
    mcreStateLogf("ADV RX %s snr=%d.%02d", n->id_prefix, snr_int, snr_frac);
  }
}

void mcreStateInit(void* mesh_instance, void* fs_instance) {
  g_mcre_state = {};
  g_mcre_state.initialized = true;
  g_mcre_state.mesh_instance = mesh_instance;
  g_mcre_state.fs_instance = fs_instance;
  g_mcre_state.init_epoch_s = mcreNowEpoch();
  g_mcre_state.last_loop_ms = millis();
  g_start_ms = millis();

  mcreStateLogf("HTTP/extended runtime initialized");
}

void mcreStateLoopTick() {
  if (!g_mcre_state.initialized) return;

  g_mcre_state.loop_ticks++;
  g_mcre_state.last_loop_ms = millis();
  g_mcre_state.uptime_ms = (uint64_t)(millis() - g_start_ms);
}

void mcreStateRecordRx(const mesh::Packet* pkt, const McreRxMeta& meta) {
  if (!g_mcre_state.initialized) return;

  g_mcre_state.rx_packets++;
  g_mcre_state.last_snr = meta.last_snr;
  g_mcre_state.last_rssi = meta.last_rssi;

  mcreUpdateNeighborFromAdvert(pkt, meta);
}

void mcreStateRecordTx(const mesh::Packet* pkt, const McreTxMeta& meta) {
  (void)meta;
  if (!g_mcre_state.initialized) return;

  g_mcre_state.tx_packets++;
  if (pkt && pkt->getPayloadType() == PAYLOAD_TYPE_ADVERT) {
    mcreStateLogf("ADV TX");
  }
}

void mcreStateLogf(const char* fmt, ...) {
  if (!g_mcre_state.initialized || !fmt || !fmt[0]) return;

  char line[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  McreLogEntry* dst = &g_mcre_state.logs[g_mcre_state.logs_next];
  dst->ts = mcreNowEpoch();
  StrHelper::strncpy(dst->msg, line, sizeof(dst->msg));

  g_mcre_state.logs_next = (uint8_t)((g_mcre_state.logs_next + 1) % MCRE_MAX_LOG_ENTRIES);
  if (g_mcre_state.logs_count < MCRE_MAX_LOG_ENTRIES) {
    g_mcre_state.logs_count++;
  }
}

void mcreStateClearLogs() {
  g_mcre_state.logs_count = 0;
  g_mcre_state.logs_next = 0;
  memset(g_mcre_state.logs, 0, sizeof(g_mcre_state.logs));
}

const McreRuntimeState& mcreStateSnapshot() {
  return g_mcre_state;
}

void* mcreStateMeshInstance() {
  return g_mcre_state.mesh_instance;
}

void* mcreStateFsInstance() {
  return g_mcre_state.fs_instance;
}
