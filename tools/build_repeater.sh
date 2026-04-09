#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
  cat <<'USAGE'
Usage:
  ./build.sh [options]

Core options:
  --target <name>           Target profile from config/targets (default: xiao_s3_wio)
  --pio-env <name>          Build a specific upstream PlatformIO env directly
  --upload-chip <chip>      Override esptool chip (esp32, esp32s3, esp32c3, esp32c6, ...)

Actions:
  --build                   Clone upstream, inject overlay, and compile firmware
  --upload                  Upload previously built merged image (no rebuild)
  --monitor                 Open serial monitor after upload (implies --upload)
  --smoke                   Run smoke tests after upload (requires --device-ip)
  --no-smoke                Disable smoke tests
  --clean                   Delete work directory before running

Utility:
  --device-ip <ip>          Device IP used by smoke tests
  --upload-port <dev>       Explicit serial port (for upload/monitor)
  --wifi-ssid <ssid>        Override WiFi SSID (compile-time bootstrap)
  --wifi-password <pass>    Override WiFi password (compile-time bootstrap)
  --wifi-open               Force open WiFi (empty password)
  --tcp-port <port>         Override RAW TCP port (compile-time bootstrap)
  --console-port <port>     Override console TCP port (compile-time bootstrap)
  --mirror-port <port>      Override console mirror TCP port (compile-time bootstrap)
  --http-port <port>        Override HTTP port (compile-time bootstrap)
  --firmware-version <ver>  Override firmware version string (default: auto from upstream + '-extended')
  --mqtt-enable             Enable MQTT compile path for this build
  --mqtt-disable            Disable MQTT compile path for this build
  --mqtt-iata <code>        Required for MQTT builds (2-4 letters)
  --letsmesh, --LetsMesh    Enable both LetsMesh brokers (US + EU, token auth)
  --mqtt-custom-name <n>    Enable custom/extra broker profile name
  --mqtt-custom-host <h>    Enable custom/extra broker host
  --mqtt-custom-user <u>    Enable custom/extra broker username
  --mqtt-custom-pass <p>    Enable custom/extra broker password
  --mqtt-custom-port <p>    Optional custom/extra broker port (default 1883)
  --mqtt-custom-transport <t>
                            Optional custom/extra transport (tcp|websockets)
  --mqtt-custom-tls <0|1>   Optional custom/extra TLS enable
  --mqtt-custom-verify <0|1>
                            Optional custom/extra TLS verification
  --validate-config         Validate effective config and exit
  --allow-upload-fallback-rebuild
                            Allow PlatformIO upload fallback when merged image is missing
  --list-targets            List local target profiles from config/targets
  --list-esp32-envs         List discovered upstream ESP32 repeater environments
  --print-build-flags       Print effective PLATFORMIO_BUILD_FLAGS and exit
  --print-effective-config  Print effective bootstrap config (secrets redacted) and exit
  --help                    Show this help

Behavior:
  - If no action flags are provided, help is shown.
  - --upload uses existing artifacts and does not rebuild.
  - Fallback rebuild on upload is disabled by default (enable explicitly with --allow-upload-fallback-rebuild).
  - MQTT build is strict: when enabled (via config or CLI), it requires
    a valid IATA code and at least one enabled broker profile.
  - CLI path for MQTT profile selection: --letsmesh and/or --mqtt-custom-*.

Examples:
  ./build.sh --list-targets
  ./build.sh --list-esp32-envs
  ./build.sh --target xiao_s3_wio --validate-config --print-build-flags
  ./build.sh --target xiao_s3_wio --build
  ./build.sh --target xiao_s3_wio --build --wifi-ssid <ssid> --wifi-password <password>
  ./build.sh --target xiao_s3_wio --build --upload --upload-port <serial-port>
  ./build.sh --target xiao_s3_wio --upload --upload-port <serial-port>
  ./build.sh --target xiao_s3_wio --build --upload --monitor --upload-port <serial-port>
  ./build.sh --target xiao_s3_wio --build --upload --smoke --device-ip 192.168.1.50
  ./build.sh --target xiao_s3_wio --build --mqtt-enable --letsmesh --mqtt-iata SZZ
  ./build.sh --target xiao_s3_wio --build --mqtt-enable --mqtt-iata SZZ \
    --mqtt-custom-name custom --mqtt-custom-host mqtt.example.net \
    --mqtt-custom-user user --mqtt-custom-pass pass
  ./build.sh --pio-env Heltec_v3_repeater --build
USAGE
}

detect_host_os() {
  local os
  os="$(uname -s 2>/dev/null || echo unknown)"
  case "$os" in
    Darwin) echo "macos" ;;
    Linux) echo "linux" ;;
    *) echo "other" ;;
  esac
}

tool_install_hint() {
  local cmd="$1"
  local os="$2"
  case "$cmd" in
    git)
      if [ "$os" = "macos" ]; then
        echo "brew install git"
      elif [ "$os" = "linux" ]; then
        echo "Use your package manager, e.g. apt: sudo apt install git"
      else
        echo "Install git from your platform package manager"
      fi
      ;;
    python3)
      if [ "$os" = "macos" ]; then
        echo "brew install python"
      elif [ "$os" = "linux" ]; then
        echo "Use your package manager, e.g. apt: sudo apt install python3"
      else
        echo "Install Python 3 from your platform package manager"
      fi
      ;;
    pio)
      if [ "$os" = "macos" ]; then
        echo "pipx install platformio  (or: brew install platformio)"
      elif [ "$os" = "linux" ]; then
        echo "pipx install platformio  (or: pip3 install --user platformio)"
      else
        echo "Install PlatformIO CLI (command: pio)"
      fi
      ;;
    curl)
      if [ "$os" = "macos" ]; then
        echo "curl is usually preinstalled; otherwise: brew install curl"
      elif [ "$os" = "linux" ]; then
        echo "Use your package manager, e.g. apt: sudo apt install curl"
      else
        echo "Install curl from your platform package manager"
      fi
      ;;
    nc)
      if [ "$os" = "macos" ]; then
        echo "nc is usually preinstalled (netcat)"
      elif [ "$os" = "linux" ]; then
        echo "Use your package manager, e.g. apt: sudo apt install netcat-openbsd"
      else
        echo "Install netcat (nc) from your platform package manager"
      fi
      ;;
    esptool)
      if [ "$os" = "macos" ] || [ "$os" = "linux" ]; then
        echo "pipx install esptool  (or: pip3 install --user esptool)"
      else
        echo "Install esptool (or provide python3 + git for bundled fallback)"
      fi
      ;;
    *)
      echo "Install '${cmd}' from your platform package manager"
      ;;
  esac
}

require_any_cmd() {
  local found=1
  local c
  for c in "$@"; do
    if command -v "$c" >/dev/null 2>&1; then
      found=0
      break
    fi
  done
  return "$found"
}

preflight_require_cmds() {
  local os="$1"
  shift
  local missing=()
  local c
  for c in "$@"; do
    if ! command -v "$c" >/dev/null 2>&1; then
      missing+=("$c")
    fi
  done
  if [ "${#missing[@]}" -eq 0 ]; then
    return 0
  fi

  log_error "Missing required tools: ${missing[*]}"
  for c in "${missing[@]}"; do
    log_error "  - ${c}: $(tool_install_hint "$c" "$os")"
  done
  die "Install missing tools and rerun."
}

detect_pio_bin() {
  if command -v pio >/dev/null 2>&1; then
    echo "pio"
    return 0
  fi
  if command -v platformio >/dev/null 2>&1; then
    echo "platformio"
    return 0
  fi
  return 1
}

find_boot_app0_path() {
  local default_path
  default_path="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
  if [ -f "$default_path" ]; then
    echo "$default_path"
    return 0
  fi

  find "${HOME}/.platformio/packages" -path '*/framework-arduinoespressif32/tools/partitions/boot_app0.bin' -print -quit 2>/dev/null
}

ensure_vendor_esptool() {
  local repo_url="${ESPTOOL_REPO_URL:-https://github.com/espressif/esptool.git}"
  local repo_ref="${ESPTOOL_REPO_REF:-master}"
  local vendor_dir="${PROJECT_ROOT}/.tools/esptool"

  require_cmd git
  require_cmd python3

  mkdir -p "${PROJECT_ROOT}/.tools"
  if [ ! -d "${vendor_dir}/.git" ]; then
    log_info "Cloning esptool fallback (${repo_ref})..."
    git clone --depth 1 --branch "${repo_ref}" "${repo_url}" "${vendor_dir}" >/dev/null 2>&1 ||
      die "Failed to clone esptool fallback"
  fi

  [ -f "${vendor_dir}/esptool.py" ] || die "esptool fallback is missing esptool.py"
  echo "${vendor_dir}/esptool.py"
}

run_esptool_command() {
  if command -v esptool >/dev/null 2>&1; then
    esptool "$@"
    return
  fi
  if command -v esptool.py >/dev/null 2>&1; then
    esptool.py "$@"
    return
  fi

  local vendor_py
  vendor_py="$(ensure_vendor_esptool)"
  python3 "${vendor_py}" "$@"
}

infer_chip_from_image_file() {
  local image_file="$1"
  [ -f "$image_file" ] || return 1

  local info
  info="$(run_esptool_command image_info "$image_file" 2>/dev/null || true)"
  [ -n "$info" ] || return 1

  if echo "$info" | grep -qi 'ESP32-S3'; then
    echo "esp32s3"
    return 0
  fi
  if echo "$info" | grep -qi 'ESP32-S2'; then
    echo "esp32s2"
    return 0
  fi
  if echo "$info" | grep -qi 'ESP32-C6'; then
    echo "esp32c6"
    return 0
  fi
  if echo "$info" | grep -qi 'ESP32-C3'; then
    echo "esp32c3"
    return 0
  fi
  if echo "$info" | grep -qi 'ESP32'; then
    echo "esp32"
    return 0
  fi

  return 1
}

detect_upload_port() {
  if [ -n "${UPLOAD_PORT:-}" ]; then
    if [ -e "$UPLOAD_PORT" ]; then
      echo "$UPLOAD_PORT"
      return 0
    fi
    die "Configured upload port does not exist: ${UPLOAD_PORT}"
  fi

  local pattern
  local node
  shopt -s nullglob
  for pattern in ${UPLOAD_PORT_CANDIDATES}; do
    for node in $pattern; do
      echo "$node"
      shopt -u nullglob
      return 0
    done
  done
  shopt -u nullglob

  return 1
}

append_define_num() {
  local key="$1"
  local value="$2"
  PLATFORMIO_BUILD_FLAGS_COMPOSED+=" -D ${key}=${value}"
}

append_define_str() {
  local key="$1"
  local value="$2"
  local escaped
  escaped="$(escape_define_string "$value")"
  PLATFORMIO_BUILD_FLAGS_COMPOSED+=" -D ${key}='\"${escaped}\"'"
}

compose_platformio_build_flags() {
  PLATFORMIO_BUILD_FLAGS_COMPOSED=""

  append_define_str "WIFI_SSID" "${WIFI_SSID}"
  append_define_str "WIFI_PWD" "${WIFI_PASSWORD}"
  append_define_str "WIFI_PASSWORD" "${WIFI_PASSWORD}"

  append_define_num "TCP_PORT" "${TCP_PORT}"
  append_define_num "CONSOLE_PORT" "${CONSOLE_PORT}"
  append_define_num "CONSOLE_MIRROR_PORT" "${CONSOLE_MIRROR_PORT}"

  append_define_num "HTTP_STATS_PORT" "${HTTP_STATS_PORT}"
  append_define_num "HTTP_MAX_CLIENTS" "${HTTP_MAX_CLIENTS}"
  append_define_num "HTTP_MAX_HEADER_BYTES" "${HTTP_MAX_HEADER_BYTES}"
  append_define_num "HTTP_MAX_BODY_BYTES" "${HTTP_MAX_BODY_BYTES}"
  append_define_num "HTTP_CLIENT_TIMEOUT_MS" "${HTTP_CLIENT_TIMEOUT_MS}"
  append_define_num "HTTP_READ_BUDGET_PER_LOOP" "${HTTP_READ_BUDGET_PER_LOOP}"

  append_define_num "WIFI_DEBUG_LOGGING" "${WIFI_DEBUG_LOGGING}"
  append_define_num "MESH_PACKET_LOGGING" "${MESH_PACKET_LOGGING}"
  append_define_num "MESH_DEBUG" "${MESH_DEBUG}"
  append_define_num "BRIDGE_DEBUG" "${BRIDGE_DEBUG}"
  append_define_num "BLE_DEBUG_LOGGING" "${BLE_DEBUG_LOGGING}"

  append_define_num "MQTT_REPORTING_ENABLED" "${MQTT_REPORTING_ENABLED}"
  append_define_num "MQTT_MAX_ACTIVE_BROKERS" "${MQTT_MAX_ACTIVE_BROKERS}"
  append_define_str "MQTT_IATA" "${MQTT_IATA}"
  append_define_str "MQTT_TOPIC_STATUS" "${MQTT_TOPIC_STATUS}"
  append_define_str "MQTT_TOPIC_PACKETS" "${MQTT_TOPIC_PACKETS}"
  append_define_str "MQTT_TOPIC_RAW" "${MQTT_TOPIC_RAW}"

  append_define_num "MQTT_LETSMESH_US_ENABLED" "${MQTT_LETSMESH_US_ENABLED}"
  append_define_str "MQTT_LETSMESH_US_AUTH_METHOD" "${MQTT_LETSMESH_US_AUTH_METHOD}"
  append_define_num "MQTT_LETSMESH_EU_ENABLED" "${MQTT_LETSMESH_EU_ENABLED}"
  append_define_str "MQTT_LETSMESH_EU_AUTH_METHOD" "${MQTT_LETSMESH_EU_AUTH_METHOD}"

  append_define_num "MQTT_EXTRA_ENABLED" "${MQTT_EXTRA_ENABLED}"
  append_define_str "MQTT_EXTRA_NAME" "${MQTT_EXTRA_NAME}"
  append_define_str "MQTT_EXTRA_HOST" "${MQTT_EXTRA_HOST}"
  append_define_num "MQTT_EXTRA_PORT" "${MQTT_EXTRA_PORT}"
  append_define_num "MQTT_EXTRA_KEEPALIVE" "${MQTT_EXTRA_KEEPALIVE}"
  append_define_str "MQTT_EXTRA_TRANSPORT" "${MQTT_EXTRA_TRANSPORT}"
  append_define_str "MQTT_EXTRA_WS_PATH" "${MQTT_EXTRA_WS_PATH}"
  append_define_num "MQTT_EXTRA_TLS_ENABLED" "${MQTT_EXTRA_TLS_ENABLED}"
  append_define_num "MQTT_EXTRA_TLS_VERIFY" "${MQTT_EXTRA_TLS_VERIFY}"
  append_define_str "MQTT_EXTRA_AUTH_METHOD" "${MQTT_EXTRA_AUTH_METHOD}"
  append_define_str "MQTT_EXTRA_USERNAME" "${MQTT_EXTRA_USERNAME}"
  append_define_str "MQTT_EXTRA_PASSWORD" "${MQTT_EXTRA_PASSWORD}"
  append_define_str "MQTT_EXTRA_CLIENT_ID" "${MQTT_EXTRA_CLIENT_ID}"
  append_define_str "MQTT_EXTRA_AUDIENCE" "${MQTT_EXTRA_AUDIENCE}"
  append_define_str "MQTT_EXTRA_OWNER" "${MQTT_EXTRA_OWNER}"
  append_define_str "MQTT_EXTRA_EMAIL" "${MQTT_EXTRA_EMAIL}"

  append_define_str "MQTT_HOST" "${MQTT_HOST}"
  append_define_num "MQTT_PORT" "${MQTT_PORT}"
  append_define_num "MQTT_KEEPALIVE" "${MQTT_KEEPALIVE}"
  append_define_str "MQTT_TRANSPORT" "${MQTT_TRANSPORT}"
  append_define_str "MQTT_WS_PATH" "${MQTT_WS_PATH}"
  append_define_num "MQTT_TLS_ENABLED" "${MQTT_TLS_ENABLED}"
  append_define_num "MQTT_TLS_VERIFY" "${MQTT_TLS_VERIFY}"
  append_define_str "MQTT_AUTH_METHOD" "${MQTT_AUTH_METHOD}"
  append_define_str "MQTT_USERNAME" "${MQTT_USERNAME}"
  append_define_str "MQTT_PASSWORD" "${MQTT_PASSWORD}"
  append_define_str "MQTT_CLIENT_ID" "${MQTT_CLIENT_ID}"
  append_define_str "MQTT_AUDIENCE" "${MQTT_AUDIENCE}"
  append_define_str "MQTT_OWNER" "${MQTT_OWNER}"
  append_define_str "MQTT_EMAIL" "${MQTT_EMAIL}"

  append_define_str "FIRMWARE_VERSION" "${FIRMWARE_VERSION}"
  append_define_str "FIRMWARE_BUILD_DATE" "$(date '+%d %b %Y')"
}

generate_merged_image() {
  local build_dir="$1"
  local merged_path="${build_dir}/firmware-merged.bin"
  local bootloader="${build_dir}/bootloader.bin"
  local partitions="${build_dir}/partitions.bin"
  local firmware="${build_dir}/firmware.bin"
  local boot_app0
  local merge_chip="${UPLOAD_CHIP}"
  local detected_chip

  boot_app0="$(find_boot_app0_path || true)"
  if [ ! -f "$bootloader" ] || [ ! -f "$partitions" ] || [ ! -f "$firmware" ] || [ -z "$boot_app0" ] || [ ! -f "$boot_app0" ]; then
    log_warn "Skipping merged image generation: missing bootloader/partitions/firmware/boot_app0"
    return 0
  fi

  detected_chip="$(infer_chip_from_image_file "$bootloader" || true)"
  if [ -n "$detected_chip" ]; then
    merge_chip="$detected_chip"
  fi

  log_info "Generating merged ESP32 image..."
  run_esptool_command --chip "${merge_chip}" merge-bin -o "$merged_path" --flash_mode keep --flash_freq keep --flash_size keep \
    0x0000 "$bootloader" 0x8000 "$partitions" 0xE000 "$boot_app0" 0x10000 "$firmware" >/dev/null
  log_success "Merged image: ${merged_path}"
}

infer_upload_chip_from_env() {
  local env_name
  env_name="$(echo "${1:-}" | tr '[:upper:]' '[:lower:]')"
  if [[ "$env_name" == *"esp32c6"* ]] || [[ "$env_name" == *"_c6"* ]]; then
    echo "esp32c6"
  elif [[ "$env_name" == *"esp32c3"* ]] || [[ "$env_name" == *"_c3"* ]]; then
    echo "esp32c3"
  elif [[ "$env_name" == *"esp32s2"* ]] || [[ "$env_name" == *"_s2"* ]]; then
    echo "esp32s2"
  elif [[ "$env_name" == *"esp32s3"* ]] || [[ "$env_name" == *"_s3"* ]]; then
    echo "esp32s3"
  else
    echo "esp32"
  fi
}

safe_clean_workdir() {
  [ -n "${WORK_DIR:-}" ] || die "WORK_DIR is empty"
  if [[ "${WORK_DIR}" = /* ]]; then
    die "WORK_DIR must be a relative path, got: ${WORK_DIR}"
  fi
  case "${WORK_DIR}" in
    "."|"./"|"/"|".."|"../")
      die "Refusing to clean unsafe WORK_DIR: ${WORK_DIR}"
      ;;
  esac

  local clean_path="${PROJECT_ROOT}/${WORK_DIR}"
  [ "$clean_path" != "/" ] || die "Refusing to clean root path"

  log_info "Cleaning work directory: ${WORK_DIR}"
  rm -rf -- "${clean_path:?}"
}

clone_upstream_fresh() {
  local upstream_dir="$1"
  log_info "Cloning fresh upstream..."
  rm -rf "$upstream_dir"
  mkdir -p "$(dirname "$upstream_dir")"
  git clone --depth 1 --branch "${UPSTREAM_REPO_BRANCH}" "${UPSTREAM_REPO_URL}" "$upstream_dir" >/dev/null 2>&1
}

ensure_upstream_present() {
  local upstream_dir="$1"
  if [ -d "${upstream_dir}/.git" ]; then
    return 0
  fi
  clone_upstream_fresh "$upstream_dir"
}

validate_pio_env_exists() {
  local upstream_dir="$1"
  local env_name="$2"
  local files=("${upstream_dir}/platformio.ini")
  local variants_glob=("${upstream_dir}"/variants/*/platformio.ini)
  if [ -e "${variants_glob[0]}" ]; then
    files+=("${variants_glob[@]}")
  fi

  if ! grep -E -n "^\[env:${env_name}\]" "${files[@]}" >/dev/null 2>&1; then
    die "PlatformIO environment not found upstream: ${env_name}"
  fi
}

extract_upstream_firmware_version() {
  local upstream_dir="$1"
  local header="${upstream_dir}/examples/simple_repeater/MyMesh.h"
  [ -f "$header" ] || return 1

  grep -E '^[[:space:]]*#define[[:space:]]+FIRMWARE_VERSION[[:space:]]+"[^"]+"' "$header" \
    | head -n1 \
    | sed -E 's/.*"([^"]+)".*/\1/'
}

resolve_firmware_version_for_build() {
  # CLI override is highest priority.
  if [ -n "$CLI_FIRMWARE_VERSION" ]; then
    FIRMWARE_VERSION="$CLI_FIRMWARE_VERSION"
    return 0
  fi

  case "${FIRMWARE_VERSION:-}" in
    ""|auto|auto-extended)
      ;;
    *)
      return 0
      ;;
  esac

  local upstream_base
  upstream_base="$(extract_upstream_firmware_version "$UPSTREAM_DIR" || true)"
  [ -n "$upstream_base" ] || die "Failed to detect upstream FIRMWARE_VERSION from examples/simple_repeater/MyMesh.h"

  if [[ "$upstream_base" == *-extended ]]; then
    FIRMWARE_VERSION="$upstream_base"
  else
    FIRMWARE_VERSION="${upstream_base}-extended"
  fi

  log_info "Resolved firmware version from upstream: ${upstream_base} -> ${FIRMWARE_VERSION}"
}

list_target_profiles() {
  local targets_dir="${PROJECT_ROOT}/config/targets"
  if [ ! -d "$targets_dir" ]; then
    return 0
  fi
  find "$targets_dir" -maxdepth 1 -type f -name '*.env' -print | sed -E 's#.*/##; s#\.env$##' | sort
}

validate_enum_ci() {
  local label="$1"
  local value="$2"
  shift 2
  local normalized
  normalized="$(echo "$value" | tr '[:upper:]' '[:lower:]')"
  local candidate
  for candidate in "$@"; do
    if [ "$normalized" = "$(echo "$candidate" | tr '[:upper:]' '[:lower:]')" ]; then
      return 0
    fi
  done
  die "${label} has invalid value '${value}'. Allowed: $*"
}

validate_uint_range() {
  local label="$1"
  local value="$2"
  local min="$3"
  local max="$4"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    die "${label} must be an integer, got: ${value}"
  fi
  if [ "$value" -lt "$min" ] || [ "$value" -gt "$max" ]; then
    die "${label} must be between ${min} and ${max}, got: ${value}"
  fi
}

validate_build_bootstrap_config() {
  [ -n "${WIFI_SSID:-}" ] || die "WIFI_SSID cannot be empty (required for repeater web panel)"
  [ "${#WIFI_SSID}" -le 32 ] || die "WIFI_SSID must be at most 32 chars"
  if [ "${#WIFI_PASSWORD}" -gt 0 ] && [ "${#WIFI_PASSWORD}" -lt 8 ]; then
    die "WIFI_PASSWORD must be empty (open network) or at least 8 chars"
  fi
  [ "${#WIFI_PASSWORD}" -le 63 ] || die "WIFI_PASSWORD must be at most 63 chars"

  validate_uint_range "TCP_PORT" "${TCP_PORT}" 1 65535
  validate_uint_range "CONSOLE_PORT" "${CONSOLE_PORT}" 1 65535
  validate_uint_range "CONSOLE_MIRROR_PORT" "${CONSOLE_MIRROR_PORT}" 1 65535
  validate_uint_range "HTTP_STATS_PORT" "${HTTP_STATS_PORT}" 1 65535
  validate_uint_range "HTTP_MAX_CLIENTS" "${HTTP_MAX_CLIENTS}" 1 16
  validate_uint_range "HTTP_MAX_HEADER_BYTES" "${HTTP_MAX_HEADER_BYTES}" 1024 32768
  validate_uint_range "HTTP_MAX_BODY_BYTES" "${HTTP_MAX_BODY_BYTES}" 1024 65536
  validate_uint_range "HTTP_CLIENT_TIMEOUT_MS" "${HTTP_CLIENT_TIMEOUT_MS}" 250 60000
  validate_uint_range "HTTP_READ_BUDGET_PER_LOOP" "${HTTP_READ_BUDGET_PER_LOOP}" 64 8192

  validate_uint_range "WIFI_DEBUG_LOGGING" "${WIFI_DEBUG_LOGGING}" 0 1
  validate_uint_range "MESH_PACKET_LOGGING" "${MESH_PACKET_LOGGING}" 0 1
  validate_uint_range "MESH_DEBUG" "${MESH_DEBUG}" 0 1
  validate_uint_range "BRIDGE_DEBUG" "${BRIDGE_DEBUG}" 0 1
  validate_uint_range "BLE_DEBUG_LOGGING" "${BLE_DEBUG_LOGGING}" 0 1

  validate_uint_range "MQTT_REPORTING_ENABLED" "${MQTT_REPORTING_ENABLED}" 0 1
  validate_uint_range "MQTT_MAX_ACTIVE_BROKERS" "${MQTT_MAX_ACTIVE_BROKERS}" 1 4
  validate_uint_range "MQTT_LETSMESH_US_ENABLED" "${MQTT_LETSMESH_US_ENABLED}" 0 1
  validate_uint_range "MQTT_LETSMESH_EU_ENABLED" "${MQTT_LETSMESH_EU_ENABLED}" 0 1
  validate_uint_range "MQTT_EXTRA_ENABLED" "${MQTT_EXTRA_ENABLED}" 0 1
  validate_enum_ci "MQTT_LETSMESH_US_AUTH_METHOD" "${MQTT_LETSMESH_US_AUTH_METHOD}" token password none
  validate_enum_ci "MQTT_LETSMESH_EU_AUTH_METHOD" "${MQTT_LETSMESH_EU_AUTH_METHOD}" token password none
  validate_enum_ci "MQTT_EXTRA_AUTH_METHOD" "${MQTT_EXTRA_AUTH_METHOD}" token password none
  validate_enum_ci "MQTT_EXTRA_TRANSPORT" "${MQTT_EXTRA_TRANSPORT}" tcp websockets ws wss mqtt mqtts
  validate_uint_range "MQTT_EXTRA_PORT" "${MQTT_EXTRA_PORT}" 1 65535
  validate_uint_range "MQTT_EXTRA_KEEPALIVE" "${MQTT_EXTRA_KEEPALIVE}" 5 3600
  validate_uint_range "MQTT_EXTRA_TLS_ENABLED" "${MQTT_EXTRA_TLS_ENABLED}" 0 1
  validate_uint_range "MQTT_EXTRA_TLS_VERIFY" "${MQTT_EXTRA_TLS_VERIFY}" 0 1

  if [ "${MQTT_EXTRA_ENABLED}" = "1" ]; then
    [ -n "${MQTT_EXTRA_NAME}" ] || die "MQTT_EXTRA_NAME is required when MQTT_EXTRA_ENABLED=1"
    [ -n "${MQTT_EXTRA_HOST}" ] || die "MQTT_EXTRA_HOST is required when MQTT_EXTRA_ENABLED=1"
    [ -n "${MQTT_EXTRA_USERNAME}" ] || die "MQTT_EXTRA_USERNAME is required when MQTT_EXTRA_ENABLED=1"
    [ -n "${MQTT_EXTRA_PASSWORD}" ] || die "MQTT_EXTRA_PASSWORD is required when MQTT_EXTRA_ENABLED=1"
  fi

  validate_enum_ci "MQTT_AUTH_METHOD" "${MQTT_AUTH_METHOD}" token password none
  validate_enum_ci "MQTT_TRANSPORT" "${MQTT_TRANSPORT}" tcp websockets ws wss mqtt mqtts
  validate_uint_range "MQTT_PORT" "${MQTT_PORT}" 1 65535
  validate_uint_range "MQTT_KEEPALIVE" "${MQTT_KEEPALIVE}" 5 3600
  validate_uint_range "MQTT_TLS_ENABLED" "${MQTT_TLS_ENABLED}" 0 1
  validate_uint_range "MQTT_TLS_VERIFY" "${MQTT_TLS_VERIFY}" 0 1

  if ! [[ "${MQTT_IATA}" =~ ^[A-Za-z]{2,4}$ ]]; then
    die "MQTT_IATA must be 2-4 letters, got: ${MQTT_IATA}"
  fi
  [ -n "${MQTT_TOPIC_STATUS}" ] || die "MQTT_TOPIC_STATUS cannot be empty"
  [ -n "${MQTT_TOPIC_PACKETS}" ] || die "MQTT_TOPIC_PACKETS cannot be empty"
  [ -n "${MQTT_TOPIC_RAW}" ] || die "MQTT_TOPIC_RAW cannot be empty"
}

enforce_mqtt_cli_contract() {
  local cli_broker_selected=0
  local cli_custom_any=0
  local enabled_broker_count=0

  if [ "${CLI_MQTT_LETSMESH:-0}" -eq 1 ]; then
    cli_broker_selected=1
  fi

  if [ -n "${CLI_MQTT_CUSTOM_NAME:-}" ] || [ -n "${CLI_MQTT_CUSTOM_HOST:-}" ] || \
     [ -n "${CLI_MQTT_CUSTOM_USER:-}" ] || [ -n "${CLI_MQTT_CUSTOM_PASS:-}" ] || \
     [ -n "${CLI_MQTT_CUSTOM_PORT:-}" ] || [ -n "${CLI_MQTT_CUSTOM_TRANSPORT:-}" ] || \
     [ -n "${CLI_MQTT_CUSTOM_TLS:-}" ] || [ -n "${CLI_MQTT_CUSTOM_VERIFY:-}" ]; then
    cli_custom_any=1
    cli_broker_selected=1
  fi

  if [ "$cli_custom_any" -eq 1 ]; then
    [ -n "${CLI_MQTT_CUSTOM_NAME:-}" ] || die "Custom MQTT requires --mqtt-custom-name"
    [ -n "${CLI_MQTT_CUSTOM_HOST:-}" ] || die "Custom MQTT requires --mqtt-custom-host"
    [ -n "${CLI_MQTT_CUSTOM_USER:-}" ] || die "Custom MQTT requires --mqtt-custom-user"
    [ -n "${CLI_MQTT_CUSTOM_PASS:-}" ] || die "Custom MQTT requires --mqtt-custom-pass"
  fi

  if [ "${CLI_MQTT_ENABLE_SET:-0}" -eq 1 ] && [ "${CLI_MQTT_ENABLE_VALUE:-0}" = "1" ] && [ "$cli_broker_selected" -eq 0 ]; then
    die "MQTT enabled from CLI but no brokers selected. Use --letsmesh and/or --mqtt-custom-*."
  fi

  if [ "${MQTT_LETSMESH_US_ENABLED}" = "1" ]; then
    enabled_broker_count=$((enabled_broker_count + 1))
  fi
  if [ "${MQTT_LETSMESH_EU_ENABLED}" = "1" ]; then
    enabled_broker_count=$((enabled_broker_count + 1))
  fi
  if [ "${MQTT_EXTRA_ENABLED}" = "1" ]; then
    enabled_broker_count=$((enabled_broker_count + 1))
  fi
  if [ -n "${MQTT_HOST:-}" ]; then
    enabled_broker_count=$((enabled_broker_count + 1))
  fi

  if [ "${MQTT_REPORTING_ENABLED}" = "1" ]; then
    if [ -z "${MQTT_IATA:-}" ] || [ "${MQTT_IATA}" = "XXX" ]; then
      die "MQTT build requires explicit IATA. Use --mqtt-iata <code> or set MQTT_IATA in config/local.env."
    fi
    if [ "$enabled_broker_count" -eq 0 ]; then
      die "MQTT is enabled but no broker profiles are active. Use --letsmesh and/or --mqtt-custom-* (or set MQTT_* profile vars in config)."
    fi
    if [ "${MQTT_EXTRA_ENABLED}" = "1" ]; then
      [ -n "${MQTT_EXTRA_NAME}" ] || die "MQTT extra broker requires MQTT_EXTRA_NAME"
      [ -n "${MQTT_EXTRA_HOST}" ] || die "MQTT extra broker requires MQTT_EXTRA_HOST"
      [ -n "${MQTT_EXTRA_USERNAME}" ] || die "MQTT extra broker requires MQTT_EXTRA_USERNAME"
      [ -n "${MQTT_EXTRA_PASSWORD}" ] || die "MQTT extra broker requires MQTT_EXTRA_PASSWORD"
    fi
  fi
}

print_build_flags_multiline() {
  local flags="$1"
  echo "$flags" | sed 's/ -D /\n-D /g' | sed '/^$/d'
}

print_effective_config() {
  local pio_env_display
  pio_env_display="${PIO_ENV:-${PIO_ENV_OVERRIDE:-}}"
  cat <<EOF
TARGET=${TARGET}
PIO_ENV=${pio_env_display}
UPLOAD_CHIP=${UPLOAD_CHIP:-}
UPLOAD_BAUD=${UPLOAD_BAUD}
MONITOR_BAUD=${MONITOR_BAUD}
FIRMWARE_VERSION=${FIRMWARE_VERSION}
WIFI_SSID=${WIFI_SSID}
WIFI_PASSWORD=<redacted:${#WIFI_PASSWORD}>
TCP_PORT=${TCP_PORT}
CONSOLE_PORT=${CONSOLE_PORT}
CONSOLE_MIRROR_PORT=${CONSOLE_MIRROR_PORT}
HTTP_STATS_PORT=${HTTP_STATS_PORT}
HTTP_MAX_CLIENTS=${HTTP_MAX_CLIENTS}
HTTP_MAX_HEADER_BYTES=${HTTP_MAX_HEADER_BYTES}
HTTP_MAX_BODY_BYTES=${HTTP_MAX_BODY_BYTES}
HTTP_CLIENT_TIMEOUT_MS=${HTTP_CLIENT_TIMEOUT_MS}
HTTP_READ_BUDGET_PER_LOOP=${HTTP_READ_BUDGET_PER_LOOP}
WIFI_DEBUG_LOGGING=${WIFI_DEBUG_LOGGING}
MESH_PACKET_LOGGING=${MESH_PACKET_LOGGING}
MESH_DEBUG=${MESH_DEBUG}
BRIDGE_DEBUG=${BRIDGE_DEBUG}
BLE_DEBUG_LOGGING=${BLE_DEBUG_LOGGING}
MQTT_REPORTING_ENABLED=${MQTT_REPORTING_ENABLED}
MQTT_MAX_ACTIVE_BROKERS=${MQTT_MAX_ACTIVE_BROKERS}
MQTT_IATA=${MQTT_IATA}
MQTT_LETSMESH_US_ENABLED=${MQTT_LETSMESH_US_ENABLED}
MQTT_LETSMESH_EU_ENABLED=${MQTT_LETSMESH_EU_ENABLED}
MQTT_EXTRA_ENABLED=${MQTT_EXTRA_ENABLED}
MQTT_EXTRA_NAME=${MQTT_EXTRA_NAME}
MQTT_EXTRA_HOST=${MQTT_EXTRA_HOST}
MQTT_EXTRA_PORT=${MQTT_EXTRA_PORT}
MQTT_EXTRA_AUTH_METHOD=${MQTT_EXTRA_AUTH_METHOD}
MQTT_EXTRA_USERNAME=${MQTT_EXTRA_USERNAME}
MQTT_EXTRA_PASSWORD=<redacted:${#MQTT_EXTRA_PASSWORD}>
MQTT_HOST=${MQTT_HOST}
MQTT_PORT=${MQTT_PORT}
MQTT_TRANSPORT=${MQTT_TRANSPORT}
MQTT_AUTH_METHOD=${MQTT_AUTH_METHOD}
MQTT_USERNAME=${MQTT_USERNAME}
MQTT_PASSWORD=<redacted:${#MQTT_PASSWORD}>
MQTT_CLIENT_ID=${MQTT_CLIENT_ID}
EOF
}

list_upstream_esp32_repeater_envs() {
  local upstream_dir="$1"
  local variants_glob=("${upstream_dir}"/variants/*/platformio.ini)
  [ -e "${variants_glob[0]}" ] || return 0

  local meta
  meta="$({
    for f in "${variants_glob[@]}"; do
      awk '
        function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
        /^\[/ {
          sec = $0
          sub(/^\[/, "", sec)
          sub(/\]$/, "", sec)
          print "S|" sec
          next
        }
        {
          line = $0
          sub(/[;#].*$/, "", line)
          line = trim(line)
          if (sec == "" || line == "") next
          if (line ~ /^extends[ \t]*=/) {
            val = line
            sub(/^extends[ \t]*=/, "", val)
            val = trim(val)
            gsub(/[ \t]/, "", val)
            print "E|" sec "|" val
          } else if (line ~ /^platform[ \t]*=/) {
            val = line
            sub(/^platform[ \t]*=/, "", val)
            val = trim(val)
            print "P|" sec "|" val
          }
        }
      ' "$f"
    done
  } | sort -u)"

  declare -A sections=()
  declare -A extends_map=()
  declare -A esp32_sections=()

  while IFS='|' read -r kind sec val; do
    [ -n "$kind" ] || continue
    case "$kind" in
      S)
        sections["$sec"]=1
        ;;
      E)
        sections["$sec"]=1
        if [ -n "${extends_map[$sec]:-}" ]; then
          extends_map["$sec"]+="${extends_map[$sec]:+,}${val}"
        else
          extends_map["$sec"]="$val"
        fi
        ;;
      P)
        sections["$sec"]=1
        if echo "$val" | grep -qi 'espressif32'; then
          esp32_sections["$sec"]=1
        fi
        ;;
    esac
  done <<< "$meta"

  esp32_sections["esp32_base"]=1
  esp32_sections["esp32c6_base"]=1

  for _ in {1..24}; do
    local changed=0
    for sec in "${!sections[@]}"; do
      if [ "${esp32_sections[$sec]:-0}" -eq 1 ]; then
        continue
      fi
      IFS=',' read -r -a parents <<< "${extends_map[$sec]:-}"
      for p in "${parents[@]}"; do
        p="${p//[[:space:]]/}"
        [ -n "$p" ] || continue
        if [ "${esp32_sections[$p]:-0}" -eq 1 ]; then
          esp32_sections["$sec"]=1
          changed=1
          break
        fi
      done
    done
    [ "$changed" -eq 0 ] && break
  done

  for sec in "${!sections[@]}"; do
    if [[ "$sec" == env:*_repeater ]] && [ "${esp32_sections[$sec]:-0}" -eq 1 ]; then
      echo "${sec#env:}"
    fi
  done | sort
}

PROJECT_ROOT="$(project_root)"
HOST_OS="$(detect_host_os)"
PIO_BIN=""

TARGET="xiao_s3_wio"
PIO_ENV_OVERRIDE=""
PIO_ENV=""
UPLOAD_CHIP_OVERRIDE=""
LIST_TARGETS=0
LIST_ESP32_ENVS=0
PRINT_BUILD_FLAGS=0
PRINT_EFFECTIVE_CONFIG=0
DO_VALIDATE_CONFIG=0

DO_BUILD=0
DO_UPLOAD=0
DO_MONITOR=0
DO_SMOKE=0
DO_CLEAN=0
ALLOW_UPLOAD_FALLBACK_REBUILD=0
DEVICE_IP="${DEVICE_IP:-}"

CLI_WIFI_SSID=""
CLI_WIFI_PASSWORD=""
CLI_WIFI_PASSWORD_SET=0
CLI_WIFI_OPEN=0
CLI_TCP_PORT=""
CLI_CONSOLE_PORT=""
CLI_MIRROR_PORT=""
CLI_HTTP_PORT=""
CLI_FIRMWARE_VERSION=""
CLI_MQTT_ENABLE_SET=0
CLI_MQTT_ENABLE_VALUE=""
CLI_MQTT_IATA=""
CLI_MQTT_LETSMESH=0
CLI_MQTT_CUSTOM_NAME=""
CLI_MQTT_CUSTOM_HOST=""
CLI_MQTT_CUSTOM_USER=""
CLI_MQTT_CUSTOM_PASS=""
CLI_MQTT_CUSTOM_PORT=""
CLI_MQTT_CUSTOM_TRANSPORT=""
CLI_MQTT_CUSTOM_TLS=""
CLI_MQTT_CUSTOM_VERIFY=""

if [[ $# -eq 0 ]]; then
  usage
  exit 0
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      require_option_value "$1" "${2:-}"
      TARGET="$2"
      shift 2
      ;;
    --pio-env)
      require_option_value "$1" "${2:-}"
      PIO_ENV_OVERRIDE="$2"
      shift 2
      ;;
    --upload-chip)
      require_option_value "$1" "${2:-}"
      UPLOAD_CHIP_OVERRIDE="$2"
      shift 2
      ;;
    --build)
      DO_BUILD=1
      shift
      ;;
    --upload)
      DO_UPLOAD=1
      shift
      ;;
    --monitor)
      DO_MONITOR=1
      DO_UPLOAD=1
      shift
      ;;
    --smoke)
      DO_SMOKE=1
      shift
      ;;
    --no-smoke)
      DO_SMOKE=0
      shift
      ;;
    --clean)
      DO_CLEAN=1
      shift
      ;;
    --device-ip)
      require_option_value "$1" "${2:-}"
      DEVICE_IP="$2"
      shift 2
      ;;
    --upload-port)
      require_option_value "$1" "${2:-}"
      UPLOAD_PORT="$2"
      shift 2
      ;;
    --wifi-ssid)
      require_option_value "$1" "${2:-}"
      CLI_WIFI_SSID="$2"
      shift 2
      ;;
    --wifi-password)
      require_option_value "$1" "${2:-}"
      CLI_WIFI_PASSWORD="$2"
      CLI_WIFI_PASSWORD_SET=1
      shift 2
      ;;
    --wifi-open)
      CLI_WIFI_OPEN=1
      CLI_WIFI_PASSWORD_SET=1
      CLI_WIFI_PASSWORD=""
      shift
      ;;
    --tcp-port)
      require_option_value "$1" "${2:-}"
      CLI_TCP_PORT="$2"
      shift 2
      ;;
    --console-port)
      require_option_value "$1" "${2:-}"
      CLI_CONSOLE_PORT="$2"
      shift 2
      ;;
    --mirror-port)
      require_option_value "$1" "${2:-}"
      CLI_MIRROR_PORT="$2"
      shift 2
      ;;
    --http-port)
      require_option_value "$1" "${2:-}"
      CLI_HTTP_PORT="$2"
      shift 2
      ;;
    --firmware-version)
      require_option_value "$1" "${2:-}"
      CLI_FIRMWARE_VERSION="$2"
      shift 2
      ;;
    --mqtt-enable)
      CLI_MQTT_ENABLE_SET=1
      CLI_MQTT_ENABLE_VALUE="1"
      shift
      ;;
    --mqtt-disable)
      CLI_MQTT_ENABLE_SET=1
      CLI_MQTT_ENABLE_VALUE="0"
      shift
      ;;
    --mqtt-iata)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_IATA="$2"
      shift 2
      ;;
    --letsmesh|--LetsMesh)
      CLI_MQTT_LETSMESH=1
      shift
      ;;
    --mqtt-custom-name)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_NAME="$2"
      shift 2
      ;;
    --mqtt-custom-host)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_HOST="$2"
      shift 2
      ;;
    --mqtt-custom-user)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_USER="$2"
      shift 2
      ;;
    --mqtt-custom-pass)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_PASS="$2"
      shift 2
      ;;
    --mqtt-custom-port)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_PORT="$2"
      shift 2
      ;;
    --mqtt-custom-transport)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_TRANSPORT="$2"
      shift 2
      ;;
    --mqtt-custom-tls)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_TLS="$2"
      shift 2
      ;;
    --mqtt-custom-verify)
      require_option_value "$1" "${2:-}"
      CLI_MQTT_CUSTOM_VERIFY="$2"
      shift 2
      ;;
    --validate-config)
      DO_VALIDATE_CONFIG=1
      shift
      ;;
    --allow-upload-fallback-rebuild)
      ALLOW_UPLOAD_FALLBACK_REBUILD=1
      shift
      ;;
    --list-targets)
      LIST_TARGETS=1
      shift
      ;;
    --list-esp32-envs)
      LIST_ESP32_ENVS=1
      shift
      ;;
    --print-build-flags)
      PRINT_BUILD_FLAGS=1
      shift
      ;;
    --print-effective-config)
      PRINT_EFFECTIVE_CONFIG=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      die "Unknown option: $1"
      ;;
  esac
done

if [ "$CLI_WIFI_OPEN" -eq 1 ] && [ "$CLI_WIFI_PASSWORD_SET" -eq 1 ] && [ -n "$CLI_WIFI_PASSWORD" ]; then
  die "Use either --wifi-password or --wifi-open, not both"
fi

if [ "$LIST_TARGETS" -eq 1 ] && [ "$LIST_ESP32_ENVS" -eq 0 ] &&
   [ "$PRINT_BUILD_FLAGS" -eq 0 ] && [ "$PRINT_EFFECTIVE_CONFIG" -eq 0 ] && [ "$DO_VALIDATE_CONFIG" -eq 0 ] &&
   [ "$DO_BUILD" -eq 0 ] && [ "$DO_UPLOAD" -eq 0 ] && [ "$DO_MONITOR" -eq 0 ] &&
   [ "$DO_SMOKE" -eq 0 ] && [ "$DO_CLEAN" -eq 0 ]; then
  list_target_profiles
  exit 0
fi

if [ "$DO_BUILD" -eq 1 ] || [ "$DO_UPLOAD" -eq 1 ] || [ "$DO_MONITOR" -eq 1 ] || [ "$LIST_ESP32_ENVS" -eq 1 ] || [ "$DO_SMOKE" -eq 1 ]; then
  preflight_require_cmds "$HOST_OS" awk grep sed find sort
fi

if [ "$DO_BUILD" -eq 1 ] || [ "$LIST_ESP32_ENVS" -eq 1 ]; then
  preflight_require_cmds "$HOST_OS" git
fi

if [ "$DO_BUILD" -eq 1 ] || [ "$DO_MONITOR" -eq 1 ] || [ "$ALLOW_UPLOAD_FALLBACK_REBUILD" -eq 1 ]; then
  PIO_BIN="$(detect_pio_bin || true)"
  if [ -z "$PIO_BIN" ]; then
    log_error "Missing required tools: pio/platformio"
    log_error "  - pio: $(tool_install_hint "pio" "$HOST_OS")"
    die "Install PlatformIO CLI and rerun."
  fi
fi

if [ "$DO_UPLOAD" -eq 1 ] || [ "$DO_BUILD" -eq 1 ]; then
  if ! require_any_cmd esptool esptool.py; then
    preflight_require_cmds "$HOST_OS" python3 git
    log_info "esptool/esptool.py not found in PATH; bundled fallback will be used."
  fi
fi

if [ "$DO_SMOKE" -eq 1 ]; then
  preflight_require_cmds "$HOST_OS" curl nc
fi

load_env_file "${PROJECT_ROOT}/config/base.env"

NEED_TARGET_PROFILE=0
if [ "$DO_BUILD" -eq 1 ] || [ "$DO_UPLOAD" -eq 1 ] || [ "$DO_MONITOR" -eq 1 ] || [ "$DO_SMOKE" -eq 1 ] ||
   [ "$PRINT_BUILD_FLAGS" -eq 1 ] || [ "$PRINT_EFFECTIVE_CONFIG" -eq 1 ] || [ "$DO_VALIDATE_CONFIG" -eq 1 ]; then
  NEED_TARGET_PROFILE=1
fi

if [ -z "$PIO_ENV_OVERRIDE" ] && [ "$NEED_TARGET_PROFILE" -eq 1 ]; then
  TARGET_FILE="${PROJECT_ROOT}/config/targets/${TARGET}.env"
  [ -f "$TARGET_FILE" ] || die "Unknown target profile: ${TARGET} (missing ${TARGET_FILE})"
  load_env_file "$TARGET_FILE"
else
  if [ -n "$PIO_ENV_OVERRIDE" ]; then
    PIO_ENV="$PIO_ENV_OVERRIDE"
    TARGET_READY=1
  fi
fi

HAS_LOCAL_ENV=0
if [ -f "${PROJECT_ROOT}/config/local.env" ]; then
  HAS_LOCAL_ENV=1
fi
load_env_file "${PROJECT_ROOT}/config/local.env"

[ "${BUILD_ROLE:-}" = "repeater" ] || die "BUILD_ROLE must be 'repeater' for this project"

if [ "$LIST_TARGETS" -eq 1 ]; then
  list_target_profiles
fi

UPSTREAM_DIR="${PROJECT_ROOT}/${WORK_DIR}/meshcore-upstream"

if [ "$LIST_ESP32_ENVS" -eq 1 ]; then
  ensure_upstream_present "$UPSTREAM_DIR"
  list_upstream_esp32_repeater_envs "$UPSTREAM_DIR"
fi

if [ "$LIST_TARGETS" -eq 1 ] || [ "$LIST_ESP32_ENVS" -eq 1 ]; then
  if [ "$DO_BUILD" -eq 0 ] && [ "$DO_UPLOAD" -eq 0 ] && [ "$DO_MONITOR" -eq 0 ] && [ "$DO_SMOKE" -eq 0 ] && [ "$DO_CLEAN" -eq 0 ] && [ "$DO_VALIDATE_CONFIG" -eq 0 ]; then
    exit 0
  fi
fi

if [ "$NEED_TARGET_PROFILE" -eq 1 ]; then
  if [ -n "$PIO_ENV_OVERRIDE" ]; then
    UPLOAD_CHIP="${UPLOAD_CHIP_OVERRIDE:-$(infer_upload_chip_from_env "$PIO_ENV")}"
    UPLOAD_PORT_CANDIDATES="${UPLOAD_PORT_CANDIDATES:-/dev/cu.usbmodem* /dev/ttyACM* /dev/cu.SLAB_USBtoUART*}"
  else
    [ "${TARGET_READY:-0}" = "1" ] || die "Target '${TARGET}' is not marked as ready"
    if [ -n "$UPLOAD_CHIP_OVERRIDE" ]; then
      UPLOAD_CHIP="$UPLOAD_CHIP_OVERRIDE"
    fi
  fi
fi

WIFI_SSID="${WIFI_SSID:-}"
WIFI_PASSWORD="${WIFI_PASSWORD:-}"
TCP_PORT="${TCP_PORT:-5002}"
CONSOLE_PORT="${CONSOLE_PORT:-5001}"
CONSOLE_MIRROR_PORT="${CONSOLE_MIRROR_PORT:-5003}"
HTTP_STATS_PORT="${HTTP_STATS_PORT:-80}"
HTTP_MAX_CLIENTS="${HTTP_MAX_CLIENTS:-4}"
HTTP_MAX_HEADER_BYTES="${HTTP_MAX_HEADER_BYTES:-6144}"
HTTP_MAX_BODY_BYTES="${HTTP_MAX_BODY_BYTES:-12288}"
HTTP_CLIENT_TIMEOUT_MS="${HTTP_CLIENT_TIMEOUT_MS:-2500}"
HTTP_READ_BUDGET_PER_LOOP="${HTTP_READ_BUDGET_PER_LOOP:-768}"
WIFI_DEBUG_LOGGING="${WIFI_DEBUG_LOGGING:-1}"
MESH_PACKET_LOGGING="${MESH_PACKET_LOGGING:-1}"
MESH_DEBUG="${MESH_DEBUG:-0}"
BRIDGE_DEBUG="${BRIDGE_DEBUG:-0}"
BLE_DEBUG_LOGGING="${BLE_DEBUG_LOGGING:-0}"
MQTT_REPORTING_ENABLED="${MQTT_REPORTING_ENABLED:-0}"
MQTT_MAX_ACTIVE_BROKERS="${MQTT_MAX_ACTIVE_BROKERS:-4}"
MQTT_IATA="${MQTT_IATA:-XXX}"
MQTT_TOPIC_STATUS="${MQTT_TOPIC_STATUS-}"
MQTT_TOPIC_PACKETS="${MQTT_TOPIC_PACKETS-}"
MQTT_TOPIC_RAW="${MQTT_TOPIC_RAW-}"
if [ -z "$MQTT_TOPIC_STATUS" ]; then
  MQTT_TOPIC_STATUS='meshcore/{IATA}/{PUBLIC_KEY}/status'
fi
if [ -z "$MQTT_TOPIC_PACKETS" ]; then
  MQTT_TOPIC_PACKETS='meshcore/{IATA}/{PUBLIC_KEY}/packets'
fi
if [ -z "$MQTT_TOPIC_RAW" ]; then
  MQTT_TOPIC_RAW='meshcore/{IATA}/{PUBLIC_KEY}/raw'
fi
MQTT_LETSMESH_US_ENABLED="${MQTT_LETSMESH_US_ENABLED:-0}"
MQTT_LETSMESH_US_AUTH_METHOD="${MQTT_LETSMESH_US_AUTH_METHOD:-token}"
MQTT_LETSMESH_EU_ENABLED="${MQTT_LETSMESH_EU_ENABLED:-0}"
MQTT_LETSMESH_EU_AUTH_METHOD="${MQTT_LETSMESH_EU_AUTH_METHOD:-token}"
MQTT_EXTRA_ENABLED="${MQTT_EXTRA_ENABLED:-0}"
MQTT_EXTRA_NAME="${MQTT_EXTRA_NAME:-extra}"
MQTT_EXTRA_HOST="${MQTT_EXTRA_HOST:-}"
MQTT_EXTRA_PORT="${MQTT_EXTRA_PORT:-1883}"
MQTT_EXTRA_KEEPALIVE="${MQTT_EXTRA_KEEPALIVE:-60}"
MQTT_EXTRA_TRANSPORT="${MQTT_EXTRA_TRANSPORT:-tcp}"
MQTT_EXTRA_WS_PATH="${MQTT_EXTRA_WS_PATH:-/}"
MQTT_EXTRA_TLS_ENABLED="${MQTT_EXTRA_TLS_ENABLED:-0}"
MQTT_EXTRA_TLS_VERIFY="${MQTT_EXTRA_TLS_VERIFY:-1}"
MQTT_EXTRA_AUTH_METHOD="${MQTT_EXTRA_AUTH_METHOD:-none}"
MQTT_EXTRA_USERNAME="${MQTT_EXTRA_USERNAME:-}"
MQTT_EXTRA_PASSWORD="${MQTT_EXTRA_PASSWORD:-}"
MQTT_EXTRA_CLIENT_ID="${MQTT_EXTRA_CLIENT_ID:-}"
MQTT_EXTRA_AUDIENCE="${MQTT_EXTRA_AUDIENCE:-}"
MQTT_EXTRA_OWNER="${MQTT_EXTRA_OWNER:-}"
MQTT_EXTRA_EMAIL="${MQTT_EXTRA_EMAIL:-}"
MQTT_HOST="${MQTT_HOST:-}"
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_KEEPALIVE="${MQTT_KEEPALIVE:-60}"
MQTT_TRANSPORT="${MQTT_TRANSPORT:-tcp}"
MQTT_WS_PATH="${MQTT_WS_PATH:-/}"
MQTT_TLS_ENABLED="${MQTT_TLS_ENABLED:-0}"
MQTT_TLS_VERIFY="${MQTT_TLS_VERIFY:-1}"
MQTT_AUTH_METHOD="${MQTT_AUTH_METHOD:-none}"
MQTT_USERNAME="${MQTT_USERNAME:-}"
MQTT_PASSWORD="${MQTT_PASSWORD:-}"
MQTT_CLIENT_ID="${MQTT_CLIENT_ID:-}"
MQTT_AUDIENCE="${MQTT_AUDIENCE:-}"
MQTT_OWNER="${MQTT_OWNER:-}"
MQTT_EMAIL="${MQTT_EMAIL:-}"
FIRMWARE_VERSION="${FIRMWARE_VERSION:-auto-extended}"
UPLOAD_BAUD="${UPLOAD_BAUD:-460800}"
MONITOR_BAUD="${MONITOR_BAUD:-115200}"

# CLI bootstrap overrides (highest priority)
if [ -n "$CLI_WIFI_SSID" ]; then
  WIFI_SSID="$CLI_WIFI_SSID"
fi
if [ "$CLI_WIFI_PASSWORD_SET" -eq 1 ]; then
  WIFI_PASSWORD="$CLI_WIFI_PASSWORD"
fi
if [ -n "$CLI_TCP_PORT" ]; then
  TCP_PORT="$CLI_TCP_PORT"
fi
if [ -n "$CLI_CONSOLE_PORT" ]; then
  CONSOLE_PORT="$CLI_CONSOLE_PORT"
fi
if [ -n "$CLI_MIRROR_PORT" ]; then
  CONSOLE_MIRROR_PORT="$CLI_MIRROR_PORT"
fi
if [ -n "$CLI_HTTP_PORT" ]; then
  HTTP_STATS_PORT="$CLI_HTTP_PORT"
fi
if [ -n "$CLI_FIRMWARE_VERSION" ]; then
  FIRMWARE_VERSION="$CLI_FIRMWARE_VERSION"
fi

if [ "$CLI_MQTT_ENABLE_SET" -eq 1 ]; then
  MQTT_REPORTING_ENABLED="$CLI_MQTT_ENABLE_VALUE"
fi

if [ -n "$CLI_MQTT_IATA" ]; then
  MQTT_IATA="$CLI_MQTT_IATA"
fi

if [ "$CLI_MQTT_LETSMESH" -eq 1 ]; then
  MQTT_REPORTING_ENABLED=1
  MQTT_LETSMESH_US_ENABLED=1
  MQTT_LETSMESH_US_AUTH_METHOD=token
  MQTT_LETSMESH_EU_ENABLED=1
  MQTT_LETSMESH_EU_AUTH_METHOD=token
fi

if [ -n "$CLI_MQTT_CUSTOM_NAME" ] || [ -n "$CLI_MQTT_CUSTOM_HOST" ] || \
   [ -n "$CLI_MQTT_CUSTOM_USER" ] || [ -n "$CLI_MQTT_CUSTOM_PASS" ] || \
   [ -n "$CLI_MQTT_CUSTOM_PORT" ] || [ -n "$CLI_MQTT_CUSTOM_TRANSPORT" ] || \
   [ -n "$CLI_MQTT_CUSTOM_TLS" ] || [ -n "$CLI_MQTT_CUSTOM_VERIFY" ]; then
  MQTT_REPORTING_ENABLED=1
  MQTT_EXTRA_ENABLED=1
  [ -n "$CLI_MQTT_CUSTOM_NAME" ] && MQTT_EXTRA_NAME="$CLI_MQTT_CUSTOM_NAME"
  [ -n "$CLI_MQTT_CUSTOM_HOST" ] && MQTT_EXTRA_HOST="$CLI_MQTT_CUSTOM_HOST"
  [ -n "$CLI_MQTT_CUSTOM_USER" ] && MQTT_EXTRA_USERNAME="$CLI_MQTT_CUSTOM_USER"
  [ -n "$CLI_MQTT_CUSTOM_PASS" ] && MQTT_EXTRA_PASSWORD="$CLI_MQTT_CUSTOM_PASS"
  [ -n "$CLI_MQTT_CUSTOM_PORT" ] && MQTT_EXTRA_PORT="$CLI_MQTT_CUSTOM_PORT"
  [ -n "$CLI_MQTT_CUSTOM_TRANSPORT" ] && MQTT_EXTRA_TRANSPORT="$CLI_MQTT_CUSTOM_TRANSPORT"
  [ -n "$CLI_MQTT_CUSTOM_TLS" ] && MQTT_EXTRA_TLS_ENABLED="$CLI_MQTT_CUSTOM_TLS"
  [ -n "$CLI_MQTT_CUSTOM_VERIFY" ] && MQTT_EXTRA_TLS_VERIFY="$CLI_MQTT_CUSTOM_VERIFY"
  if [ -z "${MQTT_EXTRA_AUTH_METHOD:-}" ] || [ "${MQTT_EXTRA_AUTH_METHOD}" = "none" ]; then
    MQTT_EXTRA_AUTH_METHOD="password"
  fi
fi

if [ "$NEED_TARGET_PROFILE" -eq 1 ]; then
  # Strict bootstrap validation is required only for build and explicit
  # config validation. Upload/smoke actions operate on existing artifacts.
  if [ "$DO_BUILD" -eq 1 ] || [ "$DO_VALIDATE_CONFIG" -eq 1 ]; then
    if [ "$HAS_LOCAL_ENV" -eq 0 ]; then
      if [ "$WIFI_SSID" = "mesh_ssid" ] || [ "$WIFI_SSID" = "" ]; then
        die "config/local.env not found. Provide required bootstrap via CLI, e.g. --wifi-ssid <ssid> --wifi-password <pass> (or --wifi-open)."
      fi
      if [ "$WIFI_PASSWORD" = "mesh_password" ]; then
        die "config/local.env not found and default placeholder WiFi password is active. Use --wifi-password <pass> or --wifi-open."
      fi
    fi

    if [ "$WIFI_SSID" = "your_wifi_ssid" ]; then
      die "WIFI_SSID is still a placeholder value ('your_wifi_ssid'). Set a real SSID in config/local.env or via --wifi-ssid."
    fi
    if [ "$WIFI_PASSWORD" = "your_wifi_password" ]; then
      die "WIFI_PASSWORD is still a placeholder value ('your_wifi_password'). Set a real password or use --wifi-open."
    fi

    enforce_mqtt_cli_contract
    validate_build_bootstrap_config
  fi
fi

if [ "$DO_VALIDATE_CONFIG" -eq 1 ]; then
  log_success "Configuration validation passed"
fi

if [ "$PRINT_EFFECTIVE_CONFIG" -eq 1 ]; then
  print_effective_config
fi

if [ "$PRINT_BUILD_FLAGS" -eq 1 ]; then
  compose_platformio_build_flags
  print_build_flags_multiline "${PLATFORMIO_BUILD_FLAGS_COMPOSED}"
fi

if [ "$PRINT_BUILD_FLAGS" -eq 1 ] || [ "$PRINT_EFFECTIVE_CONFIG" -eq 1 ] || [ "$DO_VALIDATE_CONFIG" -eq 1 ]; then
  if [ "$DO_BUILD" -eq 0 ] && [ "$DO_UPLOAD" -eq 0 ] && [ "$DO_MONITOR" -eq 0 ] && [ "$DO_SMOKE" -eq 0 ] && [ "$DO_CLEAN" -eq 0 ]; then
    exit 0
  fi
fi

if [ "$DO_MONITOR" -eq 1 ] && [ -z "${UPLOAD_PORT:-}" ]; then
  UPLOAD_PORT="$(detect_upload_port || true)"
fi

if [ "$DO_CLEAN" -eq 1 ]; then
  safe_clean_workdir
fi

BUILD_DIR="${UPSTREAM_DIR}/.pio/build/${PIO_ENV}"

if [ "$DO_BUILD" -eq 1 ]; then
  clone_upstream_fresh "$UPSTREAM_DIR"
  validate_pio_env_exists "$UPSTREAM_DIR" "$PIO_ENV"
  resolve_firmware_version_for_build

  log_info "Injecting custom overlay..."
  "${PROJECT_ROOT}/tools/inject_customizations.sh" --repo "$UPSTREAM_DIR"

  compose_platformio_build_flags
  export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS_COMPOSED}"
  log_info "Building environment: ${PIO_ENV}"
  "$PIO_BIN" run -d "$UPSTREAM_DIR" -e "$PIO_ENV"

  generate_merged_image "$BUILD_DIR"
  log_success "Build completed"
fi

if [ "$DO_UPLOAD" -eq 1 ]; then
  validate_pio_env_exists "$UPSTREAM_DIR" "$PIO_ENV"

  local_port="${UPLOAD_PORT:-}"
  if [ -z "$local_port" ]; then
    local_port="$(detect_upload_port || true)"
  fi
  [ -n "$local_port" ] || die "Upload requested but no serial port was detected"

  merged_image="${BUILD_DIR}/firmware-merged.bin"
  firmware_image="${BUILD_DIR}/firmware.bin"
  upload_chip="${UPLOAD_CHIP}"
  detected_chip="$(infer_chip_from_image_file "${BUILD_DIR}/bootloader.bin" || true)"
  if [ -n "$detected_chip" ]; then
    upload_chip="$detected_chip"
  fi

  if [ -f "$merged_image" ]; then
    log_info "Uploading merged image to ${local_port}"
    run_esptool_command --chip "${upload_chip}" --port "${local_port}" --baud "${UPLOAD_BAUD}" write-flash -z 0x0 "$merged_image"
  elif [ -f "$firmware_image" ]; then
    if [ "$ALLOW_UPLOAD_FALLBACK_REBUILD" -eq 1 ]; then
      log_warn "Merged image not found, using explicit fallback to PlatformIO upload (may rebuild)"
      "$PIO_BIN" run -d "$UPSTREAM_DIR" -e "$PIO_ENV" -t upload --upload-port "$local_port"
    else
      die "Merged image not found. Build first (./build.sh --build) or rerun with --allow-upload-fallback-rebuild."
    fi
  else
    die "No firmware artifact found. Run with --build first."
  fi
  log_success "Upload completed"
fi

if [ "$DO_SMOKE" -eq 1 ]; then
  [ -n "$DEVICE_IP" ] || die "Smoke tests require --device-ip"
  "${PROJECT_ROOT}/tools/smoke_test.sh" --device-ip "$DEVICE_IP" --tcp-port "$TCP_PORT" --console-port "$CONSOLE_PORT"
fi

if [ "$DO_MONITOR" -eq 1 ]; then
  local_port="${UPLOAD_PORT:-}"
  if [ -z "$local_port" ]; then
    local_port="$(detect_upload_port || true)"
  fi
  [ -n "$local_port" ] || die "Monitor requested but no serial port was detected"

  log_info "Opening serial monitor on ${local_port} @ ${MONITOR_BAUD}"
  "$PIO_BIN" device monitor --port "$local_port" --baud "$MONITOR_BAUD"
fi

log_success "Done"
