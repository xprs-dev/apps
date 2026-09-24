/*
 * A native HAL for the Things wapp's tests: the core as the wapp sees it.
 * The station table (hal_xprs_stations / hal_xprs_station), the archive
 * (hal_xprs_history, answered by which query it is), the callsign follow
 * list, a capture of everything hal_msg_send says, an injectable event queue
 * and a clock the test moves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── the station table ────────────────────────────────────────────────── */
char g_stations_json[8192] = "[]";
int32_t hal_xprs_stations(char *out, uint32_t cap)
{
    uint32_t n = strlen(g_stations_json);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_stations_json, n);
    return (int32_t)n;
}

#define STN 8
char g_station_call[STN][16];
char g_station_json[STN][1024];
int  g_station_n;
void station_set(const char *call, const char *json)
{
    for (int i = 0; i < g_station_n; i++)
        if (!strcmp(g_station_call[i], call)) { snprintf(g_station_json[i], 1024, "%s", json); return; }
    if (g_station_n >= STN) return;
    snprintf(g_station_call[g_station_n], 16, "%s", call);
    snprintf(g_station_json[g_station_n++], 1024, "%s", json);
}
int32_t hal_xprs_station(const char *call, uint32_t cl, char *out, uint32_t cap)
{
    for (int i = 0; i < g_station_n; i++) {
        if (strlen(g_station_call[i]) != cl || memcmp(g_station_call[i], call, cl)) continue;
        uint32_t n = strlen(g_station_json[i]);
        if (!n) return 0;
        if (n > cap) return -(int32_t)n;
        memcpy(out, g_station_json[i], n);
        return (int32_t)n;
    }
    return 0;
}

/* ── the archive: answered by the first entry whose key is in the query ─ */
#define HN 16
char g_hist_key[HN][96];
char g_hist_ans[HN][8192];
int  g_hist_calls[HN];
int  g_hist_n;
int  g_hist_total;
void history_set(const char *key, const char *answer)
{
    for (int i = 0; i < g_hist_n; i++)
        if (!strcmp(g_hist_key[i], key)) { snprintf(g_hist_ans[i], 8192, "%s", answer); return; }
    if (g_hist_n >= HN) return;
    snprintf(g_hist_key[g_hist_n], 96, "%s", key);
    snprintf(g_hist_ans[g_hist_n++], 8192, "%s", answer);
}
int history_calls(const char *key)
{
    for (int i = 0; i < g_hist_n; i++) if (!strcmp(g_hist_key[i], key)) return g_hist_calls[i];
    return 0;
}
int32_t hal_xprs_history(const char *q, uint32_t ql, char *out, uint32_t cap)
{
    char query[512];
    snprintf(query, sizeof query, "%.*s", (int)ql, q);
    g_hist_total++;
    for (int i = 0; i < g_hist_n; i++) {
        if (!strstr(query, g_hist_key[i])) continue;
        g_hist_calls[i]++;
        uint32_t n = strlen(g_hist_ans[i]);
        if (n > cap) return -(int32_t)n;
        memcpy(out, g_hist_ans[i], n);
        return (int32_t)n;
    }
    memcpy(out, "[]", 2);
    return 2;
}

/* ── following by callsign ────────────────────────────────────────────── */
char g_followed[8][16];
int  g_followed_n;
int  g_follow_calls;
int32_t hal_xprs_follow(const char *c, uint32_t cl, int32_t on)
{
    g_follow_calls++;
    char call[16];
    snprintf(call, sizeof call, "%.*s", (int)cl, c);
    if (!cl || !strncmp(call, "X5", 2)) return -1;
    for (int i = 0; i < g_followed_n; i++)
        if (!strcmp(g_followed[i], call)) {
            if (!on) { memmove(g_followed[i], g_followed[i + 1], (size_t)(g_followed_n - i - 1) * 16); g_followed_n--; }
            return 0;
        }
    if (on && g_followed_n < 8) snprintf(g_followed[g_followed_n++], 16, "%s", call);
    return 0;
}
int32_t hal_xprs_followed(char *out, uint32_t cap)
{
    char s[256] = "[";
    for (int i = 0; i < g_followed_n; i++) {
        if (i) strcat(s, ",");
        strcat(s, "\""); strcat(s, g_followed[i]); strcat(s, "\"");
    }
    strcat(s, "]");
    uint32_t n = strlen(s);
    if (n > cap) return -(int32_t)n;
    memcpy(out, s, n);
    return (int32_t)n;
}

/* ── asking what is around ────────────────────────────────────────────── */
int g_discover_calls;
int g_discover_rc = 1;
int32_t hal_xprs_discover(void) { g_discover_calls++; return g_discover_rc; }

/* ── what the wapp says to the host ───────────────────────────────────── */
#define CAPN 400
static char *g_cap[CAPN];
static int g_capn;
void hal_msg_send(const char *j, uint32_t n)
{
    if (g_capn < CAPN) { g_cap[g_capn] = malloc(n + 1); memcpy(g_cap[g_capn], j, n); g_cap[g_capn][n] = 0; g_capn++; }
}
void cap_clear(void) { for (int i = 0; i < g_capn; i++) free(g_cap[i]); g_capn = 0; }
int cap_count(const char *s) { int c = 0; for (int i = 0; i < g_capn; i++) if (strstr(g_cap[i], s)) c++; return c; }
const char *cap_last(const char *s) { for (int i = g_capn - 1; i >= 0; i--) if (strstr(g_cap[i], s)) return g_cap[i]; return 0; }

/* ── the host's inbox and the core's events ───────────────────────────── */
static char g_inbox[4096];
static int g_inbox_set;
void inbox_set(const char *s) { snprintf(g_inbox, sizeof g_inbox, "%s", s); g_inbox_set = 1; }
uint32_t hal_msg_recv(char *b, uint32_t cap)
{
    if (!g_inbox_set) return 0;
    uint32_t n = strlen(g_inbox); if (n > cap) n = cap;
    memcpy(b, g_inbox, n); g_inbox_set = 0; return n;
}

#define EVQ 32
static char *g_evt[EVQ], *g_evd[EVQ];
static int g_evr, g_evw;
char g_subs[16][40];
int g_subn;
void event_push(const char *t, const char *d)
{
    if (g_evw - g_evr >= EVQ) return;
    g_evt[g_evw % EVQ] = strdup(t); g_evd[g_evw % EVQ] = strdup(d); g_evw++;
}
int32_t hal_event_subscribe(const char *t, uint32_t n)
{
    for (int i = 0; i < g_subn; i++) if (strlen(g_subs[i]) == n && !memcmp(g_subs[i], t, n)) return 0;
    if (g_subn < 16) snprintf(g_subs[g_subn++], 40, "%.*s", (int)n, t);
    return 0;
}
int32_t hal_event_unsubscribe(const char *t, uint32_t n)
{
    for (int i = 0; i < g_subn; i++) {
        if (strlen(g_subs[i]) != n || memcmp(g_subs[i], t, n)) continue;
        memmove(g_subs[i], g_subs[i + 1], (size_t)(g_subn - i - 1) * 40);
        g_subn--;
        return 0;
    }
    return -1;
}
int subscribed(const char *t) { for (int i = 0; i < g_subn; i++) if (!strcmp(g_subs[i], t)) return 1; return 0; }

/* ── this wapp's own key-value space, kept across a simulated restart ── */
#define KVN 64
char g_kv_key[KVN][64];
char g_kv_val[KVN][512];
int  g_kv_n;
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap)
{
    for (int i = 0; i < g_kv_n; i++) {
        if (strlen(g_kv_key[i]) != kl || memcmp(g_kv_key[i], k, kl)) continue;
        uint32_t n = strlen(g_kv_val[i]);
        if (n > cap) n = cap;
        memcpy(out, g_kv_val[i], n);
        return n;
    }
    return 0;
}
int32_t hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl)
{
    for (int i = 0; i < g_kv_n; i++) {
        if (strlen(g_kv_key[i]) != kl || memcmp(g_kv_key[i], k, kl)) continue;
        snprintf(g_kv_val[i], 512, "%.*s", (int)vl, v);
        return 0;
    }
    if (g_kv_n >= KVN) return -1;
    snprintf(g_kv_key[g_kv_n], 64, "%.*s", (int)kl, k);
    snprintf(g_kv_val[g_kv_n++], 512, "%.*s", (int)vl, v);
    return 0;
}
int32_t hal_kv_delete(const char *k, uint32_t kl)
{
    for (int i = 0; i < g_kv_n; i++) {
        if (strlen(g_kv_key[i]) != kl || memcmp(g_kv_key[i], k, kl)) continue;
        memmove(g_kv_key[i], g_kv_key[i + 1], (size_t)(g_kv_n - i - 1) * 64);
        memmove(g_kv_val[i], g_kv_val[i + 1], (size_t)(g_kv_n - i - 1) * 512);
        g_kv_n--;
        return 0;
    }
    return -1;
}
void kv_wipe(void) { g_kv_n = 0; }
const char *kv_peek(const char *k)
{
    for (int i = 0; i < g_kv_n; i++) if (!strcmp(g_kv_key[i], k)) return g_kv_val[i];
    return 0;
}
uint32_t hal_event_available(void) { return (uint32_t)(g_evw - g_evr); }
uint32_t hal_event_recv(char *tb, uint32_t tc, char *db, uint32_t dc)
{
    if (g_evr == g_evw) return 0;
    int i = g_evr++ % EVQ;
    snprintf(tb, tc, "%s", g_evt[i]);
    uint32_t n = strlen(g_evd[i]); if (n > dc) n = dc;
    memcpy(db, g_evd[i], n);
    free(g_evt[i]); free(g_evd[i]);
    return n;
}

/* ── time, log, page ──────────────────────────────────────────────────── */
uint64_t g_ms = 1000000;
uint64_t g_epoch = 1789000000;   /* 2026-09-10 */
uint64_t hal_time_ms(void) { return g_ms; }
uint64_t hal_time_epoch(void) { return g_epoch; }
/* A fixed offset, so a test that reads the clock on a picture reads the
 * same clock on every machine. */
/* MINUTES east of UTC, the unit the HAL states. It was seconds here while
 * the wapp read it as seconds too, so the pair agreed with each other and
 * with nothing else: the test passed and the picture was two minutes off. */
int32_t g_utc_offset = 120;
int32_t hal_time_utc_offset(void) { return g_utc_offset; }
void hal_log(int32_t l, const char *m, uint32_t n) { (void)l; (void)m; (void)n; }
int g_ui_attached = 1;
int32_t hal_ui_attached(void) { return g_ui_attached; }

/* ── a camera on the far end of HTTP ──────────────────────────────────
 * Canned answers chosen by a substring of the URL, each with a status, a
 * body (bytes, so a JPEG is a JPEG) and how many polls it takes to arrive.
 * The tests read back what was asked for, which is how the login body and
 * the snapshot URL are held to their shape without a camera. */
#define HTTPN 8
static struct {
    char match[96];
    int status;
    unsigned char body[8192];
    unsigned blen;
    int polls;             /* polls before it is done */
} g_canned[HTTPN];
static int g_canned_n;

void http_set(const char *match, int status, const void *body, unsigned blen, int polls)
{
    for (int i = 0; i < g_canned_n; i++) {
        if (strcmp(g_canned[i].match, match)) continue;
        g_canned[i].status = status; g_canned[i].blen = blen; g_canned[i].polls = polls;
        if (blen) memcpy(g_canned[i].body, body, blen > 8192 ? 8192 : blen);
        return;
    }
    if (g_canned_n >= HTTPN) return;
    snprintf(g_canned[g_canned_n].match, 96, "%s", match);
    g_canned[g_canned_n].status = status;
    g_canned[g_canned_n].polls = polls;
    g_canned[g_canned_n].blen = blen;
    if (blen) memcpy(g_canned[g_canned_n].body, body, blen > 8192 ? 8192 : blen);
    g_canned_n++;
}

#define REQN 8
static struct { int live, idx, polls; unsigned off; } g_req[REQN];
static int g_req_next = 1;
char g_last_url[512];
char g_last_body[1024];
int  g_last_method;
int  g_http_calls;

int32_t hal_http_request(int32_t method, const char *url, uint32_t ul,
                         const char *body, uint32_t bl)
{
    g_http_calls++;
    g_last_method = method;
    snprintf(g_last_url, sizeof g_last_url, "%.*s", (int)ul, url);
    snprintf(g_last_body, sizeof g_last_body, "%.*s", (int)bl, body ? body : "");
    int h = g_req_next++;
    if (h >= REQN) return -1;
    g_req[h].live = 1; g_req[h].polls = 0; g_req[h].off = 0; g_req[h].idx = -1;
    for (int i = 0; i < g_canned_n; i++)
        if (strstr(g_last_url, g_canned[i].match)) { g_req[h].idx = i; break; }
    return h;
}
int32_t hal_http_poll(int32_t h)
{
    if (h <= 0 || h >= REQN || !g_req[h].live) return -1;
    if (g_req[h].idx < 0) return -1;                 /* nothing answers there */
    if (++g_req[h].polls <= g_canned[g_req[h].idx].polls) return 0;
    return 1;
}
int32_t hal_http_status(int32_t h)
{
    if (h <= 0 || h >= REQN || !g_req[h].live || g_req[h].idx < 0) return -1;
    return g_canned[g_req[h].idx].status;
}
int32_t hal_http_read_response(int32_t h, char *buf, uint32_t cap)
{
    if (h <= 0 || h >= REQN || !g_req[h].live || g_req[h].idx < 0) return 0;
    unsigned left = g_canned[g_req[h].idx].blen - g_req[h].off;
    if (!left || !cap) return 0;
    unsigned n = left < cap ? left : cap;
    memcpy(buf, g_canned[g_req[h].idx].body + g_req[h].off, n);
    g_req[h].off += n;
    return (int32_t)n;
}
void hal_http_free(int32_t h) { if (h > 0 && h < REQN) g_req[h].live = 0; }
void http_reset(void)
{
    g_canned_n = 0; g_req_next = 1; g_http_calls = 0;
    g_last_url[0] = g_last_body[0] = 0;
    for (int i = 0; i < REQN; i++) g_req[i].live = 0;
}

/* ── this device's key, and sealing to it ─────────────────────────────
 * Not the real curve: a reversible transform with the same shape, so the
 * test holds the wapp to sealing what it keeps and to never writing the
 * password down in the clear. */
uint32_t hal_identity_pubkey(char *b, uint32_t cap)
{
    const char *k = "npub1mockmockmockmockmockmockmockmockmockmockmockmockmock";
    uint32_t n = strlen(k); if (n > cap) n = cap;
    memcpy(b, k, n); return n;
}
uint32_t hal_encrypt(const char *pk, uint32_t pl, const char *msg, uint32_t ml,
                     char *out, uint32_t cap)
{
    (void)pk; (void)pl;
    if (ml * 2 + 6 > cap) return 0;
    memcpy(out, "seal:", 5);
    for (uint32_t i = 0; i < ml; i++) sprintf(out + 5 + i * 2, "%02x", (unsigned char)msg[i]);
    return 5 + ml * 2;
}
uint32_t hal_decrypt(const char *pk, uint32_t pl, const char *blob, uint32_t bl,
                     char *out, uint32_t cap)
{
    (void)pk; (void)pl;
    if (bl < 5 || memcmp(blob, "seal:", 5)) return 0;
    uint32_t n = (bl - 5) / 2;
    if (n >= cap) return 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned v; sscanf(blob + 5 + i * 2, "%2x", &v); out[i] = (char)v;
    }
    out[n] = 0;
    return n;
}

/* ── a socket, and what a camera would send down one ──────────────────
 * The live view is a TCP connection carrying multipart JPEGs. The test
 * scripts what the far end does: whether it connects at all, and the bytes
 * it sends, handed over in pieces the way a socket hands them over. */
static struct {
    int  live;
    int  state;                 /* 0 connecting, 1 open, 2 closed */
    char host[64];
    int  port;
    char sent[512];
    unsigned char rx[1200000];
    unsigned rxn, rxoff;
} g_sock;
int g_sock_opens;
int g_sock_open_rc = 0;         /* -1 = the host refuses to open one */
int g_sock_closes;

void sock_reset(void)
{
    memset(&g_sock, 0, sizeof g_sock);
    g_sock_opens = g_sock_closes = 0;
    g_sock_open_rc = 0;
}
void sock_state(int s) { g_sock.state = s; }
void sock_feed(const void *b, unsigned n)
{
    if (g_sock.rxn + n > sizeof g_sock.rx) return;
    memcpy(g_sock.rx + g_sock.rxn, b, n);
    g_sock.rxn += n;
}
const char *sock_sent(void) { return g_sock.sent; }
const char *sock_host(void) { return g_sock.host; }
int sock_port(void) { return g_sock.port; }
unsigned sock_unread(void) { return g_sock.rxn - g_sock.rxoff; }

int32_t hal_socket_open(const char *host, uint32_t hl, int32_t port)
{
    g_sock_opens++;
    if (g_sock_open_rc < 0) return -1;
    memset(&g_sock, 0, sizeof g_sock);
    snprintf(g_sock.host, sizeof g_sock.host, "%.*s", (int)hl, host);
    g_sock.port = port;
    g_sock.live = 1;
    g_sock.state = 1;
    return 7;
}
int32_t hal_socket_status(int32_t h) { return h == 7 && g_sock.live ? g_sock.state : 2; }
int32_t hal_socket_send(int32_t h, const char *b, uint32_t n)
{
    if (h != 7 || !g_sock.live || g_sock.state != 1) return -1;
    unsigned at = (unsigned)strlen(g_sock.sent);
    unsigned room = (unsigned)sizeof g_sock.sent - at - 1;
    if (n > room) n = room;
    memcpy(g_sock.sent + at, b, n);
    g_sock.sent[at + n] = 0;
    return (int32_t)n;
}
uint32_t hal_socket_recv(int32_t h, char *b, uint32_t cap)
{
    if (h != 7 || !g_sock.live) return 0;
    unsigned left = g_sock.rxn - g_sock.rxoff;
    if (!left || !cap) return 0;
    unsigned n = left < cap ? left : cap;
    memcpy(b, g_sock.rx + g_sock.rxoff, n);
    g_sock.rxoff += n;
    return n;
}
void hal_socket_close(int32_t h)
{
    if (h != 7) return;
    g_sock_closes++;
    g_sock.live = 0;
    g_sock.state = 2;
}

/* ── the video sink, counted rather than shown ─────────────────────────
 * The tests hold the wapp to WHAT it pushes and when it stops, not to what
 * a frame looks like: a decoded picture is openh264's business and it is
 * not compiled into this build. */
int g_video_frames;
int g_video_w, g_video_h;
int g_video_configs;

void hal_video_config(int32_t w, int32_t h, int32_t pixfmt)
{
    (void)pixfmt;
    g_video_configs++;
    g_video_w = w;
    g_video_h = h;
}
void hal_video_frame(const uint8_t *d, uint32_t n, int32_t w, int32_t h,
                     int32_t pixfmt, int32_t pts)
{
    (void)d; (void)n; (void)pixfmt; (void)pts;
    g_video_frames++;
    g_video_w = w;
    g_video_h = h;
}
void hal_video_end(void) { }

/* The decoder itself is C++ and lives only in the wasm build; here it is a
 * stand-in that says "no picture yet", so the RTSP half can be tested. */
int th_h264_open(void) { return 1; }
void th_h264_close(void) { }
int th_h264_decode(const unsigned char *au, unsigned len,
                   void (*emit)(const unsigned char *rgba, int w, int h))
{
    (void)au; (void)len; (void)emit;
    return 0;
}
