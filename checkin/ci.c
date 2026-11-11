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
    /* A certificate is the station's word that THIS key asked. An ask whose
     * signature does not check against a key we hold proves nobody. */
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
        char rssi[16] = "", ref[64];
        ci_link_of(a->bearer, link, sizeof link);
        ci_ref_of(file ? file : "", ref, sizeof ref);
        opt(out, cap, "link", link);
        opt(out, cap, "file", ref);
        opt(out, cap, "title", title);
        opt(out, cap, "route", a->via);
        if (a->rssi) { cat_int(rssi, a->rssi, sizeof rssi); al_cat(rssi, "dBm", sizeof rssi); }
        opt(out, cap, "rssi", rssi);
        opt(out, cap, "pos", pos);
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
