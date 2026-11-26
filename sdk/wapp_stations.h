/*
 * wapp_stations: every XPRS station a wapp knows of, kept in its own table,
 * drawn on its map and listed in its drawer: the one implementation of what
 * the xprs.dev portal's map does (portal/page/chat_page.html mapStations,
 * mapPos, mapDraw, mapList), for wapps. Check-in and Mesh link it.
 *
 *   MODULE_SRCS += ../sdk/wapp_stations.c ../alerts/util.c
 *
 * Where the rows come from: the core keeps identities and service
 * announcements from anyone once followed (wst_follow, hal_xprs_follow_kind)
 * and asks the archivers for new ones on its own cadence; this module copies
 * what a map and a list need into the `places` table, one row per station
 * with its position already read, so a screen opens on that copy at once and
 * the copy catches up with the archive a few pages per step (wst_step). The
 * stations this device hears right now are told with wst_heard.
 *
 * What a station is, read the way the portal reads it:
 *   - t:identity pos: (6.3.2; redaction bars name a square, 6.2.1), nick:,
 *     k: (its key, kept only from a verified identity: who can be messaged);
 *   - t:service serve:archive,relay source:solar pos: (12.9), its newest word.
 * Filters are tags: "serve:<v>", "source:<v>", "reach:<local|regional|
 * global>", "placed" (says where it is), "key" (can be messaged). A new kind
 * of station is a new tag with no change here.
 *
 * Pure helpers first (no HAL; native tests call them directly), then the
 * table, the walk, the map and the list.
 */
#ifndef WAPP_STATIONS_H
#define WAPP_STATIONS_H

#include <stdint.h>

/* ── where a station says it is ───────────────────────────────────────── */
/* "lat,lon" in decimal degrees into degrees times 1e7. 1 when well formed. */
int  wst_pos_parse(const char *pos, long *lat7, long *lon7);
int  wst_pos_ok(const char *pos);
/* The same point with four decimals (eleven metres). */
int  wst_pos_norm(const char *in, char *out, unsigned cap);
/* A pos: with redaction bars (38█████,-9█████) as the MIDDLE of the square
 * its readable digits name, with the square's half-width in metres; a plain
 * value is its own point (half 0). The C twin of the core's xprsPosCell and
 * the portal's mapPos. */
int  wst_pos_square(const char *pos, char *mid, unsigned cap, long *half_m);
/* Metres between two points, -1 when either is not one. */
long wst_dist_m(const char *a, const char *b);
/* "350 m", "4.2 km", "35 km". */
void wst_dist_text(long m, char *out, unsigned cap);
/* An XPRS callsign (X1..X5, two to six letters or digits, -SSID allowed),
 * not another network's node. */
int  wst_xprs_call(const char *call);
/* A callsign without its -SSID. */
void wst_base_call(char *out, unsigned cap, const char *call);
/* "just now", "12 min ago", "5 h ago", "3 d ago". */
void wst_ago(char *out, unsigned cap, unsigned long long ts, unsigned long long now);
/* "Archivers" for serve:archive, "Solar" for source:solar, "On the map" for
 * placed...; an unknown tag's value, capitalised. */
void wst_tag_name(const char *tag, char *out, unsigned cap);

/* ── the table and its walk of the archive ────────────────────────────── */
typedef struct {
    /* Keep stations that never said where they are (a list wants them;
     * Check-in's map does not). */
    int keep_unplaced;
    /* On the map: stations whose newest word is younger than this. */
    unsigned long long recent_s;
    /* Marker colour of a station (and of a square of them). */
    const char *color;
    /* A station the wapp draws itself (Check-in's check-in points): 1 skips
     * it on the map. May be 0. */
    int (*skip)(const char *call);
    /* Each place drawn, for the wapp's framing of its map. May be 0. */
    void (*placed)(const char *pos);
    /* Markers carry what their popup offers: an archiver "Visit", any
     * other station "Open", and "Message" when it published a key; the
     * host sends marker_action. */
    int actions;
    /* A walk of the archive finished; [changed] when it brought anything. */
    void (*done)(int changed);
} wst_opts_t;

/* Start: create or extend `places` in [db], the wapp's open store. */
void wst_init(int32_t db, const wst_opts_t *o);
/* This device's own callsign: never on its own map or list. */
void wst_me(const char *call);
/* Ask the core to keep identities and service announcements from anyone. */
void wst_follow(void);
/* Bring the copy up to date, unless it is or was a moment ago; [now_]
 * skips the wait (a screen just opened). */
void wst_begin(int now_);
/* A few pages of the walk; call from module_tick while wst_busy(). */
void wst_step(void);
int  wst_busy(void);
/* When the copy was last brought up to date (epoch s), 0 never. */
unsigned long long wst_updated(void);
/* One archive or history row (JSON with "wire", "sig") into the copy.
 * 1 when it changed what is kept. */
int  wst_take(const char *row);
/* A station this device hears now, how (local|regional|global) and when
 * (epoch s): kept beside what the archive says. */
void wst_heard(const char *call, const char *reach, unsigned long long ts);
/* A station named by the archivers (hal_xprs_names): its name and when it
 * was heard of, so a search finds it before its identity is walked. */
void wst_named(const char *call, const char *nick, unsigned long long ts);

/* ── the map ──────────────────────────────────────────────────────────── */
/* Every placed station as a ui.map.marker "pl:<call>", the freshest 1500,
 * with its kind ("archive", "person" or "station": the host draws
 * archivers apart, above the rest), its tags, and with opts.actions
 * "Visit" (its page, as its visitor), "Message" when it published a key and
 * "Keep my messages here" for an archiver not yet keeping them (action
 * "keep"). The host
 * groups near ones and fans out those on one point. Does not clear the map
 * first. wst_draw_f draws only those matching [f] (its search and tags, not
 * its area). */
void wst_draw(void);
struct wst_filter;
void wst_draw_f(const struct wst_filter *f);
/* The pills over the map: "N stations", "Updated <when>". */
void wst_status(void);
/* How many are on the map (placed and recent). */
int  wst_count_placed(void);

/* ── the list ─────────────────────────────────────────────────────────── */
typedef struct wst_filter {
    char q[40];          /* part of a callsign or a name, any case */
    char area[40];       /* only the stations at this point (a pos) */
    char kind[12];       /* "" all, "archive", "people" (X1) or "other" */
    char tags[8][28];    /* every one of these */
    int  ntags;
} wst_filter_t;

/* The filter over the map, the portal's All / Archivers / People / Other:
 * [{"id":"all","label":"All","count":N,"on":true},...] with the counts of
 * [f]'s search and tags. Bytes written. */
int  wst_kinds(const wst_filter_t *f, char *out, unsigned cap);
/* The archivers that keep this phone's messages, from hal_xprs_archivers'
 * answer ({"list":[...]}): drawn filled, tagged "Keeps your messages",
 * and without the "Keep my messages here" action. */
void wst_mine(const char *json);
int  wst_is_mine(const char *call);

/* Toggle a tag in [f]; 1 when it is now on. */
int  wst_filter_tag(wst_filter_t *f, const char *tag);
int  wst_filter_has(const wst_filter_t *f, const char *tag);
/* The stations matching [f], freshest first, rows [offset, offset+limit)
 * as the JSON array hal_db_query writes ({call,nick,pos,half,last,serve,src,
 * key,reach}). Bytes written, 0 none. */
int  wst_list(const wst_filter_t *f, int offset, int limit, char *out, unsigned cap);
/* How many match [f]; and how many are known at all. */
int  wst_count(const wst_filter_t *f);
int  wst_total(void);
/* One station's row as wst_list writes it, 0 when unknown. */
int  wst_row(const char *call, char *out, unsigned cap);
/* The tags known with how many stations carry each, most first, as
 * [{"id":"serve:archive","label":"Archivers","count":12,"on":true}] with
 * "on" from [f]. Bytes written. */
int  wst_chips(const wst_filter_t *f, char *out, unsigned cap);
/* The tags of one row (from wst_list) as display words joined by " · ". */
void wst_row_tags(const char *row, char *out, unsigned cap);

#endif
