#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <hub|sensor|demo> [build|upload|monitor|clean|bootlog] [--port DEVICE]" >&2
  exit 1
fi
TARGET="$1"
shift
ACTION="${1:-build}"
if [[ $# -gt 0 ]]; then shift; fi
exec python3 "$ROOT/scripts/shunya_build.py" idf "$TARGET" "$ACTION" "$@"
