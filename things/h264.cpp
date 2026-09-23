#include "h264.h"

#include "codec_api.h"
#include "codec_app_def.h"

/* One picture's worth of RGBA. 640x480 is what the doorbell's sub stream
 * is; a camera that sends more than this is refused rather than silently
 * reallocating on a device the wapp knows nothing about. */
#define MAXW 1280
#define MAXH 720
static unsigned char g_rgba[MAXW * MAXH * 4];

static ISVCDecoder *g_dec;

extern "C" int th_h264_open(void)
{
    if (g_dec) return 1;
    if (WelsCreateDecoder(&g_dec) != 0 || !g_dec) { g_dec = 0; return 0; }
    SDecodingParam dp;
    for (unsigned i = 0; i < sizeof dp; i++) ((char *)&dp)[i] = 0;
    dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    dp.eEcActiveIdc = ERROR_CON_DISABLE;
    if (g_dec->Initialize(&dp) != 0) {
        WelsDestroyDecoder(g_dec);
        g_dec = 0;
        return 0;
    }
    return 1;
}

extern "C" void th_h264_close(void)
{
    if (!g_dec) return;
    g_dec->Uninitialize();
    WelsDestroyDecoder(g_dec);
    g_dec = 0;
}

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

extern "C" int th_h264_decode(const unsigned char *au, unsigned len,
                              void (*emit)(const unsigned char *rgba, int w, int h))
{
    if (!g_dec || !au || !len) return 0;
    unsigned char *pd[3] = { 0, 0, 0 };
    SBufferInfo bi;
    for (unsigned i = 0; i < sizeof bi; i++) ((char *)&bi)[i] = 0;
    if (g_dec->DecodeFrameNoDelay((const unsigned char *)au, (int)len, pd, &bi) != 0)
        return 0;
    if (bi.iBufferStatus != 1 || !pd[0]) return 0;

    int w = bi.UsrData.sSystemBuffer.iWidth;
    int h = bi.UsrData.sSystemBuffer.iHeight;
    int sy = bi.UsrData.sSystemBuffer.iStride[0];
    int sc = bi.UsrData.sSystemBuffer.iStride[1];
    if (w <= 0 || h <= 0 || w > MAXW || h > MAXH) return 0;

    /* BT.601, the same arithmetic the player wapp uses: the host holds no
     * codec and takes RGBA, so the conversion is ours. */
    for (int y = 0; y < h; y++) {
        const unsigned char *yr = pd[0] + y * sy;
        const unsigned char *ur = pd[1] + (y >> 1) * sc;
        const unsigned char *vr = pd[2] + (y >> 1) * sc;
        unsigned char *o = g_rgba + (unsigned)y * (unsigned)w * 4;
        for (int x = 0; x < w; x++) {
            int c = yr[x] - 16;
            int d = ur[x >> 1] - 128;
            int e = vr[x >> 1] - 128;
            o[0] = clamp8((298 * c + 409 * e + 128) >> 8);
            o[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[2] = clamp8((298 * c + 516 * d + 128) >> 8);
            o[3] = 255;
            o += 4;
        }
    }
    if (emit) emit(g_rgba, w, h);
    return 1;
}
