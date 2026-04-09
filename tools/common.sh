#!/usr/bin/env bash

set -Eeuo pipefail

BLUE="\033[0;34m"
GREEN="\033[0;32m"
YELLOW="\033[1;33m"
RED="\033[0;31m"
NC="\033[0m"

log_info() {
  echo -e "${BLUE}[*]${NC} $*"
}

log_success() {
  echo -e "${GREEN}[+]${NC} $*"
}

log_warn() {
  echo -e "${YELLOW}[!]${NC} $*"
}

log_error() {
  echo -e "${RED}[x]${NC} $*" >&2
}

die() {
  log_error "$*"
  exit 1
}

project_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "${script_dir}/.." && pwd
}

require_cmd() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || die "Missing required command: ${cmd}"
}

require_option_value() {
  local opt="$1"
  local val="${2:-}"
  if [ -z "$val" ]; then
    die "Option ${opt} requires a value"
  fi
}

load_env_file() {
  local file="$1"
  if [ -f "$file" ]; then
    # shellcheck disable=SC1090
    source "$file"
  fi
}

first_existing_path() {
  for path in "$@"; do
    if [ -e "$path" ]; then
      echo "$path"
      return 0
    fi
  done
  return 1
}

escape_define_string() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '%s' "$value"
}
