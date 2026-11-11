/*
 * ci.h -- the check-in, as packets: built, read and decided. Pure: no HAL, so
 * the native tests run exactly what the wapp runs. Strings, time, JSON and
 * the section 5 identifier come from the Alerts wapp's util.c (al_*).
 *
 * Nothing here is new to XPRS. The ask is an operator-defined command
 * (XPRS.md 11: what a cmd: word means is agreed between the two stations),
 * the certificate is that command's t:result (11.1), and every key on it is
 * one the format already has. The wapp builds a wire UNSIGNED and the core
 * signs it, so a builder leaves room for " sig:" and 60 characters.
 */
#ifndef CHECKIN_CI_H
#define CHECKIN_CI_H

#define CI_WIRE_MAX       250
#define CI_SIG_ROOM       65
#define CI_WIRE_UNSIGNED  (CI_WIRE_MAX - CI_SIG_ROOM)

/* How old an ask may be when it arrives (11.4's window), and how far ahead
 * of our clock it may claim to be (a phone's clock is not a GNSS clock). */
#define CI_FRESH_S        300ULL
#define CI_SKEW_S         120ULL

/* The answer to an ask, as section 11.1's codes. */
enum {
    CI_OK          = 200,
    CI_WAIT        = 0,    /* no answer yet: the asker's key is still on its way */
    CI_REFUSED     = 403,  /* unsigned, forged, out of range, scope too wide */
    CI_STALE       = 408,
    CI_AGAIN_LATER = 429,  /* already checked in today */
    CI_NO_PLACE    = 500   /* the station does not know where it is */
};

/* The station's rules. */
typedef struct {
    int local_only;        /* 1: in person only (Bluetooth, WiFi, LAN) */
} ci_rules_t;

/* What the core said about one heard t:command (the xprs.command row). */
typedef struct {
    char id[7];
    char from[16];
    char cmd[16];
    char scope[12];        /* the core's verdict: local|regional|global|... */
    int  in_range;         /* heard inside its scope's reach */
    int  for_us;
    char sig[12];          /* verified|forged|unverified|unsigned */
    char bearer[12];
    int  rssi;
    char via[64];
    unsigned long long ts; /* the ask's own ts:, epoch */
} ci_ask_t;

/* One certificate, read. */
typedef struct {
    char station[16];
    char to[16];
    char r[7];
    int  code;
    char scope[12];
    char link[12];
    char rssi[12];
    char route[64];
    char pos[28];
    char title[32];
    char file[64];
    unsigned long long ts;
} ci_cert_t;

/* Read an xprs.command (or xprs.result) row's facts into [a]. 1 when it is
 * a check-in ask: t:command, cmd:checkin. */
int ci_read_ask(const char *row, ci_ask_t *a);

/* Decide: 200, 403, 408 or 429. [checked_today] says the station already
 * certified this visitor today. A stranger's ask never comes here: the
 * caller only decides asks addressed to us. */
int ci_decide(const ci_ask_t *a, const ci_rules_t *r,
              unsigned long long now, int checked_today);

/* The ask: t:command f:<me> d:<station> ts: cmd:checkin scope:<local|regional>.
 * Length, or -1 when it would not fit signed. */
int ci_build_ask(char *out, unsigned cap, const char *me, const char *station,
                 unsigned long long now, const char *scope);

/* The certificate (code 200) or a refusal (any other code):
 *   t:result f:<me> d:<visitor> ts: r:<ask id> code: scope:<ask's>
 * and on a certificate link: and pos: (REQUIRED: a check-in is a meeting at
 * a place, and the logbook maps every one), route: when it came through
 * relays (REQUIRED then: it decides the grade), then as far as they fit
 * signed:
 *   file: title: rssi:
 * [file] [title] may be empty, [rssi] 0 for none. Length, or -1 when the head
 * does not fit, or for a certificate without a valid [pos]. */
int ci_build_result(char *out, unsigned cap, const char *me, const ci_ask_t *a,
                    unsigned long long now, int code, const char *file,
                    const char *title, const char *pos);

/* Read a heard t:result wire. 1 when it is one. */
int ci_read_cert(const char *wire, ci_cert_t *c);

/* The bearer word as a link: value (15.6.1): ble5 is ble; anything not on
 * the air or a local network gives "". */
void ci_link_of(const char *bearer, char *out, unsigned cap);

/* How close the meeting was: "local" (in person), "regional" (heard
 * directly over a radio) or "relayed" (through relays). */
const char *ci_grade(const char *link, const char *route);

/* A day number in the zone [offset_min] east of UTC, for "once a day". */
unsigned long long ci_day(unsigned long long epoch, int offset_min);

/* t:event f:<me> ts: title: tag:checkin [since:] until: [file:] [pos:] [m:]
 * (XPRS.md 26). since: is left out when the event starts at its ts:; file:,
 * pos: and m: go in that order as far as they fit signed, m: cut to the room
 * left. [title] is folded to a label (a-z 0-9 -). Length, or -1 when the
 * head does not fit. */
int ci_build_event(char *out, unsigned cap, const char *me,
                   unsigned long long now, const char *pos, const char *title,
                   unsigned long long since, unsigned long long until,
                   const char *file, const char *m);

/* t:file f:<me> ts: file:<ref> [size:<n>B] m:Check-in badge -- the badge,
 * described as a publication (XPRS.md 7.7.1), so the core advertises it and
 * places it with this station's archivers. [ref] is <digest>.<ext>. */
int ci_build_badge(char *out, unsigned cap, const char *me,
                   unsigned long long now, const char *ref,
                   unsigned long long size);

/* "lat,lon" in decimal degrees, latitude first (XPRS.md 15.1): 1 when it is
 * one and in range, with both times 1e7 into [lat7] and [lon7] (either may
 * be NULL). */
int ci_pos_parse(const char *pos, long *lat7, long *lon7);
int ci_pos_ok(const char *pos);

/* The same position written with four decimals (eleven metres), which is
 * what a station claims about where it stands. 0 when [in] is not one. */
int ci_pos_norm(const char *in, char *out, unsigned cap);

/* Fold free text to a label: lowercase, spaces to -, anything else dropped,
 * at most cap-1. Empty when nothing is left. */
void ci_label(char *out, unsigned cap, const char *text);

/* The <digest>.<ext> of a file: token or a bare reference; "" otherwise. */
void ci_ref_of(const char *token, char *out, unsigned cap);

#endif
