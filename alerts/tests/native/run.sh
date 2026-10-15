#!/bin/sh
# Native tests for the Alerts wapp: the whole module against a mock HAL.
# Not part of the wasm build; run:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_alerts.c" "$HERE/hal_mock.c" "$SRC/util.c" "$SRC/alert.c" \
  -o /tmp/alerts_native_test
exec /tmp/alerts_native_test
