#!/bin/bash

# Build PyroWave in the same oldest-supported userspace as Moonlight, then use
# Moonlight's normal AppImage pipeline. Run this from the Ubuntu 22.04 build
# environment documented in .github/workflows/build-appimage.yml.

set -euo pipefail

fail()
{
  echo "$1" >&2
  exit 1
}

SOURCE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYROWAVE_SOURCE=${PYROWAVE_SOURCE:-"$SOURCE_ROOT/deps/PyroWave"}
PYROWAVE_SOURCE=$(readlink -f "$PYROWAVE_SOURCE")
PYROWAVE_BUILD=${PYROWAVE_BUILD:-"$SOURCE_ROOT/build/pyrowave-appimage"}
PYROWAVE_PREFIX=${PYROWAVE_PREFIX:-"$SOURCE_ROOT/build/pyrowave-appimage-prefix"}

[ -f "$PYROWAVE_SOURCE/CMakeLists.txt" ] || fail "Set PYROWAVE_SOURCE to a copy of Pyroshine's vendor/pyrowave"

# Idempotent: pins Granite and re-applies the vendored Granite patches.
(cd "$PYROWAVE_SOURCE" && ./checkout_granite.sh)
[ "$(git -C "$PYROWAVE_SOURCE/Granite" rev-parse HEAD 2>/dev/null)" = "53090429539bf3404250d18fe58db971f0907c04" ] || \
  fail "PyroWave's pinned Granite checkout is missing or incorrect"

cmake -S "$PYROWAVE_SOURCE" -B "$PYROWAVE_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PYROWAVE_PREFIX" \
  -DPYROWAVE_DEVEL=OFF \
  -DPYROWAVE_UTILS=OFF
cmake --build "$PYROWAVE_BUILD" -j"$(nproc)"
cmake --install "$PYROWAVE_BUILD"
mkdir -p "$PYROWAVE_PREFIX/share/licenses/pyrowave"
cp "$PYROWAVE_SOURCE/LICENSE" "$PYROWAVE_PREFIX/share/licenses/pyrowave/LICENSE"

export PYROWAVE_PREFIX
cd "$SOURCE_ROOT"
exec "$SOURCE_ROOT/scripts/build-appimage.sh"
