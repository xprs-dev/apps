/*
 * wapp_stations.c -- see wapp_stations.h.
 *
 * Lifted from Check-in 0.6 ("Stations everywhere", ci_pos_square) the second
 * time a wapp needed it (Mesh's map), with what the portal's map has besides:
 * service tags, stations with no place, the heard ones, search and filters.
 */
#include "../hal/xprs_wasm_hal.h"
#include "util.h"
#include "wapp_stations.h"

/* ── pure helpers ─────────────────────────────────────────────────────── */

/* One decimal number with at most [maxint] integer digits; its value into
 * [v] times 1e7 (truncated past seven decimals). */
static int dec7(const char **pp, long *v, int maxint)
{
    const char *p = *pp;
    int neg = 0, nint = 0, nfrac = 0;
    long iv = 0, fv = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { if (++nint > maxint) return 0; iv = iv * 10 + (*p++ - '0'); }
    if (!nint) return 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (nfrac < 7) { fv = fv * 10 + (*p - '0'); nfrac++; }
            p++;
        }
        if (!nfrac) return 0;
    }
    while (nfrac < 7) { fv *= 10; nfrac++; }
    *v = (iv * 10000000L + fv) * (neg ? -1 : 1);
    *pp = p;
    return 1;
}

int wst_pos_parse(const char *pos, long *lat7, long *lon7)
{
    const char *p = pos;
    long la, lo;
    if (!p || !*p || !dec7(&p, &la, 2) || *p != ',') return 0;
    p++;
    if (!dec7(&p, &lo, 3) || *p) return 0;
    if (la < -900000000L || la > 900000000L || lo < -1800000000L || lo > 1800000000L) return 0;
    if (lat7) *lat7 = la;
    if (lon7) *lon7 = lo;
    return 1;
}

int wst_pos_ok(const char *pos) { return wst_pos_parse(pos, 0, 0); }

/* cos(x) for |x| <= pi/2 (a latitude), by its series: no libm in a wapp. */
static double cos_lat(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 + x2 * x2 / 24 - x2 * x2 * x2 / 720 + x2 * x2 * x2 * x2 / 40320;
}

long wst_dist_m(const char *a, const char *b)
{
    long la1, lo1, la2, lo2;
    if (!wst_pos_parse(a, &la1, &lo1) || !wst_pos_parse(b, &la2, &lo2)) return -1;
    const double rad = 3.14159265358979 / 180.0 / 1e7;
    double dlat = (double)(la2 - la1) * rad;
    long dl = lo2 - lo1;
    if (dl > 1800000000L) dl -= 3600000000L;
    if (dl < -1800000000L) dl += 3600000000L;
    double dlon = (double)dl * rad * cos_lat((double)(la1 + la2) / 2 * rad);
    double d2 = dlat * dlat + dlon * dlon, r = d2;
    if (d2 <= 0) return 0;
    for (int i = 0; i < 30; i++) r = (r + d2 / r) / 2;   /* sqrt, Newton */
    return (long)(r * 6371000.0 + 0.5);
}

void wst_dist_text(long m, char *out, unsigned cap)
{
    out[0] = 0;
    if (m < 0) return;
    if (m < 1000) {
        al_cat_u(out, (unsigned long long)((m + 5) / 10 * 10), cap);
        al_cat(out, " m", cap);
    } else if (m < 10000) {
        long d = (m + 50) / 100;
        al_cat_u(out, (unsigned long long)(d / 10), cap);
        al_cat(out, ".", cap);
        al_cat_u(out, (unsigned long long)(d % 10), cap);
        al_cat(out, " km", cap);
    } else {
        al_cat_u(out, (unsigned long long)((m + 500) / 1000), cap);
        al_cat(out, " km", cap);
    }
}

/* Degrees times 1e7 as text with four decimals (eleven metres). */
static void deg4(char *d, unsigned cap, long v)
{
    if (v < 0) { al_cat(d, "-", cap); v = -v; }
    al_cat_u(d, (unsigned long long)(v / 10000000L), cap);
    al_cat(d, ".", cap);
    long f = (v % 10000000L) / 1000L;
    char t[5] = { (char)('0' + f / 1000), (char)('0' + f / 100 % 10),
                  (char)('0' + f / 10 % 10), (char)('0' + f % 10), 0 };
    al_cat(d, t, cap);
}

int wst_pos_norm(const char *in, char *out, unsigned cap)
{
    long la, lo;
    out[0] = 0;
    if (!wst_pos_parse(in, &la, &lo)) return 0;
    deg4(out, cap, la);
    al_cat(out, ",", cap);
    deg4(out, cap, lo);
    return 1;
}

/* One coordinate of a pos: value with its redaction bars (U+2588, UTF-8
 * E2 96 88) dropped: the readable digits into [shown], and whether any bar
 * was there. 0 when a byte other than a digit, '-', '.' or a bar appears. */
static int unbar(const char *s, unsigned n, char *shown, unsigned cap, int *barred)
{
    unsigned k = 0;
    *barred = 0;
    for (unsigned i = 0; i < n; i++) {
        if ((unsigned char)s[i] == 0xE2 && i + 2 < n &&
            (unsigned char)s[i + 1] == 0x96 && (unsigned char)s[i + 2] == 0x88) {
            *barred = 1;
            i += 2;
            continue;
        }
        char c = s[i];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return 0;
        if (k + 1 >= cap) return 0;
        shown[k++] = c;
    }
    shown[k] = 0;
    return k > 0;
}

/* The middle of the square [shown] names when its last digits are hidden,
 * times 1e7: 38 is anywhere from 38 to 39, so 38.5; -9 is -9.5. [half7]
 * gets the square's half-width. Unchanged when nothing was hidden. */
static int coord(const char *s, unsigned n, int maxint, long *v7, long *half7)
{
    char shown[24];
    int barred;
    if (!unbar(s, n, shown, sizeof shown, &barred)) return 0;
    const char *p = shown;
    if (!dec7(&p, v7, maxint) || *p) return 0;
    if (!barred) return 1;
    int dec = 0;
    for (const char *q = shown; *q; q++) if (*q == '.') { dec = (int)al_len(q + 1); break; }
    long unit = 10000000L;
    for (int i = 0; i < dec && unit > 1; i++) unit /= 10;
    long h = unit / 2;
    *v7 += shown[0] == '-' ? -h : h;
    if (h > *half7) *half7 = h;
    return 1;
}

int wst_pos_square(const char *pos, char *mid, unsigned cap, long *half_m)
{
    mid[0] = 0;
    *half_m = 0;
    if (!pos) return 0;
    const char *comma = pos;
    while (*comma && *comma != ',') comma++;
    if (*comma != ',') return 0;
    long la, lo, h7 = 0;
    if (!coord(pos, (unsigned)(comma - pos), 2, &la, &h7) ||
        !coord(comma + 1, al_len(comma + 1), 3, &lo, &h7)) return 0;
    if (la < -900000000L || la > 900000000L || lo < -1800000000L || lo > 1800000000L) return 0;
    deg4(mid, cap, la);
    al_cat(mid, ",", cap);
    deg4(mid, cap, lo);
    /* A degree of latitude is 111.32 km. */
    *half_m = (long)((long long)h7 * 111320LL / 10000000LL);
    return 1;
}

int wst_xprs_call(const char *call)
{
    if (!call || call[0] != 'X' || call[1] < '1' || call[1] > '5') return 0;
    int n = 0;
    const char *p = call + 2;
    for (; *p && *p != '-'; p++, n++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return 0;
    return n >= 2 && n <= 6;
}

void wst_base_call(char *out, unsigned cap, const char *call)
{
    unsigned k = 0;
    while (call[k] && call[k] != '-' && k + 1 < cap) { out[k] = call[k]; k++; }
    out[k] = 0;
}

void wst_ago(char *out, unsigned cap, unsigned long long ts, unsigned long long now)
{
    unsigned long long s = now > ts ? now - ts : 0;
    out[0] = 0;
    if (s < 120) { al_cpy(out, "just now", cap); return; }
    al_cat_u(out, s < 7200 ? s / 60 : s < 172800 ? s / 3600 : s / 86400, cap);
    al_cat(out, s < 7200 ? " min ago" : s < 172800 ? " h ago" : " d ago", cap);
}

/* The portal's MAP_TAG_NAMES, plus how a station is heard. */
static const char *const TAG_NAMES[][2] = {
    {"serve:archive", "Archivers"}, {"serve:relay", "Relays"}, {"serve:internet", "Internet"},
    {"serve:aprs", "APRS"}, {"serve:files", "Files"}, {"serve:meshtastic", "Meshtastic"},
    {"source:solar", "Solar"}, {"source:battery", "Battery"}, {"source:mains", "Mains power"},
    {"reach:local", "Nearby"}, {"reach:regional", "Regional"}, {"reach:global", "Global"},
    {"placed", "On the map"}, {"key", "Can be messaged"},
    {"kind:people", "People"}, {"kind:other", "Other stations"},
};

void wst_tag_name(const char *tag, char *out, unsigned cap)
{
    for (unsigned i = 0; i < sizeof TAG_NAMES / sizeof TAG_NAMES[0]; i++)
        if (al_eq(tag, TAG_NAMES[i][0])) { al_cpy(out, TAG_NAMES[i][1], cap); return; }
    const char *v = tag;
    for (const char *p = tag; *p; p++) if (*p == ':') { v = p + 1; break; }
    al_cpy(out, v, cap);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

/* ── the database ─────────────────────────────────────────────────────── */
static int32_t g_db = -1;
static wst_opts_t g_o;
static char g_me[16];
static char g_par[1024];
static char g_one[4096];          /* one small answer */
static char g_obj[2048];          /* one row of an answer */

static unsigned long long now_s(void) { return hal_time_epoch(); }

static void p_begin(void) { al_cpy(g_par, "[", sizeof g_par); }
static void p_sep(void) { if (al_len(g_par) > 1) al_cat(g_par, ",", sizeof g_par); }
static void p_str(const char *s)
{
    p_sep();
    al_cat(g_par, "\"", sizeof g_par);
    al_jesc(g_par, s, sizeof g_par);
    al_cat(g_par, "\"", sizeof g_par);
}
static void p_int(unsigned long long v) { p_sep(); al_cat_u(g_par, v, sizeof g_par); }
static void p_end(void) { al_cat(g_par, "]", sizeof g_par); }

static int exec_(const char *sql, int params)
{
    if (g_db < 0) return -1;
    return hal_db_exec(g_db, sql, al_len(sql), params ? g_par : 0, params ? al_len(g_par) : 0);
}

static int query_(const char *sql, int params, char *out, unsigned cap)
{
    if (g_db < 0) { out[0] = 0; return 0; }
    int32_t n = hal_db_query(g_db, sql, al_len(sql), params ? g_par : 0, params ? al_len(g_par) : 0,
                             out, cap - 1);
    if (n <= 0) { out[0] = 0; return n < 0 ? n : 0; }
    out[n] = 0;
    return n;
}

/* The first element of a JSON array answer. */
static const char *rows_of(const char *a)
{
    while (*a && *a != '[') a++;
    return *a == '[' ? a + 1 : a;
}

static unsigned long long u_of(const char *s)
{
    unsigned long long v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (unsigned)(*s - '0');
    return v;
}

static unsigned long long json_u(const char *row, const char *key)
{
    char v[24];
    return al_json(row, key, v, sizeof v) ? u_of(v) : 0;
}

static int one_int(const char *sql, int params)
{
    if (query_(sql, params, g_one, sizeof g_one) <= 0) return 0;
    if (!al_json_next(rows_of(g_one), g_obj, sizeof g_obj)) return 0;
    return (int)json_u(g_obj, "n");
}

static void kv_put_u(const char *k, unsigned long long v)
{
    char t[24] = "";
    al_cat_u(t, v, sizeof t);
    hal_kv_set(k, al_len(k), t, al_len(t));
}

static unsigned long long kv_u(const char *k)
{
    char v[24];
    uint32_t n = hal_kv_get(k, al_len(k), v, sizeof v - 1);
    v[n < sizeof v ? n : 0] = 0;
    return u_of(v);
}

static void say(const char *m) { hal_msg_send(m, al_len(m)); }

/* Recent enough to be on the map. */
static unsigned long long recent_floor(void)
{
    unsigned long long n = now_s(), r = g_o.recent_s ? g_o.recent_s : 7ULL * 86400ULL;
    return n > r ? n - r : 0;
}

/* Heard now: a reach is a word about the last quarter of an hour. */
#define WST_HEARD_S 900ULL
static unsigned long long heard_floor(void)
{
    unsigned long long n = now_s();
    return n > WST_HEARD_S ? n - WST_HEARD_S : 0;
}

void wst_init(int32_t db, const wst_opts_t *o)
{
    g_db = db;
    if (o) g_o = *o;
    if (!g_o.color) g_o.color = "#90A4AE";
    /* Check-in 0.6 made this table with its first six columns; what the
     * list and the filters need came after, added once to a store that has
     * the old shape (rows written before an ADD COLUMN read as empty). */
    exec_("CREATE TABLE IF NOT EXISTS places (call TEXT PRIMARY KEY, nick TEXT,"
          " pos TEXT, half INTEGER, pts INTEGER, seen INTEGER)", 0);
    if (query_("SELECT last FROM places LIMIT 1", 0, g_one, sizeof g_one) < 0) {
        exec_("ALTER TABLE places ADD COLUMN serve TEXT", 0);
        exec_("ALTER TABLE places ADD COLUMN src TEXT", 0);
        exec_("ALTER TABLE places ADD COLUMN sts INTEGER", 0);
        exec_("ALTER TABLE places ADD COLUMN key TEXT", 0);
        exec_("ALTER TABLE places ADD COLUMN kts INTEGER", 0);
        exec_("ALTER TABLE places ADD COLUMN reach TEXT", 0);
        exec_("ALTER TABLE places ADD COLUMN hts INTEGER", 0);
        exec_("ALTER TABLE places ADD COLUMN last INTEGER", 0);
        exec_("UPDATE places SET serve = '', src = '', sts = 0, key = '', kts = 0,"
              " reach = '', hts = 0, last = seen", 0);
    }
}

void wst_me(const char *call) { wst_base_call(g_me, sizeof g_me, call ? call : ""); }

void wst_follow(void)
{
    hal_xprs_follow_kind("identity", 8, 1);
    hal_xprs_follow_kind("service", 7, 1);
}

/* ── one station into the copy ────────────────────────────────────────── */
typedef struct {
    char nick[48], pos[40], serve[96], src[24], key[80], reach[12];
    long half;
    unsigned long long pts, seen, sts, kts, hts;
    int had;
} st_row_t;

static void row_read(const char *call, st_row_t *r)
{
    for (unsigned i = 0; i < sizeof *r; i++) ((char *)r)[i] = 0;
    p_begin(); p_str(call); p_end();
    if (query_("SELECT nick, pos, half, pts, seen, serve, src, sts, key, kts, reach, hts"
               " FROM places WHERE call = ?", 1, g_one, sizeof g_one) <= 0) return;
    if (!al_json_next(rows_of(g_one), g_obj, sizeof g_obj)) return;
    r->had = 1;
    al_json(g_obj, "nick", r->nick, sizeof r->nick);
    al_json(g_obj, "pos", r->pos, sizeof r->pos);
    al_json(g_obj, "serve", r->serve, sizeof r->serve);
    al_json(g_obj, "src", r->src, sizeof r->src);
    al_json(g_obj, "key", r->key, sizeof r->key);
    al_json(g_obj, "reach", r->reach, sizeof r->reach);
    r->half = (long)json_u(g_obj, "half");
    r->pts = json_u(g_obj, "pts");
    r->seen = json_u(g_obj, "seen");
    r->sts = json_u(g_obj, "sts");
    r->kts = json_u(g_obj, "kts");
    r->hts = json_u(g_obj, "hts");
}

static void row_write(const char *call, const st_row_t *r)
{
    unsigned long long last = r->seen > r->hts ? r->seen : r->hts;
    p_begin(); p_str(call); p_str(r->nick); p_str(r->pos); p_int((unsigned long long)r->half);
    p_int(r->pts); p_int(r->seen); p_str(r->serve); p_str(r->src); p_int(r->sts);
    p_str(r->key); p_int(r->kts); p_str(r->reach); p_int(r->hts); p_int(last); p_end();
    exec_("INSERT OR REPLACE INTO places (call, nick, pos, half, pts, seen, serve, src, sts,"
          " key, kts, reach, hts, last) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", 1);
}

static int same(const st_row_t *a, const st_row_t *b)
{
    return al_eq(a->nick, b->nick) && al_eq(a->pos, b->pos) && a->half == b->half &&
           a->pts == b->pts && a->seen == b->seen && al_eq(a->serve, b->serve) &&
           al_eq(a->src, b->src) && a->sts == b->sts && al_eq(a->key, b->key) &&
           a->kts == b->kts && al_eq(a->reach, b->reach) && a->hts == b->hts;
}

static unsigned long long g_top;   /* the newest row's ts seen by a walk */

int wst_take(const char *row)
{
    static char wire[1200];
    char type[12], f[16], call[16], ts[24], pos[80], nick[48], sig[12], mid[40] = "";
    if (!al_json(row, "wire", wire, sizeof wire)) return 0;
    if (al_json(row, "sig", sig, sizeof sig) && al_eq(sig, "forged")) return 0;
    if (!al_starts(wire, "t:")) return 0;
    {
        unsigned k = 0;
        const char *p = wire + 2;
        while (*p && *p != ' ' && k + 1 < sizeof type) type[k++] = *p++;
        type[k] = 0;
    }
    int is_id = al_eq(type, "identity"), is_svc = al_eq(type, "service");
    if (!is_id && !is_svc) return 0;
    /* Addressed to one station (d:), not announced: not what it is. */
    {
        char d[16];
        if (al_field(wire, "d", d, sizeof d) && d[0] && !al_eq(d, "-")) return 0;
    }
    al_field(wire, "f", f, sizeof f);
    wst_base_call(call, sizeof call, f);
    if (!wst_xprs_call(call) || (g_me[0] && al_eq(call, g_me))) return 0;
    al_field(wire, "ts", ts, sizeof ts);
    unsigned long long t = al_epoch(ts);
    if (!t) return 0;
    if (t > g_top) g_top = t;

    int has_pos = al_field(wire, "pos", pos, sizeof pos) && pos[0];
    long half = 0;
    if (has_pos && !al_eq(pos, "-") && !wst_pos_square(pos, mid, sizeof mid, &half)) has_pos = 0;
    if (has_pos && al_eq(pos, "-")) mid[0] = 0;

    st_row_t o, r;
    row_read(call, &o);
    /* A station never placed is not kept, unless the wapp lists those too. */
    if (!o.had && !g_o.keep_unplaced && (!has_pos || !mid[0])) return 0;
    r = o;
    if (has_pos && t >= o.pts) { al_cpy(r.pos, mid, sizeof r.pos); r.half = half; r.pts = t; }
    if (is_id) {
        int has_nick = al_field(wire, "nick", nick, sizeof nick) && nick[0] && !al_eq(nick, "-");
        if (has_nick && (t >= o.seen || !o.nick[0])) al_cpy(r.nick, nick, sizeof r.nick);
        /* Its key, from an identity whose signature was checked: who can be
         * messaged (sealed to it). */
        char k[80];
        if (al_eq(sig, "verified") && al_field(wire, "k", k, sizeof k) && k[0] && t >= o.kts) {
            al_cpy(r.key, k, sizeof r.key);
            r.kts = t;
        }
    }
    if (is_svc && t >= o.sts) {
        char sv[90], src[24];
        r.serve[0] = 0;
        if (al_field(wire, "serve", sv, sizeof sv) && sv[0] && !al_eq(sv, "-")) {
            al_cpy(r.serve, ",", sizeof r.serve);
            al_cat(r.serve, sv, sizeof r.serve);
            al_cat(r.serve, ",", sizeof r.serve);
        }
        r.src[0] = 0;
        if (al_field(wire, "source", src, sizeof src) && !al_eq(src, "-")) al_cpy(r.src, src, sizeof r.src);
        r.sts = t;
    }
    if (t > r.seen) r.seen = t;
    if (o.had && same(&o, &r)) return 0;
    row_write(call, &r);
    return 1;
}

void wst_heard(const char *call_, const char *reach, unsigned long long ts)
{
    char call[16];
    wst_base_call(call, sizeof call, call_);
    if (!wst_xprs_call(call) || (g_me[0] && al_eq(call, g_me))) return;
    st_row_t o, r;
    row_read(call, &o);
    r = o;
    al_cpy(r.reach, reach ? reach : "", sizeof r.reach);
    if (ts > r.hts) r.hts = ts;
    if (o.had && same(&o, &r)) return;
    row_write(call, &r);
}

void wst_named(const char *call_, const char *nick, unsigned long long ts)
{
    char call[16];
    wst_base_call(call, sizeof call, call_);
    if (!wst_xprs_call(call) || (g_me[0] && al_eq(call, g_me))) return;
    st_row_t o, r;
    row_read(call, &o);
    r = o;
    if (nick && nick[0] && !al_eq(nick, "-") && !o.nick[0]) al_cpy(r.nick, nick, sizeof r.nick);
    if (ts > r.seen) r.seen = ts;
    if (o.had && same(&o, &r)) return;
    row_write(call, &r);
}

/* ── the walk of the archive ──────────────────────────────────────────── */
/* Newest first, a few pages per step, down to a floor: a whole walk every
 * few hours (one an archiver replayed late may be older than the newest
 * held), else from an hour before the newest held. */
#define W_PAGE       100
#define W_STEP_PAGES 2
#define W_EVERY_S    120ULL
#define W_FULL_S     (6ULL * 3600ULL)
#define W_MARGIN_S   3600ULL

static char g_page[65536];
static struct {
    int on, full, changed, limit;
    unsigned long long until, floor;
} g_w;

int wst_busy(void) { return g_w.on; }
unsigned long long wst_updated(void) { return kv_u("pl_at"); }

void wst_begin(int now_)
{
    if (g_w.on) return;
    unsigned long long n = now_s(), at = kv_u("pl_at"), cur = kv_u("pl_cur");
    if (!now_ && at && n < at + W_EVERY_S) return;
    g_w.on = 1;
    g_w.until = 0;
    g_top = cur;
    g_w.changed = 0;
    g_w.limit = W_PAGE;
    g_w.full = !cur || n >= kv_u("pl_full") + W_FULL_S;
    g_w.floor = g_w.full || cur < W_MARGIN_S ? 0 : cur - W_MARGIN_S;
}

static void walk_done(void)
{
    g_w.on = 0;
    unsigned long long n = now_s();
    kv_put_u("pl_cur", g_top);
    if (g_w.full) kv_put_u("pl_full", n);
    kv_put_u("pl_at", n);
    if (g_o.done) g_o.done(g_w.changed);
}

void wst_step(void)
{
    for (int pg = 0; g_w.on && pg < W_STEP_PAGES; pg++) {
        /* Public words only ("to":[""]): a directed t:service (a claim to
         * the names keeper, d:X3KEEP) says nothing of what a station
         * serves, and being the newest it would wipe its serve:archive. */
        char q[240] = "{\"types\":[\"identity\",\"service\"],\"to\":[\"\"],\"limit\":", st[24];
        al_cat_u(q, (unsigned long long)g_w.limit, sizeof q);
        if (g_w.floor) {
            al_stamp(st, sizeof st, g_w.floor);
            al_cat(q, ",\"since\":\"", sizeof q); al_cat(q, st, sizeof q); al_cat(q, "\"", sizeof q);
        }
        if (g_w.until) {
            al_stamp(st, sizeof st, g_w.until);
            al_cat(q, ",\"until\":\"", sizeof q); al_cat(q, st, sizeof q); al_cat(q, "\"", sizeof q);
        }
        al_cat(q, "}", sizeof q);
        int32_t n = hal_xprs_history(q, al_len(q), g_page, sizeof g_page - 1);
        if (n < 0 && g_w.limit > 10) { g_w.limit /= 2; continue; }   /* rows too long: smaller pages */
        if (n <= 0) { walk_done(); return; }
        g_page[n] = 0;
        const char *p = rows_of(g_page);
        static char obj[2048];
        int rows = 0;
        unsigned long long oldest = 0;
        exec_("BEGIN", 0);
        while ((p = al_json_next(p, obj, sizeof obj)) != 0) {
            char w[1200], ts[24];
            rows++;
            if (al_json(obj, "wire", w, sizeof w) && al_field(w, "ts", ts, sizeof ts)) {
                unsigned long long t = al_epoch(ts);
                if (t && (!oldest || t < oldest)) oldest = t;
            }
            if (wst_take(obj)) g_w.changed = 1;
        }
        exec_("COMMIT", 0);
        if (rows < g_w.limit || !oldest || (g_w.floor && oldest <= g_w.floor)) { walk_done(); return; }
        /* The next page ends where this one did; a second full of one
         * second's rows steps past it rather than reading it again. */
        g_w.until = oldest == g_w.until ? oldest - 1 : oldest;
    }
}

/* ── the map ──────────────────────────────────────────────────────────── */
#define DRAW_MAX 1500
static char g_draw[196608];

static void marker(const char *id, const char *pos, const char *label, const char *detail,
                   const char *actions, const char *kind)
{
    char lat[24], lon[24], m[1400];
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
    al_cat(m, g_o.color, sizeof m);
    al_cat(m, "\",\"detail\":\"", sizeof m);
    al_jesc(m, detail, sizeof m);
    al_cat(m, "\"", sizeof m);
    if (kind && kind[0]) { al_cat(m, ",\"kind\":\"", sizeof m); al_cat(m, kind, sizeof m); al_cat(m, "\"", sizeof m); }
    if (actions && actions[0]) { al_cat(m, ",\"actions\":", sizeof m); al_cat(m, actions, sizeof m); }
    al_cat(m, "}", sizeof m);
    say(m);
}

static void where_of(const wst_filter_t *f, char *w, unsigned cap);

/* What a station is on the map, as the portal's filter has it: an archiver
 * (it keeps messages: serve:archive) whatever else it is, a person (an X1
 * callsign), or another station. */
static const char *kind_of(const char *call, const char *serve)
{
    for (const char *s = serve; *s; s++)
        if (al_starts(s, ",archive,")) return "archive";
    return al_starts(call, "X1") ? "person" : "station";
}

void wst_draw_f(const wst_filter_t *f)
{
    /* Every station that says where it is, one marker each, the freshest
     * DRAW_MAX: the host groups the near ones and fans out the ones on one
     * spot. The list's search and chips apply (not its area). */
    char sql[1100] = "SELECT call, nick, pos, half, last, key, serve, src FROM places", w[600];
    wst_filter_t g;
    if (f) { g = *f; g.area[0] = 0; }
    where_of(f ? &g : 0, w, sizeof w);
    p_int(recent_floor());
    p_end();
    al_cat(sql, w, sizeof sql);
    al_cat(sql, " AND pos != '' AND last >= ? ORDER BY last DESC LIMIT 1500", sizeof sql);
    if (query_(sql, 1, g_draw, sizeof g_draw) <= 0) return;
    const char *p = rows_of(g_draw);
    unsigned long long now = now_s();
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char call[16] = "", nick[48] = "", pos[40] = "", key[80] = "", serve[96] = "", tags[160] = "";
        al_json(g_obj, "call", call, sizeof call);
        al_json(g_obj, "nick", nick, sizeof nick);
        al_json(g_obj, "pos", pos, sizeof pos);
        al_json(g_obj, "key", key, sizeof key);
        al_json(g_obj, "serve", serve, sizeof serve);
        long half = (long)json_u(g_obj, "half");
        unsigned long long seen = json_u(g_obj, "last");
        if ((g_o.skip && g_o.skip(call)) || !wst_pos_ok(pos)) continue;
        const char *kind = kind_of(call, serve);
        /* The popup: name, what it serves and runs on, how near the point
         * is, how fresh. */
        char id[24] = "pl:", detail[400] = "", a[24], sq[24], acts[200] = "";
        al_cat(id, call, sizeof id);
        if (nick[0]) al_cpy(detail, nick, sizeof detail);
        wst_row_tags(g_obj, tags, sizeof tags);
        if (tags[0]) {
            if (detail[0]) al_cat(detail, "\n", sizeof detail);
            al_cat(detail, tags, sizeof detail);
        }
        if (detail[0]) al_cat(detail, "\n", sizeof detail);
        if (half) {
            wst_dist_text(half * 2, sq, sizeof sq);
            al_cat(detail, "Somewhere within ", sizeof detail);
            al_cat(detail, sq, sizeof detail);
            al_cat(detail, " \xC2\xB7 ", sizeof detail);
        }
        wst_ago(a, sizeof a, seen, now);
        al_cat(detail, a, sizeof detail);
        /* What it offers: an archiver is visited (its rooms, groups and
         * chat, as its visitor), any other station opened; Message when
         * it published a key to seal to. */
        if (g_o.actions) {
            al_cpy(acts, al_eq(kind, "archive") ? "[{\"id\":\"visit\",\"label\":\"Visit\"}"
                                                : "[{\"id\":\"open\",\"label\":\"Open\"}", sizeof acts);
            if (key[0]) al_cat(acts, ",{\"id\":\"message\",\"label\":\"Message\"}", sizeof acts);
            al_cat(acts, "]", sizeof acts);
        }
        marker(id, pos, call, detail, acts, kind);
        if (g_o.placed) g_o.placed(pos);
    }
}

void wst_draw(void) { wst_draw_f(0); }

int wst_count_placed(void)
{
    p_begin(); p_str(g_me); p_int(recent_floor()); p_end();
    return one_int("SELECT COUNT(*) AS n FROM places WHERE pos != '' AND call != ? AND last >= ?", 1);
}

/* "2026-11-25 14:05", in the device's zone. */
static void when(char *out, unsigned cap, unsigned long long epoch)
{
    char s[24];
    long long t = (long long)epoch + (long long)hal_time_utc_offset() * 60;
    al_stamp(s, sizeof s, t < 0 ? 0 : (unsigned long long)t);
    s[10] = ' ';
    s[16] = 0;
    al_cpy(out, s, cap);
}

void wst_status(void)
{
    char n[16] = "", m[300], up[48];
    al_cat_u(n, (unsigned long long)wst_count_placed(), sizeof n);
    unsigned long long at = kv_u("pl_at");
    if (at) { char w[24]; al_cpy(up, "Updated ", sizeof up); when(w, sizeof w, at); al_cat(up, w, sizeof up); }
    else al_cpy(up, "Looking for stations...", sizeof up);
    al_cpy(m, "{\"type\":\"ui.map.status\",\"items\":[{\"id\":\"pl_n\",\"label\":\"", sizeof m);
    al_cat(m, n, sizeof m);
    al_cat(m, al_eq(n, "1") ? " station" : " stations", sizeof m);
    al_cat(m, "\",\"on\":false},{\"id\":\"pl_at\",\"label\":\"", sizeof m);
    al_jesc(m, up, sizeof m);
    al_cat(m, "\",\"on\":false}]}", sizeof m);
    say(m);
}

/* ── the list ─────────────────────────────────────────────────────────── */
int wst_filter_has(const wst_filter_t *f, const char *tag)
{
    for (int i = 0; i < f->ntags; i++) if (al_eq(f->tags[i], tag)) return 1;
    return 0;
}

int wst_filter_tag(wst_filter_t *f, const char *tag)
{
    for (int i = 0; i < f->ntags; i++)
        if (al_eq(f->tags[i], tag)) {
            for (int j = i; j + 1 < f->ntags; j++) al_cpy(f->tags[j], f->tags[j + 1], sizeof f->tags[j]);
            f->ntags--;
            return 0;
        }
    if (!tag[0] || f->ntags >= (int)(sizeof f->tags / sizeof f->tags[0])) return 0;
    al_cpy(f->tags[f->ntags++], tag, sizeof f->tags[0]);
    return 1;
}

/* WHERE for [f], its parameters into g_par (begun, not ended). */
static void where_of(const wst_filter_t *f, char *w, unsigned cap)
{
    al_cpy(w, " WHERE call != ?", cap);
    p_begin(); p_str(g_me);
    if (f && f->q[0]) {
        char like[48] = "%", lo[40];
        unsigned i = 0;
        for (; f->q[i] && i + 1 < sizeof lo; i++)
            lo[i] = (f->q[i] >= 'A' && f->q[i] <= 'Z') ? (char)(f->q[i] + 32) : f->q[i];
        lo[i] = 0;
        al_cat(like, lo, sizeof like);
        al_cat(like, "%", sizeof like);
        al_cat(w, " AND (lower(call) LIKE ? OR lower(nick) LIKE ?)", cap);
        p_str(like); p_str(like);
    }
    if (f && f->area[0]) { al_cat(w, " AND pos = ?", cap); p_str(f->area); }
    for (int i = 0; f && i < f->ntags; i++) {
        const char *t = f->tags[i];
        if (al_eq(t, "placed")) al_cat(w, " AND pos != ''", cap);
        else if (al_eq(t, "key")) al_cat(w, " AND key != ''", cap);
        else if (al_starts(t, "serve:")) {
            char v[40] = "%,";
            al_cat(v, t + 6, sizeof v);
            al_cat(v, ",%", sizeof v);
            al_cat(w, " AND serve LIKE ?", cap);
            p_str(v);
        } else if (al_eq(t, "kind:people")) {
            al_cat(w, " AND call LIKE 'X1%' AND serve NOT LIKE '%,archive,%'", cap);
        } else if (al_eq(t, "kind:other")) {
            al_cat(w, " AND call NOT LIKE 'X1%' AND serve NOT LIKE '%,archive,%'", cap);
        } else if (al_starts(t, "source:")) { al_cat(w, " AND src = ?", cap); p_str(t + 7); }
        else if (al_starts(t, "reach:")) {
            al_cat(w, " AND reach = ? AND hts >= ?", cap);
            p_str(t + 6); p_int(heard_floor());
        }
    }
}

int wst_list(const wst_filter_t *f, int offset, int limit, char *out, unsigned cap)
{
    char sql[900] = "SELECT call, nick, pos, half, last, serve, src, key, reach, hts FROM places", w[600];
    where_of(f, w, sizeof w);
    p_int((unsigned long long)(limit > 0 ? limit : 50));
    p_int((unsigned long long)(offset > 0 ? offset : 0));
    p_end();
    al_cat(sql, w, sizeof sql);
    al_cat(sql, " ORDER BY last DESC, call LIMIT ? OFFSET ?", sizeof sql);
    int n = query_(sql, 1, out, cap);
    return n > 0 ? n : 0;
}

int wst_count(const wst_filter_t *f)
{
    char sql[800] = "SELECT COUNT(*) AS n FROM places", w[600];
    where_of(f, w, sizeof w);
    p_end();
    al_cat(sql, w, sizeof sql);
    return one_int(sql, 1);
}

int wst_total(void)
{
    p_begin(); p_str(g_me); p_end();
    return one_int("SELECT COUNT(*) AS n FROM places WHERE call != ?", 1);
}

int wst_row(const char *call, char *out, unsigned cap)
{
    char c[16];
    wst_base_call(c, sizeof c, call);
    p_begin(); p_str(c); p_end();
    int n = query_("SELECT call, nick, pos, half, last, serve, src, key, reach, hts FROM places"
                   " WHERE call = ?", 1, g_one, sizeof g_one);
    if (n <= 0 || !al_json_next(rows_of(g_one), out, cap)) { out[0] = 0; return 0; }
    return (int)al_len(out);
}

/* Tags with their counts. */
#define MAX_CHIPS 32
static struct { char id[28]; int n; } g_chip[MAX_CHIPS];
static int g_nchip;

static void chip_add(const char *id, int n)
{
    if (n <= 0 || !id[0]) return;
    for (int i = 0; i < g_nchip; i++) if (al_eq(g_chip[i].id, id)) { g_chip[i].n += n; return; }
    if (g_nchip >= MAX_CHIPS) return;
    al_cpy(g_chip[g_nchip].id, id, sizeof g_chip[0].id);
    g_chip[g_nchip++].n = n;
}

/* serve is ",archive,relay,": each word counts once per station. */
static void chips_of_group(const char *sql, const char *col, const char *prefix, int split)
{
    if (query_(sql, 1, g_draw, sizeof g_draw) <= 0) return;
    const char *p = rows_of(g_draw);
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char v[96], id[40];
        int n = (int)json_u(g_obj, "n");
        if (!al_json(g_obj, col, v, sizeof v) || !v[0]) continue;
        if (!split) { al_cpy(id, prefix, sizeof id); al_cat(id, v, sizeof id); chip_add(id, n); continue; }
        const char *s = v;
        while (*s) {
            char word[28];
            unsigned k = 0;
            while (*s == ',') s++;
            while (*s && *s != ',' && k + 1 < sizeof word) word[k++] = *s++;
            word[k] = 0;
            if (!k) continue;
            al_cpy(id, prefix, sizeof id);
            al_cat(id, word, sizeof id);
            chip_add(id, n);
        }
    }
}

int wst_chips(const wst_filter_t *f, char *out, unsigned cap)
{
    g_nchip = 0;
    p_begin(); p_str(g_me); p_int(heard_floor()); p_end();
    chips_of_group("SELECT reach, COUNT(*) AS n FROM places WHERE call != ? AND reach != ''"
                   " AND hts >= ? GROUP BY reach", "reach", "reach:", 0);
    p_begin(); p_str(g_me); p_end();
    chips_of_group("SELECT serve, COUNT(*) AS n FROM places WHERE call != ? AND serve != ''"
                   " GROUP BY serve", "serve", "serve:", 1);
    p_begin(); p_str(g_me); p_end();
    chips_of_group("SELECT src, COUNT(*) AS n FROM places WHERE call != ? AND src != ''"
                   " GROUP BY src", "src", "source:", 0);
    p_begin(); p_str(g_me); p_end();
    chip_add("kind:people", one_int("SELECT COUNT(*) AS n FROM places WHERE call != ? AND call LIKE 'X1%'"
                                    " AND serve NOT LIKE '%,archive,%'", 1));
    p_begin(); p_str(g_me); p_end();
    chip_add("kind:other", one_int("SELECT COUNT(*) AS n FROM places WHERE call != ? AND call NOT LIKE 'X1%'"
                                   " AND serve NOT LIKE '%,archive,%'", 1));
    p_begin(); p_str(g_me); p_end();
    chip_add("placed", one_int("SELECT COUNT(*) AS n FROM places WHERE call != ? AND pos != ''", 1));
    p_begin(); p_str(g_me); p_end();
    chip_add("key", one_int("SELECT COUNT(*) AS n FROM places WHERE call != ? AND key != ''", 1));
    /* A chip that is on stays shown, even at none. */
    for (int i = 0; f && i < f->ntags; i++) {
        int found = 0;
        for (int j = 0; j < g_nchip; j++) if (al_eq(g_chip[j].id, f->tags[i])) found = 1;
        if (!found && g_nchip < MAX_CHIPS) { al_cpy(g_chip[g_nchip].id, f->tags[i], sizeof g_chip[0].id); g_chip[g_nchip++].n = 0; }
    }
    /* Most first (the portal's order); how a station is heard leads. */
    for (int i = 1; i < g_nchip; i++)
        for (int j = i; j > 0; j--) {
            int ra = al_starts(g_chip[j].id, "reach:"), rb = al_starts(g_chip[j - 1].id, "reach:");
            if (ra > rb || (ra == rb && g_chip[j].n > g_chip[j - 1].n)) {
                char t[28];
                int n = g_chip[j].n;
                al_cpy(t, g_chip[j].id, sizeof t);
                al_cpy(g_chip[j].id, g_chip[j - 1].id, sizeof t);
                g_chip[j].n = g_chip[j - 1].n;
                al_cpy(g_chip[j - 1].id, t, sizeof t);
                g_chip[j - 1].n = n;
            } else break;
        }
    al_cpy(out, "[", cap);
    for (int i = 0; i < g_nchip; i++) {
        char label[40];
        wst_tag_name(g_chip[i].id, label, sizeof label);
        if (i) al_cat(out, ",", cap);
        al_cat(out, "{\"id\":\"", cap);
        al_jesc(out, g_chip[i].id, cap);
        al_cat(out, "\",\"label\":\"", cap);
        al_jesc(out, label, cap);
        al_cat(out, "\",\"count\":", cap);
        al_cat_u(out, (unsigned long long)g_chip[i].n, cap);
        al_cat(out, f && wst_filter_has(f, g_chip[i].id) ? ",\"on\":true}" : ",\"on\":false}", cap);
    }
    al_cat(out, "]", cap);
    return (int)al_len(out);
}

void wst_row_tags(const char *row, char *out, unsigned cap)
{
    char sv[96], src[24], t[40], name[40];
    out[0] = 0;
    if (al_json(row, "serve", sv, sizeof sv)) {
        const char *s = sv;
        while (*s) {
            char word[28];
            unsigned k = 0;
            while (*s == ',') s++;
            while (*s && *s != ',' && k + 1 < sizeof word) word[k++] = *s++;
            word[k] = 0;
            if (!k) continue;
            al_cpy(t, "serve:", sizeof t);
            al_cat(t, word, sizeof t);
            wst_tag_name(t, name, sizeof name);
            if (out[0]) al_cat(out, " \xC2\xB7 ", cap);
            al_cat(out, name, cap);
        }
    }
    if (al_json(row, "src", src, sizeof src) && src[0]) {
        al_cpy(t, "source:", sizeof t);
        al_cat(t, src, sizeof t);
        wst_tag_name(t, name, sizeof name);
        if (out[0]) al_cat(out, " \xC2\xB7 ", cap);
        al_cat(out, name, cap);
    }
}
