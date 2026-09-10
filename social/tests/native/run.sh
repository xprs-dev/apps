#!/bin/sh
# Native integration test for the Social wapp: a mock HAL drives module_init /
# module_handle_event and asserts what the wapp asked the core to publish and
# what it drew. architecture.md section 6: a wapp touches nothing but the HAL,
# so a wapp feature is testable without a device — and posting had no test at
# all, which is how the host came to publish on the wapp's behalf.
# Not part of the wasm build — run manually:  sh tests/native/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../.."
cc -O0 -g -I"$SRC" -I"$SRC/../hal" -Wno-attributes \
  "$HERE/test_social.c" "$HERE/hal_mock.c" "$SRC/main.c" \
  -o /tmp/social_cttest
exec /tmp/social_cttest
