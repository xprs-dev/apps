#!/bin/sh
# Native integration test for the mesh wapp: a canned mock HAL drives
# module_tick / module_handle_event and asserts the emitted messages.
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SDK="$SRC/../sdk"
# The stations map keeps its table in zx, the host's database: hal_db runs
# on the real engine through sdk/zxdb_bridge (DART=/path/to/dart if not on
# PATH).
. "$SDK/zxdb_env.sh"
ZXDB_ROOT="${TMPDIR:-/tmp}/mesh_native_db"
rm -rf "$ZXDB_ROOT"
export ZXDB_ROOT
${CC:-cc} -O0 -g -I"$SRC" -I"$SRC/../hal" -I"$SRC/../alerts" -I"$SDK" -Wno-attributes \
  "$HERE/test_mesh.c" "$HERE/hal_mock.c" "$SDK/hal_db_zx.c" "$SRC/main.c" "$SRC/browse.c" \
  "$SDK/wapp_stations.c" "$SRC/../alerts/util.c" \
  -o "${TMPDIR:-/tmp}/mesh_cttest"
exec "${TMPDIR:-/tmp}/mesh_cttest"
