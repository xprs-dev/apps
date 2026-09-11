/*
 * firmwares -- set up the station you just flashed (XPRS.md 11.9, 11.10).
 *
 * A freshly flashed station has no owner, no network and no name, and says
 * so: it airs `t:request q:owner` with its key in `k:` on its local bearers.
 * This wapp hears that on `xprs.request`, tells the person holding the phone
 * that a station nearby can be set up, and then, from the Station screen:
 *
 *   claim it        cmd:set owner:<me> k:<my npub>
 *   put it online   ssid and password, sealed in x: to the station's key
 *   name it         nick, time zone, its own hotspot on or off
 *   give it a key   a new one it makes itself, or an nsec, sealed
 *   read its stats  cmd:zdiag
 *
 * Every packet goes through hal_xprs_send and every secret through
 * hal_encrypt: the core owns the radio, the signature and the key, and this
 * wapp never learns which bearer carried anything (docs/architecture.md).
 * Passwords arrive in `$type:"secret"` fields, which the host never stores,
 * are sealed the moment they are read, and the buffers are cleared.
 *
 * Event-driven, no clock, and no transport. A command is handed to the core
 * once; the core airs it again until the station answers (docs/architecture.md
 * 1: retries are the core's) and says so on `xprs.status.tx` when it never
 * does. This wapp listens for answers only while it has asked something, and
 * for identities only while a station is changing its key: an idle phone
 * hands it nothing but the rare ask to be claimed.
 */
#include "../hal/xprs_wasm_hal.h"
#include "wire.h"

#define ST_MAX       8

typedef struct {
    char call[12];
    char npub[70];
    char nick[20], wifi[12], ip[20], ap[4], zone[8];
    char fw[24], uptime[16], peers[8], heap[20], reset[16];
    char bearer[8];
    int  rssi;
    unsigned long long heard_ms;
    int  unowned;                 /* asked to be claimed */
    int  mine;                    /* claimed by this profile */
    /* The one command in flight to it. The wire is kept only to say it
     * again after a 408, under a newer stamp. */
    char pend[FW_WIRE_UNSIGNED + 1];
    char pend_id[7];
    char pend_what[8];            /* claim wifi set key zdiag */
    int  pend_408, pend_final;
    char next_x[FW_WIRE_UNSIGNED];/* the second half of a long ssid+pass */
    char rekey_npub[70];          /* following a new key (11.10) */
    unsigned long long last_ts;   /* the last ts: we used with it */
} st_t;

static st_t g_st[ST_MAX];
static int  g_nst;
static int  g_sel = -1;
static int  g_sub_res, g_sub_tx, g_sub_ids;
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

static void kv_put(const char *k, const char *v) { hal_kv_set(k, fw_len(k), v, fw_len(v)); }

static void save(void)
{
    char list[ST_MAX * 13] = "";
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (!s->mine && !s->unowned) continue;
        if (list[0]) fw_cat(list, ",", sizeof list);
        fw_cat(list, s->call, sizeof list);
        char k[24] = "st.", v[200] = "";
        fw_cat(k, s->call, sizeof k);
        fw_cat(v, s->npub, sizeof v);
        fw_cat(v, s->mine ? "|1|" : "|0|", sizeof v);
        fw_cat(v, s->unowned ? "1|" : "0|", sizeof v);
        fw_cat(v, s->nick, sizeof v);
        fw_cat(v, "|", sizeof v);
        fw_cat(v, s->fw, sizeof v);
        kv_put(k, v);
    }
    kv_put("st.list", list);
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
        char k[24] = "st.", v[200];
        fw_cat(k, call, sizeof k);
        n = hal_kv_get(k, fw_len(k), v, sizeof v - 1);
        v[n] = 0;
        int i = add(call);
        if (i < 0) break;
        st_t *s = &g_st[i];
        /* npub|mine|unowned|nick|fw */
        char *f[5] = {0};
        f[0] = v;
        for (int j = 1, q = 0; v[q] && j < 5; q++)
            if (v[q] == '|') { v[q] = 0; f[j++] = v + q + 1; }
        fw_cpy(s->npub, f[0], sizeof s->npub);
        s->mine = f[1] && f[1][0] == '1';
        s->unowned = f[2] && f[2][0] == '1';
        if (f[3]) fw_cpy(s->nick, f[3], sizeof s->nick);
        if (f[4]) fw_cpy(s->fw, f[4], sizeof s->fw);
    }
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
     * is where a setup that went wrong gets read afterwards. */
    char l[200] = "firmwares: ";
    if (call && call[0]) { fw_cat(l, call, sizeof l); fw_cat(l, ": ", sizeof l); }
    fw_cat(l, text, sizeof l);
    hal_log(1, l, fw_len(l));
}

static void item(const char *id, const char *title, const char *sub, int *first)
{
    if (!*first) fw_cat(g_out, ",", sizeof g_out);
    *first = 0;
    fw_cat(g_out, "{\"id\":\"", sizeof g_out);
    fw_jesc(g_out, id, sizeof g_out);
    fw_cat(g_out, "\",\"title\":\"", sizeof g_out);
    fw_jesc(g_out, title, sizeof g_out);
    fw_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    fw_jesc(g_out, sub, sizeof g_out);
    fw_cat(g_out, "\"}", sizeof g_out);
}

static void push_list(void)
{
    fw_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":[",
           sizeof g_out);
    int any = 0;
    for (int pass = 0; pass < 2; pass++) {
        int first = 1, n = 0;
        for (int i = 0; i < g_nst; i++) {
            st_t *s = &g_st[i];
            if (pass == 0 ? !(s->unowned && !s->mine) : !s->mine) continue;
            if (first) {
                if (any) fw_cat(g_out, ",", sizeof g_out);
                fw_cat(g_out, pass == 0
                       ? "{\"title\":\"Waiting for an owner\",\"items\":["
                       : "{\"title\":\"Yours\",\"items\":[", sizeof g_out);
                any = 1;
            }
            char sub[96] = "";
            if (pass == 0) {
                fw_cpy(sub, "Freshly flashed. Tap to claim it", sizeof sub);
            } else {
                fw_cpy(sub, s->wifi[0] ? "WiFi " : "", sizeof sub);
                fw_cat(sub, s->wifi, sizeof sub);
                if (s->ip[0]) { fw_cat(sub, " ", sizeof sub); fw_cat(sub, s->ip, sizeof sub); }
                if (s->fw[0]) { fw_cat(sub, sub[0] ? ", firmware " : "firmware ", sizeof sub); fw_cat(sub, s->fw, sizeof sub); }
                if (!sub[0]) fw_cpy(sub, "Tap to set it up", sizeof sub);
            }
            item(s->call, s->nick[0] ? s->nick : s->call, sub, &first);
            n++;
        }
        if (!first) fw_cat(g_out, "]}", sizeof g_out);
        (void)n;
    }
    fw_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

static void detail(const char *label, const char *value, int *first)
{
    if (!value || !value[0]) return;
    if (!*first) fw_cat(g_out, ",", sizeof g_out);
    *first = 0;
    fw_cat(g_out, "{\"label\":\"", sizeof g_out);
    fw_jesc(g_out, label, sizeof g_out);
    fw_cat(g_out, "\",\"value\":\"", sizeof g_out);
    fw_jesc(g_out, value, sizeof g_out);
    fw_cat(g_out, "\"}", sizeof g_out);
}

static void push_detail(void)
{
    if (g_sel < 0 || g_sel >= g_nst) return;
    st_t *s = &g_st[g_sel];
    fw_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"detail\",\"value\":["
                  "{\"title\":\"Station\",\"items\":[", sizeof g_out);
    int first = 1;
    detail("Callsign", s->call, &first);
    detail("Owner", s->mine ? "You" : s->unowned ? "Nobody yet: claim it" : "Somebody else", &first);
    char heard[40] = "";
    if (s->bearer[0]) {
        for (unsigned j = 0; s->bearer[j] && j < 8; j++) {
            char c[2] = { fw_up(s->bearer[j]), 0 };
            fw_cat(heard, c, sizeof heard);
        }
        if (s->rssi) { fw_cat(heard, " at -", sizeof heard); fw_cat_u(heard, (unsigned)(s->rssi < 0 ? -s->rssi : s->rssi), sizeof heard); fw_cat(heard, " dBm", sizeof heard); }
    }
    detail("Heard over", heard, &first);
    detail("Key", s->npub, &first);
    fw_cat(g_out, "]},{\"title\":\"Network\",\"items\":[", sizeof g_out);
    first = 1;
    detail("WiFi", s->wifi, &first);
    detail("Address", s->ip, &first);
    detail("Hotspot", s->ap, &first);
    detail("Name", s->nick, &first);
    detail("Time zone", s->zone, &first);
    if (first) detail("Not known yet", "set it up or read its stats", &first);
    fw_cat(g_out, "]},{\"title\":\"Firmware\",\"items\":[", sizeof g_out);
    first = 1;
    detail("Version", s->fw, &first);
    detail("Up for", s->uptime, &first);
    detail("Stations it hears", s->peers, &first);
    detail("Free memory, KB (now/largest/lowest)", s->heap, &first);
    detail("Last reset", s->reset, &first);
    if (first) detail("Not read yet", "press Read stats", &first);
    fw_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
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

static void screen_open(const char *name, const char *title)
{
    char m[160] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    fw_cat(m, name, sizeof m);
    fw_cat(m, "\",\"title\":\"", sizeof m);
    fw_jesc(m, title, sizeof m);
    fw_cat(m, "\"}", sizeof m);
    say(m);
}

/* ── What to listen to ────────────────────────────────────────────────── */
static void sub(int *held, int want, const char *topic)
{
    if (want && !*held) hal_event_subscribe(topic, fw_len(topic));
    if (!want && *held) hal_event_unsubscribe(topic, fw_len(topic));
    *held = want;
}

/* Answers only while a command is out, identities only while a station is
 * changing its key. Everything else on those topics is somebody else's. */
static void listen(void)
{
    int asked = 0, keying = 0;
    for (int i = 0; i < g_nst; i++) {
        if (g_st[i].pend_id[0]) asked = 1;
        if (g_st[i].rekey_npub[0] ||
            (g_st[i].pend_id[0] && fw_eq(g_st[i].pend_what, "key"))) keying = 1;
    }
    sub(&g_sub_res, asked, "xprs.result");
    sub(&g_sub_tx, asked, "xprs.status.tx");
    sub(&g_sub_ids, keying, "xprs.identity");
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
    if (send_cmd(s, w, "claim") == 0) log_line(s->call, "Claiming it...");
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
        if (send_sealed(s, body, "wifi") == 0) log_line(s->call, "Sending the network, sealed to the station...");
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
                log_line(s->call, "Sending the network name, then the password...");
        }
        for (unsigned i = 0; body[i]; i++) body[i] = 0;
    }
    for (unsigned i = 0; pass[i]; i++) pass[i] = 0;
    for (unsigned i = 0; ssid[i]; i++) ssid[i] = 0;
    field_set("wifi_pass", "");
}

static void do_set(st_t *s, const char *fields, const char *what)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (fw_set(w, sizeof w, g_me, s->call, ts, fields) < 0) {
        log_line(s->call, "Too much to say in one packet");
        return;
    }
    if (send_cmd(s, w, what) == 0) log_line(s->call, "Sending...");
}

static void do_zdiag(st_t *s)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (fw_zdiag(w, sizeof w, g_me, s->call, ts) >= 0 && send_cmd(s, w, "zdiag") == 0)
        log_line(s->call, "Asking for its stats...");
}

/* ── What the station says ────────────────────────────────────────────── */
static void take_state(st_t *s, const char *wire)
{
    char v[40];
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
    /* A station is its key. A callsign is six characters and anyone can
     * grind a key that derives it in about a thousand tries: a second key
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
    /* How it was heard, for the Station screen when it is next drawn: kept
     * in memory, written nowhere. */
    fw_json(row, "bearer", s->bearer, sizeof s->bearer);
    char r[12];
    if (fw_json(row, "rssi", r, sizeof r)) {
        int neg = r[0] == '-', v = 0;
        for (const char *p = r + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        s->rssi = neg ? -v : v;
    }
    /* The same ask comes back every half minute until somebody answers it:
     * only the first one of a run is news, and only news costs anything. */
    int news = known < 0 || !s->unowned || s->mine;
    if (!news) return;
    /* Erased and asking again: what we knew of it is what it was. */
    s->nick[0] = s->wifi[0] = s->ip[0] = s->ap[0] = s->zone[0] = 0;
    s->fw[0] = s->uptime[0] = s->peers[0] = s->heap[0] = s->reset[0] = 0;
    fw_cpy(s->npub, k, sizeof s->npub);
    s->unowned = 1;
    s->mine = 0;                   /* erased and asking again: not ours now */
    save();
    push_list();
    if (g_sel == i) push_detail();
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
            save();
            push_list();
            if (g_sel == i) push_detail();
            listen();
            return;
        }
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

    if (fw_eq(code, "202")) {
        char k[80];
        if (fw_eq(s->pend_what, "key") && fw_field(wire, "k", k, sizeof k)) {
            fw_cpy(s->rekey_npub, k, sizeof s->rekey_npub);
            char nc[16], line[96] = "Restarting under a new identity, ";
            fw_call_of(k, "X3", nc, sizeof nc);
            fw_cat(line, nc, sizeof line);
            fw_cat(line, "...", sizeof line);
            log_line(s->call, line);
        } else if (fw_eq(s->pend_what, "wifi")) {
            log_line(s->call, "Joining the network...");
        } else {
            log_line(s->call, "Working on it...");
        }
        s->pend_final = 1;
    } else if (fw_eq(code, "200")) {
        if (fw_eq(s->pend_what, "claim")) {
            s->mine = 1;
            s->unowned = 0;
            log_line(s->call, "Claimed. It is yours; now put it on your network.");
        } else if (fw_eq(s->pend_what, "wifi") && s->next_x[0]) {
            /* The name went; now the password. */
            char ts[24], w[FW_WIRE_UNSIGNED + 1];
            stamp_for(s, ts, sizeof ts);
            int rc = fw_sealed(w, sizeof w, g_me, s->call, ts, s->next_x);
            for (unsigned q = 0; s->next_x[q]; q++) s->next_x[q] = 0;
            done_pending(s);
            if (rc > 0) send_cmd(s, w, "wifi");
            save(); push_list(); if (g_sel == i) push_detail();
            return;
        } else if (fw_eq(s->pend_what, "wifi")) {
            char line[80] = "On the network";
            if (s->ip[0]) { fw_cat(line, " at ", sizeof line); fw_cat(line, s->ip, sizeof line); }
            log_line(s->call, line);
        } else if (fw_eq(s->pend_what, "key")) {
            log_line(s->call, "New identity in use.");
        } else if (fw_eq(s->pend_what, "zdiag")) {
            log_line(s->call, "Stats updated.");
        } else {
            log_line(s->call, "Done.");
        }
        done_pending(s);
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
        char what[8];
        fw_cpy(what, s->pend_what, sizeof what);
        done_pending(s);
        if (rc > 0) send_cmd(s, w, what);
        s->pend_408 = 1;              /* a second 408 is the clock, said once */
    } else {
        char line[160] = "";
        if (fw_eq(code, "403"))      fw_cpy(line, "Refused: ", sizeof line);
        else if (fw_eq(code, "408")) fw_cpy(line, "Refused as old. Is this phone's clock right? ", sizeof line);
        else if (fw_eq(code, "404")) fw_cpy(line, "Its firmware cannot do that; update it first. ", sizeof line);
        else if (fw_eq(code, "429")) fw_cpy(line, "Busy, try again in a moment. ", sizeof line);
        else if (fw_eq(code, "500") && fw_eq(s->pend_what, "wifi")) fw_cpy(line, "Could not join: ", sizeof line);
        else { fw_cpy(line, "Refused (", sizeof line); fw_cat(line, code, sizeof line); fw_cat(line, "): ", sizeof line); }
        fw_cat(line, m, sizeof line);
        log_line(s->call, line);
        done_pending(s);
    }
    save();
    push_list();
    if (g_sel == i) push_detail();
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
        if (fw_eq(state, "unfinished"))
            log_line(s->call, "It took the command but never said how it ended. Read its stats to see where it is.");
        else if (fw_eq(state, "unanswered"))
            log_line(s->call, "No answer in five minutes. Is it still in range, and powered?");
        else
            return;
        done_pending(s);
        if (g_sel == i) push_detail();
        return;
    }
}

static void drain_events(void)
{
    while (hal_event_available()) {
        uint32_t n = hal_event_recv(g_topic, sizeof g_topic - 1, g_ev, sizeof g_ev - 1);
        g_ev[n] = 0;
        g_topic[sizeof g_topic - 1] = 0;
        if (fw_eq(g_topic, "xprs.request"))        on_request(g_ev);
        else if (fw_eq(g_topic, "xprs.result"))    on_result(g_ev);
        else if (fw_eq(g_topic, "xprs.status.tx")) on_status(g_ev);
        else if (fw_eq(g_topic, "xprs.identity"))  on_identity(g_ev);
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
        if (g_sel >= 0) push_detail();
        return;
    }
    if (fw_eq(cmd, "stations_tap")) {
        char id[16];
        if (!fields("stations_id", id, sizeof id)) return;
        g_sel = find(id);
        if (g_sel < 0) return;
        push_detail();
        screen_open("Station", g_st[g_sel].nick[0] ? g_st[g_sel].nick : id);
        return;
    }
    if (fw_eq(cmd, "back")) { say("{\"type\":\"ui.screen.close\"}"); return; }

    st_t *s = selected();
    if (!s) return;
    if (!g_me[0]) who_am_i();
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
                log_line(s->call, "Sending an open network...");
        } else do_wifi(s, ssid, pass);
        for (unsigned i = 0; pass[i]; i++) pass[i] = 0;
    } else if (fw_eq(cmd, "station_apply")) {
        char nick[24] = "", zone[12] = "", ap[8] = "", f[80] = "";
        fields("nick", nick, sizeof nick);
        fields("zone", zone, sizeof zone);
        fields("hotspot", ap, sizeof ap);
        if (nick[0]) { fw_cat(f, "nick:", sizeof f); fw_cat(f, nick, sizeof f); }
        if (zone[0]) { if (f[0]) fw_cat(f, " ", sizeof f); fw_cat(f, "zone:", sizeof f); fw_cat(f, zone, sizeof f); }
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
        else if (fw_eq(how, "new")) do_set(s, "key:new", "key");
        else if (fw_eq(how, "import")) {
            if (!fw_starts(nsec, "nsec1") || fw_len(nsec) != 63) {
                log_line(s->call, "That is not an nsec");
            } else {
                char body[96];
                const char *kv[] = { "nsec", nsec, 0 };
                if (fw_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "key") == 0)
                    log_line(s->call, "Sending the key, sealed to the station...");
            }
        } else log_line(s->call, "It keeps the key it has");
        for (unsigned i = 0; nsec[i]; i++) nsec[i] = 0;
        field_set("nsec", "");
    } else if (fw_eq(cmd, "stats")) {
        if (!s->mine) log_line(s->call, "Only its owner may read its stats");
        else do_zdiag(s);
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
