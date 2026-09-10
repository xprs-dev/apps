/*
 * social — the XPRS feed. Everything you read here was heard on the air.
 *
 *   Activity  ($type:"chat")    t:status packets (section 27) this station
 *                               heard, plus the ones it sent
 *   Following ($type:"people")  callsigns you follow; the Following tab is
 *                               this feed narrowed to them
 *   Search                      the local spool, by words and by callsign
 *
 * There is NO internet here and no NOSTR. A post becomes a `t:status` on
 * every bearer the core has active, and the feed is built from what the
 * station archived (`hal_xprs_history`). The wapp supplies words and reads
 * rows; it does not know or choose how bytes travel.
 *
 * Two consequences worth stating, because they are design and not omission:
 *
 *  - A like is a t:reaction (section 6.5) and a reply is a status carrying
 *    r: (section 27) — both real packets, aired here. A repost has no XPRS
 *    packet, so that command is answered with a note in the log rather than
 *    pretending to work.
 *  - Identity is a callsign, not an npub. A callsign is a label (section 3),
 *    so the `sig` the archive verified travels with every post and the feed
 *    says which ones are signed.
 *
 * Build: cd wapps/social && WASI_SDK_PATH=~/wasi-sdk make
 */
#include "../hal/xprs_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

static int json_raw(const char *json, const char *key, char *out, unsigned m) {
    char pat[48];
    str_copy(pat, "\"", sizeof(pat));
    str_cat(pat, key, sizeof(pat));
    str_cat(pat, "\":", sizeof(pat));
    unsigned pl = str_len(pat);
    for (const char *p = json; *p; p++) {
        unsigned i = 0;
        while (i < pl && p[i] == pat[i]) i++;
        if (i != pl) continue;
        p += pl;
        unsigned o = 0;
        int instr = 0;
        if (*p == '"') { instr = 1; p++; }
        while (*p && o < m - 1) {
            if (instr) {
                if (*p == '\\' && p[1]) { out[o++] = *p++; if (o < m - 1) out[o++] = *p++; continue; }
                if (*p == '"') break;
            } else if (*p == ',' || *p == '}' || *p == ']') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char uc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lc(*a) == lc(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void fmt_hhmm(const char *unix_s, char *out) {
    long v = 0;
    for (const char *p = unix_s; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    int hh = (int)((v / 3600) % 24), mm = (int)((v / 60) % 60);
    out[0] = '0' + hh / 10; out[1] = '0' + hh % 10; out[2] = ':';
    out[3] = '0' + mm / 10; out[4] = '0' + mm % 10; out[5] = '\0';
}

/* Append the display-time fields the host feed wants: "time" (HH:MM clock) +
 * "t" (absolute epoch, MILLISECONDS) so older posts get a date prefix. */
static void cat_time_fields(char *dst, const char *ts, unsigned m) {
    str_cat(dst, "\"time\":\"", m);
    if (ts[0]) { char hm[8]; fmt_hhmm(ts, hm); str_cat(dst, hm, m); }
    str_cat(dst, "\",\"t\":", m);
    str_cat(dst, ts[0] ? ts : "0", m);
    str_cat(dst, "000", m); /* seconds → ms */
}

/* ── Walking a JSON array of objects ─────────────────────────────────── */

/* Copy the next top-level {...} out of [s] starting at *pos into [out].
 * Brace-counting that respects strings and escapes, so a `wire` containing a
 * brace cannot end the object early. Returns 1 when one was copied. */
static int next_obj(const char *s, unsigned *pos, char *out, unsigned cap) {
    unsigned i = *pos;
    while (s[i] && s[i] != '{') i++;
    if (!s[i]) return 0;
    unsigned depth = 0, o = 0;
    int instr = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (o < cap - 1) out[o++] = c;
        if (instr) {
            if (c == '\\' && s[i + 1]) { if (o < cap - 1) out[o++] = s[++i]; continue; }
            if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (!depth) { i++; break; } }
    }
    out[o] = '\0';
    *pos = i;
    return o > 0;
}

/* ── Reading an XPRS wire ────────────────────────────────────────────── */

/* Value of `key:` in a wire, up to the next space. `m:` is greedy to the end
 * of the packet (section 2), which is what makes a status body able to hold
 * spaces at all — so it is handled separately by wire_body(). */
static int wire_key(const char *wire, const char *key, char *out, unsigned cap) {
    unsigned kl = str_len(key);
    for (const char *p = wire; *p; p++) {
        if (p != wire && p[-1] != ' ') continue;
        unsigned i = 0;
        while (i < kl && p[i] == key[i]) i++;
        if (i != kl || p[kl] != ':') continue;
        p += kl + 1;
        unsigned o = 0;
        while (*p && *p != ' ' && o < cap - 1) out[o++] = *p++;
        out[o] = '\0';
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/* Everything after " m:" — the status text. Stays JSON-escaped, exactly as the
 * archive handed it over, because it is written straight back into a JSON
 * string. Returns 0 when the packet carries no body. */
static int wire_body(const char *wire, char *out, unsigned cap) {
    for (const char *p = wire; *p; p++) {
        if (!(p == wire || p[-1] == ' ')) continue;
        if (p[0] == 'm' && p[1] == ':') {
            str_copy(out, p + 2, cap);
            return out[0] ? 1 : 0;
        }
    }
    out[0] = '\0';
    return 0;
}

/* ── State ───────────────────────────────────────────────────────────── */
#define SEEN_MAX    256
#define FOLLOW_MAX   64
#define CALL_MAX     16
#define PART_MAX      8

static char g_hist[60000];              /* one hal_xprs_history reply        */
static char g_row[2048];                /* one row out of it                 */
static char g_msg[16384];               /* outbound UI message               */
static char g_seen[SEEN_MAX][20];       /* packet ids already shown (ring)   */
static int  g_nseen = 0;
static char g_follow[FOLLOW_MAX][CALL_MAX];
static int  g_nfollow = 0;
static char g_query[128] = "";          /* Search box                        */

/* The conversations we are part of: every status of OURS, by id, and the
 * parent it answered. A reply naming one of these is somebody talking to us,
 * which is what makes it worth a notification -- and it is a CONTENT question,
 * so it is answered here rather than in the core. */
#define MINE_MAX 64
static char g_mine[MINE_MAX][20];
static int  g_nmine = 0;
static char g_kv[1600];                 /* follow list as stored             */

/* Multi-part statuses (section 6.6) arrive as separate packets sharing a head.
 * Hold the pieces until the set is complete, keyed by sender + ts. */
typedef struct {
    char from[CALL_MAX];
    char ts[24];
    int  total;
    int  have;
    char part[9][260];
} group_t;
static group_t g_grp[PART_MAX];
static int g_ngrp = 0;

/* ── Seen ring ───────────────────────────────────────────────────────── */
static int seen(const char *id) {
    for (int i = 0; i < g_nseen && i < SEEN_MAX; i++)
        if (str_eq(g_seen[i], id)) return 1;
    return 0;
}
static void mark_seen(const char *id) {
    str_copy(g_seen[g_nseen % SEEN_MAX], id, 20);
    g_nseen++;
}

/* ── Our own threads ─────────────────────────────────────────────────── */
static int my_thread(const char *id) {
    if (!id || !id[0]) return 0;
    for (int i = 0; i < g_nmine && i < MINE_MAX; i++)
        if (str_eq(g_mine[i], id)) return 1;
    return 0;
}
static void mark_mine(const char *id) {
    if (!id || !id[0] || my_thread(id)) return;
    str_copy(g_mine[g_nmine % MINE_MAX], id, 20);
    g_nmine++;
}

/* ── Speaking on the air ─────────────────────────────────────────────────
 * Likes and replies go OUT from here: a like is a t:reaction (section 6.5)
 * naming the status's section-5 id, a reply is itself a t:status carrying
 * r: (section 27). The core signs and picks the bearers; the archive is the
 * core's too — this wapp only composes words. */
static char g_call[CALL_MAX] = "";
static const char *my_call(void) {
    if (!g_call[0]) {
        unsigned n = hal_identity(g_call, sizeof(g_call) - 1);
        if (n >= sizeof(g_call)) n = sizeof(g_call) - 1;
        g_call[n] = '\0';
        for (int i = 0; g_call[i]; i++) g_call[i] = uc(g_call[i]);
    }
    return g_call;
}

/* Epoch -> "YYYY-MM-DD_hh:mm:ss" UTC (section 4.8). Same civil-date
 * arithmetic the chat wapp uses; exact for any date this will ever stamp. */
static void two(char *d, int v) { d[0] = (char)('0' + (v / 10) % 10); d[1] = (char)('0' + v % 10); }
/* Now, as the seconds the feed sorts by. `ts` in a row from the spool is the
 * unix time (cat_time_fields divides and multiplies it); the wire's
 * "YYYY-MM-DD_hh:mm:ss" is a different thing and putting one where the other
 * belongs produced `"t":2026-08-29_10:40:00000`, which is not JSON — the whole
 * append was dropped by the host and the post never appeared. */
static void epoch_now(char *out, unsigned cap) {
    unsigned long long e = (unsigned long long)hal_time_epoch();
    char tmp[24]; int n = 0;
    if (e == 0) { str_copy(out, "0", cap); return; }
    while (e && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (int)(e % 10)); e /= 10; }
    int o = 0;
    while (n > 0 && o < (int)cap - 1) out[o++] = tmp[--n];
    out[o] = '\0';
}

/* "YYYY-MM-DD_hh:mm:ss" (section 4.8) -> unix seconds, as text.
 *
 * The two doors a post arrives through do not agree about `ts`: a row out of
 * the spool carries the epoch, and a packet delivered live carries the wire's
 * civil timestamp. The feed sorts on the epoch, and a civil timestamp put
 * where it belongs produced `"t":2026-09-10_08:32:06000` -- not JSON, so the
 * host dropped the whole append and a post that HAD arrived never appeared.
 * Converted here, where the difference is known, so one shape reaches the UI.
 */
static void epoch_from_ts(const char *ts, char *out, unsigned cap) {
    long y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    const char *p = ts;
    while (*p >= '0' && *p <= '9') y = y * 10 + (*p++ - '0');
    if (*p == '-') p++;
    while (*p >= '0' && *p <= '9') mo = mo * 10 + (*p++ - '0');
    if (*p == '-') p++;
    while (*p >= '0' && *p <= '9') d = d * 10 + (*p++ - '0');
    if (*p == '_') p++;
    while (*p >= '0' && *p <= '9') hh = hh * 10 + (*p++ - '0');
    if (*p == ':') p++;
    while (*p >= '0' && *p <= '9') mi = mi * 10 + (*p++ - '0');
    if (*p == ':') p++;
    while (*p >= '0' && *p <= '9') ss = ss * 10 + (*p++ - '0');
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        str_copy(out, "0", cap);
        return;
    }
    /* days_from_civil (the inverse of the one stamp_now uses). */
    long yy = y - (mo <= 2 ? 1 : 0);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    long yoe = yy - era * 400;
    long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    long long e = (long long)days * 86400 + hh * 3600 + mi * 60 + ss;
    if (e < 0) e = 0;
    char tmp[24]; int n = 0;
    if (e == 0) { str_copy(out, "0", cap); return; }
    while (e && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (int)(e % 10)); e /= 10; }
    int o = 0;
    while (n > 0 && o < (int)cap - 1) out[o++] = tmp[--n];
    out[o] = '\0';
}

static void stamp_now(char *out, unsigned cap) {
    if (cap < 20) { out[0] = '\0'; return; }
    unsigned long long e = hal_time_epoch();
    long z = (long)(e / 86400ULL);
    unsigned s = (unsigned)(e % 86400ULL);
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned long mm = mp < 10 ? mp + 3 : mp - 9;
    int y = (int)(yy + (mm <= 2 ? 1 : 0));
    out[0] = (char)('0' + (y / 1000) % 10);
    out[1] = (char)('0' + (y / 100) % 10);
    out[2] = (char)('0' + (y / 10) % 10);
    out[3] = (char)('0' + y % 10);
    out[4] = '-'; two(out + 5, (int)mm);
    out[7] = '-'; two(out + 8, (int)dd);
    out[10] = '_'; two(out + 11, (int)(s / 3600));
    out[13] = ':'; two(out + 14, (int)((s / 60) % 60));
    out[16] = ':'; two(out + 17, (int)(s % 60));
    out[19] = '\0';
}

/* A JSON string value out of json_raw still carries its escapes; a wire is
 * one plain line, so undo them (and flatten whatever control characters an
 * input box smuggled in into spaces). */
static void unescape(char *s) {
    unsigned o = 0;
    for (unsigned i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) {
            char n = s[++i];
            c = n == 'n' || n == 't' || n == 'r' ? ' ' : n;
        }
        if ((unsigned char)c < 32) c = ' ';
        s[o++] = c;
    }
    s[o] = '\0';
}

/* Tell the host one like vote landed on one post: it tallies per liker in
 * the core activity archive (idempotent), so replays cost nothing. */
static void push_react(const char *mid, const char *from, int like, int mine) {
    str_copy(g_msg, "{\"type\":\"ui.activity.react\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, mid, sizeof(g_msg));
    str_cat(g_msg, "\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"like\":", sizeof(g_msg));
    str_cat(g_msg, like ? "true" : "false", sizeof(g_msg));
    str_cat(g_msg, ",\"mine\":", sizeof(g_msg));
    str_cat(g_msg, mine ? "true" : "false", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Follows (callsigns, kept in host kv) ────────────────────────────── */
#define FOLLOW_KEY "xprs.follow.calls"

static void follows_load(void) {
    g_nfollow = 0;
    uint32_t n = hal_kv_get(FOLLOW_KEY, str_len(FOLLOW_KEY), g_kv, sizeof(g_kv) - 1);
    if (n == 0 || n >= sizeof(g_kv)) { g_kv[0] = '\0'; return; }
    g_kv[n] = '\0';
    /* Stored as "CALL,CALL,CALL" — a list this small does not need JSON. */
    char one[CALL_MAX]; unsigned o = 0;
    for (const char *p = g_kv; ; p++) {
        if (*p && *p != ',') { if (o < sizeof(one) - 1) one[o++] = uc(*p); continue; }
        one[o] = '\0';
        if (one[0] && g_nfollow < FOLLOW_MAX) str_copy(g_follow[g_nfollow++], one, CALL_MAX);
        o = 0;
        if (!*p) break;
    }
}

static void follows_save(void) {
    g_kv[0] = '\0';
    for (int i = 0; i < g_nfollow; i++) {
        if (i) str_cat(g_kv, ",", sizeof(g_kv));
        str_cat(g_kv, g_follow[i], sizeof(g_kv));
    }
    hal_kv_set(FOLLOW_KEY, str_len(FOLLOW_KEY), g_kv, str_len(g_kv));
}

static int followed(const char *call) {
    for (int i = 0; i < g_nfollow; i++) if (str_eq(g_follow[i], call)) return 1;
    return 0;
}

static void follow_add_call(const char *raw) {
    char call[CALL_MAX]; unsigned o = 0;
    /* A suffix names a device; following is a person (section 3.1). */
    for (const char *p = raw; *p && *p != '-' && o < sizeof(call) - 1; p++) {
        if (*p == ' ') continue;
        call[o++] = uc(*p);
    }
    call[o] = '\0';
    if (!call[0] || followed(call) || g_nfollow >= FOLLOW_MAX) return;
    str_copy(g_follow[g_nfollow++], call, CALL_MAX);
    follows_save();
}

static void follow_remove_call(const char *raw) {
    char call[CALL_MAX]; unsigned o = 0;
    for (const char *p = raw; *p && *p != '-' && o < sizeof(call) - 1; p++) call[o++] = uc(*p);
    call[o] = '\0';
    for (int i = 0; i < g_nfollow; i++) {
        if (!str_eq(g_follow[i], call)) continue;
        for (int j = i; j + 1 < g_nfollow; j++) str_copy(g_follow[j], g_follow[j + 1], CALL_MAX);
        g_nfollow--;
        follows_save();
        return;
    }
}

/* ── Notifications (app/docs/notifications.md) ───────────────────────────
 * There is no hal_notify: a notification is a message TYPE on the outbox, and
 * the host routes it. `tag` is the packet's section 5 identifier, which the
 * notification service dedupes once ever and across restarts -- so a backfill
 * cannot replay yesterday's replies as today's buzzes. */
static void notify_about(const char *tag, const char *who, const char *what,
                         const char *scope) {
    /* Rare by construction — a reply or a like naming something of ours — so
     * one line each is a record of what the operator was told, not chatter. */
    {
        char l[96];
        str_copy(l, "[social] notify ", sizeof(l));
        str_cat(l, who, sizeof(l));
        str_cat(l, what, sizeof(l));
        hal_log(6, l, str_len(l));
    }
    str_copy(g_msg, "{\"type\":\"notify\",\"level\":\"info\",\"title\":\"Social\",\"body\":\"",
             sizeof(g_msg));
    str_cat(g_msg, who, sizeof(g_msg));
    str_cat(g_msg, what, sizeof(g_msg));
    str_cat(g_msg, "\",\"tag\":\"", sizeof(g_msg));
    str_cat(g_msg, tag, sizeof(g_msg));
    str_cat(g_msg, "\",\"scope\":\"", sizeof(g_msg));
    str_cat(g_msg, scope, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── The feed ────────────────────────────────────────────────────────── */

/* One post into a chat field. [body] is already JSON-escaped (it came out of
 * the archive's JSON and goes straight back into ours). */
static void feed_append(const char *field, const char *from, const char *body,
                        const char *mid, const char *parent, const char *ts,
                        const char *sig, const char *bearer, int own,
                        const char *source) {
    str_copy(g_msg, "{\"type\":\"ui.chat.append\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, field, sizeof(g_msg));
    str_cat(g_msg, "\",\"message\":{\"dir\":\"", sizeof(g_msg));
    str_cat(g_msg, own ? "out" : "in", sizeof(g_msg));
    str_cat(g_msg, "\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"author\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    str_cat(g_msg, body, sizeof(g_msg));           /* already escaped */
    str_cat(g_msg, "\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, mid, sizeof(g_msg));
    /* A reply to a status is itself a status carrying r: (section 27) — the
     * parent is that id, and the host threads on it. */
    str_cat(g_msg, "\",\"parent\":\"", sizeof(g_msg));
    str_cat(g_msg, parent, sizeof(g_msg));
    str_cat(g_msg, "\",\"pop\":0,\"source\":\"", sizeof(g_msg));
    str_cat(g_msg, source, sizeof(g_msg));
    str_cat(g_msg, "\"", sizeof(g_msg));
    /* Whether the archive could check the signature is part of the post: a
     * callsign is a label, so "who said this" is the signature's answer. */
    if (sig[0]) {
        str_cat(g_msg, ",\"tags\":[\"", sizeof(g_msg));
        str_cat(g_msg, sig, sizeof(g_msg));
        if (bearer[0]) { str_cat(g_msg, "\",\"", sizeof(g_msg)); str_cat(g_msg, bearer, sizeof(g_msg)); }
        str_cat(g_msg, "\"]", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Find (or start) the part-group a multi-part status belongs to. */
static group_t *group_for(const char *from, const char *ts, int total) {
    for (int i = 0; i < g_ngrp; i++)
        if (str_eq(g_grp[i].from, from) && str_eq(g_grp[i].ts, ts)) return &g_grp[i];
    group_t *g = &g_grp[g_ngrp % PART_MAX];
    if (g_ngrp < PART_MAX) g_ngrp++;
    str_copy(g->from, from, CALL_MAX);
    str_copy(g->ts, ts, sizeof(g->ts));
    g->total = total; g->have = 0;
    for (int i = 0; i < 9; i++) g->part[i][0] = '\0';
    return g;
}

/* Read the spool and push anything new into [field].
 *
 * Every status goes out once. A post from a followed callsign is labelled
 * `source:"following"`, everything else `source:"xprs"` -- the host files
 * those into its two archives and the Activity tabs read them, so the
 * narrowing lives where the tabs already are rather than being duplicated
 * here. [match] narrows by words (Search); pass "" for none. */
static void feed_from_spool(const char *field, const char *query,
                            const char *match) {
    int n = hal_xprs_history(query, str_len(query), g_hist, sizeof(g_hist) - 1);
    if (n <= 0) return;                 /* negative = our buffer is too small */
    g_hist[n] = '\0';

    /* The spool is newest first; walk it backwards is not possible in one
     * pass over a flat string, so collect ids first and emit oldest-last —
     * the host feed sorts by "t" anyway, so order here only affects which
     * ones win the seen-ring when the window is larger than the ring. */
    unsigned pos = 0;
    while (next_obj(g_hist, &pos, g_row, sizeof(g_row))) {
        char type[24] = "";
        json_raw(g_row, "type", type, sizeof(type));
        /* A reaction (6.5) is a tally on a post, never a row of its own. */
        if (str_eq(type, "reaction")) {
            char rid[20] = "", rfrom[CALL_MAX] = "", rwire[300] = "";
            json_raw(g_row, "id", rid, sizeof(rid));
            json_raw(g_row, "from", rfrom, sizeof(rfrom));
            json_raw(g_row, "wire", rwire, sizeof(rwire));
            if (!rid[0] || !rfrom[0] || seen(rid)) continue;
            char tgt[12] = "", act[12] = "";
            wire_key(rwire, "r", tgt, sizeof(tgt));
            int add = wire_key(rwire, "add", act, sizeof(act)) && str_eq(act, "like");
            int rem = !add && wire_key(rwire, "remove", act, sizeof(act)) && str_eq(act, "like");
            if (!tgt[0] || (!add && !rem)) continue;
            mark_seen(rid);
            char person[CALL_MAX]; unsigned o = 0;
            for (const char *p = rfrom; *p && *p != '-' && o < sizeof(person) - 1; p++) person[o++] = uc(*p);
            person[o] = '\0';
            push_react(tgt, person, add, str_eq(person, my_call()));
            continue;
        }
        if (!str_eq(type, "status")) continue;

        char id[20] = "", from[CALL_MAX] = "", ts[24] = "", sig[16] = "",
             bearer[12] = "", wire[1100] = "", own[8] = "";
        json_raw(g_row, "id", id, sizeof(id));
        json_raw(g_row, "from", from, sizeof(from));
        json_raw(g_row, "ts", ts, sizeof(ts));
        json_raw(g_row, "sig", sig, sizeof(sig));
        json_raw(g_row, "bearer", bearer, sizeof(bearer));
        json_raw(g_row, "own", own, sizeof(own));
        json_raw(g_row, "wire", wire, sizeof(wire));
        if (!id[0] || !wire[0]) continue;

        /* The bare callsign is the person (section 3.1). */
        char person[CALL_MAX]; unsigned o = 0;
        for (const char *p = from; *p && *p != '-' && o < sizeof(person) - 1; p++) person[o++] = uc(*p);
        person[o] = '\0';

        const char *source = followed(person) ? "following" : "xprs";

        char body[1100] = "";
        if (!wire_body(wire, body, sizeof(body))) continue;
        if (match[0] && !contains_ci(body, match) && !contains_ci(person, match)) continue;

        /* r: names the status this one replies to (section 27). */
        char parent[12] = "";
        wire_key(wire, "r", parent, sizeof(parent));

        /* Section 6.6: `n:i/k` means this is one piece of a longer status. */
        char nfield[12] = "";
        if (wire_key(wire, "n", nfield, sizeof(nfield)) && nfield[0]) {
            int idx = 0, tot = 0; const char *p = nfield;
            while (*p >= '0' && *p <= '9') idx = idx * 10 + (*p++ - '0');
            if (*p == '/') { p++; while (*p >= '0' && *p <= '9') tot = tot * 10 + (*p++ - '0'); }
            if (idx >= 1 && idx <= 9 && tot >= 1 && tot <= 9) {
                char pts[24] = ""; wire_key(wire, "ts", pts, sizeof(pts));
                group_t *g = group_for(person, pts, tot);
                if (!g->part[idx - 1][0]) { str_copy(g->part[idx - 1], body, sizeof(g->part[0])); g->have++; }
                if (g->have < g->total) continue;    /* still incomplete */
                if (seen(id)) continue;
                char whole[2200] = "";
                for (int i = 0; i < g->total; i++) str_cat(whole, g->part[i], sizeof(whole));
                mark_seen(id);
                if (str_eq(own, "true")) { mark_mine(id); mark_mine(parent); }
                feed_append(field, person, whole, id, parent, ts, sig, bearer, str_eq(own, "true"), source);
                continue;
            }
        }

        if (seen(id)) continue;
        mark_seen(id);
        if (str_eq(own, "true")) { mark_mine(id); mark_mine(parent); }
        feed_append(field, person, body, id, parent, ts, sig, bearer, str_eq(own, "true"), source);
    }
}

/* ── One packet, live ────────────────────────────────────────────────────
 *
 * The core publishes every packet it accepts on a topic named after its type
 * (`xprs.status`, `xprs.reaction`), the moment it has it. That is the same
 * post the spool will hand back at the next flush, so the row is built the
 * same way and marked seen under the same section 5 identifier -- whichever
 * copy arrives first wins, and the other one is a no-op.
 *
 * The event carries `wire`, so the parsing below is the parsing feed_from_spool
 * already does; nothing about a packet is read two different ways here.
 *
 * [live] is 1 for a packet arriving now and 0 for the backfill. It decides one
 * thing only: whether this may raise a notification. A restart re-reads sixty
 * rows, and a person who opens Social should not be told sixty times.
 *
 * [draw] is 0 when no page is attached. A notification is exactly the case
 * where nobody is looking, so it is decided either way; only the drawing is
 * skipped. The seen ring is still marked, which costs nothing: opening the
 * page sends `ready`, which clears the ring and refills from the spool.
 */
static void row_from_event(const char *row, int live, int draw) {
    char type[24] = "";
    json_raw(row, "type", type, sizeof(type));

    char id[20] = "", from[CALL_MAX] = "", wire[1100] = "";
    json_raw(row, "id", id, sizeof(id));
    json_raw(row, "from", from, sizeof(from));
    json_raw(row, "wire", wire, sizeof(wire));
    if (!id[0] || !from[0] || !wire[0]) return;

    /* The bare callsign is the person (section 3.1). */
    char person[CALL_MAX]; unsigned o = 0;
    for (const char *p = from; *p && *p != '-' && o < sizeof(person) - 1; p++)
        person[o++] = uc(*p);
    person[o] = '\0';
    const int ours = str_eq(person, my_call());

    if (str_eq(type, "reaction")) {
        char tgt[20] = "", act[12] = "";
        wire_key(wire, "r", tgt, sizeof(tgt));
        int add = wire_key(wire, "add", act, sizeof(act)) && str_eq(act, "like");
        int rem = !add && wire_key(wire, "remove", act, sizeof(act)) && str_eq(act, "like");
        if (!tgt[0] || (!add && !rem) || seen(id)) return;
        mark_seen(id);
        if (draw) push_react(tgt, person, add, ours);
        /* Somebody liked something of ours. A card, not a buzz in a pocket. */
        if (live && add && !ours && my_thread(tgt))
            notify_about(id, person, " liked your post", "app");
        return;
    }
    if (!str_eq(type, "status")) return;
    if (seen(id)) return;

    char wts[24] = "", ts[24] = "", sig[16] = "", bearer[12] = "", body[1100] = "";
    json_raw(row, "ts", wts, sizeof(wts));
    epoch_from_ts(wts, ts, sizeof(ts));
    json_raw(row, "sig", sig, sizeof(sig));
    json_raw(row, "bearer", bearer, sizeof(bearer));
    if (!wire_body(wire, body, sizeof(body))) return;

    char parent[20] = "";
    wire_key(wire, "r", parent, sizeof(parent));

    mark_seen(id);
    /* Ours: this post, and the conversation it was an answer in. Somebody who
     * replies to either is replying to us. */
    if (ours) { mark_mine(id); mark_mine(parent); }
    if (draw)
        feed_append("activity", person, body, id, parent, ts, sig, bearer, ours,
                    followed(person) ? "following" : "xprs");

    /* Somebody answered in a conversation we are part of. */
    if (live && !ours && my_thread(parent))
        notify_about(id, person, " replied to you", "both");
}

/* Which conversations are ours, learned from the spool at startup.
 *
 * Draws nothing and marks nothing as seen: a background engine has no page,
 * and the ring that decides what has been SHOWN must stay empty so opening
 * Social still fills the feed. What this fills is the OTHER ring — the posts
 * of ours a reply could name — without which a station that was restarted
 * knows none of its own conversations and quietly stops telling its operator
 * that somebody answered.
 *
 * One read of forty rows, once, when the engine starts. */
static void mine_from_spool(void) {
    static const char *q = "{\"limit\":40,\"types\":[\"status\"]}";
    int n = hal_xprs_history(q, str_len(q), g_hist, sizeof(g_hist) - 1);
    if (n <= 0) return;
    g_hist[n] = '\0';
    unsigned pos = 0;
    while (next_obj(g_hist, &pos, g_row, sizeof(g_row))) {
        char own[8] = "", id[20] = "", wire[1100] = "";
        json_raw(g_row, "own", own, sizeof(own));
        if (!str_eq(own, "true")) continue;
        json_raw(g_row, "id", id, sizeof(id));
        json_raw(g_row, "wire", wire, sizeof(wire));
        if (!id[0]) continue;
        mark_mine(id);
        char parent[20] = "";
        if (wire[0] && wire_key(wire, "r", parent, sizeof(parent)))
            mark_mine(parent);
    }
}

/* ── Following panel ─────────────────────────────────────────────────── */
static void push_follows(void) {
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"follows_list\",\"sections\":"
                    "[{\"title\":\"Following (", sizeof(g_msg));
    char cnt[8]; int v = g_nfollow, o = 0; char tmp[8];
    if (!v) { cnt[0] = '0'; cnt[1] = '\0'; }
    else { while (v) { tmp[o++] = (char)('0' + v % 10); v /= 10; } int k = 0; while (o) cnt[k++] = tmp[--o]; cnt[k] = '\0'; }
    str_cat(g_msg, cnt, sizeof(g_msg));
    str_cat(g_msg, ")\",\"items\":[", sizeof(g_msg));
    for (int i = 0; i < g_nfollow; i++) {
        if (i) str_cat(g_msg, ",", sizeof(g_msg));
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg));
        str_cat(g_msg, g_follow[i], sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
        str_cat(g_msg, g_follow[i], sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"heard over the air\"}", sizeof(g_msg));
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

static void clear_field(const char *field) {
    str_copy(g_msg, "{\"type\":\"ui.chat.clear\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, field, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Module ──────────────────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[social] up — XPRS only", 24);
    /* The feed's source is the spool, and the core says when the spool grew --
     * once per flush, which is also once per burst. This used to be a sqlite
     * read every 2.8 seconds on a 700ms clock, whose answer on a quiet radio
     * is always the same sixty rows. */
    {
        /* Live, per packet type (section 4.2): the core publishes a status the
         * moment it accepts one, so a post from anybody is on the timeline
         * without waiting for the archive's flush. */
        static const char *live = "xprs.status";
        hal_event_subscribe(live, str_len(live));
        static const char *react = "xprs.reaction";
        hal_event_subscribe(react, str_len(react));
        /* And the backfill: what a restart missed, and the parts that only
         * become a post once the whole set has arrived. */
        static const char *t = "core.archive";
        hal_event_subscribe(t, str_len(t));
    }
    follows_load();
    /* Which threads are ours, so a reply arriving before anybody opens the
     * page is still recognised as somebody answering us. */
    mine_from_spool();
    {
        char l[64];
        str_copy(l, "[social] up, threads of mine: ", sizeof(l));
        char n[8]; int v = g_nmine, o = 0;
        if (v == 0) n[o++] = '0';
        else { char t[8]; int k = 0; while (v && k < 7) { t[k++] = (char)('0' + v % 10); v /= 10; }
               while (k > 0) n[o++] = t[--k]; }
        n[o] = '\0';
        str_cat(l, n, sizeof(l));
        hal_log(6, l, str_len(l));
    }
    /* Nothing is pushed here. A ui.* message sent before the page has
     * attached is read by nobody, and the seen-ring would still have marked
     * those posts as shown — the feed would then be permanently empty with
     * the spool full. The first push happens on `ready`. */
    return 0;
}

/* Something happened: a packet arrived, or the spool grew.
 *
 * A live packet is appended on its own -- one row, no query -- and only
 * `core.archive` costs a spool read. That is the difference between being told
 * and asking, and it is what a person watching the screen actually feels. */
static void drain_core_events(void) {
    static char topic[48];
    /* A delivered packet, not a counter: the row carries the wire, the
     * provenance and the signature verdict. 256 bytes held none of it. */
    static char data[4096];
    int refill = 0;
    /* Only while somebody is looking: with the page detached these appends go
     * nowhere, and the seen-ring would eat them (see module_init). */
    const int attached = hal_ui_attached();
    for (int guard = 0; guard < 16; guard++) {
        if (hal_event_available() == 0) break;
        if (hal_event_recv(topic, sizeof(topic) - 1, data, sizeof(data) - 1) == 0)
            break;
        if (str_eq(topic, "core.archive")) { refill = 1; continue; }
        if (str_eq(topic, "xprs.status") || str_eq(topic, "xprs.reaction"))
            row_from_event(data, 1, attached);
    }
    if (!refill || !attached) return;
    feed_from_spool("activity",
        "{\"limit\":60,\"types\":[\"status\",\"reaction\"]}", "");
}

/* No clock: a feed changes when a packet is spooled, and the core says so. */
int32_t module_tick(void) {
    return 0;
}

int32_t module_handle_event(void) {
    /* The host bundles EVERY scalar field into each command message, so this
     * buffer sizes the whole field map and not the one value we want. At 6 KB
     * a long-enough map pushed `activity_input` past the end and the post was
     * read as empty: no status, no error, nothing anywhere. Sized for the map
     * with room to spare, and a truncated read is now visible rather than
     * silently short. */
    drain_core_events();
    static char buf[24576];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    if (n >= sizeof(buf) - 1) hal_log(4, "[social] event truncated", 25);
    buf[n] = '\0';

    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh") ||
        str_eq(cmd, "activity_refresh")) {
        follows_load();
        push_follows();
        /* The page is (re)attached and its buffer starts empty, so forget what
         * we think it has already been shown and fill it from the spool. */
        g_nseen = 0;
        g_ngrp = 0;
        feed_from_spool("activity",
            "{\"limit\":120,\"types\":[\"status\",\"reaction\"]}", "");

    } else if (str_eq(cmd, "activity_send")) {
        /* The post, twice over: on the screen now, on the air in the core's
         * own time.
         *
         * hal_xprs_status composes and signs before it returns and hands back
         * the section 5 identifier, so the row can be drawn immediately and
         * keyed on the name the packet already has. The copy that comes back
         * -- off the air, or out of the spool at the next flush -- carries the
         * same id and the seen-ring swallows it, so the feed shows it once.
         *
         * This used to be aired by the host instead, because a post from here
         * once vanished. The cause was a 6 KB event buffer that truncated the
         * text to empty (the buffer above is 24 KB now), not the round trip.
         * The core still owns every transport decision; this asks it to
         * publish and says so on screen. */
        char text[6000] = "";
        if (json_raw(buf, "activity_input", text, sizeof(text)) && text[0]) {
            char body[6000];
            str_copy(body, text, sizeof(body));
            unescape(body);
            char mid[24] = "";
            if (hal_xprs_status(body, str_len(body), 0, 0, 0, 0,
                                mid, sizeof(mid) - 1) == 0 && mid[0]) {
                char ts[24]; epoch_now(ts, sizeof(ts));
                mark_seen(mid);
                mark_mine(mid);
                /* `text` is still the escaped form the host sent us, which is
                 * what feed_append wants. */
                feed_append("activity", my_call(), text, mid, "", ts,
                            "verified", "", 1, "xprs");
            } else {
                hal_log(4, "[social] post refused by the core", 33);
                notify_about("", "", "Could not post", "app");
            }
        }

    } else if (str_eq(cmd, "clear_feed")) {
        g_nseen = 0;
        clear_field("activity");

    } else if (str_eq(cmd, "search_go") || str_eq(cmd, "search_input_changed") ||
               str_eq(cmd, "search_kind_changed") ||
               str_eq(cmd, "search_when_changed")) {
        json_raw(buf, "search_input", g_query, sizeof(g_query));
        clear_field("search_results");
        int keep = g_nseen; g_nseen = 0;   /* search has its own field */
        feed_from_spool("search_results",
            "{\"limit\":200,\"types\":[\"status\"]}", g_query);
        g_nseen = keep;

    } else if (str_eq(cmd, "follow_add")) {
        /* The host names an action's companion input after the field. */
        char v[64] = "";
        if (json_raw(buf, "follow_input", v, sizeof(v)) && v[0]) follow_add_call(v);
        push_follows();

    } else if (str_eq(cmd, "profile_follow") || str_eq(cmd, "profile_unfollow")) {
        /* The Follow button on a post's author. For an XPRS feed the author
         * IS the callsign, so there is no short-key to resolve back. */
        char v[64] = "";
        if (json_raw(buf, "profile_target", v, sizeof(v)) && v[0]) {
            if (str_eq(cmd, "profile_follow")) follow_add_call(v);
            else follow_remove_call(v);
        }
        push_follows();

    } else if (str_eq(cmd, "follows_list_unfollow")) {
        char v[64] = "";
        if (json_raw(buf, "follows_list_id", v, sizeof(v)) && v[0]) follow_remove_call(v);
        push_follows();

    } else if (str_eq(cmd, "follows_list")) {
        push_follows();

    } else if (str_eq(cmd, "activity_like")) {
        /* A like on a post: t:reaction add:like r:<id> (6.5); retracting one
         * is remove:like. The core signs it and picks the bearers; the vote
         * also lands in the local tally NOW rather than after the spool
         * round trip. */
        char mid[20] = "", set[8] = "";
        json_raw(buf, "activity_mid", mid, sizeof(mid));
        json_raw(buf, "activity_set", set, sizeof(set));
        if (mid[0] && my_call()[0]) {
            int unlike = set[0] == '0';
            char ts[24]; stamp_now(ts, sizeof(ts));
            char wire[160];
            str_copy(wire, "t:reaction f:", sizeof(wire));
            str_cat(wire, my_call(), sizeof(wire));
            str_cat(wire, " ts:", sizeof(wire)); str_cat(wire, ts, sizeof(wire));
            str_cat(wire, unlike ? " remove:like r:" : " add:like r:", sizeof(wire));
            str_cat(wire, mid, sizeof(wire));
            if (hal_xprs_send(wire, str_len(wire)) == 0)
                push_react(mid, my_call(), !unlike, 1);
            else
                hal_log(4, "[social] like refused by the core", 34);
        }

    } else if (str_eq(cmd, "activity_reply")) {
        /* A reply is itself a status carrying r: (section 27). */
        char mid[24] = "", text[400] = "", esc[400] = "";
        json_raw(buf, "activity_target_mid", mid, sizeof(mid));
        json_raw(buf, "activity_input", esc, sizeof(esc));
        str_copy(text, esc, sizeof(text));
        unescape(text);
        if (mid[0] && text[0]) {
            /* The same verb as a post, with the parent named: the core builds
             * `t:status ... r:<parent>`, signs it and splits it if it has to,
             * which is one less place composing a wire by hand. */
            char rid[24] = "";
            if (hal_xprs_status(text, str_len(text), 0, 0, mid, str_len(mid),
                                rid, sizeof(rid) - 1) == 0 && rid[0]) {
                char ts[24]; epoch_now(ts, sizeof(ts));
                mark_seen(rid);
                mark_mine(rid);
                mark_mine(mid);      /* we are in this conversation now */
                feed_append("activity", my_call(), esc, rid, mid, ts,
                            "verified", "", 1, "xprs");
            } else {
                hal_log(4, "[social] reply refused by the core", 34);
            }
        }

    } else if (str_eq(cmd, "activity_repost")) {
        /* Still honest: section 27 has no repost packet. */
        hal_log(4, "[social] no XPRS packet for repost", 34);
    }
    return 0;
}

/* 0 = no clock: see module_tick. */
int32_t module_tick_interval_ms(void) { return 0; }

void module_destroy(void) {}
