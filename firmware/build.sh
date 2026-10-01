#!/usr/bin/env bash
# Firmware build. Uses the official Arm toolchain downloaded into ~/pico/toolchain
# (the Homebrew one ships without newlib, so nosys.specs is missing and the link fails).
set -euo pipefail

export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico/pico-sdk}"

# The official Arm tarball extracts into a subdirectory with a versioned name.
# Depending on how it was unpacked, the binaries live in <root>/bin or in
# <root>/arm-gnu-toolchain-*/bin: look in both places instead of assuming one.
if [[ -z "${PICO_TOOLCHAIN_PATH:-}" ]]; then
    for candidate in "$HOME/pico/toolchain" "$HOME"/pico/toolchain/arm-gnu-toolchain-*; do
        if [[ -x "$candidate/bin/arm-none-eabi-gcc" ]]; then
            PICO_TOOLCHAIN_PATH="$candidate"
            break
        fi
    done
fi

# Without this check CMake silently falls back to the Homebrew toolchain and the
# error shows up much later, as 'cannot read spec file nosys.specs' while linking
# the boot stage2. Far better to fail right here and say why.
if [[ -z "${PICO_TOOLCHAIN_PATH:-}" || ! -x "${PICO_TOOLCHAIN_PATH}/bin/arm-none-eabi-gcc" ]]; then
    echo "ERROR: Arm toolchain not found." >&2
    echo "Expected in ~/pico/toolchain/bin/ or ~/pico/toolchain/arm-gnu-toolchain-*/bin/" >&2
    echo "Do not use the Homebrew arm-none-eabi-gcc formula: it has no newlib." >&2
    echo "Download the official Arm tarball and extract it into ~/pico/toolchain/," >&2
    echo "or export PICO_TOOLCHAIN_PATH by hand." >&2
    exit 1
fi
export PICO_TOOLCHAIN_PATH

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# PANEL=6HP ./build.sh for the 3U/6HP 128x64 build; default is 1U/20HP
# 128x32. ENC=REV ./build.sh for a module whose encoder turns the wrong way
# (see pins.h: the EC11's outer pair is not standardised). Every combination
# gets its own build directory, so all four UF2s can exist at once instead of
# reconfiguring back and forth - and so a stale cache cannot hand you a UF2
# built for the other panel or the other direction.
PANEL="${PANEL:-1U}"
ENC="${ENC:-STD}"

case "$PANEL" in
    1U)  BUILD_DIR="$HERE/build" ;;
    6HP) BUILD_DIR="$HERE/build-6hp" ;;
    *)   echo "ERROR: PANEL must be 1U or 6HP, got '$PANEL'" >&2; exit 1 ;;
esac

case "$ENC" in
    STD) ;;
    REV) BUILD_DIR="${BUILD_DIR}-rev" ;;
    *)   echo "ERROR: ENC must be STD or REV, got '$ENC'" >&2; exit 1 ;;
esac

# Build identifier for the ABOUT page: the git short hash, +"-dirty" if the
# working tree has uncommitted changes. Computed here rather than in
# CMakeLists.txt because build.sh always reconfigures below, so there is no
# staleness risk to guard against with execute_process() at configure time.
GIT_HASH="$(git -C "$HERE" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
if ! git -C "$HERE" diff --quiet 2>/dev/null || ! git -C "$HERE" diff --cached --quiet 2>/dev/null; then
    GIT_HASH="${GIT_HASH}-dirty"
fi

echo "Toolchain: $PICO_TOOLCHAIN_PATH"
echo "Panel: $PANEL"
echo "Encoder: $ENC"
echo "Build: $GIT_HASH"
cmake -S "$HERE" -B "$BUILD_DIR" -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DMURDRUM_PANEL="$PANEL" -DMURDRUM_ENC="$ENC" -DFIRMWARE_BUILD="$GIT_HASH"
cmake --build "$BUILD_DIR" "$@"

echo
echo "UF2: $BUILD_DIR/sampleplayer.uf2"
echo "Flash: hold BOOTSEL, plug the Pico 2 in, then:"
echo "  picotool load -x '$BUILD_DIR/sampleplayer.uf2'"
