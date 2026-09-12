/*
 * firmwares -- set up the station you just flashed (XPRS.md 11.9, 11.10),
 * and read how it is doing (15.5).
 *
 * A freshly flashed station has no owner, no network and no name, and says
 * so: it airs `t:request q:owner` with its key in `k:` on its local bearers.
 * This wapp hears that on `xprs.request`, tells the person holding the phone
 * that a station nearby can be set up, and then, one small screen per task:
 *
 *   Station   what it is and how it stands; Claim while nobody owns it
 *   WiFi      network and password, sealed in x: to the station's key
 *   Name      nick, time zone, its own hotspot
 *   Identity  a new key the station makes, or an nsec, sealed
 *   Stats     15.5 telemetry the core already heard, the policy (11.9,
 *             anybody may ask), and the firmware's own diagnostics
 *   Answers   every answer the station gave
 *
 * Every packet goes through hal_xprs_send and every secret through
 * hal_encrypt: the core owns the radio, the signature and the key, and this
 * wapp never learns which bearer carried anything (docs/architecture.md).
 * Passwords arrive in `$type:"secret"` fields, which the host never stores,
 * are sealed the moment they are read, and the buffers are cleared.
 *
 * Event-driven, no clock, no transport. A command is handed to the core once;
 * the core airs it again until the station answers and says so on
 * `xprs.status.tx` when it never does. This wapp listens for answers only
 * while it has asked something, for identities only while a station is
 * changing its key, and for observations only while a policy ask is out: an
 * idle phone hands it nothing but the rare ask to be claimed.
 */
#include "../hal/xprs_wasm_hal.h"
#include "wire.h"

#define ST_MAX       8

typedef struct {
    char call[12];
    char npub[70];
    int  asking;                  /* a Refresh is out: empty tiles read "..." */
    /* What it said about itself (11.10 results, zdiag, q:policy, q:mail). */
    char nick[20], wifi[12], ip[20], ap[4], zone[8];
    char fw[24], uptime[16], peers[8], heap[20], reset[16];
    char health[16], slot[16], radio[48], crash[32], mail[8];
    char pol_owner[40], pol_use[12], pol_first[40], pol_serve[40];
    char bearer[8];
    int  rssi;
    unsigned long long heard_ms;
    int  unowned;                 /* asked to be claimed */
    int  mine;                    /* claimed by this profile */
    int  theirs;                  /* owned by somebody else */
    /* The one command in flight to it. The wire is kept only to say it
     * again after a 408, under a newer stamp. */
    char pend[FW_WIRE_UNSIGNED + 1];
    char pend_id[7];
    char pend_what[8];            /* claim wifi set key zdiag zcore */
    int  pend_408, pend_final;
    char last_what[8];            /* what a late final answer was to */
    char next_x[FW_WIRE_UNSIGNED];/* the second half of a long ssid+pass */
    char rekey_npub[70];          /* following a new key (11.10) */
    unsigned long long last_ts;   /* the last ts: we used with it */
    int  ask_pol;                 /* a q:policy is out: observations wanted */
    char now[96];                 /* the one line the Station screen shows */
} st_t;

static st_t g_st[ST_MAX];
static int  g_nst;
static int  g_sel = -1;
static int  g_sub_res, g_sub_tx, g_sub_ids, g_sub_obs;
static char g_me[16];
static char g_my_npub[70];

/* The last few identities heard, so a station that changed its key can be
 * followed even when its 202 k: was missed: its 200 then arrives from a
 * callsign this table names (11.10). */
static struct { char call[12]; char npub[70]; } g_ids[4];
static int g_ids_w;

static char g_ev[4096];
static char g_topic[64];
static char g_buf[4096];
static char g_out[8192];
static char g_host[1024];         /* one hal_xprs_station answer */

static void say(const char *json) { hal_msg_send(json, fw_len(json)); }

/* ── Who we are ───────────────────────────────────────────────────────── */
static void who_am_i(void)
{
    uint32_t n = hal_identity(g_me, sizeof g_me - 1);
    g_me[n] = 0;
    char pk[80];
    n = hal_identity_pubkey(pk, sizeof pk - 1);
    pk[n] = 0;
    if (fw_starts(pk, "npub1")) {
        fw_cpy(g_my_npub, pk, sizeof g_my_npub);
    } else if (n) {
        n = hal_npub(pk, fw_len(pk), g_my_npub, sizeof g_my_npub - 1);
        g_my_npub[n] = 0;
    }
}

/* ── The stations, kept across the engine's restarts ─────────────────── */
static int find(const char *call)
{
    for (int i = 0; i < g_nst; i++) if (fw_eq(g_st[i].call, call)) return i;
    return -1;
}

static int add(const char *call)
{
    int i = find(call);
    if (i >= 0) return i;
    if (g_nst >= ST_MAX) {
        /* Forget the longest-unheard one that is not ours. */
        int old = -1;
        for (int j = 0; j < g_nst; j++)
            if (!g_st[j].mine && (old < 0 || g_st[j].heard_ms < g_st[old].heard_ms)) old = j;
        if (old < 0) return -1;
        i = old;
    } else {
        i = g_nst++;
    }
    st_t *s = &g_st[i];
    for (unsigned k = 0; k < sizeof *s; k++) ((char *)s)[k] = 0;
    fw_cpy(s->call, call, sizeof s->call);
    return i;
}

static void drop(int i)
{
    if (i < 0 || i >= g_nst) return;
    for (int j = i; j + 1 < g_nst; j++) g_st[j] = g_st[j + 1];
    g_nst--;
    for (unsigned k = 0; k < sizeof g_st[g_nst]; k++) ((char *)&g_st[g_nst])[k] = 0;
}

static void kv_put(const char *k, const char *v) { hal_kv_set(k, fw_len(k), v, fw_len(v)); }

static void save(void)
{
    char list[ST_MAX * 13] = "";
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (!s->mine && !s->unowned && !s->theirs) continue;
        if (list[0]) fw_cat(list, ",", sizeof list);
        fw_cat(list, s->call, sizeof list);
        char k[24] = "st.", v[320] = "";
        fw_cat(k, s->call, sizeof k);
        fw_cat(v, s->npub, sizeof v);
        fw_cat(v, s->mine ? "|1|" : "|0|", sizeof v);
        fw_cat(v, s->unowned ? "1|" : "0|", sizeof v);
        fw_cat(v, s->nick, sizeof v);
        fw_cat(v, "|", sizeof v);
        fw_cat(v, s->fw, sizeof v);
        fw_cat(v, s->theirs ? "|1|" : "|0|", sizeof v);
        fw_cat(v, s->wifi, sizeof v);
        fw_cat(v, "|", sizeof v);
        fw_cat(v, s->ip, sizeof v);
        /* The policy it last stated, so the Stats screen opens with it. */
        fw_cat(v, "|", sizeof v); fw_cat(v, s->pol_owner, sizeof v);
        fw_cat(v, "|", sizeof v); fw_cat(v, s->pol_use, sizeof v);
        fw_cat(v, "|", sizeof v); fw_cat(v, s->pol_first, sizeof v);
        fw_cat(v, "|", sizeof v); fw_cat(v, s->pol_serve, sizeof v);
        kv_put(k, v);
    }
    kv_put("st.list", list);
}

/* What the station last answered to cmd:zdiag, so the Stats screen opens
 * with the last known figures and says when they are being refreshed. */
static void save_stats(const st_t *s)
{
    char k[24] = "stx.", v[220] = "";
    fw_cat(k, s->call, sizeof k);
    const char *f[] = { s->uptime, s->peers, s->heap, s->reset, s->health, s->slot, s->radio, s->crash, s->mail };
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (i) fw_cat(v, "|", sizeof v);
        fw_cat(v, f[i], sizeof v);
    }
    kv_put(k, v);
}

static void load_stats(st_t *s)
{
    char k[24] = "stx.", v[220];
    fw_cat(k, s->call, sizeof k);
    uint32_t n = hal_kv_get(k, fw_len(k), v, sizeof v - 1);
    v[n] = 0;
    if (!n) return;
    char *f[9] = {0};
    f[0] = v;
    for (int j = 1, q = 0; v[q] && j < 9; q++)
        if (v[q] == '|') { v[q] = 0; f[j++] = v + q + 1; }
    char *dst[] = { s->uptime, s->peers, s->heap, s->reset, s->health, s->slot, s->radio, s->crash, s->mail };
    unsigned caps[] = { sizeof s->uptime, sizeof s->peers, sizeof s->heap, sizeof s->reset, sizeof s->health,
                        sizeof s->slot, sizeof s->radio, sizeof s->crash, sizeof s->mail };
    for (int i = 0; i < 9; i++) if (f[i]) fw_cpy(dst[i], f[i], caps[i]);
}

static void load(void)
{
    char list[ST_MAX * 13 + 1];
    uint32_t n = hal_kv_get("st.list", 7, list, sizeof list - 1);
    list[n] = 0;
    for (char *p = list; *p; ) {
        char call[12];
        unsigned c = 0;
        while (*p && *p != ',' && c < sizeof call - 1) call[c++] = *p++;
        call[c] = 0;
        if (*p == ',') p++;
        if (!call[0]) continue;
        char k[24] = "st.", v[320];
        fw_cat(k, call, sizeof k);
        n = hal_kv_get(k, fw_len(k), v, sizeof v - 1);
        v[n] = 0;
        int i = add(call);
        if (i < 0) break;
        st_t *s = &g_st[i];
        /* npub|mine|unowned|nick|fw|theirs|wifi|ip|owner|use|first|serve */
        char *f[12] = {0};
        f[0] = v;
        for (int j = 1, q = 0; v[q] && j < 12; q++)
            if (v[q] == '|') { v[q] = 0; f[j++] = v + q + 1; }
        fw_cpy(s->npub, f[0], sizeof s->npub);
        s->mine = f[1] && f[1][0] == '1';
        s->unowned = f[2] && f[2][0] == '1';
        if (f[3]) fw_cpy(s->nick, f[3], sizeof s->nick);
        if (f[4]) fw_cpy(s->fw, f[4], sizeof s->fw);
        s->theirs = f[5] && f[5][0] == '1';
        if (f[6]) fw_cpy(s->wifi, f[6], sizeof s->wifi);
        if (f[7]) fw_cpy(s->ip, f[7], sizeof s->ip);
        if (f[8]) fw_cpy(s->pol_owner, f[8], sizeof s->pol_owner);
        if (f[9]) fw_cpy(s->pol_use, f[9], sizeof s->pol_use);
        if (f[10]) fw_cpy(s->pol_first, f[10], sizeof s->pol_first);
        if (f[11]) fw_cpy(s->pol_serve, f[11], sizeof s->pol_serve);
        load_stats(s);
    }
}

/* ── What the core holds about a station (hal_xprs_station) ──────────── */
/* Fills g_host; 1 when the core has heard it this hour. */
static int host_facts(const st_t *s)
{
    int n = hal_xprs_station(s->call, fw_len(s->call), g_host, sizeof g_host - 1);
    if (n <= 0) { g_host[0] = 0; return 0; }
    g_host[n] = 0;
    return 1;
}

static unsigned long long host_num(const char *key)
{
    char v[24];
    if (!fw_json(g_host, key, v, sizeof v)) return 0;
    unsigned long long n = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (unsigned)(*p - '0');
    return n;
}

static void ago_words(unsigned long long ms, char *out, unsigned cap)
{
    fw_cpy(out, "", cap);
    if (ms < 60000ULL)         { fw_cat_u(out, ms / 1000, cap); fw_cat(out, " s ago", cap); }
    else if (ms < 3600000ULL)  { fw_cat_u(out, ms / 60000, cap); fw_cat(out, " min ago", cap); }
    else                       { fw_cat_u(out, ms / 3600000, cap); fw_cat(out, " h ago", cap); }
}

/* "BLE -87 dBm" or "LAN", from the core's last sighting, else what the ask
 * carried. [rssi_out], when given, gets the level alone for a tile. */
static void signal_parts(const st_t *s, int have_host, char *out, unsigned cap, int *rssi_out)
{
    char b[8] = "";
    int rssi = s->rssi;
    if (have_host) {
        fw_json(g_host, "bearer", b, sizeof b);
        char r[12];
        if (fw_json(g_host, "rssi", r, sizeof r)) {
            int neg = r[0] == '-', v = 0;
            for (const char *p = r + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
            rssi = neg ? -v : v;
        }
    }
    if (!b[0]) fw_cpy(b, s->bearer, sizeof b);
    out[0] = 0;
    for (unsigned j = 0; b[j] && j < sizeof b; j++) {
        char c[2] = { fw_up(b[j]), 0 };
        fw_cat(out, c, cap);
    }
    if (fw_eq(out, "BLE5")) fw_cpy(out, "BLE", cap);
    if (rssi_out) { *rssi_out = rssi; if (!out[0]) fw_cpy(out, "not heard", cap); return; }
    if (rssi) { fw_cat(out, " -", cap); fw_cat_u(out, (unsigned)(rssi < 0 ? -rssi : rssi), cap); fw_cat(out, " dBm", cap); }
    if (!out[0]) fw_cpy(out, "not heard", cap);
}

static void signal_words(const st_t *s, int have_host, char *out, unsigned cap)
{
    signal_parts(s, have_host, out, cap, 0);
}

static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, const char *progress, int alert);

/* The Signal tile: the level as the number, the bearer and the age as the
 * hint, so a phone-width tile is not cut short. */
static void signal_tile(const st_t *s, int have_host, unsigned long long agoms, int alert)
{
    char b[24], hint[48] = "", num[12] = "";
    int rssi = 0;
    signal_parts(s, have_host, b, sizeof b, &rssi);
    if (rssi) { fw_cpy(num, "-", sizeof num); fw_cat_u(num, (unsigned)(rssi < 0 ? -rssi : rssi), sizeof num); }
    fw_cpy(hint, b, sizeof hint);
    if (have_host) { char ago[24]; ago_words(agoms, ago, sizeof ago); fw_cat(hint, ", ", sizeof hint); fw_cat(hint, ago, sizeof hint); }
    tile("signal", "Signal", rssi ? num : b, rssi ? "dBm" : "", rssi || have_host ? hint : "", "", alert);
}

/* ── Screens ──────────────────────────────────────────────────────────── */
static void log_line(const char *call, const char *text)
{
    char m[400] = "{\"type\":\"ui.log.append\",\"field\":\"log\",\"line\":\"";
    if (call && call[0]) { fw_jesc(m, call, sizeof m); fw_cat(m, ": ", sizeof m); }
    fw_jesc(m, text, sizeof m);
    fw_cat(m, "\"}", sizeof m);
    say(m);
    /* And the host's log: a headless engine's lines reach /api/log, which
     * is where a setup that went wrong gets read afterwards. Only what a
     * person did or a station answered comes through here, never a packet. */
    char l[200] = "firmwares: ";
    if (call && call[0]) { fw_cat(l, call, sizeof l); fw_cat(l, ": ", sizeof l); }
    fw_cat(l, text, sizeof l);
    hal_log(1, l, fw_len(l));
}

static void field_set(const char *field, const char *value)
{
    char m[200] = "{\"type\":\"ui.field.set\",\"field\":\"";
    fw_cat(m, field, sizeof m);
    fw_cat(m, "\",\"value\":\"", sizeof m);
    fw_jesc(m, value, sizeof m);
    fw_cat(m, "\"}", sizeof m);
    say(m);
}

/* `<name>__hidden`: the host leaves the button or field off the screen. */
static void flag_hidden(const char *name, int hidden)
{
    char m[120] = "{\"type\":\"ui.field.set\",\"field\":\"";
    fw_cat(m, name, sizeof m);
    fw_cat(m, hidden ? "__hidden\",\"value\":true}" : "__hidden\",\"value\":false}", sizeof m);
    say(m);
}

static void screen_open(const char *name, const char *title)
{
    char m[160] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    fw_cat(m, name, sizeof m);
    fw_cat(m, "\",\"title\":\"", sizeof m);
    fw_jesc(m, title, sizeof m);
    fw_cat(m, "\"}", sizeof m);
    say(m);
}

/* One details section with one line, for a "what is happening" row. */
static void one_line(const char *field, const char *title, const char *text)
{
    char m[300] = "{\"type\":\"ui.field.set\",\"field\":\"";
    fw_cat(m, field, sizeof m);
    if (!text || !text[0]) { fw_cat(m, "\",\"value\":[]}", sizeof m); say(m); return; }
    fw_cat(m, "\",\"value\":[{\"title\":\"\",\"items\":[{\"label\":\"", sizeof m);
    fw_jesc(m, title, sizeof m);
    fw_cat(m, "\",\"value\":\"", sizeof m);
    fw_jesc(m, text, sizeof m);
    fw_cat(m, "\"}]}]}", sizeof m);
    say(m);
}

/* The latest thing about a station, on its screen and the WiFi screen. */
static void set_now(st_t *s, const char *text)
{
    fw_cpy(s->now, text, sizeof s->now);
    if (g_sel >= 0 && &g_st[g_sel] == s) {
        one_line("now", "Now", s->now);
        one_line("wifi_now", "Now", s->now);
    }
}

/* ── Stats tiles ──────────────────────────────────────────────────────── */
static void tiles_begin(const char *field)
{
    fw_cpy(g_out, "{\"type\":\"ui.stats.set\",\"field\":\"", sizeof g_out);
    fw_cat(g_out, field, sizeof g_out);
    fw_cat(g_out, "\",\"tiles\":[", sizeof g_out);
}

static const char *g_empty = "?";   /* what an empty tile shows */

static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, const char *progress, int alert)
{
    unsigned n = fw_len(g_out);
    if (g_out[n - 1] != '[') fw_cat(g_out, ",", sizeof g_out);
    fw_cat(g_out, "{\"id\":\"", sizeof g_out);
    fw_jesc(g_out, id, sizeof g_out);
    fw_cat(g_out, "\",\"label\":\"", sizeof g_out);
    fw_jesc(g_out, label, sizeof g_out);
    fw_cat(g_out, "\",\"value\":\"", sizeof g_out);
    fw_jesc(g_out, value && value[0] ? value : g_empty, sizeof g_out);
    fw_cat(g_out, "\"", sizeof g_out);
    if (unit && unit[0]) { fw_cat(g_out, ",\"unit\":\"", sizeof g_out); fw_jesc(g_out, unit, sizeof g_out); fw_cat(g_out, "\"", sizeof g_out); }
    if (hint && hint[0]) { fw_cat(g_out, ",\"hint\":\"", sizeof g_out); fw_jesc(g_out, hint, sizeof g_out); fw_cat(g_out, "\"", sizeof g_out); }
    if (progress && progress[0]) { fw_cat(g_out, ",\"progress\":", sizeof g_out); fw_cat(g_out, progress, sizeof g_out); }
    if (alert) fw_cat(g_out, ",\"alert\":true", sizeof g_out);
    fw_cat(g_out, "}", sizeof g_out);
}

static void tiles_end(void)
{
    fw_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* The Station screen: six tiles, the Now line, and which buttons apply. */
static void push_hub(void)
{
    if (g_sel < 0 || g_sel >= g_nst) return;
    st_t *s = &g_st[g_sel];
    int have = host_facts(s);
    unsigned long long agoms = have ? host_num("agoMs") : 0;
    int stale = !have || agoms > 600000ULL;

    tiles_begin("hub");
    tile("owner", "Owner",
         s->mine ? "You" : s->unowned ? "Nobody" : s->theirs ? "Not you" : "?",
         "", s->unowned && !s->mine ? "tap Claim" : s->theirs ? "somebody else's" : "", "", 0);
    {
        int joining = fw_eq(s->wifi, "joining");
        tile("wifi", "WiFi", s->wifi[0] ? s->wifi : "unknown", "", s->ip,
             joining ? "0.5" : "", fw_eq(s->wifi, "failed"));
    }
    signal_tile(s, have, agoms, stale);
    {
        char up[16] = "";
        if (have) fw_json(g_host, "uptime", up, sizeof up);
        if (!up[0]) fw_cpy(up, s->uptime, sizeof up);
        tile("up", "Up", up, "", "", "", 0);
    }
    {
        char fw[24] = "";
        if (have) fw_json(g_host, "fw", fw, sizeof fw);
        if (!fw[0]) fw_cpy(fw, s->fw, sizeof fw);
        tile("fw", "Firmware", fw, "", "", "", 0);
    }
    {
        char pe[8] = "";
        if (have) fw_json(g_host, "peers", pe, sizeof pe);
        if (!pe[0]) fw_cpy(pe, s->peers, sizeof pe);
        tile("peers", "Peers", pe, "", "", "", 0);
    }
    tiles_end();
    if (!s->now[0])
        fw_cpy(s->now, s->mine ? "Yours" : s->unowned ? "Waiting for an owner"
                     : s->theirs ? "Somebody else's" : "Not heard from yet", sizeof s->now);
    one_line("now", "Now", s->now);

    flag_hidden("claim", !(s->unowned && !s->mine));
    flag_hidden("open_wifi", !s->mine);
    flag_hidden("open_name", !s->mine);
    flag_hidden("open_identity", !s->mine);
}

static void item(const char *id, const char *title, const char *sub,
                 const char *tags, int dim, int *first)
{
    if (!*first) fw_cat(g_out, ",", sizeof g_out);
    *first = 0;
    fw_cat(g_out, "{\"id\":\"", sizeof g_out);
    fw_jesc(g_out, id, sizeof g_out);
    fw_cat(g_out, "\",\"title\":\"", sizeof g_out);
    fw_jesc(g_out, title, sizeof g_out);
    fw_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    fw_jesc(g_out, sub, sizeof g_out);
    fw_cat(g_out, "\",\"tags\":[", sizeof g_out);
    fw_cat(g_out, tags, sizeof g_out);
    fw_cat(g_out, "]", sizeof g_out);
    if (dim) fw_cat(g_out, ",\"dim\":true", sizeof g_out);
    fw_cat(g_out, "}", sizeof g_out);
}

static void tag(char *tags, unsigned cap, const char *prefix, const char *v)
{
    if (!v || !v[0]) return;
    if (tags[0]) fw_cat(tags, ",", cap);
    fw_cat(tags, "\"", cap);
    fw_jesc(tags, prefix, cap);
    fw_jesc(tags, v, cap);
    fw_cat(tags, "\"", cap);
}

static void push_list(void)
{
    fw_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":[",
           sizeof g_out);
    int any = 0;
    static const char *const titles[3] = {
        "{\"title\":\"Waiting for an owner\",\"items\":[",
        "{\"title\":\"Yours\",\"items\":[",
        "{\"title\":\"Others\",\"items\":["
    };
    for (int pass = 0; pass < 3; pass++) {
        int first = 1;
        for (int i = 0; i < g_nst; i++) {
            st_t *s = &g_st[i];
            int in = pass == 0 ? (s->unowned && !s->mine)
                   : pass == 1 ? s->mine
                   : (s->theirs && !s->mine && !s->unowned);
            if (!in) continue;
            if (first) {
                if (any) fw_cat(g_out, ",", sizeof g_out);
                fw_cat(g_out, titles[pass], sizeof g_out);
                any = 1;
            }
            int have = host_facts(s);
            char sub[64], ago[24];
            signal_words(s, have, sub, sizeof sub);
            unsigned long long agoms = have ? host_num("agoMs") : 0;
            if (have) { ago_words(agoms, ago, sizeof ago); fw_cat(sub, ", ", sizeof sub); fw_cat(sub, ago, sizeof sub); }
            if (pass == 0) fw_cat(sub, ". Tap to claim it", sizeof sub);
            char tags[160] = "", fw[24] = "", up[16] = "";
            if (have) { fw_json(g_host, "fw", fw, sizeof fw); fw_json(g_host, "uptime", up, sizeof up); }
            tag(tags, sizeof tags, "fw ", fw[0] ? fw : s->fw);
            tag(tags, sizeof tags, "WiFi ", s->wifi);
            tag(tags, sizeof tags, "up ", up[0] ? up : s->uptime);
            item(s->call, s->nick[0] ? s->nick : s->call, sub, tags,
                 !have || agoms > 600000ULL, &first);
        }
        if (!first) fw_cat(g_out, "]}", sizeof g_out);
    }
    fw_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* The health word of zh:<up>/<required>: every required part up, or not. */
static void health_words(const char *zh, char *out, unsigned cap)
{
    unsigned long long up = 0, req = 0;
    const char *p = zh;
    for (; *p && *p != '/'; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
        if (d < 0) { fw_cpy(out, zh, cap); return; }
        up = up * 16 + (unsigned)d;
    }
    if (*p != '/') { fw_cpy(out, zh, cap); return; }
    for (p++; *p; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
        if (d < 0) break;
        req = req * 16 + (unsigned)d;
    }
    unsigned long long missing = req & ~up;
    int n = 0;
    for (; missing; missing >>= 1) n += (int)(missing & 1);
    if (n == 0) fw_cpy(out, "all up", cap);
    else { fw_cpy(out, "", cap); fw_cat_u(out, (unsigned)n, cap); fw_cat(out, n == 1 ? " part down" : " parts down", cap); }
}

/* Split "a/b/c" into up to four pieces. */
static int slashes(const char *v, char out[4][16])
{
    int n = 0, o = 0;
    out[0][0] = 0;
    for (const char *p = v; *p && n < 4; p++) {
        if (*p == '/') { out[n][o] = 0; n++; o = 0; if (n < 4) out[n][0] = 0; continue; }
        if (o < 15) out[n][o++] = *p;
    }
    if (n < 4) { out[n][o] = 0; n++; }
    return n;
}

static void push_stats(void)
{
    if (g_sel < 0 || g_sel >= g_nst) return;
    st_t *s = &g_st[g_sel];
    int have = host_facts(s);
    char v[64], w[4][16];
    g_empty = s->asking ? "..." : "?";

    /* 15.5: what the core heard the station say about itself. */
    tiles_begin("st_station");
    v[0] = 0; if (have) fw_json(g_host, "fw", v, sizeof v);
    tile("fw", "Firmware", v[0] ? v : s->fw, "", "", "", 0);
    v[0] = 0; if (have) fw_json(g_host, "uptime", v, sizeof v);
    tile("up", "Up", v[0] ? v : s->uptime, "", "since its last restart", "", 0);
    v[0] = 0; if (have) fw_json(g_host, "lifetime", v, sizeof v);
    tile("life", "Lifetime", v, "", "across every restart", "", 0);
    v[0] = 0; if (have) fw_json(g_host, "peers", v, sizeof v);
    tile("peers", "Peers", v[0] ? v : s->peers, "", "stations it reaches", "", 0);
    v[0] = 0; if (have) fw_json(g_host, "mail", v, sizeof v);
    tile("mail", "Mail held", v[0] ? v : s->mail, "", "for other stations", "", 0);
    v[0] = 0; if (have) fw_json(g_host, "count", v, sizeof v);
    tile("count", "Records", v, "", "in its archive", "", 0);
    signal_tile(s, have, have ? host_num("agoMs") : 0, 0);
    v[0] = 0; if (have) fw_json_list(g_host, "bearers", v, sizeof v);
    /* "ble+lan": a phone-width tile has room for that and not for a list. */
    for (unsigned i = 0, o = 0; ; i++) {
        if (v[i] == ',') { v[o++] = '+'; if (v[i + 1] == ' ') i++; continue; }
        v[o++] = v[i];
        if (!v[i]) break;
    }
    tile("bearers", "Heard over", v, "", "", "", 0);
    v[0] = 0; if (have) fw_json(g_host, "sig", v, sizeof v);
    tile("sig", "Signatures", v, "", "", "", fw_eq(v, "forged"));
    tiles_end();
    v[0] = 0; if (have) fw_json_list(g_host, "hears", v, sizeof v);
    one_line("st_hears", "Hears", v);

    /* 11.9: the policy, what anybody may ask. */
    tiles_begin("st_policy");
    tile("owner", "Owner", s->pol_owner, "", "", "", 0);
    tile("use", "Use", s->pol_use, "", "who may send through it", "", 0);
    tile("first", "First", s->pol_first, "", "served ahead of others", "", 0);
    tile("serve", "Serve", s->pol_serve, "", "what it does for others", "", 0);
    tiles_end();

    /* The firmware's own words (cmd:zdiag, its owner only). */
    tiles_begin("st_diag");
    if (s->heap[0]) {
        int n = slashes(s->heap, w);
        char hint[48] = "";
        if (n >= 3) { fw_cpy(hint, "largest ", sizeof hint); fw_cat(hint, w[1], sizeof hint); fw_cat(hint, ", lowest ", sizeof hint); fw_cat(hint, w[2], sizeof hint); }
        tile("heap", "Free memory", w[0], "KB", hint, "", 0);
    } else {
        tile("heap", "Free memory", "", "KB", s->mine ? "tap Refresh" : "its owner may ask", "", 0);
    }
    tile("reset", "Last reset", s->reset, "", "", "", fw_eq(s->reset, "panic"));
    if (s->health[0]) { health_words(s->health, v, sizeof v); tile("health", "Health", v, "", s->health, "", !fw_eq(v, "all up")); }
    else tile("health", "Health", "", "", "", "", 0);
    if (s->slot[0]) { slashes(s->slot, w); tile("slot", "Slot", w[0], "", "the OTA slot in use", "", 0); }
    else tile("slot", "Slot", "", "", "the OTA slot in use", "", 0);
    if (s->radio[0]) {
        /* "rx/tx/cancel/drop done/issued/fail" as kept by take_state. */
        char zn[24] = "", zs[24] = "";
        unsigned i = 0, o = 0;
        for (; s->radio[i] && s->radio[i] != ' ' && o < sizeof zn - 1; i++) zn[o++] = s->radio[i];
        zn[o] = 0;
        if (s->radio[i] == ' ') fw_cpy(zs, s->radio + i + 1, sizeof zs);
        int n = slashes(zn, w);
        char hint[48] = "";
        if (n >= 4) { fw_cpy(hint, "sent ", sizeof hint); fw_cat(hint, w[1], sizeof hint); fw_cat(hint, ", dropped ", sizeof hint); fw_cat(hint, w[3], sizeof hint); }
        tile("radio", "ESP-NOW heard", w[0], "", hint, "", 0);
        n = slashes(zs, w);
        hint[0] = 0;
        if (n >= 3) { fw_cpy(hint, "of ", sizeof hint); fw_cat(hint, w[1], sizeof hint); fw_cat(hint, ", failed ", sizeof hint); fw_cat(hint, w[2], sizeof hint); }
        tile("sent", "Sent", w[0], "", hint, "", n >= 3 && !fw_eq(w[2], "0"));
    }
    if (s->crash[0]) tile("crash", "Crashed in", s->crash, "", "tap Crash report", "", 1);
    tiles_end();
    flag_hidden("crash", !s->crash[0]);
    g_empty = "?";
}

/* ── What to listen to ────────────────────────────────────────────────── */
static void sub(int *held, int want, const char *topic)
{
    if (want && !*held) hal_event_subscribe(topic, fw_len(topic));
    if (!want && *held) hal_event_unsubscribe(topic, fw_len(topic));
    *held = want;
}

/* Answers only while a command is out, identities only while a station is
 * changing its key, observations only while a policy ask is out. Everything
 * else on those topics is somebody else's. */
static void listen(void)
{
    int asked = 0, keying = 0, asking = 0;
    for (int i = 0; i < g_nst; i++) {
        if (g_st[i].pend_id[0]) asked = 1;
        if (g_st[i].rekey_npub[0] ||
            (g_st[i].pend_id[0] && fw_eq(g_st[i].pend_what, "key"))) keying = 1;
        if (g_st[i].ask_pol) asking = 1;
    }
    sub(&g_sub_res, asked, "xprs.result");
    sub(&g_sub_tx, asked, "xprs.status.tx");
    sub(&g_sub_ids, keying, "xprs.identity");
    sub(&g_sub_obs, asking, "xprs.observation");
}

static void stamp_for(st_t *s, char *ts, unsigned cap)
{
    unsigned long long now = hal_time_epoch();
    if (now <= s->last_ts) now = s->last_ts + 1;
    s->last_ts = now;
    fw_stamp(ts, cap, now);
}

/* Air [wire], remember it as the command in flight to [s]. */
static int send_cmd(st_t *s, const char *wire, const char *what)
{
    int rc = hal_xprs_send(wire, fw_len(wire));
    if (rc != 0) {
        log_line(s->call, rc == -2 ? "Refused by this phone: not allowed to send that"
                                   : "This phone could not send that command");
        return rc;
    }
    fw_cpy(s->pend, wire, sizeof s->pend);
    fw_id(wire, fw_len(wire), s->pend_id);
    fw_cpy(s->pend_what, what, sizeof s->pend_what);
    fw_cpy(s->last_what, what, sizeof s->last_what);
    s->pend_final = 0;
    listen();
    return 0;
}

static void done_pending(st_t *s)
{
    s->pend[0] = 0;
    s->pend_id[0] = 0;
    s->pend_what[0] = 0;
    s->pend_final = 0;
    s->pend_408 = 0;
    listen();
}

/* ── Commands to a station ────────────────────────────────────────────── */
static void do_claim(st_t *s)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (fw_claim(w, sizeof w, g_me, s->call, ts, g_my_npub) < 0) {
        log_line(s->call, "This phone's callsign is too long to claim with");
        return;
    }
    if (send_cmd(s, w, "claim") == 0) { log_line(s->call, "Claiming..."); set_now(s, "Claiming..."); }
}

/* Seal [body] to the station and air it; zero the body either way. */
static int send_sealed(st_t *s, char *body, const char *what)
{
    static char x[FW_WIRE_UNSIGNED];
    uint32_t n = hal_encrypt(s->npub, fw_len(s->npub), body, fw_len(body), x, sizeof x - 1);
    for (unsigned i = 0; body[i]; i++) body[i] = 0;
    if (!n) { log_line(s->call, "Could not seal it to the station's key"); return -1; }
    x[n] = 0;
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    int rc = fw_sealed(w, sizeof w, g_me, s->call, ts, x);
    for (unsigned i = 0; x[i]; i++) x[i] = 0;
    if (rc < 0) {
        log_line(s->call, "Too long to seal into one packet");
        return -1;
    }
    return send_cmd(s, w, what);
}

static void do_wifi(st_t *s, char *ssid, char *pass)
{
    char body[128];
    const char *kv1[] = { "ssid", ssid, "pass", pass, 0 };
    int n = fw_body(body, sizeof body, kv1);
    s->next_x[0] = 0;
    if (n < 0) {
        log_line(s->call, "A network name or password cannot hold a line break");
    } else if (n <= FW_BODY_ONE_MAX) {
        if (send_sealed(s, body, "wifi") == 0) set_now(s, "Sending the network, sealed...");
    } else {
        /* Too long for one packet: the name first, then the password, and
         * the station joins when it has the second (11.10). The password's
         * body is sealed now and kept as ciphertext only. */
        const char *kv2[] = { "pass", pass, 0 };
        char b2[96];
        if (fw_body(b2, sizeof b2, kv2) < 0) { log_line(s->call, "Password too long"); }
        else {
            static char x2[FW_WIRE_UNSIGNED];
            uint32_t m = hal_encrypt(s->npub, fw_len(s->npub), b2, fw_len(b2), x2, sizeof x2 - 1);
            for (unsigned i = 0; b2[i]; i++) b2[i] = 0;
            x2[m] = 0;
            fw_cpy(s->next_x, x2, sizeof s->next_x);
            for (unsigned i = 0; x2[i]; i++) x2[i] = 0;
            const char *kv3[] = { "ssid", ssid, 0 };
            char b3[64];
            if (m && fw_body(b3, sizeof b3, kv3) > 0 && send_sealed(s, b3, "wifi") == 0)
                set_now(s, "Sending the network name, then the password...");
        }
        for (unsigned i = 0; body[i]; i++) body[i] = 0;
    }
    for (unsigned i = 0; pass[i]; i++) pass[i] = 0;
    for (unsigned i = 0; ssid[i]; i++) ssid[i] = 0;
}

static void do_set(st_t *s, const char *fields, const char *what)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (fw_set(w, sizeof w, g_me, s->call, ts, fields) < 0) {
        log_line(s->call, "Too much to say in one packet");
        return;
    }
    if (send_cmd(s, w, what) == 0) set_now(s, "Sending...");
}

static void do_cmd(st_t *s, const char *cmd, const char *what)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (fw_cmd(w, sizeof w, g_me, s->call, ts, cmd) >= 0 && send_cmd(s, w, what) == 0)
        set_now(s, fw_eq(what, "zdiag") ? "Asking for its stats..." : "Asking for its crash report...");
}

/* q:policy and q:mail: a t:request, answered by anybody's station as an
 * observation (11.9, 9.12.3). Not a command, so not the courier's: asked
 * once, and read from xprs.observation while the ask is out. */
static void do_ask(st_t *s)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (fw_ask(w, sizeof w, g_me, s->call, ts, "policy") > 0) hal_xprs_send(w, fw_len(w));
    stamp_for(s, ts, sizeof ts);
    if (fw_ask(w, sizeof w, g_me, s->call, ts, "mail") > 0) hal_xprs_send(w, fw_len(w));
    s->ask_pol = 1;
    listen();
}

/* ── What the station says ────────────────────────────────────────────── */
static void take_state(st_t *s, const char *wire)
{
    char v[48];
    if (fw_field(wire, "wifi", v, sizeof v)) fw_cpy(s->wifi, v, sizeof s->wifi);
    if (fw_field(wire, "ip", v, sizeof v)) fw_cpy(s->ip, v, sizeof s->ip);
    else if (fw_field(wire, "wifi", v, sizeof v)) s->ip[0] = 0;
    if (fw_field(wire, "ap", v, sizeof v)) fw_cpy(s->ap, v, sizeof s->ap);
    if (fw_field(wire, "nick", v, sizeof v)) fw_cpy(s->nick, v, sizeof s->nick);
    if (fw_field(wire, "zone", v, sizeof v)) fw_cpy(s->zone, v, sizeof s->zone);
    if (fw_field(wire, "fw", v, sizeof v)) fw_cpy(s->fw, v, sizeof s->fw);
    if (fw_field(wire, "uptime", v, sizeof v)) fw_cpy(s->uptime, v, sizeof s->uptime);
    if (fw_field(wire, "peers", v, sizeof v)) fw_cpy(s->peers, v, sizeof s->peers);
    if (fw_field(wire, "zm", v, sizeof v)) fw_cpy(s->heap, v, sizeof s->heap);
    if (fw_field(wire, "zr", v, sizeof v)) fw_cpy(s->reset, v, sizeof s->reset);
    if (fw_field(wire, "zh", v, sizeof v)) fw_cpy(s->health, v, sizeof s->health);
    if (fw_field(wire, "zp", v, sizeof v)) fw_cpy(s->slot, v, sizeof s->slot);
    if (fw_field(wire, "zn", v, sizeof v)) {
        fw_cpy(s->radio, v, sizeof s->radio);
        if (fw_field(wire, "zs", v, sizeof v)) { fw_cat(s->radio, " ", sizeof s->radio); fw_cat(s->radio, v, sizeof s->radio); }
    }
    if (fw_field(wire, "zc", v, sizeof v)) fw_cpy(s->crash, v, sizeof s->crash);
    else if (fw_field(wire, "zm", v, sizeof v)) s->crash[0] = 0;   /* a zdiag without one */
}

static void on_request(const char *row)
{
    char wire[FW_WIRE_MAX + 1], q[12], call[16], k[80], sig[16];
    /* Believed only when it is signed by the key it carries and the
     * callsign derives from that key (11.9): anything else is somebody
     * inviting this phone to hand a stranger its password. The core's
     * verdict first, because it is the cheapest test. */
    if (!fw_json(row, "sig", sig, sizeof sig) || !fw_eq(sig, "verified")) return;
    if (!fw_json(row, "wire", wire, sizeof wire)) return;
    if (!fw_field(wire, "q", q, sizeof q) || !fw_eq(q, "owner")) return;
    if (!fw_field(wire, "f", call, sizeof call)) return;
    if (!fw_field(wire, "k", k, sizeof k)) return;
    int known = find(call);
    /* A station is its key. A callsign is a few characters and anyone can
     * grind a key that derives it in a few thousand tries: a second key
     * for a callsign we already hold is somebody else, and is not believed
     * however well it signs. The key we hold was derived when we took it. */
    if (known >= 0 && g_st[known].npub[0]) {
        if (!fw_eq(g_st[known].npub, k)) return;
    } else if (!fw_call_matches(call, k)) {
        return;
    }
    int i = known >= 0 ? known : add(call);
    if (i < 0) return;
    st_t *s = &g_st[i];
    s->heard_ms = hal_time_ms();
    /* How it was heard, for the screen when it is next drawn: kept in
     * memory, written nowhere. */
    fw_json(row, "bearer", s->bearer, sizeof s->bearer);
    char r[12];
    if (fw_json(row, "rssi", r, sizeof r)) {
        int neg = r[0] == '-', v = 0;
        for (const char *p = r + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        s->rssi = neg ? -v : v;
    }
    /* The same ask comes back every half minute until somebody answers it:
     * only the first one of a run is news, and only news costs anything. */
    int news = known < 0 || !s->unowned || s->mine || s->theirs;
    if (!news) return;
    /* Erased and asking again: what we knew of it is what it was. */
    s->nick[0] = s->wifi[0] = s->ip[0] = s->ap[0] = s->zone[0] = 0;
    s->fw[0] = s->uptime[0] = s->peers[0] = s->heap[0] = s->reset[0] = 0;
    s->health[0] = s->slot[0] = s->radio[0] = s->crash[0] = s->mail[0] = 0;
    s->pol_owner[0] = s->pol_use[0] = s->pol_first[0] = s->pol_serve[0] = 0;
    fw_cpy(s->npub, k, sizeof s->npub);
    s->unowned = 1;
    s->mine = 0;                   /* erased and asking again: not ours now */
    s->theirs = 0;
    set_now(s, "Waiting for an owner");
    save();
    push_list();
    if (g_sel == i) push_hub();
    {
        char m[300] = "{\"type\":\"notify\",\"level\":\"info\",\"title\":\"A station nearby can be set up\",\"body\":\"";
        fw_jesc(m, call, sizeof m);
        fw_cat(m, " was just flashed and is waiting for an owner. Open Firmwares to claim it.\",\"tag\":\"firmwares.unowned.", sizeof m);
        fw_jesc(m, call, sizeof m);
        fw_cat(m, "\"}", sizeof m);
        say(m);
    }
}

/* Heard only while a station is changing its key (listen()). */
static void on_identity(const char *row)
{
    char wire[FW_WIRE_MAX + 1], call[16], k[80], sig[16];
    fw_json(row, "sig", sig, sizeof sig);
    if (!fw_eq(sig, "verified")) return;
    if (!fw_json(row, "wire", wire, sizeof wire)) return;
    if (!fw_field(wire, "f", call, sizeof call) || !fw_field(wire, "k", k, sizeof k)) return;
    if (!fw_call_matches(call, k)) return;
    fw_cpy(g_ids[g_ids_w].call, call, sizeof g_ids[0].call);
    fw_cpy(g_ids[g_ids_w].npub, k, sizeof g_ids[0].npub);
    g_ids_w = (g_ids_w + 1) % 4;
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (s->rekey_npub[0] && fw_eq(s->rekey_npub, k)) {
            /* The new key announcing itself: follow it (11.10). */
            char was[12];
            fw_cpy(was, s->call, sizeof was);
            fw_cpy(s->call, call, sizeof s->call);
            fw_cpy(s->npub, k, sizeof s->npub);
            s->rekey_npub[0] = 0;
            char line[80] = "Came back as ";
            fw_cat(line, call, sizeof line);
            log_line(was, line);
            set_now(s, line);
            save();
            push_list();
            if (g_sel == i) { push_hub(); screen_open("Station", s->nick[0] ? s->nick : s->call); }
            listen();
            return;
        }
    }
}

/* Heard only while a policy ask is out (listen()): the station's policy or
 * its mail count, as an observation addressed to us. */
static void on_observation(const char *row)
{
    char wire[FW_WIRE_MAX + 1], from[16], v[48], sub[12];
    if (!fw_json(row, "forUs", v, sizeof v) || !fw_eq(v, "true")) return;
    if (!fw_json(row, "sig", v, sizeof v) || !fw_eq(v, "verified")) return;
    if (!fw_json(row, "wire", wire, sizeof wire)) return;
    if (!fw_field(wire, "f", from, sizeof from)) return;
    int i = find(from);
    if (i < 0) return;
    st_t *s = &g_st[i];
    if (!fw_field(wire, "s", sub, sizeof sub)) return;
    if (fw_eq(sub, "policy")) {
        if (fw_field(wire, "owner", v, sizeof v)) fw_cpy(s->pol_owner, v, sizeof s->pol_owner);
        if (fw_field(wire, "use", v, sizeof v)) fw_cpy(s->pol_use, v, sizeof s->pol_use);
        if (fw_field(wire, "first", v, sizeof v)) fw_cpy(s->pol_first, v, sizeof s->pol_first);
        if (fw_field(wire, "serve", v, sizeof v)) fw_cpy(s->pol_serve, v, sizeof s->pol_serve);
        /* Who it belongs to, from its own mouth: mine, nobody's, or another's. */
        if (fw_eq(s->pol_owner, "none")) { s->unowned = 1; s->mine = 0; s->theirs = 0; }
        else if (fw_starts(s->pol_owner, g_me) &&
                 (s->pol_owner[fw_len(g_me)] == 0 || s->pol_owner[fw_len(g_me)] == ',')) {
            s->mine = 1; s->unowned = 0; s->theirs = 0;
        } else { s->theirs = 1; s->mine = 0; s->unowned = 0; }
        s->ask_pol = 0;
        if (!s->mine) s->asking = 0;          /* nothing more is coming */
        listen();
        save();
        push_list();
        if (g_sel == i) { push_hub(); push_stats(); }
    } else if (fw_eq(sub, "mail")) {
        if (fw_field(wire, "mail", v, sizeof v)) fw_cpy(s->mail, v, sizeof s->mail);
        if (g_sel == i) push_stats();
    }
}

static void on_result(const char *row)
{
    char wire[FW_WIRE_MAX + 1], from[16], r[8], code[8], m[100], v[16];
    /* The cheap tests first: most answers on the air are somebody else's. */
    if (!fw_json(row, "forUs", v, sizeof v) || !fw_eq(v, "true")) return;
    /* An answer is believed only when it is signed by the key the station
     * is known by (XPRS.md 9.1): the core says whether it is, and anything
     * else could be anybody claiming the station said yes. */
    if (!fw_json(row, "sig", v, sizeof v) || !fw_eq(v, "verified")) return;
    if (!fw_json(row, "wire", wire, sizeof wire)) return;
    fw_field(wire, "f", from, sizeof from);
    if (!fw_field(wire, "r", r, sizeof r) || !fw_field(wire, "code", code, sizeof code)) return;
    m[0] = 0;
    fw_field(wire, "m", m, sizeof m);

    int i = -1;
    for (int j = 0; j < g_nst; j++)
        if (g_st[j].pend_id[0] && fw_eq(g_st[j].pend_id, r)) { i = j; break; }
    if (i < 0) return;                        /* not something we asked */
    st_t *s = &g_st[i];
    /* A repeat of the command gets where the station stands again; an
     * answer that says nothing new is not news. */
    if (s->pend_final && fw_eq(code, "202")) return;
    if (!fw_eq(from, s->call)) {
        /* Only the station, or the station under its new key, answers: the
         * key its 202 named, or, when that was missed, one it announced and
         * the core verified, answering our key command by its r:. */
        char k2[16] = "";
        const char *nk = 0;
        if (s->rekey_npub[0]) {
            fw_call_of(s->rekey_npub, "X3", k2, sizeof k2);
            if (fw_eq(from, k2)) nk = s->rekey_npub;
        }
        static char rk[80];
        if (!nk && fw_eq(s->pend_what, "key")) {
            /* Its own key, in the answer (11.10), or one it announced. */
            if (fw_field(wire, "k", rk, sizeof rk) && fw_call_matches(from, rk)) nk = rk;
            for (int j = 0; !nk && j < 4; j++)
                if (fw_eq(g_ids[j].call, from)) nk = g_ids[j].npub;
        }
        if (!nk) return;
        fw_cpy(s->call, from, sizeof s->call);
        fw_cpy(s->npub, nk, sizeof s->npub);
        s->rekey_npub[0] = 0;
    }
    take_state(s, wire);
    s->heard_ms = hal_time_ms();
    char what[8];
    fw_cpy(what, s->pend_what, sizeof what);

    if (fw_eq(code, "202")) {
        char k[80];
        if (fw_eq(what, "key") && fw_field(wire, "k", k, sizeof k)) {
            fw_cpy(s->rekey_npub, k, sizeof s->rekey_npub);
            char nc[16], line[96] = "Restarting under a new identity, ";
            fw_call_of(k, "X3", nc, sizeof nc);
            fw_cat(line, nc, sizeof line);
            fw_cat(line, "...", sizeof line);
            log_line(s->call, line);
            set_now(s, line);
        } else if (fw_eq(what, "wifi")) {
            log_line(s->call, "Joining the network...");
            set_now(s, "Joining the network...");
        } else {
            log_line(s->call, "Working on it...");
            set_now(s, "Working on it...");
        }
        s->pend_final = 1;
    } else if (fw_eq(code, "200")) {
        if (fw_eq(what, "claim")) {
            s->mine = 1;
            s->unowned = 0;
            s->theirs = 0;
            log_line(s->call, "Claimed. It is yours.");
            set_now(s, "Yours. Put it on your WiFi, or give it a name.");
        } else if (fw_eq(what, "wifi") && s->next_x[0]) {
            /* The name went; now the password. */
            char ts[24], w[FW_WIRE_UNSIGNED + 1];
            stamp_for(s, ts, sizeof ts);
            int rc = fw_sealed(w, sizeof w, g_me, s->call, ts, s->next_x);
            for (unsigned q = 0; s->next_x[q]; q++) s->next_x[q] = 0;
            done_pending(s);
            if (rc > 0) send_cmd(s, w, "wifi");
            set_now(s, "Sending the password...");
            save(); push_list(); if (g_sel == i) push_hub();
            return;
        } else if (fw_eq(what, "wifi")) {
            char line[80];
            fw_cpy(line, fw_eq(s->wifi, "off") ? "Off the WiFi" : "On the network", sizeof line);
            if (s->ip[0]) { fw_cat(line, " at ", sizeof line); fw_cat(line, s->ip, sizeof line); }
            log_line(s->call, line);
            set_now(s, line);
        } else if (fw_eq(what, "key")) {
            log_line(s->call, "New identity in use.");
            set_now(s, "New identity in use.");
        } else if (fw_eq(what, "zdiag")) {
            s->asking = 0;
            save_stats(s);
            log_line(s->call, "Stats read.");
            set_now(s, "Stats read.");
        } else if (fw_eq(what, "zcore")) {
            char line[160] = "Crash: ";
            fw_cat(line, s->crash, sizeof line);
            if (m[0]) { fw_cat(line, " at ", sizeof line); fw_cat(line, m, sizeof line); }
            log_line(s->call, line);
            set_now(s, "Crash report in Answers.");
        } else {
            log_line(s->call, "Saved.");
            set_now(s, "Saved.");
        }
        done_pending(s);
    } else if (fw_eq(code, "206") && fw_eq(what, "zcore")) {
        char line[160] = "Crash, more: ";
        fw_cat(line, m, sizeof line);
        log_line(s->call, line);
        return;
    } else if (fw_eq(code, "408") && !s->pend_408) {
        /* Not newer than the last command it took: say it again, later. The
         * same x: under a new ts: is the same secret, re-signed. */
        s->pend_408 = 1;
        char ts[24], w[FW_WIRE_UNSIGNED + 1], x[FW_WIRE_UNSIGNED], rest[FW_WIRE_UNSIGNED];
        stamp_for(s, ts, sizeof ts);
        int rc = -1;
        if (fw_field(s->pend, "x", x, sizeof x)) {
            rc = fw_sealed(w, sizeof w, g_me, s->call, ts, x);
        } else {
            const char *c = s->pend;
            while (*c && !fw_starts(c, "cmd:")) c++;
            fw_cpy(rest, c, sizeof rest);
            fw_cpy(w, "t:command f:", sizeof w);
            fw_cat(w, g_me, sizeof w); fw_cat(w, " d:", sizeof w); fw_cat(w, s->call, sizeof w);
            fw_cat(w, " ts:", sizeof w); fw_cat(w, ts, sizeof w); fw_cat(w, " ", sizeof w);
            fw_cat(w, rest, sizeof w);
            rc = fw_len(w) <= FW_WIRE_UNSIGNED ? (int)fw_len(w) : -1;
        }
        done_pending(s);
        if (rc > 0) send_cmd(s, w, what);
        s->pend_408 = 1;              /* a second 408 is the clock, said once */
    } else {
        char line[160] = "";
        if (fw_eq(code, "403")) {
            fw_cpy(line, "Refused: ", sizeof line);
            if (fw_eq(what, "claim") || fw_starts(m, "not the owner")) {
                s->theirs = 1; s->unowned = 0; s->mine = 0;
            }
        }
        else if (fw_eq(code, "408")) fw_cpy(line, "Refused as old. Is this phone's clock right? ", sizeof line);
        else if (fw_eq(code, "404")) fw_cpy(line, "Its firmware cannot do that; update it first. ", sizeof line);
        else if (fw_eq(code, "429")) fw_cpy(line, "Busy, try again in a moment. ", sizeof line);
        else if (fw_eq(code, "500") && fw_eq(what, "wifi")) fw_cpy(line, "Could not join: ", sizeof line);
        else { fw_cpy(line, "Refused (", sizeof line); fw_cat(line, code, sizeof line); fw_cat(line, "): ", sizeof line); }
        fw_cat(line, m, sizeof line);
        log_line(s->call, line);
        set_now(s, line);
        if (fw_eq(what, "zdiag")) s->asking = 0;
        done_pending(s);
    }
    save();
    push_list();
    if (g_sel == i) { push_hub(); if (fw_eq(what, "zdiag") || fw_eq(what, "zcore")) push_stats(); }
}

/* The core gave up on a command: nothing final came back in 11.4's
 * window, however often it was aired. */
static void on_status(const char *row)
{
    char id[8], state[16];
    if (!fw_json(row, "id", id, sizeof id) || !fw_json(row, "state", state, sizeof state)) return;
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (!s->pend_id[0] || !fw_eq(s->pend_id, id)) continue;
        const char *line;
        if (fw_eq(state, "unfinished"))
            line = "It took the command but never said how it ended. Read its stats to see where it is.";
        else if (fw_eq(state, "unanswered"))
            line = "No answer in five minutes. Is it still in range, and powered?";
        else
            return;
        log_line(s->call, line);
        set_now(s, line);
        s->asking = 0;
        done_pending(s);
        if (g_sel == i) { push_hub(); push_stats(); }
        return;
    }
}

static void drain_events(void)
{
    while (hal_event_available()) {
        uint32_t n = hal_event_recv(g_topic, sizeof g_topic - 1, g_ev, sizeof g_ev - 1);
        g_ev[n] = 0;
        g_topic[sizeof g_topic - 1] = 0;
        if (fw_eq(g_topic, "xprs.request"))           on_request(g_ev);
        else if (fw_eq(g_topic, "xprs.result"))       on_result(g_ev);
        else if (fw_eq(g_topic, "xprs.status.tx"))    on_status(g_ev);
        else if (fw_eq(g_topic, "xprs.identity"))     on_identity(g_ev);
        else if (fw_eq(g_topic, "xprs.observation"))  on_observation(g_ev);
    }
}

/* ── What the person does ─────────────────────────────────────────────── */
static st_t *selected(void)
{
    if (g_sel < 0 || g_sel >= g_nst) { log_line("", "Pick a station first"); return 0; }
    return &g_st[g_sel];
}

static int fields(const char *key, char *out, unsigned cap)
{
    /* The host bundles every scalar field under "fields"; the name is unique
     * enough in this wapp that the flat scan finds the right one. */
    return fw_json(g_buf, key, out, cap);
}

static void on_command(void)
{
    char cmd[40] = "";
    if (!fw_json(g_buf, "command", cmd, sizeof cmd)) return;
    if (fw_eq(cmd, "ready") || fw_eq(cmd, "refresh")) {
        who_am_i();
        push_list();
        if (g_sel >= 0) push_hub();
        return;
    }
    if (fw_eq(cmd, "stations_tap")) {
        char id[16];
        if (!fields("stations_id", id, sizeof id)) return;
        g_sel = find(id);
        if (g_sel < 0) return;
        push_hub();
        screen_open("Station", g_st[g_sel].nick[0] ? g_st[g_sel].nick : id);
        return;
    }
    if (fw_eq(cmd, "back")) { say("{\"type\":\"ui.screen.close\"}"); return; }
    if (fw_eq(cmd, "log_clear")) { say("{\"type\":\"ui.log.clear\",\"field\":\"log\"}"); return; }

    st_t *s = selected();
    if (!s) return;
    if (!g_me[0]) who_am_i();

    /* Opening a task's screen sends nothing. */
    if (fw_eq(cmd, "open_wifi")) {
        one_line("wifi_now", "Now", s->now);
        screen_open("WiFi", s->call);
        return;
    }
    if (fw_eq(cmd, "open_name")) {
        field_set("nick", s->nick);
        field_set("zone", s->zone[0] ? s->zone : "auto");
        field_set("hotspot", "same");
        screen_open("Name", s->call);
        return;
    }
    if (fw_eq(cmd, "open_identity")) {
        field_set("identity", "new");
        field_set("nsec", "");
        screen_open("Identity", s->call);
        return;
    }
    if (fw_eq(cmd, "open_stats")) {
        /* Opening the screen is the ask: the policy and the mail count
         * from anybody's station, the diagnostics from one that is ours. */
        if (!s->pend_id[0] || s->pend_final) {
            if (s->pend_id[0]) done_pending(s);
            do_ask(s);
            if (s->mine) do_cmd(s, "cmd:zdiag", "zdiag");
            s->asking = 1;
        }
        push_stats();
        screen_open("Stats", s->call);
        return;
    }
    if (fw_eq(cmd, "open_answers")) { screen_open("Answers", s->call); return; }
    if (fw_eq(cmd, "forget")) {
        char was[12];
        fw_cpy(was, s->call, sizeof was);
        char k[24] = "st.";
        fw_cat(k, was, sizeof k);
        kv_put(k, "");
        drop(g_sel);
        g_sel = -1;
        save();
        push_list();
        say("{\"type\":\"ui.screen.close\"}");
        log_line(was, "Forgotten");
        return;
    }

    /* One command at a time. Once the station has taken one (a 202) it is
     * working on its own, and a new one may go. */
    if (s->pend_id[0] && !s->pend_final) { log_line(s->call, "Still waiting for its last answer"); return; }
    if (s->pend_id[0]) done_pending(s);

    if (fw_eq(cmd, "claim")) {
        do_claim(s);
    } else if (fw_eq(cmd, "wifi_apply")) {
        char ssid[40] = "", pass[72] = "";
        fields("ssid", ssid, sizeof ssid);
        fields("wifi_pass", pass, sizeof pass);
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (!ssid[0]) log_line(s->call, "Which network?");
        else if (pass[0] && (fw_len(pass) < 8 || fw_len(pass) > 63))
            log_line(s->call, "A WiFi password is 8 to 63 characters");
        else if (!pass[0]) {
            /* An open network: the name, sealed, then join. */
            char body[64];
            const char *kv[] = { "ssid", ssid, "wifi", "join", 0 };
            if (fw_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "wifi") == 0)
                set_now(s, "Sending an open network...");
        } else do_wifi(s, ssid, pass);
        for (unsigned i = 0; pass[i]; i++) pass[i] = 0;
        field_set("wifi_pass", "");
    } else if (fw_eq(cmd, "wifi_off")) {
        if (!s->mine) log_line(s->call, "Claim it first");
        else do_set(s, "wifi:off", "wifi");
    } else if (fw_eq(cmd, "station_apply")) {
        char nick[24] = "", zone[12] = "", ap[8] = "", f[80] = "";
        fields("nick", nick, sizeof nick);
        fields("zone", zone, sizeof zone);
        fields("hotspot", ap, sizeof ap);
        if (nick[0]) { fw_cat(f, "nick:", sizeof f); fw_cat(f, nick, sizeof f); }
        if (zone[0] && !fw_eq(zone, s->zone[0] ? s->zone : "auto")) {
            if (f[0]) fw_cat(f, " ", sizeof f);
            fw_cat(f, "zone:", sizeof f); fw_cat(f, zone, sizeof f);
        }
        if (fw_eq(ap, "on") || fw_eq(ap, "off")) {
            if (f[0]) fw_cat(f, " ", sizeof f);
            fw_cat(f, "ap:", sizeof f); fw_cat(f, ap, sizeof f);
        }
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (!f[0]) log_line(s->call, "Nothing to change");
        else do_set(s, f, "set");
    } else if (fw_eq(cmd, "identity_apply")) {
        char how[12] = "", nsec[72] = "";
        fields("identity", how, sizeof how);
        fields("nsec", nsec, sizeof nsec);
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (fw_eq(how, "import")) {
            if (!fw_starts(nsec, "nsec1") || fw_len(nsec) != 63) {
                log_line(s->call, "That is not an nsec");
            } else {
                char body[96];
                const char *kv[] = { "nsec", nsec, 0 };
                if (fw_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "key") == 0)
                    set_now(s, "Sending the key, sealed...");
            }
        } else do_set(s, "key:new", "key");
        for (unsigned i = 0; nsec[i]; i++) nsec[i] = 0;
        field_set("nsec", "");
    } else if (fw_eq(cmd, "stats")) {
        do_ask(s);
        s->asking = 1;
        if (s->mine) do_cmd(s, "cmd:zdiag", "zdiag");
        else set_now(s, "Asking what it does for others...");
        push_stats();
    } else if (fw_eq(cmd, "crash")) {
        if (!s->mine) log_line(s->call, "Only its owner may read that");
        else do_cmd(s, "cmd:zcore", "zcore");
    }
}

/* ── Entry points ─────────────────────────────────────────────────────── */
int32_t module_init(void)
{
    /* The one thing an idle phone listens for: a station asking to be
     * claimed. Answers and identities are listened for while asked (listen). */
    static const char t[] = "xprs.request";
    hal_event_subscribe(t, sizeof t - 1);
    who_am_i();
    load();
    push_list();
    return 0;
}

int32_t module_tick(void)
{
    drain_events();
    return 0;
}

int32_t module_handle_event(void)
{
    drain_events();
    uint32_t n = hal_msg_recv(g_buf, sizeof g_buf - 1);
    if (n == 0) return 0;
    g_buf[n] = 0;
    on_command();
    /* Every command carries every field, secrets included: whatever this one
     * was, the password it brought does not outlive it. */
    for (unsigned i = 0; i < n; i++) g_buf[i] = 0;
    return 0;
}

/* No clock: everything here happens because something arrived. */
int32_t module_tick_interval_ms(void) { return 0; }

void module_destroy(void) {}
