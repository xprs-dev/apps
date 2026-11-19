#!/bin/sh
# Assemble the Compiler add-on's toolchain into media/:
#
#   media/cc.wasm     xcc's C compiler and linker, itself compiled to WASI;
#                     the host runs it for other wapps' builds
#   media/usr/        its C headers (usr/include) and C library (usr/lib),
#                     plus the XPRS headers in usr/xprs/ and the test runner
#                     in usr/xprs/sdk/
#
# xcc (https://github.com/tyfkda/xcc, MIT) is fetched at a pinned commit and
# patched with xcc.patch:
#   - an import with __attribute__((import_name)) links by its C name, so
#     hal_log (imported as "log") is no longer bound to the C library's log()
#   - __attribute__((used)) keeps a static variable, so test-case tables and
#     the functions they reach are compiled
# It is built twice: natively (the host's cc), then by that build into WASI.
# Needs git, a C compiler and llvm-ar.
set -e
cd "$(dirname "$0")"

REPO=https://github.com/tyfkda/xcc
COMMIT=659a1773f20499864a8c3fdc7cd25cb506092bc1

AR=$(command -v llvm-ar || ls /usr/bin/llvm-ar-* 2>/dev/null | sort -V | tail -1)
[ -n "$AR" ] || { echo "[compiler] llvm-ar not found" >&2; exit 1; }

CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/xprs-compiler"
SRC="$CACHE/xcc-$COMMIT"
if [ ! -d "$SRC/.git" ]; then
    echo "[compiler] fetching xcc $COMMIT..."
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" fetch -q --depth 1 "$REPO" "$COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
# The commit id is the content's hash: anything else is refused.
[ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || {
    echo "[compiler] xcc is not at $COMMIT" >&2; exit 1; }
git -C "$SRC" checkout -q -- . && git -C "$SRC" clean -qfdx
git -C "$SRC" apply "$PWD/xcc.patch"

# xcc's Makefile calls "llvm-ar" by that name; distros may only have llvm-ar-N.
BIN=$(mktemp -d)
trap 'rm -rf "$BIN"' EXIT
ln -s "$AR" "$BIN/llvm-ar"
PATH="$BIN:$PATH" make -s -C "$SRC" -j2 wcc >/dev/null
make -s -C "$SRC" HOST_TARGET=wcc HOST_WCC=./wcc CC=./wcc WCC_TARGET= \
    wcc-self-hosting >/dev/null
# Relink it with its command entry exported as xprs_run instead of _start:
# the app's runtime runs a module's _start while instantiating it, and hands
# over captured output only some time after a call. xprs_run writes the
# compiler's messages to /tmp/cc.log, which the host reads when it returns.
cat > "$SRC/xprs_run.c" <<'END'
#include <stdio.h>
#include <stdlib.h>
extern void __wasm_call_ctors(void);
extern int __main_void(void);
/* What _start does, with the log opened once the C library (its view of
 * the preopened folders included) is set up. */
void xprs_run(void) {
    __wasm_call_ctors();
    FILE *log = fopen("/tmp/cc.log", "w");
    if (log) {
        stdout = log;
        stderr = log;
    }
    exit(__main_void());
}
END
(cd "$SRC" && ./wcc -c -o obj/xprs_run.o xprs_run.c &&
    ./wcc --entry-point= -e xprs_run -o cc.wasm obj/gen2wcc/*.o obj/xprs_run.o)

rm -rf media
mkdir -p media/usr/lib media/usr/xprs/sdk
cp "$SRC/cc.wasm" media/cc.wasm
cp -r "$SRC/include" media/usr/include
cp "$SRC/lib/wcrt0.a" "$SRC/lib/wlibc.a" media/usr/lib/
cp ../hal/*.h media/usr/xprs/
cp ../sdk/wapp_test.c ../sdk/wapp_test.h ../sdk/wapp_i18n.h media/usr/xprs/sdk/

mkdir -p licenses
rm -f licenses/LLVM.txt
cp "$SRC/LICENSE" licenses/xcc.txt

touch media/.ready
echo "[compiler] toolchain ready: $(du -sh media | cut -f1)"
