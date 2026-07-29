#!/usr/bin/env bash
# Repeatable terminal build for GP2040-CE on this machine's Pico VS Code toolchain.
# Mirrors the environment declared in .vscode/settings.json (SDK 2.3.0 / toolchain
# 15_2_Rel1 / picotool 2.3.0) so CLI builds match the IDE. Local helper; not committed.
#
# Usage:  ./build-usbpassthrough.sh [BOARD] [BUILD_DIR]
#   BOARD      default: RP2040AdvancedBreakoutBoardUSBPassthrough
#   BUILD_DIR  default: build
set -euo pipefail

PS="$HOME/.pico-sdk"
ROOT_EARLY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Read the versions straight from the CMakeLists pico-vscode block (kept in sync by
# the VS Code Pico extension), so this can't drift from the configured SDK again.
sdkver() { grep -m1 "set($1 " "$ROOT_EARLY/CMakeLists.txt" | sed -E "s/.*set\($1 ([^)]*)\).*/\1/"; }
SDK_VER="$(sdkver sdkVersion)"
TC_VER="$(sdkver toolchainVersion)"
PICOTOOL_VER="$(sdkver picotoolVersion)"
CMAKE_VER=v4.3.4
NINJA_VER=v1.13.2

# Fail early with a clear message if the pinned SDK isn't installed yet.
if [ ! -d "$PS/sdk/$SDK_VER" ]; then
  echo "ERROR: pico-sdk $SDK_VER is not installed at $PS/sdk/$SDK_VER" >&2
  echo "Install it via the VS Code Pico extension (Switch SDK -> $SDK_VER), then re-run." >&2
  echo "Installed SDKs: $(ls "$PS/sdk" 2>/dev/null | tr '\n' ' ')" >&2
  exit 1
fi
# nanopb's proto step builds a Python venv, so it needs a FULL CPython (venv+pip).
# The pico-sdk bundled python is an embeddable build with no venv module; use the
# uv-managed CPython 3.13 instead (matches how this project was built before).
PY_EXE="$HOME/.local/bin/python3.13.exe"

BOARD="${1:-RP2040AdvancedBreakoutBoardUSBPassthrough}"
BUILD_DIR="${2:-build}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export PATH="$PS/toolchain/$TC_VER/bin:$PS/picotool/$PICOTOOL_VER/picotool:$PS/cmake/$CMAKE_VER/bin:$PS/ninja/$NINJA_VER:$PATH"
export PICO_SDK_PATH="$PS/sdk/$SDK_VER"
export GP2040_BOARDCONFIG="$BOARD"
export SKIP_WEBBUILD=TRUE

# Reuse an already-fetched ArduinoJson if present, to avoid a network round-trip.
AJ_ARG=()
if [ -d "$ROOT/build/_deps/arduinojson-src" ]; then
  AJ_ARG=(-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON="$ROOT/build/_deps/arduinojson-src")
elif [ -d "$ROOT/build-uwd/_deps/arduinojson-src" ]; then
  AJ_ARG=(-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON="$ROOT/build-uwd/_deps/arduinojson-src")
fi

echo ">> Board=$BOARD  SDK=$SDK_VER  Toolchain=$TC_VER  Picotool=$PICOTOOL_VER  Dir=$BUILD_DIR"
cmake -S "$ROOT" -B "$ROOT/$BUILD_DIR" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PS/ninja/$NINJA_VER/ninja.exe" \
  -DPython3_EXECUTABLE="$PY_EXE" \
  -DCMAKE_BUILD_TYPE=Release \
  "${AJ_ARG[@]}"

# Pre-seed the nanopb proto venv WITH a compatible setuptools. GP2040 pins an old
# nanopb whose generator imports pkg_resources, which setuptools >=81 removed; pinning
# setuptools<81 keeps pkg_resources available. Creating environment.txt here satisfies
# compile_proto.cmake's venv target so it doesn't recreate the venv with a bad setuptools.
VENV="$ROOT/$BUILD_DIR/venv"
if [ ! -f "$VENV/environment.txt" ]; then
  echo ">> Seeding proto venv (setuptools<81 for nanopb's pkg_resources)"
  "$PY_EXE" -m venv "$VENV"
  "$VENV/Scripts/pip" --disable-pip-version-check -q install -r "$ROOT/lib/nanopb/extra/requirements.txt" "setuptools<81"
  "$VENV/Scripts/pip" freeze > "$VENV/environment.txt"
fi

cmake --build "$ROOT/$BUILD_DIR" --parallel

echo ">> Artifacts:"
ls -1 "$ROOT/$BUILD_DIR"/GP2040-CE_*."uf2" 2>/dev/null || echo "   (no .uf2 produced)"
