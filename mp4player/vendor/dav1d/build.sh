#!/usr/bin/env bash
# Rebuild lib/libdav1d.a (and include/dav1d/version.h) from dav1d source for
# wasm32-wasi. Run through the Makefile, which passes its own compiler:
#
#   make dav1d DAV1D_SRC=/path/to/dav1d      # a 1.4.3 checkout
#
# i.e.  build.sh <dav1d-src> <compiler command words...>
#
# This is the README's recipe as a script: meson cross build, no asm, no
# tools/tests/examples, and -pthread stripped (meson's threads dependency
# adds it, and it makes clang emit the wasm atomics that wasm_run rejects with
# "threads support is not enabled"). Needs meson and ninja; $WASM_AR names the
# archiver (llvm-ar).
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <dav1d-src> <cc> [cc flags...]" >&2
    exit 2
fi
src=$(cd "$1" && pwd); shift
here=$(cd "$(dirname "$0")" && pwd)
build=$here/build-wasi
ar=${WASM_AR:-llvm-ar}
command -v "$ar" >/dev/null || ar=$(dirname "$(command -v "$1")")/llvm-ar

rm -rf "$build"
mkdir -p "$build"

# The compiler, minus -pthread. Every word of the Makefile's $(CC) is baked
# in, so --sysroot and -resource-dir travel with it.
{
    echo '#!/usr/bin/env bash'
    printf 'cc=('; printf '%q ' "$@"; echo ')'
    echo 'args=(); for a in "$@"; do [ "$a" = -pthread ] || args+=("$a"); done'
    echo 'exec "${cc[@]}" "${args[@]}"'
} > "$build/cc"
chmod +x "$build/cc"

cat > "$build/cross.ini" <<EOF
[binaries]
c = '$build/cc'
ar = '$ar'

[built-in options]
c_args = ['--target=wasm32-wasi', '-O2', '-msimd128', '-D_GNU_SOURCE', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PTHREAD', '-ffunction-sections', '-fdata-sections']
c_link_args = ['--target=wasm32-wasi']
default_library = 'static'

[host_machine]
system = 'wasi'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'
EOF

meson setup "$build/out" "$src" --cross-file "$build/cross.ini" \
    -Denable_asm=false -Denable_tools=false -Denable_tests=false \
    -Denable_examples=false -Dlogging=false
ninja -C "$build/out"

cp "$build/out/src/libdav1d.a" "$here/lib/libdav1d.a"
cp "$src"/include/dav1d/*.h "$here/include/dav1d/"
cp "$build/out/include/dav1d/version.h" "$here/include/dav1d/"
echo "dav1d: $here/lib/libdav1d.a rebuilt from $src"
