/*
 * checkin -- a logbook of meetings over the air, chased like geocaches.
 *
 * A chaser near a station asks it for a check-in: a signed operator command,
 * cmd:checkin, scoped local (in person) or regional (LoRa too). The station's
 * copy of this wapp answers with the command's t:result, signed: the ask's
 * identifier, the radio it was heard on, its signal and relays, the station's
 * position, the event running, and the HASH of the badge it hands out. The
 * two packets together are the evidence. Nothing here is new to XPRS
 * (ci.h).
 *
 *   Nearby      stations in reach now, a Check in button on each
 *   Logbook     every check-in, counted; tap one for the evidence
 *   Badges      the wall: one row per badge earned, with how many times
 *   Events      check-in events stations announced (t:event tag:checkin)
 *   My station  the bot: on or off, in person only, greeting, badge,
 *               events, and the guestbook of visitors
 *
 * The wapp decides what a check-in means; the core does everything else
 * (docs/architecture.md). hal_xprs_send hands the ask to the core's command
 * courier, which airs it again until it is answered and says so on
 * xprs.status.tx when it never is; the wapp listens to xprs.result and
 * xprs.status.tx only while an ask is out. The bot listens to xprs.command
 * while it is on, and to nothing when it is off. A badge is a file: hash:
 * the station imports the picture into the media store and describes it in a
 * t:file publication, which the core advertises and places with the
 * station's archivers; a chaser asks the core for the hash once
 * (hal_media_fetch) and draws it from the store. The wapp never sees bytes
 * and never names a lane.
 */
#include "../hal/xprs_wasm_hal.h"
#include "util.h"
#include "ci.h"

#define PEND_MAX 8

static char g_me[16];
static int32_t g_db = -1;
static char g_buf[4096];          /* one command from the page */
static char g_ev[4096];           /* one event */
static char g_topic[64];
static char g_out[16384];         /* one message to the page */
static char g_rows[24576];        /* one query answer */
static char g_obj[4096];          /* one row of it */
static char g_big[32768];         /* hal_xprs_stations / history */

/* Settings, in kv. */
static int  g_bot_on;
static int  g_local_only;
static int  g_chase_regional;
static char g_greeting[160];
static char g_badge[80];          /* the station's badge token, file:<ref> */
static char g_intro[104];         /* a line about this station, for its pin */
static unsigned long long g_point_at;   /* when the check-in point was last announced */

/* Where the map looks (the host says on every pan, as setViewport). */
static char g_view_lat[24] = "", g_view_lon[24] = "";
static int  g_view_zoom = 2;
static int  g_map_fitted;         /* the map was centred on the check-ins once */

/* Asks out: the core delivers them; this is only what to match answers to. */
static struct { char id[7]; char station[16]; unsigned long long ts; char wire[CI_WIRE_MAX + 1]; } g_pend[PEND_MAX];
static int g_listening;           /* subscribed to the answer topics */
static unsigned long long g_greet_until; /* take a greeting until then */
static char g_greet_from[16];

/* The entry open on the Entry screen, and the event being written. */
static char g_sel[8];
static struct { char badge[80]; } g_draft;

static void say(const char *json) { hal_msg_send(json, al_len(json)); }
static unsigned long long now_s(void) { return hal_time_epoch(); }
static int zone(void) { return (int)hal_time_utc_offset(); }

static void log_host(const char *text)
{
    char l[200] = "checkin: ";
    al_cat(l, text, sizeof l);
    hal_log(1, l, al_len(l));
}

static void who_am_i(void)
{
    uint32_t n = hal_identity(g_me, sizeof g_me - 1);
    g_me[n] = 0;
}

static void notify(const char *level, const char *title, const char *body)
{
    char m[600] = "{\"type\":\"notify\",\"level\":\"";
    al_cat(m, level, sizeof m);
    al_cat(m, "\",\"title\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\",\"body\":\"", sizeof m);
    al_jesc(m, body, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* The answer to what the person just did, on the page (ui.snack). The bell
 * is for what happens while nobody looks: a notification from the app that
 * is on screen is not shown at all, so the page's own feedback must not go
 * there. */
static void toast(const char *level, const char *text)
{
    char m[600] = "{\"type\":\"ui.snack\",\"level\":\"";
    al_cat(m, level, sizeof m);
    al_cat(m, "\",\"text\":\"", sizeof m);
    al_jesc(m, text, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* Both: news that may arrive with the page open (snackbar) or closed (bell). */
static void tell(const char *level, const char *text)
{
    toast(level, text);
    notify(level, "Check-in", text);
}

static void field_set_str(const char *field, const char *value)
{
    char m[600] = "{\"type\":\"ui.field.set\",\"field\":\"";
    al_cat(m, field, sizeof m);
    al_cat(m, "\",\"value\":\"", sizeof m);
    al_jesc(m, value, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

static void field_set_bool(const char *field, int v)
{
    char m[160] = "{\"type\":\"ui.field.set\",\"field\":\"";
    al_cat(m, field, sizeof m);
    al_cat(m, v ? "\",\"value\":true}" : "\",\"value\":false}", sizeof m);
    say(m);
}

static void screen_open(const char *name, const char *title)
{
    char m[200] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    al_cat(m, name, sizeof m);
    al_cat(m, "\",\"title\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* "Thu 17 Oct 2026, 15:16" is more than this needs: "2026-10-17 15:16". */
static void when(char *out, unsigned cap, unsigned long long epoch)
{
    char s[24];
    long long t = (long long)epoch + (long long)zone() * 60;
    al_stamp(s, sizeof s, t < 0 ? 0 : (unsigned long long)t);
    s[10] = ' ';
    s[16] = 0;
    al_cpy(out, s, cap);
}

/* ── settings ─────────────────────────────────────────────────────────── */
static int kv_bool(const char *k, int dflt)
{
    char v[4];
    uint32_t n = hal_kv_get(k, al_len(k), v, sizeof v - 1);
    return n ? v[0] == '1' : dflt;
}

static void kv_put_bool(const char *k, int v)
{
    hal_kv_set(k, al_len(k), v ? "1" : "0", 1);
}

static void kv_str(const char *k, char *out, unsigned cap)
{
    uint32_t n = hal_kv_get(k, al_len(k), out, cap - 1);
    out[n < cap ? n : cap - 1] = 0;
}

static void load_settings(void)
{
    g_bot_on = kv_bool("bot", 0);
    g_local_only = kv_bool("local", 1);
    g_chase_regional = kv_bool("chase_reg", 0);
    kv_str("greet", g_greeting, sizeof g_greeting);
    kv_str("badge", g_badge, sizeof g_badge);
    kv_str("intro", g_intro, sizeof g_intro);
}

/* ── the database ─────────────────────────────────────────────────────── */
/* Bind parameters as the JSON array hal_db_* takes. */
static char g_params[2048];
static void p_begin(void) { al_cpy(g_params, "[", sizeof g_params); }
static void p_sep(void) { if (al_len(g_params) > 1) al_cat(g_params, ",", sizeof g_params); }
static void p_str(const char *s)
{
    p_sep();
    al_cat(g_params, "\"", sizeof g_params);
    al_jesc(g_params, s, sizeof g_params);
    al_cat(g_params, "\"", sizeof g_params);
}
static void p_int(unsigned long long v) { p_sep(); al_cat_u(g_params, v, sizeof g_params); }
static void p_end(void) { al_cat(g_params, "]", sizeof g_params); }

static int db_exec(const char *sql, int with_params)
{
    if (g_db < 0) return -1;
    return hal_db_exec(g_db, sql, al_len(sql),
                       with_params ? g_params : 0, with_params ? al_len(g_params) : 0);
}

/* Rows into g_rows; the number of bytes, 0 for none or an error. */
static int db_query(const char *sql, int with_params)
{
    if (g_db < 0) return 0;
    int32_t n = hal_db_query(g_db, sql, al_len(sql),
                             with_params ? g_params : 0, with_params ? al_len(g_params) : 0,
                             g_rows, sizeof g_rows - 1);
    if (n <= 0) { g_rows[0] = 0; return 0; }
    g_rows[n] = 0;
    return n;
}

/* The first row's value of [col], as text. */
static int db_one(const char *sql, int with_params, const char *col, char *out, unsigned cap)
{
    out[0] = 0;
    if (!db_query(sql, with_params)) return 0;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    if (!al_json_next(p, g_obj, sizeof g_obj)) return 0;
    return al_json(g_obj, col, out, cap);
}

static void db_init(void)
{
    static const char path[] = "checkin.db";
    g_db = hal_db_open(path, sizeof path - 1);
    if (g_db < 0) { log_host("no database"); return; }
    db_exec("CREATE TABLE IF NOT EXISTS log (id TEXT PRIMARY KEY, station TEXT,"
            " ts INTEGER, link TEXT, grade TEXT, rssi TEXT, route TEXT, pos TEXT,"
            " title TEXT, badge TEXT, cert TEXT, ask TEXT, greeting TEXT)", 0);
    db_exec("CREATE TABLE IF NOT EXISTS guests (id TEXT PRIMARY KEY, visitor TEXT,"
            " ts INTEGER, day INTEGER, link TEXT, grade TEXT, title TEXT, cert TEXT)", 0);
    db_exec("CREATE TABLE IF NOT EXISTS events (title TEXT PRIMARY KEY,"
            " since INTEGER, until INTEGER, badge TEXT, m TEXT)", 0);
    db_exec("CREATE TABLE IF NOT EXISTS stations (call TEXT PRIMARY KEY, last INTEGER)", 0);
    /* Asks out, so an answer that arrives after the page closed is still
     * matched by whichever engine runs then (the background one, when the
     * owner turned it on). Results are never archived: missed is lost. */
    db_exec("CREATE TABLE IF NOT EXISTS asks (id TEXT PRIMARY KEY, station TEXT,"
            " ts INTEGER, wire TEXT)", 0);
}

/* ── subscriptions ────────────────────────────────────────────────────── */
static void sub(const char *t, int on)
{
    if (on) hal_event_subscribe(t, al_len(t));
    else hal_event_unsubscribe(t, al_len(t));
}

static int pending_count(void)
{
    int n = 0;
    for (int i = 0; i < PEND_MAX; i++) if (g_pend[i].id[0]) n++;
    return n;
}

/* Listen for answers only while an ask is out or a greeting may come. */
static void listen_update(void)
{
    int want = pending_count() > 0 || (g_greet_until && now_s() < g_greet_until);
    if (want == g_listening) return;
    g_listening = want;
    sub("xprs.result", want);
    sub("xprs.status.tx", want);
    sub("xprs.message", want);
}

static void pend_drop(int i)
{
    p_begin(); p_str(g_pend[i].id); p_end();
    db_exec("DELETE FROM asks WHERE id = ?", 1);
    g_pend[i].id[0] = 0;
}

/* The asks still out from before this engine started: answers to them can
 * still come for 11.4's five minutes, and the courier's report after that. */
static void pend_load(void)
{
    unsigned long long now = now_s();
    p_begin(); p_int(now > 900 ? now - 900 : 0); p_end();
    db_exec("DELETE FROM asks WHERE ts < ?", 1);
    if (!db_query("SELECT id, station, ts, wire FROM asks ORDER BY ts DESC LIMIT 8", 0)) return;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    for (int i = 0; i < PEND_MAX && (p = al_json_next(p, g_obj, sizeof g_obj)) != 0; i++) {
        char ts[24];
        al_json(g_obj, "id", g_pend[i].id, sizeof g_pend[i].id);
        al_json(g_obj, "station", g_pend[i].station, sizeof g_pend[i].station);
        al_json(g_obj, "wire", g_pend[i].wire, sizeof g_pend[i].wire);
        al_json(g_obj, "ts", ts, sizeof ts);
        g_pend[i].ts = 0;
        for (const char *q = ts; *q >= '0' && *q <= '9'; q++) g_pend[i].ts = g_pend[i].ts * 10 + (unsigned)(*q - '0');
    }
}

static void bot_update(void)
{
    static int on = -1;
    if (on == g_bot_on) return;
    on = g_bot_on;
    sub("xprs.command", g_bot_on);
}

/* ── Nearby ───────────────────────────────────────────────────────────── */
static int g_first;
static void rows_begin(const char *field)
{
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"", sizeof g_out);
    al_cat(g_out, field, sizeof g_out);
    al_cat(g_out, "\",\"sections\":[", sizeof g_out);
}
static void section_begin(const char *title, int *any)
{
    if (*any) al_cat(g_out, ",", sizeof g_out);
    *any = 1;
    al_cat(g_out, "{\"title\":\"", sizeof g_out);
    al_jesc(g_out, title, sizeof g_out);
    al_cat(g_out, "\",\"items\":[", sizeof g_out);
    g_first = 1;
}
static void section_end(void) { al_cat(g_out, "]}", sizeof g_out); }
static void rows_end(void) { al_cat(g_out, "]}", sizeof g_out); say(g_out); }

/* An icon for the next row() that has no picture: a row about a THING (a
 * badge not chosen yet, an event, a check-in without a badge) reads better as
 * an icon than as a generated face. Reset by row(). */
static const char *g_icon;

/* One row. [avatar] a file: token or "", [call] the station it is about. */
static void row(const char *id, const char *title, const char *sub_,
                const char *t1, const char *t2, const char *avatar,
                const char *call, const char *action, const char *label)
{
    if (!g_first) al_cat(g_out, ",", sizeof g_out);
    g_first = 0;
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_jesc(g_out, id, sizeof g_out);
    al_cat(g_out, "\",\"title\":\"", sizeof g_out);
    al_jesc(g_out, title, sizeof g_out);
    al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    al_jesc(g_out, sub_, sizeof g_out);
    al_cat(g_out, "\",\"tags\":[", sizeof g_out);
    if (t1 && t1[0]) { al_cat(g_out, "\"", sizeof g_out); al_jesc(g_out, t1, sizeof g_out); al_cat(g_out, "\"", sizeof g_out); }
    if (t2 && t2[0]) {
        if (t1 && t1[0]) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out); al_jesc(g_out, t2, sizeof g_out); al_cat(g_out, "\"", sizeof g_out);
    }
    al_cat(g_out, "]", sizeof g_out);
    if (avatar && avatar[0]) {
        al_cat(g_out, ",\"avatar\":\"", sizeof g_out);
        al_jesc(g_out, avatar, sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out);
    }
    if (g_icon && (!avatar || !avatar[0])) {
        al_cat(g_out, ",\"icon\":\"", sizeof g_out);
        al_cat(g_out, g_icon, sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out);
    }
    if (call && call[0] && !g_icon) {
        al_cat(g_out, ",\"call\":\"", sizeof g_out);
        al_jesc(g_out, call, sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out);
    }
    if (action && action[0]) {
        al_cat(g_out, ",\"action\":\"", sizeof g_out);
        al_cat(g_out, action, sizeof g_out);
        al_cat(g_out, "\",\"actionLabel\":\"", sizeof g_out);
        al_jesc(g_out, label, sizeof g_out);
        al_cat(g_out, "\",\"actionStyle\":\"filled\"", sizeof g_out);
    }
    al_cat(g_out, "}", sizeof g_out);
    g_icon = 0;
}

static int log_count_for(const char *call)
{
    char v[16];
    p_begin(); p_str(call); p_end();
    if (!db_one("SELECT COUNT(*) AS n FROM log WHERE station = ?", 1, "n", v, sizeof v)) return 0;
    int n = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
    return n;
}

static int known_station(const char *call)
{
    char v[16];
    p_begin(); p_str(call); p_end();
    return db_one("SELECT call FROM stations WHERE call = ?", 1, "call", v, sizeof v) && v[0];
}

/* Nearby's rows, gathered before they are drawn: the host lists stations
 * most recently heard first, and with the list following the air that order
 * moved rows under the person's finger. Stations known to give check-ins come
 * first, then by callsign: a station stays where it was. */
#define NEAR_MAX 64
static struct { char id[16]; char sub[48]; int known; } g_near[NEAR_MAX];

static int near_before(int a, int b)
{
    if (g_near[a].known != g_near[b].known) return g_near[a].known > g_near[b].known;
    return al_cmp(g_near[a].id, g_near[b].id) < 0;
}

static void push_nearby(void)
{
    int32_t n = hal_xprs_stations(g_big, sizeof g_big - 1);
    int count = 0;
    if (n > 0) {
        g_big[n] = 0;
        const char *p = g_big;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        /* The first section is "heard over the air" in the last eleven
         * minutes: who a check-in can reach. The others are not in reach. */
        if (al_json_next(p, g_obj, sizeof g_obj)) {
            const char *it = al_json_arr(g_obj, "items");
            static char item[1024];
            while (count < NEAR_MAX && it && (it = al_json_next(it, item, sizeof item)) != 0) {
                char id[16], kind[12];
                al_json(item, "id", id, sizeof id);
                al_json(item, "kind", kind, sizeof kind);
                if (!id[0] || al_eq(kind, "device") || al_eq(id, g_me)) continue;
                al_cpy(g_near[count].id, id, sizeof g_near[count].id);
                al_json(item, "subtitle", g_near[count].sub, sizeof g_near[count].sub);
                g_near[count].known = known_station(id);
                count++;
            }
        }
    }
    /* Insertion sort: a few dozen rows. */
    int order[NEAR_MAX];
    for (int i = 0; i < count; i++) {
        int j = i;
        while (j > 0 && near_before(i, order[j - 1])) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    rows_begin("nearby");
    int any = 0;
    section_begin("In reach now", &any);
    for (int k = 0; k < count; k++) {
        const char *id = g_near[order[k]].id;
        char tag[32] = "", tag2[32] = "";
        int had = log_count_for(id);
        if (had) {
            al_cpy(tag, "checked in ", sizeof tag);
            al_cat_u(tag, (unsigned long long)had, sizeof tag);
            al_cat(tag, had == 1 ? " time" : " times", sizeof tag);
        }
        if (g_near[order[k]].known) al_cpy(tag2, "gives check-ins", sizeof tag2);
        int asking = 0;
        for (int q = 0; q < PEND_MAX; q++)
            if (g_pend[q].id[0] && al_eq(g_pend[q].station, id)) asking = 1;
        if (asking) row(id, id, g_near[order[k]].sub, tag, tag2, "", id, "nearby_asking", "Asking...");
        else row(id, id, g_near[order[k]].sub, tag, tag2, "", id, "nearby_checkin", "Check in");
    }
    section_end();
    rows_end();
    field_set_bool("chase_regional", g_chase_regional);
}

/* ── checking in ──────────────────────────────────────────────────────── */
static int pend_find(const char *id)
{
    for (int i = 0; i < PEND_MAX; i++) if (g_pend[i].id[0] && al_eq(g_pend[i].id, id)) return i;
    return -1;
}

static void do_checkin(const char *station)
{
    char wire[CI_WIRE_MAX + 1];
    who_am_i();
    unsigned long long now = now_s();
    int n = ci_build_ask(wire, sizeof wire, g_me, station, now,
                         g_chase_regional ? "regional" : "local");
    if (n < 0) { toast("warning", "No callsign yet: set up your profile first."); return; }
    for (int i = 0; i < PEND_MAX; i++)
        if (g_pend[i].id[0] && al_eq(g_pend[i].station, station)) {
            toast("info", "Already asking that station; wait for its answer.");
            return;
        }
    int slot = -1;
    for (int i = 0; i < PEND_MAX; i++) if (!g_pend[i].id[0]) { slot = i; break; }
    if (slot < 0) { toast("warning", "Too many asks out; wait for an answer."); return; }
    int rc = hal_xprs_send(wire, (uint32_t)n);
    if (rc != 0) { toast("warning", "This device could not send the ask."); return; }
    /* The identifier of the wire we built is the one the signed copy has:
     * a signature changes neither f:, ts: nor the payload (XPRS.md 5). */
    al_id(wire, (unsigned)n, g_pend[slot].id);
    al_cpy(g_pend[slot].station, station, sizeof g_pend[slot].station);
    al_cpy(g_pend[slot].wire, wire, sizeof g_pend[slot].wire);
    g_pend[slot].ts = now;
    push_nearby();
    p_begin(); p_str(g_pend[slot].id); p_str(station); p_int(now); p_str(wire); p_end();
    db_exec("INSERT OR REPLACE INTO asks (id, station, ts, wire) VALUES (?,?,?,?)", 1);
    listen_update();
    char b[96] = "Asking ";
    al_cat(b, station, sizeof b);
    al_cat(b, " for a check-in...", sizeof b);
    toast("info", b);
}

/* The signed copy of our own ask, from the archive; the unsigned one we
 * built when the archive does not have it yet. */
static void own_ask(const char *id, const char *built, char *out, unsigned cap)
{
    char q[48] = "{\"id\":\"";
    al_cat(q, id, sizeof q);
    al_cat(q, "\"}", sizeof q);
    al_cpy(out, built, cap);
    int32_t n = hal_xprs_history(q, al_len(q), g_big, sizeof g_big - 1);
    if (n <= 0) return;
    g_big[n] = 0;
    char w[CI_WIRE_MAX + 64];
    if (al_json(g_big, "wire", w, sizeof w) && w[0]) al_cpy(out, w, cap);
}

static void fetch_badge(const char *ref)
{
    char tok[80] = "file:";
    if (!ref[0]) return;
    al_cat(tok, ref, sizeof tok);
    hal_media_fetch(tok, al_len(tok));
}

static void push_log(void);
static void push_badges(void);
static void push_map(void);
static int has_tag(const char *wire, const char *tag);
static void label_name(char *out, unsigned cap, const char *label);

static void on_result(const char *row_)
{
    char wire[CI_WIRE_MAX + 64], sig[12];
    ci_cert_t c;
    if (!al_json(row_, "wire", wire, sizeof wire)) return;
    if (!ci_read_cert(wire, &c)) return;
    int i = pend_find(c.r);
    if (i < 0 || !al_eq(c.station, g_pend[i].station)) return;
    al_json(row_, "sig", sig, sizeof sig);
    if (al_eq(sig, "forged")) return;
    char station[16];
    al_cpy(station, g_pend[i].station, sizeof station);
    char body[200];
    if (c.code == CI_OK && !ci_pos_ok(c.pos)) {
        /* Signed, and still not a check-in: it names no place, and every
         * check-in is a place on the map. */
        al_cpy(body, station, sizeof body);
        al_cat(body, " answered without saying where it is: not a valid check-in.", sizeof body);
        tell("warning", body);
    } else if (c.code == CI_OK) {
        char ask[CI_WIRE_MAX + 64];
        /* What is new about this one, for the message: a station never met
         * before, a badge not yet on the wall. */
        int first_here = log_count_for(station) == 0;
        int new_badge = 0;
        if (c.file[0]) {
            char v[8];
            p_begin(); p_str(c.file); p_end();
            new_badge = !(db_one("SELECT id FROM log WHERE badge = ? LIMIT 1", 1, "id", v, sizeof v) && v[0]);
        }
        own_ask(g_pend[i].id, g_pend[i].wire, ask, sizeof ask);
        p_begin();
        p_str(c.r); p_str(station); p_int(c.ts ? c.ts : now_s());
        p_str(c.link); p_str(ci_grade(c.link, c.route)); p_str(c.rssi);
        p_str(c.route); p_str(c.pos); p_str(c.title); p_str(c.file);
        p_str(wire); p_str(ask);
        p_end();
        db_exec("INSERT OR REPLACE INTO log (id, station, ts, link, grade, rssi, route,"
                " pos, title, badge, cert, ask, greeting) VALUES"
                " (?,?,?,?,?,?,?,?,?,?,?,?,'')", 1);
        p_begin(); p_str(station); p_int(now_s()); p_end();
        db_exec("INSERT OR REPLACE INTO stations (call, last) VALUES (?, ?)", 1);
        fetch_badge(c.file);
        /* The greeting, if the owner wrote one, follows as a message. */
        al_cpy(g_greet_from, station, sizeof g_greet_from);
        g_greet_until = now_s() + 300;
        al_cpy(body, first_here ? "\xF0\x9F\x8E\x89 First check-in at " : "\xE2\x9C\x85 Checked in at ", sizeof body);
        al_cat(body, station, sizeof body);
        if (c.title[0]) { al_cat(body, " (", sizeof body); al_cat(body, c.title, sizeof body); al_cat(body, ")", sizeof body); }
        if (new_badge) al_cat(body, ". New badge on your wall!", sizeof body);
        tell("success", body);
        g_map_fitted = 0;          /* the map shows the new place */
    } else {
        al_cpy(body, station, sizeof body);
        al_cat(body, c.code == CI_AGAIN_LATER ? ": you already checked in here today."
                   : c.code == CI_STALE ? ": the ask arrived too late. Is this device's clock right?"
                   : c.code == CI_REFUSED ? ": refused. It takes check-ins in person only, and your ask did not reach it that way."
                   : c.code == CI_NO_PLACE ? ": its owner has not said where it stands yet, so it cannot give check-ins."
                   : ": no check-in.", sizeof body);
        tell("warning", body);
    }
    pend_drop(i);
    listen_update();
    push_log();
    push_badges();
    push_map();
    push_nearby();
}

static void on_status(const char *row_)
{
    char id[8], state[16];
    if (!al_json(row_, "id", id, sizeof id)) return;
    int i = pend_find(id);
    if (i < 0) return;
    al_json(row_, "state", state, sizeof state);
    if (!al_eq(state, "unanswered") && !al_eq(state, "unfinished")) return;
    char body[160];
    al_cpy(body, g_pend[i].station, sizeof body);
    al_cat(body, " did not answer: it gives no check-ins, or it is out of reach.", sizeof body);
    tell("warning", body);
    pend_drop(i);
    listen_update();
    push_nearby();
}

/* A message from the station we just checked in at: its owner's greeting. */
static void on_message(const char *row_)
{
    char from[80], text[200];
    if (!g_greet_until || now_s() > g_greet_until) { listen_update(); return; }
    /* Two shapes reach this topic: a packet heard on the air (from: the
     * callsign, the words in its wire) and a message that came over LXMF
     * (call: the callsign, the words in content). */
    if (!al_json(row_, "call", from, sizeof from) || !from[0])
        al_json(row_, "from", from, sizeof from);
    if (!al_eq(from, g_greet_from)) return;
    if (!al_json(row_, "content", text, sizeof text) || !text[0]) {
        char wire[CI_WIRE_MAX + 64];
        if (!al_json(row_, "wire", wire, sizeof wire)) return;
        al_field(wire, "m", text, sizeof text);
    }
    if (!text[0]) return;
    p_begin(); p_str(text); p_str(from); p_end();
    db_exec("UPDATE log SET greeting = ? WHERE id = (SELECT id FROM log WHERE station = ?"
            " ORDER BY ts DESC LIMIT 1)", 1);
    g_greet_until = 0;
    listen_update();
    push_log();
}

/* ── the bot ──────────────────────────────────────────────────────────── */
/* The event running now, if any: its title and badge. */
static void active_event(char *title, unsigned tcap, char *badge, unsigned bcap)
{
    title[0] = badge[0] = 0;
    p_begin(); p_int(now_s()); p_int(now_s()); p_end();
    if (!db_query("SELECT title, badge FROM events WHERE since <= ? AND until > ?"
                  " ORDER BY since DESC LIMIT 1", 1)) return;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    if (!al_json_next(p, g_obj, sizeof g_obj)) return;
    al_json(g_obj, "title", title, tcap);
    al_json(g_obj, "badge", badge, bcap);
}

/* Degrees times 1e7 as "38.7071", four places (11 m). */
static void deg(char *d, unsigned cap, long v)
{
    if (v < 0) { al_cat(d, "-", cap); v = -v; }
    al_cat_u(d, (unsigned long long)(v / 10000000L), cap);
    al_cat(d, ".", cap);
    long f = (v % 10000000L) / 1000L;
    char t[5] = { (char)('0' + f / 1000), (char)('0' + f / 100 % 10),
                  (char)('0' + f / 10 % 10), (char)('0' + f % 10), 0 };
    al_cat(d, t, cap);
}

/* Where this station is, exactly as its owner shares it (Settings >
 * Privacy): the same coarsened position its beacon carries, from the core.
 * Empty when nothing is shared: then it gives no check-ins, because a
 * check-in without a place is not one. */
static int g_acc;                 /* the shared position's radius, metres */
static void my_pos(char *out, unsigned cap)
{
    char j[120];
    out[0] = 0;
    g_acc = 0;
    int32_t n = hal_xprs_position(j, sizeof j - 1);
    if (n <= 0 || n >= (int32_t)sizeof j) return;
    j[n] = 0;
    char a[12];
    al_json(j, "pos", out, cap);
    if (al_json(j, "acc", a, sizeof a))
        for (const char *q = a; *q >= '0' && *q <= '9'; q++) g_acc = g_acc * 10 + (*q - '0');
    if (!ci_pos_ok(out)) out[0] = 0;
}

/* Where the person holding this device is now, for "how far": a GPS fix,
 * else the station's shared position. Never aired. */
static void here_pos(char *out, unsigned cap)
{
    out[0] = 0;
    int32_t lat = hal_sensor_gps_lat(), lon = hal_sensor_gps_lon();
    if (lat != (int32_t)0x80000000 && lon != (int32_t)0x80000000 && (lat || lon)) {
        deg(out, cap, lat);
        al_cat(out, ",", cap);
        deg(out, cap, lon);
        return;
    }
    my_pos(out, cap);
}

/* ── the check-in point ─────────────────────────────────────────────── */
/* A station that gives check-ins and shares where it is says so on the air:
 * a t:event tag:checkin under CI_POINT_TITLE, with its shared position, its
 * badge and its intro, good for two hours and renewed every hour while it
 * gives check-ins. Turning check-ins off airs one that closes in a minute,
 * so the pin leaves other people's maps. No position shared: no point. */
#define POINT_EVERY_S 3600ULL
#define POINT_FOR_S   7200ULL
static void announce_point(int closing)
{
    char pos[40], wire[CI_WIRE_MAX + 1];
    my_pos(pos, sizeof pos);
    if (!pos[0]) return;
    who_am_i();
    if (!g_me[0]) return;
    unsigned long long now = now_s();
    int n = ci_build_event(wire, sizeof wire, g_me, now, pos, CI_POINT_TITLE, now,
                           now + (closing ? 60ULL : POINT_FOR_S), g_badge, g_intro);
    if (n < 0) return;
    if (hal_xprs_send(wire, (uint32_t)n) == 0) g_point_at = closing ? 0 : now;
}

/* Called on every wake: renew the point when it is due. */
static void point_due(void)
{
    if (!g_bot_on) return;
    unsigned long long now = now_s();
    if (g_point_at && now < g_point_at + POINT_EVERY_S) return;
    announce_point(0);
    if (!g_point_at) g_point_at = now;   /* nothing shared: ask again in an hour */
}

static void push_guests(void);
static void push_station(void);

static void on_command_packet(const char *row_)
{
    ci_ask_t a;
    if (!g_bot_on || !ci_read_ask(row_, &a) || !a.for_us) return;
    who_am_i();
    if (!g_me[0] || al_eq(a.from, g_me)) return;
    char cert[CI_WIRE_MAX + 1];
    /* The same ask again (the courier repeats until answered): the same
     * certificate again, byte for byte, so it collapses at the asker. */
    p_begin(); p_str(a.id); p_end();
    if (db_one("SELECT cert FROM guests WHERE id = ?", 1, "cert", cert, sizeof cert) && cert[0]) {
        hal_xprs_send(cert, al_len(cert));
        return;
    }
    unsigned long long now = now_s(), day = ci_day(now, zone());
    char v[8];
    p_begin(); p_str(a.from); p_int(day); p_end();
    int today = db_one("SELECT id FROM guests WHERE visitor = ? AND day = ?", 1, "id", v, sizeof v) && v[0];
    ci_rules_t rules = { g_local_only };
    int code = ci_decide(&a, &rules, now, today);
    if (code == CI_WAIT) return;
    char title[32], ebadge[80], pos[40];
    active_event(title, sizeof title, ebadge, sizeof ebadge);
    my_pos(pos, sizeof pos);
    if (code == CI_OK && !pos[0]) code = CI_NO_PLACE;
    int n = ci_build_result(cert, sizeof cert, g_me, &a, now, code,
                            ebadge[0] ? ebadge : g_badge, title, pos);
    if (n < 0) return;
    if (hal_xprs_send(cert, (uint32_t)n) != 0) return;
    if (code != CI_OK) return;
    char link[12];
    ci_link_of(a.bearer, link, sizeof link);
    p_begin();
    p_str(a.id); p_str(a.from); p_int(now); p_int(day);
    p_str(link); p_str(ci_grade(link, a.via)); p_str(title); p_str(cert);
    p_end();
    db_exec("INSERT OR REPLACE INTO guests (id, visitor, ts, day, link, grade, title, cert)"
            " VALUES (?,?,?,?,?,?,?,?)", 1);
    if (g_greeting[0]) {
        char mid[8];
        hal_xprs_message(a.from, al_len(a.from), g_greeting, al_len(g_greeting), 0, mid, sizeof mid);
    }
    char body[96];
    al_cpy(body, a.from, sizeof body);
    al_cat(body, " checked in at your station", sizeof body);
    tell("info", body);
    push_station();
}

/* ── Logbook ──────────────────────────────────────────────────────────── */
static void tile(int *first, const char *id, const char *label, const char *value, const char *hint)
{
    if (!*first) al_cat(g_out, ",", sizeof g_out);
    *first = 0;
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_cat(g_out, id, sizeof g_out);
    al_cat(g_out, "\",\"label\":\"", sizeof g_out);
    al_jesc(g_out, label, sizeof g_out);
    al_cat(g_out, "\",\"value\":\"", sizeof g_out);
    al_jesc(g_out, value, sizeof g_out);
    al_cat(g_out, "\",\"hint\":\"", sizeof g_out);
    al_jesc(g_out, hint, sizeof g_out);
    al_cat(g_out, "\"}", sizeof g_out);
}

static void count(const char *sql, char *out, unsigned cap)
{
    if (!db_one(sql, 0, "n", out, cap) || !out[0]) al_cpy(out, "0", cap);
}

static void push_stats(void)
{
    char total[12], stations[12], local[12], events[12], badges[12];
    count("SELECT COUNT(*) AS n FROM log", total, sizeof total);
    count("SELECT COUNT(DISTINCT station) AS n FROM log", stations, sizeof stations);
    count("SELECT COUNT(*) AS n FROM log WHERE grade = 'local'", local, sizeof local);
    count("SELECT COUNT(DISTINCT title) AS n FROM log WHERE title != ''", events, sizeof events);
    count("SELECT COUNT(DISTINCT badge) AS n FROM log WHERE badge != ''", badges, sizeof badges);
    /* Days in a row with a check-in, ending today or yesterday. */
    char streak[12] = "0";
    {
        unsigned long long today = ci_day(now_s(), zone()), want = today;
        int n = 0;
        if (db_query("SELECT ts FROM log ORDER BY ts DESC LIMIT 400", 0)) {
            const char *p = g_rows;
            while (*p && *p != '[') p++;
            if (*p == '[') p++;
            while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
                char ts[24];
                unsigned long long t = 0;
                al_json(g_obj, "ts", ts, sizeof ts);
                for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
                unsigned long long d = ci_day(t, zone());
                if (n == 0 && d + 1 == today) want = d;      /* none yet today */
                if (d == want) { n++; want--; }
                else if (d < want) break;
            }
        }
        streak[0] = 0;
        al_cat_u(streak, (unsigned long long)n, sizeof streak);
    }
    al_cpy(g_out, "{\"type\":\"ui.stats.set\",\"field\":\"lb_stats\",\"tiles\":[", sizeof g_out);
    int first = 1;
    tile(&first, "total", "Check-ins", total, "every one, all time");
    tile(&first, "stations", "Stations", stations, "different stations");
    tile(&first, "local", "In person", local, "over Bluetooth, WiFi or LAN");
    tile(&first, "events", "Events", events, "check-ins at an event");
    tile(&first, "badges", "Badges", badges, "different badges");
    tile(&first, "streak", "Streak", streak, "days in a row");
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

static void push_log(void)
{
    push_stats();
    rows_begin("log");
    int any = 0;
    if (db_query("SELECT id, station, ts, grade, title, badge, greeting FROM log"
                 " ORDER BY ts DESC LIMIT 200", 0)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        section_begin("Logbook", &any);
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char id[8], st[16], ts[24], grade[12], title[32], badge[80], tok[88] = "", w[24], sub_[200];
            al_json(g_obj, "id", id, sizeof id);
            al_json(g_obj, "station", st, sizeof st);
            al_json(g_obj, "ts", ts, sizeof ts);
            al_json(g_obj, "grade", grade, sizeof grade);
            al_json(g_obj, "title", title, sizeof title);
            al_json(g_obj, "badge", badge, sizeof badge);
            al_json(g_obj, "greeting", sub_, sizeof sub_);
            unsigned long long t = 0;
            for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
            when(w, sizeof w, t);
            if (!sub_[0]) al_cpy(sub_, w, sizeof sub_);
            if (badge[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, badge, sizeof tok); }
            if (!tok[0]) g_icon = "check";
            row(id, title[0] ? title : st, sub_, al_eq(grade, "local") ? "in person" : grade,
                title[0] ? st : w, tok, st, "", "");
        }
        section_end();
    }
    rows_end();
}

/* A logbook column as a person reads it: the radio by its name, the grade
 * in words, and the badge as what it is rather than its file's hash (the
 * picture itself is on the Badges wall). */
static const char *entry_value(const char *col, const char *v)
{
    static const struct { const char *word, *name; } links[] = {
        { "ble", "Bluetooth" }, { "lan", "LAN" }, { "wifi", "WiFi" },
        { "espnow", "ESP-NOW" }, { "lora", "LoRa" }, { "halow", "HaLow" },
        { "vhf", "VHF" }, { "uhf", "UHF" }, { "hf", "HF" }, { "cb", "CB" }, { "pmr", "PMR" }
    };
    if (al_eq(col, "link"))
        for (unsigned i = 0; i < sizeof links / sizeof links[0]; i++)
            if (al_eq(v, links[i].word)) return links[i].name;
    if (al_eq(col, "grade")) {
        if (al_eq(v, "local")) return "in person";
        if (al_eq(v, "relayed")) return "through another station";
    }
    if (al_eq(col, "badge")) return "collected: it is on your Badges wall";
    return v;
}

static void push_entry(void)
{
    if (!g_sel[0]) return;
    p_begin(); p_str(g_sel); p_end();
    if (!db_query("SELECT * FROM log WHERE id = ?", 1)) return;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    if (!al_json_next(p, g_obj, sizeof g_obj)) return;
    static const struct { const char *col, *label; } rows_[] = {
        { "station", "Station" }, { "grade", "How close" }, { "link", "Heard over" },
        { "rssi", "Signal" }, { "route", "Through" }, { "pos", "Station position" },
        { "title", "Event" }, { "greeting", "Greeting" }, { "badge", "Badge" },
        { "cert", "Certificate (signed by the station)" }, { "ask", "Your ask" }
    };
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"e_info\",\"value\":[{\"title\":\"\",\"items\":[", sizeof g_out);
    int first = 1;
    char ts[24], w[24];
    al_json(g_obj, "ts", ts, sizeof ts);
    unsigned long long t = 0;
    for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
    when(w, sizeof w, t);
    al_cat(g_out, "{\"label\":\"When\",\"value\":\"", sizeof g_out);
    al_jesc(g_out, w, sizeof g_out);
    al_cat(g_out, "\"}", sizeof g_out);
    first = 0;
    for (unsigned i = 0; i < sizeof rows_ / sizeof rows_[0]; i++) {
        static char v[CI_WIRE_MAX + 64];
        if (!al_json(g_obj, rows_[i].col, v, sizeof v) || !v[0]) continue;
        if (!first) al_cat(g_out, ",", sizeof g_out);
        first = 0;
        al_cat(g_out, "{\"label\":\"", sizeof g_out);
        al_jesc(g_out, rows_[i].label, sizeof g_out);
        al_cat(g_out, "\",\"value\":\"", sizeof g_out);
        al_jesc(g_out, entry_value(rows_[i].col, v), sizeof g_out);
        al_cat(g_out, "\"}", sizeof g_out);
    }
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
}

/* ── The map ──────────────────────────────────────────────────────────── */
/* One pin: its id says what a tap opens (log:<id>, ev:<id>, me). */
static void marker(const char *id, const char *pos, const char *label,
                   const char *color, const char *detail)
{
    char lat[24], lon[24], m[600];
    unsigned i = 0, k = 0;
    while (pos[i] && pos[i] != ',' && k + 1 < sizeof lat) lat[k++] = pos[i++];
    lat[k] = 0;
    if (pos[i] != ',') return;
    al_cpy(lon, pos + i + 1, sizeof lon);
    al_cpy(m, "{\"type\":\"ui.map.marker\",\"id\":\"", sizeof m);
    al_jesc(m, id, sizeof m);
    al_cat(m, "\",\"lat\":", sizeof m);
    al_cat(m, lat, sizeof m);
    al_cat(m, ",\"lon\":", sizeof m);
    al_cat(m, lon, sizeof m);
    al_cat(m, ",\"label\":\"", sizeof m);
    al_jesc(m, label, sizeof m);
    al_cat(m, "\",\"color\":\"", sizeof m);
    al_cat(m, color, sizeof m);
    al_cat(m, "\",\"detail\":\"", sizeof m);
    al_jesc(m, detail, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* The box around every check-in, times 1e7; 0 when there is none. */
static long g_s, g_w, g_n, g_e;
static int  g_boxed;
static void box_add(const char *pos)
{
    long la, lo;
    if (!ci_pos_parse(pos, &la, &lo)) return;
    if (!g_boxed) { g_s = g_n = la; g_w = g_e = lo; g_boxed = 1; return; }
    if (la < g_s) g_s = la;
    if (la > g_n) g_n = la;
    if (lo < g_w) g_w = lo;
    if (lo > g_e) g_e = lo;
}

static void cat_deg7(char *d, unsigned cap, long v)
{
    deg(d, cap, v);
}

/* Centre the map on the check-ins, at the zoom that holds them all. */
static void fit_map(void)
{
    if (!g_boxed) return;
    long span = g_n - g_s, spanx = g_e - g_w;
    if (spanx > span) span = spanx;
    int z = 2;
    /* A tile at zoom z spans 360/2^z degrees; keep the box in about two. */
    while (z < 15 && (3600000000L >> (z + 1)) > span * 2) z++;
    char m[200] = "{\"type\":\"ui.map.viewport\",\"lat\":";
    cat_deg7(m, sizeof m, (g_s + g_n) / 2);
    al_cat(m, ",\"lon\":", sizeof m);
    cat_deg7(m, sizeof m, (g_w + g_e) / 2);
    al_cat(m, ",\"zoom\":", sizeof m);
    al_cat_u(m, (unsigned long long)z, sizeof m);
    al_cat(m, "}", sizeof m);
    say(m);
}

/* The overview, saved for off-grid use once per new area: the region of the
 * check-ins (padded half a degree) at zooms 3 to 9. A detailed city is the
 * person's own act (save_area). */
static void save_overview(void)
{
    if (!g_boxed) return;
    char box[160] = "", have[160] = "";
    const long pad = 5000000L;
    cat_deg7(box, sizeof box, g_s - pad);
    al_cat(box, ",", sizeof box);
    cat_deg7(box, sizeof box, g_w - pad);
    al_cat(box, ",", sizeof box);
    cat_deg7(box, sizeof box, g_n + pad);
    al_cat(box, ",", sizeof box);
    cat_deg7(box, sizeof box, g_e + pad);
    kv_str("ovbox", have, sizeof have);
    if (al_eq(box, have)) return;
    hal_kv_set("ovbox", 5, box, al_len(box));
    char m[260] = "{\"type\":\"ui.map.prefetch\",\"bbox\":[";
    al_cat(m, box, sizeof m);
    al_cat(m, "],\"minZoom\":3,\"maxZoom\":9,\"max\":500}", sizeof m);
    say(m);
}

/* Check-in points heard (the stations giving check-ins that share where
 * they are), the newest announcement per station, still open, not ours. */
#define PT_MAX 40
static struct {
    char call[16], pos[40], intro[104], file[80], id[8];
    unsigned long long ts;
    long dist;
} g_pts[PT_MAX];
static int g_npts;

static void load_points(void)
{
    static const char q[] = "{\"types\":[\"event\"],\"limit\":200}";
    g_npts = 0;
    int32_t n = hal_xprs_history(q, sizeof q - 1, g_big, sizeof g_big - 1);
    if (n <= 0) return;
    g_big[n] = 0;
    who_am_i();
    char here[40];
    here_pos(here, sizeof here);
    unsigned long long now = now_s();
    const char *p = g_big;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char wire[CI_WIRE_MAX + 64], from[16], title[32], u[24], ts[24], pos[40];
        if (!al_json(g_obj, "wire", wire, sizeof wire) || !has_tag(wire, "checkin")) continue;
        al_field(wire, "title", title, sizeof title);
        if (!al_eq(title, CI_POINT_TITLE)) continue;
        al_field(wire, "f", from, sizeof from);
        if (!from[0] || al_eq(from, g_me)) continue;
        al_field(wire, "pos", pos, sizeof pos);
        al_field(wire, "until", u, sizeof u);
        al_field(wire, "ts", ts, sizeof ts);
        unsigned long long t = al_epoch(ts);
        int k = 0;
        while (k < g_npts && !al_eq(g_pts[k].call, from)) k++;
        if (k < g_npts && g_pts[k].ts >= t) continue;      /* we hold a newer one */
        if (al_epoch(u) <= now || !ci_pos_ok(pos)) {      /* closed, or no place */
            if (k < g_npts) { g_pts[k] = g_pts[--g_npts]; }
            continue;
        }
        if (k == g_npts) { if (g_npts == PT_MAX) continue; g_npts++; }
        al_cpy(g_pts[k].call, from, sizeof g_pts[k].call);
        al_cpy(g_pts[k].pos, pos, sizeof g_pts[k].pos);
        al_field(wire, "m", g_pts[k].intro, sizeof g_pts[k].intro);
        al_field(wire, "file", g_pts[k].file, sizeof g_pts[k].file);
        al_json(g_obj, "id", g_pts[k].id, sizeof g_pts[k].id);
        g_pts[k].ts = t;
        g_pts[k].dist = here[0] ? ci_dist_m(here, pos) : -1;
    }
    /* Nearest first; unknown distances last, by callsign. */
    for (int i = 1; i < g_npts; i++)
        for (int j = i; j > 0; j--) {
            long a = g_pts[j - 1].dist, b = g_pts[j].dist;
            int swap = (a < 0 && b >= 0) || (a >= 0 && b >= 0 && b < a) ||
                       (a < 0 && b < 0 && al_cmp(g_pts[j].call, g_pts[j - 1].call) < 0);
            if (!swap) break;
            __typeof__(g_pts[0]) t = g_pts[j]; g_pts[j] = g_pts[j - 1]; g_pts[j - 1] = t;
        }
}

static int visited(const char *call)
{
    char v[8];
    p_begin(); p_str(call); p_end();
    return db_one("SELECT id FROM log WHERE station = ? LIMIT 1", 1, "id", v, sizeof v) && v[0];
}

static int point_find(const char *call)
{
    for (int i = 0; i < g_npts; i++) if (al_eq(g_pts[i].call, call)) return i;
    return -1;
}

/* A place's details, with its coordinates to copy into a navigation app. */
static void place_sheet(const char *id, const char *title, const char *intro,
                        const char *pos, long dist, const char *extra)
{
    char body[400] = "", d[24];
    if (intro[0]) { al_cat(body, intro, sizeof body); al_cat(body, "\n\n", sizeof body); }
    if (dist >= 0) {
        ci_dist_text(dist, d, sizeof d);
        al_cat(body, "About ", sizeof body);
        al_cat(body, d, sizeof body);
        al_cat(body, " from you. ", sizeof body);
    }
    al_cat(body, "Position ", sizeof body);
    al_cat(body, pos, sizeof body);
    al_cat(body, ".", sizeof body);
    if (extra[0]) { al_cat(body, "\n", sizeof body); al_cat(body, extra, sizeof body); }
    char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"";
    al_jesc(m, id, sizeof m);
    al_cat(m, "\",\"title\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\",\"body\":\"", sizeof m);
    al_jesc(m, body, sizeof m);
    al_cat(m, "\",\"copy\":\"", sizeof m);
    al_jesc(m, pos, sizeof m);
    al_cat(m, "\",\"chips\":[{\"label\":\"Show on map\",\"value\":\"map\"}]}", sizeof m);
    say(m);
}

static void point_sheet(const char *call)
{
    load_points();
    int i = point_find(call);
    if (i < 0) { toast("info", "That station no longer gives check-ins."); return; }
    char extra[120] = "", w[24], id[24] = "pt:";
    when(w, sizeof w, g_pts[i].ts);
    al_cpy(extra, visited(call) ? "You have checked in here. " : "Not visited yet. ", sizeof extra);
    al_cat(extra, "Announced ", sizeof extra);
    al_cat(extra, w, sizeof extra);
    al_cat(extra, ". Check in when you are there: it answers in person.", sizeof extra);
    al_cat(id, call, sizeof id);
    place_sheet(id, call, g_pts[i].intro, g_pts[i].pos, g_pts[i].dist, extra);
}

static void push_map(void)
{
    say("{\"type\":\"ui.map.markers.clear\"}");
    g_boxed = 0;
    if (db_query("SELECT id, station, ts, pos, title FROM log ORDER BY ts DESC LIMIT 500", 0)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char id[8], st[16], ts[24], pos[40], title[32], mid[16] = "log:", w[24], detail[80];
            al_json(g_obj, "id", id, sizeof id);
            al_json(g_obj, "station", st, sizeof st);
            al_json(g_obj, "ts", ts, sizeof ts);
            al_json(g_obj, "pos", pos, sizeof pos);
            al_json(g_obj, "title", title, sizeof title);
            if (!ci_pos_ok(pos)) continue;
            unsigned long long t = 0;
            for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
            when(w, sizeof w, t);
            al_cat(mid, id, sizeof mid);
            al_cpy(detail, w, sizeof detail);
            if (title[0]) { al_cat(detail, " \xC2\xB7 ", sizeof detail); al_cat(detail, title, sizeof detail); }
            marker(mid, pos, st, "#FAFAFA", detail);
            box_add(pos);
        }
    }
    /* Check-in events still to come or running: the places to go next. */
    static const char q[] = "{\"types\":[\"event\"],\"limit\":120}";
    int32_t n = hal_xprs_history(q, sizeof q - 1, g_big, sizeof g_big - 1);
    if (n > 0) {
        g_big[n] = 0;
        const char *p = g_big;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char wire[CI_WIRE_MAX + 64], id[8], from[16], title[32], u[24], pos[40], mid[16] = "ev:";
            if (!al_json(g_obj, "wire", wire, sizeof wire) || !has_tag(wire, "checkin")) continue;
            al_field(wire, "pos", pos, sizeof pos);
            al_field(wire, "until", u, sizeof u);
            if (!ci_pos_ok(pos) || al_epoch(u) <= now_s()) continue;
            al_json(g_obj, "id", id, sizeof id);
            al_field(wire, "f", from, sizeof from);
            al_field(wire, "title", title, sizeof title);
            if (al_eq(title, CI_POINT_TITLE)) continue;   /* drawn as a station below */
            al_cat(mid, id, sizeof mid);
            char nice[40];
            label_name(nice, sizeof nice, title);
            marker(mid, pos, nice, "#FFC107", from);
        }
    }
    /* Stations giving check-ins: green to visit, pale once visited. */
    load_points();
    for (int i = 0; i < g_npts; i++) {
        char mid[24] = "st:", detail[64] = "";
        int been = visited(g_pts[i].call);
        al_cat(mid, g_pts[i].call, sizeof mid);
        if (g_pts[i].dist >= 0) { ci_dist_text(g_pts[i].dist, detail, sizeof detail); al_cat(detail, " away", sizeof detail); }
        if (been) al_cat(detail, detail[0] ? " \xC2\xB7 visited" : "visited", sizeof detail);
        marker(mid, g_pts[i].pos, g_pts[i].call, been ? "#A5D6A7" : "#43A047", detail);
    }
    char me[40];
    my_pos(me, sizeof me);
    if (me[0] && g_bot_on) marker("me", me, "Your station", "#4FC3F7", "gives check-ins here");
    if (!g_map_fitted && g_boxed) { fit_map(); g_map_fitted = 1; }
    save_overview();
}

/* ── Badges ───────────────────────────────────────────────────────────── */
static void push_badges(void)
{
    rows_begin("badges");
    int any = 0;
    if (db_query("SELECT badge, station, title, COUNT(*) AS n, MAX(ts) AS last FROM log"
                 " WHERE badge != '' GROUP BY badge, station, title ORDER BY last DESC", 0)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        section_begin("Badge wall", &any);
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char badge[80], st[16], title[32], n[12], last[24], tok[88] = "file:", sub_[64], w[24], id[120];
            al_json(g_obj, "badge", badge, sizeof badge);
            al_json(g_obj, "station", st, sizeof st);
            al_json(g_obj, "title", title, sizeof title);
            al_json(g_obj, "n", n, sizeof n);
            al_json(g_obj, "last", last, sizeof last);
            unsigned long long t = 0;
            for (const char *q = last; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
            when(w, sizeof w, t);
            al_cat(tok, badge, sizeof tok);
            al_cpy(sub_, "earned ", sizeof sub_);
            al_cat(sub_, n, sizeof sub_);
            al_cat(sub_, al_eq(n, "1") ? " time" : " times", sizeof sub_);
            al_cpy(id, badge, sizeof id);
            al_cat(id, "|", sizeof id);
            al_cat(id, st, sizeof id);
            row(id, title[0] ? title : st, sub_, title[0] ? st : "", w, tok, st, "", "");
            fetch_badge(badge);   /* the core keeps it once it has it */
        }
        section_end();
    }
    rows_end();
}

/* ── Events (chaser side) ─────────────────────────────────────────────── */
/* A label as a name: dashes to spaces, each word capitalised. */
static void label_name(char *out, unsigned cap, const char *label)
{
    unsigned o = 0;
    int up = 1;
    for (const char *p = label; *p && o + 1 < cap; p++) {
        char c = *p == '-' ? ' ' : *p;
        if (up && c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[o++] = c;
        up = c == ' ';
    }
    out[o] = 0;
}

static int has_tag(const char *wire, const char *tag)
{
    char v[96];
    if (!al_field(wire, "tag", v, sizeof v)) return 0;
    const char *p = v;
    unsigned tl = al_len(tag);
    while (*p) {
        if (al_starts(p, tag) && (p[tl] == 0 || p[tl] == ',')) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

static void push_events(void)
{
    static const char q[] = "{\"types\":[\"event\"],\"limit\":120}";
    int32_t n = hal_xprs_history(q, sizeof q - 1, g_big, sizeof g_big - 1);
    unsigned long long now = now_s();
    rows_begin("events");
    int any = 0;
    load_points();
    if (g_npts) {
        section_begin("Stations giving check-ins", &any);
        for (int i = 0; i < g_npts; i++) {
            char id[24] = "st:", d[40] = "", tok[88] = "";
            al_cat(id, g_pts[i].call, sizeof id);
            if (g_pts[i].dist >= 0) { ci_dist_text(g_pts[i].dist, d, sizeof d); al_cat(d, " away", sizeof d); }
            if (g_pts[i].file[0]) {
                al_cpy(tok, "file:", sizeof tok);
                al_cat(tok, g_pts[i].file, sizeof tok);
                fetch_badge(g_pts[i].file);
            }
            g_icon = "cell_tower";
            row(id, g_pts[i].call, g_pts[i].intro[0] ? g_pts[i].intro : "Gives check-ins",
                d, visited(g_pts[i].call) ? "visited" : "new badge", tok, g_pts[i].call, "", "");
        }
        section_end();
    }
    section_begin("Check-in events", &any);
    if (n > 0) {
        g_big[n] = 0;
        const char *p = g_big;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char wire[CI_WIRE_MAX + 64], id[8], from[16], title[32], s[24], u[24], file[80], tok[88] = "", m[160], ws[24], wu[24], tag[48];
            if (!al_json(g_obj, "wire", wire, sizeof wire) || !has_tag(wire, "checkin")) continue;
            al_json(g_obj, "id", id, sizeof id);
            al_field(wire, "f", from, sizeof from);
            al_field(wire, "title", title, sizeof title);
            if (al_eq(title, CI_POINT_TITLE)) continue;   /* a station, listed above */
            al_field(wire, "since", s, sizeof s);
            al_field(wire, "until", u, sizeof u);
            al_field(wire, "file", file, sizeof file);
            al_field(wire, "m", m, sizeof m);
            unsigned long long until = al_epoch(u), since = al_epoch(s);
            if (!since) {     /* no since: it started when it was announced */
                char ts[24];
                al_field(wire, "ts", ts, sizeof ts);
                since = al_epoch(ts);
            }
            if (!until || until <= now) continue;
            when(ws, sizeof ws, since);
            when(wu, sizeof wu, until);
            al_cpy(tag, since <= now ? "running now" : "from ", sizeof tag);
            if (since > now) al_cat(tag, ws, sizeof tag);
            if (file[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, file, sizeof tok); fetch_badge(file); }
            char until_tag[40] = "until ";
            al_cat(until_tag, wu, sizeof until_tag);
            char nice[40];
            label_name(nice, sizeof nice, title);
            g_icon = "schedule";
            row(id, nice, m[0] ? m : from, tag, until_tag, tok, from, "", "");
        }
    }
    section_end();
    rows_end();
}

/* ── My station ───────────────────────────────────────────────────────── */
/* A details card: label/value pairs. */
static void details(const char *field, const char *const *pairs, int n)
{
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof g_out);
    al_cat(g_out, field, sizeof g_out);
    al_cat(g_out, "\",\"value\":[{\"title\":\"\",\"items\":[", sizeof g_out);
    for (int i = 0; i < n; i++) {
        if (i) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "{\"label\":\"", sizeof g_out);
        al_jesc(g_out, pairs[2 * i], sizeof g_out);
        al_cat(g_out, "\",\"value\":\"", sizeof g_out);
        al_jesc(g_out, pairs[2 * i + 1], sizeof g_out);
        al_cat(g_out, "\"}", sizeof g_out);
    }
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
}

/* The Guestbook screen: one list (the host shows one per screen), in three
 * sections: the badge handed out, the station's events, and its visitors. */
static void push_guests(void)
{
    rows_begin("guests");
    int any = 0;
    section_begin("Your badge", &any);
    g_icon = "star";
    if (g_badge[0]) row("badge", "Your badge", "Handed out with every check-in", "", "", g_badge, "", "", "");
    else row("badge", "No badge yet", "Choose a small picture: visitors collect it", "", "", "", "", "", "");
    section_end();
    if (db_query("SELECT title, since, until, badge FROM events ORDER BY since DESC LIMIT 50", 0)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        int first_ev = 1;
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char title[32], s[24], u[24], badge[80], ws[24], wu[24];
            if (first_ev) { section_begin("Your events", &any); first_ev = 0; }
            al_json(g_obj, "title", title, sizeof title);
            al_json(g_obj, "since", s, sizeof s);
            al_json(g_obj, "until", u, sizeof u);
            al_json(g_obj, "badge", badge, sizeof badge);
            unsigned long long since = 0, until = 0;
            for (const char *q = s; *q >= '0' && *q <= '9'; q++) since = since * 10 + (unsigned)(*q - '0');
            for (const char *q = u; *q >= '0' && *q <= '9'; q++) until = until * 10 + (unsigned)(*q - '0');
            when(ws, sizeof ws, since);
            when(wu, sizeof wu, until);
            char until_tag[40] = "until ";
            al_cat(until_tag, wu, sizeof until_tag);
            char nice[40];
            label_name(nice, sizeof nice, title);
            g_icon = "schedule";
            row(title, nice, ws, until <= now_s() ? "ended" : "", until_tag, badge, "", "", "");
        }
        if (!first_ev) section_end();
    }
    if (db_query("SELECT id, visitor, ts, grade, title FROM guests ORDER BY ts DESC LIMIT 200", 0)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        int first_g = 1;
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char id[8], who[16], ts[24], grade[12], title[32], w[24];
            if (first_g) { section_begin("Guestbook", &any); first_g = 0; }
            al_json(g_obj, "id", id, sizeof id);
            al_json(g_obj, "visitor", who, sizeof who);
            al_json(g_obj, "ts", ts, sizeof ts);
            al_json(g_obj, "grade", grade, sizeof grade);
            al_json(g_obj, "title", title, sizeof title);
            unsigned long long t = 0;
            for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
            when(w, sizeof w, t);
            row(id, who, w, al_eq(grade, "local") ? "in person" : grade, title, "", who, "", "");
        }
        if (!first_g) section_end();
    }
    rows_end();
}

static void push_station(void)
{
    char visitors[12], events[12];
    field_set_bool("bot_on", g_bot_on);
    field_set_bool("local_only", g_local_only);
    field_set_str("greeting", g_greeting);
    count("SELECT COUNT(*) AS n FROM guests", visitors, sizeof visitors);
    count("SELECT COUNT(*) AS n FROM events", events, sizeof events);
    char pos[40];
    my_pos(pos, sizeof pos);
    field_set_str("intro", g_intro);
    char where[80] = "";
    if (pos[0]) {
        al_cpy(where, pos, sizeof where);
        if (g_acc > 0) {
            char r[24];
            ci_dist_text(g_acc, r, sizeof r);
            al_cat(where, " (within ", sizeof where);
            al_cat(where, r, sizeof where);
            al_cat(where, ")", sizeof where);
        }
    }
    const char *pairs[] = {
        "Giving check-ins", g_bot_on ? "yes" : "no",
        "Where", where[0] ? where : "not shared: turn on Share my station's position in Settings > Privacy",
        "Badge", g_badge[0] ? "chosen" : "none yet",
        "Visitors", visitors,
        "Events", events,
    };
    details("st_info", pairs, 5);
    push_guests();
}

/* A picked picture: into the media store, as a token. 0 when it failed. */
static int import_picture(const char *path, char *token, unsigned cap)
{
    uint32_t n = hal_media_put_file(path, al_len(path), token, cap - 1);
    if (n == 0 || n >= cap) { token[0] = 0; return 0; }
    token[n] = 0;
    return 1;
}

/* Describe the badge in a t:file publication: the core advertises the hash
 * and places the bytes with this station's archivers (XPRS.md 12.9.2). */
static void publish_badge(const char *token)
{
    char ref[64], wire[CI_WIRE_MAX + 1], meta[512], size[24] = "";
    ci_ref_of(token, ref, sizeof ref);
    if (!ref[0]) return;
    uint32_t mn = hal_media_meta(token, al_len(token), meta, sizeof meta - 1);
    if (mn > 0 && mn < sizeof meta) { meta[mn] = 0; al_json(meta, "size", size, sizeof size); }
    unsigned long long sz = 0;
    for (const char *q = size; *q >= '0' && *q <= '9'; q++) sz = sz * 10 + (unsigned)(*q - '0');
    who_am_i();
    int n = ci_build_badge(wire, sizeof wire, g_me, now_s(), ref, sz);
    if (n > 0) hal_xprs_send(wire, (uint32_t)n);
}

static void pick_picture(const char *title)
{
    char m[300] = "{\"type\":\"file.pick\",\"kind\":\"image\",\"title\":\"";
    al_jesc(m, title, sizeof m);
    al_cat(m, "\",\"extensions\":[\"png\",\"jpg\",\"jpeg\",\"webp\",\"gif\"]}", sizeof m);
    say(m);
}

static int g_picking_event;   /* the next picked file is the event's badge */

static void on_picked(void)
{
    char path[400], token[96];
    if (!al_json(g_buf, "path", path, sizeof path) || !path[0]) return;
    if (!import_picture(path, token, sizeof token)) {
        toast("warning", "Could not read that picture.");
        return;
    }
    if (g_picking_event) {
        g_picking_event = 0;
        al_cpy(g_draft.badge, token, sizeof g_draft.badge);
        const char *pairs[] = { "Event badge", "chosen: handed out during the event" };
        details("ev_badge", pairs, 1);
        return;
    }
    al_cpy(g_badge, token, sizeof g_badge);
    hal_kv_set("badge", 5, g_badge, al_len(g_badge));
    publish_badge(g_badge);
    if (g_bot_on) announce_point(0);
    push_station();
    toast("success", "Badge saved. Visitors get it with their next check-in.");
}

static void publish_event(void)
{
    char title[64], desc[160], len[8], label[32], wire[CI_WIRE_MAX + 1], pos[40];
    al_json(g_buf, "ev_title", title, sizeof title);
    al_json(g_buf, "ev_desc", desc, sizeof desc);
    if (!al_json(g_buf, "ev_len", len, sizeof len) || !len[0]) al_cpy(len, "day", sizeof len);
    ci_label(label, sizeof label, title);
    if (!label[0]) { toast("warning", "Give the event a name."); return; }
    if (al_eq(label, CI_POINT_TITLE)) { toast("warning", "That name is taken by every station's own check-in point: pick another."); return; }
    unsigned long long now = now_s();
    unsigned long long span = al_eq(len, "month") ? 30ULL * 86400 : al_eq(len, "week") ? 7ULL * 86400 : 86400ULL;
    who_am_i();
    my_pos(pos, sizeof pos);
    int n = ci_build_event(wire, sizeof wire, g_me, now, pos, label, now, now + span,
                           g_draft.badge, desc);
    if (n < 0) { toast("warning", "That does not fit in one packet: shorten the name."); return; }
    if (hal_xprs_send(wire, (uint32_t)n) != 0) { toast("warning", "This device could not announce it."); return; }
    if (g_draft.badge[0]) publish_badge(g_draft.badge);
    char ref[64];
    ci_ref_of(g_draft.badge, ref, sizeof ref);
    char tok[80] = "";
    if (ref[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, ref, sizeof tok); }
    p_begin(); p_str(label); p_int(now); p_int(now + span); p_str(tok); p_str(desc); p_end();
    db_exec("INSERT OR REPLACE INTO events (title, since, until, badge, m) VALUES (?,?,?,?,?)", 1);
    g_draft.badge[0] = 0;
    say("{\"type\":\"ui.screen.close\"}");
    push_station();
    toast("success", "Event announced. Check-ins during it carry its name and badge.");
}

/* ── the page ─────────────────────────────────────────────────────────── */
static void push_all(void)
{
    push_map();
    push_nearby();
    push_log();
    push_badges();
    push_events();
    push_station();
}

/* A page is open: follow the station table (core.monitor) so Nearby shows
 * who walks into reach, at most once every ten seconds. */
static int g_page_seen;
static unsigned long long g_nearby_at;
static void page_seen(void)
{
    if (g_page_seen) return;
    g_page_seen = 1;
    sub("core.monitor", 1);
}

static void on_command(void)
{
    char cmd[48] = "", id[128] = "", type[24] = "";
    page_seen();
    al_json(g_buf, "type", type, sizeof type);
    if (al_eq(type, "file.open")) { on_picked(); return; }
    if (al_eq(type, "setViewport")) {
        char z[8];
        al_json(g_buf, "lat", g_view_lat, sizeof g_view_lat);
        al_json(g_buf, "lon", g_view_lon, sizeof g_view_lon);
        if (al_json(g_buf, "zoom", z, sizeof z)) {
            g_view_zoom = 0;
            for (const char *q = z; *q >= '0' && *q <= '9'; q++) g_view_zoom = g_view_zoom * 10 + (*q - '0');
        }
        return;
    }
    if (al_eq(type, "map.prefetch")) {
        char state[12], held[12];
        al_json(g_buf, "state", state, sizeof state);
        al_json(g_buf, "held", held, sizeof held);
        if (al_eq(state, "busy")) toast("info", "Already saving an area of the map: one at a time.");
        if (al_eq(state, "done") && held[0] && !al_eq(held, "0")) {
            char body[120] = "\xF0\x9F\x97\xBA Map saved for off-grid use: ";
            al_cat(body, held, sizeof body);
            al_cat(body, " tiles.", sizeof body);
            toast("success", body);
        }
        return;
    }
    if (!al_json(g_buf, "command", cmd, sizeof cmd) &&
        !al_json(g_buf, "action", cmd, sizeof cmd)) return;
    if (al_eq(cmd, "ready") || al_eq(cmd, "refresh")) { who_am_i(); push_all(); return; }
    if (al_eq(cmd, "nearby_refresh")) { push_nearby(); toast("info", "Up to date."); return; }
    if (al_eq(cmd, "nearby_asking")) {
        toast("info", "Still waiting for its answer: a station has a few minutes to reply.");
        return;
    }
    if (al_eq(cmd, "nearby_checkin") || al_eq(cmd, "nearby_tap")) {
        if (al_json(g_buf, "nearby_id", id, sizeof id) && id[0]) do_checkin(id);
        return;
    }
    if (al_eq(cmd, "set_chase")) {
        char v[8];
        al_json(g_buf, "chase_regional", v, sizeof v);
        g_chase_regional = al_eq(v, "true");
        kv_put_bool("chase_reg", g_chase_regional);
        toast("info", g_chase_regional ? "Asking over LoRa too: stations within radio range may answer."
                                       : "In person only: Bluetooth, WiFi and the local network.");
        return;
    }
    if (al_eq(cmd, "log_tap")) {
        if (!al_json(g_buf, "log_id", id, sizeof id)) return;
        al_cpy(g_sel, id, sizeof g_sel);
        push_entry();
        screen_open("Entry", "Check-in");
        return;
    }
    if (al_eq(cmd, "remove_entry")) {
        if (!g_sel[0]) return;
        char st[16] = "";
        p_begin(); p_str(g_sel); p_end();
        db_one("SELECT station FROM log WHERE id = ?", 1, "station", st, sizeof st);
        p_begin(); p_str(g_sel); p_end();
        db_exec("DELETE FROM log WHERE id = ?", 1);
        g_sel[0] = 0;
        g_map_fitted = 0;
        push_log();
        push_badges();
        push_map();
        char body[96] = "Removed your check-in";
        if (st[0]) { al_cat(body, " at ", sizeof body); al_cat(body, st, sizeof body); }
        al_cat(body, ".", sizeof body);
        toast("info", body);
        /* Back to where the person came from (the logbook or the map), not
         * the logbook re-opened as a page of its own. */
        say("{\"type\":\"ui.screen.close\"}");
        return;
    }
    if (al_eq(cmd, "events_refresh")) { push_events(); push_map(); return; }
    if (al_eq(cmd, "events_tap")) {
        if (al_json(g_buf, "events_id", id, sizeof id) && al_starts(id, "st:")) point_sheet(id + 3);
        return;
    }
    if (al_eq(cmd, "prompt")) {
        char pid[32], v[12];
        al_json(g_buf, "prompt_id", pid, sizeof pid);
        al_json(g_buf, "prompt_value", v, sizeof v);
        if (al_starts(pid, "pt:") && al_eq(v, "map")) {
            load_points();
            int i = point_find(pid + 3);
            if (i < 0) return;
            char lat[24], m[120] = "{\"type\":\"ui.map.locate\",\"lat\":";
            unsigned k = 0;
            while (g_pts[i].pos[k] && g_pts[i].pos[k] != ',' && k + 1 < sizeof lat) { lat[k] = g_pts[i].pos[k]; k++; }
            lat[k] = 0;
            al_cat(m, lat, sizeof m);
            al_cat(m, ",\"lon\":", sizeof m);
            al_cat(m, g_pts[i].pos + k + 1, sizeof m);
            al_cat(m, "}", sizeof m);
            say(m);
        }
        return;
    }
    if (al_eq(cmd, "marker_tap")) {
        if (!al_json(g_buf, "id", id, sizeof id)) return;
        if (al_starts(id, "st:")) { point_sheet(id + 3); return; }
        if (al_starts(id, "log:")) {
            al_cpy(g_sel, id + 4, sizeof g_sel);
            push_entry();
            screen_open("Entry", "Check-in");
        }
        return;
    }
    if (al_eq(cmd, "fit_map")) {
        g_map_fitted = 0;
        push_map();
        if (!g_boxed) {
            char me[40];
            my_pos(me, sizeof me);
            if (me[0]) {
                char m[160] = "{\"type\":\"ui.map.viewport\",\"lat\":";
                unsigned i = 0;
                char lat[24];
                while (me[i] && me[i] != ',' && i + 1 < sizeof lat) { lat[i] = me[i]; i++; }
                lat[i] = 0;
                al_cat(m, lat, sizeof m);
                al_cat(m, ",\"lon\":", sizeof m);
                al_cat(m, me + i + 1, sizeof m);
                al_cat(m, ",\"zoom\":12}", sizeof m);
                say(m);
            }
            toast("info", "No check-ins yet: find a station nearby and check in!");
        }
        return;
    }
    if (al_eq(cmd, "save_area")) {
        /* The area on screen, from here to three levels closer: a town at
         * street level. The host caps and paces it. */
        if (g_view_zoom < 10) {
            /* At a country's or the world's scale this would be a bulk
             * download of coarse tiles, not a town to walk around in. */
            toast("info", "Zoom in to the town you want first: this saves its streets.");
            return;
        }
        int z = g_view_zoom;
        char m[120] = "{\"type\":\"ui.map.prefetch\",\"minZoom\":";
        al_cat_u(m, (unsigned long long)z, sizeof m);
        al_cat(m, ",\"maxZoom\":", sizeof m);
        al_cat_u(m, (unsigned long long)(z + 3 > 17 ? 17 : z + 3), sizeof m);
        al_cat(m, ",\"max\":1500}", sizeof m);
        say(m);
        toast("info", "Saving this area of the map for off-grid use...");
        return;
    }
    if (al_eq(cmd, "save_intro")) {
        al_json(g_buf, "intro", g_intro, sizeof g_intro);
        hal_kv_set("intro", 5, g_intro, al_len(g_intro));
        if (g_bot_on) announce_point(0);
        toast("success", g_intro[0] ? "Intro saved: it goes with your pin on other people's maps."
                                    : "Intro removed.");
        return;
    }
    if (al_eq(cmd, "set_bot")) {
        char v[8];
        al_json(g_buf, "bot_on", v, sizeof v);
        g_bot_on = al_eq(v, "true");
        if (g_bot_on) {
            char pos[40];
            my_pos(pos, sizeof pos);
            if (!pos[0]) {
                /* Every check-in names its place; a station that does not
                 * know where it stands has none to give. */
                g_bot_on = 0;
                field_set_bool("bot_on", 0);
                toast("warning", "Share your station's position first (Settings > Privacy): a check-in always names its place.");
            }
        }
        kv_put_bool("bot", g_bot_on);
        bot_update();
        announce_point(!g_bot_on);
        if (g_bot_on) toast("success", "Your station gives check-ins now, and shows on the map of anyone nearby.");
        push_station();
        push_map();
        return;
    }
    if (al_eq(cmd, "set_local")) {
        char v[8];
        al_json(g_buf, "local_only", v, sizeof v);
        g_local_only = al_eq(v, "true");
        kv_put_bool("local", g_local_only);
        toast("info", g_local_only ? "In person only: Bluetooth, WiFi and the local network."
                                   : "LoRa too: anyone within radio range may check in.");
        return;
    }
    if (al_eq(cmd, "save_greeting")) {
        al_json(g_buf, "greeting", g_greeting, sizeof g_greeting);
        hal_kv_set("greet", 5, g_greeting, al_len(g_greeting));
        toast("success", g_greeting[0] ? "Greeting saved." : "No greeting: visitors get the certificate only.");
        return;
    }
    if (al_eq(cmd, "pick_badge")) { g_picking_event = 0; pick_picture("Choose a badge"); return; }
    if (al_eq(cmd, "new_event")) {
        g_draft.badge[0] = 0;
        const char *pairs[] = { "Event badge", "none yet: the station's own badge is used" };
        details("ev_badge", pairs, 1);
        screen_open("New event", "New event");
        return;
    }
    if (al_eq(cmd, "pick_event_badge")) { g_picking_event = 1; pick_picture("Choose the event badge"); return; }
    if (al_eq(cmd, "publish_event")) { publish_event(); return; }
}

static void drain_events(void)
{
    while (hal_event_available()) {
        uint32_t n = hal_event_recv(g_topic, sizeof g_topic - 1, g_ev, sizeof g_ev - 1);
        g_ev[n < sizeof g_ev ? n : sizeof g_ev - 1] = 0;
        g_topic[sizeof g_topic - 1] = 0;
        if (al_eq(g_topic, "xprs.command")) on_command_packet(g_ev);
        else if (al_eq(g_topic, "xprs.result")) on_result(g_ev);
        else if (al_eq(g_topic, "xprs.status.tx")) on_status(g_ev);
        else if (al_eq(g_topic, "xprs.message")) on_message(g_ev);
        else if (al_eq(g_topic, "core.monitor") && now_s() >= g_nearby_at + 10) {
            g_nearby_at = now_s();
            push_nearby();
        }
    }
}

/* ── entry points ─────────────────────────────────────────────────────── */
int32_t module_init(void)
{
    who_am_i();
    db_init();
    load_settings();
    pend_load();
    listen_update();
    bot_update();
    push_all();
    return 0;
}

int32_t module_tick(void)
{
    drain_events();
    point_due();
    return 0;
}

int32_t module_handle_event(void)
{
    drain_events();
    uint32_t n = hal_msg_recv(g_buf, sizeof g_buf - 1);
    if (n == 0) return 0;
    g_buf[n < sizeof g_buf ? n : sizeof g_buf - 1] = 0;
    on_command();
    return 0;
}

/* A minute: the check-in point is renewed hourly, and nothing else here
 * runs on a clock (everything else happens because something arrived). */
int32_t module_tick_interval_ms(void) { return 60000; }

void module_destroy(void) {}
