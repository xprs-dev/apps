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

/* ── time, log, page ──────────────────────────────────────────────────── */
uint64_t g_ms = 1000000;
uint64_t g_epoch = 1789000000;   /* 2026-09-10 */
uint64_t hal_time_ms(void) { return g_ms; }
uint64_t hal_time_epoch(void) { return g_epoch; }
void hal_log(int32_t l, const char *m, uint32_t n) { (void)l; (void)m; (void)n; }
int g_ui_attached = 1;
int32_t hal_ui_attached(void) { return g_ui_attached; }
