/*
 * A native HAL for the Firmwares wapp's tests: an in-memory KV, a capture
 * of everything hal_msg_send says and every wire hal_xprs_send airs, an
 * injectable event queue, a clock the test moves, and a hal_encrypt that
 * records what it was asked to seal and answers with ciphertext of the
 * right length and no plaintext in it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── identity ─────────────────────────────────────────────────────────── */
const char *g_mock_call = "X1ME77";
const char *g_mock_npub = "npub1me77qpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jqpz";
uint32_t hal_identity(char *b, uint32_t cap) { uint32_t n = strlen(g_mock_call); if (n > cap) n = cap; memcpy(b, g_mock_call, n); return n; }
uint32_t hal_identity_pubkey(char *b, uint32_t cap) { uint32_t n = strlen(g_mock_npub); if (n > cap) n = cap; memcpy(b, g_mock_npub, n); return n; }
uint32_t hal_npub(const char *pk, uint32_t pl, char *o, uint32_t cap) { (void)pk; (void)pl; (void)o; (void)cap; return 0; }

/* ── sealing ──────────────────────────────────────────────────────────── */
char g_sealed_plain[16][256];
char g_sealed_to[16][80];
int  g_sealed_n;
uint32_t hal_encrypt(const char *pk, uint32_t pl, const char *m, uint32_t ml,
                     char *out, uint32_t cap)
{
    if (g_sealed_n < 16) {
        snprintf(g_sealed_to[g_sealed_n], 80, "%.*s", (int)pl, pk);
        snprintf(g_sealed_plain[g_sealed_n], 256, "%.*s", (int)ml, m);
        g_sealed_n++;
    }
    /* XPRS.md 6.2's length: 16 + 16 * (floor(P/16) + 1) bytes, base64url. */
    uint32_t raw = 16 + 16 * (ml / 16 + 1);
    uint32_t chars = (raw * 4 + 2) / 3;
    if (chars > cap) return 0;
    for (uint32_t i = 0; i < chars; i++) out[i] = "Qk7w"[i % 4];
    return chars;
}

/* ── what the core holds about one station ────────────────────────────── */
char g_station_json[1024];      /* what hal_xprs_station answers; empty = not heard */
int32_t hal_xprs_station(const char *call, uint32_t cl, char *out, uint32_t cap)
{
    (void)call; (void)cl;
    uint32_t n = strlen(g_station_json);
    if (!n) return 0;
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_station_json, n);
    return (int32_t)n;
}

/* ── the air ──────────────────────────────────────────────────────────── */
char g_aired[64][260];
int  g_aired_n;
int  g_send_rc;
int32_t hal_xprs_send(const char *w, uint32_t n)
{
    if (g_send_rc) return g_send_rc;
    if (g_aired_n >= 64) { memmove(g_aired[0], g_aired[1], sizeof g_aired - sizeof g_aired[0]); g_aired_n = 63; }
    snprintf(g_aired[g_aired_n++], 260, "%.*s", (int)n, w);
    return 0;
}

/* ── KV ───────────────────────────────────────────────────────────────── */
#define KVN 64
static char g_kk[KVN][64], g_kv[KVN][256];
static int g_kvn;
uint32_t hal_kv_get(const char *k, uint32_t kl, char *o, uint32_t cap)
{
    for (int i = 0; i < g_kvn; i++)
        if (strlen(g_kk[i]) == kl && !memcmp(g_kk[i], k, kl)) {
            uint32_t n = strlen(g_kv[i]); if (n > cap) n = cap;
            memcpy(o, g_kv[i], n); return n;
        }
    return 0;
}
int32_t hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl)
{
    for (int i = 0; i < g_kvn; i++)
        if (strlen(g_kk[i]) == kl && !memcmp(g_kk[i], k, kl)) {
            snprintf(g_kv[i], 256, "%.*s", (int)vl, v); return 0;
        }
    if (g_kvn >= KVN) return -1;
    snprintf(g_kk[g_kvn], 64, "%.*s", (int)kl, k);
    snprintf(g_kv[g_kvn], 256, "%.*s", (int)vl, v);
    g_kvn++;
    return 0;
}
const char *kv_dump(void)
{
    static char all[KVN * 330];
    all[0] = 0;
    for (int i = 0; i < g_kvn; i++) { strcat(all, g_kk[i]); strcat(all, "="); strcat(all, g_kv[i]); strcat(all, "\n"); }
    return all;
}

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
uint32_t hal_msg_available(void) { return g_inbox_set ? strlen(g_inbox) : 0; }
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
    for (int i = 0; i < g_subn; i++)
        if (strlen(g_subs[i]) == n && !memcmp(g_subs[i], t, n)) {
            g_subs[i][0] = 0; memmove(g_subs[i], g_subs[g_subn - 1], 40); g_subn--; return 0;
        }
    return -1;
}
int subscribed(const char *t) { for (int i = 0; i < g_subn; i++) if (!strcmp(g_subs[i], t)) return 1; return 0; }
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

/* ── time ─────────────────────────────────────────────────────────────── */
uint64_t g_ms = 1000000;
uint64_t g_epoch = 1789000000;   /* 2026-09-10 */
uint64_t hal_time_ms(void) { return g_ms; }
uint64_t hal_time_epoch(void) { return g_epoch; }
void hal_log(int32_t l, const char *m, uint32_t n) { (void)l; (void)m; (void)n; }
int32_t hal_ui_attached(void) { return 1; }

/* ── flashing over USB ────────────────────────────────────────────────── */
char g_flash_json[8192];        /* what hal_flash_state answers */
char g_flash_calls[16][160];    /* every verb, in order */
int  g_flash_calln;
int  g_flash_rc = 1;            /* what the verbs answer */
static void flash_call(const char *s) { if (g_flash_calln < 16) snprintf(g_flash_calls[g_flash_calln++], 160, "%s", s); }
int32_t hal_flash_scan(void) { flash_call("scan"); return g_flash_rc; }
int32_t hal_flash_probe(const char *d, uint32_t dl)
{
    char s[160]; snprintf(s, sizeof s, "probe %.*s", (int)dl, d); flash_call(s); return g_flash_rc;
}
int32_t hal_flash_fetch(const char *b, uint32_t bl)
{
    char s[160]; snprintf(s, sizeof s, "fetch %.*s", (int)bl, b); flash_call(s); return g_flash_rc;
}
int32_t hal_flash_write(const char *d, uint32_t dl, const char *b, uint32_t bl, int32_t wipe)
{
    char s[160]; snprintf(s, sizeof s, "write %.*s %.*s %d", (int)dl, d, (int)bl, b, (int)wipe); flash_call(s); return g_flash_rc;
}
int32_t hal_flash_cancel(void) { flash_call("cancel"); return 1; }
int32_t hal_flash_state(char *out, uint32_t cap)
{
    uint32_t n = strlen(g_flash_json);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_flash_json, n);
    return (int32_t)n;
}
