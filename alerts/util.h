/*
 * util.h -- strings, the section 5 identifier, time and JSON, with no libc.
 * Pure: no HAL, so the native tests exercise exactly what the wapp runs.
 */
#ifndef ALERTS_UTIL_H
#define ALERTS_UTIL_H

unsigned al_len(const char *s);
int      al_eq(const char *a, const char *b);
int      al_starts(const char *s, const char *prefix);
void     al_cpy(char *d, const char *s, unsigned cap);
void     al_cat(char *d, const char *s, unsigned cap);
void     al_cat_u(char *d, unsigned long long v, unsigned cap);
char     al_up(char c);

/* The identifier: sha256 of the wire (which carries no sig:/via: when the
 * wapp built it), first six lowercase hex. */
void al_id(const char *wire, unsigned len, char out[7]);
/* YYYY-MM-DD_HH:MM:SS, UTC, and back. al_epoch is 0 for anything else. */
void al_stamp(char *out, unsigned cap, unsigned long long epoch);
unsigned long long al_epoch(const char *ts);

/* One field's value off a wire: up to the next space, or for m: to the end.
 * 1 when found. */
int al_field(const char *wire, const char *key, char *out, unsigned cap);

/* JSON, the little of it the host speaks. */
void al_jesc(char *d, const char *s, unsigned cap);
int  al_json(const char *json, const char *key, char *out, unsigned cap);
int  al_json_list(const char *json, const char *key, char *out, unsigned cap);
int  al_json_obj(const char *json, const char *key, char *out, unsigned cap);
int  al_json_nth(const char *json, const char *key, int n, char *out, unsigned cap);
const char *al_json_arr(const char *json, const char *key);
const char *al_json_next(const char *p, char *out, unsigned cap);

#endif
