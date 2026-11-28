/*
 * devices -- every machine around you in one list: the boards on a USB
 * cable, the stations heard on WiFi, Bluetooth or LoRa or reached over
 * Reticulum (an ESP32, a Linux server, a phone left at home as a station),
 * and the equipment a controller speaks for (an X4 callsign: a camera, a
 * doorbell, a sensor, a plug).
 *
 * It replaces two wapps, Firmwares and Things, and keeps both whole:
 *
 *   stations.c   a board on the cable (identify, install XPRS, claim), and a
 *                station on the air: what it runs and how it is doing for
 *                anybody, and for an owner its WiFi, settings, callsign,
 *                firmware, diagnostics and answers (XPRS.md 11.8-11.10, 15.5)
 *   things.c     equipment: what each one is, what it says, its picture and
 *                live view, and being told when it rings (11.7-11.7.2)
 *
 * This file is the shell. A wapp has one inbox and one event queue, so the
 * shell reads both and hands each command and event to the half it belongs
 * to; it holds the subscriptions for both (devices.h) and draws the one
 * list from the sections each half writes.
 *
 * Who may change a station is the station's own answer (11.9 `owner:`), and
 * how a command reaches it is the core's (docs/architecture.md, "A command
 * is the core's to deliver"): a station you own is administered the same way
 * in the room and from the other side of the internet, and this wapp never
 * learns which lane carried anything.
 */
#include "../hal/xprs_wasm_hal.h"
#include "wire.h"
#include "devices.h"

char dv_buf[4096];
char dv_ev[4096];
char dv_topic[64];

static char g_list[40960];
static int  g_list_dirty;
static int  g_front;

/* ── Subscriptions, held for both halves ──────────────────────────────── */
#define SUB_MAX 8
static struct { char topic[24]; unsigned who; } g_subs[SUB_MAX];

void dv_sub(int who, const char *topic, int want)
{
    int at = -1, free_at = -1;
    for (int i = 0; i < SUB_MAX; i++) {
        if (dv_eq(g_subs[i].topic, topic)) at = i;
        else if (!g_subs[i].topic[0] && free_at < 0) free_at = i;
    }
    if (at < 0) {
        if (!want || free_at < 0) return;
        at = free_at;
        dv_cpy(g_subs[at].topic, topic, sizeof g_subs[at].topic);
        g_subs[at].who = 0;
    }
    unsigned was = g_subs[at].who;
    unsigned now = want ? (was | (unsigned)who) : (was & ~(unsigned)who);
    g_subs[at].who = now;
    if (!was && now) hal_event_subscribe(topic, dv_len(topic));
    if (was && !now) {
        hal_event_unsubscribe(topic, dv_len(topic));
        g_subs[at].topic[0] = 0;
    }
}

void dv_list_dirty(void) { g_list_dirty = 1; }

/* An engine that starts over holds nothing. The host gives every engine
 * fresh memory; the native tests restart one in place and call this. */
void dv_reset(void)
{
    for (int i = 0; i < SUB_MAX; i++) { g_subs[i].topic[0] = 0; g_subs[i].who = 0; }
    g_front = 0;
    g_list_dirty = 0;
}
void dv_front(int who) { g_front = who; }

/* ── The one list ─────────────────────────────────────────────────────── */
static void part(const char *sections, int *any)
{
    if (!sections || !sections[0]) return;
    if (*any) dv_cat(g_list, ",", sizeof g_list);
    *any = 1;
    dv_cat(g_list, sections, sizeof g_list);
}

/* On the cable, then what is yours (stations, equipment), then what is
 * merely nearby, then how to add a board. */
static void draw_list(void)
{
    if (!g_list_dirty) return;
    g_list_dirty = 0;
    /* Nobody is looking: an engine woken by a packet draws nothing. */
    if (!hal_ui_attached()) return;
    int any = 0;
    dv_cpy(g_list, "{\"type\":\"ui.people.set\",\"field\":\"devices\",\"sections\":[", sizeof g_list);
    th_list_prepare();
    part(st_sections(0), &any);
    part(th_sections(0), &any);
    part(st_sections(1), &any);
    part(th_sections(1), &any);
    part(st_sections(2), &any);
    dv_cat(g_list, "]}", sizeof g_list);
    hal_msg_send(g_list, dv_len(g_list));
}

/* ── Events and commands ──────────────────────────────────────────────── */
static void drain_events(void)
{
    while (hal_event_available()) {
        uint32_t n = hal_event_recv(dv_topic, sizeof dv_topic - 1, dv_ev, sizeof dv_ev - 1);
        dv_ev[n] = 0;
        dv_topic[sizeof dv_topic - 1] = 0;
        st_event();
        th_event();
    }
}

static int is_thing_command(const char *cmd)
{
    static const char *const mine[] = {
        "scan", "things_tap", "pin", "unpin", "connect", "disconnect", "snap",
        "live", "unlive", "th_apply", "th_forget", "th_more", "th_less",
        "watch", "watch_move", "unwatch_move", "unwatch", 0
    };
    for (int i = 0; mine[i]; i++) if (dv_eq(cmd, mine[i])) return 1;
    return 0;
}

static void on_command(void)
{
    char cmd[40] = "";
    if (!dv_json(dv_buf, "command", cmd, sizeof cmd)) {
        th_command();                 /* the host opening us at one thing */
        return;
    }
    if (dv_eq(cmd, "ready") || dv_eq(cmd, "refresh")) {
        /* Equipment first: both halves ask the local network who is there,
         * and the core answers the first ask and rate-limits the second. */
        th_command();
        st_command();
        g_list_dirty = 1;
        return;
    }
    if (dv_eq(cmd, "screen_closed") || dv_eq(cmd, "back")) {
        if (g_front == DV_TH) th_command(); else st_command();
        g_front = th_screen() ? DV_TH : st_screen() ? DV_ST : 0;
        return;
    }
    if (dv_eq(cmd, "devices_tap")) {
        char id[70];
        if (dv_json(dv_buf, "devices_id", id, sizeof id) && th_has(id)) th_open(id);
        else st_command();
        return;
    }
    if (is_thing_command(cmd)) th_command();
    else st_command();
}

/* ── Entry points ─────────────────────────────────────────────────────── */
int32_t module_init(void)
{
    st_init();
    th_init();
    g_list_dirty = 1;
    draw_list();
    return 0;
}

int32_t module_tick(void)
{
    drain_events();
    th_pump();
    draw_list();
    return 0;
}

int32_t module_handle_event(void)
{
    drain_events();
    th_pump();
    uint32_t n = hal_msg_recv(dv_buf, sizeof dv_buf - 1);
    if (n > 0) {
        dv_buf[n] = 0;
        on_command();
        /* Every command carries every field, secrets included: whatever this
         * one was, the password it brought does not outlive it. */
        for (unsigned i = 0; i < n; i++) dv_buf[i] = 0;
    }
    draw_list();
    return 0;
}

/* The page's clock is the equipment half's: it polls a picture that was
 * asked for and carries a live view (things.c, th_pump). With no page there
 * is no clock at all: everything then happens because something arrived. */
int32_t module_tick_interval_ms(void)
{
    return hal_ui_attached() ? (int32_t)PAGE_TICK_MS : 0;
}

void module_destroy(void) {}
