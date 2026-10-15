/*
 * alerts -- warnings and notices about this place (XPRS.md 24, 25, 25.3).
 *
 * An alert is a t:warning or t:info posted with scope:local and until:. It
 * goes out over Bluetooth and the local network and no further, everybody
 * nearby sees it, and a station whose owner posted it keeps it in its
 * archive and gives it, once, to everybody who arrives afterwards, until it
 * ends or is withdrawn. Nothing here is a new packet or a new key.
 *
 *   Nearby   what stands now, what is planned, and what this phone posted
 *   Post     type, how bad, what, a message, for how long, optionally where
 *   Alert    one of them; Withdraw when it is ours
 *
 * The wapp builds the words and hands them to the core (hal_xprs_send),
 * which signs and airs them; it never learns which bearer carried anything
 * (docs/architecture.md). It is told of every warning and notice the core
 * accepts on xprs.warning and xprs.info, which are rare topics, and reads
 * the archive once when it starts so that a restart forgets nothing. No
 * clock: a row that ends is dropped the next time the list is drawn.
 *
 * A new alert raises one notification, once per id, across restarts: the
 * ids already announced are kept in kv.
 */
#include "../hal/xprs_wasm_hal.h"
#include "util.h"
#include "alert.h"

#define AL_MAX     32             /* rows kept; a place has a handful */
#define GONE_MAX   32             /* withdrawals heard, applied as rows come */
#define SEEN_MAX   48             /* ids already announced, persisted */

static al_alert_t g_al[AL_MAX];
static int  g_nal;
static int  g_sel = -1;
static char g_me[16];
static struct { char id[7]; char from[16]; } g_gone[GONE_MAX];
static int  g_gone_w;
static char g_seen[SEEN_MAX][7];
static int  g_seen_w;

static char g_ev[4096];
static char g_topic[64];
static char g_buf[4096];
static char g_out[8192];
static char g_hist[24576];        /* one hal_xprs_history answer */
static char g_obj[1024];          /* one row of it */

static void say(const char *json) { hal_msg_send(json, al_len(json)); }

static void who_am_i(void)
{
    uint32_t n = hal_identity(g_me, sizeof g_me - 1);
    g_me[n] = 0;
}

static unsigned long long now_s(void) { return hal_time_epoch(); }

/* ── Announced ids, across restarts ───────────────────────────────────── */
static int seen(const char *id)
{
    for (int i = 0; i < SEEN_MAX; i++) if (al_eq(g_seen[i], id)) return 1;
    return 0;
}

static void seen_save(void)
{
    char v[SEEN_MAX * 7 + 1] = "";
    for (int i = 0; i < SEEN_MAX; i++) {
        if (!g_seen[i][0]) continue;
        if (v[0]) al_cat(v, ",", sizeof v);
        al_cat(v, g_seen[i], sizeof v);
    }
    hal_kv_set("seen", 4, v, al_len(v));
}

static void seen_add(const char *id)
{
    if (seen(id)) return;
    al_cpy(g_seen[g_seen_w], id, sizeof g_seen[0]);
    g_seen_w = (g_seen_w + 1) % SEEN_MAX;
    seen_save();
}

static void seen_load(void)
{
    char v[SEEN_MAX * 7 + 1];
    uint32_t n = hal_kv_get("seen", 4, v, sizeof v - 1);
    if (n == 0 || n >= sizeof v) return;
    v[n] = 0;
    const char *p = v;
    while (*p && g_seen_w < SEEN_MAX) {
        unsigned k = 0;
        while (p[k] && p[k] != ',') k++;
        if (k == 6) {
            for (unsigned i = 0; i < 6; i++) g_seen[g_seen_w][i] = p[i];
            g_seen[g_seen_w][6] = 0;
            g_seen_w++;
        }
        p += k;
        if (*p == ',') p++;
    }
    g_seen_w %= SEEN_MAX;
}

/* ── UI pieces ────────────────────────────────────────────────────────── */
static void log_host(const char *text)
{
    char l[200] = "alerts: ";
    al_cat(l, text, sizeof l);
    hal_log(1, l, al_len(l));
}

static void one_line(const char *field, const char *title, const char *text)
{
    char m[400] = "{\"type\":\"ui.field.set\",\"field\":\"";
    al_cat(m, field, sizeof m);
    if (!text || !text[0]) { al_cat(m, "\",\"value\":[]}", sizeof m); say(m); return; }
    al_cat(m, "\",\"value\":[{\"title\":\"\",\"items\":[{\"label\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\",\"value\":\"", sizeof m);
    al_jesc(m, text, sizeof m);
    al_cat(m, "\"}]}]}", sizeof m);
    say(m);
}

static void flag_hidden(const char *name, int hidden)
{
    char m[120] = "{\"type\":\"ui.field.set\",\"field\":\"";
    al_cat(m, name, sizeof m);
    al_cat(m, hidden ? "__hidden\",\"value\":true}" : "__hidden\",\"value\":false}", sizeof m);
    say(m);
}

static void screen_open(const char *name, const char *title)
{
    char m[160] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    al_cat(m, name, sizeof m);
    al_cat(m, "\",\"title\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* "Flood", "Works": the kind as a person says it. */
static void kind_words(const al_alert_t *a, char *out, unsigned cap)
{
    al_cpy(out, a->kind[0] ? a->kind : (al_eq(a->type, "warning") ? "warning" : "notice"), cap);
    out[0] = al_up(out[0]);
}

static void title_of(const al_alert_t *a, char *out, unsigned cap)
{
    kind_words(a, out, cap);
    if (al_eq(a->type, "warning") && a->sev[0]) {
        al_cat(out, " (", cap);
        al_cat(out, a->sev, cap);
        al_cat(out, ")", cap);
    }
}

/* ── The list ─────────────────────────────────────────────────────────── */
static int find(const char *id)
{
    for (int i = 0; i < g_nal; i++) if (al_eq(g_al[i].id, id)) return i;
    return -1;
}

static void item(const al_alert_t *a, unsigned long long now, int *first)
{
    char title[48], sub[240] = "", left[24], tags[120] = "";
    title_of(a, title, sizeof title);
    if (a->m[0]) { al_cat(sub, a->m, sizeof sub); al_cat(sub, ". ", sizeof sub); }
    al_cat(sub, a->mine ? "Posted by you" : "From ", sizeof sub);
    if (!a->mine) al_cat(sub, a->from, sizeof sub);
    al_left(left, sizeof left, now, a->until);
    if (left[0]) { al_cat(sub, ", ", sizeof sub); al_cat(sub, left, sizeof sub); }
    al_cat(tags, al_eq(a->type, "warning") ? "\"warning\"" : "\"notice\"", sizeof tags);
    if (al_eq(a->scope, "local")) al_cat(tags, ",\"here only\"", sizeof tags);
    if (a->pos[0]) al_cat(tags, ",\"has a place\"", sizeof tags);
    if (!*first) al_cat(g_out, ",", sizeof g_out);
    *first = 0;
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_jesc(g_out, a->id, sizeof g_out);
    al_cat(g_out, "\",\"title\":\"", sizeof g_out);
    al_jesc(g_out, title, sizeof g_out);
    al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    al_jesc(g_out, sub, sizeof g_out);
    al_cat(g_out, "\",\"tags\":[", sizeof g_out);
    al_cat(g_out, tags, sizeof g_out);
    al_cat(g_out, "]}", sizeof g_out);
}

/* Newest first: the rows in ts order, by selection, which for 32 is
 * nothing. */
static void push_list(void)
{
    const unsigned long long now = now_s();
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"alerts\",\"sections\":[", sizeof g_out);
    static const char *const titles[3] = {
        "{\"title\":\"Now\",\"items\":[",
        "{\"title\":\"Planned\",\"items\":[",
        "{\"title\":\"Posted by you\",\"items\":["
    };
    int any = 0;
    for (int pass = 0; pass < 3; pass++) {
        int first = 1;
        char done[AL_MAX] = { 0 };
        for (;;) {
            int best = -1;
            for (int i = 0; i < g_nal; i++) {
                if (done[i]) continue;
                const int st = al_state(&g_al[i], now);
                int in = pass == 0 ? (st == AL_NOW && !g_al[i].mine)
                       : pass == 1 ? (st == AL_PLANNED && !g_al[i].mine)
                       : (st != AL_GONE && g_al[i].mine);
                if (!in) { done[i] = 1; continue; }
                if (best < 0 || g_al[i].ts > g_al[best].ts) best = i;
            }
            if (best < 0) break;
            done[best] = 1;
            if (first) {
                if (any) al_cat(g_out, ",", sizeof g_out);
                al_cat(g_out, titles[pass], sizeof g_out);
                any = 1;
            }
            item(&g_al[best], now, &first);
        }
        if (!first) al_cat(g_out, "]}", sizeof g_out);
    }
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

static void push_alert(void)
{
    if (g_sel < 0 || g_sel >= g_nal) return;
    const al_alert_t *a = &g_al[g_sel];
    const unsigned long long now = now_s();
    char t[48], v[64];
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"alert_now\",\"value\":[{\"title\":\"\",\"items\":[", sizeof g_out);
    int first = 1;
    #define ROW(label, value) do { \
        if (!(value)[0]) break; \
        if (!first) al_cat(g_out, ",", sizeof g_out); \
        first = 0; \
        al_cat(g_out, "{\"label\":\"", sizeof g_out); \
        al_jesc(g_out, label, sizeof g_out); \
        al_cat(g_out, "\",\"value\":\"", sizeof g_out); \
        al_jesc(g_out, value, sizeof g_out); \
        al_cat(g_out, "\"}", sizeof g_out); } while (0)
    title_of(a, t, sizeof t);
    ROW("What", t);
    ROW("Message", a->m);
    ROW("From", a->mine ? "you" : a->from);
    al_stamp(v, sizeof v, a->ts);
    if (a->ts) ROW("Posted (UTC)", v);
    if (a->since) { al_stamp(v, sizeof v, a->since); ROW("Starts (UTC)", v); }
    if (a->until) { al_stamp(v, sizeof v, a->until); ROW("Ends (UTC)", v); }
    al_left(v, sizeof v, now, a->until);
    ROW("Left", v);
    ROW("Where", a->pos);
    ROW("Area", a->rad);
    ROW("Reach", al_eq(a->scope, "local") ? "here only" : "wider");
    ROW("Id", a->id);
    if (a->withdrawn) ROW("State", "withdrawn");
    #undef ROW
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
    flag_hidden("withdraw", !(a->mine && !a->withdrawn));
}

static void redraw(void)
{
    push_list();
    if (g_sel >= 0) push_alert();
}

/* ── What arrives ─────────────────────────────────────────────────────── */
static void apply_gone(al_alert_t *a)
{
    for (int g = 0; g < GONE_MAX; g++)
        if (g_gone[g].id[0] && al_eq(g_gone[g].id, a->id) && al_eq(g_gone[g].from, a->from))
            a->withdrawn = 1;
}

static void announce(const al_alert_t *a)
{
    char title[64] = "", t[48];
    title_of(a, t, sizeof t);
    al_cat(title, al_eq(a->type, "warning") ? "Warning nearby: " : "Notice nearby: ", sizeof title);
    al_cat(title, t, sizeof title);
    char m[600] = "{\"type\":\"notify\",\"level\":\"";
    int loud = al_eq(a->type, "warning") && (al_eq(a->sev, "warning") || al_eq(a->sev, "danger"));
    al_cat(m, loud ? "warning" : "info", sizeof m);
    al_cat(m, "\",\"title\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\",\"body\":\"", sizeof m);
    al_jesc(m, a->m[0] ? a->m : "Open Alerts to read it.", sizeof m);
    al_cat(m, "\",\"tag\":\"alerts.", sizeof m);
    al_cat(m, a->id, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/*
 * One warning or notice, from the air or the archive. [id] is the host's
 * section 5 identifier. Returns 1 when the list changed.
 */
static int take(const char *wire, const char *id, int may_announce)
{
    al_alert_t a;
    const int kind = al_parse(wire, &a);
    if (!kind) return 0;
    if (kind == 2) {
        /* 25.2: only the author withdraws, on this phone. A station also
         * obeys its owner (25.3); a phone has no owner to obey. */
        for (int g = 0; g < GONE_MAX; g++)
            if (al_eq(g_gone[g].id, a.id) && al_eq(g_gone[g].from, a.from)) return 0;
        al_cpy(g_gone[g_gone_w].id, a.id, sizeof g_gone[0].id);
        al_cpy(g_gone[g_gone_w].from, a.from, sizeof g_gone[0].from);
        g_gone_w = (g_gone_w + 1) % GONE_MAX;
        int i = find(a.id);
        if (i >= 0 && al_eq(g_al[i].from, a.from) && !g_al[i].withdrawn) {
            g_al[i].withdrawn = 1;
            return 1;
        }
        return 0;
    }
    if (!id || al_len(id) != 6) return 0;
    if (find(id) >= 0) return 0;          /* a copy: the id is the same */
    al_cpy(a.id, id, sizeof a.id);
    a.mine = g_me[0] && al_eq(a.from, g_me);
    apply_gone(&a);
    int slot = g_nal;
    if (g_nal >= AL_MAX) {
        /* Full: the oldest gone row makes room, else the oldest row. */
        const unsigned long long now = now_s();
        slot = -1;
        for (int i = 0; i < g_nal; i++)
            if (al_state(&g_al[i], now) == AL_GONE &&
                (slot < 0 || g_al[i].ts < g_al[slot].ts)) slot = i;
        if (slot < 0)
            for (int i = 0; i < g_nal; i++)
                if (slot < 0 || g_al[i].ts < g_al[slot].ts) slot = i;
        if (g_sel == slot) g_sel = -1;
    } else {
        g_nal++;
    }
    g_al[slot] = a;
    if (!a.mine && !seen(a.id)) {
        if (may_announce && al_state(&a, now_s()) != AL_GONE) announce(&a);
        seen_add(a.id);
    }
    return 1;
}

static int forged(const char *row)
{
    char sig[16];
    return al_json(row, "sig", sig, sizeof sig) && al_eq(sig, "forged");
}

static void on_packet(const char *row)
{
    char wire[AL_WIRE_MAX + 1], id[8];
    if (forged(row)) return;
    if (!al_json(row, "wire", wire, sizeof wire)) return;
    al_json(row, "id", id, sizeof id);
    if (take(wire, id, 1)) redraw();
}

/* What the archive holds, newest first: withdrawals before the alerts they
 * name, which take() handles either way round. Read when the engine starts,
 * so a restart or a phone that was off forgets nothing. */
static void backfill(void)
{
    static const char q[] = "{\"types\":[\"warning\",\"info\"],\"limit\":80}";
    int32_t n = hal_xprs_history(q, sizeof q - 1, g_hist, sizeof g_hist - 1);
    if (n <= 0) return;
    g_hist[n] = 0;
    const char *p = g_hist;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    char wire[AL_WIRE_MAX + 1], id[8];
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        if (forged(g_obj)) continue;
        if (!al_json(g_obj, "wire", wire, sizeof wire)) continue;
        al_json(g_obj, "id", id, sizeof id);
        take(wire, id, 1);
    }
}

static void drain_events(void)
{
    while (hal_event_available()) {
        uint32_t n = hal_event_recv(g_topic, sizeof g_topic - 1, g_ev, sizeof g_ev - 1);
        g_ev[n] = 0;
        g_topic[sizeof g_topic - 1] = 0;
        if (al_eq(g_topic, "xprs.warning") || al_eq(g_topic, "xprs.info"))
            on_packet(g_ev);
    }
}

/* ── What the person does ─────────────────────────────────────────────── */
static int fields(const char *key, char *out, unsigned cap)
{
    /* The host bundles every scalar field with the command; the names here
     * are unique, so the flat scan finds the right one. */
    return al_json(g_buf, key, out, cap);
}

/* The wire for the Post screen as it stands, or a reason in [why]. */
static int compose(char *wire, unsigned cap, char *why, unsigned wcap)
{
    char type[12] = "", sev[12] = "", kind[16] = "", text[200] = "";
    char dur[8] = "", here[8] = "", rad[12] = "";
    fields("a_type", type, sizeof type);
    fields("a_sev", sev, sizeof sev);
    fields("a_kind", kind, sizeof kind);
    fields("a_text", text, sizeof text);
    fields("a_for", dur, sizeof dur);
    fields("a_here", here, sizeof here);
    fields("a_rad", rad, sizeof rad);
    if (!type[0]) al_cpy(type, "warning", sizeof type);
    if (!sev[0]) al_cpy(sev, "watch", sizeof sev);
    if (!kind[0]) al_cpy(kind, "other", sizeof kind);
    if (!dur[0]) al_cpy(dur, "1d", sizeof dur);
    if (al_eq(rad, "spot")) rad[0] = 0;
    if (!al_kind_ok(type, kind)) {
        al_cpy(why, al_eq(type, "warning")
               ? "That is a notice, not a hazard: pick Notice as the type, or another What."
               : "That is a hazard: pick Warning as the type, or another What.", wcap);
        return -2;
    }
    const unsigned long long now = now_s();
    const unsigned long long life = al_duration(dur);
    if (!now || !life) { al_cpy(why, "This phone does not know the time yet.", wcap); return -2; }
    char pos[28] = "";
    if (al_eq(here, "true")) {
        int32_t lat = hal_sensor_gps_lat(), lon = hal_sensor_gps_lon();
        if (lat == (int32_t)0x80000000 || lon == (int32_t)0x80000000) {
            al_cpy(why, "No position yet. Try again in a moment, or turn off Mark where I am.", wcap);
            return -2;
        }
        char d[16];
        al_deg(pos, sizeof pos, lat);
        al_cat(pos, ",", sizeof pos);
        al_deg(d, sizeof d, lon);
        al_cat(pos, d, sizeof pos);
    } else {
        rad[0] = 0;
    }
    int n = al_build(wire, cap, g_me, type, kind, sev, pos, rad, now, now + life, text);
    if (n == -1) al_cpy(why, "The message is too long for one packet. Shorten it.", wcap);
    else if (n < 0) al_cpy(why, "The message has a character that cannot be sent.", wcap);
    return n;
}

static void do_post(void)
{
    char wire[AL_WIRE_MAX + 1], why[120] = "";
    who_am_i();
    int n = compose(wire, sizeof wire, why, sizeof why);
    if (n < 0) { one_line("post_now", "Not posted", why); return; }
    int rc = hal_xprs_send(wire, (uint32_t)n);
    if (rc != 0) {
        one_line("post_now", "Not posted", rc == -2 ? "This phone may not send that."
                                                    : "This phone could not send it.");
        return;
    }
    /* Our own row at once, keyed on the id the copy that comes back will
     * carry (docs/architecture.md, a wapp shows its own action at once). */
    char id[7];
    al_id(wire, (unsigned)n, id);
    take(wire, id, 0);
    one_line("post_now", "Posted", "It is on the Nearby tab. Stations you own nearby keep it and give it to whoever arrives.");
    log_host("posted an alert");
    push_list();
}

static void do_withdraw(void)
{
    if (g_sel < 0 || g_sel >= g_nal) return;
    al_alert_t *a = &g_al[g_sel];
    if (!a->mine || a->withdrawn) return;
    char wire[AL_WIRE_MAX + 1];
    int n = al_build_withdraw(wire, sizeof wire, g_me, a->type, a->id, now_s(), a->until);
    if (n < 0 || hal_xprs_send(wire, (uint32_t)n) != 0) {
        one_line("alert_now", "Not withdrawn", "This phone could not send it.");
        return;
    }
    take(wire, "", 0);                    /* marks ours withdrawn */
    log_host("withdrew an alert");
    redraw();
}

static void on_command(void)
{
    char cmd[40] = "";
    if (!al_json(g_buf, "command", cmd, sizeof cmd)) return;
    if (al_eq(cmd, "ready") || al_eq(cmd, "refresh")) {
        who_am_i();
        push_list();
        return;
    }
    if (al_eq(cmd, "alerts_tap")) {
        char id[8];
        if (!fields("alerts_id", id, sizeof id)) return;
        g_sel = find(id);
        if (g_sel < 0) return;
        char t[48];
        title_of(&g_al[g_sel], t, sizeof t);
        screen_open("Alert", t);
        push_alert();
        return;
    }
    if (al_eq(cmd, "post")) { do_post(); return; }
    if (al_eq(cmd, "withdraw")) { do_withdraw(); return; }
    if (al_eq(cmd, "back")) { g_sel = -1; say("{\"type\":\"ui.screen.close\"}"); return; }
}

/* ── Entry points ─────────────────────────────────────────────────────── */
int32_t module_init(void)
{
    /* Both topics always: an alert is news whether or not a page is open,
     * and both are rare (safety traffic and notices, not chatter). */
    static const char w[] = "xprs.warning", i[] = "xprs.info";
    hal_event_subscribe(w, sizeof w - 1);
    hal_event_subscribe(i, sizeof i - 1);
    who_am_i();
    seen_load();
    backfill();
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
    return 0;
}

/* No clock: everything here happens because something arrived. */
int32_t module_tick_interval_ms(void) { return 0; }

void module_destroy(void) {}
