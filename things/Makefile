# things — the devices around you, and a live view of the ones with a camera.
#
# Plain C, except for the decoder: watching a doorbell means H.264, and the
# host holds no codec (docs/architecture.md: the wapp decodes, the host
# uploads pixels). openh264's decoder is vendored once in the tree, by the
# player wapp, and compiled here too rather than copied — one decoder, two
# wapps, no second copy to update when it moves.
#
# Build:  cd apps/things && WASI_SDK_PATH=$HOME/wasi-sdk make
#   tests: sh tests/native/run.sh   (plain C, no decoder, no wasm)

include ../sdk/toolchain.mk
HAL := ../hal
OH  := ../mp4player/vendor/openh264/codec

OUTPUT := app.wasm

INCLUDES := \
	-I$(HAL) \
	-I. \
	-I$(OH)/api \
	-I$(OH)/api/wels \
	-I$(OH)/common/inc \
	-I$(OH)/decoder/core/inc \
	-I$(OH)/decoder/plus/inc

COMMON := \
	--target=wasm32-wasi \
	-O2 \
	-ffunction-sections -fdata-sections \
	-DNDEBUG -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PTHREAD \
	-DHAVE_POSIX_MEMALIGN \
	$(INCLUDES)

CFLAGS   := $(COMMON) -Wall -Wextra -Werror -Wno-attributes
CXXFLAGS := $(COMMON) -fno-exceptions -fno-rtti

# 32 MB is the decoder's working set plus one RGBA frame; the wapp's own
# buffers (a still, its base64) are a couple of megabytes of that.
LDFLAGS := \
	--target=wasm32-wasi \
	-nostartfiles \
	-lwasi-emulated-signal \
	-lwasi-emulated-pthread \
	-Wl,--no-entry \
	-Wl,--gc-sections \
	-Wl,-z,stack-size=2097152 \
	-Wl,--initial-memory=33554432 \
	-Wl,--export=module_init \
	-Wl,--export=module_tick \
	-Wl,--export=module_handle_event \
	-Wl,--export=module_destroy \
	-Wl,--export=module_tick_interval_ms \
	-Wl,--strip-all

# openh264 asks for POSIX semaphores its own threading uses; wasi has none
# and the decoder runs single-threaded here. The stubs are the player
# wapp's, for the same reason.
C_SRCS   := main.c wire.c rtsp.c md5.c ../mp4player/vendor/wasi_thread_stubs.c
CXX_SRCS := h264.cpp \
	$(wildcard $(OH)/common/src/*.cpp) \
	$(wildcard $(OH)/decoder/core/src/*.cpp) \
	$(wildcard $(OH)/decoder/plus/src/*.cpp)

OBJS := $(C_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o)

.PHONY: all clean
all: $(OUTPUT)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUTPUT): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS)
	@echo "Built $(OUTPUT) ($$(wc -c < $(OUTPUT)) bytes)"

clean:
	rm -f $(OBJS) $(OUTPUT)
