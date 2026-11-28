/*
 * devices.h -- what the three sources of this wapp ask of each other.
 *
 * main.c is the shell: it owns the one inbox, the one event queue, the
 * subscriptions and the one list. stations.c is everything about a station
 * (a board on the cable, a station on the air, what its owner sets) and
 * things.c everything about equipment (an X4 callsign: what it is, what it
 * says, its picture). Neither half reads the inbox or the queue itself: two
 * readers of one queue each lose what the other took.
 */
#ifndef DEVICES_H
#define DEVICES_H

#define DV_ST 1
#define DV_TH 2

/* The page clock, while a page is open (main.c). */
#define PAGE_TICK_MS 400

/* The one command and the one event being handled. */
extern char dv_buf[4096];
extern char dv_ev[4096];
extern char dv_topic[64];

/* A subscription is held while either half wants it: one half letting go of
 * `xprs.observation` must not silence the other's doorbell. */
void dv_sub(int who, const char *topic, int want);
/* What a half lists changed: the shell draws the one list once, after the
 * current command or event is done. */
void dv_list_dirty(void);
/* Forget every subscription and what is in front (a restart, in tests). */
void dv_reset(void);
/* A half opened a panel: `screen_closed` goes to it. */
void dv_front(int who);

/* Each half. *_sections writes list sections, comma separated and with no
 * envelope, into its own buffer and returns it ("" when it has none). */
void st_init(void);
void st_event(void);
void st_command(void);
int  st_screen(void);
const char *st_sections(int part);   /* 0 cable+yours, 1 other stations, 2 help */

void th_init(void);
void th_event(void);
void th_pump(void);
void th_command(void);
int  th_screen(void);
int  th_has(const char *call);
void th_open(const char *call);
void th_list_prepare(void);
const char *th_sections(int part);   /* 0 pinned, 1 nearby and archived */

/* The module's entry points (main.c), for the native tests. */
int32_t module_init(void);
int32_t module_tick(void);
int32_t module_handle_event(void);
int32_t module_tick_interval_ms(void);
void module_destroy(void);

#endif
