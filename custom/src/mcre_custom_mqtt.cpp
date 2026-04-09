#include "mcre_custom_mqtt.h"

#include "mcre_custom_state.h"

#if defined(ESP32)

#include "MyMesh.h"

#include <Mesh.h>
#include <Packet.h>
#include <Utils.h>
#include <WiFi.h>

#include <cctype>
#include <ctime>
#include <cstdint>
#include <cstring>

#include "mqtt_client.h"
extern "C" esp_err_t arduino_esp_crt_bundle_attach(void* conf) __attribute__((weak));
extern "C" esp_err_t esp_crt_bundle_attach(void* conf) __attribute__((weak));

#ifndef MQTT_REPORTING_ENABLED
#define MQTT_REPORTING_ENABLED 0
#endif
#ifndef MQTT_MAX_ACTIVE_BROKERS
#define MQTT_MAX_ACTIVE_BROKERS 4
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_KEEPALIVE
#define MQTT_KEEPALIVE 60
#endif
#ifndef MQTT_TRANSPORT
#define MQTT_TRANSPORT "tcp"
#endif
#ifndef MQTT_WS_PATH
#define MQTT_WS_PATH "/"
#endif
#ifndef MQTT_TLS_ENABLED
#define MQTT_TLS_ENABLED 0
#endif
#ifndef MQTT_TLS_VERIFY
#define MQTT_TLS_VERIFY 1
#endif
#ifndef MQTT_AUTH_METHOD
#define MQTT_AUTH_METHOD "none"
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID ""
#endif
#ifndef MQTT_AUDIENCE
#define MQTT_AUDIENCE ""
#endif
#ifndef MQTT_OWNER
#define MQTT_OWNER ""
#endif
#ifndef MQTT_EMAIL
#define MQTT_EMAIL ""
#endif
#ifndef MQTT_IATA
#define MQTT_IATA "XXX"
#endif
#ifndef MQTT_TOPIC_STATUS
#define MQTT_TOPIC_STATUS "meshcore/{IATA}/{PUBLIC_KEY}/status"
#endif
#ifndef MQTT_TOPIC_PACKETS
#define MQTT_TOPIC_PACKETS "meshcore/{IATA}/{PUBLIC_KEY}/packets"
#endif
#ifndef MQTT_TOPIC_RAW
#define MQTT_TOPIC_RAW "meshcore/{IATA}/{PUBLIC_KEY}/raw"
#endif
#ifndef MQTT_LETSMESH_US_ENABLED
#define MQTT_LETSMESH_US_ENABLED 0
#endif
#ifndef MQTT_LETSMESH_EU_ENABLED
#define MQTT_LETSMESH_EU_ENABLED 0
#endif
#ifndef MQTT_EXTRA_ENABLED
#define MQTT_EXTRA_ENABLED 0
#endif
#ifndef MQTT_EXTRA_NAME
#define MQTT_EXTRA_NAME "extra"
#endif
#ifndef MQTT_EXTRA_HOST
#define MQTT_EXTRA_HOST ""
#endif
#ifndef MQTT_EXTRA_PORT
#define MQTT_EXTRA_PORT 1883
#endif
#ifndef MQTT_EXTRA_KEEPALIVE
#define MQTT_EXTRA_KEEPALIVE 60
#endif
#ifndef MQTT_EXTRA_TRANSPORT
#define MQTT_EXTRA_TRANSPORT "tcp"
#endif
#ifndef MQTT_EXTRA_WS_PATH
#define MQTT_EXTRA_WS_PATH "/"
#endif
#ifndef MQTT_EXTRA_TLS_ENABLED
#define MQTT_EXTRA_TLS_ENABLED 0
#endif
#ifndef MQTT_EXTRA_TLS_VERIFY
#define MQTT_EXTRA_TLS_VERIFY 1
#endif
#ifndef MQTT_EXTRA_AUTH_METHOD
#define MQTT_EXTRA_AUTH_METHOD "none"
#endif
#ifndef MQTT_EXTRA_USERNAME
#define MQTT_EXTRA_USERNAME ""
#endif
#ifndef MQTT_EXTRA_PASSWORD
#define MQTT_EXTRA_PASSWORD ""
#endif
#ifndef MQTT_EXTRA_CLIENT_ID
#define MQTT_EXTRA_CLIENT_ID ""
#endif
#ifndef MQTT_EXTRA_AUDIENCE
#define MQTT_EXTRA_AUDIENCE ""
#endif
#ifndef MQTT_EXTRA_OWNER
#define MQTT_EXTRA_OWNER ""
#endif
#ifndef MQTT_EXTRA_EMAIL
#define MQTT_EXTRA_EMAIL ""
#endif
struct MqttBrokerProfile {
  const char* name;
  const char* host;
  uint16_t port;
  const char* transport;
  const char* ws_path;
  bool tls_enabled;
  bool tls_verify;
  uint16_t keepalive;
  const char* auth_method;
  const char* username;
  const char* password;
  const char* client_id;
  const char* audience;
  const char* owner;
  const char* email;
};

static McreMqttRuntimeStats g_snapshot = {};

#if MQTT_REPORTING_ENABLED
#if MQTT_MAX_ACTIVE_BROKERS < 1
#define MQTT_MAX_SLOTS 1
#elif MQTT_MAX_ACTIVE_BROKERS > 4
#define MQTT_MAX_SLOTS 4
#else
#define MQTT_MAX_SLOTS MQTT_MAX_ACTIVE_BROKERS
#endif

struct MqttClientSlot {
  esp_mqtt_client_handle_t client;
  bool started;
  bool connected;
  bool seen_connected;
  bool needs_reconnect;
  bool online_sent;
  uint32_t next_reconnect_at;
  uint32_t backoff_ms;
  uint32_t connect_started_at;
  uint32_t connected_since_epoch;
  uint32_t connect_attempts;
  uint32_t reconnects;
  uint32_t publish_ok;
  uint32_t publish_fail;
  int profile_index;
  int32_t last_error_code;
  char profile_name[24];
  char host[96];
  char uri[192];
  char client_id[96];
  char username[96];
  char password[768];
  char lwt_payload[320];
};

static MqttClientSlot g_slots[MQTT_MAX_SLOTS] = {};

static char g_node_pub_hex[65] = {0};
static char g_node_pub_hex_upper[65] = {0};
static char g_origin_name[40] = {0};
static char g_topic_status[128] = {0};
static char g_topic_packets[128] = {0};
static char g_topic_raw[128] = {0};

static uint32_t mqttNowEpoch() {
  void* ptr = mcreStateMeshInstance();
  if (ptr) {
    MyMesh* mesh = static_cast<MyMesh*>(ptr);
    if (mesh->getRTCClock()) {
      uint32_t rtc = mesh->getRTCClock()->getCurrentTime();
      if (rtc > 0) return rtc;
    }
  }
  time_t wall = std::time(nullptr);
  if (wall > 0) return (uint32_t)wall;
  return 0;
}

static const char* kMqttClientVersion = "meshcore-esp32-repeater-extended";
static const char* kLetsMeshUsHost = "mqtt-us-v1.letsmesh.net";
static const char* kLetsMeshEuHost = "mqtt-eu-v1.letsmesh.net";

static const char kLetsMeshGoogleCaPem[] PROGMEM =
  "-----BEGIN CERTIFICATE-----\n"
  "MIICnzCCAiWgAwIBAgIQf/MZd5csIkp2FV0TttaF4zAKBggqhkjOPQQDAzBHMQsw\n"
  "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
  "MBIGA1UEAxMLR1RTIFJvb3QgUjQwHhcNMjMxMjEzMDkwMDAwWhcNMjkwMjIwMTQw\n"
  "MDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRydXN0IFNlcnZp\n"
  "Y2VzMQwwCgYDVQQDEwNXRTEwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAARvzTr+\n"
  "Z1dHTCEDhUDCR127WEcPQMFcF4XGGTfn1XzthkubgdnXGhOlCgP4mMTG6J7/EFmP\n"
  "LCaY9eYmJbsPAvpWo4H+MIH7MA4GA1UdDwEB/wQEAwIBhjAdBgNVHSUEFjAUBggr\n"
  "BgEFBQcDAQYIKwYBBQUHAwIwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4EFgQU\n"
  "kHeSNWfE/6jMqeZ72YB5e8yT+TgwHwYDVR0jBBgwFoAUgEzW63T/STaj1dj8tT7F\n"
  "avCUHYwwNAYIKwYBBQUHAQEEKDAmMCQGCCsGAQUFBzAChhhodHRwOi8vaS5wa2ku\n"
  "Z29vZy9yNC5jcnQwKwYDVR0fBCQwIjAgoB6gHIYaaHR0cDovL2MucGtpLmdvb2cv\n"
  "ci9yNC5jcmwwEwYDVR0gBAwwCjAIBgZngQwBAgEwCgYIKoZIzj0EAwMDaAAwZQIx\n"
  "AOcCq1HW90OVznX+0RGU1cxAQXomvtgM8zItPZCuFQ8jSBJSjz5keROv9aYsAm5V\n"
  "sQIwJonMaAFi54mrfhfoFNZEfuNMSQ6/bIBiNLiyoX46FohQvKeIoJ99cx7sUkFN\n"
  "7uJW\n"
  "-----END CERTIFICATE-----\n"
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX\n"
  "MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE\n"
  "CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx\n"
  "NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT\n"
  "GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0\n"
  "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube\n"
  "Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e\n"
  "WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd\n"
  "BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd\n"
  "BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN\n"
  "l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw\n"
  "Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v\n"
  "Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG\n"
  "SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ\n"
  "odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY\n"
  "+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs\n"
  "kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep\n"
  "8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1\n"
  "vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl\n"
  "-----END CERTIFICATE-----\n";

static bool mqttStrEqNoCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

static String mqttJsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char ch = in[i];
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)ch < 0x20) out += " ";
        else out += ch;
        break;
    }
  }
  return out;
}

static String mqttBytesToHex(const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    out += kHex[(b >> 4) & 0x0F];
    out += kHex[b & 0x0F];
  }
  return out;
}

static String mqttBase64UrlEncode(const uint8_t* src, size_t len) {
  static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t n = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | (uint32_t)src[i + 2];
    out += kTbl[(n >> 18) & 0x3F];
    out += kTbl[(n >> 12) & 0x3F];
    out += kTbl[(n >> 6) & 0x3F];
    out += kTbl[n & 0x3F];
    i += 3;
  }
  if (i < len) {
    uint32_t n = ((uint32_t)src[i] << 16);
    out += kTbl[(n >> 18) & 0x3F];
    if (i + 1 < len) {
      n |= ((uint32_t)src[i + 1] << 8);
      out += kTbl[(n >> 12) & 0x3F];
      out += kTbl[(n >> 6) & 0x3F];
    } else {
      out += kTbl[(n >> 12) & 0x3F];
    }
  }
  return out;
}

static String mqttResolveTopicTemplate(const char* templ, const char* iata, const char* pub_hex) {
  String out = templ ? String(templ) : String();
  if (out.length() == 0) return out;
  out.replace("{IATA}", iata ? String(iata) : String("XXX"));
  out.replace("{PUBLIC_KEY}", pub_hex ? String(pub_hex) : String("UNKNOWN"));
  return out;
}

static void mqttBuildTimeFields(uint32_t epoch, String& iso, String& time_only, String& date_only) {
  DateTime dt(epoch);
  char tbuf[12];
  char dbuf[16];
  char ibuf[32];
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
  snprintf(dbuf, sizeof(dbuf), "%d/%d/%d", dt.day(), dt.month(), dt.year());
  snprintf(ibuf, sizeof(ibuf), "%04d-%02d-%02dT%02d:%02d:%02dZ", dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  iso = String(ibuf);
  time_only = String(tbuf);
  date_only = String(dbuf);
}

static String mqttBuildStatusJson(const char* status, uint32_t now_epoch) {
  String iso, t, d;
  mqttBuildTimeFields(now_epoch, iso, t, d);

  String payload;
  payload.reserve(512);
  payload += "{";
  payload += "\"status\":\"" + mqttJsonEscape(String(status ? status : "unknown")) + "\",";
  payload += "\"timestamp\":\"" + mqttJsonEscape(iso) + "\",";
  payload += "\"origin\":\"" + mqttJsonEscape(String(g_origin_name)) + "\",";
  payload += "\"origin_id\":\"" + mqttJsonEscape(String(g_node_pub_hex_upper)) + "\",";
  payload += "\"firmware_version\":\"" + mqttJsonEscape(String(FIRMWARE_VERSION)) + "\",";
  payload += "\"client_version\":\"" + mqttJsonEscape(String(kMqttClientVersion)) + "\"";
  payload += "}";
  return payload;
}

static String mqttBuildJwtToken(const mesh::LocalIdentity& self_id, uint32_t now_epoch,
                                const char* audience, const char* owner, const char* email_in,
                                bool include_owner_email) {
  uint32_t base_epoch = now_epoch;
  if (base_epoch < 946684800UL) {
    base_epoch = 946684800UL + (millis() / 1000UL);
  }
  uint32_t exp_epoch = base_epoch + 3600UL;

  const String header = "{\"alg\":\"Ed25519\",\"typ\":\"JWT\"}";
  String payload = "{\"publicKey\":\"";
  payload += String(g_node_pub_hex_upper);
  payload += "\",\"iat\":";
  payload += String((unsigned long)base_epoch);
  payload += ",\"exp\":";
  payload += String((unsigned long)exp_epoch);
  if (audience && audience[0]) payload += ",\"aud\":\"" + mqttJsonEscape(String(audience)) + "\"";
  if (include_owner_email) {
    if (owner && owner[0]) payload += ",\"owner\":\"" + mqttJsonEscape(String(owner)) + "\"";
    if (email_in && email_in[0]) {
      String email = String(email_in);
      email.toLowerCase();
      payload += ",\"email\":\"" + mqttJsonEscape(email) + "\"";
    }
  }
  payload += ",\"client\":\"" + mqttJsonEscape(String(kMqttClientVersion)) + "\"}";

  String header_b64 = mqttBase64UrlEncode((const uint8_t*)header.c_str(), header.length());
  String payload_b64 = mqttBase64UrlEncode((const uint8_t*)payload.c_str(), payload.length());
  String to_sign = header_b64 + "." + payload_b64;

  uint8_t sig[SIGNATURE_SIZE];
  self_id.sign(sig, (const uint8_t*)to_sign.c_str(), to_sign.length());
  String sig_hex = mqttBytesToHex(sig, SIGNATURE_SIZE);
  return to_sign + "." + sig_hex;
}

static void mqttSetReconnectDelay(MqttClientSlot& slot, uint32_t delay_ms) {
  if (delay_ms < 1000) delay_ms = 1000;
  if (delay_ms > 120000) delay_ms = 120000;
  slot.next_reconnect_at = millis() + delay_ms;
}

static int mqttProfileCount() {
  int count = 0;
  if (MQTT_LETSMESH_US_ENABLED) count++;
  if (MQTT_LETSMESH_EU_ENABLED) count++;
  if (MQTT_EXTRA_ENABLED && strlen(MQTT_EXTRA_HOST) > 0) count++;
  if (strlen(MQTT_HOST) > 0) count++;
  return count;
}

static bool mqttResolveProfile(int index, MqttBrokerProfile& profile) {
  int i = 0;
  if (MQTT_LETSMESH_US_ENABLED) {
    if (i == index) {
      profile = {
        "letsmesh-us", kLetsMeshUsHost, 443, "websockets", "/", true, true, 60,
        "token", "", "", "", kLetsMeshUsHost, MQTT_OWNER, MQTT_EMAIL
      };
      return true;
    }
    i++;
  }
  if (MQTT_LETSMESH_EU_ENABLED) {
    if (i == index) {
      profile = {
        "letsmesh-eu", kLetsMeshEuHost, 443, "websockets", "/", true, true, 60,
        "token", "", "", "", kLetsMeshEuHost, MQTT_OWNER, MQTT_EMAIL
      };
      return true;
    }
    i++;
  }
  if (MQTT_EXTRA_ENABLED && strlen(MQTT_EXTRA_HOST) > 0) {
    if (i == index) {
      profile = {
        MQTT_EXTRA_NAME, MQTT_EXTRA_HOST, (uint16_t)MQTT_EXTRA_PORT, MQTT_EXTRA_TRANSPORT, MQTT_EXTRA_WS_PATH,
        MQTT_EXTRA_TLS_ENABLED, MQTT_EXTRA_TLS_VERIFY, (uint16_t)MQTT_EXTRA_KEEPALIVE,
        MQTT_EXTRA_AUTH_METHOD, MQTT_EXTRA_USERNAME, MQTT_EXTRA_PASSWORD, MQTT_EXTRA_CLIENT_ID,
        MQTT_EXTRA_AUDIENCE, MQTT_EXTRA_OWNER, MQTT_EXTRA_EMAIL
      };
      return true;
    }
    i++;
  }
  if (strlen(MQTT_HOST) > 0) {
    if (i == index) {
      profile = {
        "custom", MQTT_HOST, (uint16_t)MQTT_PORT, MQTT_TRANSPORT, MQTT_WS_PATH,
        MQTT_TLS_ENABLED, MQTT_TLS_VERIFY, (uint16_t)MQTT_KEEPALIVE,
        MQTT_AUTH_METHOD, MQTT_USERNAME, MQTT_PASSWORD, MQTT_CLIENT_ID,
        MQTT_AUDIENCE, MQTT_OWNER, MQTT_EMAIL
      };
      return true;
    }
  }
  return false;
}

static int mqttGetEnabledProfileIndices(int* out_indices, int max_out) {
  int count = 0;
  const int max_profiles = mqttProfileCount();
  for (int idx = 0; idx < max_profiles; idx++) {
    MqttBrokerProfile p = {};
    if (!mqttResolveProfile(idx, p) || !p.host || p.host[0] == 0) continue;
    if (count < max_out && out_indices) out_indices[count] = idx;
    count++;
  }
  return count;
}

static void mqttPrepareIdentity(const mesh::LocalIdentity& self_id, const char* origin_name) {
  mesh::Utils::toHex(g_node_pub_hex, self_id.pub_key, PUB_KEY_SIZE);
  memset(g_node_pub_hex_upper, 0, sizeof(g_node_pub_hex_upper));
  for (size_t i = 0; i < sizeof(g_node_pub_hex_upper) - 1; i++) {
    char ch = g_node_pub_hex[i];
    if (ch == 0) break;
    g_node_pub_hex_upper[i] = (char)toupper((unsigned char)ch);
  }
  StrHelper::strncpy(g_origin_name, origin_name ? origin_name : "unknown", sizeof(g_origin_name));

  String iata = String(MQTT_IATA);
  iata.toUpperCase();
  iata.trim();
  if (iata.length() > 3) iata = iata.substring(0, 3);
  if (iata.length() == 0) iata = "XXX";

  String status_t = mqttResolveTopicTemplate(MQTT_TOPIC_STATUS, iata.c_str(), g_node_pub_hex_upper);
  String packets_t = mqttResolveTopicTemplate(MQTT_TOPIC_PACKETS, iata.c_str(), g_node_pub_hex_upper);
  String raw_t = mqttResolveTopicTemplate(MQTT_TOPIC_RAW, iata.c_str(), g_node_pub_hex_upper);
  StrHelper::strncpy(g_topic_status, status_t.c_str(), sizeof(g_topic_status));
  StrHelper::strncpy(g_topic_packets, packets_t.c_str(), sizeof(g_topic_packets));
  StrHelper::strncpy(g_topic_raw, raw_t.c_str(), sizeof(g_topic_raw));
}

static void mqttStopSlot(int slot_idx, bool clear_profile) {
  if (slot_idx < 0 || slot_idx >= MQTT_MAX_SLOTS) return;
  MqttClientSlot& slot = g_slots[slot_idx];

  if (slot.client) {
    if (slot.started) {
      esp_mqtt_client_stop(slot.client);
    }
    esp_mqtt_client_destroy(slot.client);
    slot.client = nullptr;
  }

  slot.started = false;
  slot.connected = false;
  slot.connect_started_at = 0;
  slot.connected_since_epoch = 0;
  slot.last_error_code = 0;

  if (clear_profile) {
    slot.profile_index = -1;
    slot.seen_connected = false;
    slot.needs_reconnect = false;
    slot.online_sent = false;
    slot.connect_attempts = 0;
    slot.reconnects = 0;
    slot.publish_ok = 0;
    slot.publish_fail = 0;
    slot.backoff_ms = 1000;
    slot.next_reconnect_at = 0;
    slot.profile_name[0] = 0;
    slot.host[0] = 0;
    slot.uri[0] = 0;
    slot.client_id[0] = 0;
    slot.username[0] = 0;
    slot.password[0] = 0;
    slot.lwt_payload[0] = 0;
  }
}

static bool mqttPublishOnSlot(MqttClientSlot& slot, const char* topic, const String& payload, bool retain) {
  if (!slot.client || !slot.connected || !topic || topic[0] == 0) {
    slot.publish_fail++;
    g_snapshot.publish_fail++;
    return false;
  }

  int msg_id = esp_mqtt_client_enqueue(slot.client, topic, payload.c_str(), payload.length(), 0, retain ? 1 : 0, true);
  if (msg_id < 0) {
    slot.publish_fail++;
    g_snapshot.publish_fail++;
    return false;
  }
  slot.publish_ok++;
  g_snapshot.publish_ok++;
  return true;
}

static bool mqttPublishToConnected(const char* topic, const String& payload, bool retain) {
  if (!topic || topic[0] == 0) return false;
  bool any_success = false;
  int connected = 0;
  for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
    MqttClientSlot& slot = g_slots[i];
    if (!slot.connected) continue;
    connected++;
    if (mqttPublishOnSlot(slot, topic, payload, retain)) any_success = true;
  }
  if (connected == 0) {
    g_snapshot.publish_fail++;
    return false;
  }
  return any_success;
}

static void mqttPublishOnlineForFreshConnections(uint32_t now_epoch) {
  if (!g_topic_status[0]) return;
  String payload = mqttBuildStatusJson("online", now_epoch);
  for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
    MqttClientSlot& slot = g_slots[i];
    if (!slot.connected || slot.online_sent) continue;
    if (mqttPublishOnSlot(slot, g_topic_status, payload, true)) {
      slot.online_sent = true;
    }
  }
}

static void mqttPublishRawFrame(const uint8_t* raw, int len, uint32_t now_epoch) {
  if (!raw || len <= 0 || !g_topic_raw[0]) return;

  String iso, t, d;
  mqttBuildTimeFields(now_epoch, iso, t, d);

  String payload;
  payload.reserve(len * 2 + 256);
  payload += "{";
  payload += "\"origin\":\"" + mqttJsonEscape(String(g_origin_name)) + "\",";
  payload += "\"origin_id\":\"" + mqttJsonEscape(String(g_node_pub_hex_upper)) + "\",";
  payload += "\"timestamp\":\"" + mqttJsonEscape(iso) + "\",";
  payload += "\"type\":\"RAW\",";
  payload += "\"time\":\"" + mqttJsonEscape(t) + "\",";
  payload += "\"date\":\"" + mqttJsonEscape(d) + "\",";
  payload += "\"raw\":\"" + mqttBytesToHex(raw, (size_t)len) + "\"";
  payload += "}";
  (void)mqttPublishToConnected(g_topic_raw, payload, false);
}

static void mqttPublishMeshPacket(const mesh::Packet* pkt, int len, const char* direction,
                                  float score, bool include_rx_metrics, int last_snr,
                                  int last_rssi, uint32_t now_epoch) {
  if (!pkt || !g_topic_packets[0]) return;

  uint8_t raw_buf[MAX_TRANS_UNIT + 4];
  int raw_len = pkt->writeTo(raw_buf);
  if (raw_len <= 0) return;

  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  char hash_hex[MAX_HASH_SIZE * 2 + 1];
  mesh::Utils::toHex(hash_hex, hash, MAX_HASH_SIZE);

  String iso, t, d;
  mqttBuildTimeFields(now_epoch, iso, t, d);

  String payload;
  payload.reserve(raw_len * 2 + 512);
  String dir = direction ? String(direction) : String("rx");
  dir.toLowerCase();

  payload += "{";
  payload += "\"origin\":\"" + mqttJsonEscape(String(g_origin_name)) + "\",";
  payload += "\"origin_id\":\"" + mqttJsonEscape(String(g_node_pub_hex_upper)) + "\",";
  payload += "\"timestamp\":\"" + mqttJsonEscape(iso) + "\",";
  payload += "\"type\":\"PACKET\",";
  payload += "\"direction\":\"" + mqttJsonEscape(dir) + "\",";
  payload += "\"time\":\"" + mqttJsonEscape(t) + "\",";
  payload += "\"date\":\"" + mqttJsonEscape(d) + "\",";
  payload += "\"len\":\"" + String(len) + "\",";
  payload += "\"packet_type\":\"" + String((unsigned int)pkt->getPayloadType()) + "\",";
  payload += "\"route\":\"" + String(pkt->isRouteDirect() ? "D" : "F") + "\",";
  payload += "\"payload_len\":\"" + String((unsigned int)pkt->payload_len) + "\",";
  payload += "\"raw\":\"" + mqttBytesToHex(raw_buf, (size_t)raw_len) + "\",";
  payload += "\"hash\":\"" + String(hash_hex) + "\"";
  if (include_rx_metrics) {
    payload += ",\"SNR\":\"" + String((int)last_snr) + "\"";
    payload += ",\"RSSI\":\"" + String((int)last_rssi) + "\"";
    payload += ",\"score\":\"" + String((int)(score * 1000.0f)) + "\"";
  }
  payload += "}";

  (void)mqttPublishToConnected(g_topic_packets, payload, false);
}

static void mqttRefreshSnapshot(uint32_t now_epoch, int enabled_profiles) {
  g_snapshot.enabled_profiles = (enabled_profiles >= 0) ? (uint32_t)enabled_profiles : 0;
  g_snapshot.max_slots = MQTT_MAX_SLOTS;
  g_snapshot.brokers_count = 0;

  int connected_count = 0;
  int active_idx = -1;
  int visible_slots = 0;
  const int snapshot_slots = (MQTT_MAX_SLOTS < MCRE_MQTT_MAX_BROKER_STATS) ? MQTT_MAX_SLOTS : MCRE_MQTT_MAX_BROKER_STATS;

  for (int i = 0; i < snapshot_slots; i++) {
    const MqttClientSlot& slot = g_slots[i];
    McreMqttBrokerStats& out = g_snapshot.brokers[i];
    memset(&out, 0, sizeof(out));
    out.slot = (uint8_t)(i + 1);
    out.profile_index = slot.profile_index;
    out.configured = slot.profile_index >= 0;
    out.connected = slot.connected;
    out.connected_since_epoch = slot.connected_since_epoch;
    out.connect_attempts = slot.connect_attempts;
    out.reconnects = slot.reconnects;
    out.publish_ok = slot.publish_ok;
    out.publish_fail = slot.publish_fail;
    out.last_error_code = slot.last_error_code;
    StrHelper::strncpy(out.profile_name, slot.profile_name, sizeof(out.profile_name));
    StrHelper::strncpy(out.host, slot.host, sizeof(out.host));
    StrHelper::strncpy(out.uri, slot.uri, sizeof(out.uri));
    if (slot.connected_since_epoch > 0 && now_epoch >= slot.connected_since_epoch) {
      out.connected_for_s = now_epoch - slot.connected_since_epoch;
    } else {
      out.connected_for_s = 0;
    }

    if (out.configured || out.connected || out.connect_attempts > 0 || out.reconnects > 0 ||
        out.publish_ok > 0 || out.publish_fail > 0 || out.last_error_code != 0) {
      visible_slots = i + 1;
    }
  }
  if (visible_slots <= 0 && enabled_profiles > 0) {
    visible_slots = enabled_profiles;
  }
  if (visible_slots > snapshot_slots) {
    visible_slots = snapshot_slots;
  }
  if (visible_slots < 0) {
    visible_slots = 0;
  }
  g_snapshot.brokers_count = (uint32_t)visible_slots;

  for (int i = snapshot_slots; i < MCRE_MQTT_MAX_BROKER_STATS; i++) {
    McreMqttBrokerStats& out = g_snapshot.brokers[i];
    memset(&out, 0, sizeof(out));
    out.slot = (uint8_t)(i + 1);
    out.profile_index = -1;
  }

  for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
    if (g_slots[i].connected) {
      connected_count++;
      if (active_idx < 0) active_idx = i;
    }
  }
  if (active_idx < 0) {
    for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
      if (g_slots[i].profile_index >= 0) {
        active_idx = i;
        break;
      }
    }
  }

  g_snapshot.connected_brokers = (uint32_t)connected_count;
  g_snapshot.connected = connected_count > 0;
  if (active_idx >= 0) {
    const MqttClientSlot& slot = g_slots[active_idx];
    StrHelper::strncpy(g_snapshot.active_profile, slot.profile_name, sizeof(g_snapshot.active_profile));
    StrHelper::strncpy(g_snapshot.active_host, slot.host, sizeof(g_snapshot.active_host));
    StrHelper::strncpy(g_snapshot.active_uri, slot.uri, sizeof(g_snapshot.active_uri));
    g_snapshot.last_profile_index = slot.profile_index;
  } else {
    g_snapshot.active_profile[0] = 0;
    g_snapshot.active_host[0] = 0;
    g_snapshot.active_uri[0] = 0;
    g_snapshot.last_profile_index = -1;
  }
}

static void mqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
  (void)base;
  int slot_idx = (int)(intptr_t)handler_args;
  if (slot_idx < 0 || slot_idx >= MQTT_MAX_SLOTS) return;
  MqttClientSlot& slot = g_slots[slot_idx];

  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      slot.connected = true;
      slot.needs_reconnect = false;
      slot.connect_started_at = 0;
      slot.connected_since_epoch = mqttNowEpoch();
      slot.backoff_ms = 1000;
      slot.next_reconnect_at = 0;
      slot.last_error_code = 0;
      g_snapshot.last_error_code = 0;
      if (slot.seen_connected) {
        slot.reconnects++;
        g_snapshot.reconnects++;
      }
      slot.seen_connected = true;
      slot.online_sent = false;
      g_snapshot.last_profile_index = slot.profile_index;
      mcreStateLogf("MQTT connected: %s (%s)",
                    slot.profile_name[0] ? slot.profile_name : "unknown",
                    slot.host[0] ? slot.host : "-");
      break;

    case MQTT_EVENT_DISCONNECTED:
      slot.connected = false;
      slot.connected_since_epoch = 0;
      slot.needs_reconnect = true;
      mqttSetReconnectDelay(slot, slot.backoff_ms);
      if (slot.backoff_ms < 120000) slot.backoff_ms *= 2;
      slot.online_sent = false;
      mcreStateLogf("MQTT disconnected: %s", slot.host[0] ? slot.host : "-");
      break;

    case MQTT_EVENT_ERROR:
      slot.connected = false;
      slot.connected_since_epoch = 0;
      slot.needs_reconnect = true;
      slot.online_sent = false;
      if (event && event->error_handle) {
        slot.last_error_code = event->error_handle->esp_tls_last_esp_err;
        g_snapshot.last_error_code = slot.last_error_code;
        mcreStateLogf("MQTT error tls=0x%X sock=%d rc=%d",
                      (unsigned int)event->error_handle->esp_tls_last_esp_err,
                      (int)event->error_handle->esp_transport_sock_errno,
                      (int)event->error_handle->connect_return_code);
      } else {
        slot.last_error_code = -1;
        g_snapshot.last_error_code = -1;
        mcreStateLogf("MQTT error: missing error_handle");
      }
      mqttSetReconnectDelay(slot, slot.backoff_ms);
      if (slot.backoff_ms < 120000) slot.backoff_ms *= 2;
      break;

    default:
      break;
  }
}

static bool mqttStartSlot(int slot_idx, const mesh::LocalIdentity& self_id, uint32_t now_epoch) {
  if (slot_idx < 0 || slot_idx >= MQTT_MAX_SLOTS) return false;
  MqttClientSlot& slot = g_slots[slot_idx];
  if (slot.profile_index < 0) return false;

  MqttBrokerProfile profile = {};
  if (!mqttResolveProfile(slot.profile_index, profile) || !profile.host || profile.host[0] == 0) {
    mqttStopSlot(slot_idx, true);
    return false;
  }

  String transport = String(profile.transport ? profile.transport : "tcp");
  transport.toLowerCase();
  transport.trim();
  if (transport == "ws" || transport == "wss") transport = "websockets";
  bool is_ws = (transport == "websockets");

  String uri = is_ws ? (profile.tls_enabled ? "wss://" : "ws://") : (profile.tls_enabled ? "mqtts://" : "mqtt://");
  uri += String(profile.host);
  uri += ":";
  uri += String((unsigned int)profile.port);
  if (is_ws) {
    String ws_path = String(profile.ws_path ? profile.ws_path : "/");
    ws_path.trim();
    if (ws_path.length() == 0) ws_path = "/";
    if (!ws_path.startsWith("/")) ws_path = "/" + ws_path;
    uri += ws_path;
  }
  StrHelper::strncpy(slot.profile_name, profile.name ? profile.name : "unknown", sizeof(slot.profile_name));
  StrHelper::strncpy(slot.host, profile.host ? profile.host : "", sizeof(slot.host));
  StrHelper::strncpy(slot.uri, uri.c_str(), sizeof(slot.uri));

  if (profile.client_id && profile.client_id[0]) {
    StrHelper::strncpy(slot.client_id, profile.client_id, sizeof(slot.client_id));
  } else {
    String generated_id = "mc";
    generated_id += String(slot_idx + 1);
    generated_id += "_";
    generated_id += String(g_node_pub_hex_upper).substring(0, 18);
    if (generated_id.length() > 23) generated_id = generated_id.substring(0, 23);
    StrHelper::strncpy(slot.client_id, generated_id.c_str(), sizeof(slot.client_id));
  }

  slot.username[0] = 0;
  slot.password[0] = 0;

  String auth = String(profile.auth_method ? profile.auth_method : "none");
  auth.toLowerCase();
  auth.trim();
  if (auth == "password") {
    StrHelper::strncpy(slot.username, profile.username ? profile.username : "", sizeof(slot.username));
    StrHelper::strncpy(slot.password, profile.password ? profile.password : "", sizeof(slot.password));
  } else if (auth == "token") {
    String user = "v1_" + String(g_node_pub_hex_upper);
    String token = mqttBuildJwtToken(self_id, now_epoch, profile.audience, profile.owner, profile.email,
                                     profile.tls_enabled && profile.tls_verify);
    StrHelper::strncpy(slot.username, user.c_str(), sizeof(slot.username));
    StrHelper::strncpy(slot.password, token.c_str(), sizeof(slot.password));
  }

  String offline_payload = mqttBuildStatusJson("offline", now_epoch);
  StrHelper::strncpy(slot.lwt_payload, offline_payload.c_str(), sizeof(slot.lwt_payload));

  mqttStopSlot(slot_idx, false);

  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.uri = slot.uri;
  mqtt_cfg.client_id = slot.client_id;
  mqtt_cfg.keepalive = profile.keepalive ? profile.keepalive : MQTT_KEEPALIVE;
  mqtt_cfg.disable_auto_reconnect = true;
  mqtt_cfg.skip_cert_common_name_check = profile.tls_enabled && !profile.tls_verify;

  if (profile.tls_enabled && profile.tls_verify) {
    if (arduino_esp_crt_bundle_attach) {
      mqtt_cfg.crt_bundle_attach = arduino_esp_crt_bundle_attach;
    } else if (esp_crt_bundle_attach) {
      mqtt_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    } else if (strstr(profile.host, "letsmesh.net") != nullptr) {
      mqtt_cfg.cert_pem = kLetsMeshGoogleCaPem;
    }
  }

  if (slot.username[0]) mqtt_cfg.username = slot.username;
  if (slot.password[0]) mqtt_cfg.password = slot.password;

  if (g_topic_status[0]) {
    mqtt_cfg.lwt_topic = g_topic_status;
    mqtt_cfg.lwt_msg = slot.lwt_payload;
    mqtt_cfg.lwt_msg_len = strlen(slot.lwt_payload);
    mqtt_cfg.lwt_retain = 1;
    mqtt_cfg.lwt_qos = 0;
  }

  slot.client = esp_mqtt_client_init(&mqtt_cfg);
  if (!slot.client) {
    slot.last_error_code = -2;
    g_snapshot.last_error_code = -2;
    mcreStateLogf("MQTT init failed: %s", slot.host[0] ? slot.host : "-");
    return false;
  }

  esp_mqtt_client_register_event(slot.client, MQTT_EVENT_ANY, mqttEventHandler, (void*)(intptr_t)slot_idx);
  esp_err_t start_err = esp_mqtt_client_start(slot.client);
  if (start_err != ESP_OK) {
    slot.last_error_code = start_err;
    g_snapshot.last_error_code = start_err;
    mqttStopSlot(slot_idx, false);
    mcreStateLogf("MQTT start failed err=%ld", (long)start_err);
    return false;
  }

  slot.started = true;
  slot.connected = false;
  slot.needs_reconnect = false;
  slot.online_sent = false;
  slot.connected_since_epoch = 0;
  slot.connect_started_at = millis();
  slot.next_reconnect_at = 0;
  slot.connect_attempts++;
  g_snapshot.connect_attempts++;

  mcreStateLogf("MQTT connecting: %s (%s)",
                slot.profile_name[0] ? slot.profile_name : "unknown",
                slot.host[0] ? slot.host : "-");

  return true;
}

static void mqttEnsureRuntime(const mesh::LocalIdentity& self_id, const char* origin_name, uint32_t now_epoch) {
  mqttPrepareIdentity(self_id, origin_name);

  int enabled_profile_indices[MQTT_MAX_SLOTS] = {0};
  const int enabled_count = mqttGetEnabledProfileIndices(enabled_profile_indices, MQTT_MAX_SLOTS);
  g_snapshot.enabled_profiles = (uint32_t)enabled_count;

  for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
    MqttClientSlot& slot = g_slots[i];
    const int target_profile = (i < enabled_count) ? enabled_profile_indices[i] : -1;
    if (target_profile < 0) {
      if (slot.profile_index >= 0 || slot.client) mqttStopSlot(i, true);
      continue;
    }
    if (slot.profile_index != target_profile) {
      mqttStopSlot(i, true);
      slot.profile_index = target_profile;
      slot.needs_reconnect = true;
      slot.backoff_ms = 1000;
      slot.next_reconnect_at = 0;
    }
  }

  if (WiFi.status() != WL_CONNECTED || enabled_count <= 0) {
    for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
      if (g_slots[i].profile_index >= 0) {
        mqttStopSlot(i, false);
        g_slots[i].needs_reconnect = true;
      }
    }
    mqttRefreshSnapshot(now_epoch, enabled_count);
    return;
  }

  for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
    MqttClientSlot& slot = g_slots[i];
    if (slot.profile_index < 0) continue;

    if (slot.started && !slot.connected && slot.connect_started_at > 0 &&
        (millis() - slot.connect_started_at) > 15000) {
      mqttStopSlot(i, false);
      slot.needs_reconnect = true;
      mqttSetReconnectDelay(slot, slot.backoff_ms);
      if (slot.backoff_ms < 120000) slot.backoff_ms *= 2;
    }

    if (!slot.needs_reconnect) continue;
    if ((int32_t)(millis() - slot.next_reconnect_at) < 0) continue;

    if (!mqttStartSlot(i, self_id, now_epoch)) {
      slot.needs_reconnect = true;
      mqttSetReconnectDelay(slot, slot.backoff_ms);
      if (slot.backoff_ms < 120000) slot.backoff_ms *= 2;
    }
  }

  mqttRefreshSnapshot(now_epoch, enabled_count);
}
#endif // MQTT_REPORTING_ENABLED

void mcreMqttInit() {
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  g_snapshot.compile_enabled = MQTT_REPORTING_ENABLED ? true : false;
  g_snapshot.enabled_runtime = MQTT_REPORTING_ENABLED ? true : false;
  g_snapshot.connected_brokers = 0;
  g_snapshot.enabled_profiles = 0;
  g_snapshot.max_slots = 0;
  g_snapshot.brokers_count = 0;
  g_snapshot.last_profile_index = -1;
  for (int i = 0; i < MCRE_MQTT_MAX_BROKER_STATS; i++) {
    g_snapshot.brokers[i].slot = (uint8_t)(i + 1);
    g_snapshot.brokers[i].profile_index = -1;
  }
#if MQTT_REPORTING_ENABLED
  g_snapshot.max_slots = MQTT_MAX_SLOTS;
  for (int i = 0; i < MQTT_MAX_SLOTS; i++) {
    g_slots[i] = {};
    g_slots[i].profile_index = -1;
    g_slots[i].backoff_ms = 1000;
  }
#endif
}

void mcreMqttLoop() {
#if MQTT_REPORTING_ENABLED
  void* ptr = mcreStateMeshInstance();
  if (!ptr) return;

  MyMesh* mesh = static_cast<MyMesh*>(ptr);
  uint32_t now_epoch = mqttNowEpoch();
  mqttEnsureRuntime(mesh->self_id, mesh->getNodePrefs()->node_name, now_epoch);
  mqttPublishOnlineForFreshConnections(now_epoch);
#else
  // compile-time disabled
#endif
}

void mcreMqttOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, const McreRxMeta& meta) {
  (void)mesh_instance;
#if MQTT_REPORTING_ENABLED
  if (!pkt) return;
  MyMesh* mesh = static_cast<MyMesh*>(mcreStateMeshInstance());
  if (!mesh) return;

  const uint32_t now_epoch = mqttNowEpoch();
  mqttPublishMeshPacket(pkt, meta.len, "RX", meta.score, true, meta.last_snr, meta.last_rssi, now_epoch);

  uint8_t raw_buf[MAX_TRANS_UNIT + 4];
  int raw_len = pkt->writeTo(raw_buf);
  if (raw_len > 0) {
    mqttPublishRawFrame(raw_buf, raw_len, now_epoch);
  }
#else
  (void)pkt;
  (void)meta;
#endif
}

void mcreMqttOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, const McreTxMeta& meta) {
  (void)mesh_instance;
#if MQTT_REPORTING_ENABLED
  if (!pkt) return;
  MyMesh* mesh = static_cast<MyMesh*>(mcreStateMeshInstance());
  if (!mesh) return;

  const uint32_t now_epoch = mqttNowEpoch();
  mqttPublishMeshPacket(pkt, meta.len, "TX", 0.0f, false, 0, 0, now_epoch);
#else
  (void)pkt;
  (void)meta;
#endif
}

const McreMqttRuntimeStats& mcreMqttSnapshot() {
  return g_snapshot;
}

#else

static McreMqttRuntimeStats g_snapshot = {};

void mcreMqttInit() {
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  g_snapshot.compile_enabled = false;
  g_snapshot.enabled_runtime = false;
  g_snapshot.connected_brokers = 0;
  g_snapshot.enabled_profiles = 0;
  g_snapshot.max_slots = 0;
  g_snapshot.brokers_count = 0;
  g_snapshot.last_profile_index = -1;
  for (int i = 0; i < MCRE_MQTT_MAX_BROKER_STATS; i++) {
    g_snapshot.brokers[i].slot = (uint8_t)(i + 1);
    g_snapshot.brokers[i].profile_index = -1;
  }
}

void mcreMqttLoop() {}
void mcreMqttOnPacketRx(void* mesh_instance, const mesh::Packet* pkt, const McreRxMeta& meta) {
  (void)mesh_instance;
  (void)pkt;
  (void)meta;
}
void mcreMqttOnPacketTx(void* mesh_instance, const mesh::Packet* pkt, const McreTxMeta& meta) {
  (void)mesh_instance;
  (void)pkt;
  (void)meta;
}

const McreMqttRuntimeStats& mcreMqttSnapshot() {
  return g_snapshot;
}

#endif
