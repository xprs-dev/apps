/*
 * h264 -- the decoder, behind three C functions.
 *
 * openh264 is C++ and is vendored once in the tree (apps/mp4player/vendor);
 * this wapp compiles the decoder half of it and hands the rest of the wapp
 * a C surface, because everything else here is C and there is no reason for
 * a doorbell screen to know what a decoder is written in.
 *
 * The camera's sub stream is 640x480 High profile at about 36 kB/s, which is
 * what makes a live view on a phone reasonable at all: the same camera's
 * still is 585 kB of 5 MP JPEG, one frame.
 */
#ifndef THINGS_H264_H
#define THINGS_H264_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when a decoder is ready. */
int th_h264_open(void);

/* Decode one Annex-B access unit. When a picture comes out it is converted
 * to RGBA and handed to [emit] (width, height, RGBA bytes). Returns 1 when a
 * picture was emitted. */
int th_h264_decode(const unsigned char *au, unsigned len,
                   void (*emit)(const unsigned char *rgba, int w, int h));

void th_h264_close(void);

#ifdef __cplusplus
}
#endif
#endif
