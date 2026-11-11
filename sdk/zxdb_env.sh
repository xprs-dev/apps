# Sourced by a wapp's tests/native/run.sh: hal_db on zx (sdk/hal_db_zx.c).
# Needs SDK (the path of apps/sdk) and DART (the Dart SDK's dart; default
# dart on PATH). Exports ZXDB_BRIDGE; the caller sets ZXDB_ROOT and empties
# it, and links "$SDK/hal_db_zx.c".
DART="${DART:-dart}"
if [ ! -f "$SDK/zxdb_bridge/.dart_tool/package_config.json" ]; then
  (cd "$SDK/zxdb_bridge" && { "$DART" pub get --offline >/dev/null 2>&1 || "$DART" pub get >/dev/null; })
fi
ZXDB_BRIDGE="\"$DART\" \"$SDK/zxdb_bridge/bin/zxdb_bridge.dart\""
export ZXDB_BRIDGE
