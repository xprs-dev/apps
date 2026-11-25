/*
 * mesh/browse.c: the stations as a list, and what each one offers.
 *
 * The graph (main.c) is a picture of the room; it stops reading at a few
 * dozen nodes. This is the list that scales: the stations by scope (local,
 * regional, global: the class of the nearest bearer each is heard on,
 * decided by the core), a page at a time, and for one station how a message
 * gets to it and what it offers through its visitor API: the rooms,
 * galleries, music and libraries its web page shows, the private ones this
 * profile belongs to included.
 *
 * Everything is read with hal_station_read. This file asks WHAT; how a
 * station is reached, which stations are asked how they hear another and
 * who may see a room are the core's and the station's. The hop-by-hop ways
 * are drawn, never decided from.
 */
#include "../hal/xprs_wasm_hal.h"
#include "util.h"
#include "browse.h"
#include "wapp_stations.h"

static char g_out[65536];   /* one outbound message          */
static char g_in[49152];    /* one hal_station_read answer   */
static char g_obj[2048];    /* one row of an answer          */
static char g_req[256];

static void say(const char *m) { hal_msg_send(m, al_len(m)); }

static int sread(void)
{
    int n = hal_station_read(g_req, al_len(g_req), g_in, sizeof g_in - 1);
    if (n <= 0) { g_in[0] = 0; return 0; }
    g_in[n] = 0;
    return n;
}

/* What is on screen, so a `core.station` redraws the right thing. */
enum { V_LIST, V_STATION, V_ROOM, V_FOLDER };
static int  g_view = V_LIST;
static char g_call[16];     /* the station opened            */
static char g_room[16];     /* its room opened               */
static char g_room_kind[12];
static char g_folder[16];   /* that room's folder opened     */
static char g_folder_name[64];
static int  g_folder_off;
static int  g_list_dirty = 1;


static unsigned num(const char *json, const char *key)
{
    char v[16];
    unsigned n = 0;
    if (!al_json(json, key, v, sizeof v)) return 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (unsigned)(*p - '0');
    return n;
}

static void cat_dist(char *d, unsigned m, unsigned cap)
{
    if (m < 1000) { al_cat_u(d, m, cap); al_cat(d, " m", cap); }
    else if (m < 10000) {
        al_cat_u(d, m / 1000, cap); al_cat(d, ".", cap);
        al_cat_u(d, (m % 1000) / 100, cap); al_cat(d, " km", cap);
    } else { al_cat_u(d, m / 1000, cap); al_cat(d, " km", cap); }
}

/* A bearer as people call it. */
static const char *bearer_name(const char *b)
{
    if (al_eq(b, "ble") || al_eq(b, "ble5")) return "Bluetooth";
    if (al_eq(b, "lan")) return "LAN";
    if (al_eq(b, "wifi")) return "WiFi";
    if (al_eq(b, "espnow")) return "ESP-NOW";
    if (al_eq(b, "lora")) return "LoRa";
    if (al_eq(b, "rns") || al_eq(b, "internet")) return "Reticulum";
    if (al_eq(b, "vhf")) return "VHF";
    if (al_eq(b, "uhf")) return "UHF";
    if (al_eq(b, "hf")) return "HF";
    return b;
}

/* The row's "bearers":["ble","rns"] as "Bluetooth, Reticulum". */
static void cat_bearers(char *d, const char *row, unsigned cap)
{
    const char *p = row;
    static const char pat[] = "\"bearers\":[";
    while (*p && !al_starts(p, pat)) p++;
    if (!*p) return;
    p += sizeof pat - 1;
    int any = 0;
    while (*p && *p != ']') {
        if (*p != '"') { p++; continue; }
        char b[12];
        unsigned o = 0;
        p++;
        while (*p && *p != '"' && o < sizeof b - 1) b[o++] = *p++;
        b[o] = 0;
        if (*p == '"') p++;
        if (any) al_cat(d, ", ", cap);
        al_cat(d, bearer_name(b), cap);
        any = 1;
    }
}

static const char *kind_name(const char *k)
{
    if (al_eq(k, "user")) return "Person";
    if (al_eq(k, "station")) return "Station";
    if (al_eq(k, "device")) return "Device";
    return "XPRS";
}

/* ── The list and the map ─────────────────────────────────────────────── */
/* Every station known, the way the xprs.dev portal's map page shows them
 * (portal/page/chat_page.html: mapDraw, mapList, mapChips): on the map those
 * that say where they are, in the drawer under it the whole list, freshest
 * first, 50 at a time, with a search box and chips to filter by what a
 * station serves, runs on and how it is heard. The table is the SDK's
 * (sdk/wapp_stations.c, shared with Check-in): identities and service
 * announcements from the archive, and the stations this device hears now,
 * which hal_station_read lists with their bearers. */
static void read_host(void);
static char g_me[16];
static int g_serving;
static wst_filter_t g_f;
static int g_shown = 50;
static int g_map_dirty, g_fitted;
static int g_page;                   /* the page said ready: there is a map to draw */
static unsigned long long g_heard_at, g_names_q;
static char g_rows[49152];

/* What hal_station_read says of the stations heard now, kept for the list's
 * lines (how each is reached, how far, whether it can be browsed). */
#define HEARD_MAX 256
static struct { char call[12]; char how[48]; unsigned dist; char api; char kind[10]; } g_heard[HEARD_MAX];
static int g_nheard;

static int heard_find(const char *call)
{
    for (int i = 0; i < g_nheard; i++) if (al_eq(g_heard[i].call, call)) return i;
    return -1;
}

/* The heard list into the table, a page at a time, at most every 20 s (or
 * at once when the core said the room moved and 5 s have passed). */
static void heard_sync(int moved)
{
    unsigned long long now = hal_time_epoch();
    if (g_heard_at && now < g_heard_at + (moved ? 5 : 20)) return;
    g_heard_at = now;
    g_nheard = 0;
    for (int off = 0; off < 600; off += 60) {
        al_cpy(g_req, "{\"get\":\"list\",\"scope\":\"all\",\"offset\":", sizeof g_req);
        al_cat_u(g_req, (unsigned)off, sizeof g_req);
        al_cat(g_req, "}", sizeof g_req);
        if (!sread()) break;
        const char *p = al_json_arr(g_in, "rows");
        int rows = 0;
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char call[16], reach[12], api[8], base[12];
            rows++;
            if (!al_json(g_obj, "call", call, sizeof call) || !call[0]) continue;
            al_json(g_obj, "reach", reach, sizeof reach);
            wst_base_call(base, sizeof base, call);
            /* A word every minute is enough: the table is not rewritten for
             * every few seconds of a station's age. */
            unsigned age = num(g_obj, "age_s");
            wst_heard(base, reach, (now > age ? now - age : now) / 60 * 60);
            if (g_nheard < HEARD_MAX) {
                int i = g_nheard++;
                al_cpy(g_heard[i].call, base, sizeof g_heard[i].call);
                g_heard[i].how[0] = 0;
                cat_bearers(g_heard[i].how, g_obj, sizeof g_heard[i].how);
                g_heard[i].dist = 0;
                char dm[16];
                if (al_json(g_obj, "dist_m", dm, sizeof dm) && dm[0]) g_heard[i].dist = num(g_obj, "dist_m") + 1;
                g_heard[i].api = al_json(g_obj, "api", api, sizeof api) && al_eq(api, "true");
                al_json(g_obj, "kind", g_heard[i].kind, sizeof g_heard[i].kind);
            }
        }
        if (rows < 60) break;
    }
}

/* The chips over the list: how stations are heard, what they serve and run
 * on, which say where they are and which can be messaged; and the square a
 * marker's "Show all here" chose. */
static void push_chips(void)
{
    static char chips[6144];
    wst_chips(&g_f, chips, sizeof chips);
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"st_tags\",\"value\":", sizeof g_out);
    if (g_f.area[0]) {
        al_cat(g_out, "[{\"id\":\"area\",\"label\":\"This area \xc3\x97\",\"on\":true}", sizeof g_out);
        if (chips[1] != ']') { al_cat(g_out, ",", sizeof g_out); al_cat(g_out, chips + 1, sizeof g_out); }
        else al_cat(g_out, "]", sizeof g_out);
    } else al_cat(g_out, chips, sizeof g_out);
    al_cat(g_out, "}", sizeof g_out);
    say(g_out);
}

static void push_list(void)
{
    g_list_dirty = 0;
    read_host();
    unsigned long long now = hal_time_epoch();
    int total = wst_total(), hits = wst_count(&g_f);
    int filtered = g_f.q[0] || g_f.area[0] || g_f.ntags;
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":[{\"title\":\"", sizeof g_out);
    if (filtered) {
        al_cat_u(g_out, (unsigned)hits, sizeof g_out);
        al_cat(g_out, " of ", sizeof g_out);
    }
    al_cat_u(g_out, (unsigned)total, sizeof g_out);
    al_cat(g_out, total == 1 && !filtered ? " station" : " stations", sizeof g_out);
    al_cat(g_out, "\",\"items\":[", sizeof g_out);
    int shown = 0;
    /* The phone in the hand comes first: it is a station too. */
    if (!filtered && g_me[0]) {
        shown++;
        al_cat(g_out, "{\"id\":\"", sizeof g_out);
        al_cat(g_out, g_me, sizeof g_out);
        al_cat(g_out, "\",\"icon\":\"smartphone\",\"title\":\"This phone\",\"subtitle\":\"", sizeof g_out);
        al_cat(g_out, g_me, sizeof g_out);
        al_cat(g_out, g_serving ? " \\u00b7 others can browse what you share"
                                : " \\u00b7 what you share, and with whom", sizeof g_out);
        al_cat(g_out, "\",\"tags\":[\"", sizeof g_out);
        al_cat(g_out, g_serving ? "Sharing" : "Private", sizeof g_out);
        al_cat(g_out, "\"]}", sizeof g_out);
    }
    wst_list(&g_f, 0, g_shown, g_rows, sizeof g_rows);
    const char *p = g_rows;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    int listed = 0;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char call[16], nick[48], pos[40], key[80], sub[200] = "", tags[120], a[24];
        if (!al_json(g_obj, "call", call, sizeof call) || !call[0]) continue;
        /* Room for this row and the closing of the message, or stop. */
        if (al_len(g_out) > sizeof g_out - 900) break;
        al_json(g_obj, "nick", nick, sizeof nick);
        al_json(g_obj, "pos", pos, sizeof pos);
        al_json(g_obj, "key", key, sizeof key);
        unsigned long long last = 0;
        {
            char v[24];
            if (al_json(g_obj, "last", v, sizeof v)) for (const char *q = v; *q >= '0' && *q <= '9'; q++) last = last * 10 + (unsigned)(*q - '0');
        }
        int h = heard_find(call);
        /* Its name, then what it is and how it is reached when heard here,
         * then what it serves and runs on. */
        if (nick[0]) al_cpy(sub, nick, sizeof sub);
        if (h >= 0) {
            if (sub[0]) al_cat(sub, " \xc2\xb7 ", sizeof sub);
            al_cat(sub, kind_name(g_heard[h].kind), sizeof sub);
            if (g_heard[h].how[0]) { al_cat(sub, " \xc2\xb7 ", sizeof sub); al_cat(sub, g_heard[h].how, sizeof sub); }
            if (g_heard[h].dist) { al_cat(sub, " \xc2\xb7 ", sizeof sub); cat_dist(sub, g_heard[h].dist - 1, sizeof sub); }
        }
        wst_row_tags(g_obj, tags, sizeof tags);
        if (tags[0]) { if (sub[0]) al_cat(sub, " \xc2\xb7 ", sizeof sub); al_cat(sub, tags, sizeof sub); }
        if (shown++) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "{\"id\":\"", sizeof g_out);
        al_cat(g_out, call, sizeof g_out);
        al_cat(g_out, "\",\"call\":\"", sizeof g_out);
        al_cat(g_out, call, sizeof g_out);
        al_cat(g_out, "\",\"title\":\"", sizeof g_out);
        al_cat(g_out, call, sizeof g_out);
        al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
        al_jesc(g_out, sub, sizeof g_out);
        al_cat(g_out, "\",\"tags\":[\"", sizeof g_out);
        wst_ago(a, sizeof a, last, now);
        al_cat(g_out, last ? a : "heard of", sizeof g_out);
        al_cat(g_out, "\"", sizeof g_out);
        if (pos[0]) al_cat(g_out, ",\"On the map\"", sizeof g_out);
        if (h >= 0 && g_heard[h].api) al_cat(g_out, ",\"Browse\"", sizeof g_out);
        al_cat(g_out, "]", sizeof g_out);
        if (key[0]) al_cat(g_out, ",\"action\":\"st_msg\",\"actionLabel\":\"Message\"", sizeof g_out);
        al_cat(g_out, "}", sizeof g_out);
        listed++;
    }
    if (hits > listed) {
        if (shown) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "{\"id\":\"more\",\"icon\":\"expand_more\",\"title\":\"Show more\",\"subtitle\":\"", sizeof g_out);
        al_cat_u(g_out, (unsigned)(hits - listed), sizeof g_out);
        al_cat(g_out, " more\"}", sizeof g_out);
        shown++;
    }
    if (!listed && g_f.q[0]) {
        if (shown) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "{\"id\":\"none\",\"icon\":\"travel_explore\",\"title\":\"No station matches\",\"subtitle\":\"", sizeof g_out);
        al_cat(g_out, g_names_q ? "Asking the archivers\xe2\x80\xa6" : "Enter asks the archivers", sizeof g_out);
        al_cat(g_out, "\"}", sizeof g_out);
    }
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
    push_chips();
}

/* The map: the markers anew (the SDK's), and the first time where this
 * phone says it is, else the whole world. */
static void push_map(void)
{
    g_map_dirty = 0;
    say("{\"type\":\"ui.map.markers.clear\"}");
    wst_draw();
    wst_status();
    if (g_fitted) return;
    g_fitted = 1;
    char p[96], pos[48];
    int n = hal_xprs_position(p, sizeof p - 1);
    if (n > 0) {
        p[n] = 0;
        if (al_json(p, "pos", pos, sizeof pos) && wst_pos_ok(pos)) {
            char m[160] = "{\"type\":\"ui.map.viewport\",\"lat\":";
            unsigned i = 0;
            char lat[24];
            while (pos[i] && pos[i] != ',' && i + 1 < sizeof lat) { lat[i] = pos[i]; i++; }
            lat[i] = 0;
            al_cat(m, lat, sizeof m);
            al_cat(m, ",\"lon\":", sizeof m);
            al_cat(m, pos + i + 1, sizeof m);
            al_cat(m, ",\"zoom\":8}", sizeof m);
            say(m);
        }
    }
}

/* A walk of the archive finished: redraw what it changed. */
static void walked(int changed)
{
    if (!g_page) return;             /* drawn from the table when it opens */
    if (!changed) { wst_status(); return; }
    g_map_dirty = 1;
    g_list_dirty = 1;
}

/* Show one station or square on the map: the drawer folds (the host does)
 * and the map goes there, closer for a point than for a square. */
static void locate(const char *pos, long half)
{
    char m[160] = "{\"type\":\"ui.map.locate\",\"lat\":";
    unsigned i = 0;
    char lat[24];
    while (pos[i] && pos[i] != ',' && i + 1 < sizeof lat) { lat[i] = pos[i]; i++; }
    lat[i] = 0;
    if (pos[i] != ',') return;
    al_cat(m, lat, sizeof m);
    al_cat(m, ",\"lon\":", sizeof m);
    al_cat(m, pos + i + 1, sizeof m);
    al_cat(m, ",\"zoom\":", sizeof m);
    /* A square a degree wide fits at zoom 7, a tenth at 10; a point, 14. */
    al_cat_u(m, !half ? 14 : half > 30000 ? 7 : half > 3000 ? 10 : 12, sizeof m);
    al_cat(m, "}", sizeof m);
    say(m);
}

static void message(const char *call)
{
    char m[96] = "{\"type\":\"mesh.message\",\"callsign\":\"";
    al_cat(m, call, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* What a search finds nowhere here is asked of the archivers by name
 * (hal_xprs_names, 12.9.4); core.names says when they answered. */
static void ask_names(int ask)
{
    char q[120] = "{\"q\":\"", out[49152 / 4];
    al_jesc(q, g_f.q, sizeof q);
    al_cat(q, "\",\"limit\":40,\"ask\":", sizeof q);
    al_cat(q, ask ? "true}" : "false}", sizeof q);
    int n = hal_xprs_names(q, al_len(q), out, sizeof out - 1);
    if (n <= 0) return;
    out[n] = 0;
    const char *p = out;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char call[16], nick[48];
        if (!al_json(g_obj, "call", call, sizeof call)) continue;
        al_json(g_obj, "nick", nick, sizeof nick);
        wst_named(call, nick, num(g_obj, "ts"));
    }
}

/* ── This phone as a station ──────────────────────────────────────────── */
static int g_share_busy, g_was_busy;
static unsigned g_share_done, g_share_total;

static void read_host(void)
{
    char v[8];
    al_cpy(g_req, "{\"get\":\"host\"}", sizeof g_req);
    if (!sread()) return;
    al_json(g_in, "call", g_me, sizeof g_me);
    g_serving = al_json(g_in, "serving", v, sizeof v) && al_eq(v, "true");
    g_share_busy = al_json(g_in, "busy", v, sizeof v) && al_eq(v, "true");
    g_share_done = num(g_in, "done");
    g_share_total = num(g_in, "total");
}

static int is_me(void) { return g_me[0] && al_eq(g_call, g_me); }

/* ── One station ──────────────────────────────────────────────────────── */
static void req_call(const char *get)
{
    al_cpy(g_req, "{\"call\":\"", sizeof g_req);
    al_cat(g_req, g_call, sizeof g_req);
    al_cat(g_req, "\",\"get\":\"", sizeof g_req);
    al_cat(g_req, get, sizeof g_req);
    al_cat(g_req, "\"", sizeof g_req);
}

static const char *room_icon(const char *kind)
{
    if (al_eq(kind, "gallery")) return "photo_library";
    if (al_eq(kind, "music")) return "library_music";
    if (al_eq(kind, "library")) return "folder";
    if (al_eq(kind, "forum")) return "forum";
    return "chat";
}

static const char *room_what(const char *kind)
{
    if (al_eq(kind, "gallery")) return "Gallery";
    if (al_eq(kind, "music")) return "Music";
    if (al_eq(kind, "library")) return "Files";
    if (al_eq(kind, "forum")) return "Forum";
    if (al_eq(kind, "mesh")) return "Mesh room";
    return "Chat room";
}

static int is_collection(const char *kind)
{
    return al_eq(kind, "gallery") || al_eq(kind, "music") || al_eq(kind, "library");
}

static void one_row(const char *id, const char *icon, const char *title, const char *sub)
{
    al_cat(g_out, "{\"id\":\"", sizeof g_out);
    al_cat(g_out, id, sizeof g_out);
    al_cat(g_out, "\",\"icon\":\"", sizeof g_out);
    al_cat(g_out, icon, sizeof g_out);
    al_cat(g_out, "\",\"title\":\"", sizeof g_out);
    al_jesc(g_out, title, sizeof g_out);
    al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
    al_jesc(g_out, sub, sizeof g_out);
    al_cat(g_out, "\"}", sizeof g_out);
}

/* Why a visit gave nothing, as a person reads it. */
static const char *why_text(const char *why)
{
    if (al_eq(why, "unknown")) return "This station does not offer anything to browse.";
    if (al_eq(why, "unreached")) return "It did not answer. It may be out of reach right now.";
    if (al_eq(why, "refused")) return "It did not let you in.";
    return "It could not be verified, so nothing is shown.";
}

static void push_station(void)
{
    read_host();
    int me = is_me();
    /* A share that just ended changed what this phone offers: read it anew. */
    if (me && g_was_busy && !g_share_busy) {
        req_call("forget");
        al_cat(g_req, "}", sizeof g_req);
        sread();
    }
    g_was_busy = g_share_busy;
    unsigned sh_done = g_share_done, sh_total = g_share_total;
    /* How a message gets there: the core's diagnostic, drawn as it is. */
    req_call("ways");
    al_cat(g_req, "}", sizeof g_req);
    if (me) say("{\"type\":\"ui.field.set\",\"field\":\"s_ways\",\"value\":{\"routes\":[],\"here\":true}}");
    else if (sread()) {
        al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"s_ways\",\"value\":", sizeof g_out);
        al_cat(g_out, g_in, sizeof g_out);
        al_cat(g_out, "}", sizeof g_out);
        say(g_out);
    }

    req_call("offers");
    al_cat(g_req, "}", sizeof g_req);
    sread();
    char state[12] = "", why[24] = "", known[8] = "";
    al_json(g_in, "state", state, sizeof state);
    al_json(g_in, "why", why, sizeof why);
    al_json(g_in, "known", known, sizeof known);

    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"s_offers\",\"sections\":[", sizeof g_out);
    if (me) {
        al_cat(g_out, "{\"title\":\"This phone\",\"items\":[", sizeof g_out);
        one_row("host:toggle", g_serving ? "toggle_on" : "toggle_off",
                g_serving ? "Others can browse this phone" : "Nobody can browse this phone",
                g_serving ? "Public rooms for everybody, private ones for their members. Tap to stop."
                          : "Tap to let others see what you share below.");
        al_cat(g_out, ",", sizeof g_out);
        if (g_share_busy) {
            char sub[64] = "";
            al_cat_u(sub, sh_done, sizeof sub);
            al_cat(sub, " of ", sizeof sub);
            al_cat_u(sub, sh_total, sizeof sub);
            one_row("host:busy", "hourglass_empty", "Adding files\xe2\x80\xa6", sub);
        } else {
            one_row("host:gallery", "add_photo_alternate", "Share photos", "Pictures and films from this phone");
            al_cat(g_out, ",", sizeof g_out);
            one_row("host:music", "library_add", "Share music", "Tracks from this phone");
            al_cat(g_out, ",", sizeof g_out);
            one_row("host:library", "note_add", "Share files", "Any files from this phone");
        }
    } else {
        al_cat(g_out, "{\"title\":\"This station\",\"items\":[", sizeof g_out);
        /* What the map knows of it: a key to message it, a place to show. */
        static char row[1024];
        char key[80] = "", pos[40] = "", tags[120] = "";
        if (wst_row(g_call, row, sizeof row)) {
            al_json(row, "key", key, sizeof key);
            al_json(row, "pos", pos, sizeof pos);
            wst_row_tags(row, tags, sizeof tags);
        }
        if (key[0]) {
            one_row("msg", "chat", "Message", "A private conversation in Chat");
            al_cat(g_out, ",", sizeof g_out);
        }
        if (pos[0]) {
            char sub[160] = "Where it says it is";
            if (tags[0]) { al_cat(sub, " \xc2\xb7 ", sizeof sub); al_cat(sub, tags, sizeof sub); }
            one_row("map", "map", "Show on map", sub);
            al_cat(g_out, ",", sizeof g_out);
        }
        one_row("profile", "person", "Profile and messages", "Message, mail and who this is");
    }
    al_cat(g_out, ",", sizeof g_out);
    int n = 0;
    if (al_eq(state, "ready")) {
        const char *p = al_json_arr(g_in, "rooms");
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char call[16], name[48], kind[12], about[96], priv[8], id[40], sub[160];
            if (!al_json(g_obj, "call", call, sizeof call) || !call[0]) continue;
            al_json(g_obj, "name", name, sizeof name);
            al_json(g_obj, "kind", kind, sizeof kind);
            al_json(g_obj, "about", about, sizeof about);
            al_json(g_obj, "private", priv, sizeof priv);
            al_cpy(id, "room:", sizeof id);
            al_cat(id, call, sizeof id);
            al_cat(id, ":", sizeof id);
            al_cat(id, kind, sizeof id);
            al_cpy(sub, room_what(kind), sizeof sub);
            if (al_eq(priv, "true")) al_cat(sub, " \xc2\xb7 private, you are a member", sizeof sub);
            if (about[0]) { al_cat(sub, " \xc2\xb7 ", sizeof sub); al_cat(sub, about, sizeof sub); }
            if (n++) al_cat(g_out, ",", sizeof g_out);
            one_row(id, al_eq(priv, "true") ? "lock" : room_icon(kind), name[0] ? name : call, sub);
        }
        if (!n && me) one_row("none", "info", "Nothing shared yet", "What you add above appears here.");
        else if (!n) one_row("none", "info", "Nothing shared with you",
                        al_eq(known, "true") ? "This station hosts no rooms you can see."
                                             : "It hosts no public rooms.");
    } else if (al_eq(state, "failed")) {
        one_row("retry", "refresh", "Nothing to show", why_text(why));
    } else {
        one_row("wait", "hourglass_empty", "Asking the station\xe2\x80\xa6", "Its rooms, galleries, music and files.");
    }
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
}

static void screen_open(const char *name, const char *title)
{
    char m[200] = "{\"type\":\"ui.screen.open\",\"name\":\"";
    al_cat(m, name, sizeof m);
    al_cat(m, "\",\"title\":\"", sizeof m);
    al_jesc(m, title, sizeof m);
    al_cat(m, "\"}", sizeof m);
    say(m);
}

/* ── A room's folders, a folder's items ───────────────────────────────── */
static const char *folder_word(void)
{
    if (al_eq(g_room_kind, "gallery")) return "Events";
    if (al_eq(g_room_kind, "music")) return "Albums";
    return "Shelves";
}

/* field-day-2026 as "field day 2026". */
static void cat_title(char *d, const char *slug, unsigned cap)
{
    char t[64];
    unsigned o = 0;
    for (; slug[o] && o < sizeof t - 1; o++) t[o] = slug[o] == '-' ? ' ' : slug[o];
    t[o] = 0;
    if (t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 32);
    al_jesc(d, t, cap);
}

static int g_room_private;

/* The owner's own room: who sees it, who was added, and stop sharing it.
 * Reads the offers (which name the room's audience and people) into g_in. */
static int cat_own_room(void)
{
    char v[8], call[16];
    int rows = 0;
    req_call("offers");
    al_cat(g_req, "}", sizeof g_req);
    if (!sread()) return 0;
    const char *p = al_json_arr(g_in, "rooms");
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        if (!al_json(g_obj, "call", call, sizeof call) || !al_eq(call, g_room)) continue;
        g_room_private = al_json(g_obj, "private", v, sizeof v) && al_eq(v, "true");
        one_row("own:vis", g_room_private ? "lock" : "public",
                g_room_private ? "Only people you add see this" : "Everybody can see this",
                g_room_private ? "Tap to open it to everybody." : "Tap to make it private.");
        rows++;
        if (g_room_private) {
            const char *q = al_json_arr(g_obj, "people");
            char who[16], id[24], one[64];
            while (q && (q = al_json_next(q, one, sizeof one)) != 0) {
                if (!al_json(one, "call", who, sizeof who) || !who[0]) continue;
                if (al_eq(who, g_me)) continue;      /* the owner is offered its own room */
                al_cpy(id, "m:", sizeof id);
                al_cat(id, who, sizeof id);
                al_cat(g_out, ",", sizeof g_out);
                char inv[8];
                int waiting = al_json(one, "invited", inv, sizeof inv) && al_eq(inv, "true");
                one_row(id, waiting ? "person_add" : "person", who,
                        waiting ? "Added. Sees it once they open your phone. Tap to remove."
                                : "Sees this. Tap to remove.");
            }
            al_cat(g_out, ",", sizeof g_out);
            one_row("own:add", "person_add", "Add a person", "By callsign. They see it the next time they open your phone.");
        }
        al_cat(g_out, ",", sizeof g_out);
        one_row("own:close", "delete", "Stop sharing this", "Removes it from what your phone offers.");
        break;
    }
    return rows;
}

static void push_room(void)
{
    int own = is_me();
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"r_folders\",\"sections\":[{\"title\":\"", sizeof g_out);
    al_cat(g_out, folder_word(), sizeof g_out);
    al_cat(g_out, "\",\"items\":[", sizeof g_out);
    int n = own ? cat_own_room() : 0;
    req_call("folders");
    al_cat(g_req, ",\"room\":\"", sizeof g_req);
    al_cat(g_req, g_room, sizeof g_req);
    al_cat(g_req, "\"}", sizeof g_req);
    sread();
    char state[12] = "", why[24] = "";
    al_json(g_in, "state", state, sizeof state);
    al_json(g_in, "why", why, sizeof why);
    int folders = 0;
    const char *p = al_json_arr(g_in, "folders");
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char id[16], title[64], rid[24];
        if (!al_json(g_obj, "id", id, sizeof id) || !id[0]) continue;
        al_json(g_obj, "title", title, sizeof title);
        unsigned count = num(g_obj, "count");
        if (n++) al_cat(g_out, ",", sizeof g_out);
        folders++;
        al_cpy(rid, "f:", sizeof rid);
        al_cat(rid, id, sizeof rid);
        al_cat(g_out, "{\"id\":\"", sizeof g_out);
        al_cat(g_out, rid, sizeof g_out);
        al_cat(g_out, "\",\"icon\":\"", sizeof g_out);
        al_cat(g_out, al_eq(g_room_kind, "music") ? "album" : al_eq(g_room_kind, "gallery") ? "photo_library" : "folder", sizeof g_out);
        al_cat(g_out, "\",\"title\":\"", sizeof g_out);
        cat_title(g_out, title, sizeof g_out);
        al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
        al_cat_u(g_out, count, sizeof g_out);
        al_cat(g_out, al_eq(g_room_kind, "gallery") ? (count == 1 ? " item" : " items")
                    : al_eq(g_room_kind, "music") ? (count == 1 ? " track" : " tracks")
                    : (count == 1 ? " file" : " files"), sizeof g_out);
        al_cat(g_out, "\"}", sizeof g_out);
    }
    if (!folders) {
        if (n) al_cat(g_out, ",", sizeof g_out);
        if (al_eq(state, "ready")) one_row("none", "info", "Empty", "Nothing was added here yet.");
        else if (al_eq(state, "failed")) one_row("retry", "refresh", "Nothing to show", why_text(why));
        else one_row("wait", "hourglass_empty", "Asking the station\xe2\x80\xa6", "");
    }
    al_cat(g_out, "]}]}", sizeof g_out);
    say(g_out);
}

static void push_folder(void)
{
    req_call("items");
    al_cat(g_req, ",\"room\":\"", sizeof g_req);
    al_cat(g_req, g_room, sizeof g_req);
    al_cat(g_req, "\",\"folder\":\"", sizeof g_req);
    al_cat(g_req, g_folder, sizeof g_req);
    al_cat(g_req, "\",\"offset\":", sizeof g_req);
    al_cat_u(g_req, (unsigned)g_folder_off, sizeof g_req);
    al_cat(g_req, "}", sizeof g_req);
    sread();
    unsigned total = num(g_in, "total"), shown = 0;
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"f_media\",\"value\":{\"layout\":\"", sizeof g_out);
    al_cat(g_out, al_eq(g_room_kind, "gallery") ? "grid" : "list", sizeof g_out);
    al_cat(g_out, "\",\"items\":[", sizeof g_out);
    const char *p = al_json_arr(g_in, "items");
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char file[64], prev[64], name[96], cap[160], size[16];
        if (!al_json(g_obj, "file", file, sizeof file) || !file[0]) continue;
        if (al_len(g_out) > sizeof g_out - 700) break;
        al_json(g_obj, "preview", prev, sizeof prev);
        al_json(g_obj, "name", name, sizeof name);
        al_json(g_obj, "caption", cap, sizeof cap);
        al_json(g_obj, "size", size, sizeof size);
        if (shown++) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "{\"token\":\"", sizeof g_out);
        al_cat(g_out, file, sizeof g_out);
        al_cat(g_out, "\",\"preview\":\"", sizeof g_out);
        al_cat(g_out, prev, sizeof g_out);
        al_cat(g_out, "\",\"title\":\"", sizeof g_out);
        al_jesc(g_out, cap[0] ? cap : name, sizeof g_out);
        al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
        al_jesc(g_out, size, sizeof g_out);
        if (cap[0] && name[0]) { al_cat(g_out, " \\u00b7 ", sizeof g_out); al_jesc(g_out, name, sizeof g_out); }
        al_cat(g_out, "\"}", sizeof g_out);
    }
    al_cat(g_out, "]}}", sizeof g_out);
    say(g_out);

    /* The count, and the way to the rest when a folder is longer than a page. */
    char info[96] = "";
    if (total) {
        al_cat_u(info, (unsigned)g_folder_off + (shown ? 1 : 0), sizeof info);
        al_cat(info, " to ", sizeof info);
        al_cat_u(info, (unsigned)g_folder_off + shown, sizeof info);
        al_cat(info, " of ", sizeof info);
        al_cat_u(info, total, sizeof info);
    } else {
        char state[12] = "";
        al_json(g_in, "state", state, sizeof state);
        al_cpy(info, al_eq(state, "ready") ? "Nothing here yet." : "Asking the station\xe2\x80\xa6", sizeof info);
    }
    al_cpy(g_out, "{\"type\":\"ui.field.set\",\"field\":\"f_info\",\"value\":[{\"title\":\"\",\"items\":[{\"label\":\"", sizeof g_out);
    cat_title(g_out, g_folder_name, sizeof g_out);
    al_cat(g_out, "\",\"value\":\"", sizeof g_out);
    al_jesc(g_out, info, sizeof g_out);
    al_cat(g_out, "\"}]}]}", sizeof g_out);
    say(g_out);
    int more = (unsigned)g_folder_off + shown < total;
    say(more ? "{\"type\":\"ui.field.set\",\"field\":\"f_more__hidden\",\"value\":false}"
             : "{\"type\":\"ui.field.set\",\"field\":\"f_more__hidden\",\"value\":true}");
    say(g_folder_off > 0 ? "{\"type\":\"ui.field.set\",\"field\":\"f_first__hidden\",\"value\":false}"
                         : "{\"type\":\"ui.field.set\",\"field\":\"f_first__hidden\",\"value\":true}");
}

/* ── Entry points (main.c) ────────────────────────────────────────────── */
void browse_init(void)
{
    static const char t[] = "core.station", t2[] = "core.names";
    hal_event_subscribe(t, sizeof t - 1);
    hal_event_subscribe(t2, sizeof t2 - 1);
    static const char path[] = "mesh.db";
    int32_t db = hal_db_open(path, sizeof path - 1);
    wst_opts_t o = {0};
    o.keep_unplaced = 1;                 /* the list wants every station */
    o.recent_s = 30ULL * 86400ULL;       /* on the map: heard of this month */
    o.color = "#43A047";
    o.actions = 1;                       /* Open, Message, Show all N here */
    o.done = walked;
    wst_init(db, &o);
    read_host();
    wst_me(g_me);
    wst_follow();
    heard_sync(0);
    push_list();
    wst_begin(1);
}

void browse_changed(const char *topic)
{
    if (al_eq(topic, "core.station")) {
        if (g_view == V_STATION) push_station();
        else if (g_view == V_ROOM) push_room();
        else if (g_view == V_FOLDER) push_folder();
        return;
    }
    /* The archivers answered a name asked for: read it again. */
    if (al_eq(topic, "core.names")) {
        if (g_f.q[0] && g_names_q) { ask_names(0); g_list_dirty = 1; g_map_dirty = 1; }
        return;
    }
    /* The room moved (core.monitor, core.rns.graph). Each redraw is a few
     * reads and one large message: at most one per tick, not one per packet. */
    g_list_dirty = 2;
}

void browse_tick(void)
{
    /* The copy of the archive catches up a few pages a tick, and is asked
     * to every two minutes (wst_begin keeps the pace). */
    if (wst_busy()) wst_step();
    else wst_begin(0);
    if (g_list_dirty) {
        heard_sync(g_list_dirty == 2);
        push_list();
    }
    if (g_map_dirty) push_map();
}

/* A station opened from the list, a marker or Show on map. */
static void open_station(const char *call)
{
    al_cpy(g_call, call, sizeof g_call);
    g_view = V_STATION;
    say("{\"type\":\"ui.field.set\",\"field\":\"s_ways\",\"value\":{\"routes\":[]}}");
    push_station();
    screen_open("Station", g_call);
}

int browse_command(const char *cmd, const char *full)
{
    char id[64];
    if (al_eq(cmd, "ready")) {
        /* The page opened: its map and list from the copy at once, then
         * the copy brought up to date behind them. */
        g_page = 1;
        g_map_dirty = 1;
        g_fitted = 0;
        heard_sync(0);
        push_list();
        push_map();
        wst_begin(1);
        return 0;                        /* main.c draws the graph too */
    }
    if (al_eq(cmd, "stations_tap")) {
        if (!al_json(full, "stations_id", id, sizeof id) || !id[0]) return 1;
        if (al_eq(id, "more")) { g_shown += 50; push_list(); return 1; }
        if (al_eq(id, "none")) {
            if (!g_names_q) { g_names_q = hal_time_epoch(); ask_names(1); }
            push_list();
            return 1;
        }
        open_station(id);
        return 1;
    }
    /* The row's Message button: Chat's conversation with it. */
    if (al_eq(cmd, "st_msg")) {
        if (al_json(full, "stations_id", id, sizeof id) && id[0]) message(id);
        return 1;
    }
    if (al_eq(cmd, "stations_search")) {
        char q[40] = "";
        al_json(full, "stations_query", q, sizeof q);
        unsigned i = 0;
        while (q[i] == ' ') i++;
        al_cpy(g_f.q, q + i, sizeof g_f.q);
        g_shown = 50;
        g_names_q = 0;
        /* Enter on a search that finds nothing here asks the archivers, as
         * the portal does; a few letters typed do not. */
        if (g_f.q[0] && al_len(g_f.q) >= 3 && !wst_count(&g_f)) {
            g_names_q = hal_time_epoch();
            ask_names(1);
        }
        push_list();
        return 1;
    }
    if (al_eq(cmd, "st_tags_tap")) {
        if (!al_json(full, "st_tags_id", id, sizeof id) || !id[0]) return 1;
        if (al_eq(id, "area")) g_f.area[0] = 0;
        else wst_filter_tag(&g_f, id);
        g_shown = 50;
        push_list();
        return 1;
    }
    if (al_eq(cmd, "marker_tap")) return 1;   /* the host shows its popup */
    if (al_eq(cmd, "marker_action")) {
        char act[16] = "";
        if (!al_json(full, "id", id, sizeof id)) return 1;
        al_json(full, "action", act, sizeof act);
        if (al_starts(id, "pl:") && al_eq(act, "open")) { open_station(id + 3); return 1; }
        if (al_starts(id, "pl:") && al_eq(act, "message")) { message(id + 3); return 1; }
        if (al_starts(id, "sq:") && al_eq(act, "area")) {
            /* Show all N here: the list, filtered to the square. */
            al_cpy(g_f.area, id + 3, sizeof g_f.area);
            g_shown = 50;
            push_list();
            say("{\"type\":\"ui.drawer\",\"state\":\"half\"}");
        }
        return 1;
    }
    if (al_eq(cmd, "st_stats_tap")) return 1;
    if (al_eq(cmd, "s_offers_tap")) {
        if (!al_json(full, "s_offers_id", id, sizeof id) || !id[0]) return 1;
        if (al_eq(id, "profile")) {
            char m[96] = "{\"type\":\"ui.profile.open\",\"callsign\":\"";
            al_cat(m, g_call, sizeof m);
            al_cat(m, "\"}", sizeof m);
            say(m);
            return 1;
        }
        if (al_eq(id, "msg")) { message(g_call); return 1; }
        if (al_eq(id, "map")) {
            static char row[1024];
            char pos[40] = "", v[16] = "";
            long half = 0;
            if (wst_row(g_call, row, sizeof row)) {
                al_json(row, "pos", pos, sizeof pos);
                if (al_json(row, "half", v, sizeof v)) half = (long)num(row, "half");
            }
            if (!pos[0]) return 1;
            /* Back to the map: the panel closes, the map goes there. */
            g_view = V_LIST;
            say("{\"type\":\"ui.screen.close\"}");
            locate(pos, half);
            return 1;
        }
        if (al_eq(id, "host:toggle")) {
            const char *kv = g_serving ? "hostStation=0" : "hostStation=1";
            hal_xprs_set_pref(kv, al_len(kv));
            return 1;                    /* core.station redraws when it is done */
        }
        if (al_starts(id, "host:")) {
            if (al_eq(id, "host:busy")) return 1;
            char m[96] = "{\"type\":\"station.share\",\"kind\":\"";
            al_cat(m, id + 5, sizeof m);
            al_cat(m, "\"}", sizeof m);
            say(m);
            return 1;
        }
        if (al_eq(id, "retry")) {
            /* Asked for again by hand: a new visit now, not the answer kept. */
            req_call("forget");
            al_cat(g_req, "}", sizeof g_req);
            sread();
        }
        if (al_eq(id, "retry") || al_eq(id, "wait")) { push_station(); return 1; }
        if (!al_starts(id, "room:")) return 1;
        /* room:<X5call>:<kind> */
        unsigned i = 5, o = 0;
        while (id[i] && id[i] != ':' && o < sizeof g_room - 1) g_room[o++] = id[i++];
        g_room[o] = 0;
        al_cpy(g_room_kind, id[i] == ':' ? id + i + 1 : "", sizeof g_room_kind);
        if (!is_collection(g_room_kind)) {
            say("{\"type\":\"ui.snack\",\"level\":\"info\",\"text\":\"Open its chat rooms and forums from Chat for now.\"}");
            return 1;
        }
        g_view = V_ROOM;
        push_room();
        screen_open("Room", room_what(g_room_kind));
        return 1;
    }
    if (al_eq(cmd, "prompt")) {
        /* The owner said yes to something asked below. */
        char pid[32], in[24], m[200];
        if (!al_json(full, "prompt_id", pid, sizeof pid) || !is_me()) return 1;
        al_cpy(m, "{\"type\":\"station.manage\",\"room\":\"", sizeof m);
        al_cat(m, g_room, sizeof m);
        al_cat(m, "\",\"action\":\"", sizeof m);
        if (al_eq(pid, "own:add")) {
            if (!al_json(full, "prompt_input", in, sizeof in) || !in[0]) return 1;
            al_cat(m, "add\",\"member\":\"", sizeof m);
            al_jesc(m, in, sizeof m);
        } else if (al_starts(pid, "m:")) {
            al_cat(m, "remove\",\"member\":\"", sizeof m);
            al_jesc(m, pid + 2, sizeof m);
        } else if (al_eq(pid, "own:close")) {
            al_cat(m, "close", sizeof m);
        } else return 1;
        al_cat(m, "\"}", sizeof m);
        say(m);
        if (al_eq(pid, "own:close")) {      /* the room is gone: back to the phone's page */
            g_view = V_STATION;
            say("{\"type\":\"ui.screen.close\"}");
            screen_open("Station", g_call);
        }
        return 1;
    }
    if (al_eq(cmd, "r_folders_tap")) {
        if (!al_json(full, "r_folders_id", id, sizeof id) || !id[0]) return 1;
        if (is_me() && al_eq(id, "own:vis")) {
            char m[160] = "{\"type\":\"station.manage\",\"room\":\"";
            al_cat(m, g_room, sizeof m);
            al_cat(m, g_room_private ? "\",\"action\":\"public\"}" : "\",\"action\":\"private\"}", sizeof m);
            say(m);
            return 1;
        }
        if (is_me() && al_eq(id, "own:add")) {
            say("{\"type\":\"ui.prompt\",\"id\":\"own:add\",\"title\":\"Add a person\","
                "\"body\":\"Their callsign, as it shows in the list.\","
                "\"input\":{\"hint\":\"X1…\"},\"confirm\":\"Add\"}");
            return 1;
        }
        if (is_me() && (al_eq(id, "own:close") || al_starts(id, "m:"))) {
            char m[300] = "{\"type\":\"ui.prompt\",\"id\":\"";
            al_cat(m, id, sizeof m);
            al_cat(m, "\",\"title\":\"", sizeof m);
            if (al_eq(id, "own:close")) al_cat(m, "Stop sharing this?\",\"body\":\"Nobody will see it on your phone any more.\",\"confirm\":\"Stop sharing\"}", sizeof m);
            else { al_cat(m, "Remove ", sizeof m); al_cat(m, id + 2, sizeof m); al_cat(m, "?\",\"body\":\"They will no longer see this.\",\"confirm\":\"Remove\"}", sizeof m); }
            say(m);
            return 1;
        }
        if (!al_starts(id, "f:")) { push_room(); return 1; }
        al_cpy(g_folder, id + 2, sizeof g_folder);
        g_folder_off = 0;
        /* Its title, from the listing the row came from. */
        g_folder_name[0] = 0;
        req_call("folders");
        al_cat(g_req, ",\"room\":\"", sizeof g_req);
        al_cat(g_req, g_room, sizeof g_req);
        al_cat(g_req, "\"}", sizeof g_req);
        if (sread()) {
            const char *p = al_json_arr(g_in, "folders");
            char fid[16];
            while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
                if (al_json(g_obj, "id", fid, sizeof fid) && al_eq(fid, g_folder)) {
                    al_json(g_obj, "title", g_folder_name, sizeof g_folder_name);
                    break;
                }
            }
        }
        g_view = V_FOLDER;
        push_folder();
        screen_open("Folder", room_what(g_room_kind));
        return 1;
    }
    /* The arrow on a screen this file opened: one level up, not all the way
     * out. The host shows one such screen at a time and says when it went. */
    if (al_eq(cmd, "screen_closed")) {
        if (g_view == V_FOLDER) {
            g_view = V_ROOM;
            push_room();
            screen_open("Room", room_what(g_room_kind));
        } else if (g_view == V_ROOM) {
            g_view = V_STATION;
            push_station();
            screen_open("Station", g_call);
        } else if (g_view == V_STATION) {
            g_view = V_LIST;
        }
        return 1;
    }
    if (al_eq(cmd, "f_more")) { g_folder_off += 60; push_folder(); return 1; }
    if (al_eq(cmd, "f_first")) { g_folder_off = 0; push_folder(); return 1; }
    return 0;
}
