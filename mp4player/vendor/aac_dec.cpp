// AAC decode over the PacketVideo decoder that Android shipped before
// FDK-AAC (Apache-2.0, vendor/pvaac/, see its README.xprs.md). Same
// contract as aac_dec.h: the mp4 AudioSpecificConfig once, then one access
// unit at a time to interleaved s16 PCM.
//
// Driven the way Android's own SoftAAC.cpp drove it: always two output
// channels (the decoder's mono path is broken for AAC+, so mono comes out
// duplicated on both), and HE-AAC (SBR) on until the stream proves to be plain
// AAC-LC.
#include "aac_dec.h"

#include "pvmp4audiodecoder_api.h"
#include "e_tmp4audioobjecttype.h"

#include <stdlib.h>
#include <string.h>

namespace {

struct PvAac {
  tPVMP4AudioDecoderExternal ext;
  void* mem;
  int frames_decoded;  // successful frames so far
};

// The decoder writes the SBR half of a frame 2048 samples after the core
// half, so one frame needs room for 2 x 2048 interleaved stereo samples.
const int kMaxFrameShorts = 2 * 2048 * 2;

void pv_free(PvAac* p) {
  free(p->mem);
  free(p);
}

}  // namespace

extern "C" int aac_open(AacDec* d, const uint8_t* asc, int asc_len) {
  memset(d, 0, sizeof(*d));
  if (!asc || asc_len <= 0) return 0;
  PvAac* p = (PvAac*)calloc(1, sizeof(PvAac));
  if (!p) return 0;
  p->ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
  p->ext.aacPlusEnabled = 1;
  p->ext.desiredChannels = 2;
  // Zeroed, as the fresh pages Android's malloc handed it were.
  p->mem = calloc(1, PVMP4AudioDecoderGetMemRequirements());
  if (!p->mem || PVMP4AudioDecoderInitLibrary(&p->ext, p->mem) != MP4AUDEC_SUCCESS) {
    pv_free(p);
    return 0;
  }
  p->ext.pInputBuffer = (UChar*)asc;
  p->ext.inputBufferCurrentLength = asc_len;
  p->ext.inputBufferMaxLength = 0;
  // Fails for more than two channels, which this decoder does not do.
  if (PVMP4AudioDecoderConfig(&p->ext, p->mem) != MP4AUDEC_SUCCESS) {
    pv_free(p);
    return 0;
  }
  d->h = p;
  return 1;
}

extern "C" int aac_decode(AacDec* d, const uint8_t* au, int au_len,
                          int16_t* out, int out_max, int* out_ch,
                          int* out_rate) {
  PvAac* p = (PvAac*)d->h;
  if (!p || !au || au_len <= 0 || out_max < kMaxFrameShorts) return -1;
  tPVMP4AudioDecoderExternal* e = &p->ext;
  e->pInputBuffer = (UChar*)au;
  e->inputBufferCurrentLength = au_len;
  e->inputBufferMaxLength = 0;
  e->inputBufferUsedLength = 0;
  e->remainderBits = 0;
  e->pOutputBuffer = out;
  e->pOutputBuffer_plus = out + 2048;
  e->repositionFlag = false;
  if (PVMP4AudioDecodeFrame(e, p->mem) != MP4AUDEC_SUCCESS) return -1;

  // After the second frame the object type is settled. A plain AAC-LC stream
  // that was tentatively decoded as AAC+ (low sampling rates signal SBR
  // implicitly, so the decoder has to guess) goes back to plain AAC.
  if (++p->frames_decoded == 2 &&
      (e->extendedAudioObjectType == MP4AUDIO_AAC_LC ||
       e->extendedAudioObjectType == MP4AUDIO_LTP)) {
    e->aacPlusEnabled = 0;
  }

  if (out_ch) *out_ch = e->desiredChannels;
  if (out_rate) *out_rate = e->samplingRate;
  return e->frameLength * e->aacPlusUpsamplingFactor;
}

extern "C" void aac_close(AacDec* d) {
  PvAac* p = d ? (PvAac*)d->h : NULL;
  if (!p) return;
  pv_free(p);
  d->h = NULL;
}
