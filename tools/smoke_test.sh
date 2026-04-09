#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
  cat <<'EOF'
Usage:
  tools/smoke_test.sh --device-ip <ip> [--tcp-port <port>] [--console-port <port>] [--stats-path <path>]
EOF
}

DEVICE_IP=""
TCP_PORT=5002
CONSOLE_PORT=5001
STATS_PATH="/stats"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device-ip)
      require_option_value "$1" "${2:-}"
      DEVICE_IP="$2"
      shift 2
      ;;
    --tcp-port)
      require_option_value "$1" "${2:-}"
      TCP_PORT="$2"
      shift 2
      ;;
    --console-port)
      require_option_value "$1" "${2:-}"
      CONSOLE_PORT="$2"
      shift 2
      ;;
    --stats-path)
      require_option_value "$1" "${2:-}"
      STATS_PATH="$2"
      shift 2
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

[ -n "$DEVICE_IP" ] || die "Missing --device-ip"
require_cmd curl
require_cmd nc

log_info "HTTP health check..."
curl -fsS --max-time 5 "http://${DEVICE_IP}/health" >/dev/null

log_info "HTTP stats check (${STATS_PATH})..."
tmp_stats="$(mktemp -t mcre_smoke_stats.XXXXXX.json)"
trap 'rm -f "$tmp_stats"' EXIT
curl -fsS --max-time 5 "http://${DEVICE_IP}${STATS_PATH}" >"$tmp_stats"

if ! grep -Eq "\"timestamp\"|\"wifi\"|\"ports\"|\"radio\"" "$tmp_stats"; then
  die "Stats payload does not look valid"
fi

log_info "TCP raw port check (${TCP_PORT})..."
nc -z -w 2 "$DEVICE_IP" "$TCP_PORT"

log_info "TCP console port check (${CONSOLE_PORT})..."
nc -z -w 2 "$DEVICE_IP" "$CONSOLE_PORT"

log_success "Smoke tests passed"
