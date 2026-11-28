/*
 * wire.h -- the packets of XPRS.md 11.9 and 11.10, built and read.
 *
 * Pure: no HAL, so the native tests exercise exactly what the wapp runs.
 * The wapp builds a wire UNSIGNED and the host signs it (hal_xprs_send adds
 * " sig:" and 60 characters), so every builder here refuses a wire longer
 * than FW_WIRE_UNSIGNED: past that, the signed packet would not fit the 250
 * bytes of section 4, and a command cut short is worse than one not sent.
 */
#ifndef DEVICES_WIRE_H
#define DEVICES_WIRE_H

#define FW_WIRE_MAX       250
#define FW_SIG_ROOM       65
#define FW_WIRE_UNSIGNED  (FW_WIRE_MAX - FW_SIG_ROOM)
/* A sealed ssid and pass share one command while the body is at most this
 * (11.10): 79 bytes seal to 128 characters of x:. */
#define FW_BODY_ONE_MAX   79

/* ── Strings, no libc ─────────────────────────────────────────────────── */
unsigned dv_len(const char *s);
int      dv_eq(const char *a, const char *b);
int      dv_starts(const char *s, const char *prefix);
void     dv_cpy(char *d, const char *s, unsigned cap);
void     dv_cat(char *d, const char *s, unsigned cap);
void     dv_cat_u(char *d, unsigned long long v, unsigned cap);
char     dv_up(char c);

/* ── Section 5 and section 4.8 ────────────────────────────────────────── */
/* The identifier: sha256 of the wire (which carries no sig:/via: when the
 * wapp built it), first six lowercase hex. */
void dv_id(const char *wire, unsigned len, char out[7]);
/* YYYY-MM-DD_HH:MM:SS, UTC. */
void dv_stamp(char *out, unsigned cap, unsigned long long epoch);

/* ── Reading ──────────────────────────────────────────────────────────── */
/* One field's value off a wire: up to the next space, or for m: to the end.
 * 1 when found. */
int dv_field(const char *wire, const char *key, char *out, unsigned cap);
/* The callsign a key derives to (section 3): the prefix, then the first
 * four characters after "npub1", uppercased (what a station names itself). */
void dv_call_of(const char *npub, const char *prefix, char *out, unsigned cap);
/* Does [call] derive from [npub], whatever its prefix digit? A callsign
 * takes two to five characters of the key, and says itself how many. */
int dv_call_matches(const char *call, const char *npub);

/* ── Building (unsigned; the host signs) ──────────────────────────────── */
/* Each returns the wire's length, or -1 when it would not fit signed. */
int dv_claim(char *out, unsigned cap, const char *me, const char *station,
             const char *ts, const char *my_npub);
int dv_set(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *fields);
int dv_sealed(char *out, unsigned cap, const char *me, const char *station,
              const char *ts, const char *x);
int dv_zdiag(char *out, unsigned cap, const char *me, const char *station,
             const char *ts);
/* Any other command word: "cmd:zcore", "cmd:update". */
int dv_cmd(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *cmd);
/* A t:request with q:<what> to the station: "policy", "mail" (11.9, 9.12.3). */
int dv_ask(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *what);

/* A sealed body (11.4): `cmd:set` then one `key:value` line per field.
 * [kv] alternates key and value and ends with a NULL key. -1 when a value
 * holds a newline, which the line format cannot carry. */
int dv_body(char *out, unsigned cap, const char *const *kv);

/* Order two firmware versions: <0 when [a] is older than [b], 0 equal, >0
 * newer. Dotted numbers compare as numbers (0.10.0 is after 0.9.1), and a
 * pre-release ranks below its release (0.6.0-beta.9 < 0.6.0); two
 * pre-releases of one version compare by their last number. An empty
 * version is older than any other. */
int dv_ver_cmp(const char *a, const char *b);

/* ── JSON, the little of it the host speaks ───────────────────────────── */
/* Append [s] escaped for a JSON string. */
void dv_jesc(char *d, const char *s, unsigned cap);
/* "key":<value> in flat JSON; strings unescaped just enough (\" \\ \n). */
int dv_json(const char *json, const char *key, char *out, unsigned cap);
/* "key":[..] in flat JSON: the elements joined by ", ", quotes dropped. */
int dv_json_list(const char *json, const char *key, char *out, unsigned cap);
/* "key":{..}: the object's text, braces included, for dv_json to read. */
int dv_json_obj(const char *json, const char *key, char *out, unsigned cap);
/* "key":[{..},{..}]: the n-th object (from 0), braces included. 0 when
 * there is no such element. */
int dv_json_nth(const char *json, const char *key, int n, char *out, unsigned cap);
/* Walk "key":[{..},{..}] once: dv_json_arr finds the array (NULL when
 * absent), dv_json_next copies the object at [p] into [out] and returns
 * where the next one starts, NULL at the end. Linear in the array, where
 * dv_json_nth from 0 to n is quadratic. */
const char *dv_json_arr(const char *json, const char *key);
const char *dv_json_next(const char *p, char *out, unsigned cap);

/* ── The reading half Things brought ──────────────────────────────────── */
/* A decimal, leading digits only; 0 when there are none. */
unsigned long long dv_num(const char *s);
/* Append " key:value" to a "k:v k:v" list. */
void dv_put(char *list, const char *key, const char *value, unsigned cap);
/* A top-level array: where its elements start, NULL when [json] is not one. */
const char *dv_json_top(const char *json);
/* Copy the string at [p] (a top-level array of strings) into [out] and
 * return where the next one starts, NULL at the end. */
const char *dv_json_next_str(const char *p, char *out, unsigned cap);
/* Standard base64 with padding, null-terminated. Returns the characters
 * written, or 0 when [cap] cannot hold the lot (nothing is written then). */
unsigned dv_b64(const unsigned char *in, unsigned n, char *out, unsigned cap);
/* The other way: decode [in] until its end or a character that is not
 * base64. Returns the bytes written, 0 when they would not fit. */
unsigned dv_b64_dec(const char *in, unsigned char *out, unsigned cap);

#endif
