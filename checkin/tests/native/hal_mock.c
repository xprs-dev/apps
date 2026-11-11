/*
 * A native HAL for the Check-in wapp's tests: an in-memory KV, captures of
 * what the wapp says, airs, messages and fetches, an event queue, a station
 * list, an archive answer, a clock and a position the test moves. The
 * callsign is switchable, so one process plays the station and the chaser.
 * hal_db_* is the real database: sdk/hal_db_zx.c, a zx store, as in the app.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

char g_mock_call[16] = "X1CHSR";
uint32_t hal_identity(char *b, uint32_t cap) { uint32_t n = strlen(g_mock_call); if (n > cap) n = cap; memcpy(b, g_mock_call, n); return n; }
void hal_log(int32_t level, const char *m, uint32_t n) { (void)level; (void)m; (void)n; }

uint64_t g_now = 1786960000ULL;          /* 2026-08-17_09:46:40 */
uint64_t hal_time_epoch(void) { return g_now; }
int32_t g_offset = 60;
int32_t hal_time_utc_offset(void) { return g_offset; }
int32_t g_lat = (int32_t)0x80000000, g_lon = (int32_t)0x80000000;
int32_t hal_sensor_gps_lat(void) { return g_lat; }
int32_t hal_sensor_gps_lon(void) { return g_lon; }

/* ── kv ── */
static struct { char k[32]; char v[512]; } kv[32];
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap)
{
    for (int i = 0; i < 32; i++)
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
    int f = -1;
    for (int i = 0; i < 32; i++) {
        if (strlen(kv[i].k) == kl && !memcmp(kv[i].k, k, kl)) { f = i; break; }
        if (!kv[i].k[0] && f < 0) f = i;
    }
    if (f < 0) return -1;
    snprintf(kv[f].k, sizeof kv[0].k, "%.*s", (int)kl, k);
    snprintf(kv[f].v, sizeof kv[0].v, "%.*s", (int)vl, v);
    return 0;
}

/* ── what the wapp says, airs, messages, fetches ── */
char g_said[512][4096];
int  g_said_n;
void hal_msg_send(const char *j, uint32_t n)
{
    if (g_said_n < 512) snprintf(g_said[g_said_n++], sizeof g_said[0], "%.*s", (int)n, j);
}
char g_aired[64][300];
int  g_aired_n;
int32_t g_send_rc;
int32_t hal_xprs_send(const char *w, uint32_t n)
{
    if (g_send_rc) return g_send_rc;
    if (g_aired_n < 64) snprintf(g_aired[g_aired_n++], sizeof g_aired[0], "%.*s", (int)n, w);
    return 0;
}
char g_msg_to[16], g_msg_text[200];
int g_msg_n;
int32_t hal_xprs_message(const char *to, uint32_t tl, const char *t, uint32_t xl,
                         uint32_t priv, char *id, uint32_t cap)
{
    (void)priv;
    snprintf(g_msg_to, sizeof g_msg_to, "%.*s", (int)tl, to);
    snprintf(g_msg_text, sizeof g_msg_text, "%.*s", (int)xl, t);
    g_msg_n++;
    if (cap >= 7) memcpy(id, "abcdef", 7);
    return 2;
}
char g_fetched[16][96];
int g_fetched_n;
uint32_t hal_media_fetch(const char *t, uint32_t n)
{
    if (g_fetched_n < 16) snprintf(g_fetched[g_fetched_n++], 96, "%.*s", (int)n, t);
    return 1;
}
char g_put_token[96] = "file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.png";
uint32_t hal_media_put_file(const char *p, uint32_t pl, char *out, uint32_t cap)
{
    (void)p; (void)pl;
    uint32_t n = strlen(g_put_token);
    if (n > cap) return 0;
    memcpy(out, g_put_token, n);
    return n;
}
uint32_t hal_media_meta(const char *h, uint32_t hl, char *out, uint32_t cap)
{
    (void)h; (void)hl;
    const char *m = "{\"size\":640,\"ext\":\"png\"}";
    uint32_t n = strlen(m);
    if (n > cap) return 0;
    memcpy(out, m, n);
    return n;
}

/* ── commands in, events in ── */
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
uint32_t hal_msg_available(void) { return g_cmd[0] != 0; }
static struct { char t[64]; char e[4096]; } evq[32];
static int evn, evr;
char g_subs[16][64];
int32_t hal_event_subscribe(const char *t, uint32_t n)
{
    for (int i = 0; i < 16; i++) if (!g_subs[i][0]) { snprintf(g_subs[i], 64, "%.*s", (int)n, t); return 0; }
    return -1;
}
int32_t hal_event_unsubscribe(const char *t, uint32_t n)
{
    for (int i = 0; i < 16; i++)
        if (strlen(g_subs[i]) == n && !memcmp(g_subs[i], t, n)) { g_subs[i][0] = 0; return 0; }
    return -1;
}
int mock_subscribed(const char *t)
{
    for (int i = 0; i < 16; i++) if (!strcmp(g_subs[i], t)) return 1;
    return 0;
}
uint32_t hal_event_available(void) { return evr < evn; }
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
    if (evn >= 32) { evn = evr = 0; }
    snprintf(evq[evn].t, 64, "%s", topic);
    snprintf(evq[evn].e, 4096, "%s", json);
    evn++;
}

/* ── stations and the archive ── */
char g_stations[4096] = "[{\"title\":\"Heard over the air (0)\",\"items\":[]}]";
int32_t hal_xprs_stations(char *out, uint32_t cap)
{
    uint32_t n = strlen(g_stations);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_stations, n);
    return (int32_t)n;
}
/* The owner's shared position, as the core hands it over (empty: none). */
char g_shared[96] = "";
int32_t hal_xprs_position(char *out, uint32_t cap)
{
    uint32_t n = strlen(g_shared);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_shared, n);
    return (int32_t)n;
}
char g_history[8192] = "[]";
int32_t hal_xprs_history(const char *q, uint32_t ql, char *out, uint32_t cap)
{
    (void)q; (void)ql;
    uint32_t n = strlen(g_history);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_history, n);
    return (int32_t)n;
}

/* hal.db is not mocked: sdk/hal_db_zx.c runs it on zx, the host's engine. */

void mock_clear(void)
{
    g_said_n = g_aired_n = 0;
    g_send_rc = 0;
    g_cmd[0] = 0;
    evn = evr = 0;
    g_fetched_n = 0;
    g_msg_n = 0;
}
