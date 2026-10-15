/*
 * A native HAL for the Alerts wapp's tests: an in-memory KV, a capture of
 * everything hal_msg_send says and every wire hal_xprs_send airs, an
 * injectable event queue, a history answer the test sets, a clock and a
 * position the test moves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

const char *g_mock_call = "X1ME77";
uint32_t hal_identity(char *b, uint32_t cap) { uint32_t n = strlen(g_mock_call); if (n > cap) n = cap; memcpy(b, g_mock_call, n); return n; }
void hal_log(int32_t level, const char *m, uint32_t n) { (void)level; (void)m; (void)n; }

/* ── time and place ───────────────────────────────────────────────────── */
uint64_t g_now = 1786960000ULL;          /* 2026-08-17 */
uint64_t hal_time_epoch(void) { return g_now; }
int32_t g_offset = 120;                  /* CEST */
int32_t hal_time_utc_offset(void) { return g_offset; }
int32_t g_lat = (int32_t)0x80000000, g_lon = (int32_t)0x80000000;
int32_t hal_sensor_gps_lat(void) { return g_lat; }
int32_t hal_sensor_gps_lon(void) { return g_lon; }

/* ── kv ───────────────────────────────────────────────────────────────── */
static struct { char k[32]; char v[512]; } kv[16];
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap)
{
    for (int i = 0; i < 16; i++)
        if (strlen(kv[i].k) == kl && !memcmp(kv[i].k, k, kl)) {
            uint32_t n = strlen(kv[i].v);
            if (n > cap) n = cap;
            memcpy(out, kv[i].v, n);
            return n;
        }
    return 0;
}
int32_t hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl)
{
    int free_i = -1;
    for (int i = 0; i < 16; i++) {
        if (strlen(kv[i].k) == kl && !memcmp(kv[i].k, k, kl)) { free_i = i; break; }
        if (!kv[i].k[0] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return -1;
    snprintf(kv[free_i].k, sizeof kv[0].k, "%.*s", (int)kl, k);
    snprintf(kv[free_i].v, sizeof kv[0].v, "%.*s", (int)vl, v);
    return 0;
}
void mock_kv_clear(void) { memset(kv, 0, sizeof kv); }

/* ── what the wapp says and airs ──────────────────────────────────────── */
char g_said[256][2048];
int  g_said_n;
void hal_msg_send(const char *j, uint32_t n)
{
    if (g_said_n < 256) snprintf(g_said[g_said_n++], sizeof g_said[0], "%.*s", (int)n, j);
}
char g_aired[16][300];
int  g_aired_n;
int32_t g_send_rc;
int32_t hal_xprs_send(const char *w, uint32_t n)
{
    if (g_send_rc) return g_send_rc;
    if (g_aired_n < 16) snprintf(g_aired[g_aired_n++], sizeof g_aired[0], "%.*s", (int)n, w);
    return 0;
}

/* ── commands in, events in ───────────────────────────────────────────── */
char g_cmd[4096];
uint32_t hal_msg_recv(char *b, uint32_t cap)
{
    uint32_t n = strlen(g_cmd);
    if (!n) return 0;
    if (n > cap) n = cap;
    memcpy(b, g_cmd, n);
    g_cmd[0] = 0;
    return n;
}
static struct { char t[64]; char e[2048]; } evq[16];
static int evn, evr;
char g_subs[8][64];
int  g_subs_n;
int32_t hal_event_subscribe(const char *t, uint32_t n)
{
    if (g_subs_n < 8) snprintf(g_subs[g_subs_n++], 64, "%.*s", (int)n, t);
    return 0;
}
int32_t hal_event_available(void) { return evr < evn; }
uint32_t hal_event_recv(char *t, uint32_t tc, char *e, uint32_t ec)
{
    if (evr >= evn) return 0;
    snprintf(t, tc, "%s", evq[evr].t);
    uint32_t n = strlen(evq[evr].e);
    if (n > ec) n = ec;
    memcpy(e, evq[evr].e, n);
    evr++;
    return n;
}
void mock_event(const char *topic, const char *json)
{
    if (evn >= 16) { evn = evr = 0; }
    snprintf(evq[evn].t, 64, "%s", topic);
    snprintf(evq[evn].e, 2048, "%s", json);
    evn++;
}

/* ── the archive ──────────────────────────────────────────────────────── */
char g_history[8192] = "[]";
int g_history_calls;
int32_t hal_xprs_history(const char *q, uint32_t ql, char *out, uint32_t cap)
{
    (void)q; (void)ql;
    g_history_calls++;
    uint32_t n = strlen(g_history);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_history, n);
    return (int32_t)n;
}

void mock_reset(void)
{
    g_said_n = g_aired_n = 0;
    g_send_rc = 0;
    g_cmd[0] = 0;
    evn = evr = 0;
    g_subs_n = 0;
    strcpy(g_history, "[]");
}
