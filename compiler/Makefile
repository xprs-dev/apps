MODULE_NAME := app
MODULE_SRCS := main.c
include ../sdk/Makefile.common

# The toolchain the host runs for other wapps' builds (media/cc.wasm and
# media/usr/, see fetch-toolchain.sh). Fetched, pinned and verified; never
# committed.
all: media/.ready
media/.ready: fetch-toolchain.sh xcc.patch ../hal/xprs_wasm_hal.h ../sdk/wapp_test.c ../sdk/wapp_test.h
	sh fetch-toolchain.sh
