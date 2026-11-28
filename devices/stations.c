/*
 * firmwares -- install XPRS on a board, set up the station you just flashed
 * (XPRS.md 11.9, 11.10), keep it updated (11.8) and read how it is doing
 * (15.5).
 *
 * One list, no tabs: the boards on a USB cable, then the stations heard on
 * WiFi, Bluetooth or LoRa (waiting for an owner, yours, other people's),
 * then, with nothing on the cable, how to plug a board in. Each row is drawn
 * with how it is reached (usb, wifi, bluetooth), never a random pattern.
 * A tap opens what it is:
 *
 *   Flash     a board on the cable: its picture, one line saying what
 *             happens next, a progress bar while it works, what is known
 *             about it, and one button, Install XPRS. Advanced options hold
 *             the rest: another board, a chosen callsign, erase settings.
 *   Station   a station on the air: its firmware (and whether a newer one
 *             is published), how long it has run, how it is heard, whose it
 *             is and how it is set, as plain rows; then what can be done:
 *             Claim, Update firmware, WiFi, Settings, Callsign, Statistics,
 *             History.
 *
 * Back is the host's arrow and the phone's gesture: `screen_closed` steps
 * back one panel (WiFi -> Station -> the list), so no screen carries a Back
 * button of its own.
 *
 * A freshly flashed station has no owner, no network and no name, and says
 * so: it airs `t:request q:owner` with its key in `k:` on its local bearers.
 * This wapp hears that on `xprs.request`, tells the person holding the phone
 * that a station nearby can be set up, and then, one small screen per task:
 *
 *   Station   what it is and how it stands; Claim while nobody owns it
 *   WiFi      network and password, sealed in x: to the station's key
 *   Name      nick, time zone, its own hotspot
 *   Identity  a new key the station makes, or an nsec, sealed; or the
 *             callsign an authority issued its owner (6.4.2), same key
 *   Stats     15.5 telemetry the core already heard, the policy (11.9,
 *             anybody may ask), and the firmware's own diagnostics
 *   Answers   every answer the station gave
 *
 * And before any of that, the Flash tab: a board on a USB cable (an OTG
 * cable on the phone) gets an XPRS image from xprs.dev written to it. The
 * core identifies the chip and what it runs, says which published boards
 * fit, downloads and writes the image and verifies it (hal_flash_*); this
 * wapp picks and draws. While it still holds the cable the core also makes
 * the board the active profile's, on its console (11.9: whoever holds the
 * cable can reflash it anyway), so a board flashed here never waits for an
 * owner on the air, where anybody nearby could answer first; it joins the
 * list as yours when `core.flash` says so (on_claim).
 *
 * Every packet goes through hal_xprs_send and every secret through
 * hal_encrypt: the core owns the radio, the signature and the key, and this
 * wapp never learns which bearer carried anything (docs/architecture.md).
 * Passwords arrive in `$type:"secret"` fields, which the host never stores,
 * are sealed the moment they are read, and the buffers are cleared.
 *
 * Event-driven, no clock, no transport. A command is handed to the core once;
 * the core airs it again until the station answers and says so on
 * `xprs.status.tx` when it never does. This wapp listens for answers only
 * while it has asked something, for identities only while a station is
 * changing its key, and for observations only while a policy ask is out: an
 * idle phone hands it nothing but the rare ask to be claimed.
 */
#include "../hal/xprs_wasm_hal.h"
#include "wire.h"
#include "devices.h"

#define ST_MAX       16

typedef struct {
    char call[12];
    char npub[70];
    int  asking;                  /* a Refresh is out: empty tiles read "..." */
    /* What it said about itself (11.10 results, zdiag, q:policy, q:mail). */
    char nick[20], wifi[12], ip[20], ap[4], zone[8];
    char lora[12];                /* its LoRa mode (14.8); empty: no radio */
    char freq[16];                /* the channel it is on, as it reported it */
    char fw[24], uptime[16], peers[8], heap[20], reset[16];
    char health[16], slot[16], radio[48], crash[32], mail[8];
    char count[12], room[12];     /* records held, room left: an owner's ask */
    char life[16];
    int  soft;                    /* a phone or a computer, not a board: it
                                   * answered the diagnostics ask with what it
                                   * holds and none of a board's own figures */
    char pol_owner[40], pol_use[12], pol_first[40], pol_serve[40];
    char bearer[8];
    int  rssi;
    unsigned long long heard_ms;
    int  unowned;                 /* asked to be claimed */
    int  mine;                    /* claimed by this profile */
    int  theirs;                  /* owned by somebody else */
    /* The one command in flight to it. The wire is kept only to say it
     * again after a 408, under a newer stamp. */
    char pend[FW_WIRE_UNSIGNED + 1];
    char pend_id[7];
    char pend_what[8];            /* claim wifi set key zdiag zcore */
    int  pend_408, pend_final;
    char last_what[8];            /* what a late final answer was to */
    char next_x[FW_WIRE_UNSIGNED];/* the second half of a long ssid+pass */
    char rekey_npub[70];          /* following a new key (11.10) */
    char want_call[12];           /* the callsign a call: asked for */
    unsigned long long last_ts;   /* the last ts: we used with it */
    int  ask_pol;                 /* a q:policy is out: observations wanted */
    int  probe;                   /* heard, not yet known to be ours: asked */
    char now[96];                 /* the one line the Station screen shows */
} st_t;

static st_t g_st[ST_MAX];
static int  g_nst;
static int  g_sel = -1;
static int  g_sub_res, g_sub_tx, g_sub_ids, g_sub_obs;
static char g_me[16];
static char g_dev_call[16];         /* this device's own station, if it has one */
static char g_my_npub[70];

/* The last few identities heard, so a station that changed its key can be
 * followed even when its 202 k: was missed: its 200 then arrives from a
 * callsign this table names (11.10). */
static struct { char call[12]; char npub[70]; } g_ids[4];
static int g_ids_w;

/* A callsign chosen on the Flash panel (ui.callsign.pick): the key that
 * derives it, held in memory only, for the next board flashed here. Once
 * the cable has made that board ours, [g_key_for] names it, and the key is
 * sent to it sealed (give_key) as soon as the core has heard its own key. */
static char g_want_call[12], g_want_nsec[72], g_key_for[12];

static void forget_wanted(void)
{
    for (unsigned i = 0; i < sizeof g_want_nsec; i++) g_want_nsec[i] = 0;
    g_want_call[0] = 0;
    g_key_for[0] = 0;
}

#define g_ev    dv_ev
#define g_topic dv_topic
#define g_buf   dv_buf
static char g_out[8192];
static char g_host[1536];         /* one hal_xprs_station answer */
static char g_heard[16384];       /* one hal_xprs_stations answer: a busy
                                   * place with Meshtastic and MeshCore nodes
                                   * heard this hour is well past 4 KB */
/* Stations that said they are somebody else's, this session: not asked
 * again every time the page opens. */
static char g_notmine[8][12];
static int  g_notmine_w;

static void say(const char *json) { hal_msg_send(json, dv_len(json)); }

/* ── Who we are ───────────────────────────────────────────────────────── */
static void who_am_i(void)
{
    uint32_t n = hal_identity(g_me, sizeof g_me - 1);
    g_me[n] = 0;
    n = hal_identity_device(g_dev_call, sizeof g_dev_call - 1);
    g_dev_call[n] = 0;
    char pk[80];
    n = hal_identity_pubkey(pk, sizeof pk - 1);
    pk[n] = 0;
    if (dv_starts(pk, "npub1")) {
        dv_cpy(g_my_npub, pk, sizeof g_my_npub);
    } else if (n) {
        n = hal_npub(pk, dv_len(pk), g_my_npub, sizeof g_my_npub - 1);
        g_my_npub[n] = 0;
    }
}

/* ── The stations, kept across the engine's restarts ─────────────────── */
static int find(const char *call)
{
    for (int i = 0; i < g_nst; i++) if (dv_eq(g_st[i].call, call)) return i;
    return -1;
}

static int is_this_device(const st_t *s)
{
    return g_dev_call[0] && dv_eq(s->call, g_dev_call);
}

static int add(const char *call)
{
    int i = find(call);
    if (i >= 0) return i;
    if (g_nst >= ST_MAX) {
        /* Forget the longest-unheard one that is not ours. */
        int old = -1;
        for (int j = 0; j < g_nst; j++)
            if (!g_st[j].mine && (old < 0 || g_st[j].heard_ms < g_st[old].heard_ms)) old = j;
        if (old < 0) return -1;
        i = old;
    } else {
        i = g_nst++;
    }
    st_t *s = &g_st[i];
    for (unsigned k = 0; k < sizeof *s; k++) ((char *)s)[k] = 0;
    dv_cpy(s->call, call, sizeof s->call);
    return i;
}

static void drop(int i)
{
    if (i < 0 || i >= g_nst) return;
    if (g_sel == i) g_sel = -1;
    else if (g_sel > i) g_sel--;
    for (int j = i; j + 1 < g_nst; j++) g_st[j] = g_st[j + 1];
    g_nst--;
    for (unsigned k = 0; k < sizeof g_st[g_nst]; k++) ((char *)&g_st[g_nst])[k] = 0;
}

static void kv_put(const char *k, const char *v) { hal_kv_set(k, dv_len(k), v, dv_len(v)); }

static void save(void)
{
    char list[ST_MAX * 13] = "";
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (!s->mine && !s->unowned && !s->theirs) continue;
        if (list[0]) dv_cat(list, ",", sizeof list);
        dv_cat(list, s->call, sizeof list);
        char k[24] = "st.", v[320] = "";
        dv_cat(k, s->call, sizeof k);
        dv_cat(v, s->npub, sizeof v);
        dv_cat(v, s->mine ? "|1|" : "|0|", sizeof v);
        dv_cat(v, s->unowned ? "1|" : "0|", sizeof v);
        dv_cat(v, s->nick, sizeof v);
        dv_cat(v, "|", sizeof v);
        dv_cat(v, s->fw, sizeof v);
        dv_cat(v, s->theirs ? "|1|" : "|0|", sizeof v);
        dv_cat(v, s->wifi, sizeof v);
        dv_cat(v, "|", sizeof v);
        dv_cat(v, s->ip, sizeof v);
        /* The policy it last stated, so the Stats screen opens with it. */
        dv_cat(v, "|", sizeof v); dv_cat(v, s->pol_owner, sizeof v);
        dv_cat(v, "|", sizeof v); dv_cat(v, s->pol_use, sizeof v);
        dv_cat(v, "|", sizeof v); dv_cat(v, s->pol_first, sizeof v);
        dv_cat(v, "|", sizeof v); dv_cat(v, s->pol_serve, sizeof v);
        kv_put(k, v);
    }
    kv_put("st.list", list);
}

/* What the station last answered to cmd:zdiag, so the Stats screen opens
 * with the last known figures and says when they are being refreshed. */
static void save_stats(const st_t *s)
{
    char k[24] = "stx.", v[220] = "";
    dv_cat(k, s->call, sizeof k);
    const char *f[] = { s->uptime, s->peers, s->heap, s->reset, s->health, s->slot, s->radio, s->crash, s->mail };
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (i) dv_cat(v, "|", sizeof v);
        dv_cat(v, f[i], sizeof v);
    }
    kv_put(k, v);
}

static void load_stats(st_t *s)
{
    char k[24] = "stx.", v[220];
    dv_cat(k, s->call, sizeof k);
    uint32_t n = hal_kv_get(k, dv_len(k), v, sizeof v - 1);
    v[n] = 0;
    if (!n) return;
    char *f[9] = {0};
    f[0] = v;
    for (int j = 1, q = 0; v[q] && j < 9; q++)
        if (v[q] == '|') { v[q] = 0; f[j++] = v + q + 1; }
    char *dst[] = { s->uptime, s->peers, s->heap, s->reset, s->health, s->slot, s->radio, s->crash, s->mail };
    unsigned caps[] = { sizeof s->uptime, sizeof s->peers, sizeof s->heap, sizeof s->reset, sizeof s->health,
                        sizeof s->slot, sizeof s->radio, sizeof s->crash, sizeof s->mail };
    for (int i = 0; i < 9; i++) if (f[i]) dv_cpy(dst[i], f[i], caps[i]);
}

static void load(void)
{
    char list[ST_MAX * 13 + 1];
    uint32_t n = hal_kv_get("st.list", 7, list, sizeof list - 1);
    list[n] = 0;
    for (char *p = list; *p; ) {
        char call[12];
        unsigned c = 0;
        while (*p && *p != ',' && c < sizeof call - 1) call[c++] = *p++;
        call[c] = 0;
        if (*p == ',') p++;
        if (!call[0]) continue;
        char k[24] = "st.", v[320];
        dv_cat(k, call, sizeof k);
        n = hal_kv_get(k, dv_len(k), v, sizeof v - 1);
        v[n] = 0;
        int i = add(call);
        if (i < 0) break;
        st_t *s = &g_st[i];
        /* npub|mine|unowned|nick|fw|theirs|wifi|ip|owner|use|first|serve */
        char *f[12] = {0};
        f[0] = v;
        for (int j = 1, q = 0; v[q] && j < 12; q++)
            if (v[q] == '|') { v[q] = 0; f[j++] = v + q + 1; }
        dv_cpy(s->npub, f[0], sizeof s->npub);
        s->mine = f[1] && f[1][0] == '1';
        s->unowned = f[2] && f[2][0] == '1';
        if (f[3]) dv_cpy(s->nick, f[3], sizeof s->nick);
        if (f[4]) dv_cpy(s->fw, f[4], sizeof s->fw);
        s->theirs = f[5] && f[5][0] == '1';
        if (f[6]) dv_cpy(s->wifi, f[6], sizeof s->wifi);
        if (f[7]) dv_cpy(s->ip, f[7], sizeof s->ip);
        if (f[8]) dv_cpy(s->pol_owner, f[8], sizeof s->pol_owner);
        if (f[9]) dv_cpy(s->pol_use, f[9], sizeof s->pol_use);
        if (f[10]) dv_cpy(s->pol_first, f[10], sizeof s->pol_first);
        if (f[11]) dv_cpy(s->pol_serve, f[11], sizeof s->pol_serve);
        load_stats(s);
    }
}

/* ── What the core holds about a station (hal_xprs_station) ──────────── */
/* The phone or computer this runs on is a station too, and its profiles own
 * it (docs/architecture.md, "The device and its people"): listed as yours
 * with no claim and no ask, since nothing has to be learned over the air. */
static void this_device(void)
{
    if (!g_dev_call[0] || !g_me[0]) return;
    int i = add(g_dev_call);
    if (i < 0) return;
    st_t *s = &g_st[i];
    s->mine = 1; s->unowned = 0; s->theirs = 0; s->probe = 0;
    if (!s->pol_owner[0]) dv_cpy(s->pol_owner, g_me, sizeof s->pol_owner);
}

/* The stations the core knows to be ours by their own signed word (11.9):
 * a phone at home that named us an administrator, a board claimed from
 * another device. They are ours wherever they are, so they are listed
 * without having been heard here, which is what lets one be administered
 * from somewhere it has never been in earshot. */
static void owned_from_core(void)
{
    char list[400], call[12];
    int32_t n = hal_xprs_owned(list, sizeof list - 1);
    if (n <= 0) return;
    list[n] = 0;
    for (const char *p = dv_json_top(list); p; ) {
        p = dv_json_next_str(p, call, sizeof call);
        if (!p || !call[0]) break;
        int was = find(call);
        if (was >= 0 && g_st[was].mine) continue;
        int i = add(call);
        if (i < 0) break;
        st_t *s = &g_st[i];
        s->mine = 1; s->unowned = 0; s->theirs = 0; s->probe = 0;
    }
}

/* Fills g_host; 1 when the core has heard it this hour. */
static int host_facts(const st_t *s)
{
    int n = hal_xprs_station(s->call, dv_len(s->call), g_host, sizeof g_host - 1);
    if (n <= 0) { g_host[0] = 0; return 0; }
    g_host[n] = 0;
    return 1;
}

/* The station's key, from the core, for one we did not hear ask to be
 * claimed (owned by cable, or claimed before this phone's list was made):
 * without it nothing can be sealed to it. Call right after host_facts. */
static void key_from_core(st_t *s, int have)
{
    char k[72];
    if (s->npub[0] || !have) return;
    if (dv_json(g_host, "npub", k, sizeof k) && dv_starts(k, "npub1"))
        dv_cpy(s->npub, k, sizeof s->npub);
}

static unsigned long long host_num(const char *key)
{
    char v[24];
    if (!dv_json(g_host, key, v, sizeof v)) return 0;
    unsigned long long n = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (unsigned)(*p - '0');
    return n;
}

static void ago_words(unsigned long long ms, char *out, unsigned cap)
{
    dv_cpy(out, "", cap);
    if (ms < 60000ULL)         { dv_cat_u(out, ms / 1000, cap); dv_cat(out, " s ago", cap); }
    else if (ms < 3600000ULL)  { dv_cat_u(out, ms / 60000, cap); dv_cat(out, " min ago", cap); }
    else                       { dv_cat_u(out, ms / 3600000, cap); dv_cat(out, " h ago", cap); }
}

/* "BLE -87 dBm" or "LAN", from the core's last sighting, else what the ask
 * carried. [rssi_out], when given, gets the level alone for a tile. */
static void signal_parts(const st_t *s, int have_host, char *out, unsigned cap, int *rssi_out)
{
    char b[8] = "";
    int rssi = s->rssi;
    if (have_host) {
        dv_json(g_host, "bearer", b, sizeof b);
        char r[12];
        if (dv_json(g_host, "rssi", r, sizeof r)) {
            int neg = r[0] == '-', v = 0;
            for (const char *p = r + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
            rssi = neg ? -v : v;
        }
    }
    if (!b[0]) dv_cpy(b, s->bearer, sizeof b);
    out[0] = 0;
    for (unsigned j = 0; b[j] && j < sizeof b; j++) {
        char c[2] = { dv_up(b[j]), 0 };
        dv_cat(out, c, cap);
    }
    if (dv_eq(out, "BLE5")) dv_cpy(out, "BLE", cap);
    if (rssi_out) { *rssi_out = rssi; if (!out[0]) dv_cpy(out, "not heard", cap); return; }
    if (rssi) { dv_cat(out, " -", cap); dv_cat_u(out, (unsigned)(rssi < 0 ? -rssi : rssi), cap); dv_cat(out, " dBm", cap); }
    if (!out[0]) dv_cpy(out, "not heard", cap);
}


static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, const char *progress, int alert);

/* The Signal tile: the level as the number, the bearer and the age as the
 * hint, so a phone-width tile is not cut short. */
static void signal_tile(const st_t *s, int have_host, unsigned long long agoms, int alert)
{
    char b[24], hint[48] = "", num[12] = "";
    int rssi = 0;
    signal_parts(s, have_host, b, sizeof b, &rssi);
    if (rssi) { dv_cpy(num, "-", sizeof num); dv_cat_u(num, (unsigned)(rssi < 0 ? -rssi : rssi), sizeof num); }
    dv_cpy(hint, b, sizeof hint);
    if (have_host) { char ago[24]; ago_words(agoms, ago, sizeof ago); dv_cat(hint, ", ", sizeof hint); dv_cat(hint, ago, sizeof hint); }
    tile("signal", "Signal", rssi ? num : b, rssi ? "dBm" : "", rssi || have_host ? hint : "", "", alert);
}

/* ── Screens ──────────────────────────────────────────────────────────── */
static void log_line(const char *call, const char *text)
{
    char m[400] = "{\"type\":\"ui.log.append\",\"field\":\"log\",\"line\":\"";
    if (call && call[0]) { dv_jesc(m, call, sizeof m); dv_cat(m, ": ", sizeof m); }
    dv_jesc(m, text, sizeof m);
    dv_cat(m, "\"}", sizeof m);
    say(m);
    /* And the host's log: a headless engine's lines reach /api/log, which
     * is where a setup that went wrong gets read afterwards. Only what a
     * person did or a station answered comes through here, never a packet. */
    char l[200] = "firmwares: ";
    if (call && call[0]) { dv_cat(l, call, sizeof l); dv_cat(l, ": ", sizeof l); }
    dv_cat(l, text, sizeof l);
    hal_log(1, l, dv_len(l));
}

static void field_set(const char *field, const char *value)
{
    char m[200] = "{\"type\":\"ui.field.set\",\"field\":\"";
    dv_cat(m, field, sizeof m);
    dv_cat(m, "\",\"value\":\"", sizeof m);
    dv_jesc(m, value, sizeof m);
    dv_cat(m, "\"}", sizeof m);
    say(m);
}

/* `<name>__hidden`: the host leaves the button or field off the screen. */
static void flag_hidden(const char *name, int hidden)
{
    char m[120] = "{\"type\":\"ui.field.set\",\"field\":\"";
    dv_cat(m, name, sizeof m);
    dv_cat(m, hidden ? "__hidden\",\"value\":true}" : "__hidden\",\"value\":false}", sizeof m);
    say(m);
}

/* The panel open now, so `screen_closed` (the host's back arrow, or the
 * phone's back gesture) can step back to the screen it came from instead of
 * dropping the person on the list. */
static char g_screen[16];

/* What the Flash panel was last opened as, to retitle it when the board
 * turns out to be a T-Deck rather than "Install XPRS". */
static char g_flash_shown[40];

static void screen_open(const char *name, const char *title)
{
    dv_cpy(g_screen, name, sizeof g_screen);
    dv_front(DV_ST);
    if (dv_eq(name, "Flash")) dv_cpy(g_flash_shown, title, sizeof g_flash_shown);
    char m[160] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    dv_cat(m, name, sizeof m);
    dv_cat(m, "\",\"title\":\"", sizeof m);
    dv_jesc(m, title, sizeof m);
    dv_cat(m, "\"}", sizeof m);
    say(m);
}

/* One details section with one line, for a "what is happening" row. */
static void one_line(const char *field, const char *title, const char *text)
{
    char m[300] = "{\"type\":\"ui.field.set\",\"field\":\"";
    dv_cat(m, field, sizeof m);
    if (!text || !text[0]) { dv_cat(m, "\",\"value\":[]}", sizeof m); say(m); return; }
    dv_cat(m, "\",\"value\":[{\"title\":\"\",\"items\":[{\"label\":\"", sizeof m);
    dv_jesc(m, title, sizeof m);
    dv_cat(m, "\",\"value\":\"", sizeof m);
    dv_jesc(m, text, sizeof m);
    dv_cat(m, "\"}]}]}", sizeof m);
    say(m);
}

/* The latest thing about a station, on its screen and the WiFi screen. */
static void set_now(st_t *s, const char *text)
{
    dv_cpy(s->now, text, sizeof s->now);
    if (g_sel >= 0 && &g_st[g_sel] == s) {
        one_line("now", "Now", s->now);
        one_line("wifi_now", "Now", s->now);
        one_line("admins_now", "Now", s->now);
    }
}

/* ── Stats tiles ──────────────────────────────────────────────────────── */
static void tiles_begin(const char *field)
{
    dv_cpy(g_out, "{\"type\":\"ui.stats.set\",\"field\":\"", sizeof g_out);
    dv_cat(g_out, field, sizeof g_out);
    dv_cat(g_out, "\",\"tiles\":[", sizeof g_out);
}

static const char *g_empty = "?";   /* what an empty tile shows */

static void tile(const char *id, const char *label, const char *value,
                 const char *unit, const char *hint, const char *progress, int alert)
{
    unsigned n = dv_len(g_out);
    if (g_out[n - 1] != '[') dv_cat(g_out, ",", sizeof g_out);
    dv_cat(g_out, "{\"id\":\"", sizeof g_out);
    dv_jesc(g_out, id, sizeof g_out);
    dv_cat(g_out, "\",\"label\":\"", sizeof g_out);
    dv_jesc(g_out, label, sizeof g_out);
    dv_cat(g_out, "\",\"value\":\"", sizeof g_out);
    dv_jesc(g_out, value && value[0] ? value : g_empty, sizeof g_out);
    dv_cat(g_out, "\"", sizeof g_out);
    if (unit && unit[0]) { dv_cat(g_out, ",\"unit\":\"", sizeof g_out); dv_jesc(g_out, unit, sizeof g_out); dv_cat(g_out, "\"", sizeof g_out); }
    if (hint && hint[0]) { dv_cat(g_out, ",\"hint\":\"", sizeof g_out); dv_jesc(g_out, hint, sizeof g_out); dv_cat(g_out, "\"", sizeof g_out); }
    if (progress && progress[0]) { dv_cat(g_out, ",\"progress\":", sizeof g_out); dv_cat(g_out, progress, sizeof g_out); }
    if (alert) dv_cat(g_out, ",\"alert\":true", sizeof g_out);
    dv_cat(g_out, "}", sizeof g_out);
}

static void tiles_end(void)
{
    dv_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* ── Words a person reads ─────────────────────────────────────────────── */
/* The newest firmware the catalogue offers (flash_read keeps it), so a
 * station heard on the air can be told it has an update. Stations do not
 * say which board they are, so this is the newest of any board: every
 * board is published at one version, and a station behind it is behind. */
static char g_latest[24];

/* What a bearer is called on the screen. */
static const char *bearer_name(const char *b)
{
    if (dv_starts(b, "ble")) return "Bluetooth";
    if (dv_eq(b, "lan")) return "WiFi network";
    if (dv_eq(b, "wifi")) return "WiFi";
    if (dv_eq(b, "lora")) return "LoRa radio";
    if (dv_eq(b, "rns") || dv_eq(b, "reticulum")) return "Reticulum";
    return b[0] ? b : "";
}

/* Its icon on a list row: how it is reached, never a random pattern. */
static const char *bearer_icon(const char *b)
{
    if (dv_starts(b, "ble")) return "bluetooth";
    if (dv_eq(b, "lan") || dv_eq(b, "wifi")) return "wifi";
    if (dv_eq(b, "lora")) return "cell_tower";
    return "router";
}

/* The bearer it was last heard on, from the core or from its ask. */
static void bearer_of(const st_t *s, int have, char *out, unsigned cap)
{
    out[0] = 0;
    if (have) dv_json(g_host, "bearer", out, cap);
    if (!out[0]) dv_cpy(out, s->bearer, cap);
    for (char *p = out; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
}

/* "Bluetooth, -87 dBm, 12 s ago", or "not heard lately". */
static void reach_words(const st_t *s, int have, char *out, unsigned cap)
{
    char b[12];
    bearer_of(s, have, b, sizeof b);
    int rssi = 0;
    signal_parts(s, have, out, cap, &rssi);
    dv_cpy(out, bearer_name(b), cap);
    if (!out[0]) { dv_cpy(out, "not heard lately", cap); return; }
    if (rssi) { dv_cat(out, ", -", cap); dv_cat_u(out, (unsigned)(rssi < 0 ? -rssi : rssi), cap); dv_cat(out, " dBm", cap); }
    if (have) { char ago[24]; ago_words(host_num("agoMs"), ago, sizeof ago); dv_cat(out, ", ", cap); dv_cat(out, ago, cap); }
}

/* The firmware version it runs: what the core heard last, else what it
 * told us. */
static void fw_of(const st_t *s, int have, char *out, unsigned cap)
{
    out[0] = 0;
    if (have) dv_json(g_host, "fw", out, cap);
    if (!out[0]) dv_cpy(out, s->fw, cap);
}

static int behind(const char *fw) { return fw[0] && g_latest[0] && dv_ver_cmp(fw, g_latest) < 0; }

/* When this device installed its firmware over the cable (kv fl.<call>:
 * "<epoch>|<version>"), as "3 days ago". 0 when it did not. */
static int installed_words(const char *call, char *out, unsigned cap)
{
    char k[24] = "fl.", v[48];
    dv_cat(k, call, sizeof k);
    uint32_t n = hal_kv_get(k, dv_len(k), v, sizeof v - 1);
    v[n] = 0;
    unsigned long long at = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) at = at * 10 + (unsigned)(*p - '0');
    unsigned long long now = hal_time_epoch();
    if (!at || !now || now < at) return 0;
    unsigned long long d = (now - at) / 86400ULL;
    out[0] = 0;
    if (d == 0) dv_cpy(out, "today", cap);
    else if (d == 1) dv_cpy(out, "yesterday", cap);
    else { dv_cat_u(out, d, cap); dv_cat(out, " days ago", cap); }
    dv_cat(out, ", from this device", cap);
    return 1;
}

/* ── Details: label and value rows, in titled sections ───────────────── */
static int g_det_first, g_det_rows;

static void det_begin(const char *field)
{
    dv_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"", sizeof g_out);
    dv_cat(g_out, field, sizeof g_out);
    dv_cat(g_out, "\",\"value\":[", sizeof g_out);
    g_det_first = 1;
    g_det_rows = 0;
}

static void det_section(const char *title)
{
    if (!g_det_first) dv_cat(g_out, "]},", sizeof g_out);
    g_det_first = 0;
    dv_cat(g_out, "{\"title\":\"", sizeof g_out);
    dv_jesc(g_out, title, sizeof g_out);
    dv_cat(g_out, "\",\"items\":[", sizeof g_out);
    g_det_rows = 0;
}

/* A row with nothing to say is left out rather than shown as "?". */
static void det_row(const char *label, const char *value)
{
    if (!value || !value[0]) return;
    if (g_det_rows++) dv_cat(g_out, ",", sizeof g_out);
    dv_cat(g_out, "{\"label\":\"", sizeof g_out);
    dv_jesc(g_out, label, sizeof g_out);
    dv_cat(g_out, "\",\"value\":\"", sizeof g_out);
    dv_jesc(g_out, value, sizeof g_out);
    dv_cat(g_out, "\"}", sizeof g_out);
}

static void det_end(void)
{
    if (!g_det_first) dv_cat(g_out, "]}", sizeof g_out);
    dv_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* The Station screen: what can be learned about it, then what can be done.
 * Facts as rows a person reads top to bottom; the buttons that do not apply
 * to this station (not yours, nothing newer) are left off. */
static void push_hub(void)
{
    if (g_sel < 0 || g_sel >= g_nst) return;
    st_t *s = &g_st[g_sel];
    int have = host_facts(s);
    char v[96], w[64];

    if (!s->now[0])
        dv_cpy(s->now, is_this_device(s) ? "This device. You administer it."
                     : s->mine ? "Yours" : s->unowned ? "Waiting for an owner. Claim it to set it up."
                     : s->theirs ? "Somebody else's station" : "Asking whose it is...", sizeof s->now);
    one_line("now", "Now", s->now);

    det_begin("info");
    /* The phone or computer in the person's hand has no firmware to report
     * to itself, and is not "heard" by itself. */
    int self = is_this_device(s);
    int soft = self || s->soft;
    fw_of(s, have, w, sizeof w);
    if (soft) {
        det_section("Kind");
        det_row("This is", self ? "the device in your hand" : "a phone or a computer");
        if (s->count[0]) det_row("Keeps", s->count);
        if (s->room[0]) det_row("Room left", s->room);
        if (s->mail[0]) det_row("Mail held", s->mail);
    } else if (!self || w[0]) {
        det_section("Firmware");
        if (w[0]) { dv_cpy(v, "XPRS ", sizeof v); dv_cat(v, w, sizeof v); }
        else dv_cpy(v, s->asking ? "asking..." : "not reported yet", sizeof v);
        det_row("Version", v);
    }
    if (w[0] && g_latest[0]) {
        if (behind(w)) { dv_cpy(v, g_latest, sizeof v); dv_cat(v, " is available", sizeof v); }
        else dv_cpy(v, "this is the newest", sizeof v);
        det_row("Newest", v);
    }
    if (installed_words(s->call, v, sizeof v)) det_row("Installed", v);
    v[0] = 0; if (have) dv_json(g_host, "lifetime", v, sizeof v);
    if (v[0]) { char l[48] = ""; dv_cat(l, v, sizeof l); dv_cat(l, " in total", sizeof l); det_row("Running", l); }
    v[0] = 0; if (have) dv_json(g_host, "uptime", v, sizeof v);
    if (!v[0]) dv_cpy(v, s->uptime, sizeof v);
    if (v[0]) { char l[48] = ""; dv_cat(l, v, sizeof l); dv_cat(l, " since its last restart", sizeof l); det_row("Up", l); }

    det_section("Connection");
    if (self) dv_cpy(v, "this device", sizeof v);
    else reach_words(s, have, v, sizeof v);
    det_row("Heard over", v);
    if (s->mine && s->pol_owner[0] && !dv_eq(s->pol_owner, "none"))
        det_row("Admins", s->pol_owner);
    det_row("Owner", s->mine ? "You" : s->unowned ? "Nobody yet" : s->theirs ? "Somebody else" : "");
    {
        int on_lan = 0;
        char lanes[64] = "";
        if (have) dv_json_list(g_host, "bearers", lanes, sizeof lanes);
        for (const char *p = lanes; *p; p++) if (dv_starts(p, "lan")) on_lan = 1;
        v[0] = 0;
        if (dv_eq(s->wifi, "joining")) dv_cpy(v, "joining...", sizeof v);
        else if (dv_eq(s->wifi, "failed")) dv_cpy(v, "could not join", sizeof v);
        else if (dv_eq(s->wifi, "off")) dv_cpy(v, "off", sizeof v);
        else if (s->wifi[0] || on_lan) dv_cpy(v, "connected", sizeof v);
        if (v[0] && s->ip[0]) { dv_cat(v, ", ", sizeof v); dv_cat(v, s->ip, sizeof v); }
        det_row("WiFi", v);
    }
    if (s->ap[0]) det_row("Own hotspot", dv_eq(s->ap, "on") ? "on" : dv_eq(s->ap, "off") ? "off" : s->ap);
    v[0] = 0; if (have) dv_json(g_host, "peers", v, sizeof v);
    if (!v[0]) dv_cpy(v, s->peers, sizeof v);
    if (v[0]) { dv_cat(v, " in reach", sizeof v); det_row("Stations", v); }

    det_section("Options");
    det_row("Name", s->nick);
    det_row("Time zone", s->zone);
    if (s->lora[0])
        det_row("LoRa", dv_eq(s->lora, "meshtastic") ? "XPRS + Meshtastic"
                      : dv_eq(s->lora, "meshcore") ? "XPRS + MeshCore" : "XPRS");
    det_row("Channel", s->freq);
    det_end();

    flag_hidden("claim", !(s->unowned && !s->mine));
    flag_hidden("update", !(s->mine && behind(w)));
    if (s->mine && behind(w)) {
        char l[40] = "Update to ";
        dv_cat(l, g_latest, sizeof l);
        field_set("update__label", l);
    }
    /* A phone or a computer administering itself has no WiFi to be given,
     * no board settings and no firmware to be sent: its owner is holding
     * it. What is left is who else may administer it. */
    /* The same holds for somebody's phone run from afar: its WiFi, name and
     * key are set on the phone. */
    flag_hidden("open_wifi", !s->mine || soft);
    flag_hidden("open_name", !s->mine || soft);
    flag_hidden("open_admins", !s->mine);
    flag_hidden("open_identity", !s->mine || soft);
    flag_hidden("forget", self);
    if (soft) flag_hidden("update", 1);
}

static void item_full(const char *id, const char *title, const char *sub,
                      const char *tags, int dim, const char *icon,
                      const char *picture, int *first)
{
    if (!*first) dv_cat(g_out, ",", sizeof g_out);
    *first = 0;
    dv_cat(g_out, "{\"id\":\"", sizeof g_out);
    dv_jesc(g_out, id, sizeof g_out);
    dv_cat(g_out, "\",\"title\":\"", sizeof g_out);
    dv_jesc(g_out, title, sizeof g_out);
    dv_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    dv_jesc(g_out, sub, sizeof g_out);
    dv_cat(g_out, "\",\"tags\":[", sizeof g_out);
    dv_cat(g_out, tags, sizeof g_out);
    dv_cat(g_out, "]", sizeof g_out);
    if (icon && icon[0]) { dv_cat(g_out, ",\"icon\":\"", sizeof g_out); dv_jesc(g_out, icon, sizeof g_out); dv_cat(g_out, "\"", sizeof g_out); }
    if (picture && picture[0]) { dv_cat(g_out, ",\"picture\":\"", sizeof g_out); dv_jesc(g_out, picture, sizeof g_out); dv_cat(g_out, "\"", sizeof g_out); }
    if (dim) dv_cat(g_out, ",\"dim\":true", sizeof g_out);
    dv_cat(g_out, "}", sizeof g_out);
}


static void tag(char *tags, unsigned cap, const char *prefix, const char *v)
{
    if (!v || !v[0]) return;
    if (tags[0]) dv_cat(tags, ",", cap);
    dv_cat(tags, "\"", cap);
    dv_jesc(tags, prefix, cap);
    dv_jesc(tags, v, cap);
    dv_cat(tags, "\"", cap);
}

/* The boards on the cable, and how to plug one in, drawn by the flashing
 * half below. */
static void list_cable(int *any);
static void list_cable_help(int *any);

/* The one list: boards on the cable first, then the stations heard on
 * WiFi, Bluetooth or LoRa, then (with no cable in) how to add a board. */
static void push_list(void) { dv_list_dirty(); }

int st_screen(void) { return g_screen[0] != 0; }

/* Our share of the one list (devices.h): part 0 is the boards on the cable
 * and the stations that are ours or could be, part 1 other people's, part 2
 * how to add a board when nothing is on the cable. */
const char *st_sections(int part)
{
    g_out[0] = 0;
    int any = 0;
    if (part == 2) { list_cable_help(&any); return g_out; }
    if (part == 0) list_cable(&any);
    static const char *const titles[3] = {
        "{\"title\":\"Waiting for an owner\",\"items\":[",
        "{\"title\":\"Your stations\",\"items\":[",
        "{\"title\":\"Other stations nearby\",\"items\":["
    };
    for (int pass = part == 0 ? 0 : 2; pass < (part == 0 ? 2 : 3); pass++) {
        int first = 1;
        for (int i = 0; i < g_nst; i++) {
            st_t *s = &g_st[i];
            int in = pass == 0 ? (s->unowned && !s->mine)
                   : pass == 1 ? s->mine
                   : (s->theirs && !s->mine && !s->unowned);
            if (!in) continue;
            if (first) {
                if (any) dv_cat(g_out, ",", sizeof g_out);
                dv_cat(g_out, titles[pass], sizeof g_out);
                any = 1;
            }
            int have = host_facts(s);
            unsigned long long agoms = have ? host_num("agoMs") : 0;
            char sub[96] = "", b[12], fw[24];
            if (s->nick[0]) { dv_cat(sub, s->call, sizeof sub); dv_cat(sub, ", ", sizeof sub); }
            if (is_this_device(s)) {
                dv_cat(sub, "This device", sizeof sub);
            } else {
                char r[64];
                reach_words(s, have, r, sizeof r);
                dv_cat(sub, r, sizeof sub);
            }
            if (pass == 0) dv_cat(sub, ". Tap to claim it", sizeof sub);
            bearer_of(s, have, b, sizeof b);
            fw_of(s, have, fw, sizeof fw);
            char tags[160] = "";
            tag(tags, sizeof tags, "XPRS ", fw);
            if (pass == 1 && behind(fw)) tag(tags, sizeof tags, "", "update available");
            item_full(s->call, s->nick[0] ? s->nick : s->call, sub, tags,
                      !is_this_device(s) && (!have || agoms > 600000ULL), bearer_icon(b), "", &first);
        }
        if (!first) dv_cat(g_out, "]}", sizeof g_out);
    }
    return g_out;
}

/* The health word of zh:<up>/<required>: every required part up, or not. */
static void health_words(const char *zh, char *out, unsigned cap)
{
    unsigned long long up = 0, req = 0;
    const char *p = zh;
    for (; *p && *p != '/'; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
        if (d < 0) { dv_cpy(out, zh, cap); return; }
        up = up * 16 + (unsigned)d;
    }
    if (*p != '/') { dv_cpy(out, zh, cap); return; }
    for (p++; *p; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : -1;
        if (d < 0) break;
        req = req * 16 + (unsigned)d;
    }
    unsigned long long missing = req & ~up;
    int n = 0;
    for (; missing; missing >>= 1) n += (int)(missing & 1);
    if (n == 0) dv_cpy(out, "all up", cap);
    else { dv_cpy(out, "", cap); dv_cat_u(out, (unsigned)n, cap); dv_cat(out, n == 1 ? " part down" : " parts down", cap); }
}

/* Split "a/b/c" into up to four pieces. */
static int slashes(const char *v, char out[4][16])
{
    int n = 0, o = 0;
    out[0][0] = 0;
    for (const char *p = v; *p && n < 4; p++) {
        if (*p == '/') { out[n][o] = 0; n++; o = 0; if (n < 4) out[n][0] = 0; continue; }
        if (o < 15) out[n][o++] = *p;
    }
    if (n < 4) { out[n][o] = 0; n++; }
    return n;
}

static void push_stats(void)
{
    if (g_sel < 0 || g_sel >= g_nst) return;
    st_t *s = &g_st[g_sel];
    int have = host_facts(s);
    char v[64], w[4][16];
    g_empty = s->asking ? "..." : "?";

    /* 15.5: what the core heard the station say about itself. */
    tiles_begin("st_station");
    v[0] = 0; if (have) dv_json(g_host, "fw", v, sizeof v);
    if (!(s->soft || is_this_device(s)) || v[0] || s->fw[0])
        tile("fw", "Firmware", v[0] ? v : s->fw, "", "", "", 0);
    v[0] = 0; if (have) dv_json(g_host, "uptime", v, sizeof v);
    tile("up", "Up", v[0] ? v : s->uptime, "", "since its last restart", "", 0);
    v[0] = 0; if (have) dv_json(g_host, "lifetime", v, sizeof v);
    tile("life", "Lifetime", v[0] ? v : s->life, "", "across every restart", "", 0);
    v[0] = 0; if (have) dv_json(g_host, "peers", v, sizeof v);
    tile("peers", "Peers", v[0] ? v : s->peers, "", "stations it reaches", "", 0);
    v[0] = 0; if (have) dv_json(g_host, "mail", v, sizeof v);
    tile("mail", "Mail held", v[0] ? v : s->mail, "", "for other stations", "", 0);
    v[0] = 0; if (have) dv_json(g_host, "count", v, sizeof v);
    tile("count", "Records", v[0] ? v : s->count, "", "messages and posts it keeps", "", 0);
    if (s->room[0]) tile("room", "Room left", s->room, "", "storage it still offers", "", 0);
    if (!is_this_device(s)) signal_tile(s, have, have ? host_num("agoMs") : 0, 0);
    v[0] = 0; if (have) dv_json_list(g_host, "bearers", v, sizeof v);
    {   /* the count as the figure, the lanes as the hint: a phone-width
         * tile has room for "2" and not for "ble, lan" */
        char n[4] = "";
        if (v[0]) { int c = 1; for (const char *p = v; *p; p++) if (*p == ',') c++; dv_cat_u(n, (unsigned)c, sizeof n); }
        tile("bearers", "Lanes", n, "", v, "", 0);
    }
    v[0] = 0; if (have) dv_json(g_host, "sig", v, sizeof v);
    tile("sig", "Signatures", v, "", "", "", dv_eq(v, "forged"));
    if (s->lora[0])
        tile("lora", "LoRa", dv_eq(s->lora, "meshtastic") ? "XPRS+Meshtastic"
                           : dv_eq(s->lora, "meshcore") ? "XPRS+MeshCore" : "XPRS",
             "", "which network shares its channel", "", 0);
    if (s->freq[0])
        tile("freq", "Channel", s->freq, "", "the frequency its radio is on",
             "", 0);
    tiles_end();
    v[0] = 0; if (have) dv_json_list(g_host, "hears", v, sizeof v);
    one_line("st_hears", "Hears", v[0] ? v : "nobody reported");

    /* 11.9: the policy, what anybody may ask. */
    tiles_begin("st_policy");
    tile("owner", "Owner", s->pol_owner, "", "", "", 0);
    tile("use", "Use", s->pol_use, "", "who may send through it", "", 0);
    tile("first", "First", s->pol_first, "", "served ahead of others", "", 0);
    tile("serve", "Serve", s->pol_serve, "", "what it does for others", "", 0);
    tiles_end();

    /* The firmware's own words (cmd:zdiag, its owner only). */
    tiles_begin("st_diag");
    int board = s->heap[0] || s->reset[0] || s->health[0] || s->slot[0] || s->radio[0] || s->crash[0];
    if (!board) {
        /* A phone or a server answers the same ask with what it holds
         * (shown above); the board's own figures are not its to give. */
        tile("kind", "Kind", is_this_device(s) ? "This device" : s->count[0] || s->room[0] ? "Phone or computer" : "",
             "", s->mine ? "" : "its owner may ask", "", 0);
        tiles_end();
        flag_hidden("crash", 1);
        g_empty = "?";
        return;
    }
    if (s->heap[0]) {
        int n = slashes(s->heap, w);
        char hint[48] = "";
        if (n >= 3) { dv_cpy(hint, "largest ", sizeof hint); dv_cat(hint, w[1], sizeof hint); dv_cat(hint, ", lowest ", sizeof hint); dv_cat(hint, w[2], sizeof hint); }
        tile("heap", "Free memory", w[0], "KB", hint, "", 0);
    } else {
        tile("heap", "Free memory", "", "KB", s->mine ? "tap Refresh" : "its owner may ask", "", 0);
    }
    tile("reset", "Last reset", s->reset, "", "", "", dv_eq(s->reset, "panic"));
    if (s->health[0]) { health_words(s->health, v, sizeof v); tile("health", "Health", v, "", s->health, "", !dv_eq(v, "all up")); }
    else tile("health", "Health", "", "", "", "", 0);
    if (s->slot[0]) { slashes(s->slot, w); tile("slot", "Slot", w[0], "", "the OTA slot in use", "", 0); }
    else tile("slot", "Slot", "", "", "the OTA slot in use", "", 0);
    if (s->radio[0]) {
        /* "rx/tx/cancel/drop done/issued/fail" as kept by take_state. */
        char zn[24] = "", zs[24] = "";
        unsigned i = 0, o = 0;
        for (; s->radio[i] && s->radio[i] != ' ' && o < sizeof zn - 1; i++) zn[o++] = s->radio[i];
        zn[o] = 0;
        if (s->radio[i] == ' ') dv_cpy(zs, s->radio + i + 1, sizeof zs);
        int n = slashes(zn, w);
        char hint[48] = "";
        if (n >= 4) { dv_cpy(hint, "sent ", sizeof hint); dv_cat(hint, w[1], sizeof hint); dv_cat(hint, ", dropped ", sizeof hint); dv_cat(hint, w[3], sizeof hint); }
        tile("radio", "ESP-NOW heard", w[0], "", hint, "", 0);
        n = slashes(zs, w);
        hint[0] = 0;
        if (n >= 3) { dv_cpy(hint, "of ", sizeof hint); dv_cat(hint, w[1], sizeof hint); dv_cat(hint, ", failed ", sizeof hint); dv_cat(hint, w[2], sizeof hint); }
        tile("sent", "Sent", w[0], "", hint, "", n >= 3 && !dv_eq(w[2], "0"));
    }
    if (s->crash[0]) tile("crash", "Crashed in", s->crash, "", "tap Crash report", "", 1);
    tiles_end();
    flag_hidden("crash", !s->crash[0]);
    g_empty = "?";
}

/* ── What to listen to ────────────────────────────────────────────────── */
static void sub(int *held, int want, const char *topic)
{
    if (!want == !*held) return;
    dv_sub(DV_ST, topic, want);
    *held = want;
}

/* Answers only while a command is out, identities only while a station is
 * changing its key, observations only while a policy ask is out. Everything
 * else on those topics is somebody else's. */
static void listen(void)
{
    int asked = 0, keying = 0, asking = 0;
    for (int i = 0; i < g_nst; i++) {
        if (g_st[i].pend_id[0]) asked = 1;
        if (g_st[i].rekey_npub[0] ||
            (g_st[i].pend_id[0] && (dv_eq(g_st[i].pend_what, "key") ||
                                    dv_eq(g_st[i].pend_what, "call")))) keying = 1;
        if (g_st[i].ask_pol) asking = 1;
    }
    if (g_key_for[0]) keying = 1;        /* waiting to hear the new board */
    sub(&g_sub_res, asked, "xprs.result");
    sub(&g_sub_tx, asked, "xprs.status.tx");
    sub(&g_sub_ids, keying, "xprs.identity");
    sub(&g_sub_obs, asking, "xprs.observation");
}

static void stamp_for(st_t *s, char *ts, unsigned cap)
{
    unsigned long long now = hal_time_epoch();
    if (now <= s->last_ts) now = s->last_ts + 1;
    s->last_ts = now;
    dv_stamp(ts, cap, now);
}

/* Air [wire], remember it as the command in flight to [s]. */
static int send_cmd(st_t *s, const char *wire, const char *what)
{
    int rc = hal_xprs_send(wire, dv_len(wire));
    if (rc != 0) {
        log_line(s->call, rc == -2 ? "Refused by this phone: not allowed to send that"
                                   : "This phone could not send that command");
        return rc;
    }
    dv_cpy(s->pend, wire, sizeof s->pend);
    dv_id(wire, dv_len(wire), s->pend_id);
    dv_cpy(s->pend_what, what, sizeof s->pend_what);
    dv_cpy(s->last_what, what, sizeof s->last_what);
    s->pend_final = 0;
    listen();
    return 0;
}

static void done_pending(st_t *s)
{
    s->pend[0] = 0;
    s->pend_id[0] = 0;
    s->pend_what[0] = 0;
    s->pend_final = 0;
    s->pend_408 = 0;
    listen();
}

/* ── Commands to a station ────────────────────────────────────────────── */
static void do_claim(st_t *s)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (dv_claim(w, sizeof w, g_me, s->call, ts, g_my_npub) < 0) {
        log_line(s->call, "This phone's callsign is too long to claim with");
        return;
    }
    if (send_cmd(s, w, "claim") == 0) { log_line(s->call, "Claiming..."); set_now(s, "Claiming..."); }
}

/* Seal [body] to the station and air it; zero the body either way. */
static int send_sealed(st_t *s, char *body, const char *what)
{
    static char x[FW_WIRE_UNSIGNED];
    uint32_t n = hal_encrypt(s->npub, dv_len(s->npub), body, dv_len(body), x, sizeof x - 1);
    for (unsigned i = 0; body[i]; i++) body[i] = 0;
    if (!n) { log_line(s->call, "Could not seal it to the station's key"); return -1; }
    x[n] = 0;
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    int rc = dv_sealed(w, sizeof w, g_me, s->call, ts, x);
    for (unsigned i = 0; x[i]; i++) x[i] = 0;
    if (rc < 0) {
        log_line(s->call, "Too long to seal into one packet");
        return -1;
    }
    return send_cmd(s, w, what);
}

/* The callsign chosen for a board just flashed here: its key, sealed to the
 * station's own key, which the station takes and restarts under (11.10).
 * Waits, without a clock, until the core has heard the station say its key:
 * on_identity, the next core.flash, or the next look at the list call it. */
static void give_key(void)
{
    if (!g_key_for[0] || !g_want_nsec[0]) return;
    int i = find(g_key_for);
    if (i < 0) return;
    st_t *s = &g_st[i];
    if (!s->npub[0]) key_from_core(s, host_facts(s));
    if (!s->npub[0]) return;
    if (s->pend_id[0] && !s->pend_final) return;
    if (s->pend_id[0]) done_pending(s);
    if (!g_me[0]) who_am_i();
    char body[96], line[64] = "Giving it the callsign ";
    dv_cat(line, g_want_call, sizeof line);
    const char *kv[] = { "nsec", g_want_nsec, 0 };
    if (dv_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "key") == 0) {
        log_line(s->call, line);
        set_now(s, line);
    }
    forget_wanted();
    listen();
}

static void do_wifi(st_t *s, char *ssid, char *pass)
{
    char body[128];
    const char *kv1[] = { "ssid", ssid, "pass", pass, 0 };
    int n = dv_body(body, sizeof body, kv1);
    s->next_x[0] = 0;
    if (n < 0) {
        log_line(s->call, "A network name or password cannot hold a line break");
    } else if (n <= FW_BODY_ONE_MAX) {
        if (send_sealed(s, body, "wifi") == 0) set_now(s, "Sending the network, sealed...");
    } else {
        /* Too long for one packet: the name first, then the password, and
         * the station joins when it has the second (11.10). The password's
         * body is sealed now and kept as ciphertext only. */
        const char *kv2[] = { "pass", pass, 0 };
        char b2[96];
        if (dv_body(b2, sizeof b2, kv2) < 0) { log_line(s->call, "Password too long"); }
        else {
            static char x2[FW_WIRE_UNSIGNED];
            uint32_t m = hal_encrypt(s->npub, dv_len(s->npub), b2, dv_len(b2), x2, sizeof x2 - 1);
            for (unsigned i = 0; b2[i]; i++) b2[i] = 0;
            x2[m] = 0;
            dv_cpy(s->next_x, x2, sizeof s->next_x);
            for (unsigned i = 0; x2[i]; i++) x2[i] = 0;
            const char *kv3[] = { "ssid", ssid, 0 };
            char b3[64];
            if (m && dv_body(b3, sizeof b3, kv3) > 0 && send_sealed(s, b3, "wifi") == 0)
                set_now(s, "Sending the network name, then the password...");
        }
        for (unsigned i = 0; body[i]; i++) body[i] = 0;
    }
    for (unsigned i = 0; pass[i]; i++) pass[i] = 0;
    for (unsigned i = 0; ssid[i]; i++) ssid[i] = 0;
}

static void do_set(st_t *s, const char *fields, const char *what)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (dv_set(w, sizeof w, g_me, s->call, ts, fields) < 0) {
        log_line(s->call, "Too much to say in one packet");
        return;
    }
    if (send_cmd(s, w, what) == 0) set_now(s, "Sending...");
}

static void do_cmd(st_t *s, const char *cmd, const char *what)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (dv_cmd(w, sizeof w, g_me, s->call, ts, cmd) >= 0 && send_cmd(s, w, what) == 0)
        set_now(s, dv_eq(what, "zdiag") ? "Asking for its stats..."
                 : dv_eq(what, "update") ? "Asking it to update..." : "Asking for its crash report...");
}

/* q:policy and q:mail: a t:request, answered by anybody's station as an
 * observation (11.9, 9.12.3). Not a command, so not the courier's: asked
 * once, and read from xprs.observation while the ask is out. */
static void do_ask(st_t *s)
{
    char ts[24], w[FW_WIRE_UNSIGNED + 1];
    stamp_for(s, ts, sizeof ts);
    if (dv_ask(w, sizeof w, g_me, s->call, ts, "policy") > 0) hal_xprs_send(w, dv_len(w));
    stamp_for(s, ts, sizeof ts);
    if (dv_ask(w, sizeof w, g_me, s->call, ts, "mail") > 0) hal_xprs_send(w, dv_len(w));
    s->ask_pol = 1;
    listen();
}

/* A station in earshot that is not on the list may still be ours: one
 * claimed over a cable, or before this phone kept a list. It never asks to
 * be claimed, so it is asked whose it is (q:policy, which anybody may ask)
 * when the page opens, and kept if the answer names us. Only into free
 * rows, so a probe never pushes a listed station out, and a station that
 * said it is somebody else's is not asked again this session. */
static char *find_in(char *hay, const char *needle)
{
    for (; *hay; hay++) if (dv_starts(hay, needle)) return hay;
    return 0;
}

static int notmine(const char *call)
{
    for (int i = 0; i < 8; i++) if (dv_eq(g_notmine[i], call)) return 1;
    return 0;
}

static void discover(void)
{
    int n = hal_xprs_stations(g_heard, sizeof g_heard - 1);
    if (n < 0) {
        static const char m[] = "firmwares: too many stations heard to look for yours";
        hal_log(2, m, sizeof m - 1);
    }
    if (n <= 0) return;
    g_heard[n] = 0;
    /* Only the first section: in earshot now. A station heard forty
     * minutes ago, or only over Reticulum, would not hear the ask. */
    const char *later = "\"title\":\"Heard this hour";
    for (char *p = g_heard + 1; *p; p++)
        if (dv_starts(p, later) || dv_starts(p, "\"title\":\"On Reticulum")) { *p = 0; break; }
    if (!g_me[0]) who_am_i();
    for (char *p = g_heard; (p = find_in(p, "{\"id\":\"")) != 0 && g_nst < ST_MAX; ) {
        p += 7;
        char call[12];
        unsigned k = 0;
        while (p[k] && p[k] != '"' && k < sizeof call - 1) { call[k] = p[k]; k++; }
        call[k] = 0;
        char *close = find_in(p, "}");
        if (!close) break;
        char save = *close;
        *close = 0;
        int station = find_in(p, "\"kind\":\"station\"") != 0;
        *close = save;
        p = close;
        if (!station || !call[0] || notmine(call)) continue;
        int was = find(call);
        if (was >= 0) {
            /* Asked before and never answered (the ask or the answer was
             * lost): ask again, or it would hold a row unseen for good. */
            if (g_st[was].probe) do_ask(&g_st[was]);
            continue;
        }
        if (dv_starts(call, g_me) && (call[dv_len(g_me)] == 0 || call[dv_len(g_me)] == '-'))
            continue;                      /* this phone */
        int i = add(call);
        if (i < 0) break;
        st_t *st = &g_st[i];
        st->probe = 1;
        do_ask(st);
    }
}

/* ── What the station says ────────────────────────────────────────────── */
static void take_state(st_t *s, const char *wire)
{
    char v[48];
    if (dv_field(wire, "wifi", v, sizeof v)) dv_cpy(s->wifi, v, sizeof s->wifi);
    if (dv_field(wire, "ip", v, sizeof v)) dv_cpy(s->ip, v, sizeof s->ip);
    else if (dv_field(wire, "wifi", v, sizeof v)) s->ip[0] = 0;
    if (dv_field(wire, "ap", v, sizeof v)) dv_cpy(s->ap, v, sizeof s->ap);
    if (dv_field(wire, "nick", v, sizeof v)) dv_cpy(s->nick, v, sizeof s->nick);
    /* 11.9: a result to a policy command states all four, as they are. */
    if (dv_field(wire, "owner", v, sizeof v)) dv_cpy(s->pol_owner, v, sizeof s->pol_owner);
    if (dv_field(wire, "serve", v, sizeof v)) dv_cpy(s->pol_serve, v, sizeof s->pol_serve);
    if (dv_field(wire, "zone", v, sizeof v)) dv_cpy(s->zone, v, sizeof s->zone);
    if (dv_field(wire, "lora", v, sizeof v)) dv_cpy(s->lora, v, sizeof s->lora);
    if (dv_field(wire, "freq", v, sizeof v)) dv_cpy(s->freq, v, sizeof s->freq);
    if (dv_field(wire, "fw", v, sizeof v)) dv_cpy(s->fw, v, sizeof s->fw);
    if (dv_field(wire, "uptime", v, sizeof v)) dv_cpy(s->uptime, v, sizeof s->uptime);
    if (dv_field(wire, "peers", v, sizeof v)) dv_cpy(s->peers, v, sizeof s->peers);
    /* What a station holds, in the words of 13.0 and 15.5. A phone or a
     * server says these to its owner in answer to the diagnostics ask. */
    if (dv_field(wire, "count", v, sizeof v)) dv_cpy(s->count, v, sizeof s->count);
    if (dv_field(wire, "lifetime", v, sizeof v)) dv_cpy(s->life, v, sizeof s->life);
    if (dv_field(wire, "size", v, sizeof v)) dv_cpy(s->room, v, sizeof s->room);
    if (dv_field(wire, "mail", v, sizeof v)) dv_cpy(s->mail, v, sizeof s->mail);
    if (dv_field(wire, "zm", v, sizeof v)) { dv_cpy(s->heap, v, sizeof s->heap); s->soft = 0; }
    else if (dv_field(wire, "count", v, sizeof v) && dv_field(wire, "uptime", v, sizeof v) &&
             !dv_field(wire, "fw", v, sizeof v) && !dv_field(wire, "wifi", v, sizeof v)) s->soft = 1;
    if (dv_field(wire, "zr", v, sizeof v)) dv_cpy(s->reset, v, sizeof s->reset);
    if (dv_field(wire, "zh", v, sizeof v)) dv_cpy(s->health, v, sizeof s->health);
    if (dv_field(wire, "zp", v, sizeof v)) dv_cpy(s->slot, v, sizeof s->slot);
    if (dv_field(wire, "zn", v, sizeof v)) {
        dv_cpy(s->radio, v, sizeof s->radio);
        if (dv_field(wire, "zs", v, sizeof v)) { dv_cat(s->radio, " ", sizeof s->radio); dv_cat(s->radio, v, sizeof s->radio); }
    }
    if (dv_field(wire, "zc", v, sizeof v)) dv_cpy(s->crash, v, sizeof s->crash);
    else if (dv_field(wire, "zm", v, sizeof v)) s->crash[0] = 0;   /* a zdiag without one */
}

static void on_request(const char *row)
{
    char wire[FW_WIRE_MAX + 1], q[12], call[16], k[80], sig[16];
    /* Believed only when it is signed by the key it carries and the
     * callsign derives from that key (11.9): anything else is somebody
     * inviting this phone to hand a stranger its password. The core's
     * verdict first, because it is the cheapest test. */
    if (!dv_json(row, "sig", sig, sizeof sig) || !dv_eq(sig, "verified")) return;
    if (!dv_json(row, "wire", wire, sizeof wire)) return;
    if (!dv_field(wire, "q", q, sizeof q) || !dv_eq(q, "owner")) return;
    if (!dv_field(wire, "f", call, sizeof call)) return;
    if (!dv_field(wire, "k", k, sizeof k)) return;
    int known = find(call);
    /* A station is its key. A callsign is a few characters and anyone can
     * grind a key that derives it in a few thousand tries: a second key
     * for a callsign we already hold is somebody else, and is not believed
     * however well it signs. The key we hold was derived when we took it. */
    if (known >= 0 && g_st[known].npub[0]) {
        if (!dv_eq(g_st[known].npub, k)) return;
    } else if (!dv_call_matches(call, k)) {
        return;
    }
    int i = known >= 0 ? known : add(call);
    if (i < 0) return;
    st_t *s = &g_st[i];
    s->heard_ms = hal_time_ms();
    /* How it was heard, for the screen when it is next drawn: kept in
     * memory, written nowhere. */
    dv_json(row, "bearer", s->bearer, sizeof s->bearer);
    char r[12];
    if (dv_json(row, "rssi", r, sizeof r)) {
        int neg = r[0] == '-', v = 0;
        for (const char *p = r + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        s->rssi = neg ? -v : v;
    }
    /* The same ask comes back every half minute until somebody answers it:
     * only the first one of a run is news, and only news costs anything. */
    int news = known < 0 || !s->unowned || s->mine || s->theirs;
    if (!news) return;
    /* Erased and asking again: what we knew of it is what it was. */
    s->nick[0] = s->wifi[0] = s->ip[0] = s->ap[0] = s->zone[0] = 0;
    s->fw[0] = s->uptime[0] = s->peers[0] = s->heap[0] = s->reset[0] = 0;
    s->health[0] = s->slot[0] = s->radio[0] = s->crash[0] = s->mail[0] = 0;
    s->pol_owner[0] = s->pol_use[0] = s->pol_first[0] = s->pol_serve[0] = 0;
    dv_cpy(s->npub, k, sizeof s->npub);
    s->unowned = 1;
    s->mine = 0;                   /* erased and asking again: not ours now */
    s->theirs = 0;
    set_now(s, "Waiting for an owner");
    save();
    push_list();
    if (g_sel == i) push_hub();
    {
        char m[300] = "{\"type\":\"notify\",\"level\":\"info\",\"title\":\"A station nearby can be set up\",\"body\":\"";
        dv_jesc(m, call, sizeof m);
        dv_cat(m, " was just flashed and is waiting for an owner. Open Firmwares to claim it.\",\"tag\":\"firmwares.unowned.", sizeof m);
        dv_jesc(m, call, sizeof m);
        dv_cat(m, "\"}", sizeof m);
        say(m);
    }
}

/* An X1 to X5 callsign: one a key made, not one an authority issued. */
static int self_generated(const char *c)
{
    return c[0] == 'X' && c[1] >= '1' && c[1] <= '5' && dv_len(c) >= 3;
}

/* The same station under another name: same key, new callsign (6.4.2). Its
 * record moves with it, so the old one does not come back as a stranger. */
static void rename_to(int i, const char *call)
{
    st_t *s = &g_st[i];
    char k[24] = "st.";
    dv_cat(k, s->call, sizeof k);
    kv_put(k, "");
    char line[80] = "Now transmitting as ";
    dv_cat(line, call, sizeof line);
    log_line(s->call, line);
    dv_cpy(s->call, call, sizeof s->call);
    set_now(s, line);
    save();
    push_list();
    if (g_sel == i) { push_hub(); screen_open("Station", s->nick[0] ? s->nick : s->call); }
}

/* Heard only while a station is changing its key or its callsign (listen()). */
static void on_identity(const char *row)
{
    char wire[FW_WIRE_MAX + 1], call[16], k[80], sig[16];
    dv_json(row, "sig", sig, sizeof sig);
    if (!dv_eq(sig, "verified")) return;
    if (!dv_json(row, "wire", wire, sizeof wire)) return;
    if (!dv_field(wire, "f", call, sizeof call) || !dv_field(wire, "k", k, sizeof k)) return;
    /* A station taking an issued callsign: it derives from no key, so the
     * only thing that says it is ours is the key it binds, the one we hold
     * (6.4.2), on a t:identity the core checked against that key. */
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (s->pend_id[0] && dv_eq(s->pend_what, "call") && dv_eq(s->npub, k) &&
            !dv_eq(s->call, call)) {
            rename_to(i, call);
            listen();
            return;
        }
    }
    if (!dv_call_matches(call, k)) return;
    if (g_key_for[0] && dv_eq(call, g_key_for)) {
        int j = find(call);
        if (j >= 0 && !g_st[j].npub[0]) dv_cpy(g_st[j].npub, k, sizeof g_st[j].npub);
        give_key();
    }
    dv_cpy(g_ids[g_ids_w].call, call, sizeof g_ids[0].call);
    dv_cpy(g_ids[g_ids_w].npub, k, sizeof g_ids[0].npub);
    g_ids_w = (g_ids_w + 1) % 4;
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (s->rekey_npub[0] && dv_eq(s->rekey_npub, k)) {
            /* The new key announcing itself: follow it (11.10). */
            char was[12];
            dv_cpy(was, s->call, sizeof was);
            dv_cpy(s->call, call, sizeof s->call);
            dv_cpy(s->npub, k, sizeof s->npub);
            s->rekey_npub[0] = 0;
            char line[80] = "Came back as ";
            dv_cat(line, call, sizeof line);
            log_line(was, line);
            set_now(s, line);
            save();
            push_list();
            if (g_sel == i) { push_hub(); screen_open("Station", s->nick[0] ? s->nick : s->call); }
            listen();
            return;
        }
    }
}

/* Heard only while a policy ask is out (listen()): the station's policy or
 * its mail count, as an observation addressed to us. */
/* Whether a policy's owner: list names this phone. By callsign, or by the
 * part of it that comes from the key: a station owned over a cable (or by
 * config.ini's [owners]) knows the key and not the prefix its holder chose,
 * so it names the owner with a derived X3 (XPRS.md 3, 11.9). A suffix match
 * is not proof, and needs none: the station checks every command against
 * the key itself, so a wrong guess costs a 403, never a station. */
static int owner_is_me(const char *owners)
{
    unsigned n = dv_len(g_me);
    if (n < 3) return 0;
    for (const char *p = owners; *p; ) {
        unsigned k = 0;
        while (p[k] && p[k] != ',') k++;
        if (k == n && dv_starts(p, g_me)) return 1;
        if (k == n && p[0] == 'X' && p[1] >= '0' && p[1] <= '9' && g_me[0] == 'X') {
            unsigned j = 2;
            while (j < n && p[j] == g_me[j]) j++;
            if (j == n) return 1;
        }
        p += k;
        if (*p == ',') p++;
    }
    return 0;
}

static void on_observation(const char *row)
{
    char wire[FW_WIRE_MAX + 1], from[16], v[48], sub[12];
    if (!dv_json(row, "forUs", v, sizeof v) || !dv_eq(v, "true")) return;
    if (!dv_json(row, "sig", v, sizeof v) || !dv_eq(v, "verified")) return;
    if (!dv_json(row, "wire", wire, sizeof wire)) return;
    if (!dv_field(wire, "f", from, sizeof from)) return;
    int i = find(from);
    if (i < 0) return;
    st_t *s = &g_st[i];
    if (!dv_field(wire, "s", sub, sizeof sub)) return;
    if (dv_eq(sub, "policy")) {
        if (dv_field(wire, "owner", v, sizeof v)) dv_cpy(s->pol_owner, v, sizeof s->pol_owner);
        if (dv_field(wire, "use", v, sizeof v)) dv_cpy(s->pol_use, v, sizeof s->pol_use);
        if (dv_field(wire, "first", v, sizeof v)) dv_cpy(s->pol_first, v, sizeof s->pol_first);
        if (dv_field(wire, "serve", v, sizeof v)) dv_cpy(s->pol_serve, v, sizeof s->pol_serve);
        /* Who it belongs to, from its own mouth: mine, nobody's, or another's. */
        if (dv_eq(s->pol_owner, "none")) { s->unowned = 1; s->mine = 0; s->theirs = 0; }
        else if (owner_is_me(s->pol_owner)) {
            s->mine = 1; s->unowned = 0; s->theirs = 0;
        } else { s->theirs = 1; s->mine = 0; s->unowned = 0; }
        s->ask_pol = 0;
        if (s->probe) {
            /* Asked because it was heard (discover). Somebody else's is
             * listed too, under Other stations nearby: what it runs can be
             * read by anybody. It is not asked again this session. */
            s->probe = 0;
            if (s->theirs) {
                dv_cpy(g_notmine[g_notmine_w], s->call, sizeof g_notmine[0]);
                g_notmine_w = (g_notmine_w + 1) % 8;
            } else {
                log_line(s->call, s->mine ? "Found: it is yours" : "Found: it is waiting for an owner");
            }
        }
        if (s->mine) key_from_core(s, host_facts(s));
        if (!s->mine) s->asking = 0;          /* nothing more is coming */
        set_now(s, s->mine ? "Yours" : s->unowned ? "Waiting for an owner. Claim it to set it up."
                 : "Somebody else's station. You can see how it is doing; only its owner can change it.");
        listen();
        save();
        push_list();
        if (g_sel == i) { push_hub(); push_stats(); }
    } else if (dv_eq(sub, "mail")) {
        if (dv_field(wire, "mail", v, sizeof v)) dv_cpy(s->mail, v, sizeof s->mail);
        if (g_sel == i) push_stats();
    }
}

static void on_result(const char *row)
{
    char wire[FW_WIRE_MAX + 1], from[16], r[8], code[8], m[100], v[16];
    /* The cheap tests first: most answers on the air are somebody else's. */
    if (!dv_json(row, "forUs", v, sizeof v) || !dv_eq(v, "true")) return;
    /* An answer is believed only when it is signed by the key the station
     * is known by (XPRS.md 9.1): the core says whether it is, and anything
     * else could be anybody claiming the station said yes. */
    if (!dv_json(row, "sig", v, sizeof v) || !dv_eq(v, "verified")) return;
    if (!dv_json(row, "wire", wire, sizeof wire)) return;
    dv_field(wire, "f", from, sizeof from);
    if (!dv_field(wire, "r", r, sizeof r) || !dv_field(wire, "code", code, sizeof code)) return;
    m[0] = 0;
    dv_field(wire, "m", m, sizeof m);

    int i = -1;
    for (int j = 0; j < g_nst; j++)
        if (g_st[j].pend_id[0] && dv_eq(g_st[j].pend_id, r)) { i = j; break; }
    if (i < 0) return;                        /* not something we asked */
    st_t *s = &g_st[i];
    /* A repeat of the command gets where the station stands again; an
     * answer that says nothing new is not news. */
    if (s->pend_final && dv_eq(code, "202")) return;
    if (!dv_eq(from, s->call)) {
        /* Only the station, or the station under its new key, answers: the
         * key its 202 named, or, when that was missed, one it announced and
         * the core verified, answering our key command by its r:. */
        char k2[16] = "";
        const char *nk = 0;
        if (s->rekey_npub[0]) {
            dv_call_of(s->rekey_npub, "X3", k2, sizeof k2);
            if (dv_eq(from, k2)) nk = s->rekey_npub;
        }
        static char rk[80];
        if (dv_eq(s->pend_what, "call") && dv_field(wire, "k", rk, sizeof rk) &&
            dv_eq(rk, s->npub)) {
            /* Its new callsign, under the key we already hold (6.4.2). */
            rename_to(i, from);
        } else {
            if (!nk && dv_eq(s->pend_what, "key")) {
                /* Its own key, in the answer (11.10), or one it announced. */
                if (dv_field(wire, "k", rk, sizeof rk) && dv_call_matches(from, rk)) nk = rk;
                for (int j = 0; !nk && j < 4; j++)
                    if (dv_eq(g_ids[j].call, from)) nk = g_ids[j].npub;
            }
            if (!nk) return;
            dv_cpy(s->call, from, sizeof s->call);
            dv_cpy(s->npub, nk, sizeof s->npub);
            s->rekey_npub[0] = 0;
        }
    }
    take_state(s, wire);
    s->heard_ms = hal_time_ms();
    char what[8];
    dv_cpy(what, s->pend_what, sizeof what);

    if (dv_eq(code, "202")) {
        char k[80];
        if (dv_eq(what, "key") && dv_field(wire, "k", k, sizeof k)) {
            dv_cpy(s->rekey_npub, k, sizeof s->rekey_npub);
            char nc[16], line[96] = "Restarting under a new identity, ";
            dv_call_of(k, "X3", nc, sizeof nc);
            dv_cat(line, nc, sizeof line);
            dv_cat(line, "...", sizeof line);
            log_line(s->call, line);
            set_now(s, line);
        } else if (dv_eq(what, "wifi")) {
            log_line(s->call, "Joining the network...");
            set_now(s, "Joining the network...");
        } else if (dv_eq(what, "update")) {
            char line[96] = "Updating";
            if (g_latest[0]) { dv_cat(line, " to ", sizeof line); dv_cat(line, g_latest, sizeof line); }
            dv_cat(line, ". It restarts and says how it went in a few minutes.", sizeof line);
            log_line(s->call, line);
            set_now(s, line);
        } else if (dv_eq(what, "call")) {
            char line[80] = "Restarting as ";
            dv_cat(line, s->want_call[0] ? s->want_call : "its derived callsign", sizeof line);
            dv_cat(line, "...", sizeof line);
            log_line(s->call, line);
            set_now(s, line);
        } else {
            log_line(s->call, "Working on it...");
            set_now(s, "Working on it...");
        }
        s->pend_final = 1;
    } else if (dv_eq(code, "200")) {
        if (dv_eq(what, "claim")) {
            s->mine = 1;
            s->unowned = 0;
            s->theirs = 0;
            log_line(s->call, "Claimed. It is yours.");
            set_now(s, "Yours. Put it on your WiFi, or give it a name.");
        } else if (dv_eq(what, "wifi") && s->next_x[0]) {
            /* The name went; now the password. */
            char ts[24], w[FW_WIRE_UNSIGNED + 1];
            stamp_for(s, ts, sizeof ts);
            int rc = dv_sealed(w, sizeof w, g_me, s->call, ts, s->next_x);
            for (unsigned q = 0; s->next_x[q]; q++) s->next_x[q] = 0;
            done_pending(s);
            if (rc > 0) send_cmd(s, w, "wifi");
            set_now(s, "Sending the password...");
            save(); push_list(); if (g_sel == i) push_hub();
            return;
        } else if (dv_eq(what, "wifi")) {
            char line[80];
            dv_cpy(line, dv_eq(s->wifi, "off") ? "Off the WiFi" : "On the network", sizeof line);
            if (s->ip[0]) { dv_cat(line, " at ", sizeof line); dv_cat(line, s->ip, sizeof line); }
            log_line(s->call, line);
            set_now(s, line);
        } else if (dv_eq(what, "key")) {
            log_line(s->call, "New identity in use.");
            set_now(s, "New identity in use.");
        } else if (dv_eq(what, "call")) {
            char line[80] = "Transmitting as ";
            dv_cat(line, s->call, sizeof line);
            log_line(s->call, line);
            set_now(s, line);
        } else if (dv_eq(what, "update")) {
            char line[64] = "Updated: now XPRS ";
            dv_cat(line, s->fw, sizeof line);
            log_line(s->call, line);
            set_now(s, line);
        } else if (dv_eq(what, "zdiag")) {
            s->asking = 0;
            save_stats(s);
            log_line(s->call, "Stats read.");
            set_now(s, "Stats read.");
        } else if (dv_eq(what, "zcore")) {
            char line[160] = "Crash: ";
            dv_cat(line, s->crash, sizeof line);
            if (m[0]) { dv_cat(line, " at ", sizeof line); dv_cat(line, m, sizeof line); }
            log_line(s->call, line);
            set_now(s, "Crash report in Answers.");
        } else {
            log_line(s->call, "Saved.");
            set_now(s, "Saved.");
        }
        done_pending(s);
    } else if (dv_eq(code, "206") && dv_eq(what, "zcore")) {
        char line[160] = "Crash, more: ";
        dv_cat(line, m, sizeof line);
        log_line(s->call, line);
        return;
    } else if (dv_eq(code, "408") && !s->pend_408) {
        /* Not newer than the last command it took: say it again, later. The
         * same x: under a new ts: is the same secret, re-signed. */
        s->pend_408 = 1;
        char ts[24], w[FW_WIRE_UNSIGNED + 1], x[FW_WIRE_UNSIGNED], rest[FW_WIRE_UNSIGNED];
        stamp_for(s, ts, sizeof ts);
        int rc = -1;
        if (dv_field(s->pend, "x", x, sizeof x)) {
            rc = dv_sealed(w, sizeof w, g_me, s->call, ts, x);
        } else {
            const char *c = s->pend;
            while (*c && !dv_starts(c, "cmd:")) c++;
            dv_cpy(rest, c, sizeof rest);
            dv_cpy(w, "t:command f:", sizeof w);
            dv_cat(w, g_me, sizeof w); dv_cat(w, " d:", sizeof w); dv_cat(w, s->call, sizeof w);
            dv_cat(w, " ts:", sizeof w); dv_cat(w, ts, sizeof w); dv_cat(w, " ", sizeof w);
            dv_cat(w, rest, sizeof w);
            rc = dv_len(w) <= FW_WIRE_UNSIGNED ? (int)dv_len(w) : -1;
        }
        done_pending(s);
        if (rc > 0) send_cmd(s, w, what);
        s->pend_408 = 1;              /* a second 408 is the clock, said once */
    } else {
        char line[160] = "";
        if (dv_eq(code, "403")) {
            dv_cpy(line, "Refused: ", sizeof line);
            if (dv_eq(what, "claim") || dv_starts(m, "not the owner")) {
                s->theirs = 1; s->unowned = 0; s->mine = 0;
            }
        }
        else if (dv_eq(code, "408")) dv_cpy(line, "Refused as old. Is this phone's clock right? ", sizeof line);
        else if (dv_eq(code, "404")) dv_cpy(line, "Its firmware cannot do that; update it first. ", sizeof line);
        else if (dv_eq(code, "429")) dv_cpy(line, "Busy, try again in a moment. ", sizeof line);
        else if (dv_eq(code, "500") && dv_eq(what, "wifi")) dv_cpy(line, "Could not join: ", sizeof line);
        else if (dv_eq(code, "500") && dv_eq(what, "update")) dv_cpy(line, "The update did not take, it went back to its old firmware: ", sizeof line);
        else { dv_cpy(line, "Refused (", sizeof line); dv_cat(line, code, sizeof line); dv_cat(line, "): ", sizeof line); }
        dv_cat(line, m, sizeof line);
        log_line(s->call, line);
        set_now(s, line);
        if (dv_eq(what, "zdiag")) s->asking = 0;
        done_pending(s);
    }
    save();
    push_list();
    if (g_sel == i) { push_hub(); if (dv_eq(what, "zdiag") || dv_eq(what, "zcore")) push_stats(); }
}

/* The core gave up on a command: nothing final came back in 11.4's
 * window, however often it was aired. */
static void on_status(const char *row)
{
    char id[8], state[16];
    if (!dv_json(row, "id", id, sizeof id) || !dv_json(row, "state", state, sizeof state)) return;
    for (int i = 0; i < g_nst; i++) {
        st_t *s = &g_st[i];
        if (!s->pend_id[0] || !dv_eq(s->pend_id, id)) continue;
        const char *line;
        if (dv_eq(state, "unfinished"))
            line = "It took the command but never said how it ended. Read its stats to see where it is.";
        else if (dv_eq(state, "unanswered"))
            line = "No answer in five minutes. Is it still in range, and powered?";
        else
            return;
        log_line(s->call, line);
        set_now(s, line);
        s->asking = 0;
        done_pending(s);
        if (g_sel == i) { push_hub(); push_stats(); }
        return;
    }
}

/* ── Flashing over USB ────────────────────────────────────────────────── */
/* The core owns the cable, the loader, the catalogue and the files
 * (hal_flash_*); this wapp picks, asks, and draws what `core.flash` says.
 *
 *   the list    the boards on the cable lead it. Tap one.
 *   Flash       that board. Opening it identifies the board; when one
 *               published firmware fits, it is chosen without another tap,
 *               and every published firmware is already here (prefetch), so
 *               Install XPRS is ready at once.
 *   Firmware    the catalogue with the website's pictures, the boards that
 *               fit this chip first; tapping one chooses it and goes back.
 *
 * The state is read ONCE per event into the structs below and every screen
 * is drawn from them. Drawing straight off the 8 KB JSON, one scan per
 * field, cost 60 to 200 ms per progress event on the phone, on the isolate
 * the Android session awaits on (performance.md 8.4: a wapp's cost is what
 * it does per event).
 */
typedef struct { char id[64], port[48], product[64]; int permitted, native; } fdev_t;
typedef struct {
    char id[24], name[40], family[16], version[16], port[16], local[16], photo[200];
    unsigned mb; unsigned long long local_bytes; int flashable;
} fboard_t;
static struct {
    char phase[16], message[200], error[200], note[120], board[24], part[32];
    int busy, scanning, supported;
    /* The last cable claim (the core writes the owner on the board's
     * console right after the image, XPRS.md 11.9): which station, for
     * whom, whether it took, and its sequence number. */
    char claim_station[12], claim_owner[16];
    int claim_ok;
    unsigned claim_seq;
    unsigned part_index, part_count;
    unsigned long long done, total;
    /* the probed device */
    char pid[64], chip[16], chip_label[16], runs[40], runs_version[24], suggested[24], likely[200];
    unsigned long long flash_bytes;
    fdev_t dev[8]; int ndev;
    fboard_t brd[16]; int nbrd;
} g_fs;
static char g_flash[8192];       /* the last hal_flash_state answer */
static int  g_sub_flash;
static char g_dev[64];           /* the device on the Device screen */
static char g_board[24];         /* the firmware chosen for it */
static char g_fetched[24];       /* the board a download was asked for, once */
static char g_obj[1024];         /* one row while parsing */

static int fields(const char *key, char *out, unsigned cap);

static unsigned long long num_of(const char *json, const char *key)
{
    char v[24] = "";
    unsigned long long n = 0;
    if (dv_json(json, key, v, sizeof v))
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (unsigned)(*p - '0');
    return n;
}

static int bool_of(const char *json, const char *key)
{
    char v[8] = "";
    dv_json(json, key, v, sizeof v);
    return dv_eq(v, "true");
}

/* One read, one parse. */
static void flash_read(void)
{
    int32_t n = hal_flash_state(g_flash, sizeof g_flash - 1);
    if (n < 0 || (unsigned)n >= sizeof g_flash) n = 0;
    g_flash[n] = 0;
    const char *j = g_flash;
    dv_json(j, "phase", g_fs.phase, sizeof g_fs.phase);
    dv_json(j, "message", g_fs.message, sizeof g_fs.message);
    dv_json(j, "error", g_fs.error, sizeof g_fs.error);
    dv_json(j, "catalogNote", g_fs.note, sizeof g_fs.note);
    dv_json(j, "board", g_fs.board, sizeof g_fs.board);
    dv_json(j, "part", g_fs.part, sizeof g_fs.part);
    g_fs.busy = bool_of(j, "busy");
    g_fs.scanning = bool_of(j, "scanning");
    /* Absent (no state yet) reads as supported: the tab says nothing rash. */
    g_fs.supported = !(dv_json(j, "supported", g_obj, 8) && dv_eq(g_obj, "false"));
    g_fs.claim_station[0] = g_fs.claim_owner[0] = 0;
    g_fs.claim_ok = 0;
    g_fs.claim_seq = 0;
    if (dv_json_obj(j, "claim", g_obj, sizeof g_obj)) {
        dv_json(g_obj, "station", g_fs.claim_station, sizeof g_fs.claim_station);
        dv_json(g_obj, "owner", g_fs.claim_owner, sizeof g_fs.claim_owner);
        g_fs.claim_ok = bool_of(g_obj, "ok");
        g_fs.claim_seq = (unsigned)num_of(g_obj, "seq");
    }
    g_fs.part_index = (unsigned)num_of(j, "partIndex");
    g_fs.part_count = (unsigned)num_of(j, "partCount");
    g_fs.done = num_of(j, "done");
    g_fs.total = num_of(j, "total");
    g_fs.ndev = 0;
    for (const char *p = dv_json_arr(j, "devices"); (p = dv_json_next(p, g_obj, sizeof g_obj)) && g_fs.ndev < 8; ) {
        fdev_t *d = &g_fs.dev[g_fs.ndev++];
        dv_json(g_obj, "id", d->id, sizeof d->id);
        dv_json(g_obj, "port", d->port, sizeof d->port);
        dv_json(g_obj, "product", d->product, sizeof d->product);
        d->permitted = bool_of(g_obj, "permitted");
        d->native = bool_of(g_obj, "nativeUsb");
    }
    g_fs.nbrd = 0;
    for (const char *p = dv_json_arr(j, "boards"); (p = dv_json_next(p, g_obj, sizeof g_obj)) && g_fs.nbrd < 16; ) {
        fboard_t *b = &g_fs.brd[g_fs.nbrd++];
        dv_json(g_obj, "id", b->id, sizeof b->id);
        dv_json(g_obj, "name", b->name, sizeof b->name);
        dv_json(g_obj, "family", b->family, sizeof b->family);
        dv_json(g_obj, "version", b->version, sizeof b->version);
        dv_json(g_obj, "port", b->port, sizeof b->port);
        dv_json(g_obj, "local", b->local, sizeof b->local);
        dv_json(g_obj, "photo", b->photo, sizeof b->photo);
        b->mb = (unsigned)num_of(g_obj, "flashMb");
        b->local_bytes = num_of(g_obj, "localBytes");
        b->flashable = bool_of(g_obj, "flashable");
        if (b->flashable && dv_ver_cmp(b->version, g_latest) > 0) dv_cpy(g_latest, b->version, sizeof g_latest);
    }
    g_fs.pid[0] = g_fs.chip[0] = g_fs.chip_label[0] = g_fs.runs[0] = g_fs.runs_version[0] = 0;
    g_fs.suggested[0] = g_fs.likely[0] = 0;
    g_fs.flash_bytes = 0;
    if (dv_json_obj(j, "device", g_obj, sizeof g_obj)) {
        dv_json(g_obj, "id", g_fs.pid, sizeof g_fs.pid);
        dv_json(g_obj, "chip", g_fs.chip, sizeof g_fs.chip);
        dv_json(g_obj, "chipLabel", g_fs.chip_label, sizeof g_fs.chip_label);
        dv_json(g_obj, "runs", g_fs.runs, sizeof g_fs.runs);
        dv_json(g_obj, "runsVersion", g_fs.runs_version, sizeof g_fs.runs_version);
        dv_json(g_obj, "suggested", g_fs.suggested, sizeof g_fs.suggested);
        dv_json(g_obj, "likely", g_fs.likely, sizeof g_fs.likely);
        g_fs.flash_bytes = num_of(g_obj, "flashBytes");
    }
}

static int phase_is(const char *what) { return dv_eq(g_fs.phase, what); }

static const fdev_t *dev_of(const char *id)
{
    for (int i = 0; i < g_fs.ndev; i++) if (dv_eq(g_fs.dev[i].id, id)) return &g_fs.dev[i];
    return 0;
}

static const fboard_t *board_of(const char *id)
{
    for (int i = 0; i < g_fs.nbrd; i++) if (dv_eq(g_fs.brd[i].id, id)) return &g_fs.brd[i];
    return 0;
}

/* done/total as "0.41" for a tile's progress. */
static void frac_words(char *out, unsigned cap)
{
    unsigned pc = g_fs.total ? (unsigned)(g_fs.done * 100 / g_fs.total) : 0;
    if (pc > 100) pc = 100;
    dv_cpy(out, pc >= 100 ? "1" : pc < 10 ? "0.0" : "0.", cap);
    if (pc < 100) dv_cat_u(out, pc, cap);
}


/* "ESP32S3, 16 MB, v0.4.0" for a board. */
static void board_words(const fboard_t *b, char *sub, unsigned cap)
{
    dv_cpy(sub, b->family, cap);
    for (char *p = sub; *p; p++) *p = dv_up(*p);
    if (b->mb) { dv_cat(sub, ", ", cap); dv_cat_u(sub, b->mb, cap); dv_cat(sub, " MB", cap); }
    /* The newer of what the catalogue names and what is downloaded: the
     * image's own manifest is the truth when a release left the catalogue
     * behind. */
    const char *v = b->local[0] && dv_ver_cmp(b->local, b->version) > 0 ? b->local : b->version;
    if (v[0]) { dv_cat(sub, ", XPRS ", cap); dv_cat(sub, v, cap); }
}

/* The probe's facts are about g_dev. */
static int probed(void) { return g_dev[0] && dv_eq(g_fs.pid, g_dev); }

/* Is [id] among the ids the probe said fit? */
static int fits(const char *id)
{
    for (const char *p = g_fs.likely; *p; ) {
        const char *q = p;
        while (*q && *q != ' ') q++;
        /* The length test makes fw_starts an exact-word match. */
        if ((unsigned)(q - p) == dv_len(id) && dv_starts(p, id)) return 1;
        p = *q ? q + 1 : q;
    }
    return 0;
}

/* "ttyACM0" or "001/002": the part of a port a person can match to a cable. */
static const char *port_short(const char *port)
{
    return dv_starts(port, "/dev/bus/usb/") ? port + 13 : dv_starts(port, "/dev/") ? port + 5 : port;
}

/* The board a device is, as far as anyone knows: the firmware chosen for
 * it, else the probe's one clear match. */
static const fboard_t *board_for(const fdev_t *d)
{
    if (!d) return 0;
    if (dv_eq(d->id, g_dev) && g_board[0]) return board_of(g_board);
    if (dv_eq(d->id, g_fs.pid) && g_fs.suggested[0]) return board_of(g_fs.suggested);
    return 0;
}

/* Whether the probe found XPRS on it: the image's project name says so
 * (esp32c3_xprs, tdongle_xprs). */
static int runs_xprs(void)
{
    for (const char *p = g_fs.runs; *p; p++) if (dv_starts(p, "xprs")) return 1;
    return 0;
}

/* "Runs XPRS 0.5.1", "Runs other firmware", "No firmware read yet". */
static void runs_words(char *out, unsigned cap)
{
    if (!g_fs.runs[0]) { dv_cpy(out, "no XPRS on it yet", cap); return; }
    if (runs_xprs()) {
        dv_cpy(out, "XPRS", cap);
        if (g_fs.runs_version[0]) { dv_cat(out, " ", cap); dv_cat(out, g_fs.runs_version, cap); }
        return;
    }
    dv_cpy(out, "other firmware (", cap);
    dv_cat(out, g_fs.runs, cap);
    dv_cat(out, ")", cap);
}

static void list_cable(int *any)
{
    int first = 1;
    for (int i = 0; i < g_fs.ndev; i++) {
        const fdev_t *d = &g_fs.dev[i];
        if (first) {
            if (*any) dv_cat(g_out, ",", sizeof g_out);
            dv_cat(g_out, "{\"title\":\"Connected by cable\",\"items\":[", sizeof g_out);
            *any = 1;
        }
        const fboard_t *b = board_for(d);
        int known = dv_eq(d->id, g_fs.pid) && g_fs.chip_label[0];
        char title[64], sub[128] = "", tags[120] = "";
        if (b) dv_cpy(title, b->name, sizeof title);
        else if (known) { dv_cpy(title, g_fs.chip_label, sizeof title); dv_cat(title, " board", sizeof title); }
        else dv_cpy(title, d->native ? "ESP32 board" : "Board on a USB adapter", sizeof title);
        if (known) {
            char r[64];
            runs_words(r, sizeof r);
            dv_cpy(sub, "Runs ", sizeof sub);
            dv_cat(sub, r, sizeof sub);
            dv_cat(sub, runs_xprs() && !behind(g_fs.runs_version) ? ". Tap to reinstall"
                      : runs_xprs() ? ". Tap to update" : ". Tap to install XPRS", sizeof sub);
        } else {
            dv_cpy(sub, "Tap to see what it is and install XPRS", sizeof sub);
        }
        tag(tags, sizeof tags, "", port_short(d->port));
        if (!d->permitted) tag(tags, sizeof tags, "", "asks first");
        item_full(d->id, title, sub, tags, 0, b && b->photo[0] ? "" : "usb", b ? b->photo : "", &first);
    }
    if (!first) dv_cat(g_out, "]}", sizeof g_out);
}

/* Nothing on the cable: say once, at the bottom, how a board gets here. */
static void list_cable_help(int *any)
{
    if (g_fs.ndev) return;
    int first = 1;
    if (*any) dv_cat(g_out, ",", sizeof g_out);
    *any = 1;
    dv_cat(g_out, "{\"title\":\"Install XPRS on a new board\",\"items\":[", sizeof g_out);
    if (g_fs.supported)
        item_full("_cable", "Connect a board with a USB cable",
                  "It shows up here. On a phone, use a USB OTG adapter.", "", 0, "usb", "", &first);
    else
        item_full("_cable", "This device cannot flash over USB",
                  "Use a phone or a computer with a USB port.", "", 1, "usb", "", &first);
    dv_cat(g_out, "]}", sizeof g_out);
}

/* Advanced options shown, settings erased on the next write, and the
 * callsign chosen for the next board (its key, held in memory only and
 * zeroed once it is sent or dropped). */
static int  g_adv, g_wipe;

/* What the Flash panel is called: the board, once it is known (it is
 * reopened under that name when the probe says, g_flash_shown). */
static const char *flash_title(void)
{
    const fboard_t *b = g_board[0] ? board_of(g_board) : 0;
    if (!b && probed() && g_fs.suggested[0]) b = board_of(g_fs.suggested);
    return b ? b->name : "Install XPRS";
}

/* The Flash panel: the board's picture, one line saying what happens next,
 * a progress bar while it works, what is known about the board, and one
 * button. Everything else is under Advanced options. */
static void push_device(void)
{
    if (!g_dev[0]) return;
    const fdev_t *d = dev_of(g_dev);
    int have = probed();
    const fboard_t *b = g_board[0] ? board_of(g_board) : 0;
    int mine = b && dv_eq(g_fs.board, g_board);
    int fetching = mine && phase_is("fetching");
    int writing = mine && (phase_is("writing") || phase_is("verifying"));
    /* A finished download is what gets flashed, whatever version the
     * catalogue names: the image's own manifest is the truth, and a catalogue
     * a release left behind must not hide Install behind "Downloading". */
    int ready = b && b->local[0] && !fetching;
    int nfit = 0;
    if (have) {
        for (const char *p = g_fs.likely; *p; p++) if (*p == ' ') nfit++;
        if (g_fs.likely[0]) nfit++;
    }

    char now[220] = "";
    if (g_fs.busy) dv_cpy(now, g_fs.message, sizeof now);
    else if (g_fs.error[0]) dv_cpy(now, g_fs.error, sizeof now);
    else if (mine && phase_is("done")) dv_cpy(now, g_fs.message, sizeof now);
    else if (!d) dv_cpy(now, "Unplugged. Plug it back in to go on.", sizeof now);
    else if (!have && !g_board[0]) dv_cpy(now, "Identifying the board...", sizeof now);
    else if (!g_board[0] && !g_fs.chip[0])
        dv_cpy(now, "Could not tell what board this is. Open Advanced options and choose it by its picture.", sizeof now);
    else if (!b) dv_cpy(now, nfit > 1 ? "Several boards use this chip. Open Advanced options and choose yours by its picture."
                        : "No XPRS firmware fits this chip yet.", sizeof now);
    else if (!b->flashable) { dv_cpy(now, b->name, sizeof now); dv_cat(now, " is not installed over USB. Choose another board.", sizeof now); }
    else if (ready) {
        dv_cpy(now, !runs_xprs() ? "Ready to install XPRS " : behind(g_fs.runs_version) ? "Ready to update to " : "Ready to reinstall XPRS ", sizeof now);
        dv_cat(now, b->local, sizeof now);
        dv_cat(now, ". Keep the cable in until it says done.", sizeof now);
    }
    else { dv_cpy(now, "Downloading XPRS ", sizeof now); dv_cat(now, b->version, sizeof now); dv_cat(now, "...", sizeof now); }
    one_line("dev_now", "Now", now);

    {
        const fboard_t *pic = b ? b : (have && g_fs.suggested[0]) ? board_of(g_fs.suggested) : 0;
        field_set("fl_pic", pic ? pic->photo : "");
        flag_hidden("fl_pic", !(pic && pic->photo[0]));
    }

    tiles_begin("dev_hub");
    if (fetching || writing) {
        char prog[8] = "", pc[8] = "", hint[64] = "";
        frac_words(prog, sizeof prog);
        unsigned p = g_fs.total ? (unsigned)(g_fs.done * 100 / g_fs.total) : 0;
        dv_cat_u(pc, p > 100 ? 100 : p, sizeof pc);
        if (writing) {
            dv_cpy(hint, g_fs.part, sizeof hint);
            if (g_fs.part_count) { dv_cat(hint, ", part ", sizeof hint); dv_cat_u(hint, g_fs.part_index, sizeof hint); dv_cat(hint, " of ", sizeof hint); dv_cat_u(hint, g_fs.part_count, sizeof hint); }
        } else {
            dv_cpy(hint, "XPRS ", sizeof hint); dv_cat(hint, b->version, sizeof hint);
        }
        tile("progress", fetching ? "Downloading" : phase_is("verifying") ? "Checking" : "Installing",
             pc, "%", hint, prog, 0);
    }
    tiles_end();
    flag_hidden("dev_hub", !(fetching || writing));

    det_begin("fl_info");
    det_section("");
    {
        const fboard_t *bb = b ? b : (have && g_fs.suggested[0]) ? board_of(g_fs.suggested) : 0;
        det_row("Board", bb ? bb->name : "not chosen yet");
    }
    if (have && g_fs.chip_label[0]) {
        char c[48];
        dv_cpy(c, g_fs.chip_label, sizeof c);
        if (g_fs.flash_bytes) { dv_cat(c, ", ", sizeof c); dv_cat_u(c, (unsigned)(g_fs.flash_bytes / 1048576ULL), sizeof c); dv_cat(c, " MB", sizeof c); }
        det_row("Chip", c);
        char r[64];
        runs_words(r, sizeof r);
        det_row("Runs now", r);
    } else {
        det_row("Chip", g_fs.busy && phase_is("probing") ? "asking it..." : "not identified");
    }
    if (b && b->flashable) {
        char v[64] = "XPRS ";
        dv_cat(v, ready ? b->local : b->version, sizeof v);
        dv_cat(v, ready ? ", downloaded" : fetching ? ", downloading" : "", sizeof v);
        det_row("Installs", v);
    }
    det_row("Callsign", g_want_call[0] ? g_want_call
                      : !(have && g_fs.chip[0]) ? ""
                      : (g_wipe || !runs_xprs()) ? "a new one, made by the board" : "kept");
    det_row("Settings", g_wipe ? "erased: key, owner and WiFi" : "kept");
    {
        const char *port = d ? port_short(d->port) : "unplugged";
        det_row("Cable", port);
    }
    det_end();

    flag_hidden("flash_write", g_fs.busy || !ready || !d || !b->flashable);
    if (ready) {
        char l[48];
        dv_cpy(l, !runs_xprs() ? "Install XPRS " : behind(g_fs.runs_version) ? "Update to " : "Reinstall XPRS ", sizeof l);
        dv_cat(l, b->local, sizeof l);
        field_set("flash_write__label", l);
    }
    flag_hidden("flash_cancel", !g_fs.busy);
    flag_hidden("flash_choose", g_fs.busy);
    flag_hidden("flash_identify", g_fs.busy || !d);
    flag_hidden("fl_advanced", !g_adv);
    flag_hidden("fl_callsign_clear", !g_want_call[0]);
    /* Named after the board once the probe says what it is. */
    if (dv_eq(g_screen, "Flash") && !dv_eq(g_flash_shown, flash_title())) screen_open("Flash", flash_title());
}

/* The Firmware screen: every published board with its picture, the ones
 * that fit this chip first. */
static void push_pick(void)
{
    int have = probed();
    dv_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"flash_pick\",\"sections\":[", sizeof g_out);
    int any = 0;
    for (int pass = 0; pass < 2; pass++) {
        int first = 1;
        for (int i = 0; i < g_fs.nbrd; i++) {
            const fboard_t *b = &g_fs.brd[i];
            int fit = have && fits(b->id);
            if ((pass == 0) != fit) continue;
            if (first) {
                if (any) dv_cat(g_out, ",", sizeof g_out);
                dv_cat(g_out, pass == 0 ? "{\"title\":\"Fits this board\",\"items\":[" : "{\"title\":\"All boards\",\"items\":[", sizeof g_out);
                any = 1;
            }
            char sub[64], tags[100] = "";
            board_words(b, sub, sizeof sub);
            if (have && dv_eq(b->id, g_fs.suggested)) tag(tags, sizeof tags, "", "best match");
            if (dv_eq(b->id, g_board)) tag(tags, sizeof tags, "", "chosen");
            if (b->local[0]) tag(tags, sizeof tags, "", "downloaded");
            if (!b->flashable) tag(tags, sizeof tags, "", "not over USB");
            if (!first) dv_cat(g_out, ",", sizeof g_out);
            first = 0;
            dv_cat(g_out, "{\"id\":\"", sizeof g_out); dv_jesc(g_out, b->id, sizeof g_out);
            dv_cat(g_out, "\",\"title\":\"", sizeof g_out); dv_jesc(g_out, b->name, sizeof g_out);
            dv_cat(g_out, "\",\"subtitle\":\"", sizeof g_out); dv_jesc(g_out, sub, sizeof g_out);
            dv_cat(g_out, "\",\"picture\":\"", sizeof g_out); dv_jesc(g_out, b->photo, sizeof g_out);
            dv_cat(g_out, "\",\"tags\":[", sizeof g_out); dv_cat(g_out, tags, sizeof g_out); dv_cat(g_out, "]", sizeof g_out);
            if (!b->flashable) dv_cat(g_out, ",\"dim\":true", sizeof g_out);
            dv_cat(g_out, "}", sizeof g_out);
        }
        if (!first) dv_cat(g_out, "]}", sizeof g_out);
    }
    dv_cat(g_out, "]}", sizeof g_out);
    say(g_out);
}

/* The list changes when the cable or what is known about it does, not on
 * every progress post (ten a second while writing): a key of what the
 * cable rows show, compared before the list is drawn again. */
static char g_cable_key[160];

static void push_flash_all(void)
{
    char k[160] = "";
    dv_cat(k, g_fs.phase, sizeof k);
    dv_cat_u(k, (unsigned)g_fs.ndev, sizeof k);
    for (int i = 0; i < g_fs.ndev; i++) dv_cat(k, port_short(g_fs.dev[i].port), sizeof k);
    dv_cat(k, g_fs.pid, sizeof k);
    dv_cat(k, g_fs.suggested, sizeof k);
    dv_cat(k, g_board, sizeof k);
    dv_cat(k, g_fs.runs_version, sizeof k);
    dv_cat(k, g_latest, sizeof k);
    if (!dv_eq(k, g_cable_key)) { dv_cpy(g_cable_key, k, sizeof g_cable_key); push_list(); }
    push_device();
    push_pick();
}

/* Choose [id] for g_dev and get it, once. 1 when a download was asked. */
static int choose(const char *id)
{
    dv_cpy(g_board, id, sizeof g_board);
    const fboard_t *b = board_of(id);
    if (!b) return 0;
    if (b->flashable && !(b->local[0] && dv_eq(b->local, b->version)) && !g_fs.busy && !dv_eq(g_fetched, id)) {
        /* Only once the core took it: one asked while a probe ran is asked
         * again when the core is free (on_flash). */
        if (hal_flash_fetch(g_board, dv_len(g_board))) { dv_cpy(g_fetched, id, sizeof g_fetched); return 1; }
    }
    return 0;
}

/* Keep every published firmware on this device while the page is open:
 * they are small (1 to 2 MB), and a board plugged in later flashes at once,
 * with or without a network then. One download at a time, each board asked
 * for once a session, so a failing one is not asked again in a loop. */
static char g_tried[16][24];
static int  g_ntried, g_prefetch;

static int prefetch(void)
{
    if (!g_prefetch || g_fs.busy) return 0;
    for (int i = 0; i < g_fs.nbrd; i++) {
        const fboard_t *b = &g_fs.brd[i];
        if (!b->flashable || (b->local[0] && dv_ver_cmp(b->local, b->version) >= 0)) continue;
        int seen = 0;
        for (int j = 0; j < g_ntried; j++) if (dv_eq(g_tried[j], b->id)) seen = 1;
        if (seen || g_ntried >= 16) continue;
        dv_cpy(g_tried[g_ntried++], b->id, sizeof g_tried[0]);
        if (hal_flash_fetch(b->id, dv_len(b->id))) return 1;
    }
    return 0;
}

/* A board the core just made ours over the cable (XPRS.md 11.9): on the
 * list at once, as ours. It never asks to be claimed, so without this it
 * would only turn up when discover() heard it, and a board with no WiFi yet
 * is not heard by a desktop at all. Its key comes from the core once it is
 * heard (key_from_core); the policy ask confirms what the cable set. Once
 * per claim: the core numbers them. When a callsign was chosen for it, its
 * key follows as soon as the station is heard (give_key). */
static unsigned g_claim_seen;

static void on_claim(void)
{
    if (!g_fs.claim_seq || g_fs.claim_seq == g_claim_seen) return;
    g_claim_seen = g_fs.claim_seq;
    if (!g_fs.claim_ok || !g_fs.claim_station[0]) {
        if (g_want_nsec[0]) {
            log_line("", "Not made yours over the cable, so its callsign was not set. Claim it, then choose one on its Callsign screen.");
            forget_wanted();
        }
        return;
    }
    int i = add(g_fs.claim_station);
    if (i < 0) return;
    st_t *s = &g_st[i];
    s->mine = 1; s->unowned = 0; s->theirs = 0;
    dv_cpy(s->pol_owner, g_fs.claim_owner, sizeof s->pol_owner);
    key_from_core(s, host_facts(s));
    set_now(s, "Yours");
    log_line(s->call, "Made yours over the USB cable");
    {
        /* When this device installed it, for "Installed 3 days ago". */
        char k[24] = "fl.", v[48] = "";
        const fboard_t *b = board_of(g_fs.board);
        dv_cat(k, s->call, sizeof k);
        dv_cat_u(v, hal_time_epoch(), sizeof v);
        dv_cat(v, "|", sizeof v);
        if (b) dv_cat(v, b->local, sizeof v);
        kv_put(k, v);
        if (b && b->local[0]) dv_cpy(s->fw, b->local, sizeof s->fw);
    }
    if (g_want_nsec[0]) { dv_cpy(g_key_for, s->call, sizeof g_key_for); listen(); give_key(); }
    save();
    /* The list shows it under Your stations now, not "Tap to claim it". */
    push_list();
    if (g_sel == i) push_hub();
}

/* The core moved: after a probe with one clear match, choose and fetch it;
 * otherwise, while nothing else runs, fetch the next published firmware. */
static void on_flash(void)
{
    flash_read();
    on_claim();
    int asked = 0;
    if (g_dev[0] && !g_board[0] && !g_fs.busy && probed() && g_fs.suggested[0]) {
        asked = choose(g_fs.suggested);
        flash_read();
    } else if (g_board[0] && !g_fs.busy && !dv_eq(g_fetched, g_board)) {
        /* Chosen while the core was busy: get it now. */
        asked = choose(g_board);
        flash_read();
    }
    if (!asked && prefetch()) flash_read();
    give_key();
    push_flash_all();
}

/* Opening a board on the cable is asking what it is: the board goes into
 * its loader for a moment and restarts. Not while something else is
 * running, and not again for one that answered: a second tap asks again
 * only after a probe that failed. */
static void open_usb(const char *id)
{
    if (!dv_eq(id, g_dev)) { g_board[0] = 0; g_fetched[0] = 0; }
    dv_cpy(g_dev, id, sizeof g_dev);
    flash_read();
    if (!g_fs.busy && (!probed() || !g_fs.chip[0] || g_fs.error[0]))
        hal_flash_probe(g_dev, dv_len(g_dev));
    else if (!g_fs.busy && !g_board[0] && g_fs.suggested[0])
        choose(g_fs.suggested);         /* identified on an earlier visit */
    flash_read();
    push_device();
    screen_open("Flash", flash_title());
}

static void on_flash_command(const char *cmd)
{
    sub(&g_sub_flash, 1, "core.flash");
    if (dv_eq(cmd, "flash_refresh")) {
        if (!hal_flash_scan()) log_line("", "No USB here");
    } else if (dv_eq(cmd, "flash_tap")) {
        char id[70] = "";
        fields("flash_id", id, sizeof id);
        if (id[0]) open_usb(id);
        return;
    } else if (dv_eq(cmd, "flash_choose")) {
        flash_read();
        push_pick();
        screen_open("Firmware", "Choose the board");
        return;
    } else if (dv_eq(cmd, "flash_pick_tap")) {
        char id[24] = "";
        fields("flash_pick_id", id, sizeof id);
        if (!id[0]) return;
        flash_read();
        g_fetched[0] = 0;
        choose(id);
        flash_read();
        push_flash_all();
        /* Back to the board it was chosen for. */
        screen_open("Flash", flash_title());
        return;
    } else if (dv_eq(cmd, "flash_identify")) {
        if (!g_dev[0]) log_line("", "Pick a board on the cable first");
        else if (!hal_flash_probe(g_dev, dv_len(g_dev))) log_line("", "Busy, wait for it to finish");
    } else if (dv_eq(cmd, "flash_write")) {
        if (!g_board[0]) log_line("", "Choose the board first");
        else if (!g_dev[0]) log_line("", "Pick a board on the cable first");
        else if (!hal_flash_write(g_dev, dv_len(g_dev), g_board, dv_len(g_board), g_wipe))
            log_line("", "Not now: busy, or not downloaded yet");
        else {
            char l[120] = "Writing ";
            dv_cat(l, g_board, sizeof l); dv_cat(l, " to ", sizeof l); dv_cat(l, g_dev, sizeof l);
            if (g_wipe) dv_cat(l, ", settings wiped", sizeof l);
            log_line("", l);
        }
    } else if (dv_eq(cmd, "flash_cancel")) {
        hal_flash_cancel();
    } else if (dv_eq(cmd, "fl_adv")) {
        char v[8] = "";
        fields("fl_adv", v, sizeof v);
        g_adv = dv_eq(v, "true");
    } else if (dv_eq(cmd, "fl_wipe")) {
        char v[8] = "";
        fields("wipe", v, sizeof v);
        g_wipe = dv_eq(v, "true");
    } else if (dv_eq(cmd, "fl_callsign")) {
        /* The app's own search screen, on its own thread: a station's
         * callsign is X3 and the four characters its key starts with. */
        say("{\"type\":\"ui.callsign.pick\",\"field\":\"vanity\",\"prefix\":\"X3\",\"length\":4}");
        return;
    } else if (dv_eq(cmd, "fl_callsign_clear")) {
        forget_wanted();
    } else if (dv_eq(cmd, "vanity_picked")) {
        char c[16] = "", n[72] = "";
        fields("vanity_call", c, sizeof c);
        fields("vanity_nsec", n, sizeof n);
        if (dv_starts(c, "X3") && dv_starts(n, "nsec1") && dv_len(n) == 63) {
            forget_wanted();
            dv_cpy(g_want_call, c, sizeof g_want_call);
            dv_cpy(g_want_nsec, n, sizeof g_want_nsec);
            char l[64] = "The next board flashed here becomes ";
            dv_cat(l, c, sizeof l);
            log_line("", l);
        }
        for (unsigned i = 0; n[i]; i++) n[i] = 0;
    }
    flash_read();
    push_flash_all();
}

/* One event the shell took off the queue. The topics are shared with the
 * other half, so each handler here ignores what it did not ask for. */
void st_event(void)
{
    if (dv_eq(g_topic, "xprs.request"))           on_request(g_ev);
    else if (dv_eq(g_topic, "xprs.result"))       { if (g_sub_res) on_result(g_ev); }
    else if (dv_eq(g_topic, "xprs.status.tx"))    { if (g_sub_tx) on_status(g_ev); }
    else if (dv_eq(g_topic, "xprs.identity"))     { if (g_sub_ids) on_identity(g_ev); }
    else if (dv_eq(g_topic, "xprs.observation"))  { if (g_sub_obs) on_observation(g_ev); }
    else if (dv_eq(g_topic, "core.flash"))        on_flash();
}

/* ── What the person does ─────────────────────────────────────────────── */
static st_t *selected(void)
{
    if (g_sel < 0 || g_sel >= g_nst) { log_line("", "Pick a station first"); return 0; }
    return &g_st[g_sel];
}

static int fields(const char *key, char *out, unsigned cap)
{
    /* The host bundles every scalar field under "fields"; the name is unique
     * enough in this wapp that the flat scan finds the right one. */
    return dv_json(g_buf, key, out, cap);
}

static void fresh_visit(void);

void st_command(void)
{
    char cmd[40] = "";
    if (!dv_json(g_buf, "command", cmd, sizeof cmd)) return;
    if (dv_eq(cmd, "ready") || dv_eq(cmd, "refresh")) {
        who_am_i();
        this_device();
        owned_from_core();
        if (dv_eq(cmd, "ready")) fresh_visit();
        /* Look again: ask what is on the local network to say who it is
         * now (rate-limited by the core), and read what has been heard. */
        if (dv_eq(cmd, "refresh")) hal_xprs_discover();
        discover();
        /* The page is open: look at the cable (sysfs, or the USB host
         * list) and at the catalogue when it is stale. Cheap, and no
         * network unless the catalogue is over an hour old. Then keep the
         * published firmwares here, one after the other. */
        sub(&g_sub_flash, 1, "core.flash");
        hal_flash_scan();
        flash_read();
        g_prefetch = 1;
        if (prefetch()) flash_read();
        give_key();
        g_cable_key[0] = 0;              /* draw the list whatever changed */
        push_flash_all();
        if (g_sel >= 0) push_hub();
        return;
    }
    if (dv_eq(cmd, "screen_closed")) {
        /* The back arrow or the back gesture closed a panel: step back to
         * the one it was opened from, not all the way to the list. */
        char was[16];
        dv_cpy(was, g_screen, sizeof was);
        g_screen[0] = 0;
        if ((dv_eq(was, "WiFi") || dv_eq(was, "Settings") || dv_eq(was, "Callsign") ||
             dv_eq(was, "Statistics") || dv_eq(was, "Administrators") || dv_eq(was, "History")) && g_sel >= 0 && g_sel < g_nst) {
            push_hub();
            screen_open("Station", g_st[g_sel].nick[0] ? g_st[g_sel].nick : g_st[g_sel].call);
        } else if (dv_eq(was, "Firmware") && g_dev[0]) {
            push_device();
            screen_open("Flash", flash_title());
        }
        return;
    }
    if (dv_eq(cmd, "devices_tap")) {
        char id[70];
        if (!fields("devices_id", id, sizeof id)) return;
        if (dv_eq(id, "_cable")) {
            /* The help row: look at the cable again. */
            sub(&g_sub_flash, 1, "core.flash");
            if (!hal_flash_scan()) log_line("", "No USB here");
            flash_read();
            push_flash_all();
            return;
        }
        flash_read();
        if (dev_of(id)) { sub(&g_sub_flash, 1, "core.flash"); open_usb(id); return; }
        g_sel = find(id);
        if (g_sel < 0) return;
        st_t *st = &g_st[g_sel];
        /* Opening a station asks it who owns it (q:policy, anybody may):
         * a station claimed by somebody else since it last asked to be
         * claimed stops asking, and silence is not a fact the list can
         * read. One request, answered by an observation. */
        if (!g_me[0]) who_am_i();
        if (host_facts(st) && !st->ask_pol && !(st->pend_id[0] && !st->pend_final)) do_ask(st);
        /* One that is ours is also asked how it is doing: the answer is what
         * tells a board from a phone, and so what this page offers. Not the
         * device in the person's hand, which has nothing to learn by asking. */
        if (st->mine && !is_this_device(st) && (!st->pend_id[0] || st->pend_final)) {
            if (st->pend_id[0]) done_pending(st);
            do_cmd(st, "cmd:zdiag", "zdiag");
        }
        push_hub();
        screen_open("Station", st->nick[0] ? st->nick : id);
        return;
    }
    if (dv_eq(cmd, "back")) { say("{\"type\":\"ui.screen.close\"}"); return; }
    if (dv_eq(cmd, "log_clear")) { say("{\"type\":\"ui.log.clear\",\"field\":\"log\"}"); return; }
    if (dv_starts(cmd, "flash_") || dv_starts(cmd, "fl_") || dv_eq(cmd, "vanity_picked")) {
        on_flash_command(cmd);
        return;
    }

    st_t *s = selected();
    if (!s) return;
    if (!g_me[0]) who_am_i();

    /* Opening a task's screen sends nothing. */
    if (dv_eq(cmd, "open_wifi")) {
        one_line("wifi_now", "Now", s->now);
        screen_open("WiFi", s->call);
        return;
    }
    if (dv_eq(cmd, "open_name")) {
        field_set("nick", s->nick);
        field_set("zone", s->zone[0] ? s->zone : "auto");
        field_set("hotspot", "same");
        field_set("lora", "same");
        /* A station reports lora: only when it has a radio (11.10). */
        flag_hidden("lora", !s->lora[0]);
        screen_open("Settings", s->call);
        return;
    }
    if (dv_eq(cmd, "open_admins")) {
        /* Who may administer it, as the station last said (11.9). Asked
         * again as the screen opens: another owner may have changed it. */
        field_set("owners", dv_eq(s->pol_owner, "none") ? "" : s->pol_owner);
        field_set("backups", "same");
        one_line("admins_now", "Now", s->pol_owner[0] && !dv_eq(s->pol_owner, "none")
                 ? "Each callsign above may change this station." : "Asking the station who administers it...");
        if (!s->pend_id[0] || s->pend_final) {
            if (s->pend_id[0]) done_pending(s);
            do_ask(s);
        }
        screen_open("Administrators", s->nick[0] ? s->nick : s->call);
        return;
    }
    if (dv_eq(cmd, "admins_apply")) {
        char raw[80] = "", list[48] = "", how[8] = "", f[140] = "";
        fields("owners", raw, sizeof raw);
        fields("backups", how, sizeof how);
        /* Callsigns, however they were typed: upper case, commas between. */
        int n = 0, me_in = 0;
        for (const char *p = raw; *p; ) {
            char c[12];
            unsigned k = 0;
            while (*p == ' ' || *p == ',' || *p == ';') p++;
            while (*p && *p != ' ' && *p != ',' && *p != ';') {
                if (k < sizeof c - 1) c[k++] = dv_up(*p);
                p++;
            }
            c[k] = 0;
            if (!k) continue;
            if (c[0] != 'X' || k < 4) { log_line(s->call, "That is not a callsign"); return; }
            if (++n > 4) { log_line(s->call, "A station has at most four administrators"); return; }
            if (dv_eq(c, g_me)) me_in = 1;
            if (list[0]) dv_cat(list, ",", sizeof list);
            dv_cat(list, c, sizeof list);
        }
        if (!s->mine) { log_line(s->call, "Claim it first"); return; }
        /* 11.9: a list that no longer names the signer is a transfer. Giving
         * a station away is done on purpose, not by a typing slip. */
        if (n && !me_in) {
            log_line(s->call, "Keep your own callsign in the list, or you hand the station over");
            set_now(s, "Keep your own callsign in the list");
            return;
        }
        if (n && !dv_eq(list, s->pol_owner)) { dv_cat(f, "owner:", sizeof f); dv_cat(f, list, sizeof f); }
        if (dv_eq(how, "on") || dv_eq(how, "off")) {
            /* serve: is the whole list (11.9), so the one word is added to
             * or taken from what the station said it serves. */
            char serve[48] = "";
            int has = 0;
            for (const char *p = s->pol_serve; *p; ) {
                char w[16];
                unsigned k = 0;
                while (*p == ',') p++;
                while (*p && *p != ',') { if (k < sizeof w - 1) w[k++] = *p; p++; }
                w[k] = 0;
                if (!k || dv_eq(w, "none")) continue;
                if (dv_eq(w, "archive")) { has = 1; if (dv_eq(how, "off")) continue; }
                if (serve[0]) dv_cat(serve, ",", sizeof serve);
                dv_cat(serve, w, sizeof serve);
            }
            if (dv_eq(how, "on") && !has) {
                if (serve[0]) dv_cat(serve, ",", sizeof serve);
                dv_cat(serve, "archive", sizeof serve);
            }
            if (has != dv_eq(how, "on")) {
                if (f[0]) dv_cat(f, " ", sizeof f);
                dv_cat(f, "serve:", sizeof f);
                dv_cat(f, serve[0] ? serve : "none", sizeof f);
            }
        }
        if (!f[0]) log_line(s->call, "Nothing to change");
        else do_set(s, f, "set");
        return;
    }
    if (dv_eq(cmd, "open_identity")) {
        field_set("identity", "new");
        field_set("nsec", "");
        /* The issued callsign it transmits under, when it has one. */
        int issued = !self_generated(s->call);
        field_set("call", issued ? s->call : "");
        flag_hidden("call_derived", !issued);
        screen_open("Callsign", s->call);
        return;
    }
    if (dv_eq(cmd, "open_stats")) {
        /* Opening the screen is the ask: the policy and the mail count
         * from anybody's station, the diagnostics from one that is ours. */
        if (!s->pend_id[0] || s->pend_final) {
            if (s->pend_id[0]) done_pending(s);
            do_ask(s);
            if (s->mine) do_cmd(s, "cmd:zdiag", "zdiag");
            s->asking = 1;
        }
        push_stats();
        screen_open("Statistics", s->call);
        return;
    }
    if (dv_eq(cmd, "open_answers")) { screen_open("History", s->call); return; }
    if (dv_eq(cmd, "forget")) {
        char was[12];
        dv_cpy(was, s->call, sizeof was);
        char k[24] = "st.";
        dv_cat(k, was, sizeof k);
        kv_put(k, "");
        drop(g_sel);
        g_sel = -1;
        save();
        push_list();
        say("{\"type\":\"ui.screen.close\"}");
        log_line(was, "Forgotten");
        return;
    }

    /* One command at a time. Once the station has taken one (a 202) it is
     * working on its own, and a new one may go. */
    if (s->pend_id[0] && !s->pend_final) {
        /* Reading how it is doing never holds a person's own command back:
         * the ask is given up, and made again the next time the page opens. */
        if (dv_eq(s->pend_what, "zdiag")) done_pending(s);
        else { log_line(s->call, "Still waiting for its last answer"); return; }
    }
    if (s->pend_id[0]) done_pending(s);

    if (dv_eq(cmd, "pick_callsign")) {
        if (!s->mine) { log_line(s->call, "Claim it first"); return; }
        say("{\"type\":\"ui.callsign.pick\",\"field\":\"rekey\",\"prefix\":\"X3\",\"length\":4}");
        return;
    }
    if (dv_eq(cmd, "claim")) {
        do_claim(s);
    } else if (dv_eq(cmd, "update")) {
        /* 11.8: install the newest published version. The station checks
         * the image's signature itself; a 202 now, a 200 or a 500 after it
         * restarted. */
        char fw[24];
        fw_of(s, host_facts(s), fw, sizeof fw);
        if (!s->mine) log_line(s->call, "Only its owner can update it");
        else if (!g_latest[0]) log_line(s->call, "No published firmware known yet: look again in a moment");
        else if (!behind(fw)) log_line(s->call, "It already runs the newest firmware");
        else {
            char c[40] = "cmd:update ver:";
            dv_cat(c, g_latest, sizeof c);
            do_cmd(s, c, "update");
        }
    } else if (dv_eq(cmd, "rekey_picked")) {
        char c[16] = "", nsec[72] = "";
        fields("rekey_call", c, sizeof c);
        fields("rekey_nsec", nsec, sizeof nsec);
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (!dv_starts(nsec, "nsec1") || dv_len(nsec) != 63) log_line(s->call, "That is not a key");
        else {
            char body[96], line[64] = "Giving it the callsign ";
            dv_cat(line, c, sizeof line);
            const char *kv[] = { "nsec", nsec, 0 };
            if (dv_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "key") == 0) {
                log_line(s->call, line);
                set_now(s, line);
            }
        }
        for (unsigned i = 0; nsec[i]; i++) nsec[i] = 0;
    } else if (dv_eq(cmd, "wifi_apply")) {
        char ssid[40] = "", pass[72] = "";
        fields("ssid", ssid, sizeof ssid);
        fields("wifi_pass", pass, sizeof pass);
        if (!s->npub[0]) key_from_core(s, host_facts(s));
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (!ssid[0]) log_line(s->call, "Which network?");
        else if (!s->npub[0])
            log_line(s->call, "Its key has not been heard yet: a station airs it every 30 minutes");
        else if (pass[0] && (dv_len(pass) < 8 || dv_len(pass) > 63))
            log_line(s->call, "A WiFi password is 8 to 63 characters");
        else if (!pass[0]) {
            /* An open network: the name, sealed, then join. */
            char body[64];
            const char *kv[] = { "ssid", ssid, "wifi", "join", 0 };
            if (dv_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "wifi") == 0)
                set_now(s, "Sending an open network...");
        } else do_wifi(s, ssid, pass);
        for (unsigned i = 0; pass[i]; i++) pass[i] = 0;
        field_set("wifi_pass", "");
    } else if (dv_eq(cmd, "wifi_off")) {
        if (!s->mine) log_line(s->call, "Claim it first");
        else do_set(s, "wifi:off", "wifi");
    } else if (dv_eq(cmd, "station_apply")) {
        char nick[24] = "", zone[12] = "", ap[8] = "", lora[16] = "";
        char freq[20] = "", f[160] = "";
        fields("nick", nick, sizeof nick);
        fields("zone", zone, sizeof zone);
        fields("hotspot", ap, sizeof ap);
        fields("lora", lora, sizeof lora);
        fields("freq", freq, sizeof freq);
        if (nick[0]) { dv_cat(f, "nick:", sizeof f); dv_cat(f, nick, sizeof f); }
        if (zone[0] && !dv_eq(zone, s->zone[0] ? s->zone : "auto")) {
            if (f[0]) dv_cat(f, " ", sizeof f);
            dv_cat(f, "zone:", sizeof f); dv_cat(f, zone, sizeof f);
        }
        if (dv_eq(ap, "on") || dv_eq(ap, "off")) {
            if (f[0]) dv_cat(f, " ", sizeof f);
            dv_cat(f, "ap:", sizeof f); dv_cat(f, ap, sizeof f);
        }
        /* 14.8: the LoRa mode, taken at once (the station retunes and
         * answers 200; it does not restart). Only a change is sent. */
        if ((dv_eq(lora, "xprs") || dv_eq(lora, "meshtastic") ||
             dv_eq(lora, "meshcore")) && !dv_eq(lora, s->lora)) {
            if (f[0]) dv_cat(f, " ", sizeof f);
            dv_cat(f, "lora:", sizeof f); dv_cat(f, lora, sizeof f);
        }
        /* 14.8: the channel. Not every board is an 868 MHz board, and a
         * network's presets differ by region, so the owner may name a
         * frequency (433.900MHz, or hertz) or `preset` to take the
         * region's own back. The station answers with where it landed. */
        if (freq[0] && !dv_eq(freq, s->freq)) {
            if (f[0]) dv_cat(f, " ", sizeof f);
            dv_cat(f, "freq:", sizeof f); dv_cat(f, freq, sizeof f);
        }
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (!f[0]) log_line(s->call, "Nothing to change");
        else do_set(s, f, "set");
    } else if (dv_eq(cmd, "identity_apply")) {
        char how[12] = "", nsec[72] = "";
        fields("identity", how, sizeof how);
        fields("nsec", nsec, sizeof nsec);
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (dv_eq(how, "import")) {
            if (!dv_starts(nsec, "nsec1") || dv_len(nsec) != 63) {
                log_line(s->call, "That is not an nsec");
            } else {
                char body[96];
                const char *kv[] = { "nsec", nsec, 0 };
                if (dv_body(body, sizeof body, kv) > 0 && send_sealed(s, body, "key") == 0)
                    set_now(s, "Sending the key, sealed...");
            }
        } else do_set(s, "key:new", "key");
        for (unsigned i = 0; nsec[i]; i++) nsec[i] = 0;
        field_set("nsec", "");
    } else if (dv_eq(cmd, "call_apply") || dv_eq(cmd, "call_derived")) {
        /* 6.4.2: the callsign on the owner's licence, or back to the one
         * the key derives. The station checks it the way the app's profile
         * screen does and says why when it will not take it; what is
         * refused here is only what is plainly not one. */
        char raw[24] = "", c[16] = "";
        if (dv_eq(cmd, "call_apply")) fields("call", raw, sizeof raw);
        unsigned n = 0;
        for (const char *p = raw; *p && n + 1 < sizeof c; p++) {
            if (*p == ' ') continue;
            c[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
        }
        c[n] = 0;
        if (!s->mine) log_line(s->call, "Claim it first");
        else if (dv_eq(cmd, "call_apply") && !c[0]) log_line(s->call, "Which callsign?");
        else if (n > 11) log_line(s->call, "At most 11 characters on a station");
        else if (c[0] && self_generated(c))
            log_line(s->call, "X1 to X5 come from a key. Enter the callsign on your licence.");
        else if (dv_eq(c, s->call)) log_line(s->call, "It already transmits as that");
        else {
            char f[32] = "call:";
            dv_cat(f, c[0] ? c : "derived", sizeof f);
            dv_cpy(s->want_call, c, sizeof s->want_call);
            do_set(s, f, "call");
        }
    } else if (dv_eq(cmd, "stats")) {
        do_ask(s);
        s->asking = 1;
        if (s->mine) do_cmd(s, "cmd:zdiag", "zdiag");
        else set_now(s, "Asking what it does for others...");
        push_stats();
    } else if (dv_eq(cmd, "crash")) {
        if (!s->mine) log_line(s->call, "Only its owner may read that");
        else do_cmd(s, "cmd:zcore", "zcore");
    }
}

/* ── Entry points ─────────────────────────────────────────────────────── */
/* A page in front of a person. The host remembers switches between visits:
 * both start off again, so Erase never carries over to the next board and
 * Advanced options are folded away until asked for. */
static void fresh_visit(void)
{
    g_adv = 0;
    g_wipe = 0;
    say("{\"type\":\"ui.field.set\",\"field\":\"wipe\",\"value\":false}");
    say("{\"type\":\"ui.field.set\",\"field\":\"fl_adv\",\"value\":false}");
}

void st_init(void)
{
    /* The one thing an idle phone listens for: a station asking to be
     * claimed. Answers and identities are listened for while asked (listen). */
    dv_sub(DV_ST, "xprs.request", 1);
    who_am_i();
    load();
    this_device();
    owned_from_core();
    push_list();
    /* A page in front of a person: look at the cable now (sysfs, or the
     * USB host list) and at the catalogue when it is stale. The background
     * engine on an idle phone only reads what the core already knows. */
    if (hal_ui_attached()) {
        sub(&g_sub_flash, 1, "core.flash");
        hal_flash_scan();
        fresh_visit();
        /* And keep the published firmwares here while it is open. */
        g_prefetch = 1;
    }
    flash_read();
    if (prefetch()) flash_read();
    push_flash_all();
}
