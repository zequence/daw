#!/usr/bin/env bash
# Configures and builds on Linux with CMake + Ninja (GCC). Installs missing build
# prerequisites with apt (asks first; --yes skips the question).
# Usage: ./build.sh [Debug|Release] [--yes]   (default: Release)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

config=Release yes=0
for a in "$@"; do
  case "$a" in
    --yes) yes=1 ;;
    Debug|Release) config=$a ;;
    *) echo "Usage: $0 [Debug|Release] [--yes]" >&2; exit 1 ;;
  esac
done

# Tools (command -> package) and libraries JUCE needs (pkg-config name -> package)
declare -A tools=([cmake]=cmake [ninja]=ninja-build [g++]=g++ [git]=git [pkg-config]=pkg-config)
declare -A libs=(
  [alsa]=libasound2-dev [jack]=libjack-jackd2-dev [freetype2]=libfreetype-dev
  [fontconfig]=libfontconfig1-dev [x11]=libx11-dev [xext]=libxext-dev
  [xrandr]=libxrandr-dev [xinerama]=libxinerama-dev [xcursor]=libxcursor-dev
  [xcomposite]=libxcomposite-dev [gl]=libgl-dev
)
need=()
for c in "${!tools[@]}"; do command -v "$c" >/dev/null || need+=("${tools[$c]}"); done
for l in "${!libs[@]}"; do
  if ! command -v pkg-config >/dev/null || ! pkg-config --exists "$l"; then need+=("${libs[$l]}"); fi
done
if (( ${#need[@]} )); then
  echo "Missing build prerequisites: ${need[*]}"
  if (( ! yes )); then
    read -r -p "Install with sudo apt? [y/N] " a
    [[ "$a" == [yY]* ]] || { echo "Aborted." >&2; exit 1; }
  fi
  sudo apt-get update
  sudo apt-get install -y "${need[@]}"
fi

[[ -f external/JUCE/CMakeLists.txt ]] || git submodule update --init

# Local JUCE patches (applied once; skipped when already applied)
for p in patches/*.patch; do
  [[ -e "$p" ]] || continue
  if ! git -C external/JUCE apply --reverse --check "$PWD/$p" 2>/dev/null; then
    # An older version of the patch may be applied: restore the files it touches first
    if ! git -C external/JUCE apply --check "$PWD/$p" 2>/dev/null; then
      mapfile -t touched < <(git -C external/JUCE apply --numstat "$PWD/$p" | cut -f3)
      git -C external/JUCE checkout -- "${touched[@]}"
      echo "Restored JUCE files for re-patching: ${touched[*]}"
    fi
    git -C external/JUCE apply "$PWD/$p"
    echo "Applied JUCE patch $(basename "$p")"
  fi
done

cmake -S . -B "build/$config" -G Ninja -DCMAKE_BUILD_TYPE="$config"
cmake --build "build/$config"

echo
echo "Built: $PWD/build/$config/OrchestralDAW_artefacts/$config/Daw+"
