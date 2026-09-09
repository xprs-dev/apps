#!/bin/sh
# Native integration test for the archiver wapp: a canned mock HAL drives
# module_handle_event and asserts the emitted messages. The wapp had no tests
# at all until the packet tiers arrived, which is how "Enable" came to mean
# files while the station's archiver role lived somewhere else entirely.
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes \
  "$HERE/test_archiver.c" "$HERE/hal_mock.c" "$SRC/main.c" \
  -o /tmp/archiver_cttest
exec /tmp/archiver_cttest
