#!/usr/bin/env bash
# Builds (if needed) and starts the app.
# Usage: ./run.sh [Debug|Release] [--yes]   (default: Release)
# VE Pro runs under Wine; WINEPREFIX tells the app where to find VSL's CLI.
# It defaults to the shared prefix of the ilok-linux repo this app sits in.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

config=Release yes=()
for a in "$@"; do
  case "$a" in
    Debug|Release) config=$a ;;
    --yes) yes=(--yes) ;;
    *) echo "Usage: $0 [Debug|Release] [--yes]" >&2; exit 1 ;;
  esac
done

./build.sh "$config" "${yes[@]}"

if [[ -z "${WINEPREFIX:-}" && -d ../prefix/drive_c ]]; then
  export WINEPREFIX="$(cd ../prefix && pwd)"
fi
# yabridge (Windows plugins under Wine) writes its log - and the Wine side's errors - next to the app's
if [[ -z "${YABRIDGE_DEBUG_FILE:-}" ]]; then
  logs="${XDG_CONFIG_HOME:-$HOME/.config}/DawPlus/Logs"
  mkdir -p "$logs"
  export YABRIDGE_DEBUG_FILE="$logs/yabridge.log"
fi
# (for now: every call between the app and a bridged plugin, but not audio - tracking down the crash
# in yabridge on saving; YABRIDGE_DEBUG_LEVEL=0 for the short log)
export YABRIDGE_DEBUG_LEVEL="${YABRIDGE_DEBUG_LEVEL:-1}"

exec "build/$config/OrchestralDAW_artefacts/$config/Daw+"
