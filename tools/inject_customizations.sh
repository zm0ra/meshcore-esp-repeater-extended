#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
  cat <<'EOF'
Usage:
  tools/inject_customizations.sh --repo <meshcore-clone-path>

Description:
  Copies overlay modules into examples/simple_repeater and injects minimal hooks
  into upstream MyMesh.cpp/MyMesh.h by signature anchors.
EOF
}

report_drift() {
  local mymesh_cpp="$1"
  local mymesh_h="$2"
  log_warn "Upstream signature report (for troubleshooting):"
  rg -n -e "MyMesh::begin" -e "MyMesh::loop" -e "MyMesh::logRx" -e "MyMesh::logTx" -e "_fs = fs;" -e "mesh::Mesh::loop();" -e "#include <algorithm>" "$mymesh_cpp" || true
  rg -n -e "#include \"RateLimiter.h\"" -e "class MyMesh" -e "struct NeighbourInfo" "$mymesh_h" || true
}

assert_anchor_exists() {
  local file="$1"
  local anchor="$2"
  local _label="$3"
  if ! grep -Fq "$anchor" "$file"; then
    return 1
  fi
  return 0
}

insert_after_anchor_once() {
  local file="$1"
  local anchor="$2"
  local marker="$3"
  local block="$4"

  if grep -Fq "$marker" "$file"; then
    return 0
  fi

  local tmp_file
  tmp_file="$(mktemp)"

  if ! awk -v anchor="$anchor" -v block="$block" '
    BEGIN {
      inserted = 0;
      n = split(block, lines, "\n");
    }
    {
      print $0;
      if (!inserted && index($0, anchor) > 0) {
        for (i = 1; i <= n; i++) {
          print lines[i];
        }
        inserted = 1;
      }
    }
    END {
      if (!inserted) {
        exit 41;
      }
    }
  ' "$file" > "$tmp_file"; then
    rm -f "$tmp_file"
    return 41
  fi

  mv "$tmp_file" "$file"
}

assert_exact_once_marker() {
  local file="$1"
  local marker="$2"
  local count
  count="$(grep -F -c "$marker" "$file" || true)"
  if [ "$count" -ne 1 ]; then
    die "Marker '${marker}' expected exactly once in ${file}, got ${count}"
  fi
}

check_anchor_or_fail() {
  local file="$1"
  local anchor="$2"
  local label="$3"
  if ! assert_anchor_exists "$file" "$anchor" "$label"; then
    log_error "Anchor not found (${label}): ${anchor}"
    report_drift "$MYMESH_CPP" "$MYMESH_H"
    die "Injection aborted due to upstream anchor drift"
  fi
}

insert_after_anchor_or_fail() {
  local file="$1"
  local anchor="$2"
  local marker="$3"
  local block="$4"
  if ! insert_after_anchor_once "$file" "$anchor" "$marker" "$block"; then
    log_error "Failed to inject marker '${marker}' after anchor: ${anchor}"
    report_drift "$MYMESH_CPP" "$MYMESH_H"
    die "Injection aborted due to upstream signature mismatch"
  fi
}

PROJECT_ROOT="$(project_root)"
REPO_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      require_option_value "$1" "${2:-}"
      REPO_DIR="$2"
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

[ -n "$REPO_DIR" ] || die "Missing --repo argument"
[ -d "$REPO_DIR" ] || die "Repository path does not exist: ${REPO_DIR}"

REPEATER_DIR="${REPO_DIR}/examples/simple_repeater"
MYMESH_CPP="${REPEATER_DIR}/MyMesh.cpp"
MYMESH_H="${REPEATER_DIR}/MyMesh.h"

[ -f "$MYMESH_CPP" ] || die "Missing file: ${MYMESH_CPP}"
[ -f "$MYMESH_H" ] || die "Missing file: ${MYMESH_H}"

check_anchor_or_fail "$MYMESH_CPP" "#include <algorithm>" "cpp include anchor"
check_anchor_or_fail "$MYMESH_H" "#include \"RateLimiter.h\"" "header include anchor"
check_anchor_or_fail "$MYMESH_CPP" "_fs = fs;" "begin anchor"
check_anchor_or_fail "$MYMESH_CPP" "mesh::Mesh::loop();" "loop anchor"
check_anchor_or_fail "$MYMESH_CPP" "void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {" "rx hook anchor"
check_anchor_or_fail "$MYMESH_CPP" "void MyMesh::logTx(mesh::Packet *pkt, int len) {" "tx hook anchor"

log_info "Copying overlay modules into upstream example tree..."
cp -f "${PROJECT_ROOT}/custom/include/"mcre_custom_*.h "${REPEATER_DIR}/"
cp -f "${PROJECT_ROOT}/custom/src/"mcre_custom_*.cpp "${REPEATER_DIR}/"

log_info "Injecting include and lifecycle hooks..."
insert_after_anchor_or_fail \
  "$MYMESH_CPP" \
  "#include <algorithm>" \
  "MCRE_INJECT:include" \
  "#include \"mcre_custom_entry.h\"  // MCRE_INJECT:include"

insert_after_anchor_or_fail \
  "$MYMESH_H" \
  "#include \"RateLimiter.h\"" \
  "MCRE_INJECT:contract_include" \
  "#include \"mcre_custom_contract.h\"  // MCRE_INJECT:contract_include"

insert_after_anchor_or_fail \
  "$MYMESH_CPP" \
  "_fs = fs;" \
  "MCRE_INJECT:init" \
  "  mcreCustomInit(this, fs);  // MCRE_INJECT:init"

insert_after_anchor_or_fail \
  "$MYMESH_CPP" \
  "mesh::Mesh::loop();" \
  "MCRE_INJECT:loop" \
  "  mcreCustomLoop(this);  // MCRE_INJECT:loop"

insert_after_anchor_or_fail \
  "$MYMESH_CPP" \
  "void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {" \
  "MCRE_INJECT:on_rx" \
  "  mcreCustomOnPacketRx(this, pkt, len, score, (int)_radio->getLastSNR(), (int)_radio->getLastRSSI());  // MCRE_INJECT:on_rx"

insert_after_anchor_or_fail \
  "$MYMESH_CPP" \
  "void MyMesh::logTx(mesh::Packet *pkt, int len) {" \
  "MCRE_INJECT:on_tx" \
  "  mcreCustomOnPacketTx(this, pkt, len);  // MCRE_INJECT:on_tx"

log_info "Verifying exact-once markers..."
assert_exact_once_marker "$MYMESH_CPP" "MCRE_INJECT:include"
assert_exact_once_marker "$MYMESH_CPP" "MCRE_INJECT:init"
assert_exact_once_marker "$MYMESH_CPP" "MCRE_INJECT:loop"
assert_exact_once_marker "$MYMESH_CPP" "MCRE_INJECT:on_rx"
assert_exact_once_marker "$MYMESH_CPP" "MCRE_INJECT:on_tx"
assert_exact_once_marker "$MYMESH_H" "MCRE_INJECT:contract_include"

log_success "Customization injection completed"
