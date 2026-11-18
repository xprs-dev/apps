#!/bin/sh
# Assemble the Compiler add-on's toolchain into media/:
#
#   media/llvm.wasm   clang + wasm-ld compiled to WASI (YoWASP), run by the
#                     host for other wapps' builds
#   media/usr/        its C headers and libraries (wasi-libc, compiler-rt),
#                     plus the XPRS headers in usr/xprs/ and the test runner
#                     in usr/xprs/sdk/
#
# YoWASP's npm package is pinned by version and checked against the npm
# registry's sha512 before anything is unpacked. C++ (headers, libc++) is
# left out: wapps are C.
set -e
cd "$(dirname "$0")"

VERSION=22.0.0-git20695-13
URL="https://registry.npmjs.org/@yowasp/clang/-/clang-$VERSION.tgz"
SHA512=nNIUdoQKrttdSEvO7gpkgczB1fhRtiWEqxYNJHloD9EBf0HBu1bvcYnkTYTmUacXDItRGTFWnogWKnfSe+04Cg==

CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/xprs-compiler"
TGZ="$CACHE/clang-$VERSION.tgz"
mkdir -p "$CACHE"
if [ ! -f "$TGZ" ]; then
    echo "[compiler] downloading YoWASP clang $VERSION..."
    curl -fsSL -o "$TGZ.part" "$URL"
    mv "$TGZ.part" "$TGZ"
fi
got=$(openssl dgst -sha512 -binary "$TGZ" | base64 | tr -d '\n')
if [ "$got" != "$SHA512" ]; then
    echo "[compiler] checksum mismatch for $TGZ" >&2
    rm -f "$TGZ"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
tar xzf "$TGZ" -C "$WORK" package/gen/llvm.core.wasm package/gen/llvm-resources.tar

rm -rf media
mkdir -p media/usr
mv "$WORK/package/gen/llvm.core.wasm" media/llvm.wasm
tar xf "$WORK/package/gen/llvm-resources.tar" -C media/usr
# C only.
rm -rf media/usr/include/c++ media/usr/share/libc++
find media/usr/lib -name 'libc++*' -o -name 'libunwind*' | xargs rm -f

mkdir -p media/usr/xprs/sdk
cp ../hal/*.h media/usr/xprs/
cp ../sdk/wapp_test.c ../sdk/wapp_test.h media/usr/xprs/sdk/

mkdir -p licenses
if [ ! -f licenses/LLVM.txt ]; then
    curl -fsSL -o licenses/LLVM.txt \
        https://raw.githubusercontent.com/llvm/llvm-project/main/LICENSE.TXT
fi

touch media/.ready
echo "[compiler] toolchain ready: $(du -sh media | cut -f1)"
