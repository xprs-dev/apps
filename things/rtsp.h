/*
 * rtsp -- as much of RTSP as a camera needs, and nothing else.
 *
 * DESCRIBE, SETUP, PLAY, and the RTP that follows on the SAME TCP
 * connection (RFC 2326's interleaved mode). One socket, opened by the wapp,
 * carrying the doorbell's 640x480 sub stream at about 36 kB/s -- against
 * 585 kB for one frame of the picture its `Snap` returns, which is the whole
 * reason this exists.
 *
 * It parses only what it needs: the WWW-Authenticate challenge (the camera
 * offers Digest and nothing else), the SDP's control track and its
 * sprop-parameter-sets, the session id, and then binary frames. What comes
 * out is H.264 access units in Annex-B, handed one at a time to a callback.
 *
 * The transport is INJECTED: the same code runs on hal_socket_* inside the
 * wapp and on a real socket in the native test, which is the only way to
 * develop a protocol against a camera that is not on the build machine.
 */
#ifndef THINGS_RTSP_H
#define THINGS_RTSP_H

#define RTSP_AU_MAX (192u * 1024u)

typedef struct {
    /* Bytes accepted, or < 0 when the connection is gone. */
    int (*send)(void *user, const char *buf, unsigned len);
    /* Bytes read (0 = nothing waiting), or < 0 when the connection is gone. */
    int (*recv)(void *user, char *buf, unsigned cap);
    /* Milliseconds since anything, for the timeouts. */
    unsigned long long (*now_ms)(void *user);
    void *user;
} rtsp_io;

/* One finished access unit, Annex-B, SPS/PPS first when the stream said so. */
typedef void (*rtsp_au_fn)(void *user, const unsigned char *au, unsigned len);

enum {
    RTSP_IDLE = 0,
    RTSP_DESCRIBE,       /* asked, unauthenticated: expect the challenge */
    RTSP_DESCRIBE_AUTH,  /* asked again, with the digest */
    RTSP_SETUP,
    RTSP_PLAY,
    RTSP_STREAM,         /* frames are arriving */
    RTSP_DONE,           /* finished or refused; `why` says which */
};

typedef struct {
    rtsp_io io;
    int state;
    int cseq;
    char host[64];
    int  port;
    char url[200];          /* rtsp://host:port/path */
    char track[200];        /* the control url SETUP is sent to */
    char user[32], pass[72];
    char realm[80], nonce[80];
    char session[80];
    char why[120];          /* why it stopped, in words for a person */
    unsigned long long began_ms, byte_ms, kept_ms;

    char  rx[4096];         /* an RTSP reply's head, until the blank line */
    unsigned rxn;
    unsigned char pkt[16384];  /* one interleaved packet */
    unsigned pktn, pktwant;
    int in_binary;

    unsigned char au[RTSP_AU_MAX];
    unsigned aun;
    int have_ps;            /* SPS/PPS already in front of the first frame */
    unsigned frames;

    rtsp_au_fn on_au;
    void *au_user;
} rtsp_t;

/* Point it at rtsp://<host>:<port><path> with the credentials the camera
 * will ask for. Nothing is sent yet: the socket is the caller's. */
void rtsp_begin(rtsp_t *r, const rtsp_io *io, const char *host, int port,
                const char *path, const char *user, const char *pass,
                rtsp_au_fn on_au, void *au_user);

/* Drive it. Call once the socket is open, then on every tick. Returns the
 * state; RTSP_DONE means it is over and `why` says what happened. */
int rtsp_pump(rtsp_t *r);

/* Say why it ended and move to RTSP_DONE. */
void rtsp_stop(rtsp_t *r, const char *why);

#endif
