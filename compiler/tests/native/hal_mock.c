/*
 * A mock HAL for the Compiler add-on's native tests: the host's message
 * queues, the add-on's key-value store, real files on disk, and one canned
 * HTTP service whose last request the test reads back.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_mock.h"

/* ── Messages ──────────────────────────────────────────────────────── */

#define QMAX 64
static char *g_in[QMAX];
static int g_in_n;
char *mock_out[MOCK_OUT_MAX];
int mock_out_n;

void mock_send_to_wapp(const char *json) {
    if (g_in_n < QMAX) g_in[g_in_n++] = strdup(json);
}
void mock_out_clear(void) {
    for (int i = 0; i < mock_out_n; i++) free(mock_out[i]);
    mock_out_n = 0;
}

void hal_msg_send(const char *json, uint32_t len) {
    if (mock_out_n >= MOCK_OUT_MAX) return;
    char *c = malloc(len + 1);
    memcpy(c, json, len);
    c[len] = 0;
    mock_out[mock_out_n++] = c;
}
uint32_t hal_msg_available(void) {
    return g_in_n ? (uint32_t)strlen(g_in[0]) : 0;
}
uint32_t hal_msg_recv(char *buf, uint32_t cap) {
    if (!g_in_n) return 0;
    uint32_t n = (uint32_t)strlen(g_in[0]);
    if (n > cap) n = cap;
    memcpy(buf, g_in[0], n);
    free(g_in[0]);
    memmove(g_in, g_in + 1, sizeof(char *) * (size_t)(--g_in_n));
    return n;
}

/* ── Key-value ─────────────────────────────────────────────────────── */

#define KVMAX 32
static char *g_k[KVMAX], *g_v[KVMAX];
static uint32_t g_vl[KVMAX];
static int g_kv_n;

static int kv_find(const char *k, uint32_t kl) {
    for (int i = 0; i < g_kv_n; i++)
        if (strlen(g_k[i]) == kl && memcmp(g_k[i], k, kl) == 0) return i;
    return -1;
}
uint32_t hal_kv_get(const char *k, uint32_t kl, char *v, uint32_t cap) {
    int i = kv_find(k, kl);
    if (i < 0) return 0;
    uint32_t n = g_vl[i] < cap ? g_vl[i] : cap;
    memcpy(v, g_v[i], n);
    return n;
}
uint32_t hal_kv_size(const char *k, uint32_t kl) {
    int i = kv_find(k, kl);
    return i < 0 ? 0 : g_vl[i];
}
int32_t hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl) {
    int i = kv_find(k, kl);
    if (i < 0) {
        if (g_kv_n == KVMAX) return -1;
        i = g_kv_n++;
        g_k[i] = strndup(k, kl);
    } else {
        free(g_v[i]);
    }
    g_v[i] = malloc(vl + 1);
    memcpy(g_v[i], v, vl);
    g_vl[i] = vl;
    return 0;
}
int32_t hal_kv_delete(const char *k, uint32_t kl) {
    int i = kv_find(k, kl);
    if (i < 0) return -1;
    free(g_k[i]);
    free(g_v[i]);
    g_k[i] = g_k[--g_kv_n];
    g_v[i] = g_v[g_kv_n];
    g_vl[i] = g_vl[g_kv_n];
    return 0;
}

/* ── Files: real ones, as the host's are ───────────────────────────── */

#define FMAX 8
static FILE *g_f[FMAX];
int32_t hal_file_open(const char *path, uint32_t pl, int32_t mode) {
    char p[1024];
    if (pl >= sizeof(p)) return -1;
    memcpy(p, path, pl);
    p[pl] = 0;
    for (int i = 0; i < FMAX; i++) {
        if (g_f[i]) continue;
        g_f[i] = fopen(p, mode == 0 ? "rb" : mode == 1 ? "wb" : "ab");
        return g_f[i] ? i : -1;
    }
    return -1;
}
int32_t hal_file_read(int32_t h, char *buf, uint32_t n) {
    return (h < 0 || h >= FMAX || !g_f[h]) ? -1
                                            : (int32_t)fread(buf, 1, n, g_f[h]);
}
int32_t hal_file_write(int32_t h, const char *buf, uint32_t n) {
    return (h < 0 || h >= FMAX || !g_f[h]) ? -1
                                            : (int32_t)fwrite(buf, 1, n, g_f[h]);
}
void hal_file_close(int32_t h) {
    if (h < 0 || h >= FMAX || !g_f[h]) return;
    fclose(g_f[h]);
    g_f[h] = 0;
}

/* ── HTTP: one canned service ──────────────────────────────────────── */

char mock_url[512], mock_headers[2048];
char *mock_body;
int mock_status = 200;
const char *mock_reply = "";
static int g_req_open;
static size_t g_reply_off;

int32_t hal_http_request2(int32_t method, const char *url, uint32_t ul,
                          const char *hdr, uint32_t hl, const char *body,
                          uint32_t bl, int32_t timeout_ms) {
    (void)method;
    (void)timeout_ms;
    snprintf(mock_url, sizeof(mock_url), "%.*s", (int)ul, url);
    snprintf(mock_headers, sizeof(mock_headers), "%.*s", (int)hl, hdr);
    free(mock_body);
    mock_body = strndup(body, bl);
    g_req_open = 1;
    g_reply_off = 0;
    return 7;
}
int32_t hal_http_poll(int32_t id) { return (id == 7 && g_req_open) ? 1 : -1; }
int32_t hal_http_status(int32_t id) { return id == 7 ? mock_status : -1; }
int32_t hal_http_read_response(int32_t id, char *buf, uint32_t cap) {
    if (id != 7) return 0;
    size_t left = strlen(mock_reply) - g_reply_off;
    size_t n = left < cap ? left : cap;
    memcpy(buf, mock_reply + g_reply_off, n);
    g_reply_off += n;
    return (int32_t)n;
}
void hal_http_free(int32_t id) {
    if (id == 7) g_req_open = 0;
}

/* ── The rest ──────────────────────────────────────────────────────── */

uint32_t hal_i18n_get(const char *k, uint32_t kl, char *o, uint32_t cap) {
    (void)k;
    (void)kl;
    (void)o;
    (void)cap;
    return 0; /* the code's English fallbacks */
}
uint64_t hal_time_ms(void) { return 1000; }
