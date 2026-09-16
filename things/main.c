/*
 * things -- the devices around you and the ones you follow (XPRS.md 11.7.1).
 *
 * An X4 callsign is equipment: a pump, a generator, a temperature sensor,
 * a smart plug. It has no radio. A controller holds its key and airs its
 * `t:identity` and `t:observation` for it, and deposits copies with the
 * archivers it chose (12.3). This wapp lists them and shows what they say:
 *
 *   Pinned          the ones this phone follows; kept, and fetched from the
 *                   chosen archivers while out of earshot (12.12.2)
 *   Nearby          devices the core hears now
 *   In the archive  devices whose identity this station holds
 *
 * Tap one for its readings, as sent (15.8: the unit is part of the value),
 * with the energy of 15.5.2 split by source when a site sends it that way.
 *
 * The wapp asks the core and draws. It never learns which lane carried a
 * reading or which archiver held it: pinning is hal_xprs_follow, and keeping
 * the device's packets and fetching them while it is away are the core's
 * (docs/architecture.md, "A device is followed by callsign"). It sends
 * nothing on the air.
 *
 * Opening the page asks the core to have what is in local reach say who it
 * is now (hal_xprs_discover), and Scan asks again: a device is otherwise
 * found only when its controller next airs for it. Where the core looks is
 * its own business; the answers are packets like any other.
 *
 * It exists only while its page is open: it subscribes to core.monitor and
 * core.archive then, redraws when they say something moved, and at most
 * once every two seconds however busy the room is (performance.md 8.16: a
 * wapp's cost is what it does per event).
 */
#include "../hal/xprs_wasm_hal.h"
#include "wire.h"

#define TH_MAX      48
#define CALL_MAX    16
#define REDRAW_MS   2000ULL
#define NICK_EVERY_MS 300000ULL   /* a name nobody sent is asked for again, rarely */

typedef struct {
    char call[CALL_MAX];
    char nick[24];
    int  nick_tried;
    unsigned long long nick_ms;
    /* What the archive holds, merged newest first, per key (see merge). */
    char rd[512];                 /* "k:v k:v", as sent */
    char src[160];                /* production per source: "solar:3200W wind:1400W" */
    char rd_sig[12];
    unsigned long long rd_ts;     /* epoch of the newest observation; 0 none */
    unsigned rd_gen;              /* g_arch_gen when read */
} th_t;

static th_t g_th[TH_MAX];
static int  g_nth;
static char g_sel[CALL_MAX];
static char g_pinned[TH_MAX][CALL_MAX];
static int  g_npinned;
static unsigned g_arch_gen = 1;
static int  g_dirty;
static unsigned long long g_drawn_ms;
static int  g_drawn;
static unsigned long long g_asked_ms;   /* when the core last agreed to ask */
static int  g_asked;                    /* 1 asked, 0 not asked, -1 never tried */

static char g_ev[1024];
static char g_topic[64];
static char g_buf[2048];
static char g_out[16384];
static char g_big[65536];         /* hal_xprs_stations, hal_xprs_history */
static char g_host[2048];         /* one hal_xprs_station answer */
static char g_row[1200];
static char g_wire[300];

static void say(const char *json) { hal_msg_send(json, th_len(json)); }

static void note(const char *text)
{
    char l[160] = "things: ";
    th_cat(l, text, sizeof l);
    hal_log(1, l, th_len(l));
}

/* ── What a reading is called (XPRS.md 11.7, 15.3, 15.5, 15.5.1, 15.5.2) ─ */
static const char *const NOW[][2] = {
    {"state", "State"}, {"level", "Level"}, {"target", "Setpoint"},
    {"temp", "Temperature"}, {"hum", "Humidity"},
    {"intemp", "Indoors"}, {"inhum", "Indoor humidity"},
    {"press", "Pressure"}, {"wind", "Wind"}, {"wdir", "Wind from"},
    {"gust", "Gust"}, {"rain1", "Rain, hour"}, {"rain24", "Rain, 24 h"},
    {"solar", "Sunlight"},
    {"produces", "Producing"}, {"consumes", "Consuming"}, {"grid", "Grid"},
    {"storage", "Storage"}, {"charged", "Stored"}, {"load", "Load"},
    {"source", "Source"},
    {"volt", "Supply"}, {"batt", "Battery"},
    {"dose", "Dose rate"}, {"radon", "Radon"}, {"rf", "RF field"},
    {"efield", "Electric field"}, {"mfield", "Magnetic field"},
};
#define NNOW ((int)(sizeof NOW / sizeof NOW[0]))

static const char *const TOTALS[][2] = {
    {"lifeproduces", "Produced"}, {"lifeconsumes", "Consumed"},
    {"lifegridin", "From the grid"}, {"lifegridout", "To the grid"},
    {"lifeload", "Load, total"}, {"lifedose", "Dose, total"},
    {"odometer", "Odometer"}, {"uptime", "Up"}, {"lifetime", "In service"},
    {"fw", "Firmware"},
};
#define NTOTALS ((int)(sizeof TOTALS / sizeof TOTALS[0]))

/* What the one-line summary under a name says, in this order. */
static const char *const SUMMARY[] = {
    "state", "level", "temp", "hum", "produces", "load", "consumes",
    "volt", "batt", "charged", "dose",
};
#define NSUMMARY ((int)(sizeof SUMMARY / sizeof SUMMARY[0]))

/* ── The things, and what is remembered about each ───────────────────── */
static th_t *find(const char *call)
{
    for (int i = 0; i < g_nth; i++) if (th_eq(g_th[i].call, call)) return &g_th[i];
    return 0;
}

static int pinned(const char *call)
{
    for (int i = 0; i < g_npinned; i++) if (th_eq(g_pinned[i], call)) return 1;
    return 0;
}

static th_t *get(const char *call)
{
    th_t *t = find(call);
    if (t) return t;
    int i;
    if (g_nth < TH_MAX) {
        i = g_nth++;
    } else {
        /* Forget the first one that is neither pinned nor open. */
        for (i = 0; i < g_nth; i++)
            if (!pinned(g_th[i].call) && !th_eq(g_th[i].call, g_sel)) break;
        if (i == g_nth) return 0;
    }
    t = &g_th[i];
    for (unsigned k = 0; k < sizeof *t; k++) ((char *)t)[k] = 0;
    th_cpy(t->call, call, sizeof t->call);
    return t;
}

/* The stations this phone follows by callsign: the core's list. */
static void load_pinned(void)
{
    g_npinned = 0;
    int n = hal_xprs_followed(g_big, sizeof g_big - 1);
    if (n <= 0) return;
    g_big[n] = 0;
    char c[CALL_MAX];
    for (const char *p = th_json_top(g_big); p && g_npinned < TH_MAX; ) {
        p = th_json_next_str(p, c, sizeof c);
        if (!c[0]) break;
        th_cpy(g_pinned[g_npinned++], c, CALL_MAX);
    }
}

/* ── What the core holds (hal_xprs_station, hal_xprs_history) ────────── */
/* Fills g_host; 1 when the core has heard it within the hour. */
static int host(const char *call)
{
    int n = hal_xprs_station(call, th_len(call), g_host, sizeof g_host - 1);
    if (n <= 0) { g_host[0] = 0; return 0; }
    g_host[n] = 0;
    return 1;
}

/* Heard in the last eleven minutes: the core's "in earshot" (a station heard
 * earlier this hour comes back without a packet count). */
static int host_fresh(void) { char v[12]; return th_json(g_host, "packets", v, sizeof v); }

/* The archive's rows for one query, into g_big. 0 when none. */
static int history(const char *query)
{
    int n = hal_xprs_history(query, th_len(query), g_big, sizeof g_big - 1);
    if (n <= 0) { g_big[0] = 0; return 0; }
    g_big[n] = 0;
    return n;
}

static void query_from(char *q, unsigned cap, const char *call, const char *type, int limit)
{
    th_cpy(q, "{\"from\":\"", cap);
    th_cat(q, call, cap);
    th_cat(q, "\",\"types\":[\"", cap);
    th_cat(q, type, cap);
    th_cat(q, "\"],\"limit\":", cap);
    th_cat_u(q, (unsigned long long)limit, cap);
    th_cat(q, "}", cap);
}

/* Its name, from its own t:identity, looked up once and then rarely: a name
 * nobody sent is a miss worth remembering (performance.md 3.2). */
static void nick_of(th_t *t)
{
    unsigned long long now = hal_time_ms();
    if (t->nick_tried && (t->nick[0] || now - t->nick_ms < NICK_EVERY_MS)) return;
    t->nick_tried = 1;
    t->nick_ms = now;
    char q[96];
    query_from(q, sizeof q, t->call, "identity", 2);
    if (!history(q)) return;
    for (const char *p = th_json_top(g_big); p; ) {
        p = th_json_next(p, g_row, sizeof g_row);
        if (!g_row[0]) break;
        if (th_json(g_row, "wire", g_wire, sizeof g_wire) &&
            th_field(g_wire, "nick", t->nick, sizeof t->nick)) return;
    }
}

/* Is [key] one this wapp shows? */
static int known_key(const char *key)
{
    for (int i = 0; i < NNOW; i++) if (th_eq(NOW[i][0], key)) return 1;
    for (int i = 0; i < NTOTALS; i++) if (th_eq(TOTALS[i][0], key)) return 1;
    return 0;
}

/* Fold the rows in g_big (newest first) into [t]: the newest value of each
 * key. XPRS.md 15.5.2: a site with several sources sends a `source:mixed`
 * total, which is the packet that balances, and one packet per source; a
 * packet naming one source among several is that source's share and not the
 * total. A site with one source sends everything in one packet. */
static void merge(th_t *t)
{
    t->rd[0] = t->src[0] = t->rd_sig[0] = 0;
    t->rd_ts = 0;
    int mixed = 0;
    char v[40], s[16];
    for (const char *p = th_json_top(g_big); p; ) {
        p = th_json_next(p, g_row, sizeof g_row);
        if (!g_row[0]) break;
        if (th_json(g_row, "wire", g_wire, sizeof g_wire) &&
            th_field(g_wire, "source", s, sizeof s) && th_eq(s, "mixed")) mixed = 1;
    }
    for (const char *p = th_json_top(g_big); p; ) {
        p = th_json_next(p, g_row, sizeof g_row);
        if (!g_row[0]) break;
        if (!th_json(g_row, "wire", g_wire, sizeof g_wire)) continue;
        if (!t->rd_ts) {
            th_json(g_row, "ts", v, sizeof v);
            t->rd_ts = th_num(v);
            th_json(g_row, "sig", t->rd_sig, sizeof t->rd_sig);
        }
        if (mixed && th_field(g_wire, "source", s, sizeof s) && !th_eq(s, "mixed")) {
            char had[24];
            if (th_field(g_wire, "produces", v, sizeof v) &&
                !th_field(t->src, s, had, sizeof had))
                th_put(t->src, s, v, sizeof t->src);
            continue;
        }
        /* Every key of the packet, in the order it came, first seen wins. */
        for (const char *f = g_wire; *f; ) {
            while (*f == ' ') f++;
            if (!*f || th_starts(f, "m:")) break;
            char key[20];
            unsigned k = 0;
            while (f[k] && f[k] != ':' && f[k] != ' ' && k < sizeof key - 1) { key[k] = f[k]; k++; }
            key[k] = 0;
            while (*f && *f != ' ') f++;
            char had[40];
            if (!known_key(key) || th_field(t->rd, key, had, sizeof had)) continue;
            if (th_field(g_wire, key, v, sizeof v)) th_put(t->rd, key, v, sizeof t->rd);
        }
    }
    t->rd_gen = g_arch_gen;
}

/* What the archive holds about [t], read again only after the archive has
 * changed (core.archive), never per draw. */
static void arch_read(th_t *t)
{
    if (t->rd_gen == g_arch_gen) return;
    char q[96];
    query_from(q, sizeof q, t->call, "observation", 12);
    if (!history(q)) {
        t->rd[0] = t->src[0] = t->rd_sig[0] = 0;
        t->rd_ts = 0;
        t->rd_gen = g_arch_gen;
        return;
    }
    merge(t);
}

/* The readings to show for [t], as a "k:v k:v" list: what the archive holds
 * when it is pinned (complete, and split by source), else the latest the core
 * heard nearby, else whatever the archive happens to hold. g_host must hold
 * the station's answer when [have]. Returns 1 when they came from the air. */
static int readings(th_t *t, int have, char *out, unsigned cap)
{
    out[0] = 0;
    if (pinned(t->call)) {
        arch_read(t);
        if (t->rd[0]) { th_cpy(out, t->rd, cap); return 0; }
    }
    if (have) {
        char obj[1024];
        if (th_json_obj(g_host, "readings", obj, sizeof obj)) {
            char v[40];
            for (int i = 0; i < NNOW; i++)
                if (th_json(obj, NOW[i][0], v, sizeof v)) th_put(out, NOW[i][0], v, cap);
            for (int i = 0; i < NTOTALS; i++)
                if (th_json(obj, TOTALS[i][0], v, sizeof v)) th_put(out, TOTALS[i][0], v, cap);
        }
        static const char *const self[] = { "uptime", "lifetime", "fw" };
        for (int i = 0; i < 3; i++) {
            char v[24];
            if (th_json(g_host, self[i], v, sizeof v)) th_put(out, self[i], v, cap);
        }
        if (out[0]) return 1;
    }
    arch_read(t);
    th_cpy(out, t->rd, cap);
    return 0;
}

/* "on, 23.8V, 64%" -- the first three that it has. Production says where
 * it comes from: "1900W solar". */
static void summary(const char *rd, char *out, unsigned cap)
{
    out[0] = 0;
    int n = 0;
    for (int i = 0; i < NSUMMARY && n < 3; i++) {
        char v[40];
        if (!th_field(rd, SUMMARY[i], v, sizeof v)) continue;
        if (n++) th_cat(out, ", ", cap);
        th_cat(out, v, cap);
        char s[16];
        if (th_eq(SUMMARY[i], "produces") && th_field(rd, "source", s, sizeof s)) {
            th_cat(out, " ", cap);
            th_cat(out, s, cap);
        }
    }
}

static void ago_words(unsigned long long ms, char *out, unsigned cap)
{
    out[0] = 0;
    if (ms < 60000ULL)            { th_cat_u(out, ms / 1000, cap); th_cat(out, " s ago", cap); }
    else if (ms < 3600000ULL)     { th_cat_u(out, ms / 60000, cap); th_cat(out, " min ago", cap); }
    else if (ms < 172800000ULL)   { th_cat_u(out, ms / 3600000, cap); th_cat(out, " h ago", cap); }
    else                          { th_cat_u(out, ms / 86400000ULL, cap); th_cat(out, " days ago", cap); }
}

static unsigned long long reading_age_ms(const th_t *t)
{
    unsigned long long now = hal_time_epoch();
    if (!t->rd_ts || now < t->rd_ts) return 0;
    return (now - t->rd_ts) * 1000ULL;
}

/* "LAN" / "BLE -70 dBm", from the core's last sighting. */
static void bearer_words(char *out, unsigned cap)
{
    char b[12] = "", r[12] = "";
    th_json(g_host, "bearer", b, sizeof b);
    out[0] = 0;
    for (unsigned j = 0; b[j]; j++) { char c[2] = { th_up(b[j]), 0 }; th_cat(out, c, cap); }
    if (th_eq(out, "BLE5")) th_cpy(out, "BLE", cap);
    if (th_json(g_host, "rssi", r, sizeof r) && !th_eq(r, "0")) {
        th_cat(out, " ", cap);
        th_cat(out, r, cap);
        th_cat(out, " dBm", cap);
    }
}

/* ── The list ─────────────────────────────────────────────────────────── */
static void tag(char *tags, unsigned cap, const char *v)
{
    if (!v || !v[0]) return;
    if (tags[0]) th_cat(tags, ",", cap);
    th_cat(tags, "\"", cap);
    th_jesc(tags, v, cap);
    th_cat(tags, "\"", cap);
}

static void item(th_t *t, int *first)
{
    int have = host(t->call);
    int fresh = have && host_fresh();
    nick_of(t);
    char rd[512], sum[96], sub[160] = "", tags[160] = "", w[48];
    readings(t, have, rd, sizeof rd);
    summary(rd, sum, sizeof sum);
    if (t->nick[0]) th_cpy(sub, t->call, sizeof sub);
    if (sum[0]) { if (sub[0]) th_cat(sub, ", ", sizeof sub); th_cat(sub, sum, sizeof sub); }
    if (!sub[0]) th_cpy(sub, "Nothing reported yet", sizeof sub);
    if (have) {
        char v[24];
        th_json(g_host, "agoMs", v, sizeof v);
        ago_words(th_num(v), w, sizeof w);
        tag(tags, sizeof tags, w);
        bearer_words(w, sizeof w);
        tag(tags, sizeof tags, w);
    } else if (t->rd_ts) {
        char a[32];
        ago_words(reading_age_ms(t), a, sizeof a);
        th_cpy(w, "read ", sizeof w);
        th_cat(w, a, sizeof w);
        tag(tags, sizeof tags, w);
    }
    if (!*first) th_cat(g_out, ",", sizeof g_out);
    *first = 0;
    th_cat(g_out, "{\"id\":\"", sizeof g_out);
    th_jesc(g_out, t->call, sizeof g_out);
    th_cat(g_out, "\",\"title\":\"", sizeof g_out);
    th_jesc(g_out, t->nick[0] ? t->nick : t->call, sizeof g_out);
    th_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    th_jesc(g_out, sub, sizeof g_out);
    th_cat(g_out, "\",\"tags\":[", sizeof g_out);
    th_cat(g_out, tags, sizeof g_out);
    th_cat(g_out, "]", sizeof g_out);
    if (!fresh) th_cat(g_out, ",\"dim\":true", sizeof g_out);
    th_cat(g_out, "}", sizeof g_out);
}

static int listed(char list[][CALL_MAX], int n, const char *call)
{
    for (int i = 0; i < n; i++) if (th_eq(list[i], call)) return 1;
    return 0;
}

static void section(const char *title, char list[][CALL_MAX], int n, int *any)
{
    if (!n) return;
    if (*any) th_cat(g_out, ",", sizeof g_out);
    *any = 1;
    th_cat(g_out, "{\"title\":\"", sizeof g_out);
    th_jesc(g_out, title, sizeof g_out);
    th_cat(g_out, "\",\"items\":[", sizeof g_out);
    int first = 1;
    for (int i = 0; i < n; i++) {
        th_t *t = get(list[i]);
        if (t) item(t, &first);
    }
    th_cat(g_out, "]}", sizeof g_out);
}

/* The devices whose identity the archive holds: asked again only after the
 * archive changed (core.archive), never because the room did. */
static char g_known[TH_MAX][CALL_MAX];
static int  g_nknown;
static unsigned g_known_gen;

static void load_known(void)
{
    if (g_known_gen == g_arch_gen) return;
    g_known_gen = g_arch_gen;
    g_nknown = 0;
    if (!history("{\"kind\":\"device\",\"types\":[\"identity\"],\"limit\":50}")) return;
    char from[CALL_MAX], nick[24];
    for (const char *p = th_json_top(g_big); p && g_nknown < TH_MAX; ) {
        p = th_json_next(p, g_row, sizeof g_row);
        if (!g_row[0]) break;
        if (!th_json(g_row, "from", from, sizeof from)) continue;
        int dup = 0;
        for (int i = 0; i < g_nknown; i++) if (th_eq(g_known[i], from)) dup = 1;
        if (dup) continue;
        th_cpy(g_known[g_nknown++], from, CALL_MAX);
        /* The row is its identity: the name comes free. */
        th_t *t = get(from);
        if (t && !t->nick[0] && th_json(g_row, "wire", g_wire, sizeof g_wire) &&
            th_field(g_wire, "nick", nick, sizeof nick)) {
            th_cpy(t->nick, nick, sizeof t->nick);
            t->nick_tried = 1;
            t->nick_ms = hal_time_ms();
        }
    }
}

static void draw_list(void)
{
    static char nearby[TH_MAX][CALL_MAX], known[TH_MAX][CALL_MAX];
    int nnearby = 0, nknown = 0;
    load_pinned();

    /* Nearby: every station the core lists that the core says is a device. */
    int n = hal_xprs_stations(g_big, sizeof g_big - 1);
    if (n > 0) {
        g_big[n] = 0;
        char id[CALL_MAX], kind[12];
        for (const char *p = g_big; (p = th_json_arr(p, "items")); ) {
            const char *q = p, *last = p;
            while ((q = th_json_next(q, g_row, sizeof g_row))) {
                last = q;
                if (!th_json(g_row, "kind", kind, sizeof kind) || !th_eq(kind, "device")) continue;
                if (!th_json(g_row, "id", id, sizeof id)) continue;
                if (pinned(id) || listed(nearby, nnearby, id) || nnearby >= TH_MAX) continue;
                th_cpy(nearby[nnearby++], id, CALL_MAX);
            }
            p = last;
        }
    } else if (n < 0) {
        note("the station list did not fit; Nearby left out");
    }

    /* In the archive: devices whose identity this station holds. */
    load_known();
    for (int i = 0; i < g_nknown && nknown < TH_MAX; i++) {
        if (pinned(g_known[i]) || listed(nearby, nnearby, g_known[i])) continue;
        th_cpy(known[nknown++], g_known[i], CALL_MAX);
    }

    th_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"things\",\"sections\":[", sizeof g_out);
    int any = 0;
    section("Pinned", g_pinned, g_npinned, &any);
    section("Nearby", nearby, nnearby, &any);
    section("In the archive", known, nknown, &any);
    th_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* ── One thing ────────────────────────────────────────────────────────── */
static void tile(const char *id, const char *label, const char *text)
{
    /* "23.8V" reads as 23.8 and V: the number is the tile, the unit its
     * caption. A word (state:on) is the tile whole. */
    char num[24] = "", unit[16] = "";
    unsigned i = 0;
    if (text[0] == '-' || (text[0] >= '0' && text[0] <= '9')) {
        while (text[i] && (text[i] == '-' || text[i] == '.' || (text[i] >= '0' && text[i] <= '9')) &&
               i < sizeof num - 1) { num[i] = text[i]; i++; }
        num[i] = 0;
        th_cpy(unit, text + i, sizeof unit);
    } else {
        th_cpy(num, text, sizeof num);
    }
    unsigned n = th_len(g_out);
    if (g_out[n - 1] != '[') th_cat(g_out, ",", sizeof g_out);
    th_cat(g_out, "{\"id\":\"", sizeof g_out);
    th_jesc(g_out, id, sizeof g_out);
    th_cat(g_out, "\",\"label\":\"", sizeof g_out);
    th_jesc(g_out, label, sizeof g_out);
    th_cat(g_out, "\",\"value\":\"", sizeof g_out);
    th_jesc(g_out, num, sizeof g_out);
    th_cat(g_out, "\"", sizeof g_out);
    if (unit[0]) {
        th_cat(g_out, ",\"unit\":\"", sizeof g_out);
        th_jesc(g_out, unit, sizeof g_out);
        th_cat(g_out, "\"", sizeof g_out);
    }
    th_cat(g_out, "}", sizeof g_out);
}

static void details_begin(const char *field)
{
    th_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof g_out);
    th_cat(g_out, field, sizeof g_out);
    th_cat(g_out, "\",\"value\":[{\"title\":\"\",\"items\":[", sizeof g_out);
}

static void detail(const char *label, const char *value)
{
    if (!value || !value[0]) return;
    unsigned n = th_len(g_out);
    if (g_out[n - 1] != '[') th_cat(g_out, ",", sizeof g_out);
    th_cat(g_out, "{\"label\":\"", sizeof g_out);
    th_jesc(g_out, label, sizeof g_out);
    th_cat(g_out, "\",\"value\":\"", sizeof g_out);
    th_jesc(g_out, value, sizeof g_out);
    th_cat(g_out, "\"}", sizeof g_out);
}

static void details_end(void)
{
    unsigned n = th_len(g_out);
    if (g_out[n - 1] == '[') {
        /* Nothing to say: an empty value, so the field shows nothing. */
        char *v = g_out;
        for (unsigned i = 0; i + 8 < n; i++)
            if (th_starts(v + i, "\"value\":[")) { v[i + 9] = 0; break; }
        th_cat(g_out, "]}", sizeof g_out);
    } else {
        th_cat(g_out, "]}]}", sizeof g_out);
    }
    say(g_out);
}

static void flag_hidden(const char *name, int hidden)
{
    char m[120] = "{\"type\":\"ui.field.set\",\"field\":\"";
    th_cat(m, name, sizeof m);
    th_cat(m, hidden ? "__hidden\",\"value\":true}" : "__hidden\",\"value\":false}", sizeof m);
    say(m);
}

static void push_detail(void)
{
    if (!g_sel[0]) return;
    th_t *t = get(g_sel);
    if (!t) return;
    int have = host(t->call);
    int fresh = have && host_fresh();
    nick_of(t);
    char rd[512];
    int from_air = readings(t, have, rd, sizeof rd);

    th_cpy(g_out, "{\"type\":\"ui.stats.set\",\"field\":\"th_now\",\"tiles\":[", sizeof g_out);
    int tiles = 0;
    char v[40];
    for (int i = 0; i < NNOW; i++) {
        if (!th_field(rd, NOW[i][0], v, sizeof v)) continue;
        tile(NOW[i][0], NOW[i][1], v);
        tiles++;
    }
    /* Each source's share, when the site sends the total and the shares. */
    if (!from_air) {
        for (const char *f = t->src; *f; ) {
            while (*f == ' ') f++;
            char s[16];
            unsigned k = 0;
            while (f[k] && f[k] != ':' && k < sizeof s - 1) { s[k] = f[k]; k++; }
            s[k] = 0;
            while (*f && *f != ' ') f++;
            if (!s[0] || !th_field(t->src, s, v, sizeof v)) continue;
            char id[24] = "src_", label[24] = "From ";
            th_cat(id, s, sizeof id);
            th_cat(label, s, sizeof label);
            tile(id, label, v);
            tiles++;
        }
    }
    if (!tiles) tile("none", "Readings", "none yet");
    th_cat(g_out, "]}", sizeof g_out);
    say(g_out);

    details_begin("th_about");
    detail("Callsign", t->call);
    detail("Name", t->nick);
    char heard[80] = "", w[48];
    if (have) {
        th_json(g_host, "agoMs", v, sizeof v);
        ago_words(th_num(v), heard, sizeof heard);
        bearer_words(w, sizeof w);
        if (w[0]) { th_cat(heard, ", ", sizeof heard); th_cat(heard, w, sizeof heard); }
        if (!fresh) th_cat(heard, " (out of range now)", sizeof heard);
    } else {
        th_cpy(heard, "Not in range", sizeof heard);
    }
    detail("Heard", heard);
    if (rd[0]) {
        char when[80] = "";
        if (from_air) th_cpy(when, "Heard nearby", sizeof when);
        else if (t->rd_ts) {
            ago_words(reading_age_ms(t), when, sizeof when);
            th_cat(when, ", from the archive", sizeof when);
        }
        detail("Readings", when);
    }
    char sig[16] = "";
    if (from_air) th_json(g_host, "sig", sig, sizeof sig);
    else th_cpy(sig, t->rd_sig, sizeof sig);
    detail("Signature", th_eq(sig, "verified") ? "Signed, verified"
                      : th_eq(sig, "forged") ? "FORGED: somebody else signed as it"
                      : th_eq(sig, "unverified") ? "Signed, key not known yet"
                      : th_eq(sig, "unsigned") ? "Unsigned: a claim, weigh it" : "");
    detail("Pinned", pinned(t->call) ? "Yes: kept, and fetched while away" : "No");
    details_end();

    details_begin("th_totals");
    for (int i = 0; i < NTOTALS; i++)
        if (th_field(rd, TOTALS[i][0], v, sizeof v)) detail(TOTALS[i][1], v);
    details_end();

    flag_hidden("pin", pinned(t->call));
    flag_hidden("unpin", !pinned(t->call));
}

static void screen_open(const char *name, const char *title)
{
    char m[160] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    th_cat(m, name, sizeof m);
    th_cat(m, "\",\"title\":\"", sizeof m);
    th_jesc(m, title, sizeof m);
    th_cat(m, "\"}", sizeof m);
    say(m);
}

/* ── Asking what is around ────────────────────────────────────────────── */
static void push_scan(void)
{
    char line[120] = "";
    if (g_asked > 0) {
        char ago[32];
        ago_words(hal_time_ms() - g_asked_ms, ago, sizeof ago);
        th_cpy(line, "Asked the local network who is there, ", sizeof line);
        th_cat(line, ago, sizeof line);
        th_cat(line, ". Devices appear as they answer.", sizeof line);
    } else if (g_asked == 0) {
        th_cpy(line, "Not asked now: no local network, or asked in the last half minute.", sizeof line);
    }
    details_begin("th_scan");
    detail("Scan", line);
    details_end();
}

static void discover(void)
{
    if (hal_xprs_discover() == 1) {
        g_asked = 1;
        g_asked_ms = hal_time_ms();
        note("asked the core to look for devices nearby");
    } else if (g_asked != 1 || hal_time_ms() - g_asked_ms > 30000ULL) {
        g_asked = 0;
    }
}

/* ── When to draw ─────────────────────────────────────────────────────── */
static void draw(int force)
{
    unsigned long long now = hal_time_ms();
    if (!force && !g_dirty) return;
    if (!force && g_drawn && now - g_drawn_ms < REDRAW_MS) return;  /* the tick finishes it */
    g_dirty = 0;
    g_drawn = 1;
    g_drawn_ms = now;
    draw_list();
    push_scan();
    push_detail();
}

static void drain_events(void)
{
    while (hal_event_available()) {
        uint32_t n = hal_event_recv(g_topic, sizeof g_topic - 1, g_ev, sizeof g_ev - 1);
        g_ev[n] = 0;
        g_topic[sizeof g_topic - 1] = 0;
        if (th_eq(g_topic, "core.archive")) { g_arch_gen++; g_dirty = 1; }
        else if (th_eq(g_topic, "core.monitor")) g_dirty = 1;
    }
}

/* ── What the person does ─────────────────────────────────────────────── */
static void on_command(void)
{
    char cmd[40] = "";
    if (!th_json(g_buf, "command", cmd, sizeof cmd)) return;
    if (th_eq(cmd, "refresh") || th_eq(cmd, "ready")) {
        g_arch_gen++;
        draw(1);
    } else if (th_eq(cmd, "scan")) {
        discover();
        draw(1);
    } else if (th_eq(cmd, "things_tap")) {
        char id[CALL_MAX];
        if (!th_json(g_buf, "things_id", id, sizeof id) || !id[0]) return;
        th_cpy(g_sel, id, sizeof g_sel);
        push_detail();
        th_t *t = find(g_sel);
        screen_open("Thing", t && t->nick[0] ? t->nick : g_sel);
    } else if (th_eq(cmd, "pin") || th_eq(cmd, "unpin")) {
        if (!g_sel[0]) return;
        int on = th_eq(cmd, "pin");
        if (hal_xprs_follow(g_sel, th_len(g_sel), on) != 0) {
            note("the core would not follow that callsign");
            return;
        }
        char l[64];
        th_cpy(l, on ? "pinned " : "unpinned ", sizeof l);
        th_cat(l, g_sel, sizeof l);
        note(l);
        g_arch_gen++;      /* what the archive keeps for it just changed */
        draw(1);
    } else if (th_eq(cmd, "back")) {
        g_sel[0] = 0;
        say("{\"type\":\"ui.screen.close\"}");
    }
}

/* ── Entry points ─────────────────────────────────────────────────────── */
int32_t module_init(void)
{
    /* Nobody looking, nothing to do: the core keeps and fetches what is
     * pinned whether or not this wapp runs. */
    if (!hal_ui_attached()) return 0;
    static const char mon[] = "core.monitor", arc[] = "core.archive";
    hal_event_subscribe(mon, sizeof mon - 1);
    hal_event_subscribe(arc, sizeof arc - 1);
    g_asked = -1;
    discover();
    draw(1);
    return 0;
}

int32_t module_tick(void)
{
    drain_events();
    draw(0);
    return 0;
}

int32_t module_handle_event(void)
{
    drain_events();
    uint32_t n = hal_msg_recv(g_buf, sizeof g_buf - 1);
    if (n > 0) {
        g_buf[n] = 0;
        on_command();
    }
    draw(0);
    return 0;
}

/* The page's clock, only to finish a redraw the two-second throttle held
 * back; the engine exists only while the page is open. */
int32_t module_tick_interval_ms(void) { return (int32_t)REDRAW_MS; }

void module_destroy(void) {}
