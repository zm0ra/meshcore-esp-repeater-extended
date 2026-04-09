#pragma once

#include <stdint.h>
#include "mcre_custom_contract.h"

namespace mesh {
class Packet;
}

// Hook entrypoints injected into upstream MyMesh.cpp
// All functions must stay backward compatible with MCRE_CUSTOM_API_VERSION.
void mcreCustomInit(void* mesh_instance, void* fs_instance);
void mcreCustomLoop(void* mesh_instance);
void mcreCustomOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, int len, float score, int last_snr, int last_rssi);
void mcreCustomOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, int len);
