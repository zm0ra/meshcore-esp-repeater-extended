# Configuration Reference

This project uses layered env files:

1. `config/base.env` (repository defaults)
2. `config/targets/<target>.env` (board-specific profile)
3. `config/local.env` (local override, gitignored)

Later layers override earlier ones.
CLI bootstrap options override all file layers.

## Fast Validation (No Build)

- `./build.sh --target <target> --validate-config`
- `./build.sh --target <target> --print-effective-config`
- `./build.sh --target <target> --print-build-flags`

Without `config/local.env`, you can pass required bootstrap directly:

- `./build.sh --target <target> --validate-config --wifi-ssid <ssid> --wifi-password <pass>`

## What Stays in Env Files

Only values that must be available at boot (before the web panel is usable):

- Wi-Fi bootstrap: `WIFI_SSID`, `WIFI_PASSWORD`
- TCP/HTTP listener bootstrap: `TCP_PORT`, `CONSOLE_PORT`, `CONSOLE_MIRROR_PORT`, `HTTP_STATS_PORT`
- HTTP resource limits: `HTTP_MAX_CLIENTS`, `HTTP_MAX_HEADER_BYTES`, `HTTP_MAX_BODY_BYTES`, `HTTP_CLIENT_TIMEOUT_MS`, `HTTP_READ_BUDGET_PER_LOOP`
- Compile-time diagnostics: `WIFI_DEBUG_LOGGING`, `MESH_PACKET_LOGGING`, `MESH_DEBUG`, `BRIDGE_DEBUG`, `BLE_DEBUG_LOGGING`
- MQTT compile bootstrap (profile matrix + topics)

## What Is Runtime (Web Panel)

These are intentionally not compile-time configuration:

- Node name, owner info, coordinates
- Radio preset and LoRa tuning
- Bridge settings
- Routing/security runtime fields
- Operational actions (`Advert`, `Sync Clock`, `Reboot`, `Logs`)

## MQTT Compile Bootstrap Notes

Current architecture uses compile-time MQTT profile matrix. Keep only what you need:

- `MQTT_REPORTING_ENABLED`
- `MQTT_MAX_ACTIVE_BROKERS`
- `MQTT_IATA`
- topic templates (`MQTT_TOPIC_STATUS`, `MQTT_TOPIC_PACKETS`, `MQTT_TOPIC_RAW`)
- broker profile toggles (`MQTT_LETSMESH_*`, optional `MQTT_EXTRA_ENABLED`)
- optional extra/custom broker credentials in `config/local.env` only

Strict validation:

- If `MQTT_REPORTING_ENABLED=1`, build requires:
  - valid `MQTT_IATA` (2-4 letters),
  - at least one active broker profile.
- If `MQTT_EXTRA_ENABLED=1`, build requires:
  - `MQTT_EXTRA_NAME`,
  - `MQTT_EXTRA_HOST`,
  - `MQTT_EXTRA_USERNAME`,
  - `MQTT_EXTRA_PASSWORD`.

Broker endpoint mapping:

- LetsMesh US: controlled by `MQTT_LETSMESH_US_ENABLED` + `MQTT_LETSMESH_US_AUTH_METHOD` (host is built-in).
- LetsMesh EU: controlled by `MQTT_LETSMESH_EU_ENABLED` + `MQTT_LETSMESH_EU_AUTH_METHOD` (host is built-in).
- Extra broker profile: `MQTT_EXTRA_*` variables (host/port/transport/tls/auth/credentials).
- Optional custom profile: `MQTT_HOST`, `MQTT_PORT`, `MQTT_TRANSPORT`, `MQTT_TLS_*`, `MQTT_AUTH_*`.

## Security Guidance

- Never commit `config/local.env`.
- Keep passwords/tokens only in `config/local.env`.
- `/config-export` redacts secret fields by design.
- Treat env files as trusted local input (they are sourced by bash).
