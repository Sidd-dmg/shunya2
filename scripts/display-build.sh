#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACTION="${1:-build}"
if [[ $# -gt 0 ]]; then shift; fi
exec python3 "$ROOT/scripts/shunya_build.py" display "$ACTION" "$@"
