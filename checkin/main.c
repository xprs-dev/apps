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
    if (call && call[0]) {
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

static void push_nearby(void)
{
    int32_t n = hal_xprs_stations(g_big, sizeof g_big - 1);
    rows_begin("nearby");
    int any = 0;
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
            section_begin("In reach now", &any);
            while (it && (it = al_json_next(it, item, sizeof item)) != 0) {
                char id[16], kind[12], sub_[48], tag[32] = "", tag2[32] = "";
                al_json(item, "id", id, sizeof id);
                al_json(item, "kind", kind, sizeof kind);
                al_json(item, "subtitle", sub_, sizeof sub_);
                if (!id[0] || al_eq(kind, "device") || al_eq(id, g_me)) continue;
                int had = log_count_for(id);
                if (had) {
                    al_cpy(tag, "checked in ", sizeof tag);
                    al_cat_u(tag, (unsigned long long)had, sizeof tag);
                    al_cat(tag, had == 1 ? " time" : " times", sizeof tag);
                }
                if (known_station(id)) al_cpy(tag2, "gives check-ins", sizeof tag2);
                row(id, id, sub_, tag, tag2, "", id, "checkin", "Check in");
            }
            section_end();
        }
    }
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
    if (n < 0) { notify("warning", "Check-in", "No callsign yet: set up your profile first."); return; }
    for (int i = 0; i < PEND_MAX; i++)
        if (g_pend[i].id[0] && al_eq(g_pend[i].station, station)) {
            notify("info", "Check-in", "Already asking that station; wait for its answer.");
            return;
        }
    int slot = -1;
    for (int i = 0; i < PEND_MAX; i++) if (!g_pend[i].id[0]) { slot = i; break; }
    if (slot < 0) { notify("warning", "Check-in", "Too many asks out; wait for an answer."); return; }
    int rc = hal_xprs_send(wire, (uint32_t)n);
    if (rc != 0) { notify("warning", "Check-in", "This phone could not send the ask."); return; }
    /* The identifier of the wire we built is the one the signed copy has:
     * a signature changes neither f:, ts: nor the payload (XPRS.md 5). */
    al_id(wire, (unsigned)n, g_pend[slot].id);
    al_cpy(g_pend[slot].station, station, sizeof g_pend[slot].station);
    al_cpy(g_pend[slot].wire, wire, sizeof g_pend[slot].wire);
    g_pend[slot].ts = now;
    p_begin(); p_str(g_pend[slot].id); p_str(station); p_int(now); p_str(wire); p_end();
    db_exec("INSERT OR REPLACE INTO asks (id, station, ts, wire) VALUES (?,?,?,?)", 1);
    listen_update();
    char b[96] = "Asking ";
    al_cat(b, station, sizeof b);
    al_cat(b, " for a check-in...", sizeof b);
    notify("info", "Check-in", b);
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
    if (c.code == CI_OK) {
        char ask[CI_WIRE_MAX + 64];
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
        al_cpy(body, "Checked in at ", sizeof body);
        al_cat(body, station, sizeof body);
        if (c.title[0]) { al_cat(body, " (", sizeof body); al_cat(body, c.title, sizeof body); al_cat(body, ")", sizeof body); }
        notify("success", "Check-in", body);
    } else {
        al_cpy(body, station, sizeof body);
        al_cat(body, c.code == CI_AGAIN_LATER ? ": you already checked in here today."
                   : c.code == CI_STALE ? ": the ask arrived too late. Is this phone's clock right?"
                   : c.code == CI_REFUSED ? ": refused. It may not know your key yet (try again in a minute), or it takes check-ins in person only."
                   : ": no check-in.", sizeof body);
        notify("warning", "Check-in", body);
    }
    pend_drop(i);
    listen_update();
    push_log();
    push_badges();
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
    notify("warning", "Check-in", body);
    pend_drop(i);
    listen_update();
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

static void my_pos(char *out, unsigned cap)
{
    out[0] = 0;
    int32_t lat = hal_sensor_gps_lat(), lon = hal_sensor_gps_lon();
    if (lat == (int32_t)0x80000000 || lon == (int32_t)0x80000000) return;
    deg(out, cap, lat);
    al_cat(out, ",", cap);
    deg(out, cap, lon);
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
    char title[32], ebadge[80], pos[40];
    active_event(title, sizeof title, ebadge, sizeof ebadge);
    my_pos(pos, sizeof pos);
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
    notify("info", "Check-in", body);
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
    al_cpy(g_out, "{\"type\":\"ui.stats.set\",\"field\":\"lb_stats\",\"tiles\":[", sizeof g_out);
    int first = 1;
    tile(&first, "total", "Check-ins", total, "every one, all time");
    tile(&first, "stations", "Stations", stations, "different stations");
    tile(&first, "local", "In person", local, "over Bluetooth, WiFi or LAN");
    tile(&first, "events", "Events", events, "check-ins at an event");
    tile(&first, "badges", "Badges", badges, "different badges");
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
            row(id, title[0] ? title : st, sub_, al_eq(grade, "local") ? "in person" : grade,
                title[0] ? st : w, tok, st, "", "");
        }
        section_end();
    }
    rows_end();
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
        al_jesc(g_out, al_eq(v, "local") && al_eq(rows_[i].col, "grade") ? "in person" : v, sizeof g_out);
        al_cat(g_out, "\"}", sizeof g_out);
    }
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
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
            row(id, title, m[0] ? m : from, tag, until_tag, tok, from, "", "");
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
            row(title, title, ws, until <= now_s() ? "ended" : "", until_tag, badge, "", "", "");
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
    const char *pairs[] = {
        "Giving check-ins", g_bot_on ? "yes" : "no",
        "Badge", g_badge[0] ? "chosen" : "none yet",
        "Visitors", visitors,
        "Events", events,
    };
    details("st_info", pairs, 4);
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
        notify("warning", "Check-in", "Could not read that picture.");
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
    push_station();
    notify("success", "Check-in", "Badge saved. Visitors get it with their next check-in.");
}

static void publish_event(void)
{
    char title[64], desc[160], len[8], label[32], wire[CI_WIRE_MAX + 1], pos[40];
    al_json(g_buf, "ev_title", title, sizeof title);
    al_json(g_buf, "ev_desc", desc, sizeof desc);
    if (!al_json(g_buf, "ev_len", len, sizeof len) || !len[0]) al_cpy(len, "day", sizeof len);
    ci_label(label, sizeof label, title);
    if (!label[0]) { notify("warning", "Check-in", "Give the event a name."); return; }
    unsigned long long now = now_s();
    unsigned long long span = al_eq(len, "month") ? 30ULL * 86400 : al_eq(len, "week") ? 7ULL * 86400 : 86400ULL;
    who_am_i();
    my_pos(pos, sizeof pos);
    int n = ci_build_event(wire, sizeof wire, g_me, now, pos, label, now, now + span,
                           g_draft.badge, desc);
    if (n < 0) { notify("warning", "Check-in", "That does not fit in one packet: shorten the name."); return; }
    if (hal_xprs_send(wire, (uint32_t)n) != 0) { notify("warning", "Check-in", "This phone could not announce it."); return; }
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
    notify("success", "Check-in", "Event announced. Check-ins during it carry its name and badge.");
}

/* ── the page ─────────────────────────────────────────────────────────── */
static void push_all(void)
{
    push_nearby();
    push_log();
    push_badges();
    push_events();
    push_station();
}

static void on_command(void)
{
    char cmd[48] = "", id[128] = "", type[24] = "";
    al_json(g_buf, "type", type, sizeof type);
    if (al_eq(type, "file.open")) { on_picked(); return; }
    if (!al_json(g_buf, "command", cmd, sizeof cmd) &&
        !al_json(g_buf, "action", cmd, sizeof cmd)) return;
    if (al_eq(cmd, "ready") || al_eq(cmd, "refresh")) { who_am_i(); push_all(); return; }
    if (al_eq(cmd, "nearby_refresh")) { push_nearby(); return; }
    if (al_eq(cmd, "nearby_checkin") || al_eq(cmd, "nearby_tap")) {
        if (al_json(g_buf, "nearby_id", id, sizeof id) && id[0]) do_checkin(id);
        return;
    }
    if (al_eq(cmd, "set_chase")) {
        char v[8];
        al_json(g_buf, "chase_regional", v, sizeof v);
        g_chase_regional = al_eq(v, "true");
        kv_put_bool("chase_reg", g_chase_regional);
        return;
    }
    if (al_eq(cmd, "log_tap")) {
        if (!al_json(g_buf, "log_id", id, sizeof id)) return;
        al_cpy(g_sel, id, sizeof g_sel);
        push_entry();
        screen_open("Entry", "Check-in");
        return;
    }
    if (al_eq(cmd, "events_refresh")) { push_events(); return; }
    if (al_eq(cmd, "set_bot")) {
        char v[8];
        al_json(g_buf, "bot_on", v, sizeof v);
        g_bot_on = al_eq(v, "true");
        kv_put_bool("bot", g_bot_on);
        bot_update();
        push_station();
        return;
    }
    if (al_eq(cmd, "set_local")) {
        char v[8];
        al_json(g_buf, "local_only", v, sizeof v);
        g_local_only = al_eq(v, "true");
        kv_put_bool("local", g_local_only);
        return;
    }
    if (al_eq(cmd, "save_greeting")) {
        al_json(g_buf, "greeting", g_greeting, sizeof g_greeting);
        hal_kv_set("greet", 5, g_greeting, al_len(g_greeting));
        notify("success", "Check-in", g_greeting[0] ? "Greeting saved." : "No greeting: visitors get the certificate only.");
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

/* No clock: everything here happens because something arrived. */
int32_t module_tick_interval_ms(void) { return 0; }

void module_destroy(void) {}
