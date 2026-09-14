/*
 * archiver — hold other people's things so they survive the device that made
 * them going offline, and tell the network where things are.
 *
 * Two jobs, two screens, one wapp. They used to be two:
 *
 *   Storage    keeps COPIES. Costs disk, and giving it up loses content.
 *   Directory  keeps POINTERS -- signed ~176-byte records saying which devices
 *              hold a given npub's posts and files. Addresses, never content,
 *              so a directory that vanishes costs the network a phone book
 *              rather than a library.
 *
 * These were once two wapps, and that split asked the owner to learn a
 * distinction that only starts mattering after they have already agreed to
 * help. Both answer the same question -- what is this device willing to do for
 * other people -- so they are answered in one place, with the two offers still
 * granted and revoked independently: enabling storage does not volunteer the
 * directory, and neither implies the other. XPRS.md 36.0 settles the naming:
 * on the air there is ONE role and one word for it, serve:archive, covering
 * kept packets, held mail and the directory alike.
 *
 * Storage's two rules are the whole contract:
 *   - a device that never volunteered holds nothing for anybody (silence is not
 *     consent, so it starts disabled);
 *   - the limit is a CEILING, not a target. Full is full, and nothing of the
 *     owner's is ever deleted to make room for somebody else's.
 *
 * The screen is a dashboard: how full, how many files, how much of it anyone
 * ever actually wanted, how much could be freed right now. Then the handful of
 * decisions the owner makes, and one button to reclaim the space.
 *
 * Host HAL:
 *   hal_archive_status   → JSON: quota, used, items, served, freeable, switches
 *   hal_archive_drop     → run a cleanup by id ("sweep:strangers")
 *   hal_archive_set_pref → quotaGb, followed, fromNearby, mirrorSmall
 *   hal_node_status      → JSON: serving, pointers, authors, query rates, spark…
 *   hal_node_set_pref    → volunteer=off|auto|always
 *
 * Build: cd apps/archiver && make
 */

#include "../hal/xprs_wasm_hal.h"
#include "../hal/people_finder.h"

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
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && o < m - 1) out[o++] = *p++;
        } else if (*p == '[') {
            int depth = 0;
            while (*p && o < m - 1) {
                if (*p == '[') depth++;
                if (*p == ']') { depth--; out[o++] = *p++; if (!depth) break; continue; }
                out[o++] = *p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}' && o < m - 1) out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_status[6144];
static char g_msg[12288];
static char g_reqSpark[1024];
static char g_bwSpark[1024];
static char g_qSpark[1024];

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* Bool switches need a real bool, not the string "true". */
static void set_field_raw(const char *name, const char *raw) {
    str_copy(g_msg, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof(g_msg));
    str_cat(g_msg, name, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":", sizeof(g_msg));
    str_cat(g_msg, raw, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void set_field(const char *name, const char *value) {
    str_copy(g_msg, "{\"type\":\"ui.set_field\",\"name\":\"", sizeof(g_msg));
    str_cat(g_msg, name, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
    str_cat(g_msg, value, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* A packet count a phone can read. 155948 in a tile half a screen wide is
 * rendered "15…", which is not a number -- so anything past four digits is
 * said in thousands or millions, as a person would say it. */
static void compact(char *out, int cap, const char *digits) {
    int len = 0;
    while (digits[len] >= '0' && digits[len] <= '9') len++;
    if (len <= 4 || len >= 13) { str_copy(out, digits, cap); return; }
    const char *suffix = len <= 6 ? "k" : (len <= 9 ? "M" : "G");
    int keep = len - (len <= 6 ? 3 : (len <= 9 ? 6 : 9)); /* whole units */
    int i = 0, o = 0;
    for (; i < keep && o < cap - 4; i++) out[o++] = digits[i];
    /* One decimal, but never a pointless ".0" and never on a four-digit head. */
    if (keep <= 2 && digits[keep] != '0' && o < cap - 4) {
        out[o++] = '.';
        out[o++] = digits[keep];
    }
    out[o] = '\0';
    str_cat(out, suffix, cap);
}

/* Append one stat tile (commas are the caller's problem). */
static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, const char *progress,
                 int alert) {
    str_cat(g_msg, "{\"id\":\"", sizeof(g_msg));
    str_cat(g_msg, id, sizeof(g_msg));
    str_cat(g_msg, "\",\"label\":\"", sizeof(g_msg));
    str_cat(g_msg, label, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
    str_cat(g_msg, value, sizeof(g_msg));
    str_cat(g_msg, "\"", sizeof(g_msg));
    if (unit && unit[0]) {
        str_cat(g_msg, ",\"unit\":\"", sizeof(g_msg));
        str_cat(g_msg, unit, sizeof(g_msg));
        str_cat(g_msg, "\"", sizeof(g_msg));
    }
    if (hint && hint[0]) {
        str_cat(g_msg, ",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\"", sizeof(g_msg));
    }
    if (progress && progress[0]) {
        str_cat(g_msg, ",\"progress\":", sizeof(g_msg));
        str_cat(g_msg, progress, sizeof(g_msg));
    }
    if (alert) str_cat(g_msg, ",\"alert\":true", sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
}

/* THE PACKET ARCHIVE — the three tiers of XPRS.md 12.
 *
 * "Every device is an archiver; scale is a setting, not a kind." Mine is
 * always kept; the callsigns I follow are the middle tier and the reason a
 * pocket phone keeps anything beyond its own words; strangers are a choice the
 * operator makes, because silence is not consent.
 *
 * The core decides all of that (xprs_archive_policy.dart). This reads one verb
 * and draws the answer. */
static void push_archive(void) {
    int n = hal_xprs_archive(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char pub[8] = "false", always[8] = "false", alwaysRaw[8] = "false";
    char followed[8] = "true";
    char quotaMb[16] = "500", maxDays[16] = "365";
    char own[16] = "0", fol[16] = "0", str[16] = "0", total[16] = "0";
    char bytes[24] = "0 B", quotaText[24] = "0 B", full[16] = "0";
    char asks[16] = "0", answered[16] = "0", refused[16] = "0";
    char announced[32] = "", follows[16] = "0";
    json_raw(g_status, "public", pub, sizeof(pub));
    json_raw(g_status, "alwaysOn", always, sizeof(always));
    json_raw(g_status, "alwaysOnStored", alwaysRaw, sizeof(alwaysRaw));
    json_raw(g_status, "keepFollowed", followed, sizeof(followed));
    json_raw(g_status, "quotaMb", quotaMb, sizeof(quotaMb));
    json_raw(g_status, "maxDays", maxDays, sizeof(maxDays));
    json_raw(g_status, "own", own, sizeof(own));
    json_raw(g_status, "followed", fol, sizeof(fol));
    json_raw(g_status, "stranger", str, sizeof(str));
    json_raw(g_status, "total", total, sizeof(total));
    json_raw(g_status, "bytesText", bytes, sizeof(bytes));
    json_raw(g_status, "quotaText", quotaText, sizeof(quotaText));
    json_raw(g_status, "fullFrac", full, sizeof(full));
    json_raw(g_status, "asksLastHour", asks, sizeof(asks));
    json_raw(g_status, "answered", answered, sizeof(answered));
    json_raw(g_status, "refused", refused, sizeof(refused));
    json_raw(g_status, "announced", announced, sizeof(announced));
    json_raw(g_status, "followedCallsigns", follows, sizeof(follows));

    int isPublic = str_eq(pub, "true");

    str_copy(g_msg,
             "{\"type\":\"ui.stats.set\",\"field\":\"archive_dashboard\",\"tiles\":[",
             sizeof(g_msg));
    char ownC[16], folC[16], strC[16], totalC[16];
    compact(ownC, sizeof(ownC), own);
    compact(folC, sizeof(folC), fol);
    compact(strC, sizeof(strC), str);
    compact(totalC, sizeof(totalC), total);
    /* No `unit` on the count tiles: the unit shares the value's line, and on a
     * phone that squeezed "154k" down to "15…". The word costs nothing in the
     * hint underneath, where there is a whole line for it. */
    tile("mine", "Mine", ownC, "", "packets, always kept", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char hint[48];
        str_copy(hint, "packets, from ", sizeof(hint));
        str_cat(hint, follows, sizeof(hint));
        str_cat(hint, " callsigns", sizeof(hint));
        tile("followed", "People I follow", folC, "", hint, "", 0);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        /* The only shelf the limit bounds, so it is the only one with a bar. */
        char hint[64];
        str_copy(hint, isPublic ? "packets, of " : "not kept", sizeof(hint));
        if (isPublic) str_cat(hint, quotaText, sizeof(hint));
        tile("strangers", "Strangers", isPublic ? strC : "0", "", hint,
             isPublic ? full : "", 0);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char hint[32];
        str_copy(hint, totalC, sizeof(hint));
        str_cat(hint, " packets", sizeof(hint));
        tile("total", "On disk", bytes, "", hint, "", 0);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char hint[48];
        str_copy(hint, answered, sizeof(hint));
        str_cat(hint, " answered, ", sizeof(hint));
        str_cat(hint, refused, sizeof(hint));
        str_cat(hint, " refused", sizeof(hint));
        tile("asks", "Asked of us", asks, "an hour", hint, "", 0);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    /* What the beacon actually claims, so the screen and the air agree — the
     * whole reason this tab exists. */
    tile("announced", "On the air", announced[0] ? announced : "nothing", "",
         announced[0] ? "claimed on every beacon"
                      : "this device announces no role", "", 0);
    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);

    set_field_raw("public", pub);
    set_field_raw("keep_followed", followed);
    set_field_raw("always_on", always);
    set_field(  "quota_mb", quotaMb);
    set_field(  "max_days", maxDays);
    /* Always on is a promise only a public archiver can make, so the switch is
     * greyed until then rather than silently doing nothing. */
    set_field_raw("always_on__readonly", isPublic ? "false" : "true");
    (void)alwaysRaw;
}

static void push_dashboard(void) {
    int n = hal_archive_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char quota[16] = "0", usedText[24] = "0 B", quotaText[16] = "off";
    char items[16] = "0", served[16] = "0", freeable[24] = "0 B";
    char full[16] = "0";
    char followed[8] = "true", nearby[8] = "true", mirror[8] = "true";
    char reqAvg[16] = "0", reqLast[16] = "0";
    char bwAvg[24] = "0 B", bwLast[24] = "0 B";
    json_raw(g_status, "quotaGb", quota, sizeof(quota));
    json_raw(g_status, "usedText", usedText, sizeof(usedText));
    json_raw(g_status, "quotaText", quotaText, sizeof(quotaText));
    json_raw(g_status, "items", items, sizeof(items));
    json_raw(g_status, "servedItems", served, sizeof(served));
    json_raw(g_status, "freeableText", freeable, sizeof(freeable));
    json_raw(g_status, "fullFrac", full, sizeof(full));
    json_raw(g_status, "followed", followed, sizeof(followed));
    json_raw(g_status, "fromNearby", nearby, sizeof(nearby));
    json_raw(g_status, "mirrorSmall", mirror, sizeof(mirror));
    json_raw(g_status, "reqAvgPerHour", reqAvg, sizeof(reqAvg));
    json_raw(g_status, "reqLastHour", reqLast, sizeof(reqLast));
    json_raw(g_status, "bwPerHourText", bwAvg, sizeof(bwAvg));
    json_raw(g_status, "bwLastHourText", bwLast, sizeof(bwLast));
    json_raw(g_status, "reqSpark", g_reqSpark, sizeof(g_reqSpark));
    json_raw(g_status, "bwSpark", g_bwSpark, sizeof(g_bwSpark));

    int on = !str_eq(quota, "0");

    str_copy(g_msg, "{\"type\":\"ui.stats.set\",\"field\":\"dashboard\",\"tiles\":[", sizeof(g_msg));

    /* How full — the one number that matters, with the bar under it. */
    {
        char hint[64];
        if (on) {
            str_copy(hint, "of ", sizeof(hint));
            str_cat(hint, quotaText, sizeof(hint));
        } else {
            /* "7.7 MB of off" is not a sentence. With hosting off the number
             * is what is still on disk from when it was on, and the only
             * useful thing to say about it is that nothing more will join it. */
            str_copy(hint, "already here; hosting is off", sizeof(hint));
        }
        tile("used", "Storage used", usedText, "", hint, on ? full : "", 0);
    }
    str_cat(g_msg, ",", sizeof(g_msg));

    /* What other people asked for, over the last 48 hours. */
    {
        char hint[64];
        str_copy(hint, reqLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"req\",\"label\":\"Requests per hour\",\"value\":\"", sizeof(g_msg));
        str_cat(g_msg, reqAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_reqSpark[0] ? g_reqSpark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));

    /* And what it cost the uplink to give it to them. */
    {
        char hint[64];
        str_copy(hint, bwLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"bw\",\"label\":\"Bandwidth per hour\",\"value\":\"", sizeof(g_msg));
        str_cat(g_msg, bwAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_bwSpark[0] ? g_bwSpark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("files", "Files kept", items, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("served", "Ever fetched", served, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("freeable", "Can be freed", freeable, "", "held for others", "", 0);

    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);

    set_field_raw("enabled", on ? "true" : "false");
    if (on) set_field("quota", quota);
    set_field_raw("followed", str_eq(followed, "true") ? "true" : "false");
    set_field_raw("nearby", str_eq(nearby, "true") ? "true" : "false");
    set_field_raw("mirror", str_eq(mirror, "true") ? "true" : "false");
}

/* ── Directory: the pointer half of the archiver role (XPRS.md 36.9) ─────
 *
 * A dashboard first, because a role nobody can inspect is a role nobody
 * trusts: how often anyone actually asks (with the 48-hour shape, not just a
 * lifetime total that cannot say whether anyone came today), how many links
 * are held, for how many authors, and what the hygiene sweep removed. */
static void push_directory(void) {
    int n = hal_node_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char vol[16] = "auto";
    char pointers[16] = "0", authors[16] = "0", peers[16] = "0";
    char demoted[16] = "0";
    char qLast[16] = "0", qAvg[16] = "0";
    char ixKnown[16] = "0";
    json_raw(g_status, "volunteer", vol, sizeof(vol));
    json_raw(g_status, "pointers", pointers, sizeof(pointers));
    json_raw(g_status, "authors", authors, sizeof(authors));
    json_raw(g_status, "syncPeers", peers, sizeof(peers));
    json_raw(g_status, "demoted", demoted, sizeof(demoted));
    json_raw(g_status, "queriesLastHour", qLast, sizeof(qLast));
    json_raw(g_status, "queriesAvgPerHour", qAvg, sizeof(qAvg));
    json_raw(g_status, "querySpark", g_qSpark, sizeof(g_qSpark));
    /* Host key, not a label: hal_node_status still spells this one the old
     * way. What the tile below calls it is what the user reads. */
    json_raw(g_status, "indexersKnown", ixKnown, sizeof(ixKnown));

    str_copy(g_msg, "{\"type\":\"ui.stats.set\",\"field\":\"dir_dashboard\",\"tiles\":[",
             sizeof(g_msg));
    {
        char hint[64];
        str_copy(hint, qLast, sizeof(hint));
        str_cat(hint, " in the last hour", sizeof(hint));
        str_cat(g_msg, "{\"id\":\"rate\",\"label\":\"Queries per hour\",\"value\":\"",
                sizeof(g_msg));
        str_cat(g_msg, qAvg, sizeof(g_msg));
        str_cat(g_msg, "\",\"unit\":\"avg\",\"hint\":\"", sizeof(g_msg));
        str_cat(g_msg, hint, sizeof(g_msg));
        str_cat(g_msg, "\",\"spark\":", sizeof(g_msg));
        str_cat(g_msg, g_qSpark[0] ? g_qSpark : "[]", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("pointers", "Links", pointers, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("authors", "Authors", authors, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("sync", "Synced with", peers, "peers", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("hygiene", "Pruned", demoted, "", "", "", 0);
    str_cat(g_msg, ",", sizeof(g_msg));
    tile("network", "Directories", ixKnown, "", "", "", 0);
    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);

    /* Two switches, three states: off / auto (plugged-only) / always. */
    set_field_raw("dir_enabled", str_eq(vol, "off") ? "false" : "true");
    set_field_raw("dir_plugged", str_eq(vol, "always") ? "false" : "true");
}

/* ── My archivers (XPRS 36 + 13.12): the devices I trust to keep copies of my
 *    messages and answer "where can I find X1..". The one list this station
 *    pushes copies to AND declares daily as t:mailbox hold:. Edited here with
 *    the shared callsign finder (../hal/people_finder.h). ─────────────────── */

/* Render the SAVED list into the "myarch" people field, each with a Remove. */
static void push_myarch(void) {
    static char reply[2048];
    int n = hal_xprs_archivers(reply, sizeof(reply) - 1);
    if (n < 0) n = 0;
    reply[n] = '\0';
    /* {"list":["X3RLY7",..],"auto":true} — pull the array, then walk it; and
     * reflect the auto-select flag onto its switch. */
    static char list[2048]; list[0] = '\0';
    json_raw(reply, "list", list, sizeof(list));
    char autos[8] = ""; json_raw(reply, "auto", autos, sizeof(autos));
    set_field_raw("auto", str_eq(autos, "false") ? "false" : "true");
    str_copy(g_msg,
        "{\"type\":\"ui.people.set\",\"field\":\"myarch\",\"sections\":[{"
        "\"title\":\"My archivers\",\"items\":[", sizeof(g_msg));
    const char *p = list; int first = 1;
    while (*p) {
        if (*p != '"') { p++; continue; }
        p++;
        char call[24]; int k = 0;
        while (*p && *p != '"' && k < 23) call[k++] = *p++;
        call[k] = '\0';
        if (*p == '"') p++;
        if (!call[0]) continue;
        if (!first) str_cat(g_msg, ",", sizeof(g_msg));
        first = 0;
        str_cat(g_msg, "{\"id\":\"go:", sizeof(g_msg));
        str_cat(g_msg, call, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
        str_cat(g_msg, call, sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"holds copies of my messages\","
                       "\"icon\":\"archive\",\"buttons\":[{\"icon\":"
                       "\"delete\",\"action\":\"myarch_remove\",\"tip\":"
                       "\"Remove\"}]}", sizeof(g_msg));
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Add or remove [call] from the saved list, then persist via the XPRS HAL. */
static void arch_mutate(const char *call, int add) {
    static char reply[2048];
    int n = hal_xprs_archivers(reply, sizeof(reply) - 1);
    if (n < 0) n = 0;
    reply[n] = '\0';
    static char list[2048]; list[0] = '\0';
    json_raw(reply, "list", list, sizeof(list));
    static char csv[2048]; csv[0] = '\0';
    const char *p = list; int found = 0;
    while (*p) {
        if (*p != '"') { p++; continue; }
        p++;
        char c[24]; int k = 0;
        while (*p && *p != '"' && k < 23) c[k++] = *p++;
        c[k] = '\0';
        if (*p == '"') p++;
        if (!c[0]) continue;
        int same = str_eq(c, call);
        if (same) found = 1;
        if (same && !add) continue;           /* drop the one being removed */
        if (csv[0]) str_cat(csv, ",", sizeof(csv));
        str_cat(csv, c, sizeof(csv));
    }
    if (add && !found) {
        if (csv[0]) str_cat(csv, ",", sizeof(csv));
        str_cat(csv, call, sizeof(csv));
    }
    static char kv[2100];
    str_copy(kv, "archivers=", sizeof(kv));
    str_cat(kv, csv, sizeof(kv));
    hal_xprs_set_pref(kv, str_len(kv));
}

static void refresh(void) {
    push_archive();
    push_dashboard();
    push_directory();
    push_myarch();
}

static void set_pref(const char *kv) {
    hal_archive_set_pref(kv, str_len(kv));
    push_dashboard();
}

/* A switch the user flipped: read its new value, hand it to the host under the
 * name the host knows it by. */
static void toggle_from(const char *buf, const char *field, const char *key) {
    char v[8] = "";
    if (!json_raw(buf, field, v, sizeof(v))) return;
    char kv[48];
    str_copy(kv, key, sizeof(kv));
    str_cat(kv, "=", sizeof(kv));
    str_cat(kv, str_eq(v, "true") ? "1" : "0", sizeof(kv));
    set_pref(kv);
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[archiver] up", 13);
    /* The core says when the archive's counters moved -- once per flush, not
     * once per row, since a backlog drain writes hundreds in one transaction
     * and this dashboard re-reads the whole status either way. It used to ask
     * every five seconds whether anything had happened. */
    {
        static const char *t = "core.archive";
        hal_event_subscribe(t, str_len(t));
    }
    refresh();
    return 0;
}

/* The core says its state moved; redraw what depends on it. The event carries
 * a revision, not the data -- we read what we always read, when there is a
 * reason to rather than on a clock. */
static void drain_core_events(void) {
    static char topic[48];
    static char data[256];
    int any = 0;
    for (int guard = 0; guard < 16; guard++) {
        if (hal_event_available() == 0) break;
        if (hal_event_recv(topic, sizeof(topic) - 1, data, sizeof(data) - 1) == 0)
            break;
        any = 1;
    }
    if (any) refresh();
}

/* No clock: the archive changes when something is stored, and the core says
 * so. */
int32_t module_tick(void) {
    return 0;
}

int32_t module_handle_event(void) {
    drain_core_events();
    static char buf[2048];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';

    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        refresh();
    } else if (str_eq(cmd, "dir_enabled_changed") ||
               str_eq(cmd, "dir_plugged_changed")) {
        /* The directory offer, granted and revoked on its own: two switches
         * onto three states. Read BOTH every time -- "enabled" alone cannot
         * tell auto from always, and guessing the other one silently moves a
         * setting the owner did not touch. */
        char en[8] = "", pl[8] = "";
        json_raw(buf, "dir_enabled", en, sizeof(en));
        json_raw(buf, "dir_plugged", pl, sizeof(pl));
        const char *state = str_eq(en, "true")
                                ? (str_eq(pl, "false") ? "always" : "auto")
                                : "off";
        char kv[32];
        str_copy(kv, "volunteer=", sizeof(kv));
        str_cat(kv, state, sizeof(kv));
        hal_node_set_pref(kv, str_len(kv));
        push_directory();
    /* ── The packet archive: the three tiers of XPRS.md 12. ── */
    } else if (str_eq(cmd, "public_changed")) {
        char v[8] = "";
        json_raw(buf, "public", v, sizeof(v));
        const char *kv = str_eq(v, "true") ? "public=1" : "public=0";
        hal_xprs_set_pref(kv, str_len(kv));
        push_archive();
    } else if (str_eq(cmd, "always_on_changed")) {
        /* A promise only a public archiver can make. The core gates it too;
         * refusing here as well means the switch never shows a state this
         * station is not actually in. */
        char v[8] = "", pub[8] = "false";
        json_raw(buf, "always_on", v, sizeof(v));
        json_raw(buf, "public", pub, sizeof(pub));
        if (str_eq(v, "true") && !str_eq(pub, "true")) {
            set_field_raw("always_on", "false");
        } else {
            const char *kv = str_eq(v, "true") ? "alwaysOn=1" : "alwaysOn=0";
            hal_xprs_set_pref(kv, str_len(kv));
            push_archive();
        }
    } else if (str_eq(cmd, "keep_followed_changed")) {
        char v[8] = "";
        json_raw(buf, "keep_followed", v, sizeof(v));
        const char *kv =
            str_eq(v, "true") ? "keepFollowed=1" : "keepFollowed=0";
        hal_xprs_set_pref(kv, str_len(kv));
        push_archive();
    } else if (str_eq(cmd, "quota_mb_changed")) {
        char v[16] = "";
        json_raw(buf, "quota_mb", v, sizeof(v));
        if (v[0]) {
            char kv[32];
            str_copy(kv, "archiveMaxMb=", sizeof(kv));
            str_cat(kv, v, sizeof(kv));
            hal_xprs_set_pref(kv, str_len(kv));
            push_archive();
        }
    } else if (str_eq(cmd, "max_days_changed")) {
        char v[16] = "";
        json_raw(buf, "max_days", v, sizeof(v));
        if (v[0]) {
            char kv[32];
            str_copy(kv, "archiveMaxDays=", sizeof(kv));
            str_cat(kv, v, sizeof(kv));
            hal_xprs_set_pref(kv, str_len(kv));
            push_archive();
        }
    } else if (str_eq(cmd, "enabled_changed")) {
        /* Enabling takes whatever limit the picker shows; disabling means zero,
         * and zero means this device holds nothing for anybody. */
        char en[8] = "", q[16] = "5";
        json_raw(buf, "enabled", en, sizeof(en));
        json_raw(buf, "quota", q, sizeof(q));
        char kv[32];
        str_copy(kv, "quotaGb=", sizeof(kv));
        str_cat(kv, str_eq(en, "true") ? (q[0] && !str_eq(q, "0") ? q : "5") : "0",
                sizeof(kv));
        set_pref(kv);
    } else if (str_eq(cmd, "quota_changed")) {
        char q[16] = "";
        if (json_raw(buf, "quota", q, sizeof(q)) && q[0]) {
            char kv[32];
            str_copy(kv, "quotaGb=", sizeof(kv));
            str_cat(kv, q, sizeof(kv));
            set_pref(kv);
        }
    } else if (str_eq(cmd, "followed_changed")) {
        toggle_from(buf, "followed", "followed");
    } else if (str_eq(cmd, "nearby_changed")) {
        toggle_from(buf, "nearby", "fromNearby");
    } else if (str_eq(cmd, "mirror_changed")) {
        toggle_from(buf, "mirror", "mirrorSmall");
    } else if (str_eq(cmd, "free_space")) {
        /* Give back everything held for OTHER people — strangers and followed
         * authors alike. The owner's own files, and anything they pinned, are
         * not the archive's to delete. The host raises a notification saying
         * how much came back (including "nothing", which is a real outcome and
         * must not look like a dead button). */
        const char *id = "sweep:all";
        hal_archive_drop(id, str_len(id));
        push_dashboard();
    } else if (str_eq(cmd, "myarch_search")) {
        /* Empty query shows the saved list; a query searches heard callsigns
         * with the shared finder (36.3 — the list is the operator's choice). */
        char q[64] = "";
        json_raw(buf, "myarch_query", q, sizeof(q));
        if (q[0]) pf_render("myarch", q, 0);
        else push_myarch();
    } else if (str_eq(cmd, "myarch_tap")) {
        /* Tapping a search result adds it (idempotent); a saved row's tap is a
         * harmless re-add. */
        char call[24];
        if (pf_pick(buf, "myarch", call, sizeof(call))) {
            const char *c = call[0] == '#' ? call + 1 : call; /* archivers are stations */
            arch_mutate(c, 1);
            push_myarch();
        }
    } else if (str_eq(cmd, "auto_changed")) {
        char a[8] = ""; json_raw(buf, "auto", a, sizeof(a));
        char kv[24]; str_copy(kv, "archiverAuto=", sizeof(kv));
        str_cat(kv, str_eq(a, "true") ? "1" : "0", sizeof(kv));
        hal_xprs_set_pref(kv, str_len(kv));
    } else if (str_eq(cmd, "myarch_remove")) {
        char call[24];
        if (pf_pick(buf, "myarch", call, sizeof(call))) {
            const char *c = call[0] == '#' ? call + 1 : call;
            arch_mutate(c, 0);
            push_myarch();
        }
    }
    return 0;
}

/* 0 = no clock: see module_tick. */
int32_t module_tick_interval_ms(void) { return 0; }

int32_t module_destroy(void) { return 0; }
