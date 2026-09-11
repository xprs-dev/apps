/*
 * wire.h -- the packets of XPRS.md 11.9 and 11.10, built and read.
 *
 * Pure: no HAL, so the native tests exercise exactly what the wapp runs.
 * The wapp builds a wire UNSIGNED and the host signs it (hal_xprs_send adds
 * " sig:" and 60 characters), so every builder here refuses a wire longer
 * than FW_WIRE_UNSIGNED: past that, the signed packet would not fit the 250
 * bytes of section 4, and a command cut short is worse than one not sent.
 */
#ifndef FIRMWARES_WIRE_H
#define FIRMWARES_WIRE_H

#define FW_WIRE_MAX       250
#define FW_SIG_ROOM       65
#define FW_WIRE_UNSIGNED  (FW_WIRE_MAX - FW_SIG_ROOM)
/* A sealed ssid and pass share one command while the body is at most this
 * (11.10): 79 bytes seal to 128 characters of x:. */
#define FW_BODY_ONE_MAX   79

/* ── Strings, no libc ─────────────────────────────────────────────────── */
unsigned fw_len(const char *s);
int      fw_eq(const char *a, const char *b);
int      fw_starts(const char *s, const char *prefix);
void     fw_cpy(char *d, const char *s, unsigned cap);
void     fw_cat(char *d, const char *s, unsigned cap);
void     fw_cat_u(char *d, unsigned long long v, unsigned cap);
char     fw_up(char c);

/* ── Section 5 and section 4.8 ────────────────────────────────────────── */
/* The identifier: sha256 of the wire (which carries no sig:/via: when the
 * wapp built it), first six lowercase hex. */
void fw_id(const char *wire, unsigned len, char out[7]);
/* YYYY-MM-DD_HH:MM:SS, UTC. */
void fw_stamp(char *out, unsigned cap, unsigned long long epoch);

/* ── Reading ──────────────────────────────────────────────────────────── */
/* One field's value off a wire: up to the next space, or for m: to the end.
 * 1 when found. */
int fw_field(const char *wire, const char *key, char *out, unsigned cap);
/* The callsign a key derives to (section 3): the prefix, then the first
 * four characters after "npub1", uppercased. */
void fw_call_of(const char *npub, const char *prefix, char *out, unsigned cap);
/* Does [call] derive from [npub], whatever its prefix digit? */
int fw_call_matches(const char *call, const char *npub);

/* ── Building (unsigned; the host signs) ──────────────────────────────── */
/* Each returns the wire's length, or -1 when it would not fit signed. */
int fw_claim(char *out, unsigned cap, const char *me, const char *station,
             const char *ts, const char *my_npub);
int fw_set(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *fields);
int fw_sealed(char *out, unsigned cap, const char *me, const char *station,
              const char *ts, const char *x);
int fw_zdiag(char *out, unsigned cap, const char *me, const char *station,
             const char *ts);

/* A sealed body (11.4): `cmd:set` then one `key:value` line per field.
 * [kv] alternates key and value and ends with a NULL key. -1 when a value
 * holds a newline, which the line format cannot carry. */
int fw_body(char *out, unsigned cap, const char *const *kv);

/* ── JSON, the little of it the host speaks ───────────────────────────── */
/* Append [s] escaped for a JSON string. */
void fw_jesc(char *d, const char *s, unsigned cap);
/* "key":<value> in flat JSON; strings unescaped just enough (\" \\ \n). */
int fw_json(const char *json, const char *key, char *out, unsigned cap);

#endif
