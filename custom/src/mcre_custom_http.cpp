#include "mcre_custom_http.h"

#include "mcre_custom_mqtt.h"
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

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <cstring>

#ifndef HTTP_STATS_PORT
#define HTTP_STATS_PORT 80
#endif
#ifndef MQTT_REPORTING_ENABLED
#define MQTT_REPORTING_ENABLED 0
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_TRANSPORT
#define MQTT_TRANSPORT "tcp"
#endif
#ifndef MQTT_WS_PATH
#define MQTT_WS_PATH "/"
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
#ifndef MQTT_IATA
#define MQTT_IATA "XXX"
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
#ifndef TCP_PORT
#define TCP_PORT 5002
#endif
#ifndef CONSOLE_PORT
#define CONSOLE_PORT 5001
#endif
#ifndef CONSOLE_MIRROR_PORT
#define CONSOLE_MIRROR_PORT 5003
#endif
#ifndef HTTP_MAX_CLIENTS
#define HTTP_MAX_CLIENTS 4
#endif
#ifndef HTTP_MAX_HEADER_BYTES
#define HTTP_MAX_HEADER_BYTES 6144
#endif
#ifndef HTTP_MAX_BODY_BYTES
#define HTTP_MAX_BODY_BYTES 12288
#endif
#ifndef HTTP_CLIENT_TIMEOUT_MS
#define HTTP_CLIENT_TIMEOUT_MS 2500
#endif
#ifndef HTTP_READ_BUDGET_PER_LOOP
#define HTTP_READ_BUDGET_PER_LOOP 768
#endif

struct HttpRequestSlot {
  WiFiClient client;
  bool active;
  bool headers_done;
  uint32_t accepted_ms;
  uint32_t last_activity_ms;
  int content_length;
  String header_buf;
  String request_line;
  String request_body;
};

static WiFiServer* g_http_server = nullptr;
static HttpRequestSlot g_http_slots[HTTP_MAX_CLIENTS] = {};
static uint32_t g_http_requests_total = 0;
static uint32_t g_http_not_found = 0;
static uint32_t g_http_rejected = 0;
static uint32_t g_http_max_parallel = 0;

static MyMesh* mcreMesh() {
  return static_cast<MyMesh*>(mcreStateMeshInstance());
}

static String httpJsonEscapeSimple(const String& in) {
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

static String httpUrlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char ch = in[i];
    if (ch == '+') {
      out += ' ';
    } else if (ch == '%' && i + 2 < in.length()) {
      auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
      };
      int v1 = hexVal(in[i + 1]);
      int v2 = hexVal(in[i + 2]);
      if (v1 >= 0 && v2 >= 0) {
        out += (char)((v1 << 4) | v2);
        i += 2;
      } else {
        out += ch;
      }
    } else {
      out += ch;
    }
  }
  return out;
}

static String httpGetParam(const String& params, const char* key) {
  if (!key || key[0] == 0) return String();
  String k = String(key) + "=";
  int start = 0;
  while (start < (int)params.length()) {
    int amp = params.indexOf('&', start);
    if (amp < 0) amp = params.length();
    String part = params.substring(start, amp);
    if (part.startsWith(k)) {
      return httpUrlDecode(part.substring(k.length()));
    }
    start = amp + 1;
  }
  return String();
}

static String httpMethodFromRequestLine(const String& request_line) {
  int sp = request_line.indexOf(' ');
  if (sp <= 0) return String("-");
  return request_line.substring(0, sp);
}

static String httpPathFromRequestLine(const String& request_line) {
  int first = request_line.indexOf(' ');
  if (first < 0) return String("/");
  int second = request_line.indexOf(' ', first + 1);
  if (second < 0) second = request_line.length();
  if (second <= first + 1) return String("/");
  return request_line.substring(first + 1, second);
}

static const char kRootHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MeshCore Repeater</title>
  <style>
    :root {
      --bg:#e9eef6;
      --card:#f8fbff;
      --line:#c8d4e5;
      --line2:#d7dfec;
      --text:#1f2b3d;
      --muted:#5b6e8f;
      --accent:#2a6cd6;
      --accent-bg:#dce8fb;
      --danger:#b0443c;
      --danger-bg:#fbe8e6;
      --ok:#0f6c45;
      --warn:#c17b14;
      --radius:18px;
    }
    *{box-sizing:border-box}
    html,body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;font-size:13px;line-height:1.32}
    .wrap{width:min(1420px,calc(100% - 12px));margin:6px auto 74px}
    .panel{background:rgba(255,255,255,.45);border:1px solid var(--line);border-radius:var(--radius);padding:12px;margin-bottom:8px;box-shadow:0 1px 0 rgba(255,255,255,.8) inset}
    h1{margin:0;font-size:clamp(22px,2.0vw,31px);letter-spacing:-.02em;line-height:1.08;font-weight:770}
    h2{margin:0 0 8px 0;font-size:clamp(18px,1.6vw,25px);line-height:1.1;font-weight:740;letter-spacing:-.01em}
    h3{margin:0 0 8px 0;font-size:clamp(14px,1.05vw,18px)}
    .header-top{display:flex;justify-content:space-between;align-items:flex-start;gap:10px;flex-wrap:wrap}
    .header-main{flex:1;min-width:0}
    .chips{display:flex;gap:8px;flex-wrap:wrap;margin-top:7px}
    .chip{display:inline-flex;align-items:center;gap:6px;padding:6px 11px;border:1px solid var(--line);background:#eef3fb;border-radius:999px;font-size:clamp(12px,.95vw,14px)}
    .chip b{font-weight:750}
    .updated{font-size:clamp(12px,.95vw,14px);font-weight:700;border-radius:999px;padding:6px 12px;border:1px solid var(--line);background:#eef3fb}
    .linkrow{margin-top:8px;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
    .linkrow .label{font-size:clamp(10px,.78vw,12px);text-transform:uppercase;letter-spacing:.08em;color:#5f7090;font-weight:700}
    .linkrow a{display:inline-flex;align-items:center;padding:4px 10px;border:1px solid #c2d0e4;border-radius:999px;background:#eef3fb;color:#255bb4;text-decoration:none;font-size:clamp(12px,.86vw,13px);font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
    .linkrow a:hover{background:#e4ecf9}
    .tabs{display:flex;gap:7px;margin:0 0 8px 0}
    .tab{border:1px solid #a7bddf;background:#dfeafb;padding:6px 13px;border-radius:999px;font-weight:700;font-size:clamp(12px,.9vw,14px);color:#1f3f72;cursor:pointer}
    .tab:not(.active){background:#edf2fa;color:#334f76;border-color:#c2d0e4}
    a:focus-visible, button:focus-visible, input:focus-visible, select:focus-visible, textarea:focus-visible {
      outline:2px solid #2a6cd6;
      outline-offset:2px;
    }
    .hidden{display:none!important}

    .quick-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}
    .quick{background:rgba(255,255,255,.62);border:1px solid var(--line);border-radius:14px;padding:10px;min-height:108px}
    .quick .k{font-size:clamp(10px,.82vw,12px);letter-spacing:.07em;text-transform:uppercase;color:#617293}
    .quick .v{font-size:clamp(16px,1.4vw,24px);font-weight:760;line-height:1.08;margin:3px 0}
    .quick .m{font-size:clamp(12px,.9vw,15px);color:#3c4f6f;line-height:1.3}
    .port-lines{display:grid;gap:3px}
    .port-lines .row{display:flex;justify-content:space-between;align-items:center}
    .port-lines .lbl{opacity:.95}
    .port-lines .val{font-weight:760}
    .health-banner{margin:0 0 8px 0;padding:9px 10px;border-radius:12px;border:1px solid #c7d6e8;background:#edf3fc}
    .health-banner strong{font-weight:780}
    .health-banner .hint{display:block;margin-top:4px;color:#375173}
    .health-ok{border-color:#b8ddca;background:#edf9f2}
    .health-warn{border-color:#e7d5aa;background:#fff8ea}
    .health-bad{border-color:#efc3b9;background:#fff3ef}

    .two-col{display:grid;grid-template-columns:1fr 1fr;gap:8px}
    .card{background:rgba(255,255,255,.58);border:1px solid var(--line);border-radius:14px;padding:8px}
    .kv{width:100%;border-collapse:collapse;table-layout:fixed}
    .kv th,.kv td{border:1px solid var(--line2);padding:6px 7px;vertical-align:middle;line-height:1.26}
    .kv th{width:31%;text-align:left;background:#e8eef9;font-size:clamp(11px,.82vw,13px)}
    .kv td{background:#f9fbff;font-size:clamp(11px,.82vw,13px)}
    .small-kv th,.small-kv td{font-size:clamp(11px,.78vw,12px);padding:5px 6px}
    #logsTbl{table-layout:fixed;width:100%}
    #logsTbl th:nth-child(1),#logsTbl td:nth-child(1){width:48px;text-align:right}
    #logsTbl th:nth-child(2),#logsTbl td:nth-child(2){width:72px;text-align:right}
    #logsTbl th:nth-child(3),#logsTbl td:nth-child(3){width:170px}
    #logsTbl th:nth-child(4),#logsTbl td:nth-child(4){width:82px}
    #logsTbl th:nth-child(5),#logsTbl td:nth-child(5){width:auto}
    #logsTbl td:nth-child(5){white-space:normal;overflow-wrap:anywhere}
    #mqttBrokersTbl{table-layout:fixed;width:100%}
    #mqttBrokersTbl th:nth-child(1),#mqttBrokersTbl td:nth-child(1){width:44px;text-align:right}
    #mqttBrokersTbl th:nth-child(2),#mqttBrokersTbl td:nth-child(2){width:280px}
    #mqttBrokersTbl th:nth-child(3),#mqttBrokersTbl td:nth-child(3){width:86px;text-align:center}
    #mqttBrokersTbl th:nth-child(4),#mqttBrokersTbl td:nth-child(4){width:108px;text-align:right}
    #mqttBrokersTbl th:nth-child(5),#mqttBrokersTbl td:nth-child(5){width:182px}
    #mqttBrokersTbl th:nth-child(6),#mqttBrokersTbl td:nth-child(6){width:94px;text-align:right}
    #mqttBrokersTbl th:nth-child(7),#mqttBrokersTbl td:nth-child(7){width:80px;text-align:right}
    #mqttBrokersTbl th:nth-child(8),#mqttBrokersTbl td:nth-child(8){width:88px;text-align:right}
    #mqttBrokersTbl th:nth-child(9),#mqttBrokersTbl td:nth-child(9){width:88px;text-align:right}
    #mqttBrokersTbl th:nth-child(10),#mqttBrokersTbl td:nth-child(10){width:88px;text-align:right}
    #mqttBrokersTbl td:nth-child(2),#mqttBrokersTbl td:nth-child(5){white-space:normal;overflow-wrap:anywhere;word-break:break-word}
    .mqtt-broker-cell{display:grid;gap:2px;line-height:1.2}
    .mqtt-broker-cell .name{font-weight:760}
    .mqtt-broker-cell .uri{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px;color:#334f76}

    .neighbors-legend{margin:8px 0 7px 0;font-size:clamp(11px,.82vw,13px);color:#2e4264}
    .dot{width:14px;height:14px;border-radius:50%;display:inline-block;vertical-align:middle;margin-right:6px}
    .dot.self{background:#2164d0}
    .dot.nb{background:#d78320}
    #neighborsMapWrap{background:#f0f4fb;border:1px solid #c4d3e8;border-radius:14px;padding:10px}
    #neighborsMap{height:360px;border-radius:10px;border:1px solid #c2d0e4;overflow:hidden;background:#d8e2f1}
    .map-meta{margin-top:8px;color:#496082;font-size:13px}

    .section-title{font-size:clamp(17px,1.25vw,22px);margin:1px 0 8px 0;font-weight:750}
    .group{margin-bottom:10px;border:1px solid var(--line);border-radius:13px;background:rgba(255,255,255,.58)}
    .group h4{margin:0;padding:9px 11px;font-size:clamp(13px,1.0vw,16px);background:#e8eef9;border-bottom:1px solid var(--line);border-radius:12px 12px 0 0}
    .group .body{padding:8px}
    .edit td{padding:6px}
    .edit input,.edit select,.edit textarea{width:100%;border:1px solid #c6d3e7;border-radius:12px;background:#fff;color:#1f2b3d;padding:7px 9px;font-size:clamp(12px,.85vw,14px)}
    .edit textarea{min-height:70px;resize:vertical}
    .inline-buttons{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
    .btn{border:1px solid #9eb7dc;border-radius:12px;padding:7px 12px;background:#e7effb;color:#1f4c8f;font-size:clamp(12px,.82vw,14px);font-weight:700;cursor:pointer}
    .btn:hover{filter:brightness(.98)}
    .btn:disabled{opacity:.6;cursor:not-allowed;filter:none}
    .btn.primary{background:#2f6fd6;border-color:#2f6fd6;color:#fff}
    .btn.warn{background:var(--danger-bg);border-color:#e7b6b1;color:#7d231f}
    .btn.ghost{background:#f4f7fc;color:#2a456f}
    .sub-note{font-size:clamp(11px,.78vw,13px);color:#476085;margin-top:6px}
    .ok-note{color:#0f6d4f;font-weight:700;font-size:clamp(11px,.8vw,13px)}
    .bad-note{color:#8f1f1f;font-weight:700;font-size:clamp(11px,.8vw,13px)}

    #pickerMapWrap{margin-top:8px;border:1px solid #c6d4e8;border-radius:12px;background:#f0f4fa;padding:8px}
    #pickerMap{height:240px;border-radius:10px;border:1px solid #c6d4e8;overflow:hidden;background:#d8e2f1}

    details.section{border-top:1px solid var(--line)}
    details.section summary{cursor:pointer;list-style:none;padding:9px 11px;font-size:clamp(12px,.9vw,15px);font-weight:730;background:#e8eef9}
    details.section[open] summary{border-bottom:1px solid var(--line)}

    .alert{padding:9px;border:1px solid #efc3b9;background:#fff3ef;color:#8d372b;border-radius:10px;font-size:clamp(11px,.8vw,13px)}

    .sticky{position:fixed;left:0;right:0;bottom:0;border-top:1px solid #c8d4e5;background:#f2f6fcdd;backdrop-filter:blur(4px);padding:7px 10px;display:flex;justify-content:space-between;gap:9px;align-items:center;z-index:50}
    .sticky .state-wrap{display:grid;gap:2px;min-width:260px;max-width:48%}
    .sticky .state{font-size:clamp(12px,.82vw,14px);color:#4c6184;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
    .sticky .state.form{font-weight:740}
    .sticky .state.form.ok{color:#0d6f4d}
    .sticky .state.form.err{color:#8f2622}
    .sticky .state.op{font-size:clamp(11px,.76vw,13px);color:#5f7090}
    .sticky .state.op.err{color:#8f2622}
    .sticky .state.op.ok{color:#355b8b}
    .sticky .actions{display:flex;gap:12px;flex-wrap:wrap;justify-content:flex-end}
    .sticky .actions-main{display:flex;gap:8px;flex-wrap:wrap}
    .sticky .actions-critical{display:flex;align-items:center;padding-left:10px;margin-left:2px;border-left:1px solid #c8d4e5}
    .sticky .critical-label{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:#6b5d5a;margin-right:7px}

    .barset{display:inline-flex;gap:2px;vertical-align:middle;margin-right:8px;align-items:flex-end}
    .barset i{display:inline-block;width:6px;border-radius:4px;background:#d3d9e4}
    .barset i:nth-child(1){height:7px}
    .barset i:nth-child(2){height:10px}
    .barset i:nth-child(3){height:13px}
    .barset i:nth-child(4){height:16px}
    .barset i:nth-child(5){height:19px}
    .sig-1 i:nth-child(1){background:#cb3d3d}
    .sig-2 i:nth-child(-n+2){background:#d87d22}
    .sig-3 i:nth-child(-n+3){background:#d5a11e}
    .sig-4 i:nth-child(-n+4){background:#7ba82f}
    .sig-5 i:nth-child(-n+5){background:#2f9c43}
    .mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}

    @media (max-width:1200px){
      h1{font-size:26px} h2{font-size:22px}
      .chip,.updated,.tab{font-size:13px}
      .section-title{font-size:20px}
      .quick .k{font-size:10px}.quick .v{font-size:20px}.quick .m{font-size:13px}
      .kv th,.kv td{font-size:12px}
      .group h4,details.section summary{font-size:14px}
      .edit input,.edit select,.edit textarea,.btn{font-size:13px}
      .sticky .state{font-size:13px}
      .sub-note,.ok-note,.bad-note,.alert{font-size:12px}
    }
    @media (max-width:960px){
      .quick-grid,.two-col{grid-template-columns:1fr}
      .wrap{width:calc(100% - 8px)}
      .sticky{position:static;border:1px solid var(--line);border-radius:12px;margin:12px 6px;padding:10px;flex-direction:column;align-items:stretch}
      .sticky .actions{justify-content:flex-start}
      .sticky .state-wrap{max-width:100%}
      .sticky .actions-critical{border-left:none;padding-left:0;margin-left:0}
    }
    @media (max-width:760px){
      #mqttBrokersTbl{table-layout:auto}
      #mqttBrokersTbl tr:first-child{display:none}
      #mqttBrokersTbl tbody,#mqttBrokersTbl tr,#mqttBrokersTbl td{display:block;width:100%}
      #mqttBrokersTbl tr{border:1px solid #c6d4e8;border-radius:12px;margin-bottom:8px;overflow:hidden}
      #mqttBrokersTbl td{display:grid;grid-template-columns:118px 1fr;gap:8px;padding:7px 8px;border:0;border-top:1px solid #e1e8f3}
      #mqttBrokersTbl td:first-child{border-top:0}
      #mqttBrokersTbl td::before{content:attr(data-label);font-weight:730;color:#456084}
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="panel">
      <div class="header-top">
        <div class="header-main">
          <h1>MeshCore Repeater</h1>
          <div class="chips">
            <div class="chip"><b>Firmware</b> <span id="hdrFirmware">-</span></div>
            <div class="chip"><b>Build</b> <span id="hdrBuild">-</span></div>
            <div class="chip"><b>IP</b> <span id="hdrIP">-</span></div>
            <div class="chip"><b>SSID</b> <span id="hdrSSID">-</span></div>
          </div>
        </div>
      </div>
    </div>

    <div class="tabs" role="tablist" aria-label="Main sections">
      <button id="tab-btn-status" class="tab active" data-tab="status" role="tab" aria-selected="true" aria-controls="tab-status" tabindex="0">Status</button>
      <button id="tab-btn-mqtt" class="tab" data-tab="mqtt" role="tab" aria-selected="false" aria-controls="tab-mqtt" tabindex="-1">MQTT</button>
      <button id="tab-btn-endpoints" class="tab" data-tab="endpoints" role="tab" aria-selected="false" aria-controls="tab-endpoints" tabindex="-1">Endpoints</button>
      <button id="tab-btn-config" class="tab" data-tab="config" role="tab" aria-selected="false" aria-controls="tab-config" tabindex="-1">Configuration</button>
      <button id="tab-btn-logs" class="tab" data-tab="logs" role="tab" aria-selected="false" aria-controls="tab-logs" tabindex="-1">Logs</button>
    </div>

    <section id="tab-status" class="tab-body" role="tabpanel" aria-labelledby="tab-btn-status">
      <div class="panel">
        <div class="section-title">Quick Health</div>
        <div id="healthBanner" class="health-banner health-ok"><strong>System status:</strong> waiting for data...</div>
        <div class="quick-grid">
          <div class="quick">
            <div class="k">WiFi</div>
            <div class="v" id="qWifiState">-</div>
            <div class="m" id="qWifiMeta">-</div>
          </div>
          <div class="quick">
            <div class="k">Port Clients</div>
            <div class="v" id="qPortsClients">-</div>
            <div class="m mono" id="qPortsMeta">-</div>
          </div>
          <div class="quick">
            <div class="k">Neighbors</div>
            <div class="v" id="qNeighborsCount">-</div>
            <div class="m" id="qNeighborsMeta">-</div>
          </div>
          <div class="quick">
            <div class="k">MQTT</div>
            <div class="v" id="qMqttState">-</div>
            <div class="m" id="qMqttMeta">-</div>
          </div>
        </div>
      </div>
      <details class="section" id="statusDiagnostics">
        <summary>Expand Diagnostics</summary>
        <div class="panel">
          <div class="section-title">Runtime Status</div>
          <div class="two-col">
            <div class="card">
              <h3>WiFi</h3>
              <table class="kv small-kv" id="wifiTbl"></table>
            </div>
            <div class="card">
              <h3>Radio</h3>
              <table class="kv small-kv" id="radioTbl"></table>
            </div>
            <div class="card">
              <h3>TCP</h3>
              <table class="kv small-kv" id="tcpTbl"></table>
            </div>
            <div class="card">
              <h3>MQTT Runtime</h3>
              <table class="kv small-kv" id="mqttTbl"></table>
            </div>
          </div>
        </div>
        <div class="panel">
          <div style="display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap">
            <div class="section-title">Visible Neighbors</div>
            <button class="btn ghost" id="btnLoadMap">Load Map</button>
          </div>
          <table class="kv small-kv" id="nbTbl"></table>
          <div class="neighbors-legend">Map: this node is blue, neighbors with known location are orange.</div>
          <div id="neighborsMapWrap">
            <div id="neighborsMap"></div>
            <div class="map-meta" id="neighborsMapMeta">Known neighbor locations: 0</div>
          </div>
        </div>
      </details>
    </section>

    <section id="tab-mqtt" class="tab-body hidden" role="tabpanel" aria-labelledby="tab-btn-mqtt">
      <div class="panel">
        <div class="section-title">MQTT Runtime</div>
        <table class="kv small-kv" id="mqttOnlyRuntimeTbl"></table>
      </div>
      <div class="panel">
        <div class="section-title">MQTT Brokers</div>
        <table class="kv small-kv" id="mqttBrokersTbl"></table>
      </div>
      <div class="panel">
        <div class="section-title">MQTT Build Profiles</div>
        <table class="kv small-kv" id="mqttOnlyBuildTbl"></table>
      </div>
    </section>

    <section id="tab-endpoints" class="tab-body hidden" role="tabpanel" aria-labelledby="tab-btn-endpoints">
      <div class="panel">
        <div class="section-title">Endpoints</div>
        <table class="kv small-kv" id="endpointsTbl"></table>
      </div>
    </section>

    <section id="tab-config" class="tab-body hidden" role="tabpanel" aria-labelledby="tab-btn-config">
      <div class="panel">
        <div class="section-title">Configuration</div>

        <div class="group">
          <h4>Identity</h4>
          <div class="body">
            <table class="kv edit">
              <tr><th>Node Name</th><td><input id="cfg_node_name" name="node_name" autocomplete="off"></td></tr>
              <tr><th>Node Name Routing Hash</th><td><div class="mono" id="nodeHashLine">-</div><div id="nodeHashWarn" class="ok-note">-</div></td></tr>
              <tr><th>Node Latitude</th><td><input id="cfg_node_lat" name="node_lat" inputmode="decimal"></td></tr>
              <tr><th>Node Longitude</th><td><input id="cfg_node_lon" name="node_lon" inputmode="decimal"></td></tr>
              <tr>
                <th>Location Picker</th>
                <td>
                  <div class="inline-buttons">
                    <button type="button" class="btn ghost" id="btnPickMap">Pick on map</button>
                    <button type="button" class="btn ghost" id="btnGps">Use browser GPS</button>
                  </div>
                  <div class="sub-note">Click on map to set Node Latitude/Longitude.</div>
                  <div id="pickerMapWrap"><div id="pickerMap"></div></div>
                </td>
              </tr>
            </table>
          </div>
        </div>

        <div class="group">
          <h4>Radio Settings</h4>
          <div class="body">
            <table class="kv edit">
              <tr><th>Radio Preset</th><td>
                <select id="cfg_radio_preset">
                  <option value="custom">Custom</option>
                  <option value="eu_868_long">EU 868 Long Range (869.525 / 125 / SF11 / CR5)</option>
                  <option value="eu_868_mesh">EU 868 Mesh (869.618 / 62.5 / SF8 / CR5)</option>
                  <option value="us_915_fast">US 915 Fast (915 / 250 / SF9 / CR5)</option>
                </select>
              </td></tr>
              <tr><th>LoRa Freq</th><td><input id="cfg_freq" name="freq" inputmode="decimal"></td></tr>
              <tr><th>LoRa BW</th><td><input id="cfg_bw" name="bw" inputmode="decimal"></td></tr>
              <tr><th>LoRa SF</th><td><input id="cfg_sf" name="sf" inputmode="numeric"></td></tr>
              <tr><th>LoRa CR</th><td><input id="cfg_cr" name="cr" inputmode="numeric"></td></tr>
              <tr><th>TX Power dBm</th><td><input id="cfg_tx_power" name="tx_power_dbm" inputmode="numeric"></td></tr>
              <tr><th>Advert Interval (mins)</th><td><input id="cfg_advert_interval" name="advert_interval" inputmode="numeric"></td></tr>
              <tr><th>Flood Advert Interval (hours)</th><td><input id="cfg_flood_advert_interval" name="flood_advert_interval" inputmode="numeric"></td></tr>
              <tr><th>Flood Max</th><td><input id="cfg_flood_max" name="flood_max" inputmode="numeric"></td></tr>
              <tr><th>Airtime Factor</th><td><input id="cfg_airtime_factor" name="airtime_factor" inputmode="decimal"></td></tr>
              <tr><th>RX Delay Base</th><td><input id="cfg_rx_delay_base" name="rx_delay_base" inputmode="decimal"></td></tr>
              <tr><th>TX Delay Factor</th><td><input id="cfg_tx_delay_factor" name="tx_delay_factor" inputmode="decimal"></td></tr>
              <tr><th>Direct TX Delay Factor</th><td><input id="cfg_direct_tx_delay_factor" name="direct_tx_delay_factor" inputmode="decimal"></td></tr>
              <tr><th>Bridge Enabled</th><td><input id="cfg_bridge_enabled" name="bridge_enabled" type="checkbox" style="width:auto"></td></tr>
              <tr><th>Bridge Delay (ms)</th><td><input id="cfg_bridge_delay" name="bridge_delay" inputmode="numeric"></td></tr>
              <tr><th>Bridge Packet Source</th><td>
                <select id="cfg_bridge_pkt_src" name="bridge_pkt_src"><option value="logTx">logTx</option><option value="logRx">logRx</option></select>
              </td></tr>
              <tr><th>Bridge Baud</th><td><input id="cfg_bridge_baud" name="bridge_baud" inputmode="numeric"></td></tr>
              <tr><th>Bridge Channel</th><td><input id="cfg_bridge_channel" name="bridge_channel" inputmode="numeric"></td></tr>
              <tr><th>GPS Enabled</th><td><input id="cfg_gps_enabled" name="gps_enabled" type="checkbox" style="width:auto"></td></tr>
              <tr><th>GPS Interval (s)</th><td><input id="cfg_gps_interval" name="gps_interval" inputmode="numeric"></td></tr>
            </table>
          </div>
        </div>

        <details class="section">
          <summary>Advanced Routing</summary>
          <div class="body">
            <table class="kv edit">
              <tr><th>Path Hash Mode</th><td><select id="cfg_path_hash_mode" name="path_hash_mode"><option value="0">0</option><option value="1">1</option><option value="2">2</option></select></td></tr>
              <tr><th>Loop Detect</th><td><select id="cfg_loop_detect" name="loop_detect"><option value="off">off</option><option value="minimal">minimal</option><option value="moderate">moderate</option><option value="strict">strict</option></select></td></tr>
              <tr><th>Interference Threshold</th><td><input id="cfg_interference_threshold" name="interference_threshold" inputmode="numeric"></td></tr>
              <tr><th>AGC Reset Interval (s)</th><td><input id="cfg_agc_reset_interval" name="agc_reset_interval" inputmode="numeric"></td></tr>
              <tr><th>Multi ACKs</th><td><input id="cfg_multi_acks" name="multi_acks" inputmode="numeric"></td></tr>
              <tr><th>Allow Read Only</th><td><input id="cfg_allow_read_only" name="allow_read_only" type="checkbox" style="width:auto"></td></tr>
            </table>
          </div>
        </details>

        <details class="section">
          <summary>Security</summary>
          <div class="body">
            <table class="kv edit">
              <tr><th>Password</th><td><input id="cfg_password" name="password" type="password" autocomplete="new-password"></td></tr>
              <tr><th>Guest Password</th><td><input id="cfg_guest_password" name="guest_password" type="password" autocomplete="new-password"></td></tr>
              <tr><th>Owner Info</th><td><textarea id="cfg_owner_info" name="owner_info"></textarea></td></tr>
            </table>
          </div>
        </details>

        <details class="section">
          <summary>Build-time Ports</summary>
          <div class="body">
            <table class="kv small-kv" id="portsTbl"></table>
          </div>
        </details>
      </div>
    </section>

    <section id="tab-logs" class="tab-body hidden" role="tabpanel" aria-labelledby="tab-btn-logs">
      <div class="panel">
        <div style="display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap">
          <div class="section-title">Runtime Logs</div>
          <button class="btn ghost" id="btnClearLogs">Clear Logs</button>
        </div>
        <table class="kv small-kv" id="logsTbl"></table>
      </div>
      <div class="panel">
        <div class="section-title">Neighbors (Raw)</div>
        <table class="kv small-kv" id="nbRawTbl"></table>
      </div>
    </section>
  </div>

  <div class="sticky">
    <div class="state-wrap">
      <div class="state form ok" id="formState">No unsaved changes</div>
      <div class="state op ok" id="opState">Ready</div>
    </div>
    <div class="actions">
      <div class="actions-main">
        <button class="btn primary" id="btnSave">Save</button>
        <button class="btn" id="btnAdvert">Send Advert</button>
        <button class="btn" id="btnSync">Sync Clock</button>
        <button class="btn" id="btnReload">Reload</button>
        <button class="btn" id="btnExport">Export</button>
        <button class="btn" id="btnImport">Import</button>
      </div>
      <div class="actions-critical">
        <span class="critical-label">Critical</span>
        <button class="btn warn" id="btnReboot">Reboot</button>
      </div>
      <input type="file" id="importFile" accept="application/json" class="hidden">
    </div>
  </div>

  <script>
    const $ = (id)=>document.getElementById(id);
    const fmt = (v,d='-') => (v===undefined||v===null||v==='') ? d : String(v);
    const nowSec = ()=>Math.floor(Date.now()/1000);
    const asNum = (v)=>{ const n = Number(String(v).replace(',', '.')); return Number.isFinite(n) ? n : NaN; };
    const enc = (s)=>String(s ?? '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
    const state = {
      stats:null, cfg:null, cfgLoaded:false, dirty:false,
      map:null, picker:null, pickerMarker:null, nbLayer:null,
      pickerArmed:true, mapEnabled:false, currentTab:'status', busy:false,
      cfgPrefetchScheduled:false
    };

    const signalBars = (dbmOrSnr, kind='rssi') => {
      let lvl = 1, label = 'NO SIGNAL';
      if(kind==='rssi'){
        const v = Number(dbmOrSnr);
        if(v >= -65){lvl=5;label='EXCELLENT';}
        else if(v >= -75){lvl=4;label='GOOD';}
        else if(v >= -85){lvl=3;label='FAIR';}
        else if(v >= -95){lvl=2;label='WEAK';}
        else {lvl=1;label='VERY WEAK';}
      }else{
        const v = Number(dbmOrSnr);
        if(v >= 10){lvl=5;label='EXCELLENT';}
        else if(v >= 6){lvl=4;label='GOOD';}
        else if(v >= 2){lvl=3;label='FAIR';}
        else if(v >= -5){lvl=2;label='WEAK';}
        else {lvl=1;label='NOISY';}
      }
      return `<span class="barset sig-${lvl}"><i></i><i></i><i></i><i></i><i></i></span>${enc(Number(dbmOrSnr).toFixed(kind==='rssi'?0:2))} ${kind==='rssi'?'dBm':'dB'} ${label}`;
    };

    let opStateTimer = null;
    const setFormState = (msg, ok=true)=>{
      const n = $('formState');
      if(!n) return;
      n.textContent = msg;
      n.className = 'state form ' + (ok ? 'ok' : 'err');
    };
    const setOpState = (msg, ok=true, keep=false)=>{
      const n = $('opState');
      if(!n) return;
      n.textContent = msg;
      n.className = 'state op ' + (ok ? 'ok' : 'err');
      if(!ok) keep = true;
      if(opStateTimer){
        clearTimeout(opStateTimer);
        opStateTimer = null;
      }
      if(!keep){
        opStateTimer = setTimeout(()=>{
          n.textContent = 'Ready';
          n.className = 'state op ok';
          opStateTimer = null;
        }, 5500);
      }
    };
    const markDirty = ()=>{ if(!state.dirty){ state.dirty=true; setFormState('Unsaved changes', false); } };
    const clearDirty = ()=>{ state.dirty=false; setFormState('No unsaved changes', true); };
    const setBusy = (busy)=>{
      state.busy = !!busy;
      ['btnSave','btnAdvert','btnSync','btnReload','btnReboot','btnExport','btnImport','btnClearLogs','btnLoadMap'].forEach((id)=>{
        const e = $(id);
        if (e) e.disabled = !!busy;
      });
    };

    function refreshActionButtons(){
      const perTab = {
        status: ['btnAdvert', 'btnSync', 'btnReload'],
        mqtt: ['btnReload'],
        endpoints: ['btnReload'],
        config: ['btnSave', 'btnImport', 'btnExport', 'btnReload'],
        logs: ['btnReload']
      };
      const visible = new Set(perTab[state.currentTab] || ['btnReload']);
      ['btnSave','btnAdvert','btnSync','btnReload','btnExport','btnImport'].forEach((id)=>{
        const e = $(id);
        if (!e) return;
        e.classList.toggle('hidden', !visible.has(id));
      });
    }

    function uptimeText(ms){
      const total = Math.max(0, Math.floor((Number(ms)||0)/1000));
      const d = Math.floor(total/86400), h=Math.floor((total%86400)/3600), m=Math.floor((total%3600)/60), s=total%60;
      const part=[]; if(d) part.push(d+'d'); if(h||d) part.push(h+'h'); if(m||h||d) part.push(m+'m'); part.push(s+'s');
      return part.join(' ');
    }

    function fmtUtc(ts){
      const n = Number(ts || 0);
      if(!Number.isFinite(n) || n <= 0) return '-';
      return new Date(n * 1000).toISOString().slice(0, 19).replace('T', ' ');
    }

    function fnv1a8(str){ let h=0x811c9dc5; for(let i=0;i<str.length;i++){ h^=str.charCodeAt(i); h=(h>>>0)*0x01000193; } return ((h>>>0)&0xff); }

    function tableFromPairs(el, rows){
      if(!el) return;
      el.innerHTML = rows.map(([k,v])=>`<tr><th>${enc(k)}</th><td>${v}</td></tr>`).join('');
    }

    let leafletLoadPromise = null;
    function mapSafe(){ return typeof window.L !== 'undefined'; }
    function ensureLeafletLoaded(){
      if(mapSafe()) return Promise.resolve(true);
      if(leafletLoadPromise) return leafletLoadPromise;
      leafletLoadPromise = new Promise((resolve, reject)=>{
        const fail = (msg)=>{
          leafletLoadPromise = null;
          reject(new Error(msg));
        };
        if(!document.getElementById('leaflet-css')){
          const link = document.createElement('link');
          link.id = 'leaflet-css';
          link.rel = 'stylesheet';
          link.href = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
          link.crossOrigin = '';
          document.head.appendChild(link);
        }
        if(document.getElementById('leaflet-js')){
          let retries = 20;
          const waitReady = ()=>{
            if(mapSafe()) return resolve(true);
            retries--;
            if(retries <= 0) return fail('Leaflet runtime did not initialize');
            setTimeout(waitReady, 150);
          };
          waitReady();
          return;
        }
        const script = document.createElement('script');
        script.id = 'leaflet-js';
        script.src = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
        script.crossOrigin = '';
        script.async = true;
        script.onload = ()=> mapSafe() ? resolve(true) : fail('Leaflet loaded but unavailable');
        script.onerror = ()=> fail('Leaflet script download failed');
        document.body.appendChild(script);
      });
      return leafletLoadPromise;
    }

    function renderHeader(s){
      $('hdrFirmware').textContent = fmt(s.firmware_version);
      $('hdrBuild').textContent = fmt(s.build_date);
      $('hdrIP').textContent = fmt(s.wifi?.ip);
      $('hdrSSID').textContent = fmt(s.wifi?.ssid);
    }

    function renderEndpoints(){
      tableFromPairs($('endpointsTbl'), [
        ['Runtime Stats', '<a href="/stats" target="_blank" rel="noopener noreferrer">/stats</a>'],
        ['Config Export', '<a href="/config-export" target="_blank" rel="noopener noreferrer">/config-export</a>'],
        ['Health Check', '<a href="/health" target="_blank" rel="noopener noreferrer">/health</a>'],
      ]);
    }

    function mqttConnectionState(s, c){
      const cfg = c || {};
      const m = s?.mqtt || {};
      const cfgEnabledProfiles =
        (cfg.mqtt_profile_letsmesh_us_enabled ? 1 : 0) +
        (cfg.mqtt_profile_letsmesh_eu_enabled ? 1 : 0) +
        (cfg.mqtt_profile_extra_enabled ? 1 : 0) +
        (cfg.mqtt_profile_custom_enabled ? 1 : 0);
      const enabledProfiles = Number.isFinite(Number(m.enabled_profiles))
        ? Number(m.enabled_profiles)
        : cfgEnabledProfiles;
      const runtimeConnected = Number.isFinite(Number(m.connected_brokers))
        ? Number(m.connected_brokers)
        : (m.connected ? 1 : 0);
      const connectedProfiles = Math.min(runtimeConnected, Math.max(enabledProfiles, 0));
      return {enabledProfiles, connectedProfiles};
    }

    function renderHealthBanner(s, c){
      const banner = $('healthBanner');
      const wifiOk = !!s.wifi?.connected;
      const neighbors = (Array.isArray(s.neighbors) ? s.neighbors : []).length;
      const mqttState = mqttConnectionState(s, c);
      let level = 'ok';
      const tips = [];
      if(!wifiOk){
        level = 'bad';
        tips.push('Wi-Fi is offline. Check AP range and credentials.');
      }
      if(mqttState.enabledProfiles > 0 && mqttState.connectedProfiles < mqttState.enabledProfiles){
        if(level !== 'bad') level = 'warn';
        tips.push('Some MQTT brokers are offline. Open MQTT tab and verify broker status.');
      }
      if(neighbors === 0){
        if(level === 'ok') level = 'warn';
        tips.push('No neighbors visible. Send Advert and verify LoRa preset.');
      }
      const status = level === 'ok' ? 'OK' : (level === 'warn' ? 'Warning' : 'Error');
      const hint = tips.length ? tips.slice(0, 2).join(' ') : 'All key services are operating within expected range.';
      banner.className = `health-banner health-${level}`;
      banner.innerHTML = `<strong>System status: ${enc(status)}</strong><span class="hint">${enc(hint)}</span>`;
    }

    function renderQuick(s, c){
      $('qWifiState').textContent = s.wifi?.connected ? 'Online' : 'Offline';
      $('qWifiMeta').innerHTML = signalBars(s.wifi?.rssi || -120, 'rssi');
      const p = s.ports || {};
      const totalClients = Number(p.raw_active_clients||0)+Number(p.console_active_clients||0)+Number(p.mirror_active_clients||0);
      $('qPortsClients').textContent = String(totalClients);
      $('qPortsMeta').innerHTML =
        `<div class="port-lines">` +
          `<div class="row"><span class="lbl">RAW ${enc(fmt(p.raw_port))}</span><span class="val">${enc(fmt(p.raw_active_clients,0))}</span></div>` +
          `<div class="row"><span class="lbl">CONSOLE ${enc(fmt(p.console_port))}</span><span class="val">${enc(fmt(p.console_active_clients,0))}</span></div>` +
          `<div class="row"><span class="lbl">MIRROR ${enc(fmt(p.mirror_port))}</span><span class="val">${enc(fmt(p.mirror_active_clients,0))}</span></div>` +
        `</div>`;
      const nbs = Array.isArray(s.neighbors)?s.neighbors:[];
      $('qNeighborsCount').textContent = String(nbs.length);
      $('qNeighborsMeta').textContent = nbs.length ? 'Visible now' : 'No visible neighbors';
      const mqttState = mqttConnectionState(s, c);
      $('qMqttState').textContent = `${mqttState.connectedProfiles}/${mqttState.enabledProfiles}`;
      $('qMqttMeta').textContent = mqttState.enabledProfiles ? `${mqttState.connectedProfiles} online` : 'not configured';
      renderHealthBanner(s, c || {});
    }

    function renderRuntime(s){
      const rt = s.radio||{}, w=s.wifi||{}, t=s.tcp||{}, m=s.mqtt||{}, p=s.ports||{};
      const heap = s.heap||{};
      tableFromPairs($('wifiTbl'), [
        ['Connected', enc(String(!!w.connected))],
        ['SSID', enc(fmt(w.ssid))],
        ['IP', enc(fmt(w.ip))],
        ['RSSI', signalBars(w.rssi||-120,'rssi')],
      ]);
      tableFromPairs($('radioTbl'), [
        ['Battery mV', enc(fmt(rt.battery_mv,0))],
        ['Noise Floor', signalBars(rt.noise_floor||-120,'rssi')],
        ['Last RSSI', signalBars(rt.last_rssi||-120,'rssi')],
        ['Last SNR', signalBars((rt.last_snr_x4||0)/4,'snr')],
        ['Device Time (UTC)', enc(new Date((s.timestamp||0)*1000).toISOString().replace('T',' ').replace('.000Z',' UTC') + ' (' + fmt(s.timestamp,0) + ')')],
        ['Heap Total', enc(fmt(heap.total,0))],
        ['Free Heap', enc(`${fmt(heap.free,0)} / ${fmt(heap.total,0)} (${fmt(heap.free_pct,0)}%)`)],
        ['Min Free Heap', enc(`${fmt(heap.min_free,0)} / ${fmt(heap.total,0)} (${fmt(heap.min_free_pct,0)}%)`)],
        ['Max Alloc Heap', enc(`${fmt(heap.max_alloc,0)} / ${fmt(heap.total,0)} (${fmt(heap.max_alloc_pct,0)}%)`)],
        ['Packets RX', enc(fmt(rt.packets_rx,0))],
        ['Packets TX', enc(fmt(rt.packets_tx,0))],
        ['RX Errors', enc(fmt(rt.recv_errors,0))],
        ['TX Air Secs', enc(fmt(rt.tx_air_secs,0))],
        ['RX Air Secs', enc(fmt(rt.rx_air_secs,0))],
        ['Device Start (UTC)', enc((() => { const ms=Number(rt.uptime_ms||0); const sec=Math.floor(ms/1000); const ts=Number(s.timestamp||0); return new Date((ts-sec)*1000).toISOString().replace('T',' ').replace('.000Z',' UTC') + ' (' + String(ts-sec) + ')'; })())],
        ['Uptime', enc(uptimeText(rt.uptime_ms))],
        ['Queue Len', enc(fmt(rt.queue_len,0))],
        ['Err Flags', enc(fmt(rt.err_flags,0))],
        ['Sent Flood', enc(fmt(rt.sent_flood,0))],
        ['Sent Direct', enc(fmt(rt.sent_direct,0))],
        ['Recv Flood', enc(fmt(rt.recv_flood,0))],
        ['Recv Direct', enc(fmt(rt.recv_direct,0))],
        ['Direct Dups', enc(fmt(rt.direct_dups,0))],
        ['Flood Dups', enc(fmt(rt.flood_dups,0))],
        ['Neighbors Seen', enc(fmt((Array.isArray(s.neighbors)?s.neighbors.length:0),0))],
      ]);
      tableFromPairs($('tcpTbl'), [
        ['Raw Port', enc(fmt(p.raw_port, t.port))],
        ['Raw Active Clients', enc(fmt(p.raw_active_clients, t.active_clients))],
        ['Console Port', enc(fmt(p.console_port, '-'))],
        ['Console Active Clients', enc(fmt(p.console_active_clients, '-'))],
        ['Mirror Port', enc(fmt(p.mirror_port, '-'))],
        ['Mirror Active Clients', enc(fmt(p.mirror_active_clients, '-'))],
        ['HTTP Max Parallel Requests', enc(`${fmt(s.http?.max_parallel_requests, 0)} / ${fmt(s.http?.max_client_slots, 0)} (peak/slots)`)],
        ['Total Active Clients', enc(String((Number(p.raw_active_clients||0)+Number(p.console_active_clients||0)+Number(p.mirror_active_clients||0))))],
        ['Clients Accepted', enc(fmt(t.clients_accepted,0))],
        ['Clients Rejected', enc(fmt(t.clients_rejected,0))],
        ['Clients Disconnected', enc(fmt(t.clients_disconnected,0))],
        ['Max Simultaneous', enc(fmt(t.max_simultaneous_clients,0))],
        ['RX Frames', enc(fmt(t.rx_frames,0))],
        ['RX Bytes', enc(fmt(t.rx_bytes,0))],
        ['RX Parse Errors', enc(fmt(t.rx_parse_errors,0))],
        ['RX Checksum Errors', enc(fmt(t.rx_checksum_errors,0))],
        ['TX Frames', enc(fmt(t.tx_frames,0))],
        ['TX Bytes', enc(fmt(t.tx_bytes,0))],
        ['TX Deliveries', enc(fmt(t.tx_deliveries,0))],
        ['TX Delivery Failures', enc(fmt(t.tx_delivery_failures,0))],
      ]);
      tableFromPairs($('mqttTbl'), [
        ['Compile Enabled', enc(String(!!m.enabled))],
        ['Enabled', enc(String(!!m.enabled_runtime))],
        ['Connected', enc(String(!!m.connected))],
        ['Enabled Profiles', enc(fmt(m.enabled_profiles, 0))],
        ['Connected Brokers', enc(fmt(m.connected_brokers, 0))],
        ['Max Slots', enc(fmt(m.max_slots, 0))],
        ['Active Profile', enc(fmt(m.active_profile, '-'))],
        ['Active Host', enc(fmt(m.active_host, '-'))],
        ['Active URI', enc(fmt(m.active_uri, '-'))],
        ['Connect Attempts', enc(fmt(m.connect_attempts,0))],
        ['Reconnects', enc(fmt(m.reconnects,0))],
        ['Publish OK', enc(fmt(m.publish_ok,0))],
        ['Publish Fail', enc(fmt(m.publish_fail,0))],
        ['Last Profile Index', enc(fmt(m.last_profile_index, -1))],
        ['Last Error', enc(fmt(m.last_error_code,0))],
      ]);
    }

    function renderMqttTab(s, c){
      const m = s.mqtt || {};
      const cfg = c || {};
      const mqttState = mqttConnectionState(s, c);
      tableFromPairs($('mqttOnlyRuntimeTbl'), [
        ['Connected / Configured', enc(`${mqttState.connectedProfiles}/${mqttState.enabledProfiles}`)],
        ['Compile Enabled', enc(String(!!m.enabled))],
        ['Enabled Runtime', enc(String(!!m.enabled_runtime))],
        ['Connected', enc(String(!!m.connected))],
        ['Connected Brokers', enc(fmt(m.connected_brokers, 0))],
        ['Enabled Profiles', enc(fmt(m.enabled_profiles, 0))],
        ['Max Slots', enc(fmt(m.max_slots, 0))],
        ['Active Profile', enc(fmt(m.active_profile, '-'))],
        ['Active Host', enc(fmt(m.active_host, '-'))],
        ['Active URI', enc(fmt(m.active_uri, '-'))],
        ['Connect Attempts', enc(fmt(m.connect_attempts, 0))],
        ['Reconnects', enc(fmt(m.reconnects, 0))],
        ['Publish OK', enc(fmt(m.publish_ok, 0))],
        ['Publish Fail', enc(fmt(m.publish_fail, 0))],
        ['Last Profile Index', enc(fmt(m.last_profile_index, -1))],
        ['Last Error', enc(fmt(m.last_error_code, 0))],
      ]);

      const sourceBrokers = Array.isArray(m.brokers) ? m.brokers : [];
      let brokers = sourceBrokers;
      const brokersCount = Number(m.brokers_count);
      if (Number.isFinite(brokersCount) && brokersCount > 0) {
        brokers = sourceBrokers.slice(0, brokersCount);
      }
      const brokerHdr = '<tr><th>LP</th><th>MQTT Broker</th><th>Connected</th><th>Connected Time</th><th>Connected Since (UTC)</th><th>Connect Attempts</th><th>Reconnects</th><th>Publish OK</th><th>Publish Fail</th><th>Last Error</th></tr>';
      const brokerRows = brokers.length ? brokers.map((b, idx) => {
        const lp = Number.isFinite(Number(b.slot)) ? Number(b.slot) : (idx + 1);
        const profile = fmt(b.profile_name, '-');
        const host = fmt(b.host, '-');
        const uri = fmt(b.uri, '-');
        const connected = !!b.connected;
        const connectedFor = connected ? uptimeText(Number(b.connected_for_s || 0) * 1000) : '-';
        const connectedSince = Number(b.connected_since_epoch || 0) > 0 ? `${fmtUtc(b.connected_since_epoch)} UTC` : '-';
        const brokerCell = `<div class="mqtt-broker-cell"><div class="name">${enc(profile)} @ ${enc(host)}</div><div class="uri">${enc(uri)}</div></div>`;
        return `<tr>` +
          `<td data-label="LP">${enc(lp)}</td>` +
          `<td data-label="MQTT Broker">${brokerCell}</td>` +
          `<td data-label="Connected">${enc(connected ? 'yes' : 'no')}</td>` +
          `<td data-label="Connected Time">${enc(connectedFor)}</td>` +
          `<td data-label="Connected Since (UTC)">${enc(connectedSince)}</td>` +
          `<td data-label="Connect Attempts">${enc(fmt(b.connect_attempts, 0))}</td>` +
          `<td data-label="Reconnects">${enc(fmt(b.reconnects, 0))}</td>` +
          `<td data-label="Publish OK">${enc(fmt(b.publish_ok, 0))}</td>` +
          `<td data-label="Publish Fail">${enc(fmt(b.publish_fail, 0))}</td>` +
          `<td data-label="Last Error">${enc(fmt(b.last_error_code, 0))}</td>` +
        `</tr>`;
      }).join('') : '<tr><td colspan="10">no broker stats</td></tr>';
      $('mqttBrokersTbl').innerHTML = brokerHdr + brokerRows;

      tableFromPairs($('mqttOnlyBuildTbl'), [
        ['Config Source', enc('config.env + rebuild')],
        ['Configured Profiles', enc(String(mqttState.enabledProfiles))],
        ['MQTT Compile Enabled', enc(String(!!cfg.mqtt_compile_enabled))],
        ['MQTT Enabled', enc(String(!!cfg.mqtt_enabled))],
        ['LetsMesh US', enc(cfg.mqtt_profile_letsmesh_us_enabled ? `enabled (${fmt(cfg.mqtt_profile_letsmesh_us_host, '-')})` : 'disabled')],
        ['LetsMesh EU', enc(cfg.mqtt_profile_letsmesh_eu_enabled ? `enabled (${fmt(cfg.mqtt_profile_letsmesh_eu_host, '-')})` : 'disabled')],
        ['Extra Profile', enc(cfg.mqtt_profile_extra_enabled ? `enabled (${fmt(cfg.mqtt_profile_extra_host, '-')})` : 'disabled')],
        ['Custom Profile', enc(cfg.mqtt_profile_custom_enabled ? `enabled (${fmt(cfg.mqtt_profile_custom_host, '-')})` : 'disabled')],
        ['MQTT Host', enc(fmt(cfg.mqtt_host, '-'))],
        ['MQTT Port', enc(fmt(cfg.mqtt_port, '-'))],
        ['MQTT Transport', enc(fmt(cfg.mqtt_transport, '-'))],
        ['MQTT WS Path', enc(fmt(cfg.mqtt_ws_path, '-'))],
        ['MQTT Auth Method', enc(fmt(cfg.mqtt_auth_method, '-'))],
        ['MQTT Username', enc(fmt(cfg.mqtt_username, '-'))],
        ['MQTT Client ID', enc(fmt(cfg.mqtt_client_id, '-'))],
        ['MQTT Topic Prefix', enc(fmt(cfg.mqtt_topic_prefix, '-'))],
        ['MQTT IATA', enc(fmt(cfg.mqtt_iata, '-'))],
      ]);
    }

    function renderNeighbors(s){
      const nbs = Array.isArray(s.neighbors)?s.neighbors:[];
      if(!nbs.length){
        $('nbTbl').innerHTML = '<tr><th>#</th><th>ID Prefix</th><th>Name</th><th>Signal (SNR)</th><th>Location</th><th>Last Heard</th></tr><tr><td colspan="6">none</td></tr>';
      } else {
        $('nbTbl').innerHTML = '<tr><th>#</th><th>ID Prefix</th><th>Name</th><th>Signal (SNR)</th><th>Location</th><th>Last Heard</th></tr>' + nbs.map((n,i)=>{
          const loc = n.has_location ? `${Number(n.lat).toFixed(6)}, ${Number(n.lon).toFixed(6)}` : 'n/a';
          return `<tr><td>${i+1}</td><td class="mono">${enc(n.id_prefix||'')}</td><td>${enc(n.name||'n/a')}</td><td>${signalBars(Number(n.snr||0),'snr')}</td><td>${enc(loc)}</td><td>${enc(String(n.heard_secs_ago||0)+' s ago')}</td></tr>`;
        }).join('');
      }

      const lats = nbs.filter(x=>x.has_location).map(x=>Number(x.lat));
      const lons = nbs.filter(x=>x.has_location).map(x=>Number(x.lon));
      const knownCount = lats.length;

      if(!state.mapEnabled){
        $('btnLoadMap').textContent = 'Load Map';
        $('neighborsMap').innerHTML = '<div style="padding:10px;color:#566a8a">Map is disabled. Use "Load Map" to fetch external map tiles.</div>';
        $('neighborsMapMeta').textContent = `Known neighbor locations: ${knownCount} | map disabled`;
        return;
      }

      if(mapSafe()){
        $('btnLoadMap').textContent = 'Map Loaded';
        if(!state.map){
          state.map = L.map('neighborsMap', {zoomControl:true, attributionControl:true});
          L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {maxZoom:19, attribution:'&copy; OpenStreetMap'}).addTo(state.map);
          state.nbLayer = L.layerGroup().addTo(state.map);
        }
        state.nbLayer.clearLayers();
        const points=[];
        const c = state.cfg || {};
        const selfLat = asNum(c.node_lat), selfLon = asNum(c.node_lon);
        if(Number.isFinite(selfLat) && Number.isFinite(selfLon)){
          points.push([selfLat,selfLon]);
          L.circleMarker([selfLat,selfLon], {radius:8,color:'#1f62d0',weight:2,fillColor:'#1f62d0',fillOpacity:0.9}).bindPopup('This node').addTo(state.nbLayer);
        }
        let locCount = 0;
        nbs.forEach((n)=>{
          if(n.has_location){
            const la=Number(n.lat), lo=Number(n.lon);
            if(Number.isFinite(la) && Number.isFinite(lo)){
              locCount++;
              points.push([la,lo]);
              L.circleMarker([la,lo], {radius:7,color:'#1f62d0',weight:1,fillColor:'#d78320',fillOpacity:0.95})
                .bindPopup(`<b>${enc(n.id_prefix||'')}</b><br>${enc(n.name||'n/a')}<br>SNR ${Number(n.snr||0).toFixed(2)} dB`)
                .addTo(state.nbLayer);
            }
          }
        });
        if(points.length===1){ state.map.setView(points[0], 16); }
        else if(points.length>1){ state.map.fitBounds(points, {padding:[22,22]}); }
        else { state.map.setView([53.4742393,14.5911102], 12); }

        if(lats.length){
          $('neighborsMapMeta').textContent = `Known neighbor locations: ${locCount} | Lat ${Math.min(...lats).toFixed(4)}..${Math.max(...lats).toFixed(4)} | Lon ${Math.min(...lons).toFixed(4)}..${Math.max(...lons).toFixed(4)}`;
        } else {
          $('neighborsMapMeta').textContent = 'Known neighbor locations: 0';
        }
      } else {
        $('neighborsMap').innerHTML = '<div style="padding:10px;color:#566a8a">Leaflet map script unavailable.</div>';
        $('neighborsMapMeta').textContent = `Known neighbor locations: ${knownCount} | map unavailable`;
      }
    }

    function renderLogs(s){
      const logs = Array.isArray(s.runtime_logs)?s.runtime_logs:[];
      $('logsTbl').innerHTML = '<tr><th>#</th><th>Age (s)</th><th>Date (UTC)</th><th>Source</th><th>Event</th></tr>' + (logs.length ? logs.map((l,i)=>{
        const msg = String(l.message || '');
        const src = msg.startsWith('MQTT') ? 'MQTT' : (msg.startsWith('TCP') ? 'TCP' : (msg.startsWith('HTTP') ? 'HTTP' : 'SYS'));
        return `<tr><td>${i+1}</td><td>${enc(fmt(l.age_s,0))}</td><td>${enc(fmtUtc(l.timestamp))}</td><td>${enc(src)}</td><td>${enc(msg)}</td></tr>`;
      }).join('') : '<tr><td colspan="5">No logs</td></tr>');
      const nbs = Array.isArray(s.neighbors)?s.neighbors:[];
      $('nbRawTbl').innerHTML = '<tr><th>#</th><th>ID Prefix</th><th>SNR x4 (raw)</th><th>SNR</th><th>Lat</th><th>Lon</th><th>Heard Secs Ago</th><th>Heard Timestamp</th><th>Advert Timestamp</th></tr>' + (nbs.length ? nbs.map((n,i)=>`<tr><td>${i+1}</td><td>${enc(n.id_prefix||'')}</td><td>${enc(fmt(n.snr_x4,0))}</td><td>${enc(Number(n.snr||0).toFixed(2))}</td><td>${enc(n.has_location?Number(n.lat).toFixed(6):'')}</td><td>${enc(n.has_location?Number(n.lon).toFixed(6):'')}</td><td>${enc(fmt(n.heard_secs_ago,0))}</td><td>${enc(fmt(n.heard_timestamp,0))}</td><td>${enc(fmt(n.advert_timestamp,0))}</td></tr>`).join('') : '<tr><td colspan="9">none</td></tr>');
    }

    function fillConfig(c, s){
      const val = (id,k,d='') => { const e=$(id); if(!e) return; if(e.type==='checkbox') e.checked = !!c[k]; else e.value = (c[k] ?? d); };
      val('cfg_node_name','node_name');
      val('cfg_node_lat','node_lat');
      val('cfg_node_lon','node_lon');
      val('cfg_owner_info','owner_info');
      val('cfg_password','password');
      val('cfg_guest_password','guest_password');
      val('cfg_freq','freq');
      val('cfg_bw','bw');
      val('cfg_sf','sf');
      val('cfg_cr','cr');
      val('cfg_tx_power','tx_power_dbm');
      val('cfg_advert_interval','advert_interval');
      val('cfg_flood_advert_interval','flood_advert_interval');
      val('cfg_flood_max','flood_max');
      val('cfg_airtime_factor','airtime_factor');
      val('cfg_rx_delay_base','rx_delay_base');
      val('cfg_tx_delay_factor','tx_delay_factor');
      val('cfg_direct_tx_delay_factor','direct_tx_delay_factor');
      val('cfg_bridge_enabled','bridge_enabled');
      val('cfg_bridge_delay','bridge_delay');
      val('cfg_bridge_pkt_src','bridge_pkt_src','logTx');
      val('cfg_bridge_baud','bridge_baud');
      val('cfg_bridge_channel','bridge_channel');
      val('cfg_gps_enabled','gps_enabled');
      val('cfg_gps_interval','gps_interval');
      val('cfg_path_hash_mode','path_hash_mode',0);
      val('cfg_loop_detect','loop_detect','off');
      val('cfg_interference_threshold','interference_threshold');
      val('cfg_agc_reset_interval','agc_reset_interval');
      val('cfg_multi_acks','multi_acks');
      val('cfg_allow_read_only','allow_read_only');

      tableFromPairs($('portsTbl'), [
        ['Raw Port', enc(fmt(s.ports?.raw_port, s.tcp?.port))],
        ['Console Port', enc(fmt(s.ports?.console_port, '-'))],
        ['Mirror Port', enc(fmt(s.ports?.mirror_port, '-'))],
        ['HTTP Port', enc(fmt(s.http?.port, 80))]
      ]);

      renderNameHash();
      renderPickerMap();
      clearDirty();
    }

    function renderNameHash(){
      const name = $('cfg_node_name').value || '';
      const h = fnv1a8(name || '');
      const hx = h.toString(16).toUpperCase().padStart(2,'0');
      $('nodeHashLine').textContent = `${hx} (1 byte)`;
      const nbs = (state.stats?.neighbors || []);
      const collision = nbs.find(n => String(n.id_prefix||'').toUpperCase().startsWith(hx));
      if(collision){
        $('nodeHashWarn').className='bad-note';
        $('nodeHashWarn').textContent=`Collision risk with visible neighbor ${collision.id_prefix}`;
      }else{
        $('nodeHashWarn').className='ok-note';
        $('nodeHashWarn').textContent='No collision in visible neighbors';
      }
    }

    function renderPickerMap(){
      if(!state.mapEnabled){
        $('pickerMap').innerHTML = '<div style="padding:10px;color:#566a8a">Map tools disabled. Enable map first.</div>';
        return;
      }
      if(!mapSafe()){
        $('pickerMap').innerHTML = '<div style="padding:10px;color:#566a8a">Leaflet map script unavailable.</div>';
        return;
      }
      const lat = asNum($('cfg_node_lat').value), lon=asNum($('cfg_node_lon').value);
      if(!state.picker){
        $('pickerMap').innerHTML = '';
        state.picker = L.map('pickerMap', {zoomControl:true});
        L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {maxZoom:19, attribution:'&copy; OpenStreetMap'}).addTo(state.picker);
        state.picker.on('click', (e)=>{
          if(!state.pickerArmed) return;
          $('cfg_node_lat').value = e.latlng.lat.toFixed(7);
          $('cfg_node_lon').value = e.latlng.lng.toFixed(7);
          placePickerMarker(e.latlng.lat, e.latlng.lng);
          markDirty();
        });
      }
      if(Number.isFinite(lat) && Number.isFinite(lon)){
        placePickerMarker(lat, lon);
        state.picker.setView([lat,lon], 15);
      } else {
        state.picker.setView([53.4742393,14.5911102], 12);
      }
    }

    function placePickerMarker(lat, lon){
      if(!mapSafe() || !state.picker) return;
      if(!state.pickerMarker){ state.pickerMarker = L.marker([lat,lon]).addTo(state.picker); }
      else { state.pickerMarker.setLatLng([lat,lon]); }
    }

    function applyPreset(){
      const p = $('cfg_radio_preset').value;
      const set = (k,v)=>{ $(k).value = v; markDirty(); };
      if(p==='eu_868_long'){ set('cfg_freq','869.525'); set('cfg_bw','125.000'); set('cfg_sf','11'); set('cfg_cr','5'); }
      else if(p==='eu_868_mesh'){ set('cfg_freq','869.618'); set('cfg_bw','62.500'); set('cfg_sf','8'); set('cfg_cr','5'); }
      else if(p==='us_915_fast'){ set('cfg_freq','915.000'); set('cfg_bw','250.000'); set('cfg_sf','9'); set('cfg_cr','5'); }
    }

    async function fetchJson(url){
      const r = await fetch(url, {cache:'no-store'});
      if(!r.ok) throw new Error(url+' HTTP '+r.status);
      return await r.json();
    }

    async function loadStats(){
      const s = await fetchJson('/stats');
      state.stats = s;
      renderHeader(s);
      renderQuick(s, state.cfg || {});
      renderRuntime(s);
      renderNeighbors(s);
      renderLogs(s);
      if(state.currentTab === 'mqtt'){
        renderMqttTab(s, state.cfg || {});
      }
      if(state.currentTab === 'config' && state.cfgLoaded && state.cfg){
        fillConfig(state.cfg, s);
      }
      renderEndpoints();
      if (!state.cfgLoaded && !state.cfgPrefetchScheduled) {
        state.cfgPrefetchScheduled = true;
        setTimeout(async()=>{
          if (state.cfgLoaded) return;
          try { await ensureConfigLoaded(false); } catch (_) {}
        }, 150);
      }
      setOpState('Status loaded.', true);
    }

    async function ensureConfigLoaded(force=false){
      if(!force && state.cfgLoaded && state.cfg) return state.cfg;
      const c = await fetchJson('/config-export');
      state.cfg = c;
      state.cfgLoaded = true;
      state.cfgPrefetchScheduled = true;
      if(state.stats){
        renderQuick(state.stats, c);
        if(state.currentTab === 'mqtt'){
          renderMqttTab(state.stats, c);
        }
        if(state.currentTab === 'config'){
          fillConfig(c, state.stats);
        }
      }
      return c;
    }

    async function reloadCurrent(){
      await loadStats();
      if(state.currentTab === 'mqtt' || state.currentTab === 'config'){
        await ensureConfigLoaded(false);
      }
    }

    function cfgToParams(){
      const get = (id)=>$(id)?.value ?? '';
      const chk = (id)=>$(id)?.checked ? '1' : '0';
      const norm = (v)=>String(v).replace(',', '.').trim();
      const p = new URLSearchParams();
      p.set('node_name', get('cfg_node_name').trim());
      p.set('owner_info', get('cfg_owner_info'));
      p.set('node_lat', norm(get('cfg_node_lat')));
      p.set('node_lon', norm(get('cfg_node_lon')));
      p.set('password', get('cfg_password'));
      p.set('guest_password', get('cfg_guest_password'));
      p.set('freq', norm(get('cfg_freq')));
      p.set('bw', norm(get('cfg_bw')));
      p.set('sf', norm(get('cfg_sf')));
      p.set('cr', norm(get('cfg_cr')));
      p.set('tx_power_dbm', norm(get('cfg_tx_power')));
      p.set('advert_interval', norm(get('cfg_advert_interval')));
      p.set('flood_advert_interval', norm(get('cfg_flood_advert_interval')));
      p.set('flood_max', norm(get('cfg_flood_max')));
      p.set('airtime_factor', norm(get('cfg_airtime_factor')));
      p.set('rx_delay_base', norm(get('cfg_rx_delay_base')));
      p.set('tx_delay_factor', norm(get('cfg_tx_delay_factor')));
      p.set('direct_tx_delay_factor', norm(get('cfg_direct_tx_delay_factor')));
      p.set('bridge_enabled', chk('cfg_bridge_enabled'));
      p.set('bridge_delay', norm(get('cfg_bridge_delay')));
      p.set('bridge_pkt_src', get('cfg_bridge_pkt_src'));
      p.set('bridge_baud', norm(get('cfg_bridge_baud')));
      p.set('bridge_channel', norm(get('cfg_bridge_channel')));
      p.set('gps_enabled', chk('cfg_gps_enabled'));
      p.set('gps_interval', norm(get('cfg_gps_interval')));
      p.set('path_hash_mode', get('cfg_path_hash_mode'));
      p.set('loop_detect', get('cfg_loop_detect'));
      p.set('interference_threshold', norm(get('cfg_interference_threshold')));
      p.set('agc_reset_interval', norm(get('cfg_agc_reset_interval')));
      p.set('multi_acks', norm(get('cfg_multi_acks')));
      p.set('allow_read_only', chk('cfg_allow_read_only'));
      return p;
    }

    async function postForm(url, params){
      const r = await fetch(url, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: params.toString()});
      const txt = await r.text();
      let j = null; try { j = JSON.parse(txt); } catch(_) {}
      if(!r.ok || (j && j.ok===false)) throw new Error((j && j.message) ? j.message : ('HTTP '+r.status));
      return j || {ok:true};
    }

    async function doSave(){
      if(state.busy) return;
      if(!state.dirty){
        setOpState('No unsaved changes. Open Configuration tab and edit values first.', true);
        return;
      }
      setBusy(true);
      setOpState('Saving...', true);
      try {
        await postForm('/config-save', cfgToParams());
        await loadStats();
        await ensureConfigLoaded(true);
        clearDirty();
        setOpState('Configuration saved.', true);
      } catch(e){
        setOpState('Save failed. Check values and retry. Details: '+e.message, false);
      } finally {
        setBusy(false);
      }
    }

    async function doAdvert(){
      if(state.busy) return;
      setBusy(true);
      setOpState('Sending advert...', true);
      try {
        await postForm('/advert', new URLSearchParams());
        await loadStats();
        setOpState('Advert sent. Status refreshed.', true);
      } catch(e){
        setOpState('Advert failed. Check radio state and retry. Details: '+e.message, false);
      } finally {
        setBusy(false);
      }
    }

    async function doSync(){
      if(state.busy) return;
      setBusy(true);
      setOpState('Syncing clock...', true);
      try {
        const p = new URLSearchParams(); p.set('epoch', String(nowSec()));
        await postForm('/sync-clock', p);
        await loadStats();
        setOpState('Clock synchronized. Status refreshed.', true);
      } catch(e){
        setOpState('Clock sync failed. Verify connectivity and retry. Details: '+e.message, false);
      } finally {
        setBusy(false);
      }
    }

    async function doReboot(){
      if(state.busy) return;
      if(!confirm('Reboot device now?')) return;
      const ack = prompt('Type REBOOT to confirm critical action:');
      if(ack !== 'REBOOT'){
        setOpState('Reboot canceled. Confirmation not provided.', true);
        return;
      }
      setBusy(true);
      setOpState('Reboot command sent...', true);
      try {
        await postForm('/reboot', new URLSearchParams());
        setOpState('Reboot requested. Wait ~20s then click Reload.', true);
      } catch(e){
        setOpState('Reboot failed. Device did not accept command. Details: '+e.message, false);
      } finally {
        setBusy(false);
      }
    }

    function doExport(){ window.location = '/config-export'; }

    function applyImportedConfig(cfg){
      const map = {
        node_name:'cfg_node_name', owner_info:'cfg_owner_info', node_lat:'cfg_node_lat', node_lon:'cfg_node_lon',
        password:'cfg_password', guest_password:'cfg_guest_password', freq:'cfg_freq', bw:'cfg_bw', sf:'cfg_sf', cr:'cfg_cr',
        tx_power_dbm:'cfg_tx_power', advert_interval:'cfg_advert_interval', flood_advert_interval:'cfg_flood_advert_interval',
        flood_max:'cfg_flood_max', airtime_factor:'cfg_airtime_factor', rx_delay_base:'cfg_rx_delay_base', tx_delay_factor:'cfg_tx_delay_factor',
        direct_tx_delay_factor:'cfg_direct_tx_delay_factor', bridge_delay:'cfg_bridge_delay', bridge_pkt_src:'cfg_bridge_pkt_src',
        bridge_baud:'cfg_bridge_baud', bridge_channel:'cfg_bridge_channel', gps_interval:'cfg_gps_interval',
        path_hash_mode:'cfg_path_hash_mode', loop_detect:'cfg_loop_detect', interference_threshold:'cfg_interference_threshold',
        agc_reset_interval:'cfg_agc_reset_interval', multi_acks:'cfg_multi_acks'
      };
      Object.keys(map).forEach((k)=>{ if(cfg[k]!==undefined && $(map[k])) $(map[k]).value = cfg[k]; });
      if(cfg.bridge_enabled!==undefined) $('cfg_bridge_enabled').checked = !!cfg.bridge_enabled;
      if(cfg.gps_enabled!==undefined) $('cfg_gps_enabled').checked = !!cfg.gps_enabled;
      if(cfg.allow_read_only!==undefined) $('cfg_allow_read_only').checked = !!cfg.allow_read_only;
      renderNameHash(); renderPickerMap();
      markDirty();
      setOpState('Config imported (not saved yet)', true);
    }

    function importCfgToParams(cfg){
      const p = new URLSearchParams();
      const norm = (v)=>String(v ?? '').replace(',', '.').trim();
      const setText = (k)=>{ if(cfg[k] !== undefined && cfg[k] !== null) p.set(k, String(cfg[k])); };
      const setNum = (k)=>{ if(cfg[k] !== undefined && cfg[k] !== null) p.set(k, norm(cfg[k])); };
      const setBool = (k)=>{ if(cfg[k] !== undefined) p.set(k, cfg[k] ? '1' : '0'); };

      setText('node_name');
      setText('owner_info');
      setNum('node_lat');
      setNum('node_lon');
      setText('password');
      setText('guest_password');
      setNum('freq');
      setNum('bw');
      setNum('sf');
      setNum('cr');
      setNum('tx_power_dbm');
      setNum('advert_interval');
      setNum('flood_advert_interval');
      setNum('flood_max');
      setNum('airtime_factor');
      setNum('rx_delay_base');
      setNum('tx_delay_factor');
      setNum('direct_tx_delay_factor');
      setBool('bridge_enabled');
      setNum('bridge_delay');
      setText('bridge_pkt_src');
      setNum('bridge_baud');
      setNum('bridge_channel');
      setBool('gps_enabled');
      setNum('gps_interval');
      setNum('path_hash_mode');
      setText('loop_detect');
      setNum('interference_threshold');
      setNum('agc_reset_interval');
      setNum('multi_acks');
      setBool('allow_read_only');

      return p;
    }

    async function uploadImportRaw(cfg){
      const p = importCfgToParams(cfg);
      await postForm('/config-import', p);
    }

    async function doImportFile(file){
      const raw = await file.text();
      let json;
      try { json = JSON.parse(raw); } catch(e){ throw new Error('Invalid JSON file'); }
      applyImportedConfig(json);
      setOpState('Import preview loaded. Review changes, then press Save to apply.', true);
    }

    async function activateTab(t, focusTab=false){
      state.currentTab = t;
      refreshActionButtons();
      const tabs = Array.from(document.querySelectorAll('.tab'));
      tabs.forEach((x)=>{
        const active = x.getAttribute('data-tab') === t;
        x.classList.toggle('active', active);
        x.setAttribute('aria-selected', active ? 'true' : 'false');
        x.setAttribute('tabindex', active ? '0' : '-1');
        if(active && focusTab) x.focus();
      });
      document.querySelectorAll('.tab-body').forEach((panel)=>{
        const active = panel.id === ('tab-' + t);
        panel.classList.toggle('hidden', !active);
        panel.setAttribute('aria-hidden', active ? 'false' : 'true');
      });

      if(t === 'mqtt'){
        if (state.stats) {
          renderMqttTab(state.stats, state.cfg || {});
        }
        try {
          await ensureConfigLoaded(false);
          if (state.stats) {
            renderMqttTab(state.stats, state.cfg || {});
          }
        } catch(e){
          setOpState('MQTT runtime is visible, but config metadata could not be loaded. Use Reload. Details: '+e.message, false);
        }
      }

      if(t === 'config'){
        try {
          if (!state.cfgLoaded) {
            setOpState('Loading configuration from device...', true);
          }
          await ensureConfigLoaded(false);
          if (state.cfg) {
            fillConfig(state.cfg, state.stats || {});
          }
        } catch(e){
          setOpState('Could not load config data for this tab. Use Reload. Details: '+e.message, false);
        }
      }

      if(t === 'status' && state.map){ setTimeout(()=>state.map.invalidateSize(), 50); }
      if(t === 'config' && state.picker){ setTimeout(()=>state.picker.invalidateSize(), 50); }
      if(t === 'endpoints'){ renderEndpoints(); }
    }

    function wireTabs(){
      const tabs = Array.from(document.querySelectorAll('.tab'));
      tabs.forEach((b)=>{
        b.addEventListener('click', ()=>{ activateTab(b.getAttribute('data-tab')); });
        b.addEventListener('keydown', (e)=>{
          const idx = tabs.indexOf(b);
          if(idx < 0) return;
          let next = idx;
          if(e.key === 'ArrowRight') next = (idx + 1) % tabs.length;
          else if(e.key === 'ArrowLeft') next = (idx - 1 + tabs.length) % tabs.length;
          else if(e.key === 'Home') next = 0;
          else if(e.key === 'End') next = tabs.length - 1;
          else return;
          e.preventDefault();
          const nextTab = tabs[next];
          activateTab(nextTab.getAttribute('data-tab'), true);
        });
      });
    }

    function wireA11yLabels(){
      const labels = {
        cfg_node_name: 'Node Name',
        cfg_node_lat: 'Node Latitude',
        cfg_node_lon: 'Node Longitude',
        cfg_owner_info: 'Owner Info',
        cfg_password: 'Password',
        cfg_guest_password: 'Guest Password',
        cfg_radio_preset: 'Radio Preset',
        cfg_freq: 'LoRa Frequency',
        cfg_bw: 'LoRa Bandwidth',
        cfg_sf: 'LoRa Spreading Factor',
        cfg_cr: 'LoRa Coding Rate',
        cfg_tx_power: 'TX Power dBm',
        cfg_advert_interval: 'Advert Interval Minutes',
        cfg_flood_advert_interval: 'Flood Advert Interval Hours',
        cfg_flood_max: 'Flood Max',
        cfg_airtime_factor: 'Airtime Factor',
        cfg_rx_delay_base: 'RX Delay Base',
        cfg_tx_delay_factor: 'TX Delay Factor',
        cfg_direct_tx_delay_factor: 'Direct TX Delay Factor',
        cfg_bridge_enabled: 'Bridge Enabled',
        cfg_bridge_delay: 'Bridge Delay',
        cfg_bridge_pkt_src: 'Bridge Packet Source',
        cfg_bridge_baud: 'Bridge Baud',
        cfg_bridge_channel: 'Bridge Channel',
        cfg_gps_enabled: 'GPS Enabled',
        cfg_gps_interval: 'GPS Interval Seconds',
        cfg_path_hash_mode: 'Path Hash Mode',
        cfg_loop_detect: 'Loop Detect',
        cfg_interference_threshold: 'Interference Threshold',
        cfg_agc_reset_interval: 'AGC Reset Interval',
        cfg_multi_acks: 'Multi ACKs',
        cfg_allow_read_only: 'Allow Read Only'
      };
      Object.entries(labels).forEach(([id, label])=>{
        const el = $(id);
        if(el) el.setAttribute('aria-label', label);
      });
    }

    function wireInputs(){
      document.querySelectorAll('input,textarea,select').forEach((e)=>{
        if(e.id==='importFile') return;
        e.addEventListener('input', markDirty);
        e.addEventListener('change', ()=>{ markDirty(); if(e.id==='cfg_node_name') renderNameHash(); if(e.id==='cfg_node_lat'||e.id==='cfg_node_lon') renderPickerMap(); });
      });
      $('cfg_radio_preset').addEventListener('change', applyPreset);
      $('btnPickMap').addEventListener('click', async()=>{
        if(!state.mapEnabled || !mapSafe()){
          if(state.busy) return;
          setBusy(true);
          setOpState('Loading map engine...', true);
          try {
            await ensureLeafletLoaded();
            state.mapEnabled = true;
            if(state.stats) renderNeighbors(state.stats);
            renderPickerMap();
            setOpState('Map tools enabled.', true);
          } catch(e){
            state.mapEnabled = false;
            setOpState('Map unavailable. Device still works without map. Enter coordinates manually or retry. Details: ' + e.message, false);
          } finally {
            setBusy(false);
          }
          return;
        }
        state.pickerArmed = !state.pickerArmed;
        $('btnPickMap').textContent = state.pickerArmed ? 'Pick on map' : 'Picking disabled';
      });
      $('btnGps').addEventListener('click', ()=>{
        if(!navigator.geolocation){ setOpState('Browser GPS unavailable', false); return; }
        navigator.geolocation.getCurrentPosition((pos)=>{
          $('cfg_node_lat').value = pos.coords.latitude.toFixed(7);
          $('cfg_node_lon').value = pos.coords.longitude.toFixed(7);
          renderPickerMap(); markDirty();
        }, ()=>setOpState('GPS permission denied', false), {enableHighAccuracy:true, timeout:8000});
      });
      $('btnLoadMap').addEventListener('click', async()=>{
        if(state.mapEnabled && mapSafe()){
          setOpState('Map already loaded.', true);
          return;
        }
        if(state.busy) return;
        setBusy(true);
        setOpState('Loading map engine...', true);
        try {
          await ensureLeafletLoaded();
          state.mapEnabled = true;
          if(state.stats) renderNeighbors(state.stats);
          setOpState('Map loaded for this session.', true);
        } catch(e){
          state.mapEnabled = false;
          setOpState('Map unavailable. Device still works without map. Enter coordinates manually or retry. Details: ' + e.message, false);
        } finally {
          setBusy(false);
        }
      });
    }

    $('btnSave').onclick = ()=>doSave();
    $('btnAdvert').onclick = ()=>doAdvert();
    $('btnSync').onclick = ()=>doSync();
    $('btnReload').onclick = async()=>{
      if(state.busy) return;
      setBusy(true);
      setOpState('Reloading...', true);
      try { await reloadCurrent(); }
      catch(e){ setOpState('Reload failed. Check network and retry. Details: '+e.message, false); }
      finally { setBusy(false); }
    };
    $('btnReboot').onclick = ()=>doReboot();
    $('btnExport').onclick = ()=>doExport();
    $('btnImport').onclick = ()=>$('importFile').click();
    $('btnClearLogs').onclick = async()=>{
      if(state.busy) return;
      setBusy(true);
      try {
        await postForm('/logs-clear', new URLSearchParams());
        await loadStats();
        setOpState('Logs cleared. Status refreshed.', true);
      } catch(e){
        setOpState('Logs clear failed. Retry in a moment. Details: '+e.message, false);
      } finally {
        setBusy(false);
      }
    };
    $('importFile').addEventListener('change', async(e)=>{
      const f = e.target.files && e.target.files[0];
      if(!f) return;
      try { await doImportFile(f); }
      catch(err){ setOpState('Import failed: '+err.message, false); }
      finally { e.target.value = ''; }
    });

    wireTabs();
    wireA11yLabels();
    wireInputs();
    renderEndpoints();
    activateTab('status');
    loadStats().catch((e)=>setOpState('Load failed. Check connectivity. Details: '+e.message, false));
  </script>
</body>
</html>
)HTML";

static String httpUptimeHuman(uint64_t uptime_ms) {
  uint64_t s = uptime_ms / 1000ULL;
  uint64_t d = s / 86400ULL;
  s %= 86400ULL;
  uint64_t h = s / 3600ULL;
  s %= 3600ULL;
  uint64_t m = s / 60ULL;
  s %= 60ULL;

  char buf[96];
  if (d > 0) {
    snprintf(buf, sizeof(buf), "%llud %lluh %llum %llus", (unsigned long long)d, (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
  } else if (h > 0) {
    snprintf(buf, sizeof(buf), "%lluh %llum %llus", (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
  } else if (m > 0) {
    snprintf(buf, sizeof(buf), "%llum %llus", (unsigned long long)m, (unsigned long long)s);
  } else {
    snprintf(buf, sizeof(buf), "%llus", (unsigned long long)s);
  }
  return String(buf);
}

static String httpIsoUtc(uint32_t epoch) {
  DateTime dt(epoch);
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d UTC (%lu)",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second(),
           (unsigned long)epoch);
  return String(buf);
}

static String buildStatsJson(MyMesh* mesh) {
  const McreRuntimeState& st = mcreStateSnapshot();
  const McrePortsSnapshot& ps = mcrePortsSnapshot();
  const McreMqttRuntimeStats& ms = mcreMqttSnapshot();

  uint32_t now_epoch = 0;
  if (mesh && mesh->getRTCClock()) {
    now_epoch = mesh->getRTCClock()->getCurrentTime();
  }
  if (now_epoch == 0) {
    time_t wall = std::time(nullptr);
    if (wall > 0) now_epoch = (uint32_t)wall;
  }
  if (now_epoch == 0) {
    now_epoch = st.init_epoch_s;
  }

  uint32_t uptime_s = (uint32_t)(st.uptime_ms / 1000ULL);
  uint32_t start_epoch = st.init_epoch_s;
  if (now_epoch > 0 && now_epoch >= uptime_s) {
    start_epoch = now_epoch - uptime_s;
  }

  uint16_t direct_dups = 0;
  uint16_t flood_dups = 0;
  if (mesh && mesh->getTables()) {
    auto* tbl = (SimpleMeshTables*)mesh->getTables();
    direct_dups = tbl->getNumDirectDups();
    flood_dups = tbl->getNumFloodDups();
  }

  int noise_floor = 0;
  if (mesh && mesh->_radio) {
    noise_floor = (int)((int16_t)mesh->_radio->getNoiseFloor());
  }

  unsigned long queue_len = 0;
  unsigned int err_flags = 0;
  if (mesh) {
    if (mesh->_mgr) {
      queue_len = (unsigned long)mesh->_mgr->getOutboundCount(0xFFFFFFFF);
    }
    err_flags = (unsigned int)mesh->_err_flags;
  }

  uint32_t heap_total = ESP.getHeapSize();
  uint32_t heap_free = ESP.getFreeHeap();
  uint32_t heap_min = ESP.getMinFreeHeap();
  uint32_t heap_max_alloc = ESP.getMaxAllocHeap();
  uint32_t heap_free_pct = heap_total ? (heap_free * 100UL) / heap_total : 0;
  uint32_t heap_min_pct = heap_total ? (heap_min * 100UL) / heap_total : 0;
  uint32_t heap_max_alloc_pct = heap_total ? (heap_max_alloc * 100UL) / heap_total : 0;

  int16_t neighbors_count = 0;
  McreNeighborInfo sorted_neighbors[MCRE_MAX_NEIGHBORS];
  for (int i = 0; i < MCRE_MAX_NEIGHBORS; i++) {
    if (st.neighbors[i].heard_timestamp > 0) {
      sorted_neighbors[neighbors_count++] = st.neighbors[i];
    }
  }
  std::sort(sorted_neighbors, sorted_neighbors + neighbors_count, [](const McreNeighborInfo& a, const McreNeighborInfo& b){
    return a.heard_timestamp > b.heard_timestamp;
  });

  String body;
  body.reserve(12000);
  body += "{";
  body += "\"role\":\"repeater\",";
  body += "\"timestamp\":" + String((unsigned long)now_epoch) + ",";
  body += "\"firmware_version\":\"" + httpJsonEscapeSimple(String(FIRMWARE_VERSION)) + "\",";
  body += "\"build_date\":\"" + httpJsonEscapeSimple(String(FIRMWARE_BUILD_DATE)) + "\",";

  body += "\"wifi\":{";
  body += "\"connected\":" + String(ps.wifi_connected ? "true" : "false") + ",";
  body += "\"ssid\":\"" + httpJsonEscapeSimple(String(ps.wifi_ssid)) + "\",";
  body += "\"ip\":\"" + httpJsonEscapeSimple(String(ps.wifi_ip)) + "\",";
  body += "\"rssi\":" + String(ps.wifi_rssi);
  body += "},";

  body += "\"radio\":{";
  body += "\"battery_mv\":" + String((unsigned int)board.getBattMilliVolts()) + ",";
  body += "\"noise_floor\":" + String(noise_floor) + ",";
  body += "\"last_rssi\":" + String((int)((int16_t)radio_driver.getLastRSSI())) + ",";
  body += "\"last_snr_x4\":" + String((int)((int16_t)(radio_driver.getLastSNR() * 4))) + ",";
  body += "\"packets_rx\":" + String((unsigned long)radio_driver.getPacketsRecv()) + ",";
  body += "\"packets_tx\":" + String((unsigned long)radio_driver.getPacketsSent()) + ",";
  body += "\"recv_errors\":" + String((unsigned long)radio_driver.getPacketsRecvErrors()) + ",";
  body += "\"tx_air_secs\":" + String((unsigned long)(mesh->getTotalAirTime() / 1000)) + ",";
  body += "\"rx_air_secs\":" + String((unsigned long)(mesh->getReceiveAirTime() / 1000)) + ",";
  body += "\"uptime_ms\":" + String((unsigned long long)st.uptime_ms) + ",";
  body += "\"uptime_human\":\"" + httpJsonEscapeSimple(httpUptimeHuman(st.uptime_ms)) + "\",";
  body += "\"device_time_utc\":\"" + httpJsonEscapeSimple(httpIsoUtc(now_epoch)) + "\",";
  body += "\"device_start_utc\":\"" + httpJsonEscapeSimple(httpIsoUtc(start_epoch)) + "\",";
  body += "\"queue_len\":" + String(queue_len) + ",";
  body += "\"err_flags\":" + String(err_flags) + ",";
  body += "\"sent_flood\":" + String((unsigned long)mesh->getNumSentFlood()) + ",";
  body += "\"sent_direct\":" + String((unsigned long)mesh->getNumSentDirect()) + ",";
  body += "\"recv_flood\":" + String((unsigned long)mesh->getNumRecvFlood()) + ",";
  body += "\"recv_direct\":" + String((unsigned long)mesh->getNumRecvDirect()) + ",";
  body += "\"direct_dups\":" + String((unsigned int)direct_dups) + ",";
  body += "\"flood_dups\":" + String((unsigned int)flood_dups);
  body += "},";

  body += "\"heap\":{";
  body += "\"total\":" + String((unsigned long)heap_total) + ",";
  body += "\"free\":" + String((unsigned long)heap_free) + ",";
  body += "\"min_free\":" + String((unsigned long)heap_min) + ",";
  body += "\"max_alloc\":" + String((unsigned long)heap_max_alloc) + ",";
  body += "\"free_pct\":" + String((unsigned long)heap_free_pct) + ",";
  body += "\"min_free_pct\":" + String((unsigned long)heap_min_pct) + ",";
  body += "\"max_alloc_pct\":" + String((unsigned long)heap_max_alloc_pct);
  body += "},";

  body += "\"mqtt\":{";
  body += "\"compile_enabled\":" + String(ms.compile_enabled ? "true" : "false") + ",";
  body += "\"enabled\":" + String(ms.compile_enabled ? "true" : "false") + ",";
  body += "\"enabled_runtime\":" + String(ms.enabled_runtime ? "true" : "false") + ",";
  body += "\"connected\":" + String(ms.connected ? "true" : "false") + ",";
  body += "\"connected_brokers\":" + String((unsigned long)ms.connected_brokers) + ",";
  body += "\"enabled_profiles\":" + String((unsigned long)ms.enabled_profiles) + ",";
  body += "\"max_slots\":" + String((unsigned long)ms.max_slots) + ",";
  body += "\"active_profile\":\"" + httpJsonEscapeSimple(String(ms.active_profile)) + "\",";
  body += "\"active_host\":\"" + httpJsonEscapeSimple(String(ms.active_host)) + "\",";
  body += "\"active_uri\":\"" + httpJsonEscapeSimple(String(ms.active_uri)) + "\",";
  body += "\"connect_attempts\":" + String((unsigned long)ms.connect_attempts) + ",";
  body += "\"reconnects\":" + String((unsigned long)ms.reconnects) + ",";
  body += "\"publish_ok\":" + String((unsigned long)ms.publish_ok) + ",";
  body += "\"publish_fail\":" + String((unsigned long)ms.publish_fail) + ",";
  body += "\"last_profile_index\":" + String((long)ms.last_profile_index) + ",";
  body += "\"last_error_code\":" + String((long)ms.last_error_code) + ",";
  uint32_t mqtt_brokers_count = ms.brokers_count;
  if (mqtt_brokers_count > MCRE_MQTT_MAX_BROKER_STATS) mqtt_brokers_count = MCRE_MQTT_MAX_BROKER_STATS;
  body += "\"brokers_count\":" + String((unsigned long)mqtt_brokers_count) + ",";
  body += "\"brokers\":[";
  for (uint32_t i = 0; i < mqtt_brokers_count; i++) {
    const McreMqttBrokerStats* b = &ms.brokers[i];
    if (i > 0) body += ",";
    body += "{";
    body += "\"slot\":" + String((unsigned int)b->slot) + ",";
    body += "\"configured\":" + String(b->configured ? "true" : "false") + ",";
    body += "\"connected\":" + String(b->connected ? "true" : "false") + ",";
    body += "\"profile_index\":" + String((long)b->profile_index) + ",";
    body += "\"connected_since_epoch\":" + String((unsigned long)b->connected_since_epoch) + ",";
    body += "\"connected_for_s\":" + String((unsigned long)b->connected_for_s) + ",";
    body += "\"connect_attempts\":" + String((unsigned long)b->connect_attempts) + ",";
    body += "\"reconnects\":" + String((unsigned long)b->reconnects) + ",";
    body += "\"publish_ok\":" + String((unsigned long)b->publish_ok) + ",";
    body += "\"publish_fail\":" + String((unsigned long)b->publish_fail) + ",";
    body += "\"last_error_code\":" + String((long)b->last_error_code) + ",";
    body += "\"profile_name\":\"" + httpJsonEscapeSimple(String(b->profile_name)) + "\",";
    body += "\"host\":\"" + httpJsonEscapeSimple(String(b->host)) + "\",";
    body += "\"uri\":\"" + httpJsonEscapeSimple(String(b->uri)) + "\"";
    body += "}";
  }
  body += "]";
  body += "},";

  body += "\"ports\":{";
  body += "\"raw_port\":" + String((unsigned int)ps.raw_port) + ",";
  body += "\"raw_active_clients\":" + String((unsigned long)ps.raw_active_clients) + ",";
  body += "\"console_port\":" + String((unsigned int)ps.console_port) + ",";
  body += "\"console_active_clients\":" + String((unsigned long)ps.console_active_clients) + ",";
  body += "\"mirror_port\":" + String((unsigned int)ps.mirror_port) + ",";
  body += "\"mirror_active_clients\":" + String((unsigned long)ps.mirror_active_clients);
  body += "},";

  body += "\"tcp\":{";
  body += "\"port\":" + String((unsigned int)ps.raw_port) + ",";
  body += "\"active_clients\":" + String((unsigned long)ps.raw_active_clients) + ",";
  body += "\"clients_accepted\":" + String((unsigned long)ps.tcp.clients_accepted) + ",";
  body += "\"clients_rejected\":" + String((unsigned long)ps.tcp.clients_rejected) + ",";
  body += "\"clients_disconnected\":" + String((unsigned long)ps.tcp.clients_disconnected) + ",";
  body += "\"max_simultaneous_clients\":" + String((unsigned long)ps.tcp.max_simultaneous_clients) + ",";
  body += "\"rx_frames\":" + String((unsigned long)ps.tcp.rx_frames) + ",";
  body += "\"rx_bytes\":" + String((unsigned long)ps.tcp.rx_bytes) + ",";
  body += "\"rx_parse_errors\":" + String((unsigned long)ps.tcp.rx_parse_errors) + ",";
  body += "\"rx_checksum_errors\":" + String((unsigned long)ps.tcp.rx_checksum_errors) + ",";
  body += "\"tx_frames\":" + String((unsigned long)ps.tcp.tx_frames) + ",";
  body += "\"tx_bytes\":" + String((unsigned long)ps.tcp.tx_bytes) + ",";
  body += "\"tx_deliveries\":" + String((unsigned long)ps.tcp.tx_deliveries) + ",";
  body += "\"tx_delivery_failures\":" + String((unsigned long)ps.tcp.tx_delivery_failures);
  body += "},";

  body += "\"http\":{";
  body += "\"port\":" + String((int)HTTP_STATS_PORT) + ",";
  body += "\"requests_total\":" + String((unsigned long)g_http_requests_total) + ",";
  body += "\"not_found\":" + String((unsigned long)g_http_not_found) + ",";
  body += "\"rejected\":" + String((unsigned long)g_http_rejected) + ",";
  body += "\"max_parallel_requests\":" + String((unsigned long)g_http_max_parallel) + ",";
  body += "\"max_client_slots\":" + String((unsigned long)HTTP_MAX_CLIENTS);
  body += "},";

  body += "\"neighbors\":[";
  for (int i = 0; i < neighbors_count; i++) {
    const McreNeighborInfo* n = &sorted_neighbors[i];
    if (i > 0) body += ",";
    uint32_t heard_secs_ago = (now_epoch >= n->heard_timestamp) ? (now_epoch - n->heard_timestamp) : 0;

    body += "{";
    body += "\"id_prefix\":\"" + httpJsonEscapeSimple(String(n->id_prefix)) + "\",";
    body += "\"name\":\"" + httpJsonEscapeSimple(String(n->name)) + "\",";
    body += "\"snr_x4\":" + String((int)n->snr_x4) + ",";
    body += "\"snr\":" + String(((float)n->snr_x4) / 4.0f, 2) + ",";
    body += "\"has_location\":" + String(n->has_location ? "true" : "false") + ",";
    body += "\"lat\":" + String(((double)n->lat_e6) / 1000000.0, 6) + ",";
    body += "\"lon\":" + String(((double)n->lon_e6) / 1000000.0, 6) + ",";
    body += "\"heard_secs_ago\":" + String((unsigned long)heard_secs_ago) + ",";
    body += "\"heard_timestamp\":" + String((unsigned long)n->heard_timestamp) + ",";
    body += "\"advert_timestamp\":" + String((unsigned long)n->advert_timestamp);
    body += "}";
  }
  body += "],";

  body += "\"runtime_logs\":[";
  for (int i = 0; i < st.logs_count; i++) {
    int idx = (st.logs_next + MCRE_MAX_LOG_ENTRIES - st.logs_count + i) % MCRE_MAX_LOG_ENTRIES;
    const McreLogEntry* le = &st.logs[idx];
    uint32_t age = (le->ts > 0 && now_epoch >= le->ts) ? (now_epoch - le->ts) : 0;
    if (i > 0) body += ",";
    body += "{";
    body += "\"age_s\":" + String((unsigned long)age) + ",";
    body += "\"timestamp\":" + String((unsigned long)le->ts) + ",";
    body += "\"message\":\"" + httpJsonEscapeSimple(String(le->msg)) + "\"";
    body += "}";
  }
  body += "]";

  body += "}\n";
  return body;
}

static String buildConfigExportJson(MyMesh* mesh) {
  const McreMqttRuntimeStats& ms = mcreMqttSnapshot();
  NodePrefs* p = (mesh ? mesh->getNodePrefs() : nullptr);
  if (!p) {
    return String("{\"config_error\":\"node_prefs_unavailable\",\"mqtt_compile_enabled\":") +
           String(MQTT_REPORTING_ENABLED ? "true" : "false") + "}\n";
  }

  String owner = String(p->owner_info);
  owner.replace("\n", "\\n");

  String body;
  body.reserve(3000);
  body += "{";
  body += "\"node_name\":\"" + httpJsonEscapeSimple(String(p->node_name)) + "\",";
  body += "\"owner_info\":\"" + httpJsonEscapeSimple(owner) + "\",";
  body += "\"node_lat\":" + String(p->node_lat, 7) + ",";
  body += "\"node_lon\":" + String(p->node_lon, 7) + ",";
  body += "\"password\":\"\",";
  body += "\"guest_password\":\"\",";
  body += "\"has_password\":" + String((p->password[0] != 0) ? "true" : "false") + ",";
  body += "\"has_guest_password\":" + String((p->guest_password[0] != 0) ? "true" : "false") + ",";
  body += "\"freq\":" + String(p->freq, 3) + ",";
  body += "\"bw\":" + String(p->bw, 3) + ",";
  body += "\"sf\":" + String((unsigned int)p->sf) + ",";
  body += "\"cr\":" + String((unsigned int)p->cr) + ",";
  body += "\"tx_power_dbm\":" + String((int)p->tx_power_dbm) + ",";
  body += "\"advert_interval\":" + String((unsigned int)p->advert_interval * 2) + ",";
  body += "\"flood_advert_interval\":" + String((unsigned int)p->flood_advert_interval) + ",";
  body += "\"flood_max\":" + String((unsigned int)p->flood_max) + ",";
  body += "\"airtime_factor\":" + String(p->airtime_factor, 3) + ",";
  body += "\"rx_delay_base\":" + String(p->rx_delay_base, 3) + ",";
  body += "\"tx_delay_factor\":" + String(p->tx_delay_factor, 3) + ",";
  body += "\"direct_tx_delay_factor\":" + String(p->direct_tx_delay_factor, 3) + ",";
  body += "\"bridge_enabled\":" + String(p->bridge_enabled ? "true" : "false") + ",";
  body += "\"bridge_delay\":" + String((unsigned int)p->bridge_delay) + ",";
  body += "\"bridge_pkt_src\":\"" + String(p->bridge_pkt_src ? "logRx" : "logTx") + "\",";
  body += "\"bridge_baud\":" + String((unsigned long)p->bridge_baud) + ",";
  body += "\"bridge_channel\":" + String((unsigned int)p->bridge_channel) + ",";
  body += "\"gps_enabled\":" + String(p->gps_enabled ? "true" : "false") + ",";
  body += "\"gps_interval\":" + String((unsigned long)p->gps_interval) + ",";
  body += "\"path_hash_mode\":" + String((unsigned int)p->path_hash_mode) + ",";
  const char* loop_mode = "off";
  if (p->loop_detect == LOOP_DETECT_MINIMAL) loop_mode = "minimal";
  else if (p->loop_detect == LOOP_DETECT_MODERATE) loop_mode = "moderate";
  else if (p->loop_detect == LOOP_DETECT_STRICT) loop_mode = "strict";
  body += "\"loop_detect\":\"" + String(loop_mode) + "\",";
  body += "\"interference_threshold\":" + String((unsigned int)p->interference_threshold) + ",";
  body += "\"agc_reset_interval\":" + String((unsigned int)p->agc_reset_interval * 4) + ",";
  body += "\"multi_acks\":" + String((unsigned int)p->multi_acks) + ",";
  body += "\"allow_read_only\":" + String(p->allow_read_only ? "true" : "false") + ",";

  body += "\"mqtt_compile_enabled\":" + String(MQTT_REPORTING_ENABLED ? "true" : "false") + ",";
  body += "\"mqtt_enabled\":" + String(MQTT_REPORTING_ENABLED ? "true" : "false") + ",";
  body += "\"mqtt_active_profile\":\"" + httpJsonEscapeSimple(String(ms.active_profile)) + "\",";
  body += "\"mqtt_active_host\":\"" + httpJsonEscapeSimple(String(ms.active_host)) + "\",";
  body += "\"mqtt_active_uri\":\"" + httpJsonEscapeSimple(String(ms.active_uri)) + "\",";
  body += "\"mqtt_profile_letsmesh_us_enabled\":" + String(MQTT_LETSMESH_US_ENABLED ? "true" : "false") + ",";
  body += "\"mqtt_profile_letsmesh_us_host\":\"mqtt-us-v1.letsmesh.net\",";
  body += "\"mqtt_profile_letsmesh_eu_enabled\":" + String(MQTT_LETSMESH_EU_ENABLED ? "true" : "false") + ",";
  body += "\"mqtt_profile_letsmesh_eu_host\":\"mqtt-eu-v1.letsmesh.net\",";
  body += "\"mqtt_profile_extra_enabled\":" + String((MQTT_EXTRA_ENABLED && strlen(MQTT_EXTRA_HOST) > 0) ? "true" : "false") + ",";
  body += "\"mqtt_profile_extra_name\":\"" + httpJsonEscapeSimple(String(MQTT_EXTRA_NAME)) + "\",";
  body += "\"mqtt_profile_extra_host\":\"" + httpJsonEscapeSimple(String(MQTT_EXTRA_HOST)) + "\",";
  body += "\"mqtt_profile_custom_enabled\":" + String(strlen(MQTT_HOST) > 0 ? "true" : "false") + ",";
  body += "\"mqtt_profile_custom_host\":\"" + httpJsonEscapeSimple(String(MQTT_HOST)) + "\",";
  body += "\"mqtt_host\":\"" + httpJsonEscapeSimple(String(MQTT_HOST)) + "\",";
  body += "\"mqtt_port\":" + String((unsigned int)MQTT_PORT) + ",";
  body += "\"mqtt_transport\":\"" + httpJsonEscapeSimple(String(MQTT_TRANSPORT)) + "\",";
  body += "\"mqtt_ws_path\":\"" + httpJsonEscapeSimple(String(MQTT_WS_PATH)) + "\",";
  body += "\"mqtt_auth_method\":\"" + httpJsonEscapeSimple(String(MQTT_AUTH_METHOD)) + "\",";
  body += "\"mqtt_username\":\"" + httpJsonEscapeSimple(String(MQTT_USERNAME)) + "\",";
  body += "\"mqtt_password\":\"\",";
  body += "\"mqtt_password_set\":" + String((strlen(MQTT_PASSWORD) > 0) ? "true" : "false") + ",";
  body += "\"mqtt_client_id\":\"" + httpJsonEscapeSimple(String(MQTT_CLIENT_ID)) + "\",";
  body += "\"mqtt_topic_prefix\":\"" + httpJsonEscapeSimple(String("meshcore/") + String(MQTT_IATA) + "/") + "\",";
  body += "\"mqtt_iata\":\"" + httpJsonEscapeSimple(String(MQTT_IATA)) + "\"";
  body += "}\n";

  return body;
}

static bool applyConfigFromRequest(MyMesh* mesh, const String& request_body, String& err_out) {
  auto runCmd = [&](const String& cmd) -> bool {
    if (cmd.length() == 0) return true;
    char cmd_buf[220];
    char reply[220] = {0};
    if (cmd.length() >= sizeof(cmd_buf)) {
      err_out = "Command too long";
      return false;
    }
    for (size_t i = 0; i < cmd.length(); i++) {
      char ch = cmd[i];
      if (ch == '\r' || ch == '\n') {
        err_out = "Invalid control characters in command";
        return false;
      }
    }
    StrHelper::strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf));
    mesh->handleCommand(0, cmd_buf, reply);
    if (reply[0] && (strncmp(reply, "Err", 3) == 0 || strncmp(reply, "Error", 5) == 0)) {
      err_out = String(reply);
      return false;
    }
    return true;
  };

  auto normalizeNum = [](String v) -> String {
    v.trim();
    v.replace(",", ".");
    return v;
  };
  auto parseBoolLike = [](const String& v) -> bool {
    String t = v;
    t.toLowerCase();
    return t == "1" || t == "true" || t == "on" || t == "yes";
  };
  auto sanitizeText = [](String v, size_t max_len, bool allow_pipe) -> String {
    String out;
    out.reserve(v.length());
    for (size_t i = 0; i < v.length(); i++) {
      char ch = v[i];
      if (ch == '\r' || ch == '\n') {
        out += ' ';
        continue;
      }
      if ((uint8_t)ch < 0x20) continue;
      if (ch == ';') {
        out += ' ';
        continue;
      }
      if (!allow_pipe && ch == '|') {
        out += ' ';
        continue;
      }
      out += ch;
      if (out.length() >= max_len) break;
    }
    out.trim();
    return out;
  };
  auto isNumberToken = [](const String& v, bool allow_negative) -> bool {
    if (v.length() == 0) return false;
    bool seen_digit = false;
    bool seen_dot = false;
    for (size_t i = 0; i < v.length(); i++) {
      char ch = v[i];
      if (i == 0 && allow_negative && ch == '-') continue;
      if (ch == '.') {
        if (seen_dot) return false;
        seen_dot = true;
        continue;
      }
      if (!isdigit((unsigned char)ch)) return false;
      seen_digit = true;
    }
    return seen_digit;
  };
  auto isUIntToken = [](const String& v) -> bool {
    if (v.length() == 0) return false;
    for (size_t i = 0; i < v.length(); i++) {
      if (!isdigit((unsigned char)v[i])) return false;
    }
    return true;
  };

  auto getField = [&](const char* key) -> String {
    return httpGetParam(request_body, key);
  };
  auto mustBeNumber = [&](const String& v, const char* field, bool allow_negative) {
    if (err_out.length() > 0 || v.length() == 0) return;
    if (!isNumberToken(v, allow_negative)) err_out = String("Invalid numeric value: ") + field;
  };
  auto mustBeUInt = [&](const String& v, const char* field) {
    if (err_out.length() > 0 || v.length() == 0) return;
    if (!isUIntToken(v)) err_out = String("Invalid integer value: ") + field;
  };
  auto parseDoubleSafe = [](const String& v, double& out) -> bool {
    char* endp = nullptr;
    out = strtod(v.c_str(), &endp);
    return endp && *endp == 0;
  };
  auto parseUIntSafe = [](const String& v, unsigned long& out) -> bool {
    char* endp = nullptr;
    out = strtoul(v.c_str(), &endp, 10);
    return endp && *endp == 0;
  };
  auto mustBeRangeDouble = [&](const String& v, const char* field, double min_v, double max_v) {
    if (err_out.length() > 0 || v.length() == 0) return;
    double parsed = 0.0;
    if (!parseDoubleSafe(v, parsed) || parsed < min_v || parsed > max_v) {
      err_out = String(field) + " out of range";
    }
  };
  auto mustBeRangeUInt = [&](const String& v, const char* field, unsigned long min_v, unsigned long max_v) {
    if (err_out.length() > 0 || v.length() == 0) return;
    unsigned long parsed = 0;
    if (!parseUIntSafe(v, parsed) || parsed < min_v || parsed > max_v) {
      err_out = String(field) + " out of range";
    }
  };

  String node_name = sanitizeText(getField("node_name"), 63, false);
  String owner_info = sanitizeText(getField("owner_info"), 512, true);
  String node_lat = normalizeNum(getField("node_lat"));
  String node_lon = normalizeNum(getField("node_lon"));
  String password = sanitizeText(getField("password"), 63, false);
  String guest_password = sanitizeText(getField("guest_password"), 63, false);
  String freq = normalizeNum(getField("freq"));
  String bw = normalizeNum(getField("bw"));
  String sf = normalizeNum(getField("sf"));
  String cr = normalizeNum(getField("cr"));
  String tx_power = normalizeNum(getField("tx_power_dbm"));
  String advert_interval = normalizeNum(getField("advert_interval"));
  String flood_advert_interval = normalizeNum(getField("flood_advert_interval"));
  String flood_max = normalizeNum(getField("flood_max"));
  String airtime_factor = normalizeNum(getField("airtime_factor"));
  String rx_delay_base = normalizeNum(getField("rx_delay_base"));
  String tx_delay_factor = normalizeNum(getField("tx_delay_factor"));
  String direct_tx_delay_factor = normalizeNum(getField("direct_tx_delay_factor"));
  String bridge_enabled = sanitizeText(getField("bridge_enabled"), 8, false);
  String bridge_delay = normalizeNum(getField("bridge_delay"));
  String bridge_pkt_src = sanitizeText(getField("bridge_pkt_src"), 8, false);
  String bridge_baud = normalizeNum(getField("bridge_baud"));
  String bridge_channel = normalizeNum(getField("bridge_channel"));
  String gps_enabled = sanitizeText(getField("gps_enabled"), 8, false);
  String gps_interval = normalizeNum(getField("gps_interval"));
  String path_hash_mode = normalizeNum(getField("path_hash_mode"));
  String loop_detect = sanitizeText(getField("loop_detect"), 16, false);
  String interference_threshold = normalizeNum(getField("interference_threshold"));
  String agc_reset_interval = normalizeNum(getField("agc_reset_interval"));
  String multi_acks = normalizeNum(getField("multi_acks"));
  String allow_read_only = sanitizeText(getField("allow_read_only"), 8, false);

  mustBeNumber(node_lat, "node_lat", true);
  mustBeNumber(node_lon, "node_lon", true);
  mustBeNumber(freq, "freq", false);
  mustBeNumber(bw, "bw", false);
  mustBeUInt(sf, "sf");
  mustBeUInt(cr, "cr");
  mustBeUInt(tx_power, "tx_power_dbm");
  mustBeUInt(advert_interval, "advert_interval");
  mustBeUInt(flood_advert_interval, "flood_advert_interval");
  mustBeUInt(flood_max, "flood_max");
  mustBeNumber(airtime_factor, "airtime_factor", false);
  mustBeNumber(rx_delay_base, "rx_delay_base", false);
  mustBeNumber(tx_delay_factor, "tx_delay_factor", false);
  mustBeNumber(direct_tx_delay_factor, "direct_tx_delay_factor", false);
  mustBeUInt(bridge_delay, "bridge_delay");
  mustBeUInt(bridge_baud, "bridge_baud");
  mustBeUInt(bridge_channel, "bridge_channel");
  mustBeUInt(gps_interval, "gps_interval");
  mustBeUInt(path_hash_mode, "path_hash_mode");
  mustBeUInt(interference_threshold, "interference_threshold");
  mustBeUInt(agc_reset_interval, "agc_reset_interval");
  mustBeUInt(multi_acks, "multi_acks");
  mustBeRangeDouble(node_lat, "node_lat", -90.0, 90.0);
  mustBeRangeDouble(node_lon, "node_lon", -180.0, 180.0);
  mustBeRangeDouble(freq, "freq", 100.0, 1000.0);
  mustBeRangeDouble(bw, "bw", 1.0, 1000.0);
  mustBeRangeUInt(sf, "sf", 5, 12);
  mustBeRangeUInt(cr, "cr", 4, 8);
  mustBeRangeUInt(tx_power, "tx_power_dbm", 0, 30);
  mustBeRangeUInt(advert_interval, "advert_interval", 1, 10080);
  mustBeRangeUInt(flood_advert_interval, "flood_advert_interval", 1, 720);
  mustBeRangeUInt(flood_max, "flood_max", 1, 255);
  mustBeRangeDouble(airtime_factor, "airtime_factor", 0.0, 10000.0);
  mustBeRangeDouble(rx_delay_base, "rx_delay_base", 0.0, 10.0);
  mustBeRangeDouble(tx_delay_factor, "tx_delay_factor", 0.0, 10.0);
  mustBeRangeDouble(direct_tx_delay_factor, "direct_tx_delay_factor", 0.0, 10.0);
  mustBeRangeUInt(bridge_delay, "bridge_delay", 0, 60000);
  mustBeRangeUInt(bridge_baud, "bridge_baud", 1200, 2000000);
  mustBeRangeUInt(bridge_channel, "bridge_channel", 0, 255);
  mustBeRangeUInt(gps_interval, "gps_interval", 0, 86400);
  mustBeRangeUInt(path_hash_mode, "path_hash_mode", 0, 2);
  mustBeRangeUInt(interference_threshold, "interference_threshold", 0, 255);
  mustBeRangeUInt(agc_reset_interval, "agc_reset_interval", 0, 86400);
  mustBeRangeUInt(multi_acks, "multi_acks", 0, 16);

  loop_detect.toLowerCase();
  if (err_out.length() == 0 && loop_detect.length() > 0 &&
      loop_detect != "off" && loop_detect != "minimal" && loop_detect != "moderate" && loop_detect != "strict") {
    err_out = "Invalid value: loop_detect";
  }
  String bridge_src_normalized = bridge_pkt_src;
  bridge_src_normalized.toLowerCase();
  if (err_out.length() == 0 && bridge_src_normalized.length() > 0 &&
      bridge_src_normalized != "logtx" && bridge_src_normalized != "logrx" &&
      bridge_src_normalized != "tx" && bridge_src_normalized != "rx") {
    err_out = "Invalid value: bridge_pkt_src";
  }

  if (err_out.length() > 0) {
    return false;
  }

  if (node_name.length() > 0 && !runCmd(String("set name ") + node_name)) return false;
  if (owner_info.length() > 0) {
    owner_info.replace("\\n", "\n");
    owner_info.replace("\n", "|");
    if (!runCmd(String("set owner.info ") + owner_info)) return false;
  }
  if (node_lat.length() > 0 && !runCmd(String("set lat ") + node_lat)) return false;
  if (node_lon.length() > 0 && !runCmd(String("set lon ") + node_lon)) return false;
  if (password.length() > 0 && !runCmd(String("password ") + password)) return false;
  if (guest_password.length() > 0 && !runCmd(String("set guest.password ") + guest_password)) return false;
  if (freq.length() > 0 && bw.length() > 0 && sf.length() > 0 && cr.length() > 0) {
    if (!runCmd(String("set radio ") + freq + "," + bw + "," + sf + "," + cr)) return false;
  }
  if (tx_power.length() > 0 && !runCmd(String("set tx ") + tx_power)) return false;
  if (advert_interval.length() > 0 && !runCmd(String("set advert.interval ") + advert_interval)) return false;
  if (flood_advert_interval.length() > 0 && !runCmd(String("set flood.advert.interval ") + flood_advert_interval)) return false;
  if (flood_max.length() > 0 && !runCmd(String("set flood.max ") + flood_max)) return false;
  if (airtime_factor.length() > 0 && !runCmd(String("set af ") + airtime_factor)) return false;
  if (rx_delay_base.length() > 0 && !runCmd(String("set rxdelay ") + rx_delay_base)) return false;
  if (tx_delay_factor.length() > 0 && !runCmd(String("set txdelay ") + tx_delay_factor)) return false;
  if (direct_tx_delay_factor.length() > 0 && !runCmd(String("set direct.txdelay ") + direct_tx_delay_factor)) return false;

  if (bridge_enabled.length() > 0 && !runCmd(String("set bridge.enabled ") + (parseBoolLike(bridge_enabled) ? "on" : "off"))) return false;
  if (bridge_delay.length() > 0 && !runCmd(String("set bridge.delay ") + bridge_delay)) return false;
  if (bridge_src_normalized.length() > 0) {
    if (!runCmd(String("set bridge.source ") + ((bridge_src_normalized.indexOf("rx") >= 0) ? "rx" : "tx"))) return false;
  }
  if (bridge_baud.length() > 0 && !runCmd(String("set bridge.baud ") + bridge_baud)) return false;
  if (bridge_channel.length() > 0 && !runCmd(String("set bridge.channel ") + bridge_channel)) return false;
  if (gps_enabled.length() > 0 && !runCmd(String(parseBoolLike(gps_enabled) ? "gps on" : "gps off"))) return false;
  if (gps_interval.length() > 0 && !runCmd(String("set gps.interval ") + gps_interval)) return false;
  if (path_hash_mode.length() > 0 && !runCmd(String("set path.hash.mode ") + path_hash_mode)) return false;
  if (loop_detect.length() > 0 && !runCmd(String("set loop.detect ") + loop_detect)) return false;
  if (interference_threshold.length() > 0 && !runCmd(String("set int.thresh ") + interference_threshold)) return false;
  if (agc_reset_interval.length() > 0 && !runCmd(String("set agc.reset.interval ") + agc_reset_interval)) return false;
  if (multi_acks.length() > 0 && !runCmd(String("set multi.acks ") + multi_acks)) return false;
  if (allow_read_only.length() > 0 && !runCmd(String("set allow.read.only ") + (parseBoolLike(allow_read_only) ? "on" : "off"))) return false;

  return true;
}

static void writeJsonResponse(WiFiClient& c, int status_code, const char* status_text, const String& body, bool head_only = false) {
  c.printf("HTTP/1.1 %d %s\r\n", status_code, status_text);
  c.print("Content-Type: application/json\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n");
  c.printf("Content-Length: %d\r\n\r\n", (int)body.length());
  if (!head_only) c.print(body);
}

static void writeTextResponse(WiFiClient& c, int status_code, const char* status_text,
                              const char* content_type, const String& body, bool head_only = false) {
  c.printf("HTTP/1.1 %d %s\r\n", status_code, status_text);
  c.print("Content-Type: ");
  c.print(content_type ? content_type : "text/plain");
  c.print("\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n");
  c.printf("Content-Length: %d\r\n\r\n", (int)body.length());
  if (!head_only) c.print(body);
}

static int httpCountActiveSlots() {
  int count = 0;
  for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
    if (g_http_slots[i].active) count++;
  }
  return count;
}

static void httpResetSlot(int idx) {
  if (idx < 0 || idx >= HTTP_MAX_CLIENTS) return;
  HttpRequestSlot& slot = g_http_slots[idx];
  if (slot.client) slot.client.stop();
  slot = {};
}

static int httpFindFreeSlot() {
  for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
    if (!g_http_slots[i].active) return i;
    if (!g_http_slots[i].client.connected()) {
      httpResetSlot(i);
      return i;
    }
  }
  return -1;
}

static bool httpRequestNeedsBody(const String& request_line) {
  return request_line.startsWith("POST /config-save ") ||
         request_line.startsWith("POST /config-import ") ||
         request_line.startsWith("POST /advert ") ||
         request_line.startsWith("POST /sync-clock ") ||
         request_line.startsWith("POST /reboot ") ||
         request_line.startsWith("POST /logs-clear ");
}

static bool httpTryFinalizeHeaders(HttpRequestSlot& slot) {
  int hdr_end = slot.header_buf.indexOf("\r\n\r\n");
  int delim_len = 4;
  if (hdr_end < 0) {
    hdr_end = slot.header_buf.indexOf("\n\n");
    delim_len = 2;
  }
  if (hdr_end < 0) return false;

  String headers = slot.header_buf.substring(0, hdr_end);
  String remainder = slot.header_buf.substring(hdr_end + delim_len);
  slot.header_buf = "";

  int first_nl = headers.indexOf('\n');
  if (first_nl < 0) {
    slot.request_line = headers;
  } else {
    slot.request_line = headers.substring(0, first_nl);
  }
  slot.request_line.trim();

  slot.content_length = 0;
  int pos = (first_nl < 0) ? headers.length() : (first_nl + 1);
  while (pos < (int)headers.length()) {
    int nl = headers.indexOf('\n', pos);
    if (nl < 0) nl = headers.length();
    String header = headers.substring(pos, nl);
    header.trim();
    if (header.length() > 0) {
      int colon = header.indexOf(':');
      if (colon > 0) {
        String key = header.substring(0, colon);
        key.trim();
        key.toLowerCase();
        if (key == "content-length") {
          String value = header.substring(colon + 1);
          value.trim();
          long parsed_len = strtol(value.c_str(), nullptr, 10);
          if (parsed_len > 0 && parsed_len < 131072) {
            slot.content_length = (int)parsed_len;
          }
        }
      }
    }
    pos = nl + 1;
  }

  slot.headers_done = true;
  if (remainder.length() > 0) slot.request_body += remainder;
  return true;
}

static void httpHandleRequest(MyMesh* mesh, WiFiClient& client, const String& request_line, const String& request_body) {
  if (!mesh) return;

  String req_method = httpMethodFromRequestLine(request_line);
  String req_path = httpPathFromRequestLine(request_line);
  String peer_ip = client.remoteIP().toString();
  unsigned int peer_port = (unsigned int)client.remotePort();
  mcreStateLogf("HTTP %s %s from %s:%u", req_method.c_str(), req_path.c_str(), peer_ip.c_str(), peer_port);

  const bool is_head = request_line.startsWith("HEAD ");
  const bool is_root = request_line.startsWith("GET / ") || request_line.startsWith("HEAD / ");
  const bool is_stats = request_line.startsWith("GET /stats ") || request_line.startsWith("HEAD /stats ");
  const bool is_stats_json = request_line.startsWith("GET /stats.json ") || request_line.startsWith("HEAD /stats.json ");
  const bool is_config_export = request_line.startsWith("GET /config-export ") || request_line.startsWith("HEAD /config-export ");
  const bool is_config_save = request_line.startsWith("POST /config-save ");
  const bool is_config_import = request_line.startsWith("POST /config-import ");
  const bool is_advert = request_line.startsWith("POST /advert ");
  const bool is_sync_clock = request_line.startsWith("POST /sync-clock ");
  const bool is_reboot = request_line.startsWith("POST /reboot ");
  const bool is_logs_clear = request_line.startsWith("POST /logs-clear ");
  const bool is_health = request_line.startsWith("GET /health ") || request_line.startsWith("HEAD /health ") ||
                         request_line.startsWith("GET /healthz ") || request_line.startsWith("HEAD /healthz ");

  if (is_stats_json) {
    client.print("HTTP/1.1 302 Found\r\n");
    client.print("Location: /stats\r\n");
    client.print("Connection: close\r\n\r\n");
    return;
  }

  if (is_health) {
    writeTextResponse(client, 200, "OK", "text/plain", String("OK\n"), is_head);
    return;
  }

  if (is_logs_clear) {
    mcreStateClearLogs();
    mcreStateLogf("HTTP logs cleared from web");
    writeJsonResponse(client, 200, "OK", String("{\"ok\":true,\"message\":\"Logs cleared\"}\n"), is_head);
    return;
  }

  if (is_config_export) {
    String body = buildConfigExportJson(mesh);
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: application/json\r\n");
    client.print("Cache-Control: no-store\r\n");
    client.print("Content-Disposition: attachment; filename=\"meshcore-config.json\"\r\n");
    client.print("Connection: close\r\n");
    client.printf("Content-Length: %d\r\n\r\n", (int)body.length());
    if (!is_head) client.print(body);
    return;
  }

  if (is_config_save || is_config_import) {
    String err;
    if (applyConfigFromRequest(mesh, request_body, err)) {
      mcreStateLogf(is_config_import ? "HTTP config import applied" : "HTTP config saved");
      writeJsonResponse(client, 200, "OK", String("{\"ok\":true,\"message\":\"Configuration saved\"}\n"), is_head);
    } else {
      mcreStateLogf("HTTP config save failed: %s", err.c_str());
      String body = "{\"ok\":false,\"message\":\"" + httpJsonEscapeSimple(err) + "\"}\n";
      writeJsonResponse(client, 400, "Bad Request", body, is_head);
    }
    return;
  }

  if (is_advert) {
    mesh->sendSelfAdvertisement(250, true);
    mcreStateLogf("HTTP advert sent");
    writeJsonResponse(client, 200, "OK", String("{\"ok\":true,\"message\":\"Advert sent\"}\n"), is_head);
    return;
  }

  if (is_sync_clock) {
    String epoch_s = httpGetParam(request_body, "epoch");
    unsigned long epoch = strtoul(epoch_s.c_str(), nullptr, 10);
    if (epoch >= 946684800UL) {
      char cmd_buf[64];
      char reply[120] = {0};
      snprintf(cmd_buf, sizeof(cmd_buf), "time %lu", epoch);
      mesh->handleCommand(0, cmd_buf, reply);
      mcreStateLogf("HTTP clock synced: %lu", epoch);
      writeJsonResponse(client, 200, "OK", String("{\"ok\":true,\"message\":\"Clock synced\"}\n"), is_head);
    } else {
      writeJsonResponse(client, 400, "Bad Request", String("{\"ok\":false,\"message\":\"Missing/invalid epoch\"}\n"), is_head);
    }
    return;
  }

  if (is_reboot) {
    mcreStateLogf("HTTP reboot requested");
    writeJsonResponse(client, 200, "OK", String("{\"ok\":true,\"message\":\"Rebooting\"}\n"), is_head);
    client.stop();
    delay(120);
    board.reboot();
    return;
  }

  if (is_root) {
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: text/html; charset=utf-8\r\n");
    client.print("Cache-Control: no-store\r\n");
    client.print("Connection: close\r\n");
    client.printf("Content-Length: %d\r\n\r\n", (int)strlen(kRootHtml));
    if (!is_head) client.print(kRootHtml);
    return;
  }

  if (is_stats) {
    String body = buildStatsJson(mesh);
    writeJsonResponse(client, 200, "OK", body, is_head);
    return;
  }

  g_http_not_found++;
  writeTextResponse(client, 404, "Not Found", "text/plain", String("Not Found\n"), is_head);
}

static void httpAcceptClients() {
  if (!g_http_server) return;

  for (int pass = 0; pass < HTTP_MAX_CLIENTS * 2; pass++) {
    WiFiClient incoming = g_http_server->available();
    if (!incoming) break;

    const int slot_idx = httpFindFreeSlot();
    if (slot_idx < 0) {
      g_http_rejected++;
      String reject_ip = incoming.remoteIP().toString();
      mcreStateLogf("HTTP reject %s:%u busy", reject_ip.c_str(), (unsigned int)incoming.remotePort());
      writeTextResponse(incoming, 503, "Service Unavailable", "text/plain", String("Server busy, retry\n"));
      incoming.stop();
      continue;
    }

    HttpRequestSlot& slot = g_http_slots[slot_idx];
    slot = {};
    slot.client = incoming;
    slot.active = true;
    slot.headers_done = false;
    slot.accepted_ms = millis();
    slot.last_activity_ms = slot.accepted_ms;
    slot.content_length = 0;
    slot.header_buf.reserve(512);
    g_http_requests_total++;
    int active_now = httpCountActiveSlots();
    if ((uint32_t)active_now > g_http_max_parallel) g_http_max_parallel = (uint32_t)active_now;
  }
}

static void httpProcessSlot(MyMesh* mesh, int idx) {
  if (!mesh || idx < 0 || idx >= HTTP_MAX_CLIENTS) return;
  HttpRequestSlot& slot = g_http_slots[idx];
  if (!slot.active) return;

  if (!slot.client.connected() && !slot.client.available()) {
    httpResetSlot(idx);
    return;
  }

  int read_budget = HTTP_READ_BUDGET_PER_LOOP;
  while (read_budget > 0 && slot.client.available()) {
    int ch = slot.client.read();
    if (ch < 0) break;
    slot.last_activity_ms = millis();

    if (!slot.headers_done) {
      slot.header_buf += (char)ch;
      if ((int)slot.header_buf.length() > HTTP_MAX_HEADER_BYTES) {
        writeTextResponse(slot.client, 431, "Request Header Fields Too Large", "text/plain", String("Header too large\n"));
        httpResetSlot(idx);
        return;
      }
      if (httpTryFinalizeHeaders(slot) && slot.content_length > HTTP_MAX_BODY_BYTES) {
        writeJsonResponse(slot.client, 413, "Payload Too Large",
                          String("{\"ok\":false,\"message\":\"Payload too large\"}\n"));
        httpResetSlot(idx);
        return;
      }
    } else if (slot.content_length > 0 && (int)slot.request_body.length() < slot.content_length) {
      slot.request_body += (char)ch;
      if ((int)slot.request_body.length() > HTTP_MAX_BODY_BYTES) {
        writeJsonResponse(slot.client, 413, "Payload Too Large",
                          String("{\"ok\":false,\"message\":\"Payload too large\"}\n"));
        httpResetSlot(idx);
        return;
      }
    }

    read_budget--;
  }

  if ((int32_t)(millis() - slot.last_activity_ms) > HTTP_CLIENT_TIMEOUT_MS) {
    writeTextResponse(slot.client, 408, "Request Timeout", "text/plain", String("Request timeout\n"));
    httpResetSlot(idx);
    return;
  }

  if (!slot.headers_done) return;
  if (slot.request_line.length() == 0) {
    writeTextResponse(slot.client, 400, "Bad Request", "text/plain", String("Malformed request\n"));
    httpResetSlot(idx);
    return;
  }

  const bool need_body = httpRequestNeedsBody(slot.request_line);
  if (need_body && slot.content_length > 0 && (int)slot.request_body.length() < slot.content_length) return;

  String body = slot.request_body;
  if (slot.content_length >= 0 && (int)body.length() > slot.content_length) {
    body = body.substring(0, slot.content_length);
  }
  httpHandleRequest(mesh, slot.client, slot.request_line, body);
  httpResetSlot(idx);
}

void mcreHttpInit() {
  for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
    g_http_slots[i] = {};
  }
}

void mcreHttpLoop() {
  MyMesh* mesh = mcreMesh();
  if (!mesh) return;

  if (!g_http_server && WiFi.status() == WL_CONNECTED) {
    g_http_server = new WiFiServer(HTTP_STATS_PORT);
    if (g_http_server) {
      g_http_server->begin();
      g_http_server->setNoDelay(true);
      mcreStateLogf("HTTP endpoint started on %s:%d", WiFi.localIP().toString().c_str(), HTTP_STATS_PORT);
      Serial.printf("[HTTP] Stats endpoint started on http://%s:%d/stats\n", WiFi.localIP().toString().c_str(), HTTP_STATS_PORT);
    }
  }

  if (!g_http_server) return;

  httpAcceptClients();
  int active_now = httpCountActiveSlots();
  if ((uint32_t)active_now > g_http_max_parallel) g_http_max_parallel = (uint32_t)active_now;
  for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
    httpProcessSlot(mesh, i);
  }
  active_now = httpCountActiveSlots();
  if ((uint32_t)active_now > g_http_max_parallel) g_http_max_parallel = (uint32_t)active_now;
}

#else

void mcreHttpInit() {}
void mcreHttpLoop() {}

#endif
