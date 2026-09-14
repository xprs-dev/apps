# mp4player — wasm H.264 video player wapp.
#
# Unlike the plain-C wapps (sdk/Makefile.common), this one bundles a C++
# decoder (openh264) + an mp4 demuxer (minimp4) compiled to wasm, so it
# needs clang++ and a larger source set. The host stays codec-free; this
# wapp pushes decoded RGBA via the hal_video_* sink.
#
# Build:  cd wapps && WASI_SDK_PATH=$HOME/wasi-sdk make -C mp4player
#   (or)  cd apps/mp4player && WASI_SDK_PATH=$HOME/wasi-sdk make

include ../sdk/toolchain.mk
HAL  := ../hal
OH   := vendor/openh264/codec
PVAAC := vendor/pvaac
VPX  := vendor/vpx
NEG  := vendor/nestegg
DE265 := vendor/de265

OUTPUT := app.wasm

# -Ivendor/sjlj is FIRST so its setjmp.h shadows the wasi one for libvpx (which
# uses setjmp/longjmp for error recovery; wasi has no real setjmp). See sjlj/.
INCLUDES := \
	-Ivendor/sjlj \
	-I$(HAL) \
	-Ivendor \
	-I$(OH)/api \
	-I$(OH)/api/wels \
	-I$(OH)/common/inc \
	-I$(OH)/decoder/core/inc \
	-I$(OH)/decoder/plus/inc \
	-Ivendor/opuslib/include \
	-Ivendor/opuslib/celt \
	-Ivendor/opuslib/silk \
	-Ivendor/opuslib/silk/float \
	-Ivendor/opuslib/ogg/include \
	-I$(VPX) \
	-I$(NEG)/include \
	-I$(DE265)

# -msimd128: wasm SIMD via LLVM autovectorization only (the vendored codec
# trees carry no SIMD sources) — measured ~1.2-2x, not native-asm speed. The
# REAL fast paths are the native binaries in bin/ and Android's MediaCodec;
# this narrows the gap for the wasm fallback.
CXXFLAGS := \
	--target=wasm32-wasi \
	-O2 \
	-msimd128 \
	-fno-exceptions -fno-rtti \
	-ffunction-sections -fdata-sections \
	-DNDEBUG -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PTHREAD \
	-DHAVE_POSIX_MEMALIGN \
	$(INCLUDES)

# C flags for the vendored decoder libraries (dr_libs, stb_vorbis): plain C,
# no stdio (we decode from memory). No -fno-rtti/-fno-exceptions (C-only).
CFLAGS := \
	--target=wasm32-wasi \
	-O2 \
	-ffunction-sections -fdata-sections \
	-DNDEBUG -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PTHREAD \
	-DSTB_VORBIS_NO_STDIO \
	-DOPUS_BUILD -DVAR_ARRAYS \
	$(INCLUDES)

LDFLAGS := \
	--target=wasm32-wasi \
	-nostartfiles \
	-lwasi-emulated-signal \
	-lwasi-emulated-pthread \
	-Wl,--no-entry \
	-Wl,--gc-sections \
	-Wl,-z,stack-size=5242880 \
	-Wl,--initial-memory=134217728 \
	-Wl,--max-memory=134217728 \
	-Wl,--export=module_init \
	-Wl,--export=module_tick \
	-Wl,--export=module_handle_event \
	-Wl,--export=module_destroy \
	-Wl,--export=module_tick_interval_ms \
	-Wl,--strip-all

# C++ sources: main + openh264 (video) + PacketVideo AAC (audio) + aac wrapper +
# webm backend (drives libvpx + libnestegg + libopus) + hevc backend
# (libde265; .cc sources listed separately below).
CXX_SRCS := \
	main.cpp \
	vendor/aac_dec.cpp \
	vendor/webm_dec.cpp \
	vendor/hevc_dec.cpp \
	vendor/av1_dec.cpp \
	$(wildcard $(OH)/common/src/*.cpp) \
	$(wildcard $(OH)/decoder/core/src/*.cpp) \
	$(wildcard $(OH)/decoder/plus/src/*.cpp) \
	$(wildcard $(PVAAC)/*.cpp)

# C sources: vendored audio decoders + stubs + libvpx (VP8/VP9) + nestegg +
# setjmp shim (compiled as C).
C_SRCS := \
	vendor/audio_pcm.c \
	vendor/stb_vorbis.c \
	vendor/wasi_thread_stubs.c \
	vendor/opus_dec.c \
	vendor/opuslib/ogg/framing.c \
	vendor/opuslib/ogg/bitwise.c \
	$(wildcard vendor/opuslib/celt/*.c) \
	$(wildcard vendor/opuslib/silk/*.c) \
	$(wildcard vendor/opuslib/silk/float/*.c) \
	$(wildcard vendor/opuslib/src/*.c) \
	vendor/sjlj/sjlj_shim.c \
	$(NEG)/src/nestegg.c \
	$(VPX)/vpx_config.c \
	$(wildcard $(VPX)/vpx/src/*.c) \
	$(wildcard $(VPX)/vpx_mem/*.c) \
	$(wildcard $(VPX)/vpx_scale/*.c) \
	$(wildcard $(VPX)/vpx_scale/generic/*.c) \
	$(wildcard $(VPX)/vpx_dsp/*.c) \
	$(wildcard $(VPX)/vpx_util/*.c) \
	$(wildcard $(VPX)/vp8/*.c) \
	$(wildcard $(VPX)/vp8/common/*.c) \
	$(wildcard $(VPX)/vp8/common/generic/*.c) \
	$(wildcard $(VPX)/vp8/decoder/*.c) \
	$(wildcard $(VPX)/vp9/*.c) \
	$(wildcard $(VPX)/vp9/common/*.c) \
	$(wildcard $(VPX)/vp9/decoder/*.c)

# libde265 (HEVC) — upstream uses .cc; runs single-threaded via its wasi_st.h
# shim (see vendor/de265/libde265/wasi_st.h).
CC_SRCS := $(wildcard $(DE265)/libde265/*.cc)

OBJS := $(CXX_SRCS:.cpp=.o) $(CC_SRCS:.cc=.o) $(C_SRCS:.c=.o)

.PHONY: all clean dav1d
all: $(OUTPUT)

dav1d:
	@test -n "$(DAV1D_SRC)" || { echo "set DAV1D_SRC to a dav1d 1.4.3 checkout" >&2; exit 1; }
	WASM_AR="$(WASM_AR)" vendor/dav1d/build.sh "$(DAV1D_SRC)" $(CC)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The PacketVideo AAC decoder, with the flags AOSP built it with. Without
# -fno-strict-aliasing its output is garbage at -O2: see
# vendor/pvaac/README.xprs.md. aac_dec.cpp shares the decoder's struct layout,
# so it gets the same defines.
PVAAC_FLAGS := -I$(PVAAC) -DAAC_PLUS -DHQ_SBR -DPARAMETRICSTEREO \
	-DOSCL_IMPORT_REF= -DOSCL_EXPORT_REF= -DOSCL_UNUSED_ARG= \
	-fno-strict-aliasing -Wno-c++11-narrowing
$(PVAAC)/%.o: $(PVAAC)/%.cpp
	$(CXX) $(CXXFLAGS) $(PVAAC_FLAGS) -c $< -o $@
vendor/aac_dec.o: vendor/aac_dec.cpp
	$(CXX) $(CXXFLAGS) $(PVAAC_FLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# dav1d (AV1) links as a wasm32-wasi static library. The committed
# vendor/dav1d/lib/libdav1d.a is a prebuilt copy, because dav1d's build is
# meson-only. `make dav1d DAV1D_SRC=<dav1d 1.4.3 checkout>` rebuilds it from
# source with this same toolchain (needs meson and ninja). A from-source build,
# F-Droid's, runs that before `make`. See vendor/dav1d/README.md.
DAV1D_LIB := vendor/dav1d/lib/libdav1d.a

$(OUTPUT): $(OBJS) $(DAV1D_LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(DAV1D_LIB)
	@echo "Built $(OUTPUT) ($$(wc -c < $(OUTPUT)) bytes)"

clean:
	rm -f $(OBJS) $(OUTPUT)
