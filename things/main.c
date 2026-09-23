/*
 * things -- the devices around you, what each one can do, and the ones you
 * ask to be told about (XPRS.md 11.7, 11.7.1, 11.7.2).
 *
 * An X4 callsign is equipment: a doorbell, a camera, a pump, a lock, a
 * temperature sensor, a smart plug. It has no radio. A controller holds its
 * key and airs its `t:identity` and `t:observation` for it, and deposits
 * copies with the archivers it chose (12.3).
 *
 * ── What a thing IS, the format will not say ─────────────────────────────
 *
 * There is no capability field anywhere in XPRS: an identity carries a key,
 * a name and at most a url:, and a controller's `serve:devices` does not
 * name the devices it operates. So this wapp works it out from what a thing
 * SAYS -- the `state:` word it used (a closed list of nine, 11.7), whether
 * level:, target: or url: ever appeared, which measurement keys it sends,
 * and what came back when something was asked of it. Those are bits in a
 * mask (CAP_*), the mask picks a row in CLASS[], and the row decides what
 * the screen offers. A thing that has said nothing matches the last row and
 * shows its readings, which is what this wapp did for everything before.
 * The person can correct it, and the correction is kept.
 *
 * Adding a kind of thing is one row in CLASS[]. Adding a capability is one
 * bit, a row in EV[], and a row in CLASS[].
 *
 * ── Being told ──────────────────────────────────────────────────────────
 *
 * Ask to be told about a thing and this wapp subscribes to `xprs.observation`
 * -- only while something is watched, because a subscription is a cost the
 * whole phone pays for as long as it is held -- and raises a notification on
 * `state:pressed` and `state:motion`. Once per press: the 5 identifier is
 * the tag, so the same ring heard over two bearers is one notification, and
 * a backlog replayed at start is not a doorbell ringing in the night.
 *
 * It does this with no page open and no clock at all (module_tick_interval_ms
 * is 0 without a UI), which is the only time it matters.
 *
 * The wapp asks the core and draws. It never learns which lane carried a
 * reading or which archiver held it: pinning is hal_xprs_follow, and keeping
 * a device's packets and fetching them while it is away are the core's
 * (docs/architecture.md, "A device is followed by callsign"). It sends
 * nothing on the air.
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
    /* What it can do: the wire's evidence, what it refused, what the person
     * said. A capability is never derived from the callsign (see caps_of). */
    unsigned seen, denied, forced;
    int  notmine;                 /* a code:403 came back: it can, we may not */
    char klass[12];               /* the person's word for it; "" = work it out */
    char url[128];                /* 11.7.2's pointer, newest first */
    char state[12];               /* the newest state: word it aired */
    unsigned long long state_ms;  /* epoch of that state */
    char events[400];             /* what it did: "word:<epoch> .." newest first */
    int  watch;                   /* tell me when it reports */
    char cur_id[28];              /* the newest report already told about */
    char cur_ts[24];
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

/* A delivered packet is bigger than it looks: the core's row carries the
 * packet's fields as pairs AND the wire as one string AND the provenance
 * (bearer, via:, link:, the signature verdict), so a doorbell's press --
 * seven fields, a url: and a 64-character signature -- lands at well over a
 * kilobyte. At 1024 the row arrived truncated: `from` parsed, `wire` did
 * not, and the press was dropped without a word. Found with a real doorbell
 * on 2026-09-23; a weather station's observation is longer still. */
static char g_ev[4096];
static char g_topic[64];
static char g_buf[2048];
static char g_out[16384];
static char g_big[65536];         /* hal_xprs_stations, hal_xprs_history */
static char g_host[2048];         /* one hal_xprs_station answer */
static char g_row[1200];
static char g_wire[300];

/* ── A picture, and what it costs to hold one ─────────────────────────
 *
 * One megabyte is the bound, stated rather than hoped for (performance.md
 * 8.9), and it is what a real doorbell turned out to need: the D340W serves
 * one size of picture and one only, 2560x1920 at about 700 KB, on its still
 * and in every frame of its live view. Asked for a small one five different
 * ways -- `width=`, `snapType=sub`, the sub channel its own web client uses
 * -- it answers with the same 5 MP JPEG (measured 2026-09-23). A camera that
 * serves a 40 KB substream still costs 40 KB here; this bound is what the
 * expensive kind needs, not what the usual kind costs.
 *
 * What the size then decides is CADENCE, not admission: a live view of
 * 700 KB frames is paced to one every two seconds, because each one is a
 * megabyte of base64 through the host and a 5 MP decode on the isolate the
 * widgets run on (performance.md 8.14). Bigger than the bound is refused
 * with a line saying so. */
#define PIC_MAX 1048576u
static unsigned char g_pic[PIC_MAX];
static char g_msg[PIC_MAX * 4 / 3 + 256];

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

/* ── What a thing can do (XPRS.md 11.7) ───────────────────────────────
 *
 * The format has no way for a device to SAY what it is. An X4 identity
 * carries a key, a name and at most a url:, and 11.7.1's controller airs
 * `serve:devices` without naming the devices it operates. So capability is
 * evidence: the state: word it used (a closed list of nine, 11.7), whether
 * level:, target: or url: ever appeared, which measurement keys it sends,
 * and what came back when something was asked of it. A device that has
 * never spoken is a Device with its readings, which is what this wapp
 * showed for everything before this table existed.
 */
#define CAP_SWITCH   (1u << 0)    /* state:on | off                        */
#define CAP_OPENING  (1u << 1)    /* state:open | closed                   */
#define CAP_LOCK     (1u << 2)    /* state:locked | unlocked               */
#define CAP_OCCUP    (1u << 3)    /* state:motion | clear -- report only   */
#define CAP_BUTTON   (1u << 4)    /* state:pressed        -- report only   */
#define CAP_DIM      (1u << 5)    /* level:                                */
#define CAP_SETPOINT (1u << 6)    /* target:                               */
#define CAP_MEASURE  (1u << 7)    /* a reading of 15.3 .. 15.5.1           */
#define CAP_ENERGY   (1u << 8)    /* 15.5.2                                */
#define CAP_TOTALS   (1u << 9)    /* life*, odometer                       */
#define CAP_PICTURE  (1u << 10)   /* url: seen (11.7.2)                    */
#define CAP_SNAPSHOT (1u << 11)   /* s:snapshot answered                   */
#define CAP_STREAM   (1u << 12)   /* s:stream answered                     */
#define CAP_ANSWERS  (1u << 13)   /* it answered something it was asked    */
#define NCAP 14

static const char *const CAPNAME[NCAP] = {
    "Switches on and off", "Opens and closes", "Locks and unlocks",
    "Reports movement", "Reports a button press", "Takes a level",
    "Holds a setpoint", "Reports readings", "Reports energy",
    "Keeps running totals", "Offers a picture", "Answers q:snapshot",
    "Answers q:stream", "Answers what it is asked",
};

typedef struct { const char *key; const char *word; unsigned cap; } ev_t;

/* One row per thing a wire can prove. `word` narrows a key to one value,
 * which is what the closed state: list needs; a null word means any value. */
static const ev_t EV[] = {
    {"state", "on", CAP_SWITCH},      {"state", "off", CAP_SWITCH},
    {"state", "open", CAP_OPENING},   {"state", "closed", CAP_OPENING},
    {"state", "locked", CAP_LOCK},    {"state", "unlocked", CAP_LOCK},
    {"state", "motion", CAP_OCCUP},   {"state", "clear", CAP_OCCUP},
    {"state", "pressed", CAP_BUTTON},
    {"level", 0, CAP_DIM},            {"target", 0, CAP_SETPOINT},
    {"url", 0, CAP_PICTURE},
    {"s", "snapshot", CAP_SNAPSHOT | CAP_PICTURE | CAP_ANSWERS},
    {"s", "stream", CAP_STREAM | CAP_PICTURE | CAP_ANSWERS},
    {"s", "state", CAP_ANSWERS},
    {"temp", 0, CAP_MEASURE},   {"hum", 0, CAP_MEASURE},
    {"intemp", 0, CAP_MEASURE}, {"inhum", 0, CAP_MEASURE},
    {"press", 0, CAP_MEASURE},  {"wind", 0, CAP_MEASURE},
    {"wdir", 0, CAP_MEASURE},   {"gust", 0, CAP_MEASURE},
    {"rain1", 0, CAP_MEASURE},  {"rain24", 0, CAP_MEASURE},
    {"solar", 0, CAP_MEASURE},  {"volt", 0, CAP_MEASURE},
    {"batt", 0, CAP_MEASURE},   {"dose", 0, CAP_MEASURE},
    {"radon", 0, CAP_MEASURE},  {"rf", 0, CAP_MEASURE},
    {"efield", 0, CAP_MEASURE}, {"mfield", 0, CAP_MEASURE},
    {"produces", 0, CAP_ENERGY | CAP_MEASURE},
    {"consumes", 0, CAP_ENERGY | CAP_MEASURE},
    {"grid", 0, CAP_ENERGY | CAP_MEASURE},
    {"storage", 0, CAP_ENERGY | CAP_MEASURE},
    {"charged", 0, CAP_ENERGY | CAP_MEASURE},
    {"load", 0, CAP_ENERGY | CAP_MEASURE},
    {"lifeproduces", 0, CAP_TOTALS}, {"lifeconsumes", 0, CAP_TOTALS},
    {"lifegridin", 0, CAP_TOTALS},   {"lifegridout", 0, CAP_TOTALS},
    {"lifeload", 0, CAP_TOTALS},     {"lifedose", 0, CAP_TOTALS},
    {"odometer", 0, CAP_TOTALS},
};
#define NEV ((int)(sizeof EV / sizeof EV[0]))

/* What the Thing screen offers. A panel nobody can use is hidden, never
 * drawn empty: a screen that offers an action a thing cannot do is a screen
 * lying about what is in front of the person. */
#define P_READINGS (1u << 0)
#define P_EVENTS   (1u << 1)
#define P_PICTURE  (1u << 2)
#define P_LIVE     (1u << 3)    /* it can be watched, not only sampled */

typedef struct {
    const char *id, *title, *icon;
    unsigned need, forbid, panels;
    const char *summary;      /* the keys of its one-line subtitle, in order */
} class_t;

/* First match wins, so the specific rows come first and the last row matches
 * everything. Adding a kind of thing is one row here. */
static const class_t CLASS[] = {
    {"doorbell", "Doorbell", "campaign", CAP_BUTTON | CAP_PICTURE, 0,
     P_EVENTS | P_PICTURE | P_LIVE | P_READINGS, "state,batt,volt"},
    {"bell", "Doorbell", "campaign", CAP_BUTTON, 0,
     P_EVENTS | P_READINGS, "state,batt,volt"},
    {"camera", "Camera", "video", CAP_PICTURE, CAP_BUTTON,
     P_EVENTS | P_PICTURE | P_LIVE | P_READINGS, "state,batt,volt"},
    {"motion", "Movement sensor", "radar", CAP_OCCUP, CAP_PICTURE,
     P_EVENTS | P_READINGS, "state,batt,volt"},
    {"lock", "Lock", "lock", CAP_LOCK, 0, P_EVENTS | P_READINGS, "state,batt,volt"},
    {"cover", "Gate or valve", "update", CAP_OPENING, 0,
     P_EVENTS | P_READINGS, "state,level,batt,volt"},
    {"thermostat", "Thermostat", "tune", CAP_SETPOINT, 0,
     P_READINGS, "temp,target,batt,volt"},
    {"meter", "Meter", "grid", CAP_ENERGY, 0, P_READINGS,
     "produces,load,consumes,charged"},
    {"switch", "Switch", "power", CAP_SWITCH, 0, P_READINGS,
     "state,level,load,volt,batt"},
    {"sensor", "Sensor", "monitor_heart", CAP_MEASURE, CAP_SWITCH, P_READINGS,
     "temp,hum,volt,batt,dose"},
    {"thing", "Device", "developer_board", 0, 0, P_READINGS,
     "state,level,temp,hum,produces,load,consumes,volt,batt,charged,dose"},
};
#define NCLASS ((int)(sizeof CLASS / sizeof CLASS[0]))

/* Every capability [wire] is evidence for. A wire and the "k:v" list this
 * wapp keeps are the same shape, so one walker reads both. */
static unsigned caps_of(const char *wire)
{
    unsigned caps = 0;
    char key[20], v[40];
    for (const char *f = wire; *f; ) {
        while (*f == ' ') f++;
        if (!*f || th_starts(f, "m:")) break;
        unsigned k = 0;
        while (f[k] && f[k] != ':' && f[k] != ' ' && k < sizeof key - 1) { key[k] = f[k]; k++; }
        key[k] = 0;
        while (*f && *f != ' ') f++;
        if (!key[0] || !th_field(wire, key, v, sizeof v)) continue;
        for (int i = 0; i < NEV; i++) {
            if (!th_eq(EV[i].key, key)) continue;
            if (EV[i].word && !th_eq(EV[i].word, v)) continue;
            caps |= EV[i].cap;
        }
    }
    return caps;
}

/* What it can do, all three sources folded: what it showed, what the person
 * said, and what it refused. A refusal the person has overruled is ignored --
 * a code:403 says "not on its allow list today", which its owner can change. */
static unsigned caps_of_thing(const th_t *t)
{
    return (t->seen | t->forced) & ~(t->denied & ~t->forced);
}

static const class_t *class_by_id(const char *id)
{
    for (int i = 0; i < NCLASS; i++) if (th_eq(CLASS[i].id, id)) return &CLASS[i];
    return 0;
}

static const class_t *class_of(const th_t *t)
{
    if (t->klass[0]) {
        const class_t *c = class_by_id(t->klass);
        if (c) return c;
    }
    unsigned caps = caps_of_thing(t);
    for (int i = 0; i < NCLASS; i++)
        if ((caps & CLASS[i].need) == CLASS[i].need && !(caps & CLASS[i].forbid))
            return &CLASS[i];
    return &CLASS[NCLASS - 1];
}

/* ── What this phone remembers about a thing ──────────────────────────
 *
 * A capability learned from a packet has to outlive the packet: a doorbell
 * that rang yesterday is still a doorbell with the page closed and the
 * archive swept. The record is one line per thing in this wapp's own
 * key-value space, in the same "k:v" shape as a wire so one parser reads
 * both, plus an index key naming the things there are records for.
 */
#define KV_INDEX "cap.list"

static void kv_put(const char *key, const char *val)
{
    hal_kv_set(key, th_len(key), val, th_len(val));
}

static int kv_get(const char *key, char *out, unsigned cap)
{
    uint32_t n = hal_kv_get(key, th_len(key), out, cap - 1);
    if (n >= cap) n = cap - 1;
    out[n] = 0;
    return n > 0;
}

static void kv_key(char *out, unsigned cap, const char *prefix, const char *call)
{
    th_cpy(out, prefix, cap);
    th_cat(out, call, cap);
}

/* Is there anything worth writing down? A thing nobody watches, nobody
 * renamed and that has shown nothing is not worth a key. */
static int worth_keeping(const th_t *t)
{
    return t->seen || t->forced || t->denied || t->klass[0] || t->url[0];
}

static void index_add(const char *call)
{
    char list[1024] = "";
    kv_get(KV_INDEX, list, sizeof list);
    /* Already there? The list is comma separated, so compare whole words. */
    for (const char *p = list; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        char one[CALL_MAX];
        unsigned n = (unsigned)(e - p);
        if (n >= sizeof one) n = sizeof one - 1;
        for (unsigned i = 0; i < n; i++) one[i] = p[i];
        one[n] = 0;
        if (th_eq(one, call)) return;
        p = *e ? e + 1 : e;
    }
    if (list[0]) th_cat(list, ",", sizeof list);
    th_cat(list, call, sizeof list);
    kv_put(KV_INDEX, list);
}

/* Two records, deliberately.
 *
 * More than one engine of this wapp can be alive at once -- a page and the
 * headless one, and the host keeps a page's engine a while after it closes --
 * and they share this key-value space. So what each one writes has to be
 * safe to write from a copy that may be out of date. Capabilities are a
 * UNION, which cannot go backwards whoever writes it; the watch flag and its
 * cursor are written ONLY where they change, so a stale engine redrawing
 * cannot switch off a watch somebody just armed. Found on the bench with
 * three engines up (2026-09-22). */
static void save_caps(th_t *t)
{
    char key[32], rec[400] = "", stored[400], v[128];
    kv_key(key, sizeof key, "cap.", t->call);
    unsigned seen = t->seen;
    if (kv_get(key, stored, sizeof stored) &&
        th_field(stored, "seen", v, sizeof v))
        seen |= (unsigned)th_num(v);       /* what any engine ever saw */
    t->seen = seen;
    if (!worth_keeping(t)) { hal_kv_delete(key, th_len(key)); return; }
    char n[24];
    n[0] = 0; th_cat_u(n, seen, sizeof n);      th_put(rec, "seen", n, sizeof rec);
    n[0] = 0; th_cat_u(n, t->denied, sizeof n); th_put(rec, "denied", n, sizeof rec);
    n[0] = 0; th_cat_u(n, t->forced, sizeof n); th_put(rec, "forced", n, sizeof rec);
    if (t->klass[0]) th_put(rec, "class", t->klass, sizeof rec);
    if (t->url[0]) th_put(rec, "url", t->url, sizeof rec);
    kv_put(key, rec);
    index_add(t->call);
}

/* Written where the watch is armed, dropped, or moved on by a report. */
static void save_watch(th_t *t)
{
    char key[32], rec[120] = "";
    kv_key(key, sizeof key, "watch.", t->call);
    if (!t->watch) { hal_kv_delete(key, th_len(key)); return; }
    th_put(rec, "on", "1", sizeof rec);
    if (t->cur_ts[0]) th_put(rec, "cts", t->cur_ts, sizeof rec);
    if (t->cur_id[0]) th_put(rec, "cid", t->cur_id, sizeof rec);
    kv_put(key, rec);
    index_add(t->call);
}

static void save_thing(th_t *t) { save_caps(t); }

static th_t *get(const char *call);

static void load_thing(const char *call)
{
    char key[32], rec[400], v[128];
    th_t *t = 0;
    kv_key(key, sizeof key, "cap.", call);
    if (kv_get(key, rec, sizeof rec)) {
        t = get(call);
        if (!t) return;
        if (th_field(rec, "seen", v, sizeof v)) t->seen = (unsigned)th_num(v);
        if (th_field(rec, "denied", v, sizeof v)) t->denied = (unsigned)th_num(v);
        if (th_field(rec, "forced", v, sizeof v)) t->forced = (unsigned)th_num(v);
        if (th_field(rec, "class", v, sizeof v)) th_cpy(t->klass, v, sizeof t->klass);
        if (th_field(rec, "url", v, sizeof v)) th_cpy(t->url, v, sizeof t->url);
    }
    kv_key(key, sizeof key, "watch.", call);
    if (!kv_get(key, rec, sizeof rec)) return;
    if (!t) { t = get(call); if (!t) return; }
    if (th_field(rec, "on", v, sizeof v)) t->watch = th_num(v) ? 1 : 0;
    if (th_field(rec, "cts", v, sizeof v)) th_cpy(t->cur_ts, v, sizeof t->cur_ts);
    if (th_field(rec, "cid", v, sizeof v)) th_cpy(t->cur_id, v, sizeof t->cur_id);
}

static void load_records(void)
{
    char list[1024] = "";
    if (!kv_get(KV_INDEX, list, sizeof list)) return;
    for (const char *p = list; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        char one[CALL_MAX];
        unsigned n = (unsigned)(e - p);
        if (n >= sizeof one) n = sizeof one - 1;
        for (unsigned i = 0; i < n; i++) one[i] = p[i];
        one[n] = 0;
        if (one[0]) load_thing(one);
        p = *e ? e + 1 : e;
    }
}

/* Fold one wire's evidence into [t], and keep the pointer and the state word
 * it carried. Newest first, so the first url: and the first state: win. */
static void learn(th_t *t, const char *wire)
{
    unsigned before = t->seen;
    t->seen |= caps_of(wire);
    char v[128];
    if (!t->url[0] && th_field(wire, "url", v, sizeof v)) th_cpy(t->url, v, sizeof t->url);
    if (t->seen != before) save_thing(t);
}

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
            if (!pinned(g_th[i].call) && !g_th[i].watch && !th_eq(g_th[i].call, g_sel)) break;
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
        if (!th_json(g_row, "wire", g_wire, sizeof g_wire)) continue;
        learn(t, g_wire);
        if (th_field(g_wire, "nick", t->nick, sizeof t->nick)) return;
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
    t->rd[0] = t->src[0] = t->rd_sig[0] = t->state[0] = t->events[0] = 0;
    t->rd_ts = t->state_ms = 0;
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
        learn(t, g_wire);
        if (th_field(g_wire, "state", v, sizeof v)) {
            char ts[24];
            th_json(g_row, "ts", ts, sizeof ts);
            if (!t->state[0]) {
                th_cpy(t->state, v, sizeof t->state);
                t->state_ms = th_num(ts);
            }
            /* What it did, kept as it is read: one line per report, and the
             * detail screen draws from this rather than asking again. */
            if ((th_eq(v, "pressed") || th_eq(v, "motion") || th_eq(v, "clear")) &&
                th_len(t->events) < sizeof t->events - 40) {
                if (t->events[0]) th_cat(t->events, " ", sizeof t->events);
                th_cat(t->events, v, sizeof t->events);
                char u[128];
                if (th_field(g_wire, "url", u, sizeof u)) th_cat(t->events, "+pic", sizeof t->events);
                th_cat(t->events, ",", sizeof t->events);
                th_cat(t->events, ts[0] ? ts : "0", sizeof t->events);
            }
        }
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
        if (out[0]) {
            learn(t, out);
            /* The state word heard live is the newest there is: the archive's
             * copy of the same packet may not have been read yet. */
            char w[16], a[24];
            if (th_field(out, "state", w, sizeof w)) {
                th_cpy(t->state, w, sizeof t->state);
                unsigned long long now = hal_time_epoch(), age = 0;
                if (th_json(g_host, "agoMs", a, sizeof a)) age = th_num(a) / 1000ULL;
                t->state_ms = now > age ? now - age : now;
            }
            return 1;
        }
    }
    arch_read(t);
    th_cpy(out, t->rd, cap);
    return 0;
}

/* "on, 23.8V, 64%" -- the first three that it has. Production says where
 * it comes from: "1900W solar". */
static void summary(const class_t *c, const char *rd, char *out, unsigned cap)
{
    out[0] = 0;
    int n = 0;
    for (const char *k = c->summary; *k && n < 3; ) {
        char key[20];
        unsigned i = 0;
        while (*k && *k != ',' && i < sizeof key - 1) key[i++] = *k++;
        key[i] = 0;
        while (*k == ',') k++;
        char v[40];
        if (!key[0] || !th_field(rd, key, v, sizeof v)) continue;
        if (n++) th_cat(out, ", ", cap);
        th_cat(out, v, cap);
        char s[16];
        if (th_eq(key, "produces") && th_field(rd, "source", s, sizeof s)) {
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

/* "Rang, 12 s ago". XPRS.md 11.7 has no timeout for `motion`, so a movement
 * nobody cleared is aged rather than left standing: a camera that died
 * mid-event would otherwise read as moving for ever. */
static void state_words(const th_t *t, char *out, unsigned cap);

static unsigned long long reading_age_ms(const th_t *t)
{
    unsigned long long now = hal_time_epoch();
    if (!t->rd_ts || now < t->rd_ts) return 0;
    return (now - t->rd_ts) * 1000ULL;
}

static unsigned long long state_age_ms(const th_t *t)
{
    unsigned long long now = hal_time_epoch();
    if (!t->state_ms || now < t->state_ms) return 0;
    return (now - t->state_ms) * 1000ULL;
}

static void state_words(const th_t *t, char *out, unsigned cap)
{
    out[0] = 0;
    if (!t->state[0]) return;
    const char *word = th_eq(t->state, "pressed") ? "Rang"
                     : th_eq(t->state, "motion")  ? "Movement"
                     : th_eq(t->state, "clear")   ? "Clear" : 0;
    if (!word) return;
    unsigned long long age = state_age_ms(t);
    /* Nothing cleared it and it is old: say when, not that it is happening. */
    th_cpy(out, word, cap);
    if (t->state_ms) {
        char a[32];
        ago_words(age, a, sizeof a);
        th_cat(out, ", ", cap);
        th_cat(out, a, cap);
    }
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
    const class_t *c = class_of(t);
    summary(c, rd, sum, sizeof sum);
    if (t->nick[0]) th_cpy(sub, t->call, sizeof sub);
    char ev[64];
    state_words(t, ev, sizeof ev);
    if ((c->panels & P_EVENTS) && ev[0]) {
        if (sub[0]) th_cat(sub, ", ", sizeof sub);
        th_cat(sub, ev, sizeof sub);
    } else if (sum[0]) {
        if (sub[0]) th_cat(sub, ", ", sizeof sub);
        th_cat(sub, sum, sizeof sub);
    }
    if (!sub[0]) th_cpy(sub, "Nothing reported yet", sizeof sub);
    tag(tags, sizeof tags, c->title);
    if (t->watch) tag(tags, sizeof tags, "watched");
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
    th_cat(g_out, "\",\"icon\":\"", sizeof g_out);
    th_cat(g_out, c->icon, sizeof g_out);
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
/* The thing whose Settings fields have been filled in: they are pushed once
 * per screen opening, not per draw (see push_detail). */
static char g_fields_for[CALL_MAX];

/* Is the person looking at the long half of the screen? A doorbell's
 * screen is a picture, three buttons and what it did; everything else --
 * the callsign, the signature, the running totals, what it can do, the
 * address and the password -- is read once while something is being set
 * up and never again, so it is folded away until it is asked for. */
static int g_more;

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
        /* The row is its identity: the name comes free, and so does what
         * it said about itself -- 11.7.2 puts a camera's url: here, and a
         * thing that has never been asked for is known by this alone. */
        th_t *t = get(from);
        if (!t || !th_json(g_row, "wire", g_wire, sizeof g_wire)) continue;
        learn(t, g_wire);
        if (!t->nick[0] && th_field(g_wire, "nick", nick, sizeof nick)) {
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

/* ── Reaching a camera on the LAN ─────────────────────────────────────
 *
 * A thing's XPRS identity and its address on the house network are two
 * facts, and this is what joins them. The wire says WHAT happened; the
 * picture of it comes off the camera itself, over plain HTTP, and that is
 * not XPRS traffic: no packet crosses it, no bearer is chosen, nothing is
 * relayed. The core still owns every XPRS lane (docs/architecture.md); this
 * is a client speaking a vendor's own protocol to a box on the same LAN, the
 * way the Wapp Store speaks HTTP to fetch a catalogue.
 *
 * Two drivers, because two is what proves the shape:
 *
 *   link      fetch whatever address the thing published in url: (11.7.2)
 *             or the person typed. No credentials, nothing to leak.
 *   reolink   log in to the camera's own api.cgi, hold the token for its
 *             lease, and ask it for a still. The password is sealed to this
 *             device's key before it is written down.
 *
 * A vendor that needs a third is a third row in DRIVER[].
 *
 * ── Connecting is something a person does ───────────────────────────────
 *
 * Nothing on this side happens by itself. Hearing a doorbell ring does not
 * make this phone open a connection to a camera on the house network, and a
 * thing appearing in the list does not either: a device is heard on the air,
 * it is listed, and what this wapp does about it over the LAN begins when
 * somebody presses Connect. Connecting asks the thing what it can do
 * (`/api/services`, the one request that answers instead of probing), writes
 * down the answer, and only then does the screen offer the actions that
 * answer supports. Before that the LAN actions are not greyed out, they are
 * not there: a button that needs an address nobody has given is a button
 * that exists to disappoint.
 *
 * What is kept per thing: how to reach it (the person's), and what it said
 * when asked (the thing's). `at` is the second one's date, and it is what
 * "connected" means here -- there is no socket held open between actions.
 */
typedef struct {
    char vendor[12];        /* "", "link", "reolink"                     */
    char host[64];          /* address on the LAN                        */
    char user[24];
    char pass[64];          /* plaintext ONLY in memory, never written   */
    char path[80];          /* what to GET for the "link" driver         */
    char svc[48];           /* what it answered: "snapshot,stream"       */
    char base[96];          /* the address that answered                 */
    unsigned long long at;  /* when it answered; 0 = never connected     */
} conn_t;

static char g_my_npub[70];

/* This device's own key, so a password can be sealed to it and to nothing
 * else. The private half never leaves the host (hal_encrypt). */
static const char *my_npub(void)
{
    if (!g_my_npub[0]) {
        char pk[70];
        uint32_t n = hal_identity_pubkey(pk, sizeof pk - 1);
        if (n > 0) { pk[n] = 0; th_cpy(g_my_npub, pk, sizeof g_my_npub); }
    }
    return g_my_npub;
}

static void conn_key(char *out, unsigned cap, const char *call)
{
    th_cpy(out, "conn.", cap);
    th_cat(out, call, cap);
}

static void conn_load(const char *call, conn_t *c)
{
    for (unsigned i = 0; i < sizeof *c; i++) ((char *)c)[i] = 0;
    char key[32], rec[400], v[200];
    conn_key(key, sizeof key, call);
    if (!kv_get(key, rec, sizeof rec)) return;
    if (th_field(rec, "vendor", v, sizeof v)) th_cpy(c->vendor, v, sizeof c->vendor);
    if (th_field(rec, "host", v, sizeof v)) th_cpy(c->host, v, sizeof c->host);
    if (th_field(rec, "user", v, sizeof v)) th_cpy(c->user, v, sizeof c->user);
    if (th_field(rec, "path", v, sizeof v)) th_cpy(c->path, v, sizeof c->path);
    if (th_field(rec, "svc", v, sizeof v)) th_cpy(c->svc, v, sizeof c->svc);
    if (th_field(rec, "base", v, sizeof v)) th_cpy(c->base, v, sizeof c->base);
    if (th_field(rec, "at", v, sizeof v)) c->at = th_num(v);
    /* The password comes back only if this device's key can open it. */
    if (th_field(rec, "pass", v, sizeof v) && my_npub()[0]) {
        char clear[80];
        uint32_t n = hal_decrypt(my_npub(), th_len(my_npub()), v, th_len(v),
                                 clear, sizeof clear - 1);
        if (n > 0) { clear[n] = 0; th_cpy(c->pass, clear, sizeof c->pass); }
    }
}

static void conn_save(const char *call, const conn_t *c)
{
    char key[32], rec[400] = "";
    conn_key(key, sizeof key, call);
    if (!c->vendor[0] && !c->host[0] && !c->path[0] && !c->base[0] && !c->at) {
        hal_kv_delete(key, th_len(key));
        return;
    }
    if (c->vendor[0]) th_put(rec, "vendor", c->vendor, sizeof rec);
    if (c->host[0]) th_put(rec, "host", c->host, sizeof rec);
    if (c->user[0]) th_put(rec, "user", c->user, sizeof rec);
    if (c->path[0]) th_put(rec, "path", c->path, sizeof rec);
    if (c->svc[0]) th_put(rec, "svc", c->svc, sizeof rec);
    if (c->base[0]) th_put(rec, "base", c->base, sizeof rec);
    if (c->at) {
        char n[24]; n[0] = 0;
        th_cat_u(n, c->at, sizeof n);
        th_put(rec, "at", n, sizeof rec);
    }
    if (c->pass[0] && my_npub()[0]) {
        /* Sealed to this device's key: what is written down is of no use on
         * another phone, and of no use to anything that reads the file. */
        char sealed[200];
        uint32_t n = hal_encrypt(my_npub(), th_len(my_npub()),
                                 c->pass, th_len(c->pass), sealed, sizeof sealed - 1);
        if (n > 0) { sealed[n] = 0; th_put(rec, "pass", sealed, sizeof rec); }
        else note("could not seal the password, so it was not kept");
    }
    kv_put(key, rec);
    index_add(call);
}

/* Does the thing's answer name [what]? The list is what `/api/services`
 * said, comma separated, so a whole word is compared and "snapshot" never
 * matches inside "snapshotting". */
static int svc_has_str(const char *list, const char *what)
{
    for (const char *p = list; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        unsigned n = (unsigned)(e - p), w = th_len(what);
        if (n == w) {
            unsigned i = 0;
            while (i < n && p[i] == what[i]) i++;
            if (i == n) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int svc_has(const conn_t *c, const char *what)
{
    return svc_has_str(c->svc, what);
}

/* Where this thing lives on the network: the address the person gave, else
 * the origin of the picture address it published itself (11.7.2). An origin
 * and a path are different things, and a camera that serves its still from
 * one port and its stream from another is the ordinary case. */
static void origin_of(const th_t *t, const conn_t *c, char *out, unsigned cap)
{
    out[0] = 0;
    if (c->host[0]) {
        if (!th_starts(c->host, "http://") && !th_starts(c->host, "https://"))
            th_cpy(out, "http://", cap);
        th_cat(out, c->host, cap);
        unsigned n = th_len(out);
        while (n && out[n - 1] == '/') out[--n] = 0;
        return;
    }
    if (!t->url[0]) return;
    unsigned slashes = 0, i = 0;
    while (t->url[i] && i < cap - 1) {
        if (t->url[i] == '/') {
            slashes++;
            if (slashes == 3) break;
        }
        out[i] = t->url[i];
        i++;
    }
    out[i] = 0;
    if (slashes < 2) out[0] = 0;          /* not an address at all */
}

/* "http://192.168.1.9:8080" -> host and port. 0 when it is not one. */
static int split_origin(const char *base, char *host, unsigned cap, int *port)
{
    const char *p = base;
    *port = 80;
    if (th_starts(p, "http://")) p += 7;
    else if (th_starts(p, "https://")) { p += 8; *port = 443; }
    unsigned i = 0;
    while (p[i] && p[i] != ':' && p[i] != '/' && i < cap - 1) { host[i] = p[i]; i++; }
    host[i] = 0;
    if (p[i] == ':') {
        unsigned long long v = th_num(p + i + 1);
        if (v > 0 && v < 65536) *port = (int)v;
    }
    return host[0] != 0;
}

/* ── Fetching one still ──────────────────────────────────────────────
 *
 * One request in flight, polled once per tick and never spun on: the
 * Terminal wapp's fetch loops hal_http_poll 200 times inside one call, which
 * is a wapp blocking the isolate the widgets run on. */
#define FETCH_IDLE  0
#define FETCH_LOGIN 1
#define FETCH_SNAP  2
#define FETCH_ASK   3     /* connecting: asking it what it serves */

static struct {
    char call[CALL_MAX];
    int  req;                  /* hal_http_request handle, -1 when none  */
    int  stage;
    char token[80];            /* reolink session, for its lease         */
    unsigned long long asked_ms;
    unsigned long long token_ms;
    unsigned long long pic_ms;   /* when the picture on screen was taken */
    char note[120];            /* what to say under the picture          */
    unsigned long long note_ms;
    int  probing;              /* this fetch is a Connect, not a picture */
} g_fetch = { .req = -1 };

/* ── Watching it, rather than sampling it ─────────────────────────────
 *
 * A doorbell that only answers "here is a still" is a doorbell somebody
 * refreshes by hand while a stranger is at the door. Live view is one TCP
 * connection carrying `multipart/x-mixed-replace` -- a run of JPEGs -- read
 * straight off a socket, because that is not an XPRS lane and there is no
 * packet in it: the core owns bearers, custody and keys, and none of the
 * three is involved in pulling pictures off a box on the same LAN
 * (docs/architecture.md 1). The permission is declared in the manifest,
 * which is where a claim on a communication path belongs.
 *
 * What bounds it, because an open socket is a cost nobody sees:
 *
 *  - it runs only while the page is in front of somebody (`hal_ui_attached`),
 *  - it stops itself after three minutes rather than streaming all night,
 *  - the frame being assembled shares the still's buffer, so watching costs
 *    no more memory than one picture (performance.md 8.9), and a frame that
 *    would overrun it is dropped rather than shown in halves,
 *  - the screen is asked to redraw at most a few times a second whatever the
 *    camera sends, and
 *  - leaving the screen, or the engine being disposed, closes it.
 *
 * A camera with no stream is watched the honest way: one still after
 * another, about one a second, said in those words on the screen.
 */
#define LIVE_OFF     0
#define LIVE_DIAL    1      /* the socket is opening */
#define LIVE_STREAM  2      /* JPEGs are arriving on it */
#define LIVE_STILLS  3      /* no stream: one picture after another */
#define LIVE_MAX_MS  180000ULL
#define LIVE_GAP_MS  350ULL     /* the fastest the screen is asked to change */
#define LIVE_BIG     250000u    /* over this, a frame is paced, not raced */
#define LIVE_BIG_MS  2000ULL
#define STILL_GAP_MS 1200ULL    /* a camera with no stream, sampled */

static struct {
    char call[CALL_MAX];
    int  sock;                  /* hal_socket handle, -1 when none */
    int  stage;
    int  in_frame;
    unsigned len;               /* bytes of the frame being assembled */
    unsigned frames, dropped, last_len;
    unsigned long long began_ms, shown_ms, byte_ms, note_ms;
    char note[140];
} g_live = { .sock = -1 };

static unsigned char g_rx[8192];

static void picnote(void);          /* defined with the other panels */
static void draw(int force);        /* defined with the rest of the drawing */

static void ago_words(unsigned long long ms, char *out, unsigned cap);

static void fetch_done(const char *why)
{
    if (g_fetch.req >= 0) hal_http_free(g_fetch.req);
    g_fetch.req = -1;
    g_fetch.stage = FETCH_IDLE;
    g_fetch.probing = 0;
    if (why) {
        th_cpy(g_fetch.note, why, sizeof g_fetch.note);
        g_fetch.note_ms = hal_time_ms();
        /* The last picture is still on the screen. Saying only that this
         * attempt failed leaves a person reading an old doorstep as the
         * doorstep now. */
        if (g_fetch.pic_ms && !th_starts(why, "Taken")) {
            char a[40];
            ago_words(hal_time_ms() - g_fetch.pic_ms, a, sizeof a);
            th_cat(g_fetch.note, " The picture above is from ", sizeof g_fetch.note);
            th_cat(g_fetch.note, a, sizeof g_fetch.note);
            th_cat(g_fetch.note, ".", sizeof g_fetch.note);
        }
    }
    g_dirty = 1;
    picnote();
}

/* GET, or POST when [body] is given. 1 when the host took it. */
static int fetch_http(int post, const char *url, const char *body)
{
    int r = hal_http_request(post ? 1 : 0, url, th_len(url),
                             body ? body : "", body ? th_len(body) : 0);
    if (r < 0) { fetch_done("Could not reach it."); return 0; }
    g_fetch.req = r;
    g_fetch.asked_ms = hal_time_ms();
    return 1;
}

/* Where a "link" thing's picture lives: what it published, else what the
 * person typed, else the host and path they gave. */
static void link_url(const th_t *t, const conn_t *c, char *out, unsigned cap)
{
    out[0] = 0;
    /* The address that answered Connect wins: it is the one a person chose
     * and this wapp has proof of, where url: is only what was aired. */
    if (c->at && c->base[0] && svc_has(c, "snapshot")) {
        th_cpy(out, c->base, cap);
        th_cat(out, c->path[0] ? c->path : "/door/snapshot.jpg", cap);
        return;
    }
    if (t->url[0]) { th_cpy(out, t->url, cap); return; }
    if (!c->host[0]) return;
    th_cpy(out, "http://", cap);
    th_cat(out, c->host, cap);
    if (c->path[0] && c->path[0] != '/') th_cat(out, "/", cap);
    th_cat(out, c->path[0] ? c->path : "/door/snapshot.jpg", cap);
}

static void reolink_login_url(const conn_t *c, char *out, unsigned cap)
{
    th_cpy(out, "http://", cap);
    th_cat(out, c->host, cap);
    th_cat(out, "/cgi-bin/api.cgi?cmd=Login", cap);
}

static void reolink_login_body(const conn_t *c, char *out, unsigned cap)
{
    th_cpy(out, "[{\"cmd\":\"Login\",\"action\":0,\"param\":{\"User\":{\"Version\":\"0\",\"userName\":\"", cap);
    th_jesc(out, c->user[0] ? c->user : "admin", cap);
    th_cat(out, "\",\"password\":\"", cap);
    th_jesc(out, c->pass, cap);
    th_cat(out, "\"}}}]", cap);
}

static void reolink_snap_url(const conn_t *c, const char *token, char *out, unsigned cap)
{
    th_cpy(out, "http://", cap);
    th_cat(out, c->host, cap);
    th_cat(out, "/cgi-bin/api.cgi?cmd=Snap&channel=0&rs=", cap);
    th_cat_u(out, hal_time_ms() % 1000000ULL, cap);   /* the camera wants one */
    th_cat(out, "&token=", cap);
    th_cat(out, token, cap);
}

/* Ask for a picture of [t]. */
static void fetch_start(th_t *t)
{
    if (g_fetch.req >= 0) return;            /* one at a time, per 8.6 */
    conn_t c;
    conn_load(t->call, &c);
    th_cpy(g_fetch.call, t->call, sizeof g_fetch.call);
    g_fetch.probing = 0;
    g_fetch.note[0] = 0;
    char url[240];
    if (th_eq(c.vendor, "reolink")) {
        if (!c.host[0]) { fetch_done("No address for it yet."); return; }
        if (!c.pass[0]) { fetch_done("It needs its password."); return; }
        /* A token lasts an hour; the camera allows only a few at once and
         * cannot be made to forget one, so a new login per picture is a way
         * to lock the household out of its own doorbell. */
        if (g_fetch.token[0] && hal_time_ms() - g_fetch.token_ms < 1800000ULL) {
            reolink_snap_url(&c, g_fetch.token, url, sizeof url);
            g_fetch.stage = FETCH_SNAP;
            fetch_http(0, url, 0);
            return;
        }
        char body[300];
        reolink_login_url(&c, url, sizeof url);
        reolink_login_body(&c, body, sizeof body);
        g_fetch.stage = FETCH_LOGIN;
        fetch_http(1, url, body);
        return;
    }
    link_url(t, &c, url, sizeof url);
    if (!url[0]) { fetch_done("No address for its picture yet."); return; }
    g_fetch.stage = FETCH_SNAP;
    fetch_http(0, url, 0);
}

/* ── Connecting ──────────────────────────────────────────────────────
 *
 * One request, and it is the thing's own answer about itself: what a camera
 * running our doorbell firmware serves is a fact it will state, and asking
 * beats probing six addresses to find out. A camera that has never heard of
 * `/api/services` (any stock one) is not left out: the fall-back is to fetch
 * a picture the ordinary way, and a picture coming back is proof enough of
 * a working connection to offer the actions that need one.
 */
static void connect_start(th_t *t)
{
    if (g_fetch.req >= 0) return;
    conn_t c;
    conn_load(t->call, &c);
    char base[96];
    origin_of(t, &c, base, sizeof base);
    if (!base[0]) {
        th_cpy(g_fetch.call, t->call, sizeof g_fetch.call);
        fetch_done("Tell it where it is first: its address goes in Settings, below.");
        return;
    }
    th_cpy(c.base, base, sizeof c.base);
    conn_save(t->call, &c);                  /* what to try, before trying it */
    th_cpy(g_fetch.call, t->call, sizeof g_fetch.call);
    g_fetch.probing = 1;
    g_fetch.note[0] = 0;
    char url[240];
    th_cpy(url, base, sizeof url);
    th_cat(url, "/api/services", sizeof url);
    g_fetch.stage = FETCH_ASK;
    fetch_http(0, url, 0);
}

/* The same fetch as a picture, but it is the proof a Connect is waiting for. */
static void fetch_start_probe(th_t *t)
{
    fetch_start(t);
    if (g_fetch.req >= 0) g_fetch.probing = 1;
}

/* Write down what it answered. What it means is said once, by the connection
 * line on the screen: saying it again under the picture was the same sentence
 * twice, one above the other. */
static void connected(th_t *t, const char *svc)
{
    conn_t c;
    conn_load(t->call, &c);
    th_cpy(c.svc, svc, sizeof c.svc);
    c.at = hal_time_epoch();
    conn_save(t->call, &c);
    /* What it can do is evidence like any other: the same bits a wire would
     * have set, set by an answer instead (EV[] reads both the same way). */
    if (svc_has(&c, "snapshot")) learn(t, "s:snapshot");
    if (svc_has(&c, "stream")) learn(t, "s:stream");
    fetch_done(0);
    g_fetch.note[0] = 0;
    /* Connecting is what unlocks the actions below it. Waiting out the
     * two-second redraw throttle to show them is two seconds of a person
     * wondering whether the button worked. */
    draw(1);
}

/* The body of the finished request, into g_pic. Returns its length; 0 and a
 * note when there is nothing usable. */
static unsigned fetch_body(void)
{
    unsigned len = 0;
    for (;;) {
        int n = hal_http_read_response(g_fetch.req, (char *)g_pic + len,
                                       PIC_MAX - len);
        if (n <= 0) break;
        len += (unsigned)n;
        if (len >= PIC_MAX) {
            /* More than the bound: the camera was asked for its big stream.
             * Say so rather than showing the top of a picture. */
            fetch_done("That picture is bigger than a megabyte, which is more "
                      "than this screen will hold.");
            return 0;
        }
    }
    return len;
}

/* Put what is in g_pic on the screen. 0 when it would not fit, which the
 * caller says in its own words: a still and a live frame fail differently. */
static int paint_picture(unsigned len)
{
    /* data: URI, built in one buffer: prefix, the base64, then the tail. */
    th_cpy(g_msg, "{\"type\":\"ui.field.set\",\"field\":\"th_pic\",\"value\":\"data:image/jpeg;base64,", sizeof g_msg);
    unsigned at = th_len(g_msg);
    unsigned n = th_b64(g_pic, len, g_msg + at, (unsigned)sizeof g_msg - at - 8);
    if (!n) return 0;
    th_cat(g_msg, "\"}", sizeof g_msg);
    say(g_msg);
    g_fetch.pic_ms = hal_time_ms();
    return 1;
}

static void show_picture(unsigned len)
{
    if (!paint_picture(len)) { fetch_done("The picture did not fit."); return; }
    char l[80] = "";
    th_cat(l, "Taken just now, ", sizeof l);
    th_cat_u(l, len / 1024, sizeof l);
    th_cat(l, " KB.", sizeof l);
    fetch_done(l);
}

/* Called from the tick and from every event: poll, and take the next step. */
static void fetch_pump(void)
{
    if (g_fetch.req < 0) return;
    int rc = hal_http_poll(g_fetch.req);
    if (rc == 0) {
        if (hal_time_ms() - g_fetch.asked_ms > 35000ULL)
            fetch_done("It did not answer.");
        return;
    }
    if (rc < 0) { fetch_done("Could not reach it."); return; }
    int code = hal_http_status(g_fetch.req);
    if (code < 200 || code >= 300) {
        /* A camera that never heard of the question answers 404, which is an
         * answer: ask it for a picture instead. */
        if (g_fetch.stage == FETCH_ASK) {
            th_t *t = find(g_fetch.call);
            hal_http_free(g_fetch.req);
            g_fetch.req = -1;
            g_fetch.stage = FETCH_IDLE;
            if (t) { fetch_start_probe(t); return; }
            fetch_done("It would not say what it offers.");
            return;
        }
        char l[80] = "It answered ";
        th_cat_u(l, (unsigned long long)(code < 0 ? 0 : code), sizeof l);
        th_cat(l, ".", sizeof l);
        fetch_done(l);
        return;
    }
    unsigned len = fetch_body();
    if (!len) { if (g_fetch.req >= 0) fetch_done("It sent nothing."); return; }

    if (g_fetch.stage == FETCH_ASK) {
        /* {"ok":true,"api":["services","snapshot","stream"],"callsign":"X4.."} */
        g_pic[len < PIC_MAX ? len : PIC_MAX - 1] = 0;
        const char *body = (const char *)g_pic;
        th_t *t = find(g_fetch.call);
        char ok[8] = "", said[CALL_MAX] = "";
        th_json(body, "ok", ok, sizeof ok);
        th_json(body, "callsign", said, sizeof said);
        if (!t) { fetch_done(0); return; }
        if (th_eq(ok, "true")) {
            /* The wrong address answers too. A thing that names a callsign
             * that is not this one is somebody else's camera, and connecting
             * to it would put a stranger's doorstep on this screen under this
             * device's name. */
            if (said[0] && !th_eq(said, t->call)) {
                char l[120] = "That address answers as ";
                th_cat(l, said, sizeof l);
                th_cat(l, ", not ", sizeof l);
                th_cat(l, t->call, sizeof l);
                th_cat(l, ".", sizeof l);
                fetch_done(l);
                return;
            }
            char svc[48] = "", one[24];
            const char *e = th_json_arr(body, "api");
            while (e && (e = th_json_next_str(e, one, sizeof one)) != 0) {
                if (!one[0]) continue;
                if (svc[0]) th_cat(svc, ",", sizeof svc);
                th_cat(svc, one, sizeof svc);
            }
            if (!svc[0]) th_cpy(svc, "snapshot", sizeof svc);
            connected(t, svc);
            return;
        }
        /* Not ours, or not answering that question: try for a picture, which
         * is the only thing a stock camera can be asked without a vendor's
         * own protocol. */
        hal_http_free(g_fetch.req);
        g_fetch.req = -1;
        fetch_start_probe(t);
        return;
    }

    if (g_fetch.stage == FETCH_LOGIN) {
        /* [{"cmd":"Login","code":0,"value":{"Token":{"leaseTime":3600,
         *   "name":"..."}}}] -- the only "name" in it is the token's. */
        g_pic[len < PIC_MAX ? len : PIC_MAX - 1] = 0;
        const char *tok = (const char *)g_pic;
        for (const char *p = tok; *p; p++)
            if (th_starts(p, "\"Token\"")) { tok = p; break; }
        char name[80];
        if (!th_json(tok, "name", name, sizeof name) || !name[0]) {
            fetch_done("It would not log this phone in.");
            return;
        }
        th_cpy(g_fetch.token, name, sizeof g_fetch.token);
        g_fetch.token_ms = hal_time_ms();
        hal_http_free(g_fetch.req);
        g_fetch.req = -1;
        th_t *t = find(g_fetch.call);
        conn_t c;
        conn_load(g_fetch.call, &c);
        char url[240];
        reolink_snap_url(&c, g_fetch.token, url, sizeof url);
        g_fetch.stage = FETCH_SNAP;
        if (t) fetch_http(0, url, 0);
        return;
    }

    /* A still is a JPEG: FF D8. Anything else is the camera talking back
     * (an error page, a JSON refusal), and showing it as a picture would be
     * a broken box where an answer should be. */
    if (len < 4 || g_pic[0] != 0xFF || g_pic[1] != 0xD8) {
        fetch_done("What came back was not a picture.");
        return;
    }
    if (g_fetch.probing) {
        th_t *t = find(g_fetch.call);
        g_fetch.probing = 0;
        if (t) {
            paint_picture(len);           /* the proof, on the screen */
            connected(t, "snapshot");
            return;
        }
    }
    show_picture(len);
}

/* ── Watching it ─────────────────────────────────────────────────────── */
static void live_stop(const char *why)
{
    if (g_live.sock >= 0) hal_socket_close(g_live.sock);
    g_live.sock = -1;
    g_live.stage = LIVE_OFF;
    g_live.in_frame = 0;
    g_live.len = 0;
    if (why) {
        th_cpy(g_live.note, why, sizeof g_live.note);
        g_live.note_ms = hal_time_ms();
    }
    g_dirty = 1;
}

/* What to say under a live picture: how many frames, and from where. */
static void live_said(void)
{
    char l[180] = "Live: ";
    th_cat_u(l, g_live.frames, sizeof l);
    th_cat(l, g_live.frames == 1 ? " picture" : " pictures", sizeof l);
    if (g_live.last_len) {
        th_cat(l, ", ", sizeof l);
        th_cat_u(l, g_live.last_len / 1024, sizeof l);
        th_cat(l, " KB each", sizeof l);
    }
    if (g_live.last_len > LIVE_BIG)
        th_cat(l, ", about one every two seconds: they are too big to come "
                  "faster", sizeof l);
    if (g_live.stage == LIVE_STILLS)
        th_cat(l, ", one after another (it has no stream)", sizeof l);
    if (g_live.dropped) {
        th_cat(l, ", ", sizeof l);
        th_cat_u(l, g_live.dropped, sizeof l);
        th_cat(l, " too big to show", sizeof l);
    }
    th_cat(l, ".", sizeof l);
    th_cpy(g_live.note, l, sizeof g_live.note);
    g_live.note_ms = hal_time_ms();
}

/* One complete JPEG out of the stream. */
static void live_frame(unsigned len)
{
    unsigned long long now = hal_time_ms();
    g_live.frames++;
    g_live.last_len = len;
    /* However fast the camera sends, the screen is asked to change at a rate
     * a person can see AND this device can pay for: a frame arriving inside
     * the gap is counted and dropped without being base64'd or decoded, and
     * the gap is longer for the big frames a 5 MP camera sends
     * (performance.md 8.14's cost is the host's, per picture). */
    unsigned long long gap = len > LIVE_BIG ? LIVE_BIG_MS : LIVE_GAP_MS;
    if (g_live.shown_ms && now - g_live.shown_ms < gap) return;
    if (!paint_picture(len)) { g_live.dropped++; return; }
    g_live.shown_ms = now;
    live_said();
    g_dirty = 1;
}

/* Cut frames out of whatever arrived. Multipart boundaries and their headers
 * are skipped by construction: a frame is what lies between FF D8 and FF D9,
 * and anything else in the stream is not copied anywhere. */
static void live_feed(const unsigned char *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned char ch = b[i];
        if (!g_live.in_frame) {
            if (g_live.len == 0) {
                if (ch == 0xFF) { g_pic[0] = ch; g_live.len = 1; }
            } else if (ch == 0xD8) {
                g_pic[1] = ch;
                g_live.len = 2;
                g_live.in_frame = 1;
            } else if (ch == 0xFF) {
                g_live.len = 1;
            } else {
                g_live.len = 0;
            }
            continue;
        }
        if (g_live.len >= PIC_MAX) {        /* bigger than the bound: let it go */
            g_live.in_frame = 0;
            g_live.len = 0;
            g_live.dropped++;
            continue;
        }
        g_pic[g_live.len++] = ch;
        if (ch == 0xD9 && g_pic[g_live.len - 2] == 0xFF) {
            unsigned len = g_live.len;
            g_live.in_frame = 0;
            g_live.len = 0;
            live_frame(len);
        }
    }
}

static void live_start(th_t *t)
{
    conn_t c;
    conn_load(t->call, &c);
    if (!c.at) {
        th_cpy(g_live.note, "Connect to it first.", sizeof g_live.note);
    g_live.note_ms = hal_time_ms();
        g_dirty = 1;
        return;
    }
    th_cpy(g_live.call, t->call, sizeof g_live.call);
    th_cpy(g_fetch.call, t->call, sizeof g_fetch.call);   /* whose picture */
    g_live.frames = g_live.dropped = 0;
    g_live.len = 0;
    g_live.in_frame = 0;
    g_live.shown_ms = 0;
    g_live.began_ms = g_live.byte_ms = hal_time_ms();
    if (svc_has(&c, "stream") && c.base[0]) {
        char host[64];
        int port = 80;
        if (!split_origin(c.base, host, sizeof host, &port)) {
            live_stop("That address cannot be reached.");
            return;
        }
        int s = hal_socket_open(host, th_len(host), port);
        if (s < 0) { live_stop("This device would not open a connection."); return; }
        g_live.sock = s;
        g_live.stage = LIVE_DIAL;
        th_cpy(g_live.note, "Opening a connection to it...", sizeof g_live.note);
    g_live.note_ms = hal_time_ms();
    } else {
        g_live.stage = LIVE_STILLS;
        th_cpy(g_live.note, "Live: one picture after another.", sizeof g_live.note);
    g_live.note_ms = hal_time_ms();
        fetch_start(t);
    }
    g_dirty = 1;
}

/* Called from the tick: the whole of the live view's clock. */
static void live_pump(void)
{
    if (g_live.stage == LIVE_OFF) return;
    /* Nobody is looking: an engine with no page has no business holding a
     * camera's connection open. */
    if (!hal_ui_attached()) { live_stop(0); return; }
    unsigned long long now = hal_time_ms();
    if (now - g_live.began_ms > LIVE_MAX_MS) {
        live_stop("Live view stops itself after three minutes. Tap Watch live again.");
        return;
    }
    th_t *t = find(g_live.call);
    if (!t) { live_stop(0); return; }

    if (g_live.stage == LIVE_STILLS) {
        /* A picture that arrived, not one that was asked for: pic_ms only
         * moves when one is on the screen. */
        if (g_fetch.pic_ms && g_fetch.pic_ms != g_live.shown_ms) {
            g_live.shown_ms = g_fetch.pic_ms;
            g_live.frames++;
            live_said();
        }
        if (g_fetch.req < 0 && now - g_fetch.pic_ms >= STILL_GAP_MS) fetch_start(t);
        return;
    }

    int st = hal_socket_status(g_live.sock);
    if (st == 2) {
        live_stop(g_live.frames ? "It closed the connection."
                                : "Nothing answered on that address.");
        return;
    }
    if (st == 0) {
        if (now - g_live.byte_ms > 15000ULL) live_stop("It did not answer.");
        return;
    }
    if (g_live.stage == LIVE_DIAL) {
        conn_t c;
        conn_load(t->call, &c);
        char host[64];
        int port = 80;
        split_origin(c.base, host, sizeof host, &port);
        /* HTTP/1.0 on purpose. Asked in 1.1, a Dart HttpServer (which is
         * what the doorbell firmware runs) answers `transfer-encoding:
         * chunked`, and a chunk header lands in the middle of a picture as
         * often as not: the frames come out with four bytes of hex length
         * spliced into them. 1.0 has no chunked encoding, so the body is the
         * multipart stream and nothing else, and every HTTP server has to
         * answer a 1.0 request. Measured against the doorbell, 2026-09-23. */
        char req[300] = "GET ";
        th_cat(req, "/door/stream.mjpeg", sizeof req);
        th_cat(req, " HTTP/1.0\r\nHost: ", sizeof req);
        th_cat(req, host, sizeof req);
        th_cat(req, "\r\nAccept: multipart/x-mixed-replace\r\n\r\n", sizeof req);
        if (hal_socket_send(g_live.sock, req, th_len(req)) < 0) {
            live_stop("The connection closed before it was asked anything.");
            return;
        }
        g_live.stage = LIVE_STREAM;
        g_live.byte_ms = now;
        th_cpy(g_live.note, "Asked it for a live view...", sizeof g_live.note);
    g_live.note_ms = hal_time_ms();
        return;
    }

    for (;;) {
        uint32_t n = hal_socket_recv(g_live.sock, (char *)g_rx, sizeof g_rx);
        if (!n) break;
        g_live.byte_ms = hal_time_ms();
        live_feed(g_rx, n);
    }
    if (hal_time_ms() - g_live.byte_ms > 20000ULL)
        live_stop(g_live.frames ? "It stopped sending pictures."
                                : "It answered, but sent no pictures.");
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

/* A details list, optionally under a heading of its own. A heading that
 * belongs to the list rather than to a group around it is one that goes
 * away with it: a group cannot be hidden, and "What it did" standing over
 * nothing is a heading that lies. */
static void details_begin_titled(const char *field, const char *title)
{
    th_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof g_out);
    th_cat(g_out, field, sizeof g_out);
    th_cat(g_out, "\",\"value\":[{\"title\":\"", sizeof g_out);
    th_jesc(g_out, title, sizeof g_out);
    th_cat(g_out, "\",\"items\":[", sizeof g_out);
}

static void details_begin(const char *field) { details_begin_titled(field, ""); }

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

/* One field, one value: the enum and string the person corrects with. */
static void say_field(const char *name, const char *value)
{
    char m[320] = "{\"type\":\"ui.field.set\",\"field\":\"";
    th_cat(m, name, sizeof m);
    th_cat(m, "\",\"value\":\"", sizeof m);
    th_jesc(m, value, sizeof m);
    th_cat(m, "\"}", sizeof m);
    say(m);
}

static void flag_hidden(const char *name, int hidden)
{
    char m[120] = "{\"type\":\"ui.field.set\",\"field\":\"";
    th_cat(m, name, sizeof m);
    th_cat(m, hidden ? "__hidden\",\"value\":true}" : "__hidden\",\"value\":false}", sizeof m);
    say(m);
}

/* What the picture is doing: asking, or why there is none. */
static void picnote(void)
{
    if (!g_sel[0] || !hal_ui_attached()) return;
    int mine = th_eq(g_fetch.call, g_sel);
    int live = g_live.note[0] && th_eq(g_live.call, g_sel) &&
               (g_live.stage != LIVE_OFF || !mine ||
                g_live.note_ms >= g_fetch.note_ms);
    int said = live || (mine && (g_fetch.req >= 0 || g_fetch.note[0]));
    flag_hidden("th_picnote", !said);
    details_begin("th_picnote");
    if (live)
        detail("Live", g_live.note);
    else if (g_fetch.req >= 0 && mine)
        detail("Picture", "Asking it now...");
    else if (g_fetch.note[0] && mine)
        detail("Picture", g_fetch.note);
    details_end();
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
    /* A doorbell's only reading is the state word, and "What it did" says it
     * with a time against it. A tile the size of a card for one word nobody
     * needed twice is the kind of thing that pushes the buttons off the
     * screen. */
    {
        const class_t *cl = class_of(t);
        int only_state = tiles <= 1 && (!tiles || th_field(rd, "state", v, sizeof v));
        flag_hidden("th_now", only_state && (cl->panels & P_EVENTS) != 0);
    }

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

    /* What it did: the state: words it aired, newest first. Re-set whole on
     * every draw, so it cannot double the way an appended log would. */
    const class_t *c = class_of(t);
    if (c->panels & P_EVENTS) {
        arch_read(t);          /* cached on g_arch_gen: one read per change */
        details_begin_titled("th_events", "What it did");
        int shown = 0;
        const int room = g_more ? 8 : 4;
        for (const char *f = t->events; *f && shown < room; ) {
            while (*f == ' ') f++;
            if (!*f) break;
            char word[40], at[24];
            unsigned k = 0;
            while (f[k] && f[k] != ',' && f[k] != ' ' && k < sizeof word - 1) { word[k] = f[k]; k++; }
            word[k] = 0;
            if (f[k] == ',') k++;
            unsigned a = 0;
            while (f[k] && f[k] != ' ' && a < sizeof at - 1) at[a++] = f[k++];
            at[a] = 0;
            f += k;
            if (!word[0]) continue;
            int pic = 0;
            unsigned wl = th_len(word);
            if (wl > 4 && th_eq(word + wl - 4, "+pic")) { word[wl - 4] = 0; pic = 1; }
            char when[48], line[80];
            unsigned long long at_s = th_num(at), now = hal_time_epoch();
            ago_words(at_s && now > at_s ? (now - at_s) * 1000ULL : 0, when, sizeof when);
            th_cpy(line, th_eq(word, "pressed") ? "Rang"
                       : th_eq(word, "motion") ? "Movement" : "Clear", sizeof line);
            if (pic) th_cat(line, ", picture", sizeof line);
            detail(when, line);
            shown++;
        }
        details_end();
    }

    /* What it can do, and who says so. A refusal is shown rather than hidden:
     * "it can, you may not" is a different thing from "it cannot". */
    details_begin("th_can");
    unsigned caps = caps_of_thing(t);
    for (int i = 0; i < NCAP; i++) {
        unsigned bit = 1u << i;
        if (!(caps & bit) && !(t->denied & bit)) continue;
        const char *why = (t->forced & bit) ? "You said so"
                        : (t->denied & bit) ? "It answered 400: it has no such state"
                        : "Heard on the air";
        detail(CAPNAME[i], why);
    }
    if (t->notmine)
        detail("Refused", "403: this station is not on its allow list");
    if (!caps && !t->denied)
        detail("Nothing yet", "It has not said what it can do");
    details_end();

    /* ── What this thing offers over the network, and only once asked ──
     *
     * The order on the screen is the order of the decision: what is known
     * about the connection, then the button that makes one, then the actions
     * that connection unlocked. Nothing below Connect exists until it has
     * answered, because an action whose address nobody has given cannot work
     * and a screen that offers it is lying. */
    int can_pic  = (c->panels & P_PICTURE) != 0;
    int can_live = (c->panels & P_LIVE) != 0;
    conn_t cn2;
    conn_load(t->call, &cn2);
    int linked = cn2.at != 0;
    int live_on = g_live.stage != LIVE_OFF && th_eq(g_live.call, t->call);
    int asking = g_fetch.req >= 0 && th_eq(g_fetch.call, t->call);

    /* One line, not four. What a person needs off this panel is whether the
     * thing is reachable and what that buys them; the address, the age of the
     * answer and the rest of it are details, and details live under Details. */
    details_begin("th_conn");
    if (!can_pic && !can_live) {
        detail("On the network",
               "Nothing to fetch: this kind of thing says what it does on the "
               "air.");
    } else if (linked) {
        char l[160] = "";
        if (svc_has(&cn2, "stream")) th_cpy(l, "a picture and a live view", sizeof l);
        else if (svc_has(&cn2, "snapshot")) th_cpy(l, "a picture", sizeof l);
        else th_cpy(l, "nothing it will name", sizeof l);
        th_cat(l, " from ", sizeof l);
        th_cat(l, cn2.base[0] ? cn2.base : "its address", sizeof l);
        if (!svc_has(&cn2, "stream") && can_live)
            th_cat(l, ". It serves no stream, so watching it is one picture "
                      "after another", sizeof l);
        detail("Connected", l);
    } else {
        char where[180] = "";
        conn_t probe;
        conn_load(t->call, &probe);
        origin_of(t, &probe, where, sizeof where);
        if (where[0]) {
            char l[200] = "Nothing is fetched until you connect. It is at ";
            th_cat(l, where, sizeof l);
            detail("Not connected", l);
        } else {
            detail("Not connected",
                   "It has published no address and none has been typed under "
                   "Details and settings.");
        }
    }
    details_end();

    flag_hidden("th_conn", !can_pic && !can_live && !linked);
    flag_hidden("connect", linked || (!can_pic && !can_live) || asking);
    flag_hidden("disconnect", !linked);
    int haspic = g_fetch.pic_ms != 0 && th_eq(g_fetch.call, t->call);
    flag_hidden("th_pic", !can_pic || !linked || !haspic);
    flag_hidden("snap", !linked || !can_pic || g_fetch.req >= 0 || live_on);
    flag_hidden("live", !linked || !can_live || live_on);
    flag_hidden("unlive", !live_on);
    picnote();

    /* The person's corrections, pushed when the screen opens and after they
     * are taken -- never on a redraw. A redraw two seconds into typing an
     * address, or one second after picking a kind, would put the stored
     * value back under the person's hands and take the choice with it
     * (found on the bench: Save then sent "work it out" every time). */
    if (!th_eq(g_fields_for, t->call)) {
        th_cpy(g_fields_for, t->call, sizeof g_fields_for);
        say_field("th_class", t->klass[0] ? t->klass : "auto");
        say_field("th_url", t->url);
        conn_t cn;
        conn_load(t->call, &cn);
        say_field("th_vendor", cn.vendor[0] ? cn.vendor : "link");
        say_field("th_host", cn.host);
        say_field("th_user", cn.user);
        say_field("th_path", cn.path);
        /* The password is never pushed back out: what is kept is sealed, and
         * a field that shows it again is a field that has unsealed it for
         * anybody looking over a shoulder. An empty box means "unchanged". */
        say_field("th_pass", "");
    }

    flag_hidden("th_more", g_more);
    flag_hidden("th_less", !g_more);
    flag_hidden("About", !g_more);
    flag_hidden("Settings", !g_more);

    flag_hidden("pin", pinned(t->call));
    flag_hidden("unpin", !pinned(t->call));
    /* A thing with no running totals has no Totals list: an empty panel
     * saying "nothing to show" is a row that says nothing. */
    flag_hidden("th_totals", !(caps & CAP_TOTALS));
    /* Telling is offered only for a thing that reports events; a meter has
     * nothing to interrupt anybody with. */
    int tellable = (c->panels & P_EVENTS) != 0;
    flag_hidden("watch", !tellable || t->watch);
    flag_hidden("unwatch", !tellable || !t->watch);
    flag_hidden("th_events", !(c->panels & P_EVENTS));
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

/* ── Being told, and telling ──────────────────────────────────────────
 *
 * A watched thing is one the person asked to hear about. The packets come
 * from the core on `xprs.observation` (one topic per 4.2 type), and the
 * subscription is held only while something is watched: a subscription is a
 * cost the whole phone pays for as long as it is held. Nothing polls, and
 * with no page open there is no clock at all.
 */
static int g_sub_obs;

static int anything_watched(void)
{
    for (int i = 0; i < g_nth; i++) if (g_th[i].watch) return 1;
    return 0;
}

static void watch_sync(void)
{
    static const char obs[] = "xprs.observation";
    int want = anything_watched();
    if (want && !g_sub_obs) {
        hal_event_subscribe(obs, sizeof obs - 1);
        g_sub_obs = 1;
    } else if (!want && g_sub_obs) {
        hal_event_unsubscribe(obs, sizeof obs - 1);
        g_sub_obs = 0;
    }
}

/* Is [a] no later than [b]? Both are 4.8 timestamps, which sort as text. */
static int ts_le(const char *a, const char *b)
{
    if (!a[0] || !b[0]) return 0;
    for (unsigned i = 0;; i++) {
        if (a[i] == b[i]) { if (!a[i]) return 1; continue; }
        return (unsigned char)a[i] < (unsigned char)b[i];
    }
}

static void tell(th_t *t, const char *word, const char *id)
{
    const char *body = th_eq(word, "pressed") ? "Someone at the door"
                     : th_eq(word, "motion")  ? "Movement seen" : 0;
    if (!body) return;                   /* `clear` ends an event, it is not news */
    char m[420] = "{\"type\":\"notify\",\"level\":\"";
    th_cat(m, th_eq(word, "pressed") ? "warning" : "info", sizeof m);
    th_cat(m, "\",\"title\":\"", sizeof m);
    th_jesc(m, t->nick[0] ? t->nick : t->call, sizeof m);
    th_cat(m, "\",\"body\":\"", sizeof m);
    th_jesc(m, body, sizeof m);
    /* The 5 identifier, so the same press heard twice over two bearers is one
     * notification even if this wapp were restarted between them. */
    th_cat(m, "\",\"tag\":\"thing.", sizeof m);
    th_jesc(m, t->call, sizeof m);
    th_cat(m, ".", sizeof m);
    th_jesc(m, word, sizeof m);
    th_cat(m, ".", sizeof m);
    th_jesc(m, id, sizeof m);
    th_cat(m, "\",\"scope\":\"both\"}", sizeof m);
    say(m);
}

/* One `t:observation` the core handed over. */
static void on_observation(void)
{
    char from[CALL_MAX];
    if (!th_json(g_ev, "from", from, sizeof from) || !from[0]) return;
    th_t *t = find(from);
    if (!t || !t->watch) return;          /* the whole cost of an unwatched packet */

    char id[28] = "", ts[24] = "", wire[300] = "";
    th_json(g_ev, "id", id, sizeof id);
    if (!th_json(g_ev, "wire", wire, sizeof wire)) return;
    th_field(wire, "ts", ts, sizeof ts);   /* the packet's own 4.8 stamp */

    learn(t, wire);
    g_dirty = 1;

    char word[16];
    if (!th_field(wire, "state", word, sizeof word)) return;
    if (!th_eq(word, "pressed") && !th_eq(word, "motion") && !th_eq(word, "clear")) return;

    /* Already told about: the same packet, or one older than the last we told
     * about (a backlog replayed at start is not news). */
    if (id[0] && th_eq(t->cur_id, id)) return;
    if (ts[0] && t->cur_ts[0] && ts_le(ts, t->cur_ts)) return;

    th_cpy(t->cur_id, id, sizeof t->cur_id);
    th_cpy(t->cur_ts, ts, sizeof t->cur_ts);
    th_cpy(t->state, word, sizeof t->state);
    t->state_ms = hal_time_epoch();
    save_watch(t);
    tell(t, word, id);
}

/* ── When to draw ─────────────────────────────────────────────────────── */
static void draw(int force)
{
    /* Nobody is looking: an engine woken by a packet with no page draws
     * nothing, and the host would drop it anyway. Telling still happens. */
    if (!hal_ui_attached()) { g_dirty = 0; return; }
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
        else if (th_eq(g_topic, "xprs.observation")) on_observation();
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
        g_fields_for[0] = 0;
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
    } else if (th_eq(cmd, "watch") || th_eq(cmd, "unwatch")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        t->watch = th_eq(cmd, "watch");
        if (t->watch) {
            /* Start from the newest report already held: the archive is full
             * of yesterday's presses and nobody wants to be told about those. */
            char q[96];
            query_from(q, sizeof q, t->call, "observation", 1);
            t->cur_ts[0] = t->cur_id[0] = 0;
            if (history(q)) {
                const char *p = th_json_top(g_big);
                if (p && th_json_next(p, g_row, sizeof g_row) &&
                    th_json(g_row, "wire", g_wire, sizeof g_wire))
                    th_field(g_wire, "ts", t->cur_ts, sizeof t->cur_ts);
            }
        }
        save_watch(t);
        watch_sync();
        draw(1);
    } else if (th_eq(cmd, "th_apply")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        char v[128];
        if (th_json(g_buf, "th_class", v, sizeof v)) {
            if (th_eq(v, "auto") || !v[0]) t->klass[0] = 0;
            else if (class_by_id(v)) th_cpy(t->klass, v, sizeof t->klass);
            else { char l[80] = "kind not recognised: "; th_cat(l, v, sizeof l); note(l); }
        }
        conn_t cn;
        conn_load(t->call, &cn);
        int conn_changed = 0;
        if (th_json(g_buf, "th_vendor", v, sizeof v) && v[0]) {
            th_cpy(cn.vendor, v, sizeof cn.vendor); conn_changed = 1;
        }
        if (th_json(g_buf, "th_host", v, sizeof v)) {
            th_cpy(cn.host, v, sizeof cn.host); conn_changed = 1;
        }
        if (th_json(g_buf, "th_user", v, sizeof v)) {
            th_cpy(cn.user, v, sizeof cn.user); conn_changed = 1;
        }
        if (th_json(g_buf, "th_path", v, sizeof v)) {
            th_cpy(cn.path, v, sizeof cn.path); conn_changed = 1;
        }
        /* An empty password box leaves the sealed one alone: the person who
         * only meant to change the address has not just wiped it. */
        if (th_json(g_buf, "th_pass", v, sizeof v) && v[0]) {
            th_cpy(cn.pass, v, sizeof cn.pass); conn_changed = 1;
            g_fetch.token[0] = 0;
        }
        if (conn_changed) {
            conn_save(t->call, &cn);
            /* An address the person gave is evidence too. */
            if (cn.host[0] || cn.path[0]) t->forced |= CAP_PICTURE;
        }
        if (th_json(g_buf, "th_url", v, sizeof v)) {
            th_cpy(t->url, v, sizeof t->url);
            /* An address the person typed is evidence like any other: it is
             * a camera whether or not it has ever aired a url:. */
            if (t->url[0]) t->forced |= CAP_PICTURE;
            else t->forced &= ~CAP_PICTURE;
        }
        save_thing(t);
        g_fields_for[0] = 0;       /* show what was taken, once */
        note("kept what you said about this thing");
        draw(1);
    } else if (th_eq(cmd, "th_more") || th_eq(cmd, "th_less")) {
        g_more = th_eq(cmd, "th_more");
        g_fields_for[0] = 0;       /* the settings fields are drawn again */
        draw(1);
    } else if (th_eq(cmd, "connect")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        connect_start(t);
        draw(1);
    } else if (th_eq(cmd, "disconnect")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        if (th_eq(g_live.call, t->call)) live_stop(0);
        conn_t c;
        conn_load(t->call, &c);
        /* What the person typed stays; what the thing answered goes. Being
         * disconnected is not having forgotten the address. */
        c.svc[0] = 0;
        c.at = 0;
        conn_save(t->call, &c);
        g_fetch.token[0] = 0;
        g_fetch.note[0] = 0;
        g_fetch.pic_ms = 0;
        say("{\"type\":\"ui.field.set\",\"field\":\"th_pic\",\"value\":\"\"}");
        note("disconnected");
        draw(1);
    } else if (th_eq(cmd, "live")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        live_start(t);
        draw(1);
    } else if (th_eq(cmd, "unlive")) {
        live_stop("Live view stopped.");
        draw(1);
    } else if (th_eq(cmd, "snap")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        fetch_start(t);
        draw(1);
    } else if (th_eq(cmd, "th_forget")) {
        if (!g_sel[0]) return;
        th_t *t = get(g_sel);
        if (!t) return;
        t->forced = t->denied = 0;
        t->klass[0] = t->url[0] = 0;
        conn_t none;
        for (unsigned i = 0; i < sizeof none; i++) ((char *)&none)[i] = 0;
        conn_save(t->call, &none);
        if (th_eq(g_live.call, t->call)) live_stop(0);
        g_fetch.token[0] = 0;
        t->notmine = 0;
        save_thing(t);
        g_fields_for[0] = 0;
        note("worked it out again from what it says");
        draw(1);
    } else if (th_eq(cmd, "back") || th_eq(cmd, "screen_closed")) {
        g_more = 0;
        /* `back` is this wapp's own button; `screen_closed` is the host
         * saying the person used the arrow on the panel's app bar. Both mean
         * nobody is looking at this thing any more, and a live view nobody is
         * looking at is a camera being read for nothing. */
        live_stop(0);
        g_sel[0] = 0;
        g_fetch.note[0] = 0;
        g_fetch.pic_ms = 0;
        say("{\"type\":\"ui.field.set\",\"field\":\"th_pic\",\"value\":\"\"}");
        say("{\"type\":\"ui.screen.close\"}");
    }
}

/* ── Entry points ─────────────────────────────────────────────────────── */
int32_t module_init(void)
{
    /* What is known about each thing outlives the page: a doorbell that rang
     * yesterday is still a doorbell with nobody looking, and being told about
     * the next ring is the one job that matters when the screen is off. */
    load_records();
    watch_sync();
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
    fetch_pump();
    live_pump();
    draw(0);
    return 0;
}

int32_t module_handle_event(void)
{
    drain_events();
    fetch_pump();
    live_pump();
    uint32_t n = hal_msg_recv(g_buf, sizeof g_buf - 1);
    if (n > 0) {
        g_buf[n] = 0;
        on_command();
    }
    draw(0);
    return 0;
}

/* The page's clock. It finishes a redraw the two-second throttle held back,
 * polls a picture that was asked for, and carries the live view: a stream of
 * pictures needs to be read off the socket several times a second or the
 * host's buffer is the only thing growing. Redraws are still throttled to
 * REDRAW_MS and the live frames to LIVE_GAP_MS, so a page that is merely
 * open costs a wake that finds nothing to do.
 *
 * With no page there is no clock at all: a watched thing costs a wake per
 * packet the core hands over, and nothing per hour (performance.md 8.4). */
#define PAGE_TICK_MS 400
int32_t module_tick_interval_ms(void)
{
    return hal_ui_attached() ? (int32_t)PAGE_TICK_MS : 0;
}

void module_destroy(void) {}
