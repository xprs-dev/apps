# Functionalities is a UI-only wapp: the host draws its screen, because the
# functionality registry is the core's. Its module has always been the
# Tester's, byte for byte, committed here as a bare app.wasm with no source
# beside it. This builds that same module from tester/main.c, so the wapp is
# reproducible from source like every other.
MODULE_NAME := app
MODULE_SRCS := ../tester/main.c
include ../sdk/Makefile.common
