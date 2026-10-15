/*
 * alert.c -- see alert.h.
 */
#include "alert.h"
#include "util.h"

unsigned long long al_duration(const char *n)
{
    static const struct { const char *name; unsigned long long s; } d[] = {
        { "1d", 86400ULL },        { "2d", 2ULL * 86400ULL },
        { "1w", 7ULL * 86400ULL }, { "1m", 30ULL * 86400ULL },
        { "6m", 182ULL * 86400ULL }, { "1y", 365ULL * 86400ULL },
    };
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++)
        if (al_eq(n, d[i].name)) return d[i].s;
    return 0;
}

static int one_of(const char *w, const char *const *list)
{
    for (; *list; list++) if (al_eq(w, *list)) return 1;
    return 0;
}

int al_kind_ok(const char *type, const char *kind)
{
    static const char *const warning[] = {
        "fire", "flood", "storm", "wind", "snow", "ice", "quake", "tsunami",
        "landslide", "chemical", "radiation", "outage", "road", "crowd",
        "animal", "other", 0 };
    static const char *const info[] = {
        "traffic", "stopped", "slow", "works", "closure", "rain", "snow",
        "ice", "fog", "wind", "debris", "animal", "crowd", "event", "other", 0 };
    if (al_eq(type, "warning")) return one_of(kind, warning);
    if (al_eq(type, "info")) return one_of(kind, info);
    return 0;
}

int al_sev_ok(const char *sev)
{
    static const char *const s[] = { "watch", "warning", "danger", 0 };
    return one_of(sev, s);
}

/* No newline and no other control character: a field ends at a space and
 * m: at the end of the packet, and a byte below 0x20 is neither. */
static int clean(const char *s)
{
    for (; s && *s; s++) if ((unsigned char)*s < 0x20) return 0;
    return 1;
}

static int pos_ok(const char *v)
{
    for (; *v; v++)
        if (!((*v >= '0' && *v <= '9') || *v == '.' || *v == ',' || *v == '-'))
            return 0;
    return 1;
}

static int rad_ok(const char *v)
{
    unsigned n = 0;
    while (v[n] >= '0' && v[n] <= '9') n++;
    if (!n) return 0;
    return al_eq(v + n, "m") || al_eq(v + n, "km");
}

static void head(char *out, unsigned cap, const char *type, const char *me)
{
    al_cpy(out, "t:", cap);
    al_cat(out, type, cap);
    al_cat(out, " f:", cap);
    al_cat(out, me, cap);
}

static void stamps(char *out, unsigned cap, unsigned long long now,
                   unsigned long long until)
{
    char ts[24];
    al_cat(out, " scope:local ts:", cap);
    al_stamp(ts, sizeof ts, now);
    al_cat(out, ts, cap);
    al_cat(out, " until:", cap);
    al_stamp(ts, sizeof ts, until);
    al_cat(out, ts, cap);
}

int al_build(char *out, unsigned cap, const char *me, const char *type,
             const char *kind, const char *sev, const char *pos,
             const char *rad, unsigned long long now,
             unsigned long long until, const char *m)
{
    if (cap) out[0] = 0;
    if (!al_kind_ok(type, kind)) return -2;
    int warn = al_eq(type, "warning");
    if (warn && !al_sev_ok(sev)) return -2;
    if (!me || !me[0] || !now || until <= now) return -2;
    if (!clean(m)) return -2;
    if (pos && pos[0] && !pos_ok(pos)) return -2;
    if (rad && rad[0] && (!pos || !pos[0] || !rad_ok(rad))) return -2;
    char w[AL_WIRE_MAX + 64];
    head(w, sizeof w, type, me);
    if (pos && pos[0]) {
        al_cat(w, " pos:", sizeof w);
        al_cat(w, pos, sizeof w);
        if (rad && rad[0]) { al_cat(w, " rad:", sizeof w); al_cat(w, rad, sizeof w); }
    }
    al_cat(w, " kind:", sizeof w);
    al_cat(w, kind, sizeof w);
    if (warn) { al_cat(w, " sev:", sizeof w); al_cat(w, sev, sizeof w); }
    stamps(w, sizeof w, now, until);
    /* m: is last and runs to the end (section 4); an empty one is left out
     * rather than sent as "m:". */
    unsigned ml = al_len(m);
    if (ml) {
        if (al_len(w) + 3 + ml > AL_WIRE_UNSIGNED) return -1;
        al_cat(w, " m:", sizeof w);
        al_cat(w, m, sizeof w);
    }
    unsigned n = al_len(w);
    if (n > AL_WIRE_UNSIGNED || n + 1 > cap) return -1;
    al_cpy(out, w, cap);
    return (int)n;
}

int al_build_withdraw(char *out, unsigned cap, const char *me,
                      const char *type, const char *id,
                      unsigned long long now, unsigned long long until)
{
    if (cap) out[0] = 0;
    if (!al_eq(type, "warning") && !al_eq(type, "info")) return -2;
    if (al_len(id) != 6 || !me || !me[0] || !now) return -2;
    char w[AL_WIRE_MAX + 64];
    head(w, sizeof w, type, me);
    /* The alert's own until:, so a station keeps this exactly as long as
     * the alert (25.3). An alert that named none gets a day from now. */
    stamps(w, sizeof w, now, until ? until : now + AL_DEFAULT_LIFE);
    al_cat(w, " r:", sizeof w);
    al_cat(w, id, sizeof w);
    al_cat(w, " remove:", sizeof w);
    al_cat(w, type, sizeof w);
    unsigned n = al_len(w);
    if (n > AL_WIRE_UNSIGNED || n + 1 > cap) return -1;
    al_cpy(out, w, cap);
    return (int)n;
}

int al_parse(const char *wire, al_alert_t *a)
{
    for (unsigned i = 0; i < sizeof *a; i++) ((char *)a)[i] = 0;
    char v[24];
    if (al_starts(wire, "t:warning ")) al_cpy(a->type, "warning", sizeof a->type);
    else if (al_starts(wire, "t:info ")) al_cpy(a->type, "info", sizeof a->type);
    else return 0;
    if (!al_field(wire, "f", a->from, sizeof a->from) || !a->from[0]) return 0;
    if (al_field(wire, "ts", v, sizeof v)) a->ts = al_epoch(v);
    if (al_field(wire, "until", v, sizeof v)) a->until = al_epoch(v);
    if (al_field(wire, "remove", v, sizeof v)) {
        if (!al_field(wire, "r", a->id, sizeof a->id) || al_len(a->id) != 6) return 0;
        return 2;
    }
    /* The id is the caller's: a heard wire carries sig: and maybe via:,
     * which section 5 leaves out, and the host hands the id with it. */
    if (al_field(wire, "since", v, sizeof v)) a->since = al_epoch(v);
    al_field(wire, "kind", a->kind, sizeof a->kind);
    al_field(wire, "sev", a->sev, sizeof a->sev);
    al_field(wire, "pos", a->pos, sizeof a->pos);
    al_field(wire, "rad", a->rad, sizeof a->rad);
    al_field(wire, "scope", a->scope, sizeof a->scope);
    al_field(wire, "m", a->m, sizeof a->m);
    return 1;
}

int al_state(const al_alert_t *a, unsigned long long now)
{
    if (a->withdrawn) return AL_GONE;
    if (now) {
        unsigned long long end = a->until ? a->until
                               : (a->ts ? a->ts + AL_DEFAULT_LIFE : 0);
        if (end && now > end) return AL_GONE;
        /* 25.1: not shown as current before its since:. */
        if (a->since && now < a->since) return AL_PLANNED;
    }
    return AL_NOW;
}

void al_left(char *out, unsigned cap, unsigned long long now,
             unsigned long long until)
{
    if (cap) out[0] = 0;
    if (!until || !now) return;
    if (now >= until) { al_cpy(out, "ended", cap); return; }
    unsigned long long s = until - now;
    unsigned long long n;
    const char *unit;
    if (s >= 2ULL * 86400ULL) { n = s / 86400ULL; unit = " days left"; }
    else if (s >= 2ULL * 3600ULL) { n = s / 3600ULL; unit = " hours left"; }
    else if (s >= 120ULL) { n = s / 60ULL; unit = " minutes left"; }
    else { al_cpy(out, "ending now", cap); return; }
    al_cat_u(out, n, cap);
    al_cat(out, unit, cap);
}

void al_deg(char *out, unsigned cap, long v)
{
    if (cap) out[0] = 0;
    unsigned long long a;
    if (v < 0) { al_cat(out, "-", cap); a = (unsigned long long)(-(long long)v); }
    else a = (unsigned long long)v;
    /* Round to four places: 1e7 per degree, so 1000 per step. */
    a = (a + 500ULL) / 1000ULL;
    al_cat_u(out, a / 10000ULL, cap);
    al_cat(out, ".", cap);
    char f[8];
    unsigned long long r = a % 10000ULL;
    for (int i = 3; i >= 0; i--) { f[i] = (char)('0' + r % 10); r /= 10; }
    f[4] = 0;
    al_cat(out, f, cap);
}
