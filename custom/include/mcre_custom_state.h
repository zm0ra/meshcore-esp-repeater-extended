#pragma once

#include <stdarg.h>
#include <stdint.h>

#include <MeshCore.h>

#include "mcre_custom_contract.h"

namespace mesh {
class Packet;
}

#ifndef MCRE_MAX_NEIGHBORS
#define MCRE_MAX_NEIGHBORS 32
#endif

#ifndef MCRE_MAX_LOG_ENTRIES
#define MCRE_MAX_LOG_ENTRIES 64
#endif

struct McreNeighborInfo {
  uint8_t pub_key[PUB_KEY_SIZE];
  char id_prefix[9];
  char name[32];
  int8_t snr_x4;
  uint8_t has_location;
  int32_t lat_e6;
  int32_t lon_e6;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
};

struct McreLogEntry {
  uint32_t ts;
  char msg[120];
};

struct McreRuntimeState {
  bool initialized;
  uint32_t init_epoch_s;
  uint32_t loop_ticks;
  uint64_t uptime_ms;
  uint32_t rx_packets;
  uint32_t tx_packets;
  uint32_t last_loop_ms;
  int last_snr;
  int last_rssi;

  void* mesh_instance;
  void* fs_instance;

  McreNeighborInfo neighbors[MCRE_MAX_NEIGHBORS];
  uint8_t neighbors_count;

  McreLogEntry logs[MCRE_MAX_LOG_ENTRIES];
  uint8_t logs_count;
  uint8_t logs_next;
};

void mcreStateInit(void* mesh_instance, void* fs_instance);
void mcreStateLoopTick();
void mcreStateRecordRx(const mesh::Packet* pkt, const McreRxMeta& meta);
void mcreStateRecordTx(const mesh::Packet* pkt, const McreTxMeta& meta);

void mcreStateLogf(const char* fmt, ...);
void mcreStateClearLogs();

const McreRuntimeState& mcreStateSnapshot();
void* mcreStateMeshInstance();
void* mcreStateFsInstance();
