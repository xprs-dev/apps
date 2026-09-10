# PacketVideo AAC decoder (Apache-2.0)

The AAC decoder Android shipped before it switched to FDK-AAC: AAC-LC,
HE-AAC (SBR) and HE-AACv2 (Parametric Stereo), mono and stereo.
Copyright PacketVideo and The Android Open Source Project, Apache-2.0
(`NOTICE`).

It replaced Fraunhofer's FDK-AAC here because FDK-AAC's licence grants no
patent rights. Fedora classifies it as not allowed, Debian keeps it out of
main, and F-Droid would have tagged or refused the app over it.

## Source

AOSP `platform/frameworks/av`, tag `android-4.1.2_r2.1`,
`media/libstagefright/codecs/aacdec/`: the 141 files of that directory's
`else # pv` branch in `Android.mk`, and their headers. The OMX wrappers
(`SoftAAC*.cpp`) are left out; `../aac_dec.cpp` drives the decoder the way
`SoftAAC.cpp` did. The decoder files are identical in `android-4.0.4_r2.1`
(`platform/frameworks/base`), where PV was still Android's default.

## Build flags

    -DAAC_PLUS -DHQ_SBR -DPARAMETRICSTEREO
    -DOSCL_IMPORT_REF= -DOSCL_EXPORT_REF= -DOSCL_UNUSED_ARG=
    -fno-strict-aliasing -Wno-c++11-narrowing

The first two lines are AOSP's. The last two stand in for its build
environment:

* **`-fno-strict-aliasing` is required.** The decoder reuses its buffers
  as other types (Int32 scratch memory read as Int16, pointer tables carved
  from sample buffers). AOSP compiled everything with this flag. Without it,
  clang's wasm32 `-O2` output was measured at -7 dB SNR against FDK-AAC,
  which is garbage; with it, the output is byte-identical to a native `-O0`
  build.
* `-Wno-c++11-narrowing`: the constant tables put hex values above INT32_MAX
  into Int32 arrays, which is legal in C++98 (AOSP's dialect) and an error
  in C++11. The bits are the same either way.

## Patch

One, marked `PATCHED (xprs)` in `s_ps_dec.h` and `ps_allocate_decoder.cpp`.
The Parametric Stereo decoder carved its row-pointer tables out of a reused
SBR buffer as one Int32 slot per pointer. That only holds where pointers are
32 bits wide, as on the ARM phones this was written for and on wasm32. On a
64-bit host the tables overran the delay samples and HE-AACv2 crashed, which
also made native testing impossible. The rows now live in the PS struct. On
wasm32 the decoded output is byte-identical with and without the patch.

## Verified against FDK-AAC

21 streams, decoded by this library as wasm32 and by fdk-aac 2.0.3, with
output compared after aligning the decoders' different delays: LC at
8/16/22.05/32/44.1/48 kHz mono and stereo; HE-AAC and HE-AACv2 with implicit
and explicit signalling, mono and stereo; genuinely stereo material; and a
real-world stream from another encoder. Every one had the same rate, the
same channel count and the same number of samples, at 64-75 dB SNR per
channel and the same stereo width. PV's output has 1685 samples (at 44.1 kHz
LC) less latency than FDK's.

Not supported: more than two channels (5.1 and up). `aac_open` fails and the
file plays without sound.
