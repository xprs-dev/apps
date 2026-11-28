#!/bin/sh
# Native test for the Bots wapp: the real main.c against a stand-in host.
# Not part of the wasm build; run:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
OUT="${TMPDIR:-/tmp}"
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_bots.c" "$SRC/main.c" -o "$OUT/bots_test"
"$OUT/bots_test"
