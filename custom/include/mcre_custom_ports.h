#pragma once

#include <stdint.h>

#include "mcre_custom_contract.h"

namespace mesh {
class Packet;
}

#ifndef CONSOLE_MIRROR_PORT
#define CONSOLE_MIRROR_PORT 5003
#endif

#ifndef MAX_TCP_CLIENTS
#define MAX_TCP_CLIENTS 4
#endif

struct McreTcpRuntimeStats {
  uint32_t clients_accepted;
  uint32_t clients_rejected;
  uint32_t clients_disconnected;
  uint32_t max_simultaneous_clients;
  uint32_t rx_frames;
  uint32_t rx_bytes;
  uint32_t rx_parse_errors;
  uint32_t rx_checksum_errors;
  uint32_t tx_frames;
  uint32_t tx_bytes;
  uint32_t tx_deliveries;
  uint32_t tx_delivery_failures;
};

struct McrePortsSnapshot {
  bool wifi_connected;
  int wifi_rssi;
  char wifi_ssid[33];
  char wifi_ip[20];

  uint16_t raw_port;
  uint16_t console_port;
  uint16_t mirror_port;

  uint32_t raw_active_clients;
  uint32_t console_active_clients;
  uint32_t mirror_active_clients;

  McreTcpRuntimeStats tcp;
};

void mcrePortsInit();
void mcrePortsLoop();
void mcrePortsOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, const McreRxMeta& meta);
void mcrePortsOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, const McreTxMeta& meta);

const McrePortsSnapshot& mcrePortsSnapshot();

