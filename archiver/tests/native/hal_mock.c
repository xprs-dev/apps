/*
 * Native mock HAL for the archiver wapp test. Canned status JSON for the
 * three read verbs, an injectable inbox, an outbox capture, and a record of
 * every set_pref the wapp writes — which is what the assertions are about:
 * the screen must send the operator's choice to the core and nothing else.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── outbox capture (hal_msg_send) ─────────────────────────────────────── */
#define CAP_MAX 64
static char *g_cap[CAP_MAX];
static int   g_capn = 0;
void cap_clear(void) { for (int i = 0; i < g_capn; i++) free(g_cap[i]); g_capn = 0; }
int  cap_count(void) { return g_capn; }
int  cap_contains(const char *s) {
    for (int i = 0; i < g_capn; i++) if (strstr(g_cap[i], s)) return 1;
    return 0;
}
void hal_msg_send(const char *json, uint32_t len) {
    if (g_capn >= CAP_MAX) return;
    char *c = (char *)malloc(len + 1);
    memcpy(c, json, len); c[len] = '\0';
    g_cap[g_capn++] = c;
}

/* ── injectable inbox (hal_msg_recv) ───────────────────────────────────── */
static char g_inbox[2048];
static int  g_inbox_set = 0;
void inbox_set(const char *json) {
    snprintf(g_inbox, sizeof(g_inbox), "%s", json);
    g_inbox_set = 1;
}
int32_t hal_msg_available(void) { return g_inbox_set; }
int32_t hal_msg_recv(char *out, uint32_t cap) {
    if (!g_inbox_set) return 0;
    int n = (int)strlen(g_inbox);
    if (n > (int)cap) n = (int)cap;
    memcpy(out, g_inbox, n);
    g_inbox_set = 0;
    return n;
}

/* ── what the wapp wrote to the core ───────────────────────────────────── */
#define PREF_MAX 32
static char g_prefs[PREF_MAX][64];
static int  g_prefn = 0;
void prefs_clear(void) { g_prefn = 0; }
int  pref_written(const char *kv) {
    for (int i = 0; i < g_prefn; i++) if (!strcmp(g_prefs[i], kv)) return 1;
    return 0;
}
int  prefs_count(void) { return g_prefn; }
static void note_pref(const char *kv, uint32_t len) {
    if (g_prefn >= PREF_MAX) return;
    int n = (int)len < 63 ? (int)len : 63;
    memcpy(g_prefs[g_prefn], kv, n);
    g_prefs[g_prefn][n] = '\0';
    g_prefn++;
}
int32_t hal_xprs_set_pref(const char *kv, uint32_t len) { note_pref(kv, len); return 0; }
int32_t hal_archive_set_pref(const char *kv, uint32_t len) { note_pref(kv, len); return 0; }
int32_t hal_node_set_pref(const char *kv, uint32_t len) { note_pref(kv, len); return 0; }

/* ── the packet archive, settable per scenario ─────────────────────────── */
static int g_public = 0, g_always = 0;
void archive_set_public(int on) { g_public = on; }
void archive_set_always(int on) { g_always = on; }

int32_t hal_xprs_archive(char *out, uint32_t cap) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\"public\":%s,\"alwaysOn\":%s,\"alwaysOnStored\":%s,"
        "\"keepFollowed\":true,\"keepChatter\":false,"
        "\"quotaMb\":500,\"maxDays\":365,"
        "\"records\":{\"own\":154654,\"followed\":7,\"stranger\":1274,\"total\":155935},"
        "\"bytes\":4096,\"bytesText\":\"4.0 kB\",\"quotaText\":\"500.0 MB\","
        "\"fullFrac\":0.01,\"followedCallsigns\":2,"
        "\"asksLastHour\":4,\"answered\":3,\"refused\":1,"
        "\"announced\":\"%s\",\"named\":[\"X3ARC1\"]}",
        g_public ? "true" : "false",
        (g_public && g_always) ? "true" : "false",
        g_always ? "true" : "false",
        g_public ? "archive" : "");
    if (n > (int)cap) return -n;
    memcpy(out, buf, n);
    return n;
}

/* ── the file store and the directory: canned ──────────────────────────── */
int32_t hal_archive_status(char *out, uint32_t cap) {
    const char *j = "{\"quotaGb\":0,\"archiving\":false,\"usedBytes\":0,"
        "\"items\":0,\"usedText\":\"0 B\",\"quotaText\":\"off\",\"fullFrac\":0,"
        "\"servedItems\":0,\"reqLastHour\":0,\"reqAvgPerHour\":0,\"reqSpark\":[],"
        "\"bwLastHourText\":\"0 B\",\"bwPerHourText\":\"0 B\",\"bwSpark\":[],"
        "\"freeableBytes\":0,\"freeableText\":\"0 B\",\"followed\":true,"
        "\"fromNearby\":true,\"mirrorSmall\":true}";
    int n = (int)strlen(j);
    if (n > (int)cap) return -n;
    memcpy(out, j, n);
    return n;
}
int32_t hal_archive_items(char *out, uint32_t cap) { (void)out; (void)cap; return 0; }
int32_t hal_archive_drop(const char *id, uint32_t len) { (void)id; (void)len; return 0; }

int32_t hal_node_status(char *out, uint32_t cap) {
    const char *j = "{\"volunteer\":\"auto\",\"serving\":true,\"pointers\":12,"
        "\"authors\":0,\"syncPeers\":0,\"demoted\":0,\"queriesLastHour\":0,"
        "\"queriesAvgPerHour\":0,\"querySpark\":[],\"indexersKnown\":21}";
    int n = (int)strlen(j);
    if (n > (int)cap) return -n;
    memcpy(out, j, n);
    return n;
}

int32_t hal_xprs_archivers(char *out, uint32_t cap) {
    const char *j = "{\"list\":[\"X1WATT\"],\"auto\":true}";
    int n = (int)strlen(j);
    if (n > (int)cap) return -n;
    memcpy(out, j, n);
    return n;
}
int32_t hal_xprs_stations(char *out, uint32_t cap) {
    const char *j = "[]";
    int n = (int)strlen(j);
    if (n > (int)cap) return -n;
    memcpy(out, j, n);
    return n;
}
int32_t hal_identity(char *out, uint32_t cap) {
    const char *j = "X1SELF";
    int n = (int)strlen(j);
    if (n > (int)cap) return -n;
    memcpy(out, j, n);
    return n;
}

/* ── the rest of the surface the wapp touches ──────────────────────────── */
static char g_ev[8][48];
static int  g_evn = 0;
void event_push(const char *topic) {
    if (g_evn < 8) snprintf(g_ev[g_evn++], 48, "%s", topic);
}
int32_t hal_event_subscribe(const char *t, uint32_t len) { (void)t; (void)len; return 0; }
int32_t hal_event_available(void) { return g_evn; }
int32_t hal_event_recv(char *topic, uint32_t tcap, char *data, uint32_t dcap) {
    if (g_evn <= 0) return 0;
    int n = (int)strlen(g_ev[0]);
    if (n > (int)tcap) n = (int)tcap;
    memcpy(topic, g_ev[0], n);
    if (dcap > 2) { data[0] = '{'; data[1] = '}'; }
    for (int i = 1; i < g_evn; i++) memcpy(g_ev[i - 1], g_ev[i], 48);
    g_evn--;
    return n;
}
void hal_log(int32_t lvl, const char *m, uint32_t len) { (void)lvl; (void)m; (void)len; }
int32_t hal_kv_get(const char *k, uint32_t kl, char *o, uint32_t oc) {
    (void)k; (void)kl; (void)o; (void)oc; return 0;
}
int32_t hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl) {
    (void)k; (void)kl; (void)v; (void)vl; return 0;
}
