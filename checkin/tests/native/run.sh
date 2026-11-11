#!/bin/sh
# Native tests for the Check-in wapp: the whole module against a mock HAL,
# with hal_db on zx, the host's database engine (sdk/hal_db_zx.c). Not part
# of the wasm build; run:
#   sh tests/native/run.sh          (DART=/path/to/dart if not on PATH)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SDK="$SRC/../sdk"
. "$SDK/zxdb_env.sh"
OUT="${TMPDIR:-/tmp}/checkin_native_test"
ZXDB_ROOT="${TMPDIR:-/tmp}/checkin_native_db"
rm -rf "$ZXDB_ROOT"
export ZXDB_ROOT
${CC:-cc} -O0 -g -I"$SRC" -I"$SRC/../alerts" -I"$SRC/../hal" -Wno-attributes -Wall -Wextra \
  "$HERE/test_checkin.c" "$HERE/hal_mock.c" "$SDK/hal_db_zx.c" \
  "$SRC/ci.c" "$SRC/../alerts/util.c" -o "$OUT"
exec "$OUT"
