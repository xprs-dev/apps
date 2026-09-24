#include "rtsp.h"
#include "wire.h"
#include "md5.h"

/* ── little helpers, in the house style: no libc ──────────────────────── */

static void app_u(char *d, unsigned long long v, unsigned cap) { th_cat_u(d, v, cap); }

static int ci_eq(const char *a, const char *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* The value of [key] in an RTSP head, up to the end of its line. */
static int header(const char *head, const char *key, char *out, unsigned cap)
{
    unsigned kl = th_len(key);
    out[0] = 0;
    for (const char *p = head; *p; p++) {
        if (p != head && p[-1] != '\n') continue;
        if (!ci_eq(p, key, kl)) continue;
        const char *v = p + kl;
        if (*v != ':') continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        unsigned n = 0;
        while (v[n] && v[n] != '\r' && v[n] != '\n' && n < cap - 1) n++;
        for (unsigned i = 0; i < n; i++) out[i] = v[i];
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* `key="value"` out of a header line (the digest challenge). */
static int quoted(const char *s, const char *key, char *out, unsigned cap)
{
    unsigned kl = th_len(key);
    out[0] = 0;
    for (const char *p = s; *p; p++) {
        if (!ci_eq(p, key, kl)) continue;
        const char *v = p + kl;
        while (*v == ' ') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ') v++;
        if (*v != '"') continue;
        v++;
        unsigned n = 0;
        while (v[n] && v[n] != '"' && n < cap - 1) n++;
        for (unsigned i = 0; i < n; i++) out[i] = v[i];
        out[n] = 0;
        return 1;
    }
    return 0;
}

static int status_of(const char *head)
{
    /* "RTSP/1.0 200 OK" */
    const char *p = head;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    return (int)th_num(p);
}

/* ── the digest the camera asks for (RFC 2069's shape, which is what a
 *    camera sends: realm and nonce, no qop) ─────────────────────────────── */
static void digest_header(rtsp_t *r, const char *method, const char *uri,
                          char *out, unsigned cap)
{
    char buf[400], ha1[40], ha2[40], rsp[40];
    buf[0] = 0;
    th_cat(buf, r->user, sizeof buf); th_cat(buf, ":", sizeof buf);
    th_cat(buf, r->realm, sizeof buf); th_cat(buf, ":", sizeof buf);
    th_cat(buf, r->pass, sizeof buf);
    th_md5_hex(buf, th_len(buf), ha1);

    buf[0] = 0;
    th_cat(buf, method, sizeof buf); th_cat(buf, ":", sizeof buf);
    th_cat(buf, uri, sizeof buf);
    th_md5_hex(buf, th_len(buf), ha2);

    buf[0] = 0;
    th_cat(buf, ha1, sizeof buf); th_cat(buf, ":", sizeof buf);
    th_cat(buf, r->nonce, sizeof buf); th_cat(buf, ":", sizeof buf);
    th_cat(buf, ha2, sizeof buf);
    th_md5_hex(buf, th_len(buf), rsp);

    th_cpy(out, "Authorization: Digest username=\"", cap);
    th_cat(out, r->user, cap);
    th_cat(out, "\", realm=\"", cap);
    th_cat(out, r->realm, cap);
    th_cat(out, "\", nonce=\"", cap);
    th_cat(out, r->nonce, cap);
    th_cat(out, "\", uri=\"", cap);
    th_cat(out, uri, cap);
    th_cat(out, "\", response=\"", cap);
    th_cat(out, rsp, cap);
    th_cat(out, "\"\r\n", cap);
}

static int say(rtsp_t *r, const char *req)
{
    int n = r->io.send(r->io.user, req, th_len(req));
    if (n < 0) { rtsp_stop(r, "The camera closed the connection."); return 0; }
    return 1;
}

static void ask(rtsp_t *r, const char *method, const char *uri, const char *extra)
{
    char req[900], auth[500];
    th_cpy(req, method, sizeof req);
    th_cat(req, " ", sizeof req);
    th_cat(req, uri, sizeof req);
    th_cat(req, " RTSP/1.0\r\nCSeq: ", sizeof req);
    app_u(req, (unsigned)++r->cseq, sizeof req);
    th_cat(req, "\r\nUser-Agent: XPRS Things\r\n", sizeof req);
    if (r->session[0]) {
        th_cat(req, "Session: ", sizeof req);
        th_cat(req, r->session, sizeof req);
        th_cat(req, "\r\n", sizeof req);
    }
    if (r->realm[0]) {
        digest_header(r, method, uri, auth, sizeof auth);
        th_cat(req, auth, sizeof req);
    }
    if (extra) th_cat(req, extra, sizeof req);
    th_cat(req, "\r\n", sizeof req);
    say(r, req);
}

void rtsp_stop(rtsp_t *r, const char *why)
{
    if (why && !r->why[0]) th_cpy(r->why, why, sizeof r->why);
    r->state = RTSP_DONE;
}

void rtsp_begin(rtsp_t *r, const rtsp_io *io, const char *host, int port,
                const char *path, const char *user, const char *pass,
                rtsp_au_fn on_au, void *au_user)
{
    for (unsigned i = 0; i < sizeof *r; i++) ((char *)r)[i] = 0;
    r->io = *io;
    th_cpy(r->host, host, sizeof r->host);
    r->port = port > 0 ? port : 554;
    th_cpy(r->user, user, sizeof r->user);
    th_cpy(r->pass, pass, sizeof r->pass);
    th_cpy(r->url, "rtsp://", sizeof r->url);
    th_cat(r->url, host, sizeof r->url);
    th_cat(r->url, ":", sizeof r->url);
    app_u(r->url, (unsigned)r->port, sizeof r->url);
    if (path[0] != '/') th_cat(r->url, "/", sizeof r->url);
    th_cat(r->url, path, sizeof r->url);
    r->on_au = on_au;
    r->au_user = au_user;
    r->state = RTSP_IDLE;
    r->began_ms = r->byte_ms = r->kept_ms = io->now_ms(io->user);
}

/* ── what the SDP says: the control track, and the parameter sets ─────── */
static void read_sdp(rtsp_t *r, const char *sdp)
{
    /* a=control:<track>, the first one that is not "*" */
    r->track[0] = 0;
    for (const char *p = sdp; *p; p++) {
        if (p != sdp && p[-1] != '\n') continue;
        if (!th_starts(p, "a=control:")) continue;
        const char *v = p + 10;
        if (*v == '*') continue;
        unsigned n = 0;
        char t[200];
        while (v[n] && v[n] != '\r' && v[n] != '\n' && n < sizeof t - 1) { t[n] = v[n]; n++; }
        t[n] = 0;
        if (!t[0]) continue;
        if (th_starts(t, "rtsp://")) th_cpy(r->track, t, sizeof r->track);
        else {
            th_cpy(r->track, r->url, sizeof r->track);
            th_cat(r->track, "/", sizeof r->track);
            th_cat(r->track, t, sizeof r->track);
        }
        break;
    }
    if (!r->track[0]) th_cpy(r->track, r->url, sizeof r->track);

    /* sprop-parameter-sets=<b64 SPS>,<b64 PPS>: in front of the first frame,
     * because a decoder handed a picture with no parameter sets has nothing
     * to decode it against. */
    for (const char *p = sdp; *p; p++) {
        if (!th_starts(p, "sprop-parameter-sets=")) continue;
        const char *v = p + 21;
        while (*v && *v != '\r' && *v != '\n' && *v != ';') {
            char one[200];
            unsigned n = 0;
            while (v[n] && v[n] != ',' && v[n] != ';' && v[n] != '\r' &&
                   v[n] != '\n' && n < sizeof one - 1) { one[n] = v[n]; n++; }
            one[n] = 0;
            v += n;
            if (*v == ',') v++;
            unsigned char nal[300];
            unsigned m = th_b64_dec(one, nal, sizeof nal);
            if (m && r->aun + m + 4 < sizeof r->au) {
                r->au[r->aun++] = 0; r->au[r->aun++] = 0;
                r->au[r->aun++] = 0; r->au[r->aun++] = 1;
                for (unsigned i = 0; i < m; i++) r->au[r->aun++] = nal[i];
                r->have_ps = 1;
            }
        }
        break;
    }
}

/* ── RTP, unwrapped ───────────────────────────────────────────────────── */
static void au_add(rtsp_t *r, const unsigned char *b, unsigned n, int start_code)
{
    if (r->aun + n + 4 >= sizeof r->au) return;    /* a frame that big is not ours */
    if (start_code) {
        r->au[r->aun++] = 0; r->au[r->aun++] = 0;
        r->au[r->aun++] = 0; r->au[r->aun++] = 1;
    }
    for (unsigned i = 0; i < n; i++) r->au[r->aun++] = b[i];
}

static void au_done(rtsp_t *r)
{
    if (r->aun && r->on_au) {
        r->frames++;
        r->on_au(r->au_user, r->au, r->aun);
    }
    r->aun = 0;
    r->have_ps = 0;
}

static void rtp(rtsp_t *r, const unsigned char *p, unsigned n)
{
    if (n < 12) return;
    unsigned cc = p[0] & 0x0F;
    int marker = (p[1] & 0x80) != 0;
    unsigned off = 12 + cc * 4;
    if ((p[0] & 0x10)) {                 /* an extension header, skipped */
        if (off + 4 > n) return;
        unsigned ext = ((unsigned)p[off + 2] << 8 | p[off + 3]) * 4;
        off += 4 + ext;
    }
    if (off >= n) return;
    const unsigned char *pay = p + off;
    unsigned len = n - off;
    unsigned type = pay[0] & 0x1F;

    if (type == 28) {                    /* FU-A: one NAL across packets */
        if (len < 2) return;
        int start = (pay[1] & 0x80) != 0;
        unsigned char nal = (unsigned char)((pay[0] & 0xE0) | (pay[1] & 0x1F));
        if (start) {
            au_add(r, &nal, 1, 1);
            au_add(r, pay + 2, len - 2, 0);
        } else {
            au_add(r, pay + 2, len - 2, 0);
        }
    } else if (type == 24) {             /* STAP-A: several NALs in one */
        unsigned i = 1;
        while (i + 2 <= len) {
            unsigned sz = ((unsigned)pay[i] << 8) | pay[i + 1];
            i += 2;
            if (sz == 0 || i + sz > len) break;
            au_add(r, pay + i, sz, 1);
            i += sz;
        }
    } else if (type >= 1 && type <= 23) {
        au_add(r, pay, len, 1);
    }
    /* The marker bit ends a picture: everything gathered is one access unit,
     * which is what a decoder wants handed to it. */
    if (marker) au_done(r);
}

/* ── the head of an RTSP reply, and what to do about it ───────────────── */
static void reply(rtsp_t *r, const char *head, const char *body)
{
    int code = status_of(head);
    char v[200];
    if (code == 401) {
        if (r->realm[0]) { rtsp_stop(r, "The camera refused that user and password."); return; }
        if (!header(head, "WWW-Authenticate", v, sizeof v) ||
            !quoted(v, "realm", r->realm, sizeof r->realm) ||
            !quoted(v, "nonce", r->nonce, sizeof r->nonce)) {
            rtsp_stop(r, "The camera asked for a login this wapp cannot give.");
            return;
        }
        r->state = RTSP_DESCRIBE_AUTH;
        ask(r, "DESCRIBE", r->url, "Accept: application/sdp\r\n");
        return;
    }
    if (code != 200) {
        char l[120] = "The camera answered ";
        app_u(l, (unsigned)(code < 0 ? 0 : code), sizeof l);
        th_cat(l, " to the live view.", sizeof l);
        rtsp_stop(r, l);
        return;
    }
    if (r->state == RTSP_STREAM) return;   /* a keepalive's answer, nothing more */
    switch (r->state) {
    case RTSP_DESCRIBE:
    case RTSP_DESCRIBE_AUTH:
        read_sdp(r, body);
        r->state = RTSP_SETUP;
        ask(r, "SETUP", r->track,
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
        break;
    case RTSP_SETUP:
        if (header(head, "Session", v, sizeof v)) {
            unsigned n = 0;
            while (v[n] && v[n] != ';' && v[n] != ' ' && n < sizeof r->session - 1) {
                r->session[n] = v[n]; n++;
            }
            r->session[n] = 0;
        }
        r->state = RTSP_PLAY;
        ask(r, "PLAY", r->url, "Range: npt=0.000-\r\n");
        break;
    case RTSP_PLAY:
        r->state = RTSP_STREAM;
        break;
    default:
        break;
    }
}

int rtsp_pump(rtsp_t *r)
{
    if (r->state == RTSP_DONE) return r->state;
    unsigned long long now = r->io.now_ms(r->io.user);

    if (r->state == RTSP_IDLE) {
        r->state = RTSP_DESCRIBE;
        ask(r, "DESCRIBE", r->url, "Accept: application/sdp\r\n");
        return r->state;
    }

    for (;;) {
        char chunk[4096];
        int n = r->io.recv(r->io.user, chunk, sizeof chunk);
        if (n < 0) { rtsp_stop(r, "The camera closed the connection."); return r->state; }
        if (n == 0) break;
        r->byte_ms = now;
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)chunk[i];
            if (r->in_binary) {
                r->pkt[r->pktn++] = c;
                if (r->pktn == 3) {
                    /* $, channel, then two bytes of length */
                    continue;
                }
                if (r->pktn == 4) {
                    r->pktwant = ((unsigned)r->pkt[2] << 8) | r->pkt[3];
                    if (r->pktwant + 4 > sizeof r->pkt) {  /* not a frame we can hold */
                        r->in_binary = 0; r->pktn = 0;
                    }
                    continue;
                }
                if (r->pktn >= 4 + r->pktwant) {
                    if (r->pkt[1] == 0) rtp(r, r->pkt + 4, r->pktwant);
                    r->in_binary = 0;
                    r->pktn = 0;
                }
                continue;
            }
            if (c == '$' && r->rxn == 0) {          /* interleaved data */
                r->in_binary = 1;
                r->pktn = 0;
                r->pkt[r->pktn++] = c;
                continue;
            }
            if (r->rxn + 1 >= sizeof r->rx) { r->rxn = 0; continue; }
            r->rx[r->rxn++] = (char)c;
            r->rx[r->rxn] = 0;
            /* A head ends at the blank line; a DESCRIBE carries a body after
             * it, whose length the head states. */
            if (r->rxn >= 4 && r->rx[r->rxn - 4] == '\r' && r->rx[r->rxn - 3] == '\n' &&
                r->rx[r->rxn - 2] == '\r' && r->rx[r->rxn - 1] == '\n') {
                char cl[24];
                unsigned want = header(r->rx, "Content-Length", cl, sizeof cl)
                                    ? (unsigned)th_num(cl) : 0;
                if (!want) {
                    char head[4096];
                    th_cpy(head, r->rx, sizeof head);
                    r->rxn = 0; r->rx[0] = 0;
                    reply(r, head, "");
                    if (r->state == RTSP_DONE) return r->state;
                    continue;
                }
                /* read the body straight out of what is left of this chunk
                 * and, if it is short, out of the next reads */
                static char body[8192];
                unsigned got = 0;
                while (got < want && got + 1 < sizeof body) {
                    if (i + 1 < n) { body[got++] = chunk[++i]; continue; }
                    int m = r->io.recv(r->io.user, chunk, sizeof chunk);
                    if (m < 0) { rtsp_stop(r, "The camera closed the connection."); return r->state; }
                    if (m == 0) continue;
                    n = m; i = -1;
                }
                body[got] = 0;
                char head[4096];
                th_cpy(head, r->rx, sizeof head);
                r->rxn = 0; r->rx[0] = 0;
                reply(r, head, body);
                if (r->state == RTSP_DONE) return r->state;
            }
        }
    }

    if (r->state != RTSP_STREAM && now - r->began_ms > 12000ULL) {
        rtsp_stop(r, "The camera did not start the live view.");
        return r->state;
    }
    if (r->state == RTSP_STREAM) {
        /* The camera says how long it will carry a session nobody speaks on:
         * this one answers SETUP with `timeout=30`, and at thirty seconds the
         * pictures simply stop -- measured, twice, before this line existed.
         * OPTIONS is the cheapest thing to say, and saying it twice inside
         * that window is what keeps them coming. */
        if (now - r->kept_ms > 10000ULL) {
            r->kept_ms = now;
            ask(r, "OPTIONS", r->url, 0);
        }
        if (now - r->byte_ms > 15000ULL)
            rtsp_stop(r, "The live view stopped arriving.");
    }
    return r->state;
}
