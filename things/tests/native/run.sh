#!/bin/sh
# Native tests for the Things wapp: the whole module against a mock HAL.
# Not part of the wasm build; run:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_things.c" "$HERE/hal_mock.c" "$SRC/wire.c" \
  -o /tmp/things_native_test
exec /tmp/things_native_test
