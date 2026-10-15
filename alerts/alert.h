/*
 * alert.h -- the packets of XPRS.md 24, 25 and 25.3, built and read, and
 * what a receiver shows of them. Pure, like util.c.
 *
 * An alert is no new type: a t:warning (with sev:) or a t:info, posted with
 * scope:local and until:, and withdrawn by naming it (25.2) with the same
 * until: (25.3). The wapp builds a wire UNSIGNED and the host signs it, so a
 * builder refuses anything that would not fit once " sig:" and 60
 * characters are added.
 */
#ifndef ALERTS_ALERT_H
#define ALERTS_ALERT_H

#define AL_WIRE_MAX       250
#define AL_SIG_ROOM       65
#define AL_WIRE_UNSIGNED  (AL_WIRE_MAX - AL_SIG_ROOM)

/* A receiver's own end for a warning or notice that names none (25.1): a
 * day. Somebody else's packet with no until: is not a standing alert, and a
 * queue reported at eight is not still news at midnight. */
#define AL_DEFAULT_LIFE   86400ULL

typedef struct {
    char id[7];
    char type[8];                 /* warning | info */
    char from[16];
    char kind[12];
    char sev[8];                  /* a warning's; empty for a notice */
    char pos[28], rad[12];
    char scope[12];
    char m[160];
    unsigned long long ts, since, until;   /* epochs; until 0 = unstated */
    int  withdrawn;
    int  mine;
} al_alert_t;

/* What a row of the list is right now. */
enum { AL_GONE = 0, AL_PLANNED = 1, AL_NOW = 2 };

/* The durations the Post screen offers, in seconds: 1d 2d 1w 1m 6m 1y.
 * 0 for anything else. */
unsigned long long al_duration(const char *name);

/* Is [kind] one of section 24's (warning) or 25's (info) words? */
int al_kind_ok(const char *type, const char *kind);
/* Is [sev] one of watch, warning, danger? */
int al_sev_ok(const char *sev);

/* t:<type> f:<me> [pos: rad:] kind: [sev:] scope:local ts: until: [m:].
 * [pos] and [rad] may be empty. Returns the length, -1 when it would not fit
 * signed, -2 when a word is not the spec's. */
int al_build(char *out, unsigned cap, const char *me, const char *type,
             const char *kind, const char *sev, const char *pos,
             const char *rad, unsigned long long now,
             unsigned long long until, const char *m);

/* t:<type> f:<me> scope:local ts: until: r:<id> remove:<type> (25.2, 25.3). */
int al_build_withdraw(char *out, unsigned cap, const char *me,
                      const char *type, const char *id,
                      unsigned long long now, unsigned long long until);

/* Read a heard wire. 1: an alert, in [a], with a->id left for the caller
 * (the host's section 5 id, or al_id over a wire this wapp built). 2: a
 * withdrawal, the id it names in a->id and its author in a->from. 0:
 * neither. */
int al_parse(const char *wire, al_alert_t *a);

/* AL_NOW, AL_PLANNED (since: still ahead) or AL_GONE (withdrawn, or past
 * its until:, or past a day when it named none). [now] 0 is "no clock":
 * then nothing is gone by time. */
int al_state(const al_alert_t *a, unsigned long long now);

/* "3 days left", "2 hours left", "ended": the time to [until] from [now]. */
void al_left(char *out, unsigned cap, unsigned long long now,
             unsigned long long until);

/* Degrees times 1e7 as "38.7071", four places, which is eleven metres. */
void al_deg(char *out, unsigned cap, long v);

#endif
