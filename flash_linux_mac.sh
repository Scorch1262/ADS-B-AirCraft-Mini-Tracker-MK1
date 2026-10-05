#!/bin/sh
# Aufruf: ./flash_linux_mac.sh /dev/ttyACM0
set -e
[ -n "$1" ] || { echo "Aufruf: $0 /dev/ttyACM0"; exit 1; }
python3 -m esptool --chip esp32s3 --port "$1" --baud 921600 write-flash 0x0 "$(dirname "$0")/firmware/ADSB-Map-1.0.0_0x0.bin"
