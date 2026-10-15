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
 *   Post     one question per full screen, every answer a tile: warning or
 *            notice, what it is, how bad, the message, for how long, where;
 *            then a review where any tile leads back to its question
 *   Alert    one of them: the facts as tiles, the message at reading size,
 *            and Withdraw when it is ours
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
#define KEEP_MAX   48             /* warnings and notices kept in kv */

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

/* Written once per batch, never per id: the archive read at start used to
 * rewrite the whole list once for each alert it found, and on a first start
 * that was most of the 1.8 s the page waited before its first frame. */
static int g_seen_dirty;

static void seen_add(const char *id)
{
    if (seen(id)) return;
    al_cpy(g_seen[g_seen_w], id, sizeof g_seen[0]);
    g_seen_w = (g_seen_w + 1) % SEEN_MAX;
    g_seen_dirty = 1;
}

static void seen_flush(void)
{
    if (!g_seen_dirty) return;
    g_seen_dirty = 0;
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

static int zone(void) { return (int)hal_time_utc_offset(); }

/* "until Thu 22 Oct" for a choice of how long: the day is what matters. */
static void until_words(char *out, unsigned cap, unsigned long long t)
{
    char w[32];
    al_when(w, sizeof w, t, zone());
    for (char *c = w; *c; c++) if (*c == ',') { *c = 0; break; }
    al_cpy(out, "until ", cap);
    al_cat(out, w, cap);
    /* A year on is the same day and month: say which year. */
    if (t > now_s() + 300ULL * 86400ULL) {
        char st[24];
        al_stamp(st, sizeof st, t);
        st[4] = 0;
        al_cat(out, " ", cap);
        al_cat(out, st, cap);
    }
}

/* The words of sections 24 and 25, as a person says them, with the one line
 * the Post screens show under each. */
typedef struct { const char *word, *name, *hint, *group; } kind_t;
static const kind_t k_warning[] = {
    { "fire",      "Fire",        "Flames or smoke nearby",       "Heat" },
    { "flood",     "Flood",       "Rising or standing water",     "Water" },
    { "storm",     "Storm",       "Thunder and lightning",        "Weather" },
    { "wind",      "Wind",        "Strong gusts, falling branches", "Weather" },
    { "snow",      "Snow",        "Heavy snow, ways blocked",     "Weather" },
    { "ice",       "Ice",         "Slippery ground or roads",     "Weather" },
    { "quake",     "Earthquake",  "Shaking felt, more may come",  "Ground" },
    { "landslide", "Landslide",   "Ground or rocks moving",       "Ground" },
    { "tsunami",   "Tsunami",     "Get to high ground",           "Water" },
    { "chemical",  "Chemical",    "A spill, a leak, fumes",       "Toxic" },
    { "radiation", "Radiation",   "A radiological hazard",        "Toxic" },
    { "outage",    "Outage",      "No power, water or network",   "Services" },
    { "road",      "Road",        "A dangerous road or crossing", "Travel" },
    { "crowd",     "Crowd",       "A dangerous crush of people",  "People" },
    { "animal",    "Animal",      "A dangerous animal about",     "Wildlife" },
    { "other",     "Other",       "Say what it is in the message", "Anything else" },
};
static const kind_t k_info[] = {
    { "closure",   "Closure",     "A gate, path or road is shut", "Travel" },
    { "works",     "Works",       "Roadworks or building works",  "Travel" },
    { "traffic",   "Traffic",     "A queue or a jam",             "Travel" },
    { "slow",      "Slow going",  "Moving, but slowly",           "Travel" },
    { "stopped",   "Stopped car", "A vehicle stopped on the way", "Travel" },
    { "debris",    "Debris",      "Something lying in the way",   "Travel" },
    { "rain",      "Rain",        "Heavy rain, standing water",   "Weather" },
    { "fog",       "Fog",         "Hard to see far",              "Weather" },
    { "wind",      "Wind",        "Gusty, hold on to things",     "Weather" },
    { "snow",      "Snow",        "Snow on paths and roads",      "Weather" },
    { "ice",       "Ice",         "Slippery in places",           "Weather" },
    { "animal",    "Animals",     "Animals on the path or road",  "Wildlife" },
    { "crowd",     "Crowd",       "Busy, expect to wait",         "People" },
    { "event",     "Event",       "Something is on here",         "People" },
    { "other",     "Other",       "Say what it is in the message", "Anything else" },
};
#define N_WARNING (int)(sizeof k_warning / sizeof k_warning[0])
#define N_INFO    (int)(sizeof k_info / sizeof k_info[0])

static const kind_t *kind_of(const char *type, const char *word)
{
    const kind_t *t = al_eq(type, "warning") ? k_warning : k_info;
    int n = al_eq(type, "warning") ? N_WARNING : N_INFO;
    for (int i = 0; i < n; i++) if (al_eq(t[i].word, word)) return &t[i];
    return 0;
}

typedef struct { const char *word, *name, *hint, *level; int alert; } sev_t;
static const sev_t k_sev[] = {
    { "watch",   "Watch",   "May affect you: be ready", "Level 1", 0 },
    { "warning", "Warning", "Will affect you: act now", "Level 2", 0 },
    { "danger",  "Danger",  "Life-threatening: leave",  "Level 3", 1 },
};

static const sev_t *sev_of(const char *word)
{
    for (int i = 0; i < 3; i++) if (al_eq(k_sev[i].word, word)) return &k_sev[i];
    return 0;
}

typedef struct { const char *word, *name, *hint; } choice_t;
static const choice_t k_for[] = {
    { "1d", "1 day", "" }, { "2d", "2 days", "" }, { "1w", "1 week", "" },
    { "1m", "1 month", "" }, { "6m", "6 months", "" }, { "1y", "1 year", "" },
};
static const choice_t k_where[] = {
    { "none", "Not marked", "Nearby is enough" },
    { "spot", "Right here", "This exact spot" },
    { "100m", "100 m",      "A street, a car park" },
    { "500m", "500 m",      "A neighbourhood" },
    { "1km",  "1 km",       "A village" },
    { "5km",  "5 km",       "A whole town" },
};

/* "Flood", "Works": the kind as a person says it. */
static void kind_words(const al_alert_t *a, char *out, unsigned cap)
{
    const kind_t *k = kind_of(a->type, a->kind);
    if (k) { al_cpy(out, k->name, cap); return; }
    al_cpy(out, a->kind[0] ? a->kind : (al_eq(a->type, "warning") ? "Warning" : "Notice"), cap);
    out[0] = al_up(out[0]);
}

/* "Danger: flood", "Notice: closure": what the row and the notification
 * lead with. */
static void title_of(const al_alert_t *a, char *out, unsigned cap)
{
    char k[24];
    kind_words(a, k, sizeof k);
    const sev_t *s = al_eq(a->type, "warning") ? sev_of(a->sev) : 0;
    al_cpy(out, s ? s->name : (al_eq(a->type, "warning") ? "Warning" : "Notice"), cap);
    al_cat(out, ": ", cap);
    if (k[0] >= 'A' && k[0] <= 'Z') k[0] = (char)(k[0] + 32);
    al_cat(out, k, cap);
}

/* The row's icon: what kind of alert it is at a glance, one icon per
 * meaning (the host offers no hazard sign, so these are its nearest). */
static const char *icon_of(const al_alert_t *a, int state)
{
    if (state == AL_PLANNED) return "schedule";
    if (!al_eq(a->type, "warning")) return "info";
    if (al_eq(a->sev, "danger")) return "block";
    if (al_eq(a->sev, "warning")) return "campaign";
    return "visibility";
}

/* ── Tiles ────────────────────────────────────────────────────────────── */
static int g_tiles_first;

static void tiles_begin(const char *field)
{
    al_cpy(g_out, "{\"type\":\"ui.stats.set\",\"field\":\"", sizeof g_out);
    al_cat(g_out, field, sizeof g_out);
    al_cat(g_out, "\",\"tiles\":[", sizeof g_out);
    g_tiles_first = 1;
}

static void tile(const char *id, const char *label, const char *value,
                 const char *hint, int alert, int tap)
{
    if (!g_tiles_first) al_cat(g_out, ",", sizeof g_out);
    g_tiles_first = 0;
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_jesc(g_out, id, sizeof g_out);
    al_cat(g_out, "\",\"label\":\"", sizeof g_out);
    al_jesc(g_out, label, sizeof g_out);
    al_cat(g_out, "\",\"value\":\"", sizeof g_out);
    al_jesc(g_out, value, sizeof g_out);
    al_cat(g_out, "\"", sizeof g_out);
    if (hint && hint[0]) {
        al_cat(g_out, ",\"hint\":\"", sizeof g_out);
        al_jesc(g_out, hint, sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out);
    }
    if (alert) al_cat(g_out, ",\"alert\":true", sizeof g_out);
    if (tap) al_cat(g_out, ",\"tap\":true", sizeof g_out);
    al_cat(g_out, "}", sizeof g_out);
}

static void tiles_end(void)
{
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* One details card with one text in it: the message, at reading size, under
 * a small label saying what it is. */
static void message_card(const char *field, const char *title, const char *text)
{
    char m[600] = "{\"type\":\"ui.field.set\",\"field\":\"";
    al_cat(m, field, sizeof m);
    al_cat(m, "\",\"value\":[{\"title\":\"\",\"items\":[{\"label\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\",\"value\":\"", sizeof m);
    al_jesc(m, text, sizeof m);
    al_cat(m, "\"}]}]}", sizeof m);
    say(m);
}

/* ── The list ─────────────────────────────────────────────────────────── */
static int find(const char *id)
{
    for (int i = 0; i < g_nal; i++) if (al_eq(g_al[i].id, id)) return i;
    return -1;
}

/* A row: what it is, the message, and two facts as chips. */
static void item(const al_alert_t *a, unsigned long long now, int state, int *first)
{
    char title[48], sub[200] = "", left[32], tags[160] = "";
    title_of(a, title, sizeof title);
    al_cpy(sub, a->m[0] ? a->m : "No message", sizeof sub);
    if (state == AL_PLANNED) {
        char w[32];
        al_when(w, sizeof w, a->since, zone());
        al_cpy(left, "from ", sizeof left);
        al_cat(left, w, sizeof left);
    } else {
        al_left(left, sizeof left, now, a->until);
    }
    if (left[0]) { al_cat(tags, "\"", sizeof tags); al_jesc(tags, left, sizeof tags); al_cat(tags, "\"", sizeof tags); }
    if (!a->mine) {
        if (tags[0]) al_cat(tags, ",", sizeof tags);
        al_cat(tags, "\"by ", sizeof tags);
        al_jesc(tags, a->from, sizeof tags);
        al_cat(tags, "\"", sizeof tags);
    }
    if (!*first) al_cat(g_out, ",", sizeof g_out);
    *first = 0;
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_jesc(g_out, a->id, sizeof g_out);
    al_cat(g_out, "\",\"icon\":\"", sizeof g_out);
    al_cat(g_out, icon_of(a, state), sizeof g_out);
    al_cat(g_out, "\",\"title\":\"", sizeof g_out);
    al_jesc(g_out, title, sizeof g_out);
    al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    al_jesc(g_out, sub, sizeof g_out);
    al_cat(g_out, "\",\"tags\":[", sizeof g_out);
    al_cat(g_out, tags, sizeof g_out);
    al_cat(g_out, "]}", sizeof g_out);
}

/* Newest first: the rows in ts order, by selection, which for 32 is
 * nothing. Sections with nothing in them are left out. */
static void push_list(void)
{
    const unsigned long long now = now_s();
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"alerts\",\"sections\":[", sizeof g_out);
    static const char *const titles[3] = {
        "{\"title\":\"Now\",\"items\":[",
        "{\"title\":\"Planned\",\"items\":[",
        "{\"title\":\"Yours\",\"items\":["
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
            item(&g_al[best], now, al_state(&g_al[best], now), &first);
        }
        if (!first) al_cat(g_out, "]}", sizeof g_out);
    }
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* One alert: the facts as tiles, the message as the one thing to read. */
static void push_alert(void)
{
    if (g_sel < 0 || g_sel >= g_nal) return;
    const al_alert_t *a = &g_al[g_sel];
    const unsigned long long now = now_s();
    const int st = al_state(a, now);
    char v[48], h[64];
    tiles_begin("a_hub");
    if (al_eq(a->type, "warning")) {
        const sev_t *s = sev_of(a->sev);
        tile("sev", "How bad", s ? s->name : "Warning", s ? s->hint : "", s && s->alert, 0);
    } else {
        tile("sev", "Type", "Notice", "Worth knowing, not a hazard", 0, 0);
    }
    kind_words(a, v, sizeof v);
    const kind_t *k = kind_of(a->type, a->kind);
    tile("kind", "What", v, k ? k->hint : "", 0, 0);
    if (a->withdrawn) {
        tile("left", "State", "Withdrawn", "Its author said it is over", 0, 0);
    } else if (st == AL_PLANNED) {
        al_when(h, sizeof h, a->since, zone());
        tile("left", "Starts", h, "", 0, 0);
    } else if (a->until && now) {
        if (now >= a->until) al_cpy(v, "Ended", sizeof v);
        else al_span(v, sizeof v, a->until - now);
        al_cpy(h, "until ", sizeof h);
        al_when(h + 6, sizeof h - 6, a->until, zone());
        tile("left", "Left", v, h, 0, 0);
    }
    al_cpy(h, "posted ", sizeof h);
    al_when(h + 7, sizeof h - 7, a->ts, zone());
    tile("from", "From", a->mine ? "You" : a->from, a->ts ? h : "", 0, 0);
    if (a->pos[0]) {
        al_cpy(v, a->rad[0] ? a->rad : "a spot", sizeof v);
        tile("where", "Area", v, a->pos, 0, 0);
    }
    tiles_end();
    message_card("a_msg", "Message", a->m[0] ? a->m : "No message: the tiles below say it all.");
    flag_hidden("withdraw", !(a->mine && !a->withdrawn));
}

static void redraw(void)
{
    push_list();
    if (g_sel >= 0) push_alert();
}

/* ── What this wapp keeps ──────────────────────────────────────────────
 *
 * Every warning, notice and withdrawal it has taken, as the wire and the id,
 * in a ring of kv slots. A start reads these instead of the archive: the
 * archive has no index on the packet type, so asking it for the newest
 * warnings walks every row it holds, newest first, looking for rare ones,
 * on the UI isolate. On the C61 (156,000 packets) that was 2.2 s before the
 * first frame, while the screen said "All clear" over alerts that stood.
 * The archive is read once, on the very first start; after that this wapp
 * hears every alert as it arrives (it runs headless), so the ring is the
 * whole story.
 */
static int g_keep_w = -1;          /* next slot; -1 until restored */
static int g_restoring;

static void keep_key(char *k, int i)
{
    k[0] = 'w';
    k[1] = (char)('0' + i / 10);
    k[2] = (char)('0' + i % 10);
    k[3] = 0;
}

static void keep(const char *id, const char *wire)
{
    if (g_restoring || g_keep_w < 0) return;
    char k[4], v[AL_WIRE_MAX + 16];
    keep_key(k, g_keep_w);
    al_cpy(v, id, sizeof v);
    al_cat(v, " ", sizeof v);
    al_cat(v, wire, sizeof v);
    hal_kv_set(k, 3, v, al_len(v));
    g_keep_w = (g_keep_w + 1) % KEEP_MAX;
    char n[4] = { (char)('0' + g_keep_w / 10), (char)('0' + g_keep_w % 10), 0, 0 };
    hal_kv_set("wn", 2, n, 2);
}

static int take(const char *wire, const char *id, int may_announce);

/* 1 when the ring was there to read: not the first start. */
static int restore(void)
{
    char n[4];
    uint32_t got = hal_kv_get("wn", 2, n, sizeof n - 1);
    if (got != 2) { g_keep_w = 0; return 0; }
    g_keep_w = (n[0] - '0') * 10 + (n[1] - '0');
    if (g_keep_w < 0 || g_keep_w >= KEEP_MAX) g_keep_w = 0;
    g_restoring = 1;
    /* Oldest first, so a withdrawal kept after its alert finds it. */
    for (int j = 0; j < KEEP_MAX; j++) {
        int i = (g_keep_w + j) % KEEP_MAX;
        char k[4], v[AL_WIRE_MAX + 16];
        keep_key(k, i);
        uint32_t l = hal_kv_get(k, 3, v, sizeof v - 1);
        if (l < 8 || l >= sizeof v) continue;
        v[l] = 0;
        v[6] = 0;                         /* "<id> <wire>" */
        take(v + 7, v, 0);
    }
    g_restoring = 0;
    return 1;
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
        keep("------", wire);
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
    keep(a.id, wire);
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
    seen_flush();
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
    seen_flush();
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

/*
 * The alert being written. One question per screen, so nothing is a
 * dropdown and every answer is a tap; the screens are opened in turn and the
 * answers are kept here until Post. The panel's own arrow abandons it.
 */
static struct {
    char type[8], kind[12], sev[8], dur[4], where[8];
    char text[200];
    char step[12];               /* the screen showing, for Back */
} g_draft;

static void draft_reset(void)
{
    for (unsigned i = 0; i < sizeof g_draft; i++) ((char *)&g_draft)[i] = 0;
}

static void push_type(void)
{
    tiles_begin("p_type");
    tile("warning", "Hazard", "Warning", "Fire, flood, a storm...", 1, 1);
    tile("info", "Good to know", "Notice", "A closure, works, a queue...", 0, 1);
    tiles_end();
}

static void open_step(const char *step)
{
    al_cpy(g_draft.step, step, sizeof g_draft.step);
    if (al_eq(step, "Kind")) {
        int warn = al_eq(g_draft.type, "warning");
        tiles_begin("p_kind");
        const kind_t *t = warn ? k_warning : k_info;
        for (int i = 0, n = warn ? N_WARNING : N_INFO; i < n; i++)
            tile(t[i].word, t[i].group, t[i].name, t[i].hint, 0, 1);
        tiles_end();
        screen_open("Kind", warn ? "What is the hazard?" : "What is it about?");
    } else if (al_eq(step, "Severity")) {
        tiles_begin("p_sev");
        for (int i = 0; i < 3; i++)
            tile(k_sev[i].word, k_sev[i].level, k_sev[i].name, k_sev[i].hint, k_sev[i].alert, 1);
        tiles_end();
        screen_open("Severity", "How bad is it?");
    } else if (al_eq(step, "Message")) {
        char m[300] = "{\"type\":\"ui.field.set\",\"field\":\"a_text\",\"value\":\"";
        al_jesc(m, g_draft.text, sizeof m);
        al_cat(m, "\"}", sizeof m);
        say(m);
        screen_open("Message", "The message");
    } else if (al_eq(step, "Duration")) {
        const unsigned long long now = now_s();
        tiles_begin("p_for");
        for (int i = 0; i < 6; i++) {
            char h[48];
            until_words(h, sizeof h, now + al_duration(k_for[i].word));
            tile(k_for[i].word, "For", k_for[i].name, now ? h : "", 0, 1);
        }
        tiles_end();
        screen_open("Duration", "For how long?");
    } else if (al_eq(step, "Place")) {
        tiles_begin("p_where");
        for (int i = 0; i < 6; i++)
            tile(k_where[i].word, i < 2 ? "Position" : "Around me", k_where[i].name,
                 k_where[i].hint, 0, 1);
        tiles_end();
        screen_open("Place", "Where is it?");
    }
}

/* The wire for the draft, or a reason in [why]. */
static int compose(char *wire, unsigned cap, char *why, unsigned wcap)
{
    if (!g_draft.type[0] || !g_draft.kind[0]) { al_cpy(why, "Pick what it is first.", wcap); return -2; }
    const unsigned long long now = now_s();
    const unsigned long long life = al_duration(g_draft.dur[0] ? g_draft.dur : "1d");
    if (!now) { al_cpy(why, "This phone does not know the time yet.", wcap); return -2; }
    char pos[28] = "", rad[12] = "";
    if (g_draft.where[0] && !al_eq(g_draft.where, "none")) {
        int32_t lat = hal_sensor_gps_lat(), lon = hal_sensor_gps_lon();
        if (lat == (int32_t)0x80000000 || lon == (int32_t)0x80000000) {
            al_cpy(why, "This phone has no position yet. Wait a moment, or pick Not marked.", wcap);
            return -2;
        }
        char d[16];
        al_deg(pos, sizeof pos, lat);
        al_cat(pos, ",", sizeof pos);
        al_deg(d, sizeof d, lon);
        al_cat(pos, d, sizeof pos);
        if (!al_eq(g_draft.where, "spot")) al_cpy(rad, g_draft.where, sizeof rad);
    }
    int n = al_build(wire, cap, g_me, g_draft.type, g_draft.kind,
                     g_draft.sev[0] ? g_draft.sev : "watch", pos, rad, now, now + life,
                     g_draft.text);
    if (n == -1) al_cpy(why, "The message is too long to send with this alert. Shorten it.", wcap);
    else if (n < 0) al_cpy(why, "The message has a character that cannot be sent.", wcap);
    return n;
}

/* Everything chosen, as tiles that lead back to their question. */
static void open_review(void)
{
    al_cpy(g_draft.step, "Review", sizeof g_draft.step);
    int warn = al_eq(g_draft.type, "warning");
    const kind_t *k = kind_of(g_draft.type, g_draft.kind);
    tiles_begin("p_review");
    if (warn) {
        /* A warning says its type by how bad it is: "Danger" is a warning. */
        const sev_t *s = sev_of(g_draft.sev);
        tile("Severity", "Warning", s ? s->name : "Watch", s ? s->hint : "", s && s->alert, 1);
    } else {
        tile("Type", "Type", "Notice", "Not a hazard", 0, 1);
    }
    tile("Kind", k ? k->group : "What", k ? k->name : g_draft.kind, k ? k->hint : "", 0, 1);
    const unsigned long long now = now_s();
    for (int i = 0; i < 6; i++)
        if (al_eq(k_for[i].word, g_draft.dur)) {
            char h[48];
            until_words(h, sizeof h, now + al_duration(g_draft.dur));
            tile("Duration", "For", k_for[i].name, now ? h : "", 0, 1);
        }
    for (int i = 0; i < 6; i++)
        if (al_eq(k_where[i].word, g_draft.where))
            tile("Place", i < 2 ? "Position" : "Around me", k_where[i].name, k_where[i].hint, 0, 1);
    tiles_end();
    char wire[AL_WIRE_MAX + 1], why[120] = "";
    int n = compose(wire, sizeof wire, why, sizeof why);
    if (n < 0) message_card("p_msg", "Not ready yet", why);
    else message_card("p_msg", "Message", g_draft.text[0] ? g_draft.text : "No message: the tiles below say it all.");
    flag_hidden("post", n < 0);
    screen_open("Review", "Check and post");
}

/* The next question after [step], skipping what does not apply. */
static void next_after(const char *step)
{
    if (al_eq(step, "Post")) open_step("Kind");
    else if (al_eq(step, "Kind")) open_step(al_eq(g_draft.type, "warning") ? "Severity" : "Message");
    else if (al_eq(step, "Severity")) open_step("Message");
    else if (al_eq(step, "Message")) open_step("Duration");
    else if (al_eq(step, "Duration")) open_step("Place");
    else open_review();
}

/* An answer changed from the review goes straight back to the review. */
static int g_from_review;

static void answered(const char *step)
{
    if (g_from_review) { g_from_review = 0; open_review(); return; }
    next_after(step);
}

static void open_type(void)
{
    al_cpy(g_draft.step, "Type", sizeof g_draft.step);
    push_type();
    screen_open("New alert", "New alert");
}

static void step_back(void)
{
    const char *s = g_draft.step;
    g_from_review = 0;
    if (al_eq(s, "Kind")) { open_type(); return; }
    if (al_eq(s, "Severity")) open_step("Kind");
    else if (al_eq(s, "Message")) open_step(al_eq(g_draft.type, "warning") ? "Severity" : "Kind");
    else if (al_eq(s, "Duration")) open_step("Message");
    else if (al_eq(s, "Place")) open_step("Duration");
    else if (al_eq(s, "Review")) open_step("Place");
}

static void do_post(void)
{
    char wire[AL_WIRE_MAX + 1], why[120] = "";
    who_am_i();
    int n = compose(wire, sizeof wire, why, sizeof why);
    if (n < 0) { message_card("p_msg", "Not ready yet", why); return; }
    int rc = hal_xprs_send(wire, (uint32_t)n);
    if (rc != 0) {
        message_card("p_msg", "Not posted", rc == -2 ? "This phone may not send that."
                                                     : "This phone could not send it.");
        return;
    }
    /* Our own row at once, keyed on the id the copy that comes back will
     * carry (docs/architecture.md, a wapp shows its own action at once). */
    char id[7];
    al_id(wire, (unsigned)n, id);
    take(wire, id, 0);
    log_host("posted an alert");
    draft_reset();
    say("{\"type\":\"ui.field.set\",\"field\":\"a_text\",\"value\":\"\"}");
    say("{\"type\":\"ui.screen.close\"}");
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
        message_card("a_msg", "Not withdrawn", "This phone could not send it. Try again.");
        return;
    }
    take(wire, "", 0);                    /* marks ours withdrawn */
    log_host("withdrew an alert");
    g_sel = -1;
    say("{\"type\":\"ui.screen.close\"}");
    push_list();
}

static void on_command(void)
{
    char cmd[40] = "", id[16] = "";
    /* Buttons on the screen send a command; the + in the title bar sends
     * an action. Same names, either way. */
    if (!al_json(g_buf, "command", cmd, sizeof cmd) &&
        !al_json(g_buf, "action", cmd, sizeof cmd)) return;
    if (al_eq(cmd, "ready") || al_eq(cmd, "refresh")) {
        who_am_i();
        push_list();
        push_type();
        return;
    }
    if (al_eq(cmd, "screen_closed")) {
        /* The panel's arrow: an alert being written is abandoned, an alert
         * being read is put down. */
        g_sel = -1;
        g_from_review = 0;
        draft_reset();
        return;
    }
    if (al_eq(cmd, "alerts_tap")) {
        if (!fields("alerts_id", id, sizeof id)) return;
        g_sel = find(id);
        if (g_sel < 0) return;
        char t[48];
        title_of(&g_al[g_sel], t, sizeof t);
        push_alert();
        screen_open("Alert", t);
        return;
    }
    if (al_eq(cmd, "p_type_tap")) {
        if (!fields("p_type_id", id, sizeof id)) return;
        if (!al_eq(id, "warning") && !al_eq(id, "info")) return;
        int changed = !al_eq(g_draft.type, id);
        if (changed) { g_draft.kind[0] = 0; g_draft.sev[0] = 0; }
        al_cpy(g_draft.type, id, sizeof g_draft.type);
        if (g_from_review && changed) g_from_review = 0;   /* the rest no longer fits */
        answered("Post");
        return;
    }
    if (al_eq(cmd, "p_kind_tap")) {
        if (!fields("p_kind_id", id, sizeof id) || !kind_of(g_draft.type, id)) return;
        al_cpy(g_draft.kind, id, sizeof g_draft.kind);
        answered("Kind");
        return;
    }
    if (al_eq(cmd, "p_sev_tap")) {
        if (!fields("p_sev_id", id, sizeof id) || !sev_of(id)) return;
        al_cpy(g_draft.sev, id, sizeof g_draft.sev);
        answered("Severity");
        return;
    }
    if (al_eq(cmd, "p_next")) {
        fields("a_text", g_draft.text, sizeof g_draft.text);
        answered("Message");
        return;
    }
    if (al_eq(cmd, "p_for_tap")) {
        if (!fields("p_for_id", id, sizeof id) || !al_duration(id)) return;
        al_cpy(g_draft.dur, id, sizeof g_draft.dur);
        answered("Duration");
        return;
    }
    if (al_eq(cmd, "p_where_tap")) {
        if (!fields("p_where_id", id, sizeof id)) return;
        int ok = 0;
        for (int i = 0; i < 6; i++) if (al_eq(k_where[i].word, id)) ok = 1;
        if (!ok) return;
        al_cpy(g_draft.where, id, sizeof g_draft.where);
        answered("Place");
        return;
    }
    if (al_eq(cmd, "p_review_tap")) {
        /* A tile names the question it answers. */
        if (!fields("p_review_id", id, sizeof id)) return;
        g_from_review = 1;
        if (al_eq(id, "Type")) { g_from_review = 0; open_type(); return; }
        open_step(id);
        return;
    }
    if (al_eq(cmd, "p_back")) { step_back(); return; }
    if (al_eq(cmd, "post")) { do_post(); return; }
    if (al_eq(cmd, "withdraw")) { do_withdraw(); return; }
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
    if (!restore()) backfill();         /* the archive, once, ever */
    push_list();
    push_type();                        /* + opens New alert without asking */
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
