#include "mcre_custom_ports.h"

#include "mcre_custom_state.h"

#if defined(ESP32)

#define private public
#define protected public
#include "MyMesh.h"
#undef protected
#undef private

#include <Packet.h>
#include <WiFi.h>

#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef TCP_PORT
#define TCP_PORT 5002
#endif
#ifndef CONSOLE_PORT
#define CONSOLE_PORT 5001
#endif

static McrePortsSnapshot g_ports_snapshot = {};

static WiFiServer* g_raw_server = nullptr;
static WiFiClient g_raw_clients[MAX_TCP_CLIENTS];
static uint8_t g_raw_rx_buffer[MAX_TCP_CLIENTS][MAX_TRANS_UNIT + 8];
static int g_raw_rx_pos[MAX_TCP_CLIENTS] = {};
static bool g_raw_client_connected[MAX_TCP_CLIENTS] = {};
static IPAddress g_raw_peer_ip[MAX_TCP_CLIENTS];
static uint16_t g_raw_peer_port[MAX_TCP_CLIENTS] = {};

static WiFiServer* g_console_server = nullptr;
static WiFiClient g_console_client;
static char g_console_cmd[160] = {0};
static int g_console_cmd_len = 0;
static bool g_console_client_connected = false;
static IPAddress g_console_peer_ip;
static uint16_t g_console_peer_port = 0;

static WiFiServer* g_console_mirror_server = nullptr;
static WiFiClient g_console_mirror_client;
static char g_console_mirror_cmd[160] = {0};
static int g_console_mirror_cmd_len = 0;
static bool g_console_mirror_client_connected = false;
static IPAddress g_console_mirror_peer_ip;
static uint16_t g_console_mirror_peer_port = 0;

static uint32_t g_next_wifi_attempt_ms = 0;
static bool g_wifi_logged_connected = false;

static uint16_t mcreFletcher16(const uint8_t* data, size_t len) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  for (size_t i = 0; i < len; i++) {
    sum1 = (uint16_t)((sum1 + data[i]) % 255);
    sum2 = (uint16_t)((sum2 + sum1) % 255);
  }
  return (uint16_t)((sum2 << 8) | sum1);
}

static MyMesh* mcreMesh() {
  return static_cast<MyMesh*>(mcreStateMeshInstance());
}

static bool mcreEnsureWifi() {
  if (strlen(WIFI_SSID) == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);

  if (WiFi.status() == WL_CONNECTED) {
    if (!g_wifi_logged_connected) {
      g_wifi_logged_connected = true;
      mcreStateLogf("TCP WiFi connected: %s", WiFi.localIP().toString().c_str());
    }
    return true;
  }

  g_wifi_logged_connected = false;

  const uint32_t now = millis();
  if ((int32_t)(now - g_next_wifi_attempt_ms) >= 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    g_next_wifi_attempt_ms = now + 7000;
    mcreStateLogf("TCP WiFi connect attempt: %s", WIFI_SSID);
  }

  return false;
}

static void mcreInitServers() {
  if (!g_raw_server) {
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
      g_raw_rx_pos[i] = 0;
    }
    g_raw_server = new WiFiServer(TCP_PORT);
    g_raw_server->begin();
    g_raw_server->setNoDelay(true);
    Serial.printf("[TCP] Raw packet server started on %s:%d\n", WiFi.localIP().toString().c_str(), TCP_PORT);
    mcreStateLogf("TCP raw server started on %s:%d", WiFi.localIP().toString().c_str(), TCP_PORT);
  }

  if (!g_console_server) {
    g_console_server = new WiFiServer(CONSOLE_PORT);
    g_console_server->begin();
    g_console_server->setNoDelay(true);
    Serial.printf("[CONSOLE] TCP console started on %s:%d\n", WiFi.localIP().toString().c_str(), CONSOLE_PORT);
    mcreStateLogf("TCP console server started on %s:%d", WiFi.localIP().toString().c_str(), CONSOLE_PORT);
  }

  if (!g_console_mirror_server) {
    g_console_mirror_server = new WiFiServer(CONSOLE_MIRROR_PORT);
    g_console_mirror_server->begin();
    g_console_mirror_server->setNoDelay(true);
    Serial.printf("[CONSOLE] TCP mirror started on %s:%d\n", WiFi.localIP().toString().c_str(), CONSOLE_MIRROR_PORT);
    mcreStateLogf("TCP mirror server started on %s:%d", WiFi.localIP().toString().c_str(), CONSOLE_MIRROR_PORT);
  }
}

static void mcreHandleConsoleClientInput(WiFiClient& source, char* cmd_buf, int& cmd_len, int cmd_cap, bool mirror_to_usb) {
  MyMesh* mesh = mcreMesh();
  if (!mesh) return;

  while (source.available()) {
    int c = source.read();
    if (c < 0) break;
    char ch = (char)c;

    if (ch == '\b' || ch == 0x7F) {
      if (cmd_len > 0) {
        cmd_len--;
        cmd_buf[cmd_len] = 0;
        source.print("\b \b");
        if (mirror_to_usb) {
          Serial.print("\b \b");
        }
      }
      continue;
    }

    if (ch == '\n') {
      if (cmd_len == 0) {
        continue;
      }
      ch = '\r';
    }

    if (cmd_len < (cmd_cap - 1) && ch != '\r') {
      cmd_buf[cmd_len++] = ch;
      cmd_buf[cmd_len] = 0;
      source.write((const uint8_t*)&ch, 1);
      if (mirror_to_usb) {
        Serial.write((const uint8_t*)&ch, 1);
      }
    }

    if (ch == '\r' || cmd_len == (cmd_cap - 1)) {
      source.print("\r\n");
      if (mirror_to_usb) {
        Serial.print("\r\n");
      }

      char reply[160] = {0};
      if (cmd_len > 0) {
        mesh->handleCommand(0, cmd_buf, reply);
      }

      if (reply[0]) {
        source.print("  -> ");
        source.print(reply);
        source.print("\r\n");
        if (mirror_to_usb) {
          Serial.print("  -> ");
          Serial.print(reply);
          Serial.print("\r\n");
        }
      }

      cmd_len = 0;
      cmd_buf[0] = 0;
      source.print("> ");
      if (mirror_to_usb) {
        Serial.print("> ");
      }
    }
  }
}

static void mcreHandleConsoleServers() {
  if (!g_console_server || !g_console_mirror_server) return;

  if (!g_console_client.connected()) {
    if (g_console_client_connected) {
      String ip = g_console_peer_ip.toString();
      mcreStateLogf("TCP console client disconnected %s:%u", ip.c_str(), (unsigned int)g_console_peer_port);
      g_console_client_connected = false;
      g_console_peer_port = 0;
    }
    if (g_console_client) g_console_client.stop();
    WiFiClient incoming = g_console_server->available();
    if (incoming) {
      g_console_client = incoming;
      g_console_cmd_len = 0;
      g_console_cmd[0] = 0;
      g_console_peer_ip = incoming.remoteIP();
      g_console_peer_port = incoming.remotePort();
      g_console_client_connected = true;
      g_console_client.print("MeshCore repeater console\r\n> ");
      String ip = g_console_peer_ip.toString();
      mcreStateLogf("TCP console client connected %s:%u", ip.c_str(), (unsigned int)g_console_peer_port);
    }
  }
  if (g_console_client && g_console_client.connected()) {
    mcreHandleConsoleClientInput(g_console_client, g_console_cmd, g_console_cmd_len, sizeof(g_console_cmd), false);
  }

  if (!g_console_mirror_client.connected()) {
    if (g_console_mirror_client_connected) {
      String ip = g_console_mirror_peer_ip.toString();
      mcreStateLogf("TCP mirror client disconnected %s:%u", ip.c_str(), (unsigned int)g_console_mirror_peer_port);
      g_console_mirror_client_connected = false;
      g_console_mirror_peer_port = 0;
    }
    if (g_console_mirror_client) g_console_mirror_client.stop();
    WiFiClient incoming = g_console_mirror_server->available();
    if (incoming) {
      g_console_mirror_client = incoming;
      g_console_mirror_cmd_len = 0;
      g_console_mirror_cmd[0] = 0;
      g_console_mirror_peer_ip = incoming.remoteIP();
      g_console_mirror_peer_port = incoming.remotePort();
      g_console_mirror_client_connected = true;
      g_console_mirror_client.print("MeshCore repeater console mirror\r\n> ");
      Serial.print("\r\nMeshCore repeater console mirror\r\n> ");
      String ip = g_console_mirror_peer_ip.toString();
      mcreStateLogf("TCP mirror client connected %s:%u", ip.c_str(), (unsigned int)g_console_mirror_peer_port);
    }
  }
  if (g_console_mirror_client && g_console_mirror_client.connected()) {
    mcreHandleConsoleClientInput(g_console_mirror_client, g_console_mirror_cmd, g_console_mirror_cmd_len, sizeof(g_console_mirror_cmd), true);
  }
}

static bool mcreParseFrameAndSend(const uint8_t* buffer, int len, int client_idx) {
  MyMesh* mesh = mcreMesh();
  if (!mesh) return false;

  if (len < 6) {
    g_ports_snapshot.tcp.rx_parse_errors++;
    return false;
  }
  if (buffer[0] != 0xC0 || buffer[1] != 0x3E) {
    g_ports_snapshot.tcp.rx_parse_errors++;
    return false;
  }

  uint16_t payload_len = (uint16_t)(((uint16_t)buffer[2] << 8) | buffer[3]);
  int needed = 4 + payload_len + 2;

  if (payload_len > (MAX_TRANS_UNIT + 1)) {
    g_ports_snapshot.tcp.rx_parse_errors++;
    return false;
  }
  if (len != needed) {
    g_ports_snapshot.tcp.rx_parse_errors++;
    return false;
  }

  uint16_t rx_checksum = (uint16_t)(((uint16_t)buffer[4 + payload_len] << 8) | buffer[5 + payload_len]);
  uint16_t calc_checksum = mcreFletcher16(buffer + 4, payload_len);
  if (rx_checksum != calc_checksum) {
    g_ports_snapshot.tcp.rx_checksum_errors++;
    Serial.printf("[TCP] Bad checksum from client %d (got 0x%04X, want 0x%04X)\n", client_idx, rx_checksum, calc_checksum);
    return false;
  }

  mesh::Packet* pkt = mesh->_mgr->allocNew();
  if (!pkt) {
    g_ports_snapshot.tcp.rx_parse_errors++;
    return false;
  }
  if (!pkt->readFrom(buffer + 4, payload_len)) {
    mesh->_mgr->free(pkt);
    g_ports_snapshot.tcp.rx_parse_errors++;
    return false;
  }

  g_ports_snapshot.tcp.rx_frames++;
  g_ports_snapshot.tcp.rx_bytes += payload_len;
  mesh->sendFlood(pkt);
  return true;
}

static void mcreHandleRawServerRx() {
  if (!g_raw_server) return;

  WiFiClient incoming = g_raw_server->available();
  if (incoming) {
    int free_idx = -1;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
      if (!g_raw_clients[i].connected()) {
        g_raw_clients[i].stop();
        free_idx = i;
        break;
      }
    }
    if (free_idx >= 0) {
      IPAddress peer_ip = incoming.remoteIP();
      uint16_t peer_port = incoming.remotePort();
      g_raw_clients[free_idx] = incoming;
      g_raw_rx_pos[free_idx] = 0;
      g_raw_client_connected[free_idx] = true;
      g_raw_peer_ip[free_idx] = peer_ip;
      g_raw_peer_port[free_idx] = peer_port;
      g_ports_snapshot.tcp.clients_accepted++;
      String ip = peer_ip.toString();
      mcreStateLogf("TCP raw client connected idx=%d %s:%u", free_idx, ip.c_str(), (unsigned int)peer_port);
    } else {
      g_ports_snapshot.tcp.clients_rejected++;
      String ip = incoming.remoteIP().toString();
      incoming.stop();
      mcreStateLogf("TCP raw client rejected %s:%u no slot", ip.c_str(), (unsigned int)incoming.remotePort());
    }
  }

  for (int idx = 0; idx < MAX_TCP_CLIENTS; idx++) {
    WiFiClient& client = g_raw_clients[idx];

    if (!client.connected()) {
      if (g_raw_client_connected[idx]) {
        g_raw_client_connected[idx] = false;
        g_ports_snapshot.tcp.clients_disconnected++;
        String ip = g_raw_peer_ip[idx].toString();
        mcreStateLogf("TCP raw client disconnected idx=%d %s:%u", idx, ip.c_str(), (unsigned int)g_raw_peer_port[idx]);
        g_raw_peer_port[idx] = 0;
      }
      g_raw_rx_pos[idx] = 0;
      continue;
    }

    while (client.available()) {
      int c = client.read();
      if (c < 0) break;

      uint8_t b = (uint8_t)c;

      // Frame start sync: wait for 0xC0 0x3E and keep binary payload untouched.
      if (g_raw_rx_pos[idx] == 0) {
        if (b != 0xC0) continue;
        g_raw_rx_buffer[idx][g_raw_rx_pos[idx]++] = b;
        continue;
      }
      if (g_raw_rx_pos[idx] == 1) {
        if (b == 0xC0) {
          g_raw_rx_buffer[idx][0] = 0xC0;
          g_raw_rx_pos[idx] = 1;
          continue;
        }
        if (b != 0x3E) {
          g_raw_rx_pos[idx] = 0;
          continue;
        }
        g_raw_rx_buffer[idx][g_raw_rx_pos[idx]++] = b;
        continue;
      }

      if (g_raw_rx_pos[idx] >= (int)sizeof(g_raw_rx_buffer[idx])) {
        g_ports_snapshot.tcp.rx_parse_errors++;
        g_raw_rx_pos[idx] = 0;
        continue;
      }

      g_raw_rx_buffer[idx][g_raw_rx_pos[idx]++] = b;

      if (g_raw_rx_pos[idx] >= 4) {
        uint16_t payload_len = (uint16_t)(((uint16_t)g_raw_rx_buffer[idx][2] << 8) | g_raw_rx_buffer[idx][3]);
        if (payload_len > (MAX_TRANS_UNIT + 1)) {
          g_ports_snapshot.tcp.rx_parse_errors++;
          g_raw_rx_pos[idx] = 0;
          continue;
        }

        int needed = 4 + payload_len + 2;
        if (g_raw_rx_pos[idx] < needed) {
          continue;
        }

        if (g_raw_rx_pos[idx] > needed) {
          g_ports_snapshot.tcp.rx_parse_errors++;
          g_raw_rx_pos[idx] = 0;
          if (b == 0xC0) {
            g_raw_rx_buffer[idx][0] = 0xC0;
            g_raw_rx_pos[idx] = 1;
          }
          continue;
        }

        (void)mcreParseFrameAndSend(g_raw_rx_buffer[idx], needed, idx);
        g_raw_rx_pos[idx] = 0;
      }
    }
  }
}

static void mcreBroadcastRawFrame(const uint8_t* payload, int len) {
  if (!g_raw_server || !payload || len <= 0) return;

  uint16_t checksum = mcreFletcher16(payload, (size_t)len);
  g_ports_snapshot.tcp.tx_frames++;
  g_ports_snapshot.tcp.tx_bytes += (uint32_t)len;

  uint8_t header[4] = {0xC0, 0x3E, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF)};

  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    WiFiClient& client = g_raw_clients[i];
    if (!client.connected()) {
      if (client) client.stop();
      continue;
    }

    size_t written = 0;
    written += client.write(header, 4);
    written += client.write(payload, len);
    written += client.write((uint8_t)((checksum >> 8) & 0xFF));
    written += client.write((uint8_t)(checksum & 0xFF));
    written += client.write('\n');

    if (written == (size_t)(len + 7)) {
      g_ports_snapshot.tcp.tx_deliveries++;
    } else {
      g_ports_snapshot.tcp.tx_delivery_failures++;
      if (g_raw_client_connected[i]) {
        String ip = g_raw_peer_ip[i].toString();
        mcreStateLogf("TCP raw client write failed idx=%d %s:%u", i, ip.c_str(), (unsigned int)g_raw_peer_port[i]);
        g_ports_snapshot.tcp.clients_disconnected++;
        g_raw_client_connected[i] = false;
        g_raw_peer_port[i] = 0;
      }
      client.stop();
      g_raw_rx_pos[i] = 0;
    }
  }
}

static void mcreUpdateSnapshot() {
  g_ports_snapshot.wifi_connected = (WiFi.status() == WL_CONNECTED);
  g_ports_snapshot.wifi_rssi = WiFi.RSSI();

  String ssid = WiFi.SSID();
  String ip = WiFi.localIP().toString();
  StrHelper::strncpy(g_ports_snapshot.wifi_ssid, ssid.c_str(), sizeof(g_ports_snapshot.wifi_ssid));
  StrHelper::strncpy(g_ports_snapshot.wifi_ip, ip.c_str(), sizeof(g_ports_snapshot.wifi_ip));

  g_ports_snapshot.raw_port = TCP_PORT;
  g_ports_snapshot.console_port = CONSOLE_PORT;
  g_ports_snapshot.mirror_port = CONSOLE_MIRROR_PORT;

  uint32_t raw_active = 0;
  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    if (g_raw_clients[i].connected()) raw_active++;
  }
  g_ports_snapshot.raw_active_clients = raw_active;
  g_ports_snapshot.console_active_clients = (g_console_client.connected() ? 1U : 0U);
  g_ports_snapshot.mirror_active_clients = (g_console_mirror_client.connected() ? 1U : 0U);

  if (raw_active > g_ports_snapshot.tcp.max_simultaneous_clients) {
    g_ports_snapshot.tcp.max_simultaneous_clients = raw_active;
  }
}

void mcrePortsInit() {
  memset(&g_ports_snapshot, 0, sizeof(g_ports_snapshot));
  g_ports_snapshot.raw_port = TCP_PORT;
  g_ports_snapshot.console_port = CONSOLE_PORT;
  g_ports_snapshot.mirror_port = CONSOLE_MIRROR_PORT;
}

void mcrePortsLoop() {
  if (!mcreEnsureWifi()) {
    mcreUpdateSnapshot();
    return;
  }

  mcreInitServers();
  mcreHandleRawServerRx();
  mcreHandleConsoleServers();
  mcreUpdateSnapshot();
}

void mcrePortsOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, const McreRxMeta& meta) {
  (void)mesh_instance;
  (void)meta;

  if (!pkt || !g_raw_server) return;
  uint8_t buffer[MAX_TRANS_UNIT + 4];
  int payload_len = pkt->writeTo(buffer);
  if (payload_len > 0) {
    mcreBroadcastRawFrame(buffer, payload_len);
  }
}

void mcrePortsOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, const McreTxMeta& meta) {
  (void)mesh_instance;
  (void)meta;

  if (!pkt || !g_raw_server) return;
  uint8_t buffer[MAX_TRANS_UNIT + 4];
  int payload_len = pkt->writeTo(buffer);
  if (payload_len > 0) {
    mcreBroadcastRawFrame(buffer, payload_len);
  }
}

const McrePortsSnapshot& mcrePortsSnapshot() {
  return g_ports_snapshot;
}

#else

static McrePortsSnapshot g_ports_snapshot = {};

void mcrePortsInit() {}
void mcrePortsLoop() {}
void mcrePortsOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, const McreRxMeta& meta) {
  (void)mesh_instance;
  (void)pkt;
  (void)meta;
}
void mcrePortsOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, const McreTxMeta& meta) {
  (void)mesh_instance;
  (void)pkt;
  (void)meta;
}
const McrePortsSnapshot& mcrePortsSnapshot() {
  return g_ports_snapshot;
}

#endif
