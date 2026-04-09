#include "mcre_custom_entry.h"

#include "mcre_custom_http.h"
#include "mcre_custom_mqtt.h"
#include "mcre_custom_ports.h"
#include "mcre_custom_state.h"

uint32_t mcreCustomApiVersion() {
  return MCRE_CUSTOM_API_VERSION;
}

void mcreCustomInit(void* mesh_instance, void* fs_instance) {
  mcreStateInit(mesh_instance, fs_instance);
  mcrePortsInit();
  mcreMqttInit();
  mcreHttpInit();
}

void mcreCustomLoop(void* mesh_instance) {
  (void)mesh_instance;
  mcreStateLoopTick();
  mcrePortsLoop();
  mcreMqttLoop();
  mcreHttpLoop();
}

void mcreCustomOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, int len, float score, int last_snr, int last_rssi) {
  McreRxMeta meta{};
  meta.len = len;
  meta.score = score;
  meta.last_snr = last_snr;
  meta.last_rssi = last_rssi;

  mcreStateRecordRx(pkt, meta);
  mcrePortsOnPacketRx(mesh_instance, pkt, meta);
  mcreMqttOnPacketRx(mesh_instance, pkt, meta);
}

void mcreCustomOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, int len) {
  McreTxMeta meta{};
  meta.len = len;

  mcreStateRecordTx(pkt, meta);
  mcrePortsOnPacketTx(mesh_instance, pkt, meta);
  mcreMqttOnPacketTx(mesh_instance, pkt, meta);
}
