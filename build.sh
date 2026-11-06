#!/usr/bin/env bash
# Builds tee-ps3-benchmark.pkg (or another make target: ./build.sh clean, ./build.sh pkg AUTOTEST=1).
#
# GNU make cannot cope with spaces in paths such as "Application Support", so the sources are
# copied to a temporary folder without spaces, built there, and the results copied back.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
export PS3DEV="${PS3DEV:-$HOME/ps3dev}"
export PSL1GHT="${PSL1GHT:-$PS3DEV}"
# $PS3DEV first: an older toolchain (GCC 7.2) built a program that hung the console at start
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH"

if ! command -v ppu-gcc >/dev/null 2>&1; then
  echo "ppu-gcc not found - is the PS3 toolchain (PSL1GHT) installed in $PS3DEV?" >&2
  exit 1
fi

# a fixed neutral folder: build paths end up in the linker's notes, so no user folder name goes into them
STAGE=/tmp/ps3-benchmark-build
mkdir -p "$STAGE"

rsync -a --delete --exclude build/ --exclude '*.pkg' --exclude '*.self' --exclude '*.elf' \
  "$ROOT/" "$STAGE/"

make -C "$STAGE" "${@:-pkg}"

for f in "$STAGE"/*.pkg "$STAGE"/*.self; do
  [ -e "$f" ] || continue
  cp "$f" "$ROOT/"
  echo "-> $(basename "$f")"
done
