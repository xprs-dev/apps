#!/bin/sh
# Native tests for the chat wapp: a mock HAL with hal_db on zx drives the
# whole module (module_init / module_handle_event) and room.c directly.
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SDK="$SRC/../sdk"
# hal_db on zx, the host's database engine (DART=/path/to/dart if not on PATH).
. "$SDK/zxdb_env.sh"
ZXDB_ROOT=/tmp/chat_native_test
export ZXDB_ROOT
rm -rf /tmp/chat_native_test
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes -Wall \
  "$HERE/test_room.c" "$HERE/hal_mock.c" \
  "$SRC/main.c" "$SRC/room.c" "$SRC/db.c" "$SRC/thread.c" "$SRC/xprs.c" \
  "$SDK/hal_db_zx.c" -o /tmp/chat_native_test_bin
exec /tmp/chat_native_test_bin
