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
 *   Check-in    one screen: the score as tiles (tap one for what it
 *               counts), the stations in Bluetooth reach with Check in,
 *               then your PLACES, one row per station you checked in at,
 *               with the visits and the time spent in its range. Tags over
 *               the list filter it: the kind of place a station says it is
 *               (its check-in point's tag:, ~50 kinds in ci.c), automatic
 *               ones (event, badge, in person, by radio) and your own.
 *   About       a check-in, an event or a place: its badge, a small map,
 *               every fact its packets carried; a place also its time,
 *               its tags and the switch for automatic check-ins
 *   Settings    (the menu) this station: where it is, check-ins on or off,
 *               its kind of place, greeting, badge, events, Visitors.
 *
 * The first check-in at a station is asked by hand; after that this wapp
 * asks once a day by itself while the station is in Bluetooth reach, until
 * the person turns that off for the place. Time in range is the core's:
 * every three minutes hal_xprs_stations says how long ago each station was
 * heard on each radio, and ci_visit_step turns that into visits.
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
static char g_out[65536];         /* one message to the page: a list of a few hundred rows */
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
static int  g_fit_here;           /* ... and that view included the person */
static int  g_fit_zoom = -1;      /* the zoom that view chose */

/* Asks out: the core delivers them; this is only what to match answers to. */
static struct { char id[7]; char station[16]; unsigned long long ts; char wire[CI_WIRE_MAX + 1]; } g_pend[PEND_MAX];
static int g_listening;           /* subscribed to the answer topics */
static unsigned long long g_greet_until; /* take a greeting until then */
static char g_greet_from[16];

/* The entry open on the Entry screen, and the event being written. */
static char g_sel[8];
static char g_ptype[24];          /* this station's kind of place (ci_places), "" none */
static int g_quiet;               /* do_checkin: an automatic ask says nothing */
static char g_ftags[200];         /* the tags the list is filtered by, ",coffee," */
static char g_sel_station[16];   /* the About page's event is run by */
static struct { char badge[80]; } g_draft;

static void say(const char *json) { hal_msg_send(json, al_len(json)); }
static int g_page_seen;           /* a page is open: there is a screen to draw */
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
    kv_str("ptype", g_ptype, sizeof g_ptype);
    kv_str("ftags", g_ftags, sizeof g_ftags);
    if (!g_ftags[0]) al_cpy(g_ftags, ",", sizeof g_ftags);
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

static void spots_init(void);
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
    /* Every XPRS station that says where it is, as the map draws it: the
     * copy the map opens on (see "Stations everywhere"). */
    db_exec("CREATE TABLE IF NOT EXISTS places (call TEXT PRIMARY KEY, nick TEXT,"
            " pos TEXT, half INTEGER, pts INTEGER, seen INTEGER)", 0);   /* wst_init extends it */
    /* Check-in events are kept by the core now (hal_xprs_follow_kind), the
     * ones heard here and the ones archivers replay, in one archive. */
    db_exec("DROP TABLE IF EXISTS heard", 0);
    /* Asks out, so an answer that arrives after the page closed is still
     * matched by whichever engine runs then (the background one, when the
     * owner turned it on). Results are never archived: missed is lost. */
    db_exec("CREATE TABLE IF NOT EXISTS asks (id TEXT PRIMARY KEY, station TEXT,"
            " ts INTEGER, wire TEXT)", 0);
    spots_init();
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
/* The filter chip the next section belongs to (the screen's `tabs`), or 0.
 * Reset by section_begin. */
static const char *g_tab;
static void section_begin(const char *title, int *any)
{
    if (*any) al_cat(g_out, ",", sizeof g_out);
    *any = 1;
    al_cat(g_out, "{", sizeof g_out);
    if (g_tab) {
        al_cat(g_out, "\"tab\":\"", sizeof g_out);
        al_jesc(g_out, g_tab, sizeof g_out);
        al_cat(g_out, "\",", sizeof g_out);
        g_tab = 0;
    }
    al_cat(g_out, "\"title\":\"", sizeof g_out);
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
/* A third tag for the next row() (how it was heard), or 0. Reset by row(). */
static const char *g_tag3;

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
    if (g_tag3 && g_tag3[0]) {
        if ((t1 && t1[0]) || (t2 && t2[0])) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out); al_jesc(g_out, g_tag3, sizeof g_out); al_cat(g_out, "\"", sizeof g_out);
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
    g_tag3 = 0;
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
/* The Stations and Events sections, as JSON, appended to Nearby's list. */
static char g_explore[32768];
static struct { char id[16]; int rssi; int age; int known; } g_near[NEAR_MAX];

static int near_before(int a, int b)
{
    if (g_near[a].known != g_near[b].known) return g_near[a].known > g_near[b].known;
    if (g_near[a].rssi != g_near[b].rssi) return g_near[a].rssi > g_near[b].rssi;
    return al_cmp(g_near[a].id, g_near[b].id) < 0;
}

/* In reach: who a check-in in person can reach. Only Bluetooth (BLE5, the
 * bearer "ble") heard in the last ten minutes: a station on the LAN is not
 * in front of you, and LoRa is not in person. The core says how long ago
 * each station was heard on each bearer ("heard"), so a LAN packet a moment
 * ago does not hide a Bluetooth one. */
#define REACH_S 600
static int json_int(const char *json, const char *key, int dflt)
{
    char v[16];
    if (!al_json(json, key, v, sizeof v) || !v[0]) return dflt;
    int neg = v[0] == '-', n = 0;
    for (const char *q = v + neg; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
    return neg ? -n : n;
}
static int gather_reach(void)
{
    int32_t n = hal_xprs_stations(g_big, sizeof g_big - 1);
    int count = 0;
    if (n <= 0) return 0;
    g_big[n] = 0;
    const char *p = g_big;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    /* The first section is what is heard now; the others are older. */
    if (!al_json_next(p, g_obj, sizeof g_obj)) return 0;
    const char *it = al_json_arr(g_obj, "items");
    static char item[1024];
    while (count < NEAR_MAX && it && (it = al_json_next(it, item, sizeof item)) != 0) {
        char id[16], kind[12], heard[160];
        al_json(item, "id", id, sizeof id);
        al_json(item, "kind", kind, sizeof kind);
        if (!id[0] || al_eq(kind, "device") || al_eq(id, g_me)) continue;
        if (!al_json_obj(item, "heard", heard, sizeof heard)) continue;
        int age = json_int(heard, "ble", -1);
        int age5 = json_int(heard, "ble5", -1);
        if (age < 0 || (age5 >= 0 && age5 < age)) age = age5;
        if (age < 0 || age > REACH_S) continue;
        al_cpy(g_near[count].id, id, sizeof g_near[count].id);
        g_near[count].rssi = json_int(item, "rssi", -200);
        g_near[count].age = age;
        g_near[count].known = known_station(id);
        count++;
    }
    return count;
}

static void reach_section(int *any)
{
    int count = gather_reach();
    int order[NEAR_MAX];
    for (int i = 0; i < count; i++) {
        int j = i;
        while (j > 0 && near_before(i, order[j - 1])) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    /* Only while a station is in reach: the list is the places otherwise. */
    if (!count) return;
    section_begin("In reach", any);
    for (int k = 0; k < count; k++) {
        int i = order[k];
        const char *id = g_near[i].id;
        char sub_[64] = "Bluetooth", tag[32] = "", tag2[32] = "";
        if (g_near[i].rssi > -200) {
            al_cat(sub_, " \xc2\xb7 ", sizeof sub_);
            if (g_near[i].rssi < 0) al_cat(sub_, "-", sizeof sub_);
            al_cat_u(sub_, (unsigned long long)(g_near[i].rssi < 0 ? -g_near[i].rssi : g_near[i].rssi), sizeof sub_);
            al_cat(sub_, " dBm", sizeof sub_);
        }
        al_cat(sub_, " \xc2\xb7 ", sizeof sub_);
        if (g_near[i].age < 60) al_cat(sub_, "heard just now", sizeof sub_);
        else {
            al_cat(sub_, "heard ", sizeof sub_);
            al_cat_u(sub_, (unsigned long long)(g_near[i].age / 60), sizeof sub_);
            al_cat(sub_, " min ago", sizeof sub_);
        }
        int had = log_count_for(id);
        if (g_near[i].known) al_cpy(tag, "gives check-ins", sizeof tag);
        if (had) {
            al_cat_u(tag2, (unsigned long long)had, sizeof tag2);
            al_cat(tag2, had == 1 ? " check-in" : " check-ins", sizeof tag2);
        }
        int asking = 0;
        for (int q = 0; q < PEND_MAX; q++)
            if (g_pend[q].id[0] && al_eq(g_pend[q].station, id)) asking = 1;
        if (asking) row(id, id, sub_, tag, tag2, "", id, "nearby_asking", "Asking...");
        else row(id, id, sub_, tag, tag2, "", id, "nearby_checkin", "Check in");
    }
    section_end();
}

/* The one screen is drawn whole: the tiles, who is in reach, the log, the
 * badges. */
static void push_log(void);
static void push_nearby(void) { push_log(); }

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
    /* In person: the station is in Bluetooth reach (see reach_section). */
    int n = ci_build_ask(wire, sizeof wire, g_me, station, now, "local");
    if (n < 0) { if (!g_quiet) toast("warning", "Set up your callsign first."); return; }
    for (int i = 0; i < PEND_MAX; i++)
        if (g_pend[i].id[0] && al_eq(g_pend[i].station, station)) {
            if (!g_quiet) toast("info", "Already asking this station.");
            return;
        }
    int slot = -1;
    for (int i = 0; i < PEND_MAX; i++) if (!g_pend[i].id[0]) { slot = i; break; }
    if (slot < 0) { if (!g_quiet) toast("warning", "Too many open requests. Try again soon."); return; }
    int rc = hal_xprs_send(wire, (uint32_t)n);
    if (rc != 0) { if (!g_quiet) toast("warning", "Could not send. Try again."); return; }
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
    if (g_quiet) return;
    char b[96] = "Asking ";
    al_cat(b, station, sizeof b);
    al_cat(b, "...", sizeof b);
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
static void spot_refresh(const char *call, const char *type_in);
static void spots_backfill(void);
static void spots_types(void);
static void visits_sample(void);
static int asked_automatically(const char *call, unsigned long long asked_at);
static void push_entry(void);
static void open_about(const char *what);
static const char *entry_value(const char *col, const char *v);
static void push_map(void);
static int has_tag(const char *wire, const char *tag);
static void label_name(char *out, unsigned cap, const char *label);

static void on_result(const char *row_)
{
    char wire[CI_EVENT_MAX + 64], sig[12];
    ci_cert_t c;
    if (!al_json(row_, "wire", wire, sizeof wire)) return;
    if (!ci_read_cert(wire, &c)) return;
    int i = pend_find(c.r);
    if (i < 0 || !al_eq(c.station, g_pend[i].station)) return;
    al_json(row_, "sig", sig, sizeof sig);
    if (al_eq(sig, "forged")) return;
    char station[16];
    al_cpy(station, g_pend[i].station, sizeof station);
    int automatic = asked_automatically(station, g_pend[i].ts);
    char body[200];
    if (c.code == CI_OK && !wst_pos_ok(c.pos)) {
        /* Signed, and still not a check-in: it names no place, and every
         * check-in is a place on the map. */
        al_cpy(body, station, sizeof body);
        al_cat(body, " sent no location. Not counted.", sizeof body);
        if (!automatic) tell("warning", body);
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
        /* Its place: the next check-ins there are automatic (the first one
         * was asked by hand, this one), until the person turns that off. */
        spot_refresh(station, "");
        spots_types();
        /* The greeting, if the owner wrote one, follows as a message. */
        al_cpy(g_greet_from, station, sizeof g_greet_from);
        g_greet_until = now_s() + 300;
        al_cpy(body, automatic ? "Checked in automatically at " : first_here ? "First check-in at " : "Checked in at ", sizeof body);
        al_cat(body, station, sizeof body);
        if (c.title[0]) { al_cat(body, " (", sizeof body); al_cat(body, c.title, sizeof body); al_cat(body, ")", sizeof body); }
        if (new_badge) al_cat(body, ". New badge!", sizeof body);
        if (first_here) al_cat(body, ". Next time here you are checked in automatically.", sizeof body);
        tell("success", body);
        g_map_fitted = 0;          /* the map shows the new place */
    } else {
        al_cpy(body, station, sizeof body);
        al_cat(body, c.code == CI_AGAIN_LATER ? ": already checked in today."
                   : c.code == CI_STALE ? ": request too late. Check your clock."
                   : c.code == CI_REFUSED ? ": in person only."
                   : c.code == CI_NO_PLACE ? ": no location set yet."
                   : ": no check-in.", sizeof body);
        if (!automatic) tell("warning", body);
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
    al_cat(body, " did not answer.", sizeof body);
    if (!asked_automatically(g_pend[i].station, g_pend[i].ts)) tell("warning", body);
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
        char wire[CI_EVENT_MAX + 64];
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
    if (!wst_pos_ok(out)) out[0] = 0;
}

/* Where the person holding this device is now, for "how far": a GPS fix,
 * else the station's shared position. Never aired. */
static int g_here_gps;            /* here_pos came from a GPS fix */
static void here_pos(char *out, unsigned cap)
{
    out[0] = 0;
    g_here_gps = 0;
    int32_t lat = hal_sensor_gps_lat(), lon = hal_sensor_gps_lon();
    if (lat != (int32_t)0x80000000 && lon != (int32_t)0x80000000 && (lat || lon)) {
        deg(out, cap, lat);
        al_cat(out, ",", cap);
        deg(out, cap, lon);
        g_here_gps = 1;
        return;
    }
    my_pos(out, cap);
}

/* ── the check-in point ─────────────────────────────────────────────── */
/* A station that gives check-ins and shares where it is says so on the air:
 * a t:event tag:checkin under CI_POINT_TITLE, with its shared position, its
 * badge and its intro, good for an hour and renewed every quarter hour while it
 * gives check-ins. Turning check-ins off airs one that closes in a minute,
 * so the pin leaves other people's maps. No position shared: no point. */
#define POINT_EVERY_S 900ULL
#define POINT_FOR_S   3600ULL
static void announce_point(int closing)
{
    char pos[40], wire[CI_EVENT_MAX + 1];
    my_pos(pos, sizeof pos);
    if (!pos[0]) return;
    who_am_i();
    if (!g_me[0]) return;
    unsigned long long now = now_s();
    int n = ci_build_point(wire, sizeof wire, g_me, now, pos,
                           now + (closing ? 60ULL : POINT_FOR_S), g_badge, g_intro, g_ptype);
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
    al_cat(body, " checked in here", sizeof body);
    tell("info", body);
    push_station();
}

/* ── Score ────────────────────────────────────────────────────────────── */
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
    /* Every tile opens what it counts (lb_stats_tap, push_detail). */
    al_cat(g_out, "\",\"tap\":true}", sizeof g_out);
}

static void count(const char *sql, char *out, unsigned cap)
{
    if (!db_one(sql, 0, "n", out, cap) || !out[0]) al_cpy(out, "0", cap);
}

static unsigned long long secs_since(const char *call, unsigned long long from);
static unsigned long long one_u(const char *sql, int with_params, const char *col);
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
    tile(&first, "total", "Check-ins", total, "all time");
    tile(&first, "stations", "Stations", stations, "visited");
    tile(&first, "local", "In person", local, "face to face");
    tile(&first, "events", "Events", events, "attended");
    tile(&first, "badges", "Badges", badges, "collected");
    tile(&first, "streak", "Streak", streak, "days in a row");
    {
        char t[40], vis[16] = "";
        ci_span(t, sizeof t, secs_since(0, 0));
        unsigned long long nv = one_u("SELECT COUNT(*) AS n FROM visits", 0, "n") +
                                one_u("SELECT COUNT(*) AS n FROM spots WHERE vstart > 0", 0, "n");
        al_cat_u(vis, nv, sizeof vis);
        tile(&first, "time", "Time", t, "in range of places");
        tile(&first, "visits", "Visits", vis, "stays in range");
    }
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

static unsigned long long u_of(const char *s);
/* The open entry's title: its event, else its station. */
static char g_entry_title[40];

/* How a check-in was heard, as a person reads it: "Bluetooth -61 dBm". */
static void heard_as(char *out, unsigned cap, const char *link, const char *rssi)
{
    out[0] = 0;
    if (link[0]) al_cpy(out, entry_value("link", link), cap);
    if (rssi[0] && !al_eq(rssi, "0")) {
        if (out[0]) al_cat(out, " ", cap);
        al_cat(out, entry_value("rssi", rssi), cap);
    }
}

/* One check-in in a list (g_obj, a log row): the station's badge as its
 * picture (asked of the core when it is not here yet), the event or the
 * station, the greeting, and tags: in person or by radio, how it was heard,
 * when (or at which station, for an event). */
static void checkin_row(void)
{
    char id[8], st[16], ts[24], grade[12], title[32], badge[80], tok[88] = "", w[24], sub_[200],
         link[16], rssi[12], how[40];
    al_json(g_obj, "id", id, sizeof id);
    al_json(g_obj, "station", st, sizeof st);
    al_json(g_obj, "ts", ts, sizeof ts);
    al_json(g_obj, "grade", grade, sizeof grade);
    al_json(g_obj, "title", title, sizeof title);
    al_json(g_obj, "badge", badge, sizeof badge);
    al_json(g_obj, "greeting", sub_, sizeof sub_);
    al_json(g_obj, "link", link, sizeof link);
    al_json(g_obj, "rssi", rssi, sizeof rssi);
    when(w, sizeof w, u_of(ts));
    if (!sub_[0]) al_cpy(sub_, title[0] ? st : w, sizeof sub_);
    if (badge[0]) {
        al_cpy(tok, "file:", sizeof tok);
        al_cat(tok, badge, sizeof tok);
        fetch_badge(badge);
    } else g_icon = "check";
    heard_as(how, sizeof how, link, rssi);
    g_tag3 = how;
    char nice[40];
    label_name(nice, sizeof nice, title);
    row(id, title[0] ? nice : st, sub_, entry_value("grade", grade), title[0] ? st : w, tok, st, "", "");
}

/* ── what a score tile counts ─────────────────────────────────────────── */
/* Tapping a tile opens what it counts: the check-ins themselves, the ones
 * in person, the stations, events and badges, the days of the streak. A
 * check-in opens its entry; a station, an event or a day opens its own
 * check-ins. [what] is the tile's id, or st:<call>, ev:<title>, day:<n>. */
static char g_detail[64];
static unsigned long long u_of(const char *s);

/* The check-ins matching [where] (an SQL condition on log, its one
 * parameter [arg] or none), newest first, as rows that open their entry. */
static int log_rows(const char *where, const char *arg)
{
    char sql[300] = "SELECT id, station, ts, grade, title, badge, greeting, link, rssi FROM log";
    if (where && where[0]) { al_cat(sql, " WHERE ", sizeof sql); al_cat(sql, where, sizeof sql); }
    al_cat(sql, " ORDER BY ts DESC LIMIT 200", sizeof sql);
    if (arg) { p_begin(); p_str(arg); p_end(); }
    int n = 0;
    if (!db_query(sql, arg ? 1 : 0)) return 0;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        checkin_row();
        n++;
    }
    return n;
}

/* One row per value of [col] (station, title or badge): how many check-ins,
 * when the last; [prefix] makes its id (st:, ev:) so a tap opens them. */
static int group_rows(const char *col, const char *where, const char *prefix)
{
    char sql[300] = "SELECT ";
    al_cat(sql, col, sizeof sql);
    al_cat(sql, " AS v, COUNT(*) AS n, MAX(ts) AS last, MAX(station) AS st, MAX(badge) AS b,"
                " SUM(CASE WHEN grade = 'local' THEN 1 ELSE 0 END) AS here FROM log", sizeof sql);
    if (where && where[0]) { al_cat(sql, " WHERE ", sizeof sql); al_cat(sql, where, sizeof sql); }
    al_cat(sql, " GROUP BY ", sizeof sql);
    al_cat(sql, col, sizeof sql);
    al_cat(sql, " ORDER BY last DESC LIMIT 200", sizeof sql);
    int n = 0;
    if (!db_query(sql, 0)) return 0;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char v[96], cnt[12], last[24], st[16], b[80], here[12], id[110], sub_[120], w[24], tok[88] = "", t1[32] = "";
        al_json(g_obj, "v", v, sizeof v);
        al_json(g_obj, "n", cnt, sizeof cnt);
        al_json(g_obj, "last", last, sizeof last);
        al_json(g_obj, "st", st, sizeof st);
        al_json(g_obj, "b", b, sizeof b);
        al_json(g_obj, "here", here, sizeof here);
        if (!v[0]) continue;
        when(w, sizeof w, u_of(last));
        al_cpy(sub_, cnt, sizeof sub_);
        al_cat(sub_, al_eq(cnt, "1") ? " check-in \xc2\xb7 " : " check-ins \xc2\xb7 ", sizeof sub_);
        al_cat(sub_, "last ", sizeof sub_);
        al_cat(sub_, w, sizeof sub_);
        if (u_of(here)) { al_cpy(t1, here, sizeof t1); al_cat(t1, " in person", sizeof t1); }
        if (b[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, b, sizeof tok); fetch_badge(b); }
        al_cpy(id, prefix, sizeof id);
        al_cat(id, al_eq(col, "badge") ? st : v, sizeof id);
        if (!tok[0]) g_icon = al_eq(col, "title") ? "campaign" : "cell_tower";
        char nice[100];
        if (al_eq(col, "title")) label_name(nice, sizeof nice, v);
        else al_cpy(nice, al_eq(col, "badge") ? st : v, sizeof nice);
        row(id, nice, sub_, t1, al_eq(col, "title") ? st : "", tok,
            al_eq(col, "title") ? "" : st, "", "");
        n++;
    }
    return n;
}

static void spot_row(void);
static int visit_rows(const char *call);
static void push_detail(const char *what, int open)
{
    char title[80] = "";
    al_cpy(g_detail, what, sizeof g_detail);
    rows_begin("detail");
    int any = 0, n = 0;
    section_begin("", &any);
    if (al_eq(what, "total")) { al_cpy(title, "Check-ins", sizeof title); n = log_rows("", 0); }
    else if (al_eq(what, "local")) { al_cpy(title, "In person", sizeof title); n = log_rows("grade = 'local'", 0); }
    else if (al_eq(what, "stations")) { al_cpy(title, "Stations visited", sizeof title); n = group_rows("station", "", "st:"); }
    else if (al_eq(what, "events")) { al_cpy(title, "Events attended", sizeof title); n = group_rows("title", "title != ''", "ev:"); }
    else if (al_eq(what, "badges")) { al_cpy(title, "Badges collected", sizeof title); n = group_rows("badge", "badge != ''", "st:"); }
    else if (al_eq(what, "streak")) {
        /* The days of the streak, newest first: how many check-ins each,
         * and where. */
        al_cpy(title, "Streak", sizeof title);
        unsigned long long today = ci_day(now_s(), zone()), want = today;
        struct { unsigned long long d, t; int n; char where[80]; } days[60];
        int nd = 0;
        if (db_query("SELECT ts, station FROM log ORDER BY ts DESC LIMIT 400", 0)) {
            static char rows_[16384];
            al_cpy(rows_, g_rows, sizeof rows_);
            const char *p = rows_;
            while (*p && *p != '[') p++;
            if (*p == '[') p++;
            while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
                char ts[24], st[16];
                al_json(g_obj, "ts", ts, sizeof ts);
                al_json(g_obj, "station", st, sizeof st);
                unsigned long long t = u_of(ts), d = ci_day(t, zone());
                if (nd == 0 && d + 1 == today) want = d;      /* none yet today */
                if (nd && days[nd - 1].d == d) {
                    days[nd - 1].n++;
                    char k[20] = " ";
                    al_cat(k, st, sizeof k);
                    int dup = 0;
                    for (const char *q = days[nd - 1].where; *q && !dup; q++)
                        dup = al_starts(q, st) && (q == days[nd - 1].where || q[-1] == ' ');
                    if (!dup && al_len(days[nd - 1].where) + al_len(st) + 3 < sizeof days[0].where) {
                        al_cat(days[nd - 1].where, ", ", sizeof days[0].where);
                        al_cat(days[nd - 1].where, st, sizeof days[0].where);
                    }
                    continue;
                }
                if (d != want || nd >= 60) break;
                days[nd].d = d;
                days[nd].t = t;
                days[nd].n = 1;
                al_cpy(days[nd].where, st, sizeof days[0].where);
                nd++;
                want--;
            }
        }
        for (int i = 0; i < nd; i++) {
            char id[32] = "day:", w[24], lbl[16], sub_[120] = "";
            al_cat_u(id, days[i].d, sizeof id);
            when(w, sizeof w, days[i].t);
            al_cpy(lbl, w, sizeof lbl);
            lbl[10] = 0;                                 /* the date */
            al_cat_u(sub_, (unsigned long long)days[i].n, sizeof sub_);
            al_cat(sub_, days[i].n == 1 ? " check-in \xc2\xb7 " : " check-ins \xc2\xb7 ", sizeof sub_);
            al_cat(sub_, days[i].where, sizeof sub_);
            g_icon = "schedule";
            row(id, lbl, sub_, "", "", "", "", "", "");
            n++;
        }
    } else if (al_starts(what, "st:")) { al_cpy(title, what + 3, sizeof title); n = log_rows("station = ?", what + 3); }
    else if (al_starts(what, "ev:")) { label_name(title, sizeof title, what + 3); n = log_rows("title = ?", what + 3); }
    else if (al_eq(what, "time")) {
        /* The places by the time spent in their range. */
        al_cpy(title, "Time in range", sizeof title);
        if (db_query("SELECT call, nick, type, badge, visits, secs, checkins, last, vstart, vlast FROM spots"
                     " WHERE visits > 0 OR vstart > 0 ORDER BY secs DESC LIMIT 200", 0)) {
            static char rows_[24576];
            al_cpy(rows_, g_rows, sizeof rows_);
            const char *p = rows_;
            while (*p && *p != '[') p++;
            if (*p == '[') p++;
            while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) { spot_row(); n++; }
        }
    } else if (al_eq(what, "visits")) {
        /* Every stay, newest first, at any place. */
        al_cpy(title, "Visits", sizeof title);
        if (db_query("SELECT v.call AS call, v.start AS start, v.stop AS stop, v.secs AS secs,"
                     " s.nick AS nick, s.type AS type FROM visits v LEFT JOIN spots s ON s.call = v.call"
                     " ORDER BY v.start DESC LIMIT 300", 0)) {
            static char rows_[24576];
            al_cpy(rows_, g_rows, sizeof rows_);
            const char *p = rows_;
            while (*p && *p != '[') p++;
            if (*p == '[') p++;
            char o[300];
            while ((p = al_json_next(p, o, sizeof o)) != 0) {
                char call[16], nick[48], type[24], v[24], w[24], d[40], id[48] = "st:";
                al_json(o, "call", call, sizeof call);
                al_json(o, "nick", nick, sizeof nick);
                al_json(o, "type", type, sizeof type);
                unsigned long long st = al_json(o, "start", v, sizeof v) ? u_of(v) : 0;
                unsigned long long secs = al_json(o, "secs", v, sizeof v) ? u_of(v) : 0;
                when(w, sizeof w, st);
                ci_span(d, sizeof d, secs);
                const ci_place_t *k = ci_place(type);
                g_icon = k ? k->icon : "schedule";
                al_cat(id, call, sizeof id);
                row(id, nick[0] ? nick : call, w, d, nick[0] ? call : "", "", "", "", "");
                n++;
            }
        }
    } else if (al_starts(what, "vi:")) {
        al_cpy(title, "Visits", sizeof title);
        n = visit_rows(what + 3);
    }
    else if (al_starts(what, "day:")) {
        /* One day's check-ins: the day is a range of ts. */
        unsigned long long d = u_of(what + 4);
        char where[96] = "ts >= ", a[24], b[24];
        unsigned long long from = 0;
        /* The first second of that day in this zone. */
        from = d * 86400ULL;
        long long z = (long long)zone();
        if (z > 0 && from >= (unsigned long long)z) from -= (unsigned long long)z;
        else if (z < 0) from += (unsigned long long)(-z);
        a[0] = 0; al_cat_u(a, from, sizeof a);
        b[0] = 0; al_cat_u(b, from + 86400ULL, sizeof b);
        al_cat(where, a, sizeof where);
        al_cat(where, " AND ts < ", sizeof where);
        al_cat(where, b, sizeof where);
        when(title, sizeof title, from + 43200ULL);
        title[10] = 0;
        n = log_rows(where, 0);
    }
    if (!n) {
        g_icon = "info";
        row("none", "Nothing yet", "Check in at a station to start.", "", "", "", "", "", "");
    }
    section_end();
    rows_end();
    if (open) screen_open("Score detail", title);
}

static void badges_section(int *any);
static int spot_rows(void);
static void push_chips(void);
static int ftag_on(const char *label);
/* The one screen: the tiles, the tags, who is in reach, then the places
 * (the tags and the search filter them), or the events or the badges when
 * that tag is chosen. */
static void push_log(void)
{
    push_stats();
    push_chips();
    rows_begin("log");
    int any = 0;
    reach_section(&any);
    if (ftag_on("event")) {
        section_begin("Events", &any);
        group_rows("title", "title != ''", "ev:");
        section_end();
    } else if (ftag_on("badge")) {
        badges_section(&any);
    } else {
        section_begin("Places", &any);
        spot_rows();
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
        if (al_eq(v, "relayed")) return "via relay";
        if (al_eq(v, "regional")) return "by radio";
    }
    if (al_eq(col, "badge")) return "in your Badges";
    if (al_eq(col, "rssi")) {
        /* The certificate says "-61dBm"; a person reads "-61 dBm". */
        static char dbm[24];
        unsigned k = 0;
        for (unsigned i = 0; v[i] && k + 1 < sizeof dbm; i++)
            if ((v[i] >= '0' && v[i] <= '9') || v[i] == '-') dbm[k++] = v[i];
        dbm[k] = 0;
        al_cat(dbm, " dBm", sizeof dbm);
        return dbm;
    }
    if (al_eq(col, "pos")) {
        static char at[48];
        unsigned i = 0, k = 0;
        for (; v[i] && k + 2 < sizeof at; i++) {
            at[k++] = v[i];
            if (v[i] == ',') at[k++] = ' ';
        }
        at[k] = 0;
        return at;
    }
    if (al_eq(col, "route")) {
        static char via[96];
        al_cpy(via, "via ", sizeof via);
        unsigned k = al_len(via);
        for (unsigned i = 0; v[i] && k + 3 < sizeof via; i++) {
            if (v[i] == ',' || v[i] == '>') { via[k++] = ','; via[k++] = ' '; }
            else via[k++] = v[i];
        }
        via[k] = 0;
        return via;
    }
    return v;
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

/* A box around places, in degrees times 1e7. g_box holds the check-ins
 * (the area saved for off-grid use); the map's views are boxes too. */
typedef struct { long s, w, n, e; int on; } box_t;
static box_t g_box;
static void box_put(box_t *b, const char *pos)
{
    long la, lo;
    if (!wst_pos_parse(pos, &la, &lo)) return;
    if (!b->on) { b->s = b->n = la; b->w = b->e = lo; b->on = 1; return; }
    if (la < b->s) b->s = la;
    if (la > b->n) b->n = la;
    if (lo < b->w) b->w = lo;
    if (lo > b->e) b->e = lo;
}
/* What the map frames: everything on it, and what is near the person
 * holding this device (within NEAR_M). */
#define NEAR_M 50000L
static box_t g_fit_all, g_fit_near;

static void cat_deg7(char *d, unsigned cap, long v)
{
    deg(d, cap, v);
}

/* Show the whole box on a phone's map: its middle, at the closest zoom
 * where it fits in about a tile and a third (some 340 px). One place alone
 * shows its streets (zoom 14). */
static void fit_box(const box_t *b)
{
    if (!b->on) return;
    long long span = (long long)b->n - b->s, spanx = (long long)b->e - b->w;
    long mid = (long)(((long long)b->s + b->n) / 2);
    long am = mid < 0 ? -mid : mid;
    /* Mercator draws a degree of latitude taller away from the equator. */
    span = am > 550000000L ? span * 2 : am > 450000000L ? span * 16 / 10
         : am > 300000000L ? span * 13 / 10 : span;
    if (spanx > span) span = spanx;
    int z = 2;
    while (z < 14 && (3600000000LL >> (z + 1)) / 10 * 13 >= span) z++;
    g_fit_zoom = z;
    char m[200] = "{\"type\":\"ui.map.viewport\",\"lat\":";
    cat_deg7(m, sizeof m, mid);
    al_cat(m, ",\"lon\":", sizeof m);
    cat_deg7(m, sizeof m, (long)(((long long)b->w + b->e) / 2));
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
    if (!g_box.on) return;
    char box[160] = "", have[160] = "";
    const long pad = 5000000L;
    cat_deg7(box, sizeof box, g_box.s - pad);
    al_cat(box, ",", sizeof box);
    cat_deg7(box, sizeof box, g_box.w - pad);
    al_cat(box, ",", sizeof box);
    cat_deg7(box, sizeof box, g_box.n + pad);
    al_cat(box, ",", sizeof box);
    cat_deg7(box, sizeof box, g_box.e + pad);
    kv_str("ovbox", have, sizeof have);
    if (al_eq(box, have)) return;
    hal_kv_set("ovbox", 5, box, al_len(box));
    char m[260] = "{\"type\":\"ui.map.prefetch\",\"bbox\":[";
    al_cat(m, box, sizeof m);
    al_cat(m, "],\"minZoom\":3,\"maxZoom\":9,\"max\":500}", sizeof m);
    say(m);
}

/* Every check-in event known, from the one archive: the core keeps the
 * events this station heard itself and those its archivers replayed
 * (hal_xprs_follow_kind "event"), one row per packet. [fn] gets each one's
 * identifier and wire. */
typedef void (*ev_fn)(const char *id, const char *wire, void *ctx);
static char g_evrows[32768];
static void each_event(ev_fn fn, void *ctx)
{
    char id[8], wire[CI_EVENT_MAX + 64];
    static const char q[] = "{\"types\":[\"event\"],\"limit\":300}";
    int32_t n = hal_xprs_history(q, sizeof q - 1, g_big, sizeof g_big - 1);
    if (n <= 0) return;
    g_big[n] = 0;
    al_cpy(g_evrows, g_big, sizeof g_evrows);
    const char *p = g_evrows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    static char obj[CI_EVENT_MAX + 400];
    while ((p = al_json_next(p, obj, sizeof obj)) != 0) {
        al_json(obj, "id", id, sizeof id);
        if (al_json(obj, "wire", wire, sizeof wire) && has_tag(wire, "checkin")) fn(id, wire, ctx);
    }
}

/* Check-in points heard (the stations giving check-ins that share where
 * they are), the newest announcement per station, still open, not ours. */
#define PT_MAX 40
static struct {
    char call[16], pos[40], intro[104], file[80], id[8];
    unsigned long long ts;
    long dist;
    int closed;        /* its newest word: no longer giving check-ins */
} g_pts[PT_MAX];
static int g_npts;

static char g_here[40];
static void point_seen(const char *id, const char *wire, void *ctx)
{
    (void)ctx;
    char from[16], title[32], u[24], ts[24], pos[40];
    al_field(wire, "title", title, sizeof title);
    if (!al_eq(title, CI_POINT_TITLE)) return;
    al_field(wire, "f", from, sizeof from);
    if (!from[0] || al_eq(from, g_me)) return;
    al_field(wire, "pos", pos, sizeof pos);
    al_field(wire, "until", u, sizeof u);
    al_field(wire, "ts", ts, sizeof ts);
    unsigned long long t = al_epoch(ts);
    int k = 0;
    while (k < g_npts && !al_eq(g_pts[k].call, from)) k++;
    if (k < g_npts && g_pts[k].ts >= t) return;      /* we hold a newer one */
    if (k == g_npts) { if (g_npts == PT_MAX) return; g_npts++; }
    /* Closed, or no place: still the station's newest word, so an older
     * announcement read after it cannot put the station back. */
    g_pts[k].closed = al_epoch(u) <= now_s() || !wst_pos_ok(pos);
    al_cpy(g_pts[k].call, from, sizeof g_pts[k].call);
    al_cpy(g_pts[k].pos, pos, sizeof g_pts[k].pos);
    al_field(wire, "m", g_pts[k].intro, sizeof g_pts[k].intro);
    al_field(wire, "file", g_pts[k].file, sizeof g_pts[k].file);
    al_cpy(g_pts[k].id, id, sizeof g_pts[k].id);
    g_pts[k].ts = t;
    g_pts[k].dist = g_here[0] ? wst_dist_m(g_here, pos) : -1;
}

static void load_points(void)
{
    g_npts = 0;
    who_am_i();
    here_pos(g_here, sizeof g_here);
    each_event(point_seen, 0);
    int keep = 0;
    for (int i = 0; i < g_npts; i++) if (!g_pts[i].closed) g_pts[keep++] = g_pts[i];
    g_npts = keep;
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

/* A place's details, with its coordinates to copy into a navigation app.
 * Its "Show on map" goes to g_sheet_pos. */
static char g_sheet_pos[40];
static void place_sheet(const char *id, const char *title, const char *intro,
                        const char *pos, long dist, const char *extra)
{
    char body[400] = "", d[24];
    if (intro[0]) { al_cat(body, intro, sizeof body); al_cat(body, "\n\n", sizeof body); }
    if (dist >= 0) {
        wst_dist_text(dist, d, sizeof d);
        al_cat(body, d, sizeof body);
        al_cat(body, " away\n", sizeof body);
    }
    if (extra[0]) { al_cat(body, extra, sizeof body); al_cat(body, "\n", sizeof body); }
    al_cat(body, pos, sizeof body);
    al_cpy(g_sheet_pos, pos, sizeof g_sheet_pos);
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
    if (i < 0) { toast("info", "No longer giving check-ins."); return; }
    char extra[120] = "", id[24] = "pt:";
    al_cpy(extra, visited(call) ? "Visited" : "Not visited yet", sizeof extra);
    al_cat(id, call, sizeof id);
    place_sheet(id, call, g_pts[i].intro, g_pts[i].pos, g_pts[i].dist, extra);
}

/* One packet reaches us both ways (heard, and archived for a station we
 * follow): its identifier is listed once. */
#define SEEN_MAX 200
static char g_seen[SEEN_MAX][8];
static int g_nseen;
static int seen_once(const char *id)
{
    for (int i = 0; i < g_nseen; i++) if (al_eq(g_seen[i], id)) return 0;
    if (g_nseen < SEEN_MAX) al_cpy(g_seen[g_nseen++], id, sizeof g_seen[0]);
    return 1;
}

/* An event's sheet: what it is, who runs it, until when, where. */
static char g_want_ev[8];
static void event_sheet_of(const char *id, const char *wire, void *ctx)
{
    (void)ctx;
    if (!al_eq(id, g_want_ev)) return;
    g_want_ev[0] = 0;
    char from[16], title[32], u[24], pos[40], m[160], nice[40], wu[24];
    char extra[80] = "By ", sid[16] = "ev:";
    al_field(wire, "title", title, sizeof title);
    al_field(wire, "f", from, sizeof from);
    al_field(wire, "until", u, sizeof u);
    al_field(wire, "pos", pos, sizeof pos);
    al_field(wire, "m", m, sizeof m);
    label_name(nice, sizeof nice, title);
    when(wu, sizeof wu, al_epoch(u));
    al_cat(extra, from, sizeof extra);
    al_cat(extra, ", until ", sizeof extra);
    al_cat(extra, wu, sizeof extra);
    al_cat(sid, id, sizeof sid);
    here_pos(g_here, sizeof g_here);
    place_sheet(sid, nice, m, pos, g_here[0] && wst_pos_ok(pos) ? wst_dist_m(g_here, pos) : -1, extra);
}

static void event_sheet(const char *id)
{
    al_cpy(g_want_ev, id, sizeof g_want_ev);
    each_event(event_sheet_of, 0);
    if (g_want_ev[0]) { g_want_ev[0] = 0; toast("info", "This event has ended."); }
}

static void fit_put(const char *pos)
{
    box_put(&g_fit_all, pos);
    if (g_here[0] && wst_pos_ok(pos) && wst_dist_m(g_here, pos) <= NEAR_M) box_put(&g_fit_near, pos);
}

static void event_pin(const char *id, const char *wire, void *ctx)
{
    (void)ctx;
    char from[16], title[32], u[24], pos[40], mid[16] = "ev:", nice[40];
    al_field(wire, "title", title, sizeof title);
    if (al_eq(title, CI_POINT_TITLE)) return;     /* drawn as a station */
    al_field(wire, "pos", pos, sizeof pos);
    al_field(wire, "until", u, sizeof u);
    if (!wst_pos_ok(pos) || al_epoch(u) <= now_s() || !seen_once(id)) return;
    al_field(wire, "f", from, sizeof from);
    al_cat(mid, id, sizeof mid);
    label_name(nice, sizeof nice, title);
    marker(mid, pos, nice, "#FFC107", from);
    fit_put(pos);
}

static void event_row(const char *id, const char *wire, void *ctx)
{
    (void)ctx;
    char from[16], title[32], s_[24], u[24], file[80], tok[88] = "", m[160], ws[24], wu[24], tag[48];
    al_field(wire, "title", title, sizeof title);
    if (al_eq(title, CI_POINT_TITLE)) return;     /* a station, listed above */
    al_field(wire, "f", from, sizeof from);
    al_field(wire, "since", s_, sizeof s_);
    al_field(wire, "until", u, sizeof u);
    al_field(wire, "file", file, sizeof file);
    al_field(wire, "m", m, sizeof m);
    unsigned long long now = now_s(), until = al_epoch(u), since = al_epoch(s_);
    if (!since) {     /* no since: it started when it was announced */
        char ts[24];
        al_field(wire, "ts", ts, sizeof ts);
        since = al_epoch(ts);
    }
    if (!until || until <= now || !seen_once(id)) return;
    when(ws, sizeof ws, since);
    when(wu, sizeof wu, until);
    al_cpy(tag, since <= now ? "on now" : "from ", sizeof tag);
    if (since > now) al_cat(tag, ws, sizeof tag);
    if (file[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, file, sizeof tok); fetch_badge(file); }
    char until_tag[40] = "until ";
    al_cat(until_tag, wu, sizeof until_tag);
    char nice[40];
    label_name(nice, sizeof nice, title);
    char rid[16] = "ev:";
    al_cat(rid, id, sizeof rid);
    g_icon = "schedule";
    row(rid, nice, m[0] ? m : from, tag, until_tag, tok, from, "", "");
}


/* ── Stations everywhere ──────────────────────────────────────────────── */
/* Every XPRS station that says where it is, the way xprs.dev/in maps them:
 * the SDK's (sdk/wapp_stations.c), which Mesh's map shares. It keeps its
 * copy in `places` in this wapp's store, walks the archive behind the page
 * and draws grey markers; check-in points are drawn green here instead. */
#define PL_RECENT_S (7ULL * 86400ULL)    /* on the map: an identity this week */

static unsigned long long u_of(const char *s)
{
    unsigned long long v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (unsigned)(*s - '0');
    return v;
}

static void ago(char *out, unsigned cap, unsigned long long ts) { wst_ago(out, cap, ts, now_s()); }

static void push_map(void);
static int point_find(const char *call);
static void fit_put(const char *pos);


/* A station's sheet, or a square's: who is there, how approximate the
 * place is, when it was last heard, and its point to copy. */
static void place_sheet_of(const char *id)
{
    char intro[400] = "", extra[120] = "", title[40] = "", pos[40] = "", v[24], a[24], sq[24];
    long half = 0;
    unsigned long long seen = 0;
    int n = 0, total = 0;
    p_begin();
    if (al_starts(id, "pl:")) {
        p_str(id + 3); p_end();
        db_query("SELECT call, nick, pos, half, seen FROM places WHERE call = ? LIMIT 1", 1);
    } else {
        p_str(id + 3); p_int(now_s() > PL_RECENT_S ? now_s() - PL_RECENT_S : 0); p_end();
        db_query("SELECT call, nick, pos, half, seen FROM places WHERE pos = ? AND seen >= ?"
                 " ORDER BY seen DESC LIMIT 60", 1);
    }
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char call[16], nick[48];
        al_json(g_obj, "call", call, sizeof call);
        al_json(g_obj, "nick", nick, sizeof nick);
        if (point_find(call) >= 0) continue;
        total++;
        if (!pos[0]) {
            al_json(g_obj, "pos", pos, sizeof pos);
            if (al_json(g_obj, "half", v, sizeof v)) half = (long)u_of(v);
            if (al_json(g_obj, "seen", v, sizeof v)) seen = u_of(v);
            al_cpy(title, call, sizeof title);
        }
        if (al_starts(id, "pl:")) { al_cpy(intro, nick, sizeof intro); continue; }
        if (n < 12) {
            if (intro[0]) al_cat(intro, "\n", sizeof intro);
            al_cat(intro, call, sizeof intro);
            if (nick[0]) { al_cat(intro, " ", sizeof intro); al_cat(intro, nick, sizeof intro); }
            n++;
        }
    }
    if (!total || !wst_pos_ok(pos)) { toast("info", "No longer on the map."); return; }
    if (total > n && al_starts(id, "sq:")) {
        al_cat(intro, "\n+", sizeof intro);
        al_cat_u(intro, (unsigned long long)(total - n), sizeof intro);
    }
    if (al_starts(id, "sq:")) {
        title[0] = 0;
        al_cat_u(title, (unsigned long long)total, sizeof title);
        al_cat(title, total == 1 ? " station" : " stations", sizeof title);
    }
    if (half) {
        wst_dist_text(half * 2, sq, sizeof sq);
        al_cpy(extra, "Somewhere in this ", sizeof extra);
        al_cat(extra, sq, sizeof extra);
        al_cat(extra, " square\n", sizeof extra);
    }
    if (al_starts(id, "pl:")) {
        ago(a, sizeof a, seen);
        al_cat(extra, "Heard ", sizeof extra);
        al_cat(extra, a, sizeof extra);
    }
    here_pos(g_here, sizeof g_here);
    place_sheet(id, title, intro, pos, g_here[0] ? wst_dist_m(g_here, pos) : -1, extra);
}

/* ── Places: time in range, automatic check-ins, tags ─────────────────── */
/* A place is a station you checked in at: one row in `spots` with what
 * your log says of it (check-ins, in person, first, last, badge, where),
 * the time you spent in its Bluetooth range (`visits`), whether the next
 * check-ins there are automatic, its kind of place (its check-in point's
 * tag:, ci_point_type) and its tags. Everything is in this wapp's zx store
 * (hal.db). Time in range is read from the core, never measured here: every
 * VISIT_SAMPLE_MS, hal_xprs_stations says how many seconds ago each station
 * was heard on each radio (XprsMonitor, the verb architecture.md 4 gives a
 * wapp), and ci_visit_step turns that into visits. The end of a visit is
 * exact (the core's own time), its start at most one sample late. */
#define VISIT_GAP_S     600ULL    /* out of Bluetooth range this long ends a visit */
#define VISIT_MIN_S     120ULL    /* shorter was passing by: not kept */
#define AUTO_FRESH_S    180       /* heard this recently: in reach for an automatic check-in */
#define AUTO_RETRY_S    1800ULL   /* one automatic try per station per half hour */

static void spots_init(void)
{
    db_exec("CREATE TABLE IF NOT EXISTS spots (call TEXT PRIMARY KEY, nick TEXT, type TEXT,"
            " pos TEXT, badge TEXT, auto INTEGER, checkins INTEGER, here INTEGER, first INTEGER,"
            " last INTEGER, visits INTEGER, secs INTEGER, vstart INTEGER, vlast INTEGER,"
            " vrssi INTEGER, atry INTEGER, tags TEXT)", 0);
    db_exec("CREATE INDEX IF NOT EXISTS spots_last ON spots(last DESC)", 0);
    db_exec("CREATE TABLE IF NOT EXISTS visits (id TEXT PRIMARY KEY, call TEXT, start INTEGER,"
            " stop INTEGER, secs INTEGER, rssi INTEGER)", 0);
    db_exec("CREATE INDEX IF NOT EXISTS visits_call ON visits(call)", 0);
    db_exec("CREATE INDEX IF NOT EXISTS visits_start ON visits(start DESC)", 0);
    db_exec("CREATE TABLE IF NOT EXISTS mytags (id TEXT PRIMARY KEY, call TEXT, label TEXT)", 0);
    db_exec("CREATE INDEX IF NOT EXISTS mytags_label ON mytags(label)", 0);
    db_exec("CREATE INDEX IF NOT EXISTS log_station ON log(station)", 0);
}

static unsigned long long one_u(const char *sql, int with_params, const char *col)
{
    char v[24];
    if (!db_one(sql, with_params, col, v, sizeof v)) return 0;
    return u_of(v);
}

/* The tags of a place, as the list filters them: ",coffee,event,mine,". The
 * automatic ones first (its kind, event, badge, in person or by radio,
 * auto), then yours. */
static void spot_tags(const char *call, const char *type, int is_auto, char *out, unsigned cap)
{
    al_cpy(out, ",", cap);
    if (type[0]) { al_cat(out, type, cap); al_cat(out, ",", cap); }
    p_begin(); p_str(call); p_end();
    if (one_u("SELECT COUNT(*) AS n FROM log WHERE station = ? AND title != ''", 1, "n")) al_cat(out, "event,", cap);
    p_begin(); p_str(call); p_end();
    if (one_u("SELECT COUNT(*) AS n FROM log WHERE station = ? AND badge != ''", 1, "n")) al_cat(out, "badge,", cap);
    p_begin(); p_str(call); p_end();
    if (one_u("SELECT COUNT(*) AS n FROM log WHERE station = ? AND grade = 'local'", 1, "n")) al_cat(out, "in-person,", cap);
    p_begin(); p_str(call); p_end();
    if (one_u("SELECT COUNT(*) AS n FROM log WHERE station = ? AND grade != 'local'", 1, "n")) al_cat(out, "by-radio,", cap);
    if (is_auto) al_cat(out, "auto,", cap);
    p_begin(); p_str(call); p_end();
    if (db_query("SELECT label FROM mytags WHERE call = ? ORDER BY label LIMIT 40", 1)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        char o[200];
        while ((p = al_json_next(p, o, sizeof o)) != 0) {
            char l[40], k[44] = ",";
            al_json(o, "label", l, sizeof l);
            al_cat(k, l, sizeof k);
            al_cat(k, ",", sizeof k);
            int dup = 0;
            for (const char *q = out; *q && !dup; q++) dup = al_starts(q, k);
            if (l[0] && !dup) { al_cat(out, l, cap); al_cat(out, ",", cap); }
        }
    }
}

/* Bring a place's row up to date from the log and its visits; makes it
 * when it is new (the next check-ins there automatic: the first was asked
 * by hand). [type] "" keeps the kind it had. */
static void spot_refresh(const char *call, const char *type_in)
{
    char v[96], type[24] = "", badge[80] = "", pos[40] = "", tags[400];
    if (!call[0]) return;
    p_begin(); p_str(call); p_end();
    db_exec("INSERT OR IGNORE INTO spots (call, nick, type, auto, checkins, here, first, last, visits,"
            " secs, vstart, vlast, vrssi, atry, tags) VALUES (?, '', '', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ',')", 1);
    if (type_in && type_in[0]) al_cpy(type, type_in, sizeof type);
    else { p_begin(); p_str(call); p_end(); db_one("SELECT type FROM spots WHERE call = ?", 1, "type", type, sizeof type); }
    p_begin(); p_str(call); p_end();
    int is_auto = (int)one_u("SELECT auto FROM spots WHERE call = ?", 1, "auto");
    p_begin(); p_str(call); p_end();
    if (db_query("SELECT badge, pos FROM log WHERE station = ? ORDER BY ts DESC LIMIT 1", 1)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        if (al_json_next(p, g_obj, sizeof g_obj)) {
            al_json(g_obj, "badge", badge, sizeof badge);
            al_json(g_obj, "pos", pos, sizeof pos);
        }
    }
    unsigned long long n, here, first, last, visits, secs, vlast;
    p_begin(); p_str(call); p_end(); n = one_u("SELECT COUNT(*) AS n FROM log WHERE station = ?", 1, "n");
    p_begin(); p_str(call); p_end(); here = one_u("SELECT COUNT(*) AS n FROM log WHERE station = ? AND grade = 'local'", 1, "n");
    p_begin(); p_str(call); p_end(); first = one_u("SELECT MIN(ts) AS n FROM log WHERE station = ?", 1, "n");
    p_begin(); p_str(call); p_end(); last = one_u("SELECT MAX(ts) AS n FROM log WHERE station = ?", 1, "n");
    p_begin(); p_str(call); p_end(); visits = one_u("SELECT COUNT(*) AS n FROM visits WHERE call = ?", 1, "n");
    p_begin(); p_str(call); p_end(); secs = one_u("SELECT SUM(secs) AS n FROM visits WHERE call = ?", 1, "n");
    p_begin(); p_str(call); p_end(); vlast = one_u("SELECT MAX(stop) AS n FROM visits WHERE call = ?", 1, "n");
    if (vlast > last) last = vlast;
    spot_tags(call, type, is_auto, tags, sizeof tags);
    p_begin();
    p_str(type); p_str(badge); p_str(pos); p_int(n); p_int(here); p_int(first); p_int(last);
    p_int(visits); p_int(secs); p_str(tags); p_str(call);
    p_end();
    db_exec("UPDATE spots SET type = ?, badge = ?, pos = ?, checkins = ?, here = ?, first = ?, last = ?,"
            " visits = ?, secs = ?, tags = ? WHERE call = ?", 1);
    (void)v;
}

/* Every station in the log has its place (a log from before places). */
static void spots_backfill(void)
{
    unsigned long long a = one_u("SELECT COUNT(DISTINCT station) AS n FROM log", 0, "n");
    unsigned long long b = one_u("SELECT COUNT(*) AS n FROM spots", 0, "n");
    if (a <= b) return;
    if (!db_query("SELECT DISTINCT station FROM log LIMIT 500", 0)) return;
    static char rows_[16384];
    al_cpy(rows_, g_rows, sizeof rows_);
    const char *p = rows_;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    char o[120];
    while ((p = al_json_next(p, o, sizeof o)) != 0) {
        char st[16];
        al_json(o, "station", st, sizeof st);
        p_begin(); p_str(st); p_end();
        if (st[0] && !one_u("SELECT COUNT(*) AS n FROM spots WHERE call = ?", 1, "n")) spot_refresh(st, "");
    }
}

/* The kinds of place the check-in points say, for the places we have. */
static void spot_type_of(const char *id, const char *wire, void *ctx)
{
    (void)id; (void)ctx;
    char t[32], f[16], type[24], have[24] = "";
    al_field(wire, "title", t, sizeof t);
    if (!al_eq(t, CI_POINT_TITLE)) return;
    al_field(wire, "f", f, sizeof f);
    ci_point_type(wire, type, sizeof type);
    if (!f[0] || !type[0]) return;
    p_begin(); p_str(f); p_end();
    if (!db_one("SELECT type FROM spots WHERE call = ?", 1, "type", have, sizeof have)) return;
    if (!al_eq(have, type)) spot_refresh(f, type);
}
static void spots_types(void) { each_event(spot_type_of, 0); }

/* A visit ended: kept when it was more than passing by. */
static void visit_close(const char *call, unsigned long long start, unsigned long long last, int rssi)
{
    if (last > start && last - start >= VISIT_MIN_S) {
        char id[40];
        al_cpy(id, call, sizeof id);
        al_cat(id, "|", sizeof id);
        al_cat_u(id, start, sizeof id);
        p_begin(); p_str(id); p_str(call); p_int(start); p_int(last); p_int(last - start);
        p_int((unsigned long long)(rssi < 0 ? -rssi : rssi)); p_end();
        db_exec("INSERT OR REPLACE INTO visits (id, call, start, stop, secs, rssi) VALUES (?,?,?,?,?,?)", 1);
    }
    p_begin(); p_str(call); p_end();
    db_exec("UPDATE spots SET vstart = 0, vlast = 0, vrssi = 0 WHERE call = ?", 1);
    spot_refresh(call, "");
}

static int checked_in_today(const char *call)
{
    unsigned long long now = now_s();
    long long z = (long long)zone() * 60;
    unsigned long long day0 = ci_day(now, zone()) * 86400ULL;
    if (z > 0 && day0 >= (unsigned long long)z) day0 -= (unsigned long long)z;
    else if (z < 0) day0 += (unsigned long long)(-z);
    p_begin(); p_str(call); p_int(day0); p_end();
    return one_u("SELECT COUNT(*) AS n FROM log WHERE station = ? AND ts >= ?", 1, "n") > 0;
}

static void do_checkin_auto(const char *call)
{
    p_begin(); p_int(now_s()); p_str(call); p_end();
    db_exec("UPDATE spots SET atry = ? WHERE call = ?", 1);
    g_quiet = 1;
    do_checkin(call);
    g_quiet = 0;
}

/* An automatic ask's answer is told only when it is a check-in. */
static int asked_automatically(const char *call, unsigned long long asked_at)
{
    p_begin(); p_str(call); p_end();
    unsigned long long t = one_u("SELECT atry FROM spots WHERE call = ?", 1, "atry");
    return t && t + 120 >= asked_at && t <= asked_at + 120;
}

/* One look at who the core heard over Bluetooth: open, extend and close
 * the visits of every place, and ask for the day's check-in where it is
 * automatic. [stations] is hal_xprs_stations' answer. */
static void visits_sample_json(const char *stations)
{
    static struct { char call[16]; int age, rssi; } h[NEAR_MAX];
    int nh = 0;
    const char *p = stations;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    static char sec[24576];
    if (al_json_next(p, sec, sizeof sec)) {          /* heard in the last minutes */
        const char *it = al_json_arr(sec, "items");
        static char item[1024];
        while (nh < NEAR_MAX && it && (it = al_json_next(it, item, sizeof item)) != 0) {
            char id[16], heard[160];
            al_json(item, "id", id, sizeof id);
            if (!id[0] || !al_json_obj(item, "heard", heard, sizeof heard)) continue;
            int age = json_int(heard, "ble", -1), age5 = json_int(heard, "ble5", -1);
            if (age < 0 || (age5 >= 0 && age5 < age)) age = age5;
            if (age < 0) continue;
            al_cpy(h[nh].call, id, sizeof h[nh].call);
            h[nh].age = age;
            h[nh].rssi = json_int(item, "rssi", 0);
            nh++;
        }
    }
    unsigned long long now = now_s();
    if (!db_query("SELECT call, auto, vstart, vlast, vrssi, atry FROM spots LIMIT 1000", 0)) return;
    static char rows_[24576];
    al_cpy(rows_, g_rows, sizeof rows_);
    p = rows_;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    char o[300], v[24];
    while ((p = al_json_next(p, o, sizeof o)) != 0) {
        char call[16];
        al_json(o, "call", call, sizeof call);
        unsigned long long start = 0, last = 0, atry = 0;
        int rssi = 0, is_auto = 0;
        if (al_json(o, "vstart", v, sizeof v)) start = u_of(v);
        if (al_json(o, "vlast", v, sizeof v)) last = u_of(v);
        if (al_json(o, "vrssi", v, sizeof v)) rssi = -(int)u_of(v);
        if (al_json(o, "atry", v, sizeof v)) atry = u_of(v);
        if (al_json(o, "auto", v, sizeof v)) is_auto = u_of(v) != 0;
        int k = 0;
        while (k < nh && !al_eq(h[k].call, call)) k++;
        unsigned long long heard_at = k < nh && (unsigned long long)h[k].age <= now ? now - (unsigned long long)h[k].age : 0;
        int act = ci_visit_step(&start, &last, heard_at, now, VISIT_GAP_S);
        if (act == CI_V_CLOSE) {
            visit_close(call, start, last, rssi);
            start = last = 0;
            act = ci_visit_step(&start, &last, heard_at, now, VISIT_GAP_S);
        }
        if (act == CI_V_OPEN || act == CI_V_EXTEND) {
            int r = k < nh ? h[k].rssi : 0;
            if (r && (!rssi || r > rssi)) rssi = r;
            p_begin(); p_int(start); p_int(last); p_int((unsigned long long)(rssi < 0 ? -rssi : rssi)); p_str(call); p_end();
            db_exec("UPDATE spots SET vstart = ?, vlast = ?, vrssi = ? WHERE call = ?", 1);
        }
        /* The day's check-in, when it is automatic here and the station is
         * in reach now. */
        if (is_auto && k < nh && h[k].age <= AUTO_FRESH_S && now >= atry + AUTO_RETRY_S &&
            !checked_in_today(call)) {
            int asking = 0;
            for (int q = 0; q < PEND_MAX; q++) if (g_pend[q].id[0] && al_eq(g_pend[q].station, call)) asking = 1;
            if (!asking) do_checkin_auto(call);
        }
    }
}

static void visits_sample(void)
{
    int32_t n = hal_xprs_stations(g_big, sizeof g_big - 1);
    if (n <= 0 || n >= (int32_t)sizeof g_big) return;
    g_big[n] = 0;
    static char copy[32768];
    al_cpy(copy, g_big, sizeof copy);
    visits_sample_json(copy);
}

/* Seconds in range at [call] since [from] (0: ever), the open visit too. */
static unsigned long long secs_since(const char *call, unsigned long long from)
{
    unsigned long long s;
    if (call) { p_begin(); p_str(call); p_int(from); p_end(); s = one_u("SELECT SUM(secs) AS n FROM visits WHERE call = ? AND start >= ?", 1, "n"); }
    else { p_begin(); p_int(from); p_end(); s = one_u("SELECT SUM(secs) AS n FROM visits WHERE start >= ?", 1, "n"); }
    /* The visits open now. */
    char sql[160] = "SELECT SUM(vlast - vstart) AS n FROM spots WHERE vstart > 0 AND vstart >= ?";
    if (call) al_cat(sql, " AND call = ?", sizeof sql);
    p_begin(); p_int(from); if (call) p_str(call); p_end();
    return s + one_u(sql, 1, "n");
}

/* A place's line in a list: "23 visits · 41 h · last today". */
static void spot_line(char *out, unsigned cap, unsigned long long visits, unsigned long long secs,
                      unsigned long long checkins, unsigned long long last)
{
    char t[40];
    out[0] = 0;
    if (visits) {
        al_cat_u(out, visits, cap);
        al_cat(out, visits == 1 ? " visit \xc2\xb7 " : " visits \xc2\xb7 ", cap);
        ci_span(t, sizeof t, secs);
        al_cat(out, t, cap);
    } else {
        al_cat_u(out, checkins, cap);
        al_cat(out, checkins == 1 ? " check-in" : " check-ins", cap);
    }
    if (last) {
        ago(t, sizeof t, last);
        al_cat(out, " \xc2\xb7 last ", cap);
        al_cat(out, t, cap);
    }
}

/* The tags the list is filtered by (",coffee,mine,"), and the words searched. */
static char g_query[64];

/* One place as a row: its badge (else its kind's icon), its name, its
 * line, and tags: its kind, in range now, the callsign under a name. */
static void spot_row(void)
{
    char call[16], nick[48], type[24], badge[80], tok[88] = "", v[24], line[120], id[24] = "st:", now_[24] = "";
    unsigned long long visits = 0, secs = 0, checkins = 0, last = 0, vstart = 0, vlast = 0;
    al_json(g_obj, "call", call, sizeof call);
    al_json(g_obj, "nick", nick, sizeof nick);
    al_json(g_obj, "type", type, sizeof type);
    al_json(g_obj, "badge", badge, sizeof badge);
    if (al_json(g_obj, "visits", v, sizeof v)) visits = u_of(v);
    if (al_json(g_obj, "secs", v, sizeof v)) secs = u_of(v);
    if (al_json(g_obj, "checkins", v, sizeof v)) checkins = u_of(v);
    if (al_json(g_obj, "last", v, sizeof v)) last = u_of(v);
    if (al_json(g_obj, "vstart", v, sizeof v)) vstart = u_of(v);
    if (al_json(g_obj, "vlast", v, sizeof v)) vlast = u_of(v);
    if (vstart) { secs += vlast - vstart; if (vlast > last) last = vlast; }
    spot_line(line, sizeof line, visits + (vstart ? 1 : 0), secs, checkins, last);
    if (vstart) al_cpy(now_, "here now", sizeof now_);
    const ci_place_t *k = ci_place(type);
    if (badge[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, badge, sizeof tok); fetch_badge(badge); }
    else g_icon = k ? k->icon : "cell_tower";
    g_tag3 = now_;
    al_cat(id, call, sizeof id);
    row(id, nick[0] ? nick : call, line, k ? k->name : "", nick[0] ? call : "", tok, call, "", "");
}

/* The places matching the tags and the search, freshest first. */
static int spot_rows(void)
{
    char sql[700] = "SELECT call, nick, type, badge, visits, secs, checkins, last, vstart, vlast FROM spots WHERE 1 = 1";
    int np = 0;
    p_begin();
    const char *t = g_ftags;
    while (*t) {
        char w[40], like[48] = "%,";
        unsigned k = 0;
        while (*t == ',') t++;
        while (*t && *t != ',' && k + 1 < sizeof w) w[k++] = *t++;
        w[k] = 0;
        if (!w[0] || al_eq(w, "event") || al_eq(w, "badge")) continue;
        al_cat(like, w, sizeof like);
        al_cat(like, ",%", sizeof like);
        al_cat(sql, " AND tags LIKE ?", sizeof sql);
        p_str(like);
        np++;
    }
    if (g_query[0]) {
        char like[80] = "%";
        al_cat(like, g_query, sizeof like);
        al_cat(like, "%", sizeof like);
        al_cat(sql, " AND (lower(call) LIKE lower(?) OR lower(nick) LIKE lower(?) OR lower(tags) LIKE lower(?))", sizeof sql);
        p_str(like); p_str(like); p_str(like);
        np++;
    }
    p_end();
    /* Freshest first, a visit going on now counting as now. */
    al_cat(sql, " ORDER BY (CASE WHEN vlast > last THEN vlast ELSE last END) DESC LIMIT 200", sizeof sql);
    if (!db_query(sql, np ? 1 : 0)) return 0;
    static char rows_[24576];
    al_cpy(rows_, g_rows, sizeof rows_);
    const char *p = rows_;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    int n = 0;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) { spot_row(); n++; }
    return n;
}

static int ftag_on(const char *label)
{
    char k[44] = ",";
    al_cat(k, label, sizeof k);
    al_cat(k, ",", sizeof k);
    for (const char *q = g_ftags; *q; q++) if (al_starts(q, k)) return 1;
    return 0;
}

static void ftag_toggle(const char *label)
{
    char out[200] = ",";
    int was = ftag_on(label);
    /* event and badge are lists of their own, not filters on places: on
     * alone. */
    int view = al_eq(label, "event") || al_eq(label, "badge");
    const char *t = g_ftags;
    while (*t && !view) {
        char w[40];
        unsigned k = 0;
        while (*t == ',') t++;
        while (*t && *t != ',' && k + 1 < sizeof w) w[k++] = *t++;
        w[k] = 0;
        if (!w[0] || al_eq(w, label) || al_eq(w, "event") || al_eq(w, "badge")) continue;
        al_cat(out, w, sizeof out);
        al_cat(out, ",", sizeof out);
    }
    if (!was) { al_cat(out, label, sizeof out); al_cat(out, ",", sizeof out); }
    al_cpy(g_ftags, out, sizeof g_ftags);
    hal_kv_set("ftags", 5, g_ftags, al_len(g_ftags));
}

/* A tag as a person reads it: a kind of place by its name, the automatic
 * ones in words, yours as written. */
static void tag_name(const char *label, char *out, unsigned cap)
{
    const ci_place_t *k = ci_place(label);
    if (k) { al_cpy(out, k->name, cap); return; }
    if (al_eq(label, "event")) { al_cpy(out, "Events", cap); return; }
    if (al_eq(label, "badge")) { al_cpy(out, "Badges", cap); return; }
    if (al_eq(label, "in-person")) { al_cpy(out, "In person", cap); return; }
    if (al_eq(label, "by-radio")) { al_cpy(out, "By radio", cap); return; }
    if (al_eq(label, "auto")) { al_cpy(out, "Automatic", cap); return; }
    al_cpy(out, label, cap);
}

/* Every tag in use, with how many places carry it, most first; the
 * kinds and yours, then the automatic ones. [fn] gets each. */
typedef void (*tag_fn)(const char *label, unsigned long long places, void *ctx);
#define TAGS_MAX 120
static void each_tag(tag_fn fn, void *ctx)
{
    static struct { char l[40]; unsigned long long n; } tg[TAGS_MAX];
    int nt = 0;
    if (!db_query("SELECT tags FROM spots LIMIT 2000", 0)) return;
    static char rows_[24576];
    al_cpy(rows_, g_rows, sizeof rows_);
    const char *p = rows_;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    char o[500], tags[460];
    while ((p = al_json_next(p, o, sizeof o)) != 0) {
        al_json(o, "tags", tags, sizeof tags);
        const char *t = tags;
        while (*t) {
            char w[40];
            unsigned k = 0;
            while (*t == ',') t++;
            while (*t && *t != ',' && k + 1 < sizeof w) w[k++] = *t++;
            w[k] = 0;
            if (!w[0]) continue;
            int i = 0;
            while (i < nt && !al_eq(tg[i].l, w)) i++;
            if (i == nt) { if (nt == TAGS_MAX) continue; al_cpy(tg[nt].l, w, sizeof tg[0].l); tg[nt].n = 0; nt++; }
            tg[i].n++;
        }
    }
    /* Kinds of place first, then yours, then the automatic ones; most
     * places first within each. */
    #define TAG_GROUP(l) (ci_place(l) ? 0 : (al_eq(l, "event") || al_eq(l, "badge") || al_eq(l, "in-person") || \
                                            al_eq(l, "by-radio") || al_eq(l, "auto")) ? 2 : 1)
    for (int i = 1; i < nt; i++)
        for (int j = i; j > 0 && (TAG_GROUP(tg[j].l) < TAG_GROUP(tg[j - 1].l) ||
                                  (TAG_GROUP(tg[j].l) == TAG_GROUP(tg[j - 1].l) && tg[j].n > tg[j - 1].n)); j--) {
            __typeof__(tg[0]) x = tg[j]; tg[j] = tg[j - 1]; tg[j - 1] = x;
        }
    for (int i = 0; i < nt; i++) fn(tg[i].l, tg[i].n, ctx);
}

/* The tag chips over the list: every tag with its count, on when chosen. */
static int g_chips_on;    /* chip_of's pass: the chosen ones, then the rest */
static void chip_of(const char *label, unsigned long long n, void *ctx)
{
    char name[48];
    int *first = (int *)ctx;
    if (ftag_on(label) != g_chips_on) return;
    tag_name(label, name, sizeof name);
    if (!*first) al_cat(g_out, ",", sizeof g_out);
    *first = 0;
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_jesc(g_out, label, sizeof g_out);
    al_cat(g_out, "\",\"label\":\"", sizeof g_out);
    al_jesc(g_out, name, sizeof g_out);
    al_cat(g_out, "\",\"count\":", sizeof g_out);
    al_cat_u(g_out, n, sizeof g_out);
    al_cat(g_out, ftag_on(label) ? ",\"on\":true}" : ",\"on\":false}", sizeof g_out);
}
static void push_chips(void)
{
    int first = 1;
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"log_tags\",\"value\":[", sizeof g_out);
    /* The chosen tags first, where they are seen without scrolling. */
    g_chips_on = 1;
    each_tag(chip_of, &first);
    g_chips_on = 0;
    each_tag(chip_of, &first);
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* The Tags page: each tag, its places, check-ins and time. */
static void tag_row(const char *label, unsigned long long n, void *ctx)
{
    (void)ctx;
    char name[48], like[48] = "%,", line[120], t[40], id[48] = "tag:";
    tag_name(label, name, sizeof name);
    al_cat(like, label, sizeof like);
    al_cat(like, ",%", sizeof like);
    p_begin(); p_str(like); p_end();
    unsigned long long ci = one_u("SELECT SUM(checkins) AS n FROM spots WHERE tags LIKE ?", 1, "n");
    p_begin(); p_str(like); p_end();
    unsigned long long s = one_u("SELECT SUM(secs) AS n FROM spots WHERE tags LIKE ?", 1, "n");
    line[0] = 0;
    al_cat_u(line, n, sizeof line);
    al_cat(line, n == 1 ? " place \xc2\xb7 " : " places \xc2\xb7 ", sizeof line);
    al_cat_u(line, ci, sizeof line);
    al_cat(line, ci == 1 ? " check-in" : " check-ins", sizeof line);
    if (s) { ci_span(t, sizeof t, s); al_cat(line, " \xc2\xb7 ", sizeof line); al_cat(line, t, sizeof line); }
    const ci_place_t *k = ci_place(label);
    g_icon = k ? k->icon : "label";
    al_cat(id, label, sizeof id);
    row(id, name, line, "", "", "", "", "", "");
}
static void push_tags_page(void)
{
    rows_begin("tags_list");
    int any = 0;
    section_begin("", &any);
    each_tag(tag_row, 0);
    section_end();
    rows_end();
}

/* Your own tag on a place: lower case, a-z 0-9 -, like a tag: on the air. */
static void tag_add(const char *call, const char *text)
{
    char l[40], id[64];
    ci_label(l, sizeof l, text);
    if (!l[0] || !call[0]) return;
    al_cpy(id, call, sizeof id);
    al_cat(id, "|", sizeof id);
    al_cat(id, l, sizeof id);
    p_begin(); p_str(id); p_str(call); p_str(l); p_end();
    db_exec("INSERT OR REPLACE INTO mytags (id, call, label) VALUES (?,?,?)", 1);
    spot_refresh(call, "");
}
static void tag_remove(const char *call, const char *label)
{
    char id[64];
    al_cpy(id, call, sizeof id);
    al_cat(id, "|", sizeof id);
    al_cat(id, label, sizeof id);
    p_begin(); p_str(id); p_end();
    db_exec("DELETE FROM mytags WHERE id = ?", 1);
    spot_refresh(call, "");
}

/* A prompt naming tags as chips: yours not on [call] yet (add), or yours
 * on it (remove). */
static void tag_prompt(const char *call, int remove)
{
    char m[2400] = "{\"type\":\"ui.prompt\",\"id\":\"";
    al_cat(m, remove ? "untag:" : "tag:", sizeof m);
    al_jesc(m, call, sizeof m);
    al_cat(m, remove ? "\",\"title\":\"Remove a tag\",\"chips\":[" : "\",\"title\":\"Add a tag\","
                "\"body\":\"Your own word for this place, like favourite or holiday-2026. "
                "Its kind of place is a tag already.\",\"input\":{\"hint\":\"favourite\"},\"chips\":[", sizeof m);
    p_begin(); p_str(call); p_end();
    const char *sql = remove ? "SELECT DISTINCT label FROM mytags WHERE call = ? ORDER BY label LIMIT 30"
                             : "SELECT DISTINCT label FROM mytags WHERE call != ? ORDER BY label LIMIT 30";
    int first = 1;
    if (db_query(sql, 1)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        char o[120];
        while ((p = al_json_next(p, o, sizeof o)) != 0) {
            char l[40];
            al_json(o, "label", l, sizeof l);
            if (!l[0]) continue;
            if (!remove) {
                char id[64];
                al_cpy(id, call, sizeof id); al_cat(id, "|", sizeof id); al_cat(id, l, sizeof id);
                p_begin(); p_str(id); p_end();
                if (one_u("SELECT COUNT(*) AS n FROM mytags WHERE id = ?", 1, "n")) continue;
            }
            if (!first) al_cat(m, ",", sizeof m);
            first = 0;
            al_cat(m, "{\"label\":\"", sizeof m);
            al_jesc(m, l, sizeof m);
            al_cat(m, "\",\"value\":\"", sizeof m);
            al_jesc(m, l, sizeof m);
            al_cat(m, "\"}", sizeof m);
        }
    }
    al_cat(m, "]}", sizeof m);
    say(m);
}

/* A place's visits, newest first, as rows: when, how long, the signal. */
static int visit_rows(const char *call)
{
    int n = 0;
    p_begin(); p_str(call); p_end();
    {
        /* The one open now. */
        char v[24];
        unsigned long long s = 0, l = 0;
        if (db_one("SELECT vstart FROM spots WHERE call = ?", 1, "vstart", v, sizeof v)) s = u_of(v);
        p_begin(); p_str(call); p_end();
        if (db_one("SELECT vlast FROM spots WHERE call = ?", 1, "vlast", v, sizeof v)) l = u_of(v);
        if (s) {
            char w[24], d[40], sub_[80] = "since ";
            when(w, sizeof w, s);
            al_cat(sub_, w + 11, sizeof sub_);
            ci_span(d, sizeof d, l - s);
            g_icon = "bluetooth";
            row("v:now", "Here now", sub_, d, "", "", "", "", "");
            n++;
        }
    }
    p_begin(); p_str(call); p_end();
    if (!db_query("SELECT start, stop, secs, rssi FROM visits WHERE call = ? ORDER BY start DESC LIMIT 300", 1)) return n;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    char o[200];
    while ((p = al_json_next(p, o, sizeof o)) != 0) {
        char v[24], ws[24], we[24], d[40], title[48], rs[16] = "";
        unsigned long long s = 0, e = 0, secs = 0, r = 0;
        if (al_json(o, "start", v, sizeof v)) s = u_of(v);
        if (al_json(o, "stop", v, sizeof v)) e = u_of(v);
        if (al_json(o, "secs", v, sizeof v)) secs = u_of(v);
        if (al_json(o, "rssi", v, sizeof v)) r = u_of(v);
        when(ws, sizeof ws, s);
        when(we, sizeof we, e);
        al_cpy(title, ws, sizeof title);              /* "2026-11-26 08:10–09:02" */
        al_cat(title, "\xe2\x80\x93", sizeof title);
        int same = 1;
        for (int i = 0; i < 10; i++) if (ws[i] != we[i]) same = 0;
        al_cat(title, same ? we + 11 : we, sizeof title);
        ci_span(d, sizeof d, secs);
        if (r) { al_cpy(rs, "-", sizeof rs); al_cat_u(rs, r, sizeof rs); al_cat(rs, " dBm", sizeof rs); }
        char id[40] = "v:";
        al_cat_u(id, s, sizeof id);
        g_icon = "schedule";
        row(id, title, "", d, rs, "", "", "", "");
        n++;
    }
    return n;
}

/* ── About: one check-in, one event, one station ───────────────────────── */
/* What a tap on a check-in, an event or a station opens: its picture, where
 * it was on a small map ($type:"place"), and every fact the packets carried
 * (the station's certificate, the event's announcement, the station's
 * check-in point and what the stations walk knows of it), then what you did
 * there. g_about is st:<call> or ev:<title>. */
static char g_about[64];

/* A details field's value, built in g_out: d_begin, d_section, d_item...,
 * d_end sends it. */
static int g_dsec, g_dfirst;
static void d_begin(const char *field)
{
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof g_out);
    al_cat(g_out, field, sizeof g_out);
    al_cat(g_out, "\",\"value\":[", sizeof g_out);
    g_dsec = 0;
}
static void d_section(const char *title)
{
    if (g_dsec) al_cat(g_out, "]},", sizeof g_out);
    al_cat(g_out, "{\"title\":\"", sizeof g_out);
    al_jesc(g_out, title, sizeof g_out);
    al_cat(g_out, "\",\"items\":[", sizeof g_out);
    g_dsec = 1;
    g_dfirst = 1;
}
static void d_item_m(const char *label, const char *value, int mono)
{
    if (!value || !value[0]) return;
    if (!g_dfirst) al_cat(g_out, ",", sizeof g_out);
    g_dfirst = 0;
    al_cat(g_out, "{\"label\":\"", sizeof g_out);
    al_jesc(g_out, label, sizeof g_out);
    al_cat(g_out, "\",\"value\":\"", sizeof g_out);
    al_jesc(g_out, value, sizeof g_out);
    al_cat(g_out, mono ? "\",\"mono\":true}" : "\"}", sizeof g_out);
}
static void d_item(const char *label, const char *value) { d_item_m(label, value, 0); }
static void d_end(void)
{
    if (g_dsec) al_cat(g_out, "]}", sizeof g_out);
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* A $type:"place" field: [pos] with [label] on a small map, or nothing. */
static void place_set(const char *field, const char *pos, const char *label)
{
    long la, lo;
    if (!pos[0] || !wst_pos_parse(pos, &la, &lo)) { field_set_str(field, ""); return; }
    char m[300] = "{\"type\":\"ui.field.set\",\"field\":\"";
    al_cat(m, field, sizeof m);
    al_cat(m, "\",\"value\":{\"lat\":", sizeof m);
    cat_deg7(m, sizeof m, la);
    al_cat(m, ",\"lon\":", sizeof m);
    cat_deg7(m, sizeof m, lo);
    al_cat(m, ",\"label\":\"", sizeof m);
    al_jesc(m, label, sizeof m);
    al_cat(m, "\"}}", sizeof m);
    say(m);
}

/* "2026-11-20 14:02, 6 d ago". */
static void when_ago(char *out, unsigned cap, unsigned long long t)
{
    char a[24];
    out[0] = 0;
    if (!t) return;
    when(out, cap, t);
    ago(a, sizeof a, t);
    al_cat(out, ", ", cap);
    al_cat(out, a, cap);
}

/* "38.7071, -9.1355 (1.2 km from you)". */
static void where_text(char *out, unsigned cap, const char *pos)
{
    out[0] = 0;
    if (!wst_pos_ok(pos)) return;
    al_cpy(out, entry_value("pos", pos), cap);
    here_pos(g_here, sizeof g_here);
    if (g_here[0]) {
        char d[24];
        wst_dist_text(wst_dist_m(g_here, pos), d, sizeof d);
        al_cat(out, " (", cap);
        al_cat(out, d, cap);
        al_cat(out, " from you)", cap);
    }
}

/* The newest announcement [from] made under [title] (any, when [from] is
 * empty), from the archive: its wire in g_evf.wire, 1 when found. */
static struct { char from[16], title[32], wire[CI_EVENT_MAX + 64]; unsigned long long ts; int found; } g_evf;
static void ev_find_of(const char *id, const char *wire, void *ctx)
{
    (void)id; (void)ctx;
    char f[16], t[32], ts[24];
    al_field(wire, "title", t, sizeof t);
    if (!al_eq(t, g_evf.title)) return;
    al_field(wire, "f", f, sizeof f);
    if (g_evf.from[0] && !al_eq(f, g_evf.from)) return;
    al_field(wire, "ts", ts, sizeof ts);
    unsigned long long v = al_epoch(ts);
    if (g_evf.found && v <= g_evf.ts) return;
    g_evf.found = 1;
    g_evf.ts = v;
    al_cpy(g_evf.wire, wire, sizeof g_evf.wire);
}
static int find_event(const char *from, const char *title)
{
    al_cpy(g_evf.from, from, sizeof g_evf.from);
    al_cpy(g_evf.title, title, sizeof g_evf.title);
    g_evf.found = 0;
    g_evf.ts = 0;
    g_evf.wire[0] = 0;
    each_event(ev_find_of, 0);
    return g_evf.found;
}

/* A station's name as its owner gave it, from its t:identity: heard here or
 * asked of the archivers (hal_xprs_names, XPRS.md 12.9.4). */
static void station_nick(const char *call, char *out, unsigned cap)
{
    static char q[80], r[4096], o[300];
    out[0] = 0;
    if (!call[0]) return;
    al_cpy(q, "{\"q\":\"", sizeof q);
    al_jesc(q, call, sizeof q);
    al_cat(q, "\",\"limit\":8,\"ask\":true}", sizeof q);
    int32_t n = hal_xprs_names(q, al_len(q), r, sizeof r - 1);
    if (n <= 0 || n >= (int32_t)sizeof r) return;
    r[n] = 0;
    const char *p = r;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while ((p = al_json_next(p, o, sizeof o)) != 0) {
        char c[16];
        al_json(o, "call", c, sizeof c);
        if (al_eq(c, call)) { al_json(o, "nick", out, cap); return; }
    }
}

/* The next string of a JSON array of strings (after al_json_arr), or 0 at
 * its end. */
static const char *str_next(const char *p, char *out, unsigned cap)
{
    out[0] = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == ',') p++;
    if (*p != '"') return 0;
    p++;
    unsigned k = 0;
    while (*p && *p != '"') { if (k + 1 < cap) out[k++] = *p; p++; }
    out[k] = 0;
    return *p ? p + 1 : 0;
}

/* What the host heard of [call] this hour (hal_xprs_station): over which
 * radios and how long ago, its signal, what it serves, whether its
 * signatures hold. Empty strings when it was not heard. */
static void station_heard(const char *call, char *heard, unsigned hcap, char *serves, unsigned scap,
                          char *sig, unsigned sigcap, char *pos, unsigned pcap)
{
    static char r[4096];
    heard[0] = serves[0] = sig[0] = 0;
    int32_t n = hal_xprs_station(call, al_len(call), r, sizeof r - 1);
    if (n <= 0 || n >= (int32_t)sizeof r) return;
    r[n] = 0;
    const char *b = al_json_arr(r, "bearers");
    char one[16], a[24];
    while (b && (b = str_next(b, one, sizeof one)) != 0) {
        const char *w = one;
        if (!w[0]) continue;
        const char *name = entry_value("link", al_eq(w, "ble5") ? "ble" : w);
        if (heard[0] && al_len(heard) >= al_len(name)) {
            /* Bluetooth twice (ble and ble5) is one radio to a person. */
            int dup = 0;
            for (const char *q = heard; *q && !dup; q++) dup = al_starts(q, name);
            if (dup) continue;
        }
        if (heard[0]) al_cat(heard, ", ", hcap);
        al_cat(heard, name, hcap);
    }
    long long ago_ms = json_int(r, "agoMs", -1);
    if (ago_ms >= 0) {
        ago(a, sizeof a, now_s() - (unsigned long long)(ago_ms / 1000));
        if (heard[0]) al_cat(heard, ", ", hcap);
        al_cat(heard, a, hcap);
    }
    int rssi = json_int(r, "rssi", 0);
    if (rssi) {
        char d[16] = "";
        if (rssi < 0) { al_cat(d, "-", sizeof d); rssi = -rssi; }
        al_cat_u(d, (unsigned long long)rssi, sizeof d);
        al_cat(heard, " (", hcap);
        al_cat(heard, entry_value("rssi", d), hcap);
        al_cat(heard, ")", hcap);
    }
    const char *sv = al_json_arr(r, "serve");
    while (sv && (sv = str_next(sv, one, sizeof one)) != 0) {
        /* One station's services, in the singular. */
        static const struct { const char *w, *name; } svc[] = {
            { "archive", "Archive (keeps messages)" }, { "relay", "Relay" },
            { "internet", "Internet gateway" }, { "aprs", "APRS gateway" },
            { "files", "Files" }, { "meshtastic", "Meshtastic bridge" }
        };
        const char *nm = one;
        for (unsigned i = 0; i < sizeof svc / sizeof svc[0]; i++) if (al_eq(one, svc[i].w)) nm = svc[i].name;
        if (serves[0]) al_cat(serves, ", ", scap);
        al_cat(serves, nm, scap);
    }
    al_json(r, "sig", sig, sigcap);
    if (!pos[0]) al_json(r, "pos", pos, pcap);
}

/* What your log holds about [where] (st or ev, value [arg]): how many,
 * in person, first and last, and the newest check-in's station, badge, pos. */
static struct { unsigned long long n, here, first, last; char st[16], badge[80], pos[40], title[32]; } g_mine;
static void mine_of(const char *col, const char *arg)
{
    char sql[300], v[24];
    g_mine.n = g_mine.here = g_mine.first = g_mine.last = 0;
    g_mine.st[0] = g_mine.badge[0] = g_mine.pos[0] = g_mine.title[0] = 0;
    al_cpy(sql, "SELECT COUNT(*) AS n, MIN(ts) AS a, MAX(ts) AS b,"
                " SUM(CASE WHEN grade = 'local' THEN 1 ELSE 0 END) AS here FROM log WHERE ", sizeof sql);
    al_cat(sql, col, sizeof sql);
    al_cat(sql, " = ?", sizeof sql);
    p_begin(); p_str(arg); p_end();
    if (!db_query(sql, 1)) return;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    if (!al_json_next(p, g_obj, sizeof g_obj)) return;
    if (al_json(g_obj, "n", v, sizeof v)) g_mine.n = u_of(v);
    if (al_json(g_obj, "a", v, sizeof v)) g_mine.first = u_of(v);
    if (al_json(g_obj, "b", v, sizeof v)) g_mine.last = u_of(v);
    if (al_json(g_obj, "here", v, sizeof v)) g_mine.here = u_of(v);
    al_cpy(sql, "SELECT station, badge, pos, title FROM log WHERE ", sizeof sql);
    al_cat(sql, col, sizeof sql);
    al_cat(sql, " = ? ORDER BY ts DESC LIMIT 1", sizeof sql);
    p_begin(); p_str(arg); p_end();
    if (!db_query(sql, 1)) return;
    p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    if (!al_json_next(p, g_obj, sizeof g_obj)) return;
    al_json(g_obj, "station", g_mine.st, sizeof g_mine.st);
    al_json(g_obj, "badge", g_mine.badge, sizeof g_mine.badge);
    al_json(g_obj, "pos", g_mine.pos, sizeof g_mine.pos);
    al_json(g_obj, "title", g_mine.title, sizeof g_mine.title);
}

static void badge_set(const char *field, const char *ref)
{
    char tok[88] = "";
    if (ref[0]) { al_cpy(tok, "file:", sizeof tok); al_cat(tok, ref, sizeof tok); fetch_badge(ref); }
    field_set_str(field, tok);
}

static void mine_section(const char *what)
{
    char v[48];
    d_section(what);
    v[0] = 0; al_cat_u(v, g_mine.n, sizeof v);
    d_item("Check-ins", g_mine.n ? v : "None yet");
    if (g_mine.n) {
        v[0] = 0; al_cat_u(v, g_mine.here, sizeof v);
        d_item("In person", v);
        when_ago(v, sizeof v, g_mine.first);
        d_item(g_mine.n == 1 ? "When" : "First", v);
        if (g_mine.n > 1) { when_ago(v, sizeof v, g_mine.last); d_item("Last", v); }
    }
}

/* An event: its announcement (name, description, who runs it, when, where,
 * badge), then your check-ins at it. */
static void push_about_event(const char *title)
{
    char nice[40], from[16] = "", m[CI_EVENT_MAX], s_[24], u[24], ts[24], pos[40] = "", file[80] = "", v[96], nick[48];
    label_name(nice, sizeof nice, title);
    mine_of("title", title);
    al_cpy(from, g_mine.st, sizeof from);
    m[0] = s_[0] = u[0] = ts[0] = 0;
    if (find_event(from, title) || find_event("", title)) {
        al_field(g_evf.wire, "f", from, sizeof from);
        al_field(g_evf.wire, "m", m, sizeof m);
        al_field(g_evf.wire, "since", s_, sizeof s_);
        al_field(g_evf.wire, "until", u, sizeof u);
        al_field(g_evf.wire, "ts", ts, sizeof ts);
        al_field(g_evf.wire, "pos", pos, sizeof pos);
        al_field(g_evf.wire, "file", file, sizeof file);
    }
    if (!pos[0]) al_cpy(pos, g_mine.pos, sizeof pos);
    badge_set("a_badge", file[0] ? file : g_mine.badge);
    place_set("a_place", pos, nice);
    d_begin("a_info");
    d_section("Event");
    d_item("Name", nice);
    d_item("About", m);
    station_nick(from, nick, sizeof nick);
    al_cpy(v, from, sizeof v);
    if (nick[0]) { al_cat(v, " \xc2\xb7 ", sizeof v); al_cat(v, nick, sizeof v); }
    d_item("Run by", v);
    unsigned long long since = al_epoch(s_), until = al_epoch(u), at = al_epoch(ts), now = now_s();
    if (!since) since = at;
    if (since) { when(v, sizeof v, since); d_item("Starts", v); }
    if (until) { when(v, sizeof v, until); d_item("Ends", v); }
    if (until) d_item("Now", until <= now ? "Ended" : since > now ? "Not started yet" : "On now");
    where_text(v, sizeof v, pos);
    d_item("Where", v);
    if (at) { when_ago(v, sizeof v, at); d_item("Announced", v); }
    if (!g_evf.found) d_item("Announcement", "Not kept here any more: shown from your check-ins.");
    mine_section("You there");
    d_end();
    field_set_bool("about_station__hidden", from[0] == 0);
    field_set_bool("about_list__hidden", g_mine.n == 0);
    field_set_bool("a_auto__hidden", 1);
    field_set_bool("add_tag__hidden", 1);
    field_set_bool("remove_tag__hidden", 1);
    field_set_bool("about_visits__hidden", 1);
    al_cpy(g_about, "ev:", sizeof g_about);
    al_cat(g_about, title, sizeof g_about);
    al_cpy(g_sel_station, from, sizeof g_sel_station);
}

/* A station: who it is (callsign, name, what its owner says about it),
 * where, whether it gives check-ins now, when it was last heard and how,
 * the events it ran, then your check-ins there. */
static void push_about_station(const char *call)
{
    char nick[48] = "", pos[40] = "", intro[104] = "", file[80] = "", u[24] = "", v[120],
         heard[120], serves[120], sig[16];
    station_nick(call, nick, sizeof nick);
    int open_ = 0;
    if (find_event(call, CI_POINT_TITLE)) {
        char pp[40];
        al_field(g_evf.wire, "m", intro, sizeof intro);
        al_field(g_evf.wire, "file", file, sizeof file);
        al_field(g_evf.wire, "until", u, sizeof u);
        al_field(g_evf.wire, "pos", pp, sizeof pp);
        if (wst_pos_ok(pp)) al_cpy(pos, pp, sizeof pos);
        open_ = al_epoch(u) > now_s();
    }
    station_heard(call, heard, sizeof heard, serves, sizeof serves, sig, sizeof sig, pos, sizeof pos);
    mine_of("station", call);
    if (!wst_pos_ok(pos)) al_cpy(pos, g_mine.pos, sizeof pos);
    badge_set("a_badge", file[0] ? file : g_mine.badge);
    place_set("a_place", pos, call);
    d_begin("a_info");
    /* Its kind, from its check-in point's tag:, kept on its place. */
    char type[24] = "";
    if (g_evf.found && al_eq(g_evf.title, CI_POINT_TITLE)) ci_point_type(g_evf.wire, type, sizeof type);
    p_begin(); p_str(call); p_end();
    int have = one_u("SELECT COUNT(*) AS n FROM spots WHERE call = ?", 1, "n") > 0;
    if (have) {
        if (type[0]) spot_refresh(call, type);
        else { p_begin(); p_str(call); p_end(); db_one("SELECT type FROM spots WHERE call = ?", 1, "type", type, sizeof type); }
        if (nick[0]) { p_begin(); p_str(nick); p_str(call); p_end(); db_exec("UPDATE spots SET nick = ? WHERE call = ?", 1); }
    }
    const ci_place_t *kind = ci_place(type);
    d_section("Station");
    d_item("Callsign", call);
    d_item("Name", nick);
    d_item("Kind of place", kind ? kind->name : "");
    d_item("About", intro);
    if (open_) { al_cpy(v, "Yes, until ", sizeof v); char w[24]; when(w, sizeof w, al_epoch(u)); al_cat(v, w, sizeof v); }
    d_item("Gives check-ins", open_ ? v : "Not now");
    where_text(v, sizeof v, pos);
    d_item("Where", v);
    d_item("Heard this hour", heard[0] ? heard : "Not heard this hour");
    d_item("Serves", serves);
    d_item("Signatures", sig);
    /* The events it announced, newest first, as far as the archive keeps them. */
    {
        char evs[400] = "";
        int k = 0;
        p_begin(); p_str(call); p_end();
        if (db_query("SELECT title, MAX(ts) AS last FROM log WHERE station = ? AND title != ''"
                     " GROUP BY title ORDER BY last DESC LIMIT 8", 1)) {
            const char *p = g_rows;
            while (*p && *p != '[') p++;
            if (*p == '[') p++;
            while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
                char t[32], n2[40];
                al_json(g_obj, "title", t, sizeof t);
                label_name(n2, sizeof n2, t);
                if (k++) al_cat(evs, ", ", sizeof evs);
                al_cat(evs, n2, sizeof evs);
            }
        }
        mine_section("You there");
        d_item("Events", evs);
    }
    /* Time in its Bluetooth range, as the core heard it. */
    unsigned long long visits = 0, vstart = 0, vlast = 0;
    int is_auto = 0;
    char tags[400] = "";
    if (have) {
        char r[24];
        p_begin(); p_str(call); p_end(); visits = one_u("SELECT visits FROM spots WHERE call = ?", 1, "visits");
        p_begin(); p_str(call); p_end(); vstart = one_u("SELECT vstart FROM spots WHERE call = ?", 1, "vstart");
        p_begin(); p_str(call); p_end(); vlast = one_u("SELECT vlast FROM spots WHERE call = ?", 1, "vlast");
        p_begin(); p_str(call); p_end(); is_auto = one_u("SELECT auto FROM spots WHERE call = ?", 1, "auto") != 0;
        p_begin(); p_str(call); p_end(); db_one("SELECT tags FROM spots WHERE call = ?", 1, "tags", tags, sizeof tags);
        d_section("Time here");
        unsigned long long now = now_s();
        if (vstart) {
            char w[24], d[40];
            when(w, sizeof w, vstart);
            ci_span(d, sizeof d, vlast - vstart);
            al_cpy(v, "since ", sizeof v);
            al_cat(v, w + 11, sizeof v);
            al_cat(v, " (", sizeof v);
            al_cat(v, d, sizeof v);
            al_cat(v, ")", sizeof v);
            d_item("Here now", v);
        }
        ci_span(v, sizeof v, secs_since(call, 0));
        d_item("In all", visits || vstart ? v : "Not measured yet");
        if (visits || vstart) {
            ci_span(v, sizeof v, secs_since(call, now > 7 * 86400ULL ? now - 7 * 86400ULL : 0));
            d_item("Last 7 days", v);
            ci_span(v, sizeof v, secs_since(call, now > 30 * 86400ULL ? now - 30 * 86400ULL : 0));
            d_item("Last 30 days", v);
            v[0] = 0;
            al_cat_u(v, visits + (vstart ? 1 : 0), sizeof v);
            d_item("Visits", v);
            if (visits) {
                p_begin(); p_str(call); p_end();
                ci_span(v, sizeof v, one_u("SELECT SUM(secs) AS n FROM visits WHERE call = ?", 1, "n") / visits);
                d_item("Usual stay", v);
                p_begin(); p_str(call); p_end();
                ci_span(v, sizeof v, one_u("SELECT MAX(secs) AS n FROM visits WHERE call = ?", 1, "n"));
                d_item("Longest", v);
                /* How often: visits a week, counted over the days since the
                 * first (a daily place is 7, not 6 for a week begun). */
                p_begin(); p_str(call); p_end();
                unsigned long long first = one_u("SELECT MIN(start) AS n FROM visits WHERE call = ?", 1, "n");
                unsigned long long days = first && now > first ? (now - first + 86399ULL) / 86400ULL : 1;
                unsigned long long per_wk10 = visits * 70ULL / (days ? days : 1);   /* tenths */
                v[0] = 0;
                if (per_wk10 >= 10) {
                    al_cat_u(v, (per_wk10 + 5) / 10, sizeof v);
                    al_cat(v, " a week", sizeof v);
                } else {
                    al_cat(v, "once every ", sizeof v);
                    al_cat_u(v, (days + visits * 7 / 2) / (visits * 7), sizeof v);
                    al_cat(v, " weeks", sizeof v);
                }
                d_item("How often", v);
                /* The hour most visits begin. */
                p_begin(); p_str(call); p_end();
                if (db_query("SELECT start FROM visits WHERE call = ? ORDER BY start DESC LIMIT 200", 1)) {
                    int hours[24] = {0}, best = -1;
                    const char *q = g_rows;
                    while (*q && *q != '[') q++;
                    if (*q == '[') q++;
                    char o[80];
                    while ((q = al_json_next(q, o, sizeof o)) != 0) {
                        char sv[24];
                        al_json(o, "start", sv, sizeof sv);
                        long long t = (long long)u_of(sv) + (long long)zone() * 60;
                        int h = (int)((t % 86400 + 86400) % 86400 / 3600);
                        hours[h]++;
                        if (best < 0 || hours[h] > hours[best]) best = h;
                    }
                    if (best >= 0) {
                        v[0] = 0;
                        al_cat(v, "around ", sizeof v);
                        al_cat_u(v, (unsigned long long)best, sizeof v);
                        al_cat(v, ":00", sizeof v);
                        d_item("Usually arrive", v);
                    }
                }
            }
        }
        /* Its tags, as words. */
        char words[300] = "";
        const char *t = tags;
        while (*t) {
            char w[40], nm[48];
            unsigned k = 0;
            while (*t == ',') t++;
            while (*t && *t != ',' && k + 1 < sizeof w) w[k++] = *t++;
            w[k] = 0;
            if (!w[0]) continue;
            tag_name(w, nm, sizeof nm);
            if (words[0]) al_cat(words, ", ", sizeof words);
            al_cat(words, nm, sizeof words);
        }
        d_section("Tags");
        d_item("On this place", words);
        (void)r;
    }
    d_end();
    field_set_bool("about_station__hidden", 1);
    field_set_bool("about_list__hidden", g_mine.n == 0);
    field_set_bool("a_auto__hidden", !have);
    field_set_bool("a_auto", is_auto);
    field_set_bool("add_tag__hidden", !have);
    p_begin(); p_str(call); p_end();
    field_set_bool("remove_tag__hidden", !have || !one_u("SELECT COUNT(*) AS n FROM mytags WHERE call = ?", 1, "n"));
    field_set_bool("about_visits__hidden", !visits && !vstart);
    al_cpy(g_about, "st:", sizeof g_about);
    al_cat(g_about, call, sizeof g_about);
    g_sel_station[0] = 0;
}

static void open_about(const char *what)
{
    char nice[40];
    if (al_starts(what, "ev:")) {
        push_about_event(what + 3);
        label_name(nice, sizeof nice, what + 3);
        screen_open("About", nice);
    } else if (al_starts(what, "st:")) {
        char title[48];
        push_about_station(what + 3);
        /* Its name when it has one, as on the list. */
        station_nick(what + 3, title, sizeof title);
        screen_open("About", title[0] ? title : what + 3);
    }
}

/* One check-in: the badge it collected, where it was on a small map, and
 * everything the station's certificate said, with the event's description
 * and the station's name and words beside it. */
static char g_entry_st[16], g_entry_ev[32];
static void push_entry(void)
{
    if (!g_sel[0]) return;
    p_begin(); p_str(g_sel); p_end();
    if (!db_query("SELECT * FROM log WHERE id = ?", 1)) return;
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    static char row_[sizeof g_obj];
    if (!al_json_next(p, row_, sizeof row_)) return;
    char st[16] = "", ev[32] = "", nice[40] = "", b[80] = "", pos[40] = "", ts[24] = "", v[CI_WIRE_MAX + 64];
    al_json(row_, "station", st, sizeof st);
    al_json(row_, "title", ev, sizeof ev);
    al_json(row_, "badge", b, sizeof b);
    al_json(row_, "pos", pos, sizeof pos);
    al_json(row_, "ts", ts, sizeof ts);
    label_name(nice, sizeof nice, ev);
    al_cpy(g_entry_st, st, sizeof g_entry_st);
    al_cpy(g_entry_ev, ev, sizeof g_entry_ev);
    al_cpy(g_entry_title, ev[0] ? nice : st, sizeof g_entry_title);
    badge_set("e_badge", b);
    place_set("e_place", pos, st);

    /* What the announcements say about the station and the event. */
    char nick[48], intro[104] = "", about_ev[CI_EVENT_MAX] = "";
    station_nick(st, nick, sizeof nick);
    if (find_event(st, CI_POINT_TITLE)) al_field(g_evf.wire, "m", intro, sizeof intro);
    if (ev[0] && find_event(st, ev)) al_field(g_evf.wire, "m", about_ev, sizeof about_ev);

    d_begin("e_info");
    d_section("Check-in");
    when_ago(v, sizeof v, u_of(ts));
    d_item("When", v);
    d_item("Event", nice);
    d_item("About the event", about_ev);
    d_item("Station", st);
    d_item("Name", nick);
    d_item("About the station", intro);
    static const struct { const char *col, *label; } rows_[] = {
        { "greeting", "Greeting" }, { "grade", "Contact" }, { "link", "Radio" },
        { "rssi", "Signal" }, { "route", "Relays" }
    };
    for (unsigned i = 0; i < sizeof rows_ / sizeof rows_[0]; i++)
        if (al_json(row_, rows_[i].col, v, sizeof v) && v[0] && !(al_eq(rows_[i].col, "rssi") && al_eq(v, "0")))
            d_item(rows_[i].label, entry_value(rows_[i].col, v));
    where_text(v, sizeof v, pos);
    d_item("Where", v);
    d_end();

    /* The proof, as it came: for anyone who wants to check it. */
    d_begin("e_proof");
    d_section("Proof");
    if (al_json(row_, "cert", v, sizeof v)) d_item_m("The station's signed reply", v, 1);
    if (al_json(row_, "ask", v, sizeof v)) d_item_m("Your request", v, 1);
    d_end();
    field_set_bool("entry_event__hidden", ev[0] == 0);
}

static void push_map(void)
{
    uint64_t t0 = hal_time_ms();
    say("{\"type\":\"ui.map.markers.clear\"}");
    g_box.on = g_fit_all.on = g_fit_near.on = 0;
    who_am_i();
    here_pos(g_here, sizeof g_here);
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
            if (!wst_pos_ok(pos)) continue;
            unsigned long long t = 0;
            for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
            when(w, sizeof w, t);
            al_cat(mid, id, sizeof mid);
            al_cpy(detail, w, sizeof detail);
            if (title[0]) { al_cat(detail, " \xC2\xB7 ", sizeof detail); al_cat(detail, title, sizeof detail); }
            marker(mid, pos, st, "#FAFAFA", detail);
            box_put(&g_box, pos);
            fit_put(pos);
        }
    }
    /* Check-in events still to come or running: the places to go next. */
    g_nseen = 0;
    each_event(event_pin, 0);
    /* Stations giving check-ins: green to visit, pale once visited. */
    load_points();
    for (int i = 0; i < g_npts; i++) {
        char mid[24] = "st:", detail[64] = "";
        int been = visited(g_pts[i].call);
        al_cat(mid, g_pts[i].call, sizeof mid);
        if (g_pts[i].dist >= 0) { wst_dist_text(g_pts[i].dist, detail, sizeof detail); al_cat(detail, " away", sizeof detail); }
        if (been) al_cat(detail, detail[0] ? " \xC2\xB7 visited" : "visited", sizeof detail);
        marker(mid, g_pts[i].pos, g_pts[i].call, been ? "#A5D6A7" : "#43A047", detail);
        fit_put(g_pts[i].pos);
    }
    /* Every other station that says where it is, from the copy kept here. */
    uint64_t t1 = hal_time_ms();
    wst_draw();
    wst_status();
    uint64_t t2 = hal_time_ms();
    char me[40];
    my_pos(me, sizeof me);
    if (me[0] && g_bot_on) { marker("me", me, "My station", "#4FC3F7", ""); fit_put(me); }
    if (g_here_gps) marker("here", g_here, "You", "#4FC3F7", "");
    if (g_here[0]) fit_put(g_here);
    /* The first view: around the person and what is near them, else
     * everything there is. */
    if (!g_map_fitted && (g_fit_near.on || g_fit_all.on)) {
        fit_box(g_fit_near.on ? &g_fit_near : &g_fit_all);
        g_map_fitted = 1;
        g_fit_here = g_here[0] != 0;
    }
    save_overview();
    /* Name the slow part when the map is slow (performance.md 8.29). */
    uint64_t t3 = hal_time_ms();
    if (t3 - t0 >= 100) {
        char l[120] = "perf: map took ";
        al_cat_u(l, t3 - t0, sizeof l);
        al_cat(l, "ms (before stations=", sizeof l);
        al_cat_u(l, t1 - t0, sizeof l);
        al_cat(l, "ms stations=", sizeof l);
        al_cat_u(l, t2 - t1, sizeof l);
        al_cat(l, "ms after=", sizeof l);
        al_cat_u(l, t3 - t2, sizeof l);
        al_cat(l, "ms)", sizeof l);
        log_host(l);
    }
}

/* ── Badges ───────────────────────────────────────────────────────────── */
/* The badge wall: a section of the logbook's list, under its own chip. */
static void badges_section(int *any)
{
    /* One per badge: an event's badge given by two stations is one badge. */
    if (db_query("SELECT badge, MAX(station) AS station, MAX(title) AS title, COUNT(*) AS n,"
                 " MAX(ts) AS last FROM log WHERE badge != '' GROUP BY badge ORDER BY last DESC", 0)) {
        const char *p = g_rows;
        while (*p && *p != '[') p++;
        if (*p == '[') p++;
        section_begin("Badges", any);
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
            al_cpy(sub_, n, sizeof sub_);
            al_cat(sub_, al_eq(n, "1") ? " time" : " times", sizeof sub_);
            al_cpy(id, badge, sizeof id);
            al_cat(id, "|", sizeof id);
            al_cat(id, st, sizeof id);
            char nice[40];
            label_name(nice, sizeof nice, title);
            row(id, title[0] ? nice : st, sub_, title[0] ? st : "", w, tok, st, "", "");
            fetch_badge(badge);   /* the core keeps it once it has it */
        }
        section_end();
    }
}

/* The badges are part of the logbook's list. */
static void push_badges(void) { push_log(); }

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
    rows_begin("events");
    unsigned head = al_len(g_out);    /* the sections start here */
    int any = 0;
    load_points();
    if (g_npts) {
        section_begin("Stations", &any);
        for (int i = 0; i < g_npts; i++) {
            char id[24] = "st:", d[40] = "", tok[88] = "";
            al_cat(id, g_pts[i].call, sizeof id);
            if (g_pts[i].dist >= 0) { wst_dist_text(g_pts[i].dist, d, sizeof d); al_cat(d, " away", sizeof d); }
            if (g_pts[i].file[0]) {
                al_cpy(tok, "file:", sizeof tok);
                al_cat(tok, g_pts[i].file, sizeof tok);
                fetch_badge(g_pts[i].file);
            }
            g_icon = "cell_tower";
            row(id, g_pts[i].call, g_pts[i].intro[0] ? g_pts[i].intro : "Gives check-ins",
                d, visited(g_pts[i].call) ? "visited" : "", tok, g_pts[i].call, "", "");
        }
        section_end();
    }
    section_begin("Events", &any);
    g_nseen = 0;
    each_event(event_row, 0);
    section_end();
    al_cpy(g_explore, g_out + head, sizeof g_explore);
    push_nearby();
}

/* ── Settings (this station) ───────────────────────────────────────────────────── */
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

/* The Guestbook screen: one list (the host shows one per screen), in two
 * sections: the station's events, and its visitors. */
static void push_guests(void)
{
    rows_begin("guests");
    int any = 0;
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
            if (first_g) { section_begin("Visitors", &any); first_g = 0; }
            al_json(g_obj, "id", id, sizeof id);
            al_json(g_obj, "visitor", who, sizeof who);
            al_json(g_obj, "ts", ts, sizeof ts);
            al_json(g_obj, "grade", grade, sizeof grade);
            al_json(g_obj, "title", title, sizeof title);
            unsigned long long t = 0;
            for (const char *q = ts; *q >= '0' && *q <= '9'; q++) t = t * 10 + (unsigned)(*q - '0');
            when(w, sizeof w, t);
            row(id, who, w, entry_value("grade", grade), title, "", who, "", "");
        }
        if (!first_g) section_end();
    }
    rows_end();
}

static void push_station(void)
{
    field_set_bool("bot_on", g_bot_on);
    field_set_bool("local_only", g_local_only);
    field_set_str("greeting", g_greeting);
    field_set_str("intro", g_intro);
    field_set_str("place_type", g_ptype[0] ? g_ptype : "none");
    field_set_str("badge_pic", g_badge);
    field_set_str("pick_badge__label", g_badge[0] ? "Change badge" : "Choose badge");
    char pos[40];
    my_pos(pos, sizeof pos);
    char where[80] = "";
    if (pos[0]) {
        al_cpy(where, pos, sizeof where);
        if (g_acc > 0) {
            char r[24];
            wst_dist_text(g_acc, r, sizeof r);
            al_cat(where, " (within ", sizeof where);
            al_cat(where, r, sizeof where);
            al_cat(where, ")", sizeof where);
        }
    }
    /* The one thing a station needs before it can give check-ins, and the
     * button under it opens the setting itself (share_location). */
    const char *loc[] = {
        "Location", where[0] ? where : "Not shared yet. Needed to give check-ins.",
    };
    details("st_info", loc, 1);
    field_set_str("share_location__label", where[0] ? "Change location" : "Share my location");
    char visitors[32] = "Visitors", n[12];
    count("SELECT COUNT(*) AS n FROM guests", n, sizeof n);
    if (n[0] && !al_eq(n, "0")) {
        al_cat(visitors, " (", sizeof visitors);
        al_cat(visitors, n, sizeof visitors);
        al_cat(visitors, ")", sizeof visitors);
    }
    field_set_str("guests_open__label", visitors);
    char title[32], ebadge[80], nice[40] = "None running";
    active_event(title, sizeof title, ebadge, sizeof ebadge);
    if (title[0]) label_name(nice, sizeof nice, title);
    const char *ev[] = { "Now", nice };
    details("ev_info", ev, 1);
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
        toast("warning", "Could not open that picture.");
        return;
    }
    if (g_picking_event) {
        g_picking_event = 0;
        al_cpy(g_draft.badge, token, sizeof g_draft.badge);
        field_set_str("ev_pic", token);
        return;
    }
    al_cpy(g_badge, token, sizeof g_badge);
    hal_kv_set("badge", 5, g_badge, al_len(g_badge));
    publish_badge(g_badge);
    if (g_bot_on) announce_point(0);
    push_station();
    toast("success", "Badge saved.");
}

static void publish_event(void)
{
    char title[64], desc[160], len[8], label[32], wire[CI_EVENT_MAX + 1], pos[40];
    al_json(g_buf, "ev_title", title, sizeof title);
    al_json(g_buf, "ev_desc", desc, sizeof desc);
    if (!al_json(g_buf, "ev_len", len, sizeof len) || !len[0]) al_cpy(len, "day", sizeof len);
    ci_label(label, sizeof label, title);
    if (!label[0]) { toast("warning", "Name the event."); return; }
    if (al_eq(label, CI_POINT_TITLE)) { toast("warning", "That name is reserved. Pick another."); return; }
    unsigned long long now = now_s();
    unsigned long long span = al_eq(len, "month") ? 30ULL * 86400 : al_eq(len, "week") ? 7ULL * 86400 : 86400ULL;
    who_am_i();
    my_pos(pos, sizeof pos);
    int n = ci_build_event(wire, sizeof wire, g_me, now, pos, label, now, now + span,
                           g_draft.badge, desc);
    if (n < 0) { toast("warning", "Name too long."); return; }
    if (hal_xprs_send(wire, (uint32_t)n) != 0) { toast("warning", "Could not announce. Try again."); return; }
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
    toast("success", "Event announced.");
}

/* ── the page ─────────────────────────────────────────────────────────── */
static void push_all(void)
{
    push_log();          /* who is in reach, the log, the badges */
    push_station();
}

/* A page is open: follow the station table (core.monitor) so Nearby shows
 * who walks into reach, at most once every ten seconds. */
static unsigned long long g_nearby_at, g_explore_at;
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
        /* The person zoomed on their own: the first view is theirs. */
        if (g_view_zoom >= 5 && g_view_zoom != g_fit_zoom) {
            g_map_fitted = 1;
            g_fit_here = 1;
        }
        return;
    }
    if (al_eq(type, "map.prefetch")) {
        char state[12], held[12];
        al_json(g_buf, "state", state, sizeof state);
        al_json(g_buf, "held", held, sizeof held);
        if (al_eq(state, "busy")) toast("info", "Already saving an area.");
        if (al_eq(state, "done") && held[0] && !al_eq(held, "0")) {
            char body[120] = "Map saved offline (";
            al_cat(body, held, sizeof body);
            al_cat(body, " tiles).", sizeof body);
            toast("success", body);
        }
        return;
    }
    if (!al_json(g_buf, "command", cmd, sizeof cmd) &&
        !al_json(g_buf, "action", cmd, sizeof cmd)) return;
    if (al_eq(cmd, "ready") || al_eq(cmd, "refresh")) {
        who_am_i();
        push_all();          /* the map from the copy kept here, at once */
        wst_begin(1);     /* then brought up to date, a step per event or tick */
        return;
    }
    if (al_eq(cmd, "nearby_refresh")) { push_events(); push_map(); return; }
    if (al_eq(cmd, "nearby_asking")) {
        toast("info", "Still waiting for an answer.");
        return;
    }
    if (al_eq(cmd, "nearby_checkin") || al_eq(cmd, "nearby_tap")) {
        /* The button on a row of the one list sends that row as log_id. */
        if ((!al_json(g_buf, "log_id", id, sizeof id) || !id[0]) &&
            (!al_json(g_buf, "nearby_id", id, sizeof id) || !id[0])) return;
        /* A station or event further down the list opens its sheet; a
         * station in reach is checked in at. */
        if (al_starts(id, "st:")) point_sheet(id + 3);
        else if (al_starts(id, "ev:")) event_sheet(id + 3);
        else do_checkin(id);
        return;
    }
    if (al_eq(cmd, "set_chase")) {
        char v[8];
        al_json(g_buf, "chase_regional", v, sizeof v);
        g_chase_regional = al_eq(v, "true");
        kv_put_bool("chase_reg", g_chase_regional);
        toast("info", g_chase_regional ? "LoRa on." : "LoRa off.");
        return;
    }
    if (al_eq(cmd, "lb_stats_tap")) {
        if (al_json(g_buf, "lb_stats_id", id, sizeof id) && id[0]) push_detail(id, 1);
        return;
    }
    if (al_eq(cmd, "detail_tap")) {
        if (!al_json(g_buf, "detail_id", id, sizeof id) || !id[0] || al_eq(id, "none") || al_starts(id, "v:")) return;
        if (al_starts(id, "st:") || al_starts(id, "ev:")) { open_about(id); return; }
        if (al_starts(id, "day:")) { push_detail(id, 1); return; }
        al_cpy(g_sel, id, sizeof g_sel);
        push_entry();
        screen_open("Entry", g_entry_title[0] ? g_entry_title : "Check-in");
        return;
    }
    if (al_eq(cmd, "log_tags_tap")) {
        if (al_json(g_buf, "log_tags_id", id, sizeof id) && id[0]) ftag_toggle(id);
        push_log();
        return;
    }
    if (al_eq(cmd, "log_search")) {
        al_json(g_buf, "log_query", g_query, sizeof g_query);
        push_log();
        return;
    }
    if (al_eq(cmd, "tags_open")) {
        push_tags_page();
        screen_open("Tags", "Tags");
        return;
    }
    if (al_eq(cmd, "tags_list_tap")) {
        /* A tag on the Tags page: the list filtered by it alone. */
        if (!al_json(g_buf, "tags_list_id", id, sizeof id) || !al_starts(id, "tag:")) return;
        al_cpy(g_ftags, ",", sizeof g_ftags);
        ftag_toggle(id + 4);
        push_log();
        say("{\"type\":\"ui.screen.close\"}");
        return;
    }
    if (al_eq(cmd, "set_auto")) {
        char v[8];
        if (!al_starts(g_about, "st:")) return;
        al_json(g_buf, "a_auto", v, sizeof v);
        p_begin(); p_int(al_eq(v, "true") ? 1 : 0); p_str(g_about + 3); p_end();
        db_exec("UPDATE spots SET auto = ? WHERE call = ?", 1);
        spot_refresh(g_about + 3, "");
        toast("info", al_eq(v, "true") ? "Checked in automatically here, once a day." : "Check in here by hand.");
        push_log();
        return;
    }
    if (al_eq(cmd, "add_tag") || al_eq(cmd, "remove_tag")) {
        if (al_starts(g_about, "st:")) tag_prompt(g_about + 3, al_eq(cmd, "remove_tag"));
        return;
    }
    if (al_eq(cmd, "about_visits")) {
        char what[24] = "vi:";
        if (!al_starts(g_about, "st:")) return;
        al_cat(what, g_about + 3, sizeof what);
        push_detail(what, 1);
        return;
    }
    if (al_eq(cmd, "log_tap")) {
        if (!al_json(g_buf, "log_id", id, sizeof id)) return;
        if (al_starts(id, "st:") || al_starts(id, "ev:")) { open_about(id); return; }
        /* A badge (<badge>|<station>): what gave it, its event or else its station. */
        for (const char *q = id; *q; q++) if (*q == '|') {
            char b[80], t[32] = "", what[64];
            unsigned k = (unsigned)(q - id);
            if (k >= sizeof b) return;
            al_cpy(b, id, k + 1);
            p_begin(); p_str(b); p_end();
            db_one("SELECT title FROM log WHERE badge = ? ORDER BY ts DESC LIMIT 1", 1, "title", t, sizeof t);
            al_cpy(what, t[0] ? "ev:" : "st:", sizeof what);
            al_cat(what, t[0] ? t : q + 1, sizeof what);
            open_about(what);
            return;
        }
        al_cpy(g_sel, id, sizeof g_sel);
        push_entry();
        screen_open("Entry", g_entry_title[0] ? g_entry_title : "Check-in");
        return;
    }
    if (al_eq(cmd, "about_list")) { if (g_about[0]) push_detail(g_about, 1); return; }
    if (al_eq(cmd, "about_station")) {
        char what[24] = "st:";
        if (!g_sel_station[0]) return;
        al_cat(what, g_sel_station, sizeof what);
        open_about(what);
        return;
    }
    if (al_eq(cmd, "entry_station") || al_eq(cmd, "entry_event")) {
        char what[48];
        int ev = al_eq(cmd, "entry_event");
        if (!(ev ? g_entry_ev : g_entry_st)[0]) return;
        al_cpy(what, ev ? "ev:" : "st:", sizeof what);
        al_cat(what, ev ? g_entry_ev : g_entry_st, sizeof what);
        open_about(what);
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
        char body[96] = "Removed check-in";
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
        if (!al_json(g_buf, "events_id", id, sizeof id) || !id[0]) return;
        if (al_starts(id, "st:")) point_sheet(id + 3);
        else event_sheet(al_starts(id, "ev:") ? id + 3 : id);
        return;
    }
    /* Back from the position setting the button opened: show what it is now. */
    if (al_eq(cmd, "share_location")) { push_station(); push_map(); return; }
    if (al_eq(cmd, "guests_open")) {
        push_guests();
        screen_open("Visitors", "Visitors");
        return;
    }
    if (al_eq(cmd, "prompt")) {
        char pid[32], v[12];
        al_json(g_buf, "prompt_id", pid, sizeof pid);
        al_json(g_buf, "prompt_value", v, sizeof v);
        if (al_starts(pid, "tag:") || al_starts(pid, "untag:")) {
            char word[48];
            int remove = al_starts(pid, "untag:");
            const char *call = pid + (remove ? 6 : 4);
            if (!al_json(g_buf, "prompt_value", word, sizeof word) || !word[0])
                al_json(g_buf, "prompt_input", word, sizeof word);
            if (!word[0]) return;
            if (remove) tag_remove(call, word); else tag_add(call, word);
            char what[24] = "st:";
            al_cat(what, call, sizeof what);
            push_about_station(call);
            push_log();
            return;
        }
        if ((al_starts(pid, "pt:") || al_starts(pid, "ev:") || al_starts(pid, "pl:") || al_starts(pid, "sq:")) &&
            al_eq(v, "map") && wst_pos_ok(g_sheet_pos)) {
            char lat[24], m[120] = "{\"type\":\"ui.map.locate\",\"lat\":";
            unsigned k = 0;
            while (g_sheet_pos[k] && g_sheet_pos[k] != ',' && k + 1 < sizeof lat) { lat[k] = g_sheet_pos[k]; k++; }
            lat[k] = 0;
            al_cat(m, lat, sizeof m);
            al_cat(m, ",\"lon\":", sizeof m);
            al_cat(m, g_sheet_pos + k + 1, sizeof m);
            al_cat(m, "}", sizeof m);
            say(m);
        }
        return;
    }
    if (al_eq(cmd, "marker_tap")) {
        if (!al_json(g_buf, "id", id, sizeof id)) return;
        if (al_starts(id, "st:")) { point_sheet(id + 3); return; }
        if (al_starts(id, "ev:")) { event_sheet(id + 3); return; }
        if (al_starts(id, "pl:") || al_starts(id, "sq:")) { load_points(); place_sheet_of(id); return; }
        if (al_starts(id, "log:")) {
            al_cpy(g_sel, id + 4, sizeof g_sel);
            push_entry();
            screen_open("Entry", g_entry_title[0] ? g_entry_title : "Check-in");
        }
        return;
    }
    if (al_eq(cmd, "fit_map")) {
        /* Show all: every check-in, station and event, and where you are. */
        g_map_fitted = 1;
        g_fit_here = 1;            /* the person chose this view */
        push_map();
        if (g_fit_all.on) fit_box(&g_fit_all);
        else toast("info", "Nothing on the map yet.");
        return;
    }
    if (al_eq(cmd, "save_area")) {
        /* The area on screen, from here to three levels closer: a town at
         * street level. The host caps and paces it. */
        if (g_view_zoom < 10) {
            /* At a country's or the world's scale this would be a bulk
             * download of coarse tiles, not a town to walk around in. */
            toast("info", "Zoom in closer first.");
            return;
        }
        int z = g_view_zoom;
        char m[120] = "{\"type\":\"ui.map.prefetch\",\"minZoom\":";
        al_cat_u(m, (unsigned long long)z, sizeof m);
        al_cat(m, ",\"maxZoom\":", sizeof m);
        al_cat_u(m, (unsigned long long)(z + 3 > 17 ? 17 : z + 3), sizeof m);
        al_cat(m, ",\"max\":1500}", sizeof m);
        say(m);
        toast("info", "Saving this area...");
        return;
    }
    if (al_eq(cmd, "save_intro") || al_eq(cmd, "save_greeting") || al_eq(cmd, "save_texts")) {
        /* Sent by each text field as it is typed (its `apply`), so saved
         * quietly: no Save button, no toast per pause. */
        if (!al_eq(cmd, "save_intro")) {
            al_json(g_buf, "greeting", g_greeting, sizeof g_greeting);
            hal_kv_set("greet", 5, g_greeting, al_len(g_greeting));
        }
        if (!al_eq(cmd, "save_greeting")) {
            char was[sizeof g_intro];
            al_cpy(was, g_intro, sizeof was);
            al_json(g_buf, "intro", g_intro, sizeof g_intro);
            hal_kv_set("intro", 5, g_intro, al_len(g_intro));
            /* The new line goes out with the point at the next wake, not once
             * per pause in the typing. */
            if (g_bot_on && !al_eq(was, g_intro)) g_point_at = 0;
        }
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
                toast("warning", "Share your location first: tap Share my location.");
            }
        }
        kv_put_bool("bot", g_bot_on);
        bot_update();
        announce_point(!g_bot_on);
        if (g_bot_on) toast("success", "Check-ins on.");
        push_station();
        push_map();
        return;
    }
    if (al_eq(cmd, "save_type")) {
        char v[24];
        al_json(g_buf, "place_type", v, sizeof v);
        al_cpy(g_ptype, ci_place(v) ? v : "", sizeof g_ptype);
        hal_kv_set("ptype", 5, g_ptype, al_len(g_ptype));
        if (g_bot_on) announce_point(0);     /* visitors learn it at once */
        toast("info", g_ptype[0] ? "Saved." : "No kind of place.");
        return;
    }
    if (al_eq(cmd, "set_local")) {
        char v[8];
        al_json(g_buf, "local_only", v, sizeof v);
        g_local_only = al_eq(v, "true");
        kv_put_bool("local", g_local_only);
        toast("info", g_local_only ? "In person only." : "LoRa range too.");
        return;
    }
    if (al_eq(cmd, "pick_badge")) { g_picking_event = 0; pick_picture("Choose badge"); return; }
    if (al_eq(cmd, "new_event")) {
        g_draft.badge[0] = 0;
        field_set_str("ev_pic", "");
        screen_open("New event", "New event");
        return;
    }
    if (al_eq(cmd, "pick_event_badge")) { g_picking_event = 1; pick_picture("Choose event badge"); return; }
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
        else if (al_eq(g_topic, "xprs.event")) {
            /* Kept by the core already: only the screen is behind. */
            (void)g_explore_at;
        }
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
    /* Check-in events from anyone, heard here or replayed by the archivers:
     * the core keeps them and fetches what is new; this wapp reads them. */
    hal_xprs_follow_kind("event", 5, 1);
    sub("xprs.event", 1);
    load_settings();
    pend_load();
    listen_update();
    bot_update();
    spots_backfill();
    spots_types();
    visits_sample();
    push_all();
    return 0;
}


int32_t module_tick(void)
{
    drain_events();
    point_due();
    visits_sample();
    /* A station that left Bluetooth reach ten minutes ago leaves the list,
     * though nothing new was heard. */
    if (g_page_seen) push_nearby();
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

/* Three minutes: who the core heard over Bluetooth is read for the time in
 * range (the end of a visit is the core's exact time, its start at most
 * this late), and the check-in point is renewed hourly. Nothing else here
 * runs on a clock: everything else happens because something arrived. */
int32_t module_tick_interval_ms(void) { return 180000; }

void module_destroy(void) {}
