/*
 * Native mock HAL for the Social wapp tests.
 *
 * The interesting surface is small: what the wapp asks the core to publish
 * (hal_xprs_status / hal_xprs_send), what it draws (hal_msg_send), what the
 * core tells it (the event queue), and what the spool answers
 * (hal_xprs_history). Everything else is canned.
 *
 * hal_xprs_status hands back a section 5 identifier the way the real core
 * does, because the whole point of the feature under test is that the wapp
 * can draw its own post before anything has been aired.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── outbox capture (hal_msg_send) ─────────────────────────────────────── */
#define CAP_MAX 128
static char *g_cap[CAP_MAX];
static int   g_capn = 0;
void cap_clear(void) { for (int i = 0; i < g_capn; i++) free(g_cap[i]); g_capn = 0; }
int  cap_count(void) { return g_capn; }
int  cap_contains(const char *s) {
    for (int i = 0; i < g_capn; i++) if (strstr(g_cap[i], s)) return 1;
    return 0;
}
/* How many captured messages contain [s] — a feed that shows one post twice
 * is the failure this whole change is about, so it has to be countable. */
int cap_count_of(const char *s) {
    int n = 0;
    for (int i = 0; i < g_capn; i++) if (strstr(g_cap[i], s)) n++;
    return n;
}
const char *cap_at(int i) { return (i >= 0 && i < g_capn) ? g_cap[i] : ""; }
/* Every `"t":` a captured message carries must be a NUMBER — the feed sorts on
 * it, and the host parses the whole message as JSON before it ever gets there.
 * A wire timestamp put where the epoch belongs made `"t":2026-08-29_10:40:00000`
 * and the host dropped the append silently: the post was published, and simply
 * never appeared. Substring assertions all passed while that was true. */
int cap_time_numeric(void) {
    for (int i = 0; i < g_capn; i++) {
        const char *p = g_cap[i];
        while ((p = strstr(p, "\"t\":")) != NULL) {
            p += 4;
            if (*p < '0' || *p > '9') return 0;
            while (*p >= '0' && *p <= '9') p++;
            if (*p != ',' && *p != '}') return 0;
        }
    }
    return 1;
}
void hal_msg_send(const char *json, uint32_t len) {
    if (g_capn >= CAP_MAX) return;
    char *c = (char *)malloc(len + 1);
    memcpy(c, json, len); c[len] = '\0';
    g_cap[g_capn++] = c;
}

/* ── injectable command inbox (hal_msg_recv) ───────────────────────────── */
static char g_inbox[8192];
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

/* ── the core's event queue ────────────────────────────────────────────── */
#define EV_MAX 16
static char g_ev_topic[EV_MAX][48];
static char g_ev_data[EV_MAX][4096];
static int  g_evn = 0, g_evi = 0;
#define SUB_MAX 8
static char g_sub[SUB_MAX][48];
static int  g_subn = 0;
void events_clear(void) { g_evn = 0; g_evi = 0; }
void subs_clear(void) { g_subn = 0; }
int  subscribed(const char *topic) {
    for (int i = 0; i < g_subn; i++) if (!strcmp(g_sub[i], topic)) return 1;
    return 0;
}
void event_push(const char *topic, const char *data) {
    if (g_evn >= EV_MAX) return;
    snprintf(g_ev_topic[g_evn], sizeof(g_ev_topic[0]), "%s", topic);
    snprintf(g_ev_data[g_evn], sizeof(g_ev_data[0]), "%s", data ? data : "");
    g_evn++;
}
int32_t hal_event_subscribe(const char *topic, uint32_t len) {
    if (g_subn >= SUB_MAX) return -1;
    int n = (int)len < 47 ? (int)len : 47;
    memcpy(g_sub[g_subn], topic, n);
    g_sub[g_subn][n] = '\0';
    g_subn++;
    return 0;
}
int32_t hal_event_available(void) { return g_evi < g_evn; }
int32_t hal_event_recv(char *topic, uint32_t tcap, char *data, uint32_t dcap) {
    if (g_evi >= g_evn) return 0;
    snprintf(topic, tcap, "%s", g_ev_topic[g_evi]);
    snprintf(data, dcap, "%s", g_ev_data[g_evi]);
    g_evi++;
    return 1;
}

/* ── what the wapp asked the core to publish ───────────────────────────── */
static char g_last_text[6000];
static char g_last_reply[32];
static int  g_status_calls = 0;
static char g_last_wire[600];
static int  g_send_calls = 0;
static int  g_status_fail = 0;
static int  g_id_seq = 0;
void publish_clear(void) {
    g_last_text[0] = g_last_reply[0] = g_last_wire[0] = '\0';
    g_status_calls = g_send_calls = 0;
}
void status_fails(int on) { g_status_fail = on; }
const char *last_status_text(void) { return g_last_text; }
const char *last_status_reply(void) { return g_last_reply; }
/* The identifier the core handed back for the last publish — what the wapp
 * keys its own row on, and what a reply to it must name. */
static char g_last_id[32];
const char *last_status_id(void) { return g_last_id; }
const char *last_wire(void) { return g_last_wire; }
int  status_calls(void) { return g_status_calls; }
int  send_calls(void) { return g_send_calls; }

int32_t hal_xprs_status(const char *text, uint32_t tlen,
                        const char *mood, uint32_t mlen,
                        const char *reply_to, uint32_t rlen,
                        char *id_out, uint32_t id_cap) {
    (void)mood; (void)mlen;
    g_status_calls++;
    int n = (int)tlen < (int)sizeof(g_last_text) - 1 ? (int)tlen : (int)sizeof(g_last_text) - 1;
    memcpy(g_last_text, text, n); g_last_text[n] = '\0';
    n = (int)rlen < 31 ? (int)rlen : 31;
    memcpy(g_last_reply, reply_to ? reply_to : "", n); g_last_reply[n] = '\0';
    if (g_status_fail || tlen == 0) return -1;
    /* The core composes and signs before it returns, so an identifier exists
     * here too. Distinct per call, like the real ones. */
    snprintf(g_last_id, sizeof(g_last_id), "mine%03d", ++g_id_seq);
    if (id_cap > 0) snprintf(id_out, id_cap, "%s", g_last_id);
    return 0;
}
int32_t hal_xprs_send(const char *wire, uint32_t len) {
    g_send_calls++;
    int n = (int)len < (int)sizeof(g_last_wire) - 1 ? (int)len : (int)sizeof(g_last_wire) - 1;
    memcpy(g_last_wire, wire, n); g_last_wire[n] = '\0';
    return 0;
}

/* ── the spool ─────────────────────────────────────────────────────────── */
static char g_hist[16384] = "[]";
/* The query VERBATIM, exactly as many bytes as the wapp said it was handing
 * over. A length that does not match the string is invisible to a substring
 * assertion and fatal in the host, which parses the buffer as JSON and falls
 * back to "no filter" when it throws — so a read meant for statuses quietly
 * became the newest 200 rows of anything. */
static char g_last_query[256];
const char *last_query(void) { return g_last_query; }
void history_set(const char *json) { snprintf(g_hist, sizeof(g_hist), "%s", json); }
int32_t hal_xprs_history(const char *query, uint32_t qlen, char *out, uint32_t cap) {
    unsigned qn = qlen < sizeof(g_last_query) - 1 ? qlen : sizeof(g_last_query) - 1;
    memcpy(g_last_query, query, qn);
    g_last_query[qn] = '\0';
    int n = (int)strlen(g_hist);
    if (n > (int)cap) return -n;
    memcpy(out, g_hist, n);
    return n;
}

/* ── the rest of the surface ───────────────────────────────────────────── */
static int g_attached = 1;
void ui_attached(int on) { g_attached = on; }
int32_t hal_ui_attached(void) { return g_attached; }

int32_t hal_identity(char *out, uint32_t cap) {
    const char *j = "X1SELF";
    int n = (int)strlen(j);
    if (n > (int)cap) return -n;
    memcpy(out, j, n);
    return n;
}
int64_t hal_time_epoch(void) { return 1788000000; }
void hal_log(int32_t level, const char *msg, uint32_t len) {
    (void)level; (void)msg; (void)len;
}
static char g_kvbuf[2048] = "";
int32_t hal_kv_get(const char *k, uint32_t klen, char *out, uint32_t cap) {
    (void)k; (void)klen;
    int n = (int)strlen(g_kvbuf);
    if (n > (int)cap) return -n;
    memcpy(out, g_kvbuf, n);
    return n;
}
int32_t hal_kv_set(const char *k, uint32_t klen, const char *v, uint32_t vlen) {
    (void)k; (void)klen;
    int n = (int)vlen < (int)sizeof(g_kvbuf) - 1 ? (int)vlen : (int)sizeof(g_kvbuf) - 1;
    memcpy(g_kvbuf, v, n); g_kvbuf[n] = '\0';
    return 0;
}
