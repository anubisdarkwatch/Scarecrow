#!/usr/bin/env bash
# ============================================================================
#  build-all.sh  —  rebuild every Scarecrow firmware and refresh the flasher.
#  Compiles each device, merges a single flashable image (offset 0), and copies
#  it into docs/firmware/ so the GitHub Pages web flasher is never stale.
#
#  Run from the repo root:  ./scripts/build-all.sh
#  Needs: arduino-cli, the esp32 core, and the project's libraries installed.
#  Override tool locations with env vars if your setup differs:
#     LIBS=/path/to/Arduino/libraries   (extra library dir; RadioLib, M5*, etc.)
#     ESPTOOL=/path/to/esptool          BOOT_APP0=/path/to/boot_app0.bin
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."                       # repo root
ROOT="$(pwd)"
OUT="$ROOT/docs/firmware"
BUILD="$ROOT/.build"
mkdir -p "$OUT" "$BUILD"

LIBS="${LIBS:-/home/claude/Arduino/libraries}"
ESPTOOL="${ESPTOOL:-$(find "$HOME/.arduino15" -name 'esptool' -type f 2>/dev/null | head -1)}"
BOOT_APP0="${BOOT_APP0:-$(find "$HOME/.arduino15" -name boot_app0.bin 2>/dev/null | head -1)}"
[ -n "$ESPTOOL" ]  || { echo "esptool not found; set ESPTOOL=..."; exit 1; }
[ -n "$BOOT_APP0" ] || { echo "boot_app0.bin not found; set BOOT_APP0=..."; exit 1; }

# TFT_eSPI flag sets for the two CYD panels (shared cyd-hud.ino sketch)
CYD_COMMON="-DUSER_SETUP_LOADED=1 -DTFT_WIDTH=240 -DTFT_HEIGHT=320 -DTFT_MISO=12 -DTFT_MOSI=13 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1 -DTFT_BL=27 -DTFT_BACKLIGHT_ON=HIGH -DLOAD_GLCD=1 -DLOAD_FONT2=1 -DLOAD_FONT4=1 -DLOAD_FONT6=1 -DSPI_FREQUENCY=55000000"
FLAGS_CYD28="$CYD_COMMON -DILI9341_DRIVER=1 -DTFT_RGB_ORDER=TFT_BGR -DBOARD_CYD28"
FLAGS_CYD32="$CYD_COMMON -DST7789_DRIVER=1 -DTFT_INVERSION_ON=1 -DBOARD_CYD32"

# build <name> <sketch> <fqbn> <chip> <boot_off> [extra_flags]
build() {
  local name="$1" sketch="$2" fqbn="$3" chip="$4" boot_off="$5" flags="${6:-}"
  local bdir="$BUILD/$name"
  echo "==> $name  ($chip)"
  local args=(compile --fqbn "$fqbn" --libraries "$LIBS" --build-path "$bdir" "$sketch")
  [ -n "$flags" ] && args+=(--build-property "compiler.cpp.extra_flags=$flags")
  arduino-cli "${args[@]}" >/dev/null
  local stem; stem="$(basename "$sketch" .ino)"
  local fsize=4MB freq=40m
  [ "$chip" = esp32s3 ] && { fsize=8MB; freq=80m; }   # S3 nodes are 8MB flash
  "$ESPTOOL" --chip "$chip" merge-bin -o "$OUT/$name.bin" \
    --flash-mode dio --flash-freq "$freq" --flash-size "$fsize" \
    "$boot_off" "$bdir/$stem.ino.bootloader.bin" \
    0x8000 "$bdir/$stem.ino.partitions.bin" \
    0xe000 "$BOOT_APP0" \
    0x10000 "$bdir/$stem.ino.bin" >/dev/null
  printf "    -> docs/firmware/%s.bin (%s KB)\n" "$name" "$(( $(stat -c%s "$OUT/$name.bin") / 1024 ))"
}

ESP32="esp32:esp32:esp32:PartitionScheme=huge_app,FlashSize=4M"
ESP32D="esp32:esp32:esp32:PartitionScheme=default,FlashSize=4M"
ESP32S3="esp32:esp32:esp32s3:FlashSize=8M,PSRAM=disabled,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc"

build collector          firmware/scarecrow/scarecrow.ino              "$ESP32"   esp32   0x1000
build cyd-28             firmware/cyd-hud/cyd-hud.ino                   "$ESP32D"  esp32   0x1000 "$FLAGS_CYD28"
build cyd-32             firmware/cyd-hud/cyd-hud.ino                   "$ESP32D"  esp32   0x1000 "$FLAGS_CYD32"
build node-tembed        firmware/df-node/df-node.ino                  "$ESP32S3" esp32s3 0x0
build node-cardputer-adv firmware/df-node-cardputer/df-node-cardputer.ino "$ESP32S3" esp32s3 0x0

echo
echo "All firmware rebuilt into docs/firmware/. Commit docs/ to update the flasher."
