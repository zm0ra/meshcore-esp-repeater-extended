#pragma once

#include <stdint.h>

#include "mcre_custom_contract.h"

namespace mesh {
class Packet;
}

#ifndef MCRE_MQTT_MAX_BROKER_STATS
#define MCRE_MQTT_MAX_BROKER_STATS 4
#endif

struct McreMqttBrokerStats {
  uint8_t slot;
  bool configured;
  bool connected;
  int32_t profile_index;
  uint32_t connected_since_epoch;
  uint32_t connected_for_s;
  uint32_t connect_attempts;
  uint32_t reconnects;
  uint32_t publish_ok;
  uint32_t publish_fail;
  int32_t last_error_code;
  char profile_name[24];
  char host[96];
  char uri[192];
};

struct McreMqttRuntimeStats {
  bool compile_enabled;
  bool enabled_runtime;
  bool connected;
  uint32_t connected_brokers;
  uint32_t enabled_profiles;
  uint32_t max_slots;
  uint32_t brokers_count;
  uint32_t connect_attempts;
  uint32_t reconnects;
  uint32_t publish_ok;
  uint32_t publish_fail;
  int32_t last_error_code;
  int32_t last_profile_index;
  char active_profile[24];
  char active_host[96];
  char active_uri[192];
  McreMqttBrokerStats brokers[MCRE_MQTT_MAX_BROKER_STATS];
};

void mcreMqttInit();
void mcreMqttLoop();
void mcreMqttOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, const McreRxMeta& meta);
void mcreMqttOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, const McreTxMeta& meta);

const McreMqttRuntimeStats& mcreMqttSnapshot();
