#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
export SONOS_HEADLESS_BUILD=1

idf.py -B build-sonos \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG="$PWD/build-sonos/sdkconfig" \
  '-DSDKCONFIG_DEFAULTS=config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3;config/sdkconfig.defaults.sonos' \
  build

(
  cd build-sonos
  esptool --chip esp32s3 merge-bin --output airplay-sonos-esp32s3-n16r8.bin @flash_args
)
