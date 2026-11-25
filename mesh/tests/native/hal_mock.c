/*
 * Native mock HAL for the mesh wapp test. Provides just the HAL the wapp
 * uses: log, kv, msg (with an injectable inbox + an outbox capture), and the
 * three read-only Reticulum calls returning canned JSON. Compiled together with
 * main.c by run.sh — no wasm, no host.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── outbox capture (hal_msg_send) ─────────────────────────────────────── */
#define CAP_MAX 4096
static char *g_cap[CAP_MAX];
static int   g_capn = 0;
void cap_clear(void) { for (int i = 0; i < g_capn; i++) free(g_cap[i]); g_capn = 0; }
int  cap_count(void) { return g_capn; }
const char *cap_at(int i) { return (i >= 0 && i < g_capn) ? g_cap[i] : ""; }
int  cap_contains(const char *s) { for (int i = 0; i < g_capn; i++) if (strstr(g_cap[i], s)) return 1; return 0; }

void hal_msg_send(const char *json, uint32_t len) {
    if (g_capn >= CAP_MAX) return;
    char *c = (char *)malloc(len + 1);
    memcpy(c, json, len); c[len] = '\0';
    g_cap[g_capn++] = c;
}

/* ── injectable inbox (hal_msg_recv) ───────────────────────────────────── */
static char g_inbox[2048];
static int  g_inbox_set = 0;
void inbox_set(const char *s) {
    strncpy(g_inbox, s, sizeof(g_inbox) - 1);
    g_inbox[sizeof(g_inbox) - 1] = '\0';
    g_inbox_set = 1;
}
uint32_t hal_msg_available(void) { return g_inbox_set ? (uint32_t)strlen(g_inbox) : 0; }
uint32_t hal_msg_recv(char *buf, uint32_t cap) {
    if (!g_inbox_set) return 0;
    uint32_t n = strlen(g_inbox); if (n > cap) n = cap;
    memcpy(buf, g_inbox, n); g_inbox_set = 0; return n;
}

void hal_log(int32_t lvl, const char *m, uint32_t n) { (void)lvl; (void)m; (void)n; }

/* ── kv store ──────────────────────────────────────────────────────────── */
#define KV_MAX 16
static char g_kvk[KV_MAX][32];
static char g_kvv[KV_MAX][128];
static int  g_kvn = 0;
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap) {
    (void)kl;
    for (int i = 0; i < g_kvn; i++) {
        if (strcmp(g_kvk[i], k) == 0) {
            uint32_t n = strlen(g_kvv[i]); if (n > cap) n = cap;
            memcpy(out, g_kvv[i], n); out[n] = '\0'; return n;
        }
    }
    return 0;
}
void hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl) {
    (void)kl;
    for (int i = 0; i < g_kvn; i++) {
        if (strcmp(g_kvk[i], k) == 0) {
            uint32_t n = vl < 127 ? vl : 127; memcpy(g_kvv[i], v, n); g_kvv[i][n] = '\0'; return;
        }
    }
    if (g_kvn >= KV_MAX) return;
    strncpy(g_kvk[g_kvn], k, 31);
    uint32_t n = vl < 127 ? vl : 127; memcpy(g_kvv[g_kvn], v, n); g_kvv[g_kvn][n] = '\0';
    g_kvn++;
}

/* ── canned Reticulum data (the mesh wapp reads it verbatim) ─────────────────────────────────────────────── */
static char g_last_filter[256] = "";
const char *last_filter(void) { return g_last_filter; }

int32_t hal_rns_status(char *out, uint32_t cap) {
    const char *s = "{\"up\":true,\"mode\":\"tcpclient\",\"paths\":12,"
                    "\"passive\":false,\"observed\":3}";
    uint32_t n = strlen(s); if (n > cap) return -(int32_t)n;
    memcpy(out, s, n); return (int32_t)n;
}

int32_t hal_rns_hubs(char *out, uint32_t cap) {
    const char *s = "[{\"endpoint\":\"rns.beleth.net:4242\",\"connected\":true},"
                    "{\"endpoint\":\"offline.example:4242\",\"connected\":false}]";
    uint32_t n = strlen(s); if (n > cap) return -(int32_t)n;
    memcpy(out, s, n); return (int32_t)n;
}

/* ── the core event bus ────────────────────────────────────────────────────
 *
 * Absent until now, while module_init() has called hal_event_subscribe since
 * the wapp stopped polling — so this harness did not link and every scenario
 * below was dead code. The wapp redraws on an event and on nothing else, which
 * makes these three the most important mocks in the file. */
#define EVQ_MAX 16
static char g_ev[EVQ_MAX][48];
static int  g_evn = 0;
static char g_subs[8][48];
static int  g_subn = 0;

void event_push(const char* topic) {
    if (g_evn < EVQ_MAX) snprintf(g_ev[g_evn++], 48, "%s", topic);
}
int  event_subscribed(const char* topic) {
    for (int i = 0; i < g_subn; i++)
        if (!strcmp(g_subs[i], topic)) return 1;
    return 0;
}

int32_t hal_event_subscribe(const char* topic, uint32_t len) {
    if (g_subn < 8) {
        int n = (int)len < 47 ? (int)len : 47;
        memcpy(g_subs[g_subn], topic, n);
        g_subs[g_subn][n] = 0;
        g_subn++;
    }
    return 0;
}
int32_t hal_event_available(void) { return g_evn; }
int32_t hal_event_recv(char* topic, uint32_t tcap, char* data, uint32_t dcap) {
    if (g_evn <= 0) return 0;
    int n = (int)strlen(g_ev[0]);
    if (n > (int)tcap) n = (int)tcap;
    memcpy(topic, g_ev[0], n);
    if (dcap > 2) { data[0] = '{'; data[1] = '}'; }
    for (int i = 1; i < g_evn; i++) memcpy(g_ev[i - 1], g_ev[i], 48);
    g_evn--;
    return n;
}

int32_t hal_rns_nodes(const char *filter, uint32_t filter_len, char *out, uint32_t cap) {
    uint32_t fn = filter_len < sizeof(g_last_filter) - 1 ? filter_len : sizeof(g_last_filter) - 1;
    memcpy(g_last_filter, filter, fn); g_last_filter[fn] = '\0';
    const char *s =
      "{\"nodes\":["
        "{\"id\":\"self\",\"label\":\"me\",\"kind\":\"self\",\"services\":[],\"xprs\":true,\"hops\":0,\"via\":\"\",\"relayer\":\"\",\"meta\":{}},"
        "{\"id\":\"hubA\",\"label\":\"hubA\",\"kind\":\"hub\",\"services\":[],\"xprs\":false,\"hops\":1,\"via\":\"tcp\",\"relayer\":\"\",\"meta\":{\"children\":1}},"
        "{\"id\":\"leaf1\",\"label\":\"AB1CD\",\"kind\":\"leaf\",\"services\":[\"chat\",\"files\"],\"xprs\":true,\"hops\":2,\"via\":\"tcp\",\"relayer\":\"hubA\",\"meta\":{\"callsign\":\"AB1CD\"}}"
      "],\"edges\":["
        "{\"from\":\"self\",\"to\":\"hubA\",\"kind\":\"uplink\"},"
        "{\"from\":\"hubA\",\"to\":\"leaf1\",\"kind\":\"relay\"}"
      "],\"sample\":true,\"observed\":3}";
    uint32_t n = strlen(s); if (n > cap) return -(int32_t)n;
    memcpy(out, s, n); return (int32_t)n;
}

/* ── hal_station_read: canned answers per `get`, and the last request ───── */
char g_station_req[8][256];
int  g_station_reqn = 0;
const char *g_offers =
  "{\"state\":\"ready\",\"busy\":false,\"call\":\"X3PTZM\",\"serve\":[],\"tabs\":[\"chat\"],\"known\":true,"
  "\"rooms\":[{\"call\":\"X562EL\",\"name\":\"Landscapes\",\"about\":\"photos\",\"kind\":\"gallery\",\"private\":false,\"role\":\"none\",\"can_post\":false,\"members\":0},"
  "{\"call\":\"X5VK3U\",\"name\":\"Family\",\"about\":\"\",\"kind\":\"music\",\"private\":true,\"role\":\"member\",\"can_post\":false,\"members\":3}]}";
int g_host_serving = 0;
char g_set_pref[64];
int32_t hal_xprs_set_pref(const char *kv, uint32_t n) {
    if (n >= sizeof g_set_pref) n = sizeof g_set_pref - 1;
    memcpy(g_set_pref, kv, n); g_set_pref[n] = 0;
    g_host_serving = strstr(g_set_pref, "=1") != 0;
    return 0;
}
const char *g_own_offers =
  "{\"state\":\"ready\",\"busy\":false,\"call\":\"X1MINE\",\"known\":true,"
  "\"rooms\":[{\"call\":\"X5OWNR\",\"name\":\"Photos\",\"about\":\"\",\"kind\":\"gallery\",\"private\":true,\"role\":\"admin\",\"can_post\":true,\"members\":1,\"people\":[{\"call\":\"X1PAL\"}]}]}";
int32_t hal_station_read(const char *req, uint32_t len, char *out, uint32_t cap) {
    char r[256];
    uint32_t n = len < sizeof r - 1 ? len : sizeof r - 1;
    memcpy(r, req, n); r[n] = 0;
    if (g_station_reqn < 8) strcpy(g_station_req[g_station_reqn++], r);
    const char *a = "{\"state\":\"loading\",\"busy\":true}";
    if (strstr(r, "\"get\":\"host\"")) {
        g_station_reqn--;                 /* asked on every draw: not a request the checks count */
        a = g_host_serving ? "{\"call\":\"X1MINE\",\"serving\":true,\"share\":{\"busy\":false,\"done\":0,\"total\":0,\"failed\":0}}"
                           : "{\"call\":\"X1MINE\",\"serving\":false,\"share\":{\"busy\":false,\"done\":0,\"total\":0,\"failed\":0}}";
    } else if (strstr(r, "\"get\":\"list\"")) {
        if (strstr(r, "\"scope\":\"all\"") && strstr(r, "\"offset\":0"))
            a = "{\"counts\":{\"local\":2,\"regional\":0,\"global\":1,\"all\":3},\"scope\":\"all\",\"total\":3,\"offset\":0,"
                "\"rows\":[{\"call\":\"X3PTZM\",\"kind\":\"station\",\"reach\":\"local\",\"bearers\":[\"lan\",\"ble\"],\"age_s\":12,\"api\":true,\"dist_m\":420},"
                "{\"call\":\"X1NEAR\",\"kind\":\"user\",\"reach\":\"local\",\"bearers\":[\"ble\"],\"age_s\":200,\"api\":false},"
                "{\"call\":\"X1FAR\",\"kind\":\"user\",\"reach\":\"global\",\"bearers\":[\"rns\"],\"age_s\":7200,\"api\":false,\"hops\":4}]}";
        else if (strstr(r, "\"scope\":\"all\""))
            a = "{\"counts\":{},\"scope\":\"all\",\"total\":3,\"offset\":60,\"rows\":[]}";
        else if (strstr(r, "\"scope\":\"local\""))
            a = "{\"counts\":{\"local\":2,\"regional\":0,\"global\":300,\"all\":302},\"scope\":\"local\",\"total\":2,\"offset\":0,"
                "\"rows\":[{\"call\":\"X3PTZM\",\"kind\":\"station\",\"reach\":\"local\",\"bearers\":[\"lan\",\"ble\"],\"age_s\":12,\"api\":true,\"dist_m\":420},"
                "{\"call\":\"X1NEAR\",\"kind\":\"user\",\"reach\":\"local\",\"bearers\":[\"ble\"],\"age_s\":200,\"api\":false}]}";
        else if (strstr(r, "\"scope\":\"global\"") && strstr(r, "\"offset\":0"))
            a = "{\"counts\":{\"local\":2,\"regional\":0,\"global\":300,\"all\":302},\"scope\":\"global\",\"total\":300,\"offset\":0,"
                "\"rows\":[{\"call\":\"X1FAR\",\"kind\":\"user\",\"reach\":\"global\",\"bearers\":[\"rns\"],\"age_s\":7200,\"api\":false,\"hops\":4}]}";
        else if (strstr(r, "\"scope\":\"global\""))
            a = "{\"counts\":{\"local\":2,\"regional\":0,\"global\":300,\"all\":302},\"scope\":\"global\",\"total\":300,\"offset\":60,"
                "\"rows\":[{\"call\":\"X1PAGE2\",\"kind\":\"user\",\"reach\":\"global\",\"bearers\":[\"rns\"],\"age_s\":9,\"api\":false}]}";
        else
            a = "{\"counts\":{\"local\":2,\"regional\":0,\"global\":300,\"all\":302},\"scope\":\"regional\",\"total\":0,\"offset\":0,\"rows\":[]}";
    } else if (strstr(r, "\"get\":\"ways\"")) {
        a = "{\"call\":\"X3PTZM\",\"self\":\"X1ME\",\"busy\":false,\"routes\":[{\"sure\":true,\"hops\":[{\"from\":\"X1ME\",\"to\":\"X3PTZM\",\"bearer\":\"lan\",\"reach\":\"local\",\"know\":\"heard\",\"age_s\":3}]}]}";
    } else if (strstr(r, "\"get\":\"offers\"") && strstr(r, "X1MINE")) {
        a = g_own_offers;
    } else if (strstr(r, "\"get\":\"offers\"")) {
        a = g_offers;
    } else if (strstr(r, "\"get\":\"folders\"")) {
        a = "{\"state\":\"ready\",\"busy\":false,\"folders\":[{\"id\":\"ab12cd\",\"title\":\"coast-and-city\",\"count\":2,\"cover\":\"\"}]}";
    } else if (strstr(r, "\"get\":\"items\"")) {
        a = "{\"state\":\"ready\",\"busy\":false,\"total\":2,\"offset\":0,\"items\":["
            "{\"id\":\"i1\",\"caption\":\"the mast\",\"name\":\"mast.jpg\",\"size\":\"2.1MB\",\"from\":\"X1A\",\"ts\":1,\"likes\":0,\"liked\":false,"
            "\"preview\":\"file:nYxKzGm4vT2pQ8dW5jR7cL0aFbNs9hUe3oXiC6EkM1w.jpg\",\"file\":\"file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.jpg\"},"
            "{\"id\":\"i2\",\"caption\":\"\",\"name\":\"b.jpg\",\"size\":\"1MB\",\"from\":\"X1A\",\"ts\":2,\"likes\":0,\"liked\":false,"
            "\"preview\":\"\",\"file\":\"file:Zc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.jpg\"}]}";
    }
    uint32_t l = (uint32_t)strlen(a);
    if (l > cap) return -(int32_t)l;
    memcpy(out, a, l);
    return (int32_t)l;
}

/* ── the stations map (sdk/wapp_stations.c) ───────────────────────────── */
/* hal.db is not mocked: sdk/hal_db_zx.c runs it on zx, the host's engine. */
unsigned long long g_now = 1795600000ULL;
uint64_t hal_time_epoch(void) { return g_now; }
int32_t hal_time_utc_offset(void) { return 0; }
char g_followed_kind[64];
int32_t hal_xprs_follow_kind(const char *k, uint32_t kl, int32_t on) {
    if (on && kl < 20 && strlen(g_followed_kind) + kl + 2 < sizeof g_followed_kind) {
        strncat(g_followed_kind, k, kl); strcat(g_followed_kind, ",");
    }
    return 0;
}
/* What the archive holds of identities and services (the walk stops on a
 * short page). */
char g_archive[16384] = "[]";
int g_history_reads;
int32_t hal_xprs_history(const char *q, uint32_t ql, char *out, uint32_t cap) {
    (void)q; (void)ql;
    g_history_reads++;
    uint32_t n = (uint32_t)strlen(g_archive);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_archive, n);
    return (int32_t)n;
}
char g_names[2048] = "[]";
char g_names_q[256];
int32_t hal_xprs_names(const char *q, uint32_t ql, char *out, uint32_t cap) {
    uint32_t m = ql < sizeof g_names_q - 1 ? ql : sizeof g_names_q - 1;
    memcpy(g_names_q, q, m); g_names_q[m] = 0;
    uint32_t n = (uint32_t)strlen(g_names);
    if (n > cap) return -(int32_t)n;
    memcpy(out, g_names, n);
    return (int32_t)n;
}
int32_t hal_xprs_position(char *out, uint32_t cap) {
    const char *a = "{\"pos\":\"38.7077,-9.1365\",\"acc\":10}";
    uint32_t n = (uint32_t)strlen(a);
    if (n > cap) return -(int32_t)n;
    memcpy(out, a, n);
    return (int32_t)n;
}
