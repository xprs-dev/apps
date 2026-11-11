/*
 * ci.c -- see ci.h.
 */
#include "ci.h"
#include "util.h"

static int is_true(const char *row, const char *key)
{
    char v[8];
    return al_json(row, key, v, sizeof v) && al_eq(v, "true");
}

static int to_int(const char *s)
{
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void cat_int(char *d, int v, unsigned cap)
{
    if (v < 0) { al_cat(d, "-", cap); v = -v; }
    al_cat_u(d, (unsigned long long)v, cap);
}

int ci_read_ask(const char *row, ci_ask_t *a)
{
    char type[16], wire[CI_WIRE_MAX + 64], ts[24], rssi[12];
    for (unsigned i = 0; i < sizeof *a; i++) ((char *)a)[i] = 0;
    if (!al_json(row, "type", type, sizeof type) || !al_eq(type, "command")) return 0;
    if (!al_json(row, "wire", wire, sizeof wire)) return 0;
    al_field(wire, "cmd", a->cmd, sizeof a->cmd);
    if (!al_eq(a->cmd, "checkin")) return 0;
    al_json(row, "id", a->id, sizeof a->id);
    al_json(row, "from", a->from, sizeof a->from);
    al_json(row, "scope", a->scope, sizeof a->scope);
    al_json(row, "sig", a->sig, sizeof a->sig);
    al_json(row, "bearer", a->bearer, sizeof a->bearer);
    al_json(row, "via", a->via, sizeof a->via);
    if (al_json(row, "rssi", rssi, sizeof rssi)) a->rssi = to_int(rssi);
    a->in_range = is_true(row, "inRange");
    a->for_us = is_true(row, "forUs");
    if (al_json(row, "ts", ts, sizeof ts)) a->ts = al_epoch(ts);
    return 1;
}

void ci_link_of(const char *bearer, char *out, unsigned cap)
{
    static const char *const ok[] = {
        "ble", "lan", "wifi", "espnow", "lora", "halow",
        "vhf", "uhf", "hf", "cb", "pmr"
    };
    out[0] = 0;
    if (al_eq(bearer, "ble5")) { al_cpy(out, "ble", cap); return; }
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (al_eq(bearer, ok[i])) { al_cpy(out, ok[i], cap); return; }
}

static int is_local_link(const char *link)
{
    return al_eq(link, "ble") || al_eq(link, "lan") ||
           al_eq(link, "wifi") || al_eq(link, "espnow");
}

const char *ci_grade(const char *link, const char *route)
{
    if (route && route[0]) return "relayed";
    if (is_local_link(link)) return "local";
    return "regional";
}

int ci_decide(const ci_ask_t *a, const ci_rules_t *r,
              unsigned long long now, int checked_today)
{
    char link[12];
    /* A certificate is the station's word that THIS key asked. A signature
     * we cannot check yet (we have never heard the asker's key) gets no
     * answer: the core has gone to ask for the key, and the asker's courier
     * airs the same ask again within the minute, by when it checks. Any
     * other signature that does not check proves nobody. */
    if (al_eq(a->sig, "unverified")) return CI_WAIT;
    if (!al_eq(a->sig, "verified")) return CI_REFUSED;
    /* Heard outside its own scope's reach: over the internet, say. */
    if (!a->in_range) return CI_REFUSED;
    if (!al_eq(a->scope, "local") && !al_eq(a->scope, "regional")) return CI_REFUSED;
    if (r && r->local_only && !al_eq(a->scope, "local")) return CI_REFUSED;
    ci_link_of(a->bearer, link, sizeof link);
    if (!link[0]) return CI_REFUSED;
    if (!a->ts) return CI_STALE;
    if (now) {
        if (a->ts + CI_FRESH_S < now) return CI_STALE;
        if (a->ts > now + CI_SKEW_S) return CI_STALE;
    }
    if (checked_today) return CI_AGAIN_LATER;
    return CI_OK;
}

int ci_build_ask(char *out, unsigned cap, const char *me, const char *station,
                 unsigned long long now, const char *scope)
{
    char ts[24];
    if (!me[0] || !station[0]) return -1;
    al_stamp(ts, sizeof ts, now);
    out[0] = 0;
    al_cat(out, "t:command f:", cap);
    al_cat(out, me, cap);
    al_cat(out, " d:", cap);
    al_cat(out, station, cap);
    al_cat(out, " ts:", cap);
    al_cat(out, ts, cap);
    al_cat(out, " cmd:checkin scope:", cap);
    al_cat(out, al_eq(scope, "regional") ? "regional" : "local", cap);
    unsigned n = al_len(out);
    return n > CI_WIRE_UNSIGNED || n + 1 >= cap ? -1 : (int)n;
}

/* Append " key:value" when it still fits signed. */
static void opt(char *out, unsigned cap, const char *key, const char *value)
{
    if (!value || !value[0]) return;
    if (al_len(out) + 2 + al_len(key) + al_len(value) > CI_WIRE_UNSIGNED) return;
    al_cat(out, " ", cap);
    al_cat(out, key, cap);
    al_cat(out, ":", cap);
    al_cat(out, value, cap);
}

int ci_build_result(char *out, unsigned cap, const char *me, const ci_ask_t *a,
                    unsigned long long now, int code, const char *file,
                    const char *title, const char *pos)
{
    char ts[24], link[12];
    if (!me[0] || !a->from[0] || !a->id[0]) return -1;
    al_stamp(ts, sizeof ts, now);
    out[0] = 0;
    al_cat(out, "t:result f:", cap);
    al_cat(out, me, cap);
    al_cat(out, " d:", cap);
    al_cat(out, a->from, cap);
    al_cat(out, " ts:", cap);
    al_cat(out, ts, cap);
    al_cat(out, " r:", cap);
    al_cat(out, a->id, cap);
    al_cat(out, " code:", cap);
    al_cat_u(out, (unsigned long long)code, cap);
    /* The answer goes back as far as the ask came, and no further. An ask
     * with a wider scope is refused, and its refusal carries none, so it can
     * reach the asker the way the ask did. */
    if (al_eq(a->scope, "local") || al_eq(a->scope, "regional")) {
        al_cat(out, " scope:", cap);
        al_cat(out, a->scope, cap);
    }
    if (al_len(out) > CI_WIRE_UNSIGNED) return -1;
    if (code == CI_OK) {
        /* A check-in is a meeting at a PLACE: without the station's position
         * there is no certificate (ci.h). The radio and the place go first,
         * then the badge, the event, the relays and the signal while they
         * still fit signed. */
        char rssi[16] = "", ref[64];
        if (!ci_pos_ok(pos)) return -1;
        ci_link_of(a->bearer, link, sizeof link);
        ci_ref_of(file ? file : "", ref, sizeof ref);
        opt(out, cap, "link", link);
        opt(out, cap, "pos", pos);
        if (!al_field(out, "pos", rssi, sizeof rssi)) return -1;
        /* The relays say how close the meeting really was (ci_grade): a
         * relayed check-in that cannot say so is not certified at all. */
        if (a->via[0]) {
            opt(out, cap, "route", a->via);
            if (!al_field(out, "route", rssi, sizeof rssi)) return -1;
        }
        rssi[0] = 0;
        opt(out, cap, "file", ref);
        opt(out, cap, "title", title);
        if (a->rssi) { cat_int(rssi, a->rssi, sizeof rssi); al_cat(rssi, "dBm", sizeof rssi); }
        opt(out, cap, "rssi", rssi);
    }
    return (int)al_len(out);
}

int ci_read_cert(const char *wire, ci_cert_t *c)
{
    char t[12], code[8], ts[24];
    for (unsigned i = 0; i < sizeof *c; i++) ((char *)c)[i] = 0;
    if (!al_field(wire, "t", t, sizeof t) || !al_eq(t, "result")) return 0;
    al_field(wire, "f", c->station, sizeof c->station);
    al_field(wire, "d", c->to, sizeof c->to);
    al_field(wire, "r", c->r, sizeof c->r);
    if (al_field(wire, "code", code, sizeof code)) c->code = to_int(code);
    al_field(wire, "scope", c->scope, sizeof c->scope);
    al_field(wire, "link", c->link, sizeof c->link);
    al_field(wire, "rssi", c->rssi, sizeof c->rssi);
    al_field(wire, "route", c->route, sizeof c->route);
    al_field(wire, "pos", c->pos, sizeof c->pos);
    al_field(wire, "title", c->title, sizeof c->title);
    al_field(wire, "file", c->file, sizeof c->file);
    if (al_field(wire, "ts", ts, sizeof ts)) c->ts = al_epoch(ts);
    return 1;
}

unsigned long long ci_day(unsigned long long epoch, int offset_min)
{
    long long t = (long long)epoch + (long long)offset_min * 60;
    if (t < 0) t = 0;
    return (unsigned long long)t / 86400ULL;
}

void ci_label(char *out, unsigned cap, const char *text)
{
    unsigned o = 0;
    int dash = 0;
    for (const char *p = text; p && *p && o + 1 < cap; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c;
            dash = 0;
        } else if ((c == ' ' || c == '-' || c == '_') && o && !dash) {
            out[o++] = '-';
            dash = 1;
        }
    }
    while (o && out[o - 1] == '-') o--;
    out[o] = 0;
}

void ci_ref_of(const char *token, char *out, unsigned cap)
{
    out[0] = 0;
    if (!token) return;
    if (al_starts(token, "file:")) token += 5;
    int dot = 0;
    for (const char *p = token; *p; p++) {
        if (*p == ' ') return;
        if (*p == '.') dot = 1;
    }
    if (!dot || al_len(token) + 1 > cap) return;
    al_cpy(out, token, cap);
}

int ci_build_event(char *out, unsigned cap, const char *me,
                   unsigned long long now, const char *pos, const char *title,
                   unsigned long long since, unsigned long long until,
                   const char *file, const char *m)
{
    char ts[24], s[24], u[24], label[32], ref[64];
    ci_label(label, sizeof label, title);
    if (!me[0] || !label[0] || until <= since) return -1;
    ci_ref_of(file ? file : "", ref, sizeof ref);
    al_stamp(ts, sizeof ts, now);
    al_stamp(s, sizeof s, since);
    al_stamp(u, sizeof u, until);
    out[0] = 0;
    al_cat(out, "t:event f:", cap);
    al_cat(out, me, cap);
    al_cat(out, " ts:", cap);
    al_cat(out, ts, cap);
    al_cat(out, " title:", cap);
    al_cat(out, label, cap);
    al_cat(out, " tag:checkin", cap);
    /* An event that starts when it is announced says so by its ts:. */
    if (since != now) opt(out, cap, "since", s);
    al_cat(out, " until:", cap);
    al_cat(out, u, cap);
    if (al_len(out) > CI_WIRE_UNSIGNED) return -1;
    opt(out, cap, "file", ref);
    opt(out, cap, "pos", pos);
    unsigned n = al_len(out);
    if (m && m[0] && n + 4 <= CI_WIRE_UNSIGNED) {
        /* m: runs to the end; cut it to what is left, at a space. */
        unsigned room = CI_WIRE_UNSIGNED - n - 3, k = al_len(m);
        if (k > room) {
            k = room;
            while (k && m[k] != ' ') k--;
            if (!k) k = room;
        }
        al_cat(out, " m:", cap);
        unsigned l = al_len(out);
        for (unsigned i = 0; i < k && l + 1 < cap; i++) out[l++] = m[i];
        out[l] = 0;
    }
    return (int)al_len(out);
}

int ci_build_badge(char *out, unsigned cap, const char *me,
                   unsigned long long now, const char *ref,
                   unsigned long long size)
{
    char ts[24], r[64];
    ci_ref_of(ref, r, sizeof r);
    if (!me[0] || !r[0]) return -1;
    al_stamp(ts, sizeof ts, now);
    out[0] = 0;
    al_cat(out, "t:file f:", cap);
    al_cat(out, me, cap);
    al_cat(out, " ts:", cap);
    al_cat(out, ts, cap);
    al_cat(out, " file:", cap);
    al_cat(out, r, cap);
    if (size) { al_cat(out, " size:", cap); al_cat_u(out, size, cap); al_cat(out, "B", cap); }
    al_cat(out, " m:Check-in badge", cap);
    unsigned n = al_len(out);
    return n > CI_WIRE_UNSIGNED ? -1 : (int)n;
}

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

int ci_pos_parse(const char *pos, long *lat7, long *lon7)
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

int ci_pos_ok(const char *pos) { return ci_pos_parse(pos, 0, 0); }

/* cos(x) for |x| <= pi/2 (a latitude), by its series: no libm in a wapp. */
static double cos_lat(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 + x2 * x2 / 24 - x2 * x2 * x2 / 720 + x2 * x2 * x2 * x2 / 40320;
}

long ci_dist_m(const char *a, const char *b)
{
    long la1, lo1, la2, lo2;
    if (!ci_pos_parse(a, &la1, &lo1) || !ci_pos_parse(b, &la2, &lo2)) return -1;
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

void ci_dist_text(long m, char *out, unsigned cap)
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

int ci_pos_norm(const char *in, char *out, unsigned cap)
{
    long la, lo;
    out[0] = 0;
    if (!ci_pos_parse(in, &la, &lo)) return 0;
    deg4(out, cap, la);
    al_cat(out, ",", cap);
    deg4(out, cap, lo);
    return 1;
}
