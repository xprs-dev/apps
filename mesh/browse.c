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
static int  g_off[3];       /* the page of each scope        */
static int  g_list_dirty = 1;

static const char *const SCOPE[3] = { "local", "regional", "global" };
static const char *const SCOPE_TAB[3] = { "Local", "Regional", "Global" };

static unsigned num(const char *json, const char *key)
{
    char v[16];
    unsigned n = 0;
    if (!al_json(json, key, v, sizeof v)) return 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (unsigned)(*p - '0');
    return n;
}

static void cat_ago(char *d, unsigned s, unsigned cap)
{
    if (s < 90) al_cat(d, "now", cap);
    else if (s < 5400) { al_cat_u(d, s / 60, cap); al_cat(d, " min ago", cap); }
    else if (s < 172800) { al_cat_u(d, s / 3600, cap); al_cat(d, " h ago", cap); }
    else { al_cat_u(d, s / 86400, cap); al_cat(d, " days ago", cap); }
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

/* ── The list ─────────────────────────────────────────────────────────── */
static void read_host(void);
static char g_me[16];
static int g_serving;

static void push_list(void)
{
    g_list_dirty = 0;
    read_host();
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":[", sizeof g_out);
    for (int s = 0; s < 3; s++) {
        al_cpy(g_req, "{\"get\":\"list\",\"scope\":\"", sizeof g_req);
        al_cat(g_req, SCOPE[s], sizeof g_req);
        al_cat(g_req, "\",\"offset\":", sizeof g_req);
        al_cat_u(g_req, (unsigned)g_off[s], sizeof g_req);
        al_cat(g_req, "}", sizeof g_req);
        unsigned total = 0, shown = 0;
        if (sread()) total = num(g_in, "total");
        if (s) al_cat(g_out, ",", sizeof g_out);
        al_cat(g_out, "{\"tab\":\"", sizeof g_out);
        al_cat(g_out, SCOPE_TAB[s], sizeof g_out);
        al_cat(g_out, "\",\"title\":\"", sizeof g_out);
        al_cat(g_out, SCOPE_TAB[s], sizeof g_out);
        al_cat(g_out, "\",\"items\":[", sizeof g_out);
        const char *p = al_json_arr(g_in, "rows");
        /* The phone in the hand comes first: it is a station too. */
        if (s == 0 && g_me[0]) {
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
        while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
            char call[16], kind[12], api[8];
            if (!al_json(g_obj, "call", call, sizeof call) || !call[0]) continue;
            /* Room for this row and the closing of the message, or stop. */
            if (al_len(g_out) > sizeof g_out - 700) break;
            al_json(g_obj, "kind", kind, sizeof kind);
            al_json(g_obj, "api", api, sizeof api);
            if (shown++) al_cat(g_out, ",", sizeof g_out);
            al_cat(g_out, "{\"id\":\"", sizeof g_out);
            al_cat(g_out, call, sizeof g_out);
            al_cat(g_out, "\",\"call\":\"", sizeof g_out);
            al_cat(g_out, call, sizeof g_out);
            al_cat(g_out, "\",\"title\":\"", sizeof g_out);
            al_cat(g_out, call, sizeof g_out);
            al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
            al_cat(g_out, kind_name(kind), sizeof g_out);
            al_cat(g_out, " \\u00b7 ", sizeof g_out);
            cat_bearers(g_out, g_obj, sizeof g_out);
            unsigned hops = num(g_obj, "hops");
            if (hops > 1) {
                al_cat(g_out, " \\u00b7 ", sizeof g_out);
                al_cat_u(g_out, hops, sizeof g_out);
                al_cat(g_out, " hops", sizeof g_out);
            }
            char dm[16];
            if (al_json(g_obj, "dist_m", dm, sizeof dm) && dm[0]) {
                al_cat(g_out, " \\u00b7 ", sizeof g_out);
                cat_dist(g_out, num(g_obj, "dist_m"), sizeof g_out);
            }
            al_cat(g_out, "\",\"tags\":[\"", sizeof g_out);
            cat_ago(g_out, num(g_obj, "age_s"), sizeof g_out);
            al_cat(g_out, "\"", sizeof g_out);
            if (al_eq(api, "true")) al_cat(g_out, ",\"Browse\"", sizeof g_out);
            al_cat(g_out, "]}", sizeof g_out);
        }
        unsigned upto = (unsigned)g_off[s] + shown;
        if (g_off[s] > 0 || upto < total) {
            if (shown) al_cat(g_out, ",", sizeof g_out);
            al_cat(g_out, "{\"id\":\"more:", sizeof g_out);
            al_cat(g_out, SCOPE[s], sizeof g_out);
            al_cat(g_out, "\",\"icon\":\"more_horiz\",\"title\":\"", sizeof g_out);
            al_cat(g_out, upto < total ? "Show the next ones" : "Back to the first", sizeof g_out);
            al_cat(g_out, "\",\"subtitle\":\"", sizeof g_out);
            al_cat_u(g_out, (unsigned)g_off[s] + (shown ? 1 : 0), sizeof g_out);
            al_cat(g_out, " to ", sizeof g_out);
            al_cat_u(g_out, upto, sizeof g_out);
            al_cat(g_out, " of ", sizeof g_out);
            al_cat_u(g_out, total, sizeof g_out);
            al_cat(g_out, "\"}", sizeof g_out);
        }
        al_cat(g_out, "]}", sizeof g_out);
    }
    al_cat(g_out, "]}", sizeof g_out);
    say(g_out);
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

static void push_room(void)
{
    req_call("folders");
    al_cat(g_req, ",\"room\":\"", sizeof g_req);
    al_cat(g_req, g_room, sizeof g_req);
    al_cat(g_req, "\"}", sizeof g_req);
    sread();
    char state[12] = "", why[24] = "";
    al_json(g_in, "state", state, sizeof state);
    al_json(g_in, "why", why, sizeof why);
    al_cpy(g_out, "{\"type\":\"ui.people.set\",\"field\":\"r_folders\",\"sections\":[{\"title\":\"", sizeof g_out);
    al_cat(g_out, folder_word(), sizeof g_out);
    al_cat(g_out, "\",\"items\":[", sizeof g_out);
    int n = 0;
    const char *p = al_json_arr(g_in, "folders");
    while ((p = al_json_next(p, g_obj, sizeof g_obj)) != 0) {
        char id[16], title[64], rid[24];
        if (!al_json(g_obj, "id", id, sizeof id) || !id[0]) continue;
        al_json(g_obj, "title", title, sizeof title);
        unsigned count = num(g_obj, "count");
        if (n++) al_cat(g_out, ",", sizeof g_out);
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
    if (!n) {
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
    static const char t[] = "core.station";
    hal_event_subscribe(t, sizeof t - 1);
    push_list();
}

void browse_changed(const char *topic)
{
    if (al_eq(topic, "core.station")) {
        if (g_view == V_STATION) push_station();
        else if (g_view == V_ROOM) push_room();
        else if (g_view == V_FOLDER) push_folder();
        return;
    }
    /* The room moved (core.monitor, core.rns.graph). Each redraw is three
     * reads and one large message: at most one per tick, not one per packet. */
    g_list_dirty = 1;
}

void browse_tick(void)
{
    if (g_list_dirty) push_list();
}

int browse_command(const char *cmd, const char *full)
{
    char id[64];
    if (al_eq(cmd, "stations_tap") || al_eq(cmd, "stations_action")) {
        if (!al_json(full, "stations_id", id, sizeof id) || !id[0]) return 1;
        if (al_starts(id, "more:")) {
            for (int s = 0; s < 3; s++) {
                if (!al_eq(id + 5, SCOPE[s])) continue;
                al_cpy(g_req, "{\"get\":\"list\",\"scope\":\"", sizeof g_req);
                al_cat(g_req, SCOPE[s], sizeof g_req);
                al_cat(g_req, "\",\"offset\":", sizeof g_req);
                al_cat_u(g_req, (unsigned)g_off[s] + 60, sizeof g_req);
                al_cat(g_req, "}", sizeof g_req);
                g_off[s] = sread() && al_json_next(al_json_arr(g_in, "rows"), g_obj, sizeof g_obj)
                               ? g_off[s] + 60 : 0;
            }
            push_list();
            return 1;
        }
        al_cpy(g_call, id, sizeof g_call);
        g_view = V_STATION;
        say("{\"type\":\"ui.field.set\",\"field\":\"s_ways\",\"value\":{\"routes\":[]}}");
        push_station();
        screen_open("Station", g_call);
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
    if (al_eq(cmd, "r_folders_tap")) {
        if (!al_json(full, "r_folders_id", id, sizeof id) || !id[0]) return 1;
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
