#!/bin/sh
# Native integration test for the circles wapp: a mock HAL with hal_db on zx
# drives module_handle_event through the folder flow (open / add / enter / post).
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
SDK="$SRC/../sdk"
# hal_db on zx, the host's database engine (DART=/path/to/dart if not on PATH).
. "$SDK/zxdb_env.sh"
ZXDB_ROOT=/tmp/ct
export ZXDB_ROOT
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes \
  "$HERE/test_folders.c" "$HERE/hal_mock.c" \
  "$SRC/main.c" "$SRC/util.c" "$SRC/db.c" "$SRC/circle.c" \
  "$SDK/hal_db_zx.c" -o /tmp/circles_cttest
exec /tmp/circles_cttest
