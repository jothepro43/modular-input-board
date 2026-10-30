#!/bin/zsh
# Build and upload a sketch from firmware/, then restart the board with a watchdog reset.
# The plain reset after upload leaves this board stuck in its bootloader.
# Needs: arduino-cli, and a venv with pyserial and esptool (pip install pyserial esptool).
# Usage: tools/upload.sh modular_remote /path/to/venv
set -e
sketch=${1:-modular_remote}
venv=${2:?give the path to the venv}
cd "${0:A:h}/../firmware"
fqbn=esp32:esp32:adafruit_qtpy_esp32s3_nopsram
arduino-cli compile -b $fqbn "$sketch"
arduino-cli upload -b $fqbn -p "$(ls /dev/cu.usbmodem* | head -1)" "$sketch"
sleep 3
"$venv/bin/esptool" --chip esp32s3 -p "$(ls /dev/cu.usbmodem* | head -1)" --before no-reset --after watchdog-reset chip-id
