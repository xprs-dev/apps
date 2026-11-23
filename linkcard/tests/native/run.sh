#!/bin/sh
# Native tests for the link-card reader: sdk/wapp_page_meta.h against the
# fixture pages in tests/fixtures. Not part of the wasm build; run:
#   sh tests/native/run.sh          (UPDATE=1 rewrites the .expect files)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/linkcard_native_test"
${CC:-cc} -O0 -g -I"$HERE/../../../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_meta.c" -o "$OUT"
exec "$OUT" "$HERE/../fixtures"
