# XPRS WASM toolchain: which clang builds a wapp.
#
# Included by Makefile.common and by any wapp with its own Makefile
# (mp4player). Sets CC, CXX and WASM_AR.
#
# Two toolchains build the same modules:
#
#   wasi-sdk (default)
#       WASI_SDK_PATH=~/wasi-sdk, where install-wasi-sdk.sh puts it.
#
#   distro packages
#       WASI_SYSROOT=/usr with Debian's or Ubuntu's clang, lld, wasi-libc and
#       libclang-rt-dev-wasm32, plus libc++-dev-wasm32 and
#       libc++abi-dev-wasm32 for the C++ wapps. Nothing is downloaded, which is
#       how F-Droid rebuilds the wapps bundled in the app (app/docs/fdroid.md).
#       WASM_CLANG and WASM_CLANGXX name the compilers (default clang and
#       clang++; clang-18 and clang++-18 are fine). WASM_TOOLCHAIN_FLAGS
#       passes anything else to both, e.g. -resource-dir when compiler-rt's
#       wasm32 builtins live outside clang's own resource directory.
#
# Each toolchain reproduces its own output byte for byte. The two differ from
# each other only because they are different clang versions.
#
# When WASI_SDK_PATH is not set, look where install-wasi-sdk.sh actually puts
# the SDK (~/wasi-sdk) before falling back to the system-wide /opt. Defaulting
# to /opt alone meant `make` from inside a wapp directory died with
#
#   make: /opt/wasi-sdk/bin/clang: No such file or directory
#
# on a machine where the SDK was installed and working. It reads like a
# missing toolchain rather than a missing variable, which is a long way to
# send somebody for a default.

ifdef WASI_SYSROOT
WASM_CLANG   ?= clang
WASM_CLANGXX ?= clang++
WASM_AR      ?= llvm-ar
CC  := $(WASM_CLANG) --sysroot=$(WASI_SYSROOT) $(WASM_TOOLCHAIN_FLAGS)
CXX := $(WASM_CLANGXX) --sysroot=$(WASI_SYSROOT) $(WASM_TOOLCHAIN_FLAGS)
else
WASI_SDK_PATH ?= $(firstword $(wildcard $(HOME)/wasi-sdk /opt/wasi-sdk) /opt/wasi-sdk)
CC      := $(WASI_SDK_PATH)/bin/clang
CXX     := $(WASI_SDK_PATH)/bin/clang++
WASM_AR := $(WASI_SDK_PATH)/bin/llvm-ar
endif
