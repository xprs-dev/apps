#!/bin/sh
# Native tests for the Devices wapp: the real shell (main.c) with each half
# in turn against a mock HAL, the other half stubbed out.
# Not part of the wasm build; run:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
OUT="${TMPDIR:-/tmp}"
F="-O0 -g -I$SRC -I$SRC/../hal -Wno-attributes -Wall -Wextra"
cc $F "$HERE/test_stations.c" "$HERE/hal_mock_stations.c" "$HERE/stub_things.c" \
  "$SRC/main.c" "$SRC/wire.c" -o "$OUT/devices_stations_test"
cc $F "$HERE/test_things.c" "$HERE/hal_mock_things.c" "$HERE/stub_stations.c" \
  "$SRC/main.c" "$SRC/wire.c" "$SRC/rtsp.c" "$SRC/md5.c" -o "$OUT/devices_things_test"
"$OUT/devices_stations_test"
"$OUT/devices_things_test"
