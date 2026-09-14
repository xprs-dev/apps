/*
 * wire.h -- strings, a wire's fields and the little JSON the host speaks.
 *
 * Pure: no HAL, so the native tests exercise exactly what the wapp runs.
 * The same helpers as firmwares/wire.c, the reading half only: Things sends
 * nothing on the air.
 */
#ifndef THINGS_WIRE_H
#define THINGS_WIRE_H

/* ── Strings, no libc ─────────────────────────────────────────────────── */
unsigned th_len(const char *s);
int      th_eq(const char *a, const char *b);
int      th_starts(const char *s, const char *prefix);
void     th_cpy(char *d, const char *s, unsigned cap);
void     th_cat(char *d, const char *s, unsigned cap);
void     th_cat_u(char *d, unsigned long long v, unsigned cap);
char     th_up(char c);
/* A decimal, leading digits only; 0 when there are none. */
unsigned long long th_num(const char *s);

/* ── A wire's fields ──────────────────────────────────────────────────── */
/* One field's value off a wire: up to the next space, or for m: to the end.
 * 1 when found. Also reads a "k:v k:v" list this wapp keeps. */
int th_field(const char *wire, const char *key, char *out, unsigned cap);
/* Append " key:value" to a "k:v k:v" list. */
void th_put(char *list, const char *key, const char *value, unsigned cap);

/* ── JSON ─────────────────────────────────────────────────────────────── */
/* Append [s] escaped for a JSON string. */
void th_jesc(char *d, const char *s, unsigned cap);
/* "key":<value> in flat JSON; strings unescaped just enough (\" \\ \n). */
int th_json(const char *json, const char *key, char *out, unsigned cap);
/* "key":{..}: the object's text, braces included, for th_json to read. */
int th_json_obj(const char *json, const char *key, char *out, unsigned cap);
/* "key":[ : where the array's elements start, NULL when absent. */
const char *th_json_arr(const char *json, const char *key);
/* A top-level array: where its elements start, NULL when [json] is not one. */
const char *th_json_top(const char *json);
/* Copy the object at [p] into [out] and return where the next one starts,
 * NULL at the end of the array. */
const char *th_json_next(const char *p, char *out, unsigned cap);
/* Copy the string at [p] (a top-level array of strings) into [out] and
 * return where the next one starts, NULL at the end. */
const char *th_json_next_str(const char *p, char *out, unsigned cap);

#endif
