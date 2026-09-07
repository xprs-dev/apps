/*
 * people_finder.h — a generic "find a callsign" picker for any wapp.
 *
 * Lifted from the chat wapp's New-chat / Search screens and made
 * self-contained so every wapp can reuse it: it depends ONLY on the HAL
 * (hal_xprs_stations for the candidates, hal_msg_send for the panel,
 * hal_identity to hide our own callsign) and carries its own small string and
 * JSON helpers, all `static` and `pf_`-prefixed so it never clashes with a
 * wapp's own. Include it once (from your main .c) and:
 *
 *   pf_render(field, query, allow_group);   // emit the ui.people.set panel
 *   char call[24];
 *   if (pf_pick(msg, field, call, sizeof call)) { ... }  // decode a tap
 *
 * The host renders a `$type:"people"` field generically: a tap sends
 * `<field>_tap` with `<field>_id`, a search sends `<field>_search` with
 * `<field>_query`. The row id is `go:<CALL>` (or `go:#<X5group>`); pf_pick
 * strips the `go:` for you and returns the bare callsign or `#group`.
 *
 * The candidate list is "who has this device heard?" — the one table the core
 * already keeps (hal_xprs_stations): stations in earshot now and this hour on
 * a radio or the LAN, then XPRS stations heard over Reticulum. A callsign is
 * all a wapp needs (XPRS.md 3); the lane and the key are the core's business.
 */
#ifndef PEOPLE_FINDER_H
#define PEOPLE_FINDER_H

#include <stdint.h>
#include "xprs_wasm_hal.h"

/* ── small self-contained helpers ──────────────────────────────────────── */

static unsigned pf_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static char pf_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int pf_digit(char c) { return c >= '0' && c <= '9'; }
static int pf_streq(const char *a, const char *b) {
  while (*a && *b) { if (*a != *b) return 0; a++; b++; }
  return *a == *b;
}
static int pf_pre(const char *s, const char *pre) {
  while (*pre) { if (*s != *pre) return 0; s++; pre++; }
  return 1;
}
static const char *pf_find(const char *hay, const char *needle) {
  for (const char *p = hay; *p; p++) if (pf_pre(p, needle)) return p;
  return 0;
}
/* Bounded append; leaves [o] NUL-terminated. */
static void pf_cat(char *o, unsigned sz, const char *s) {
  unsigned n = pf_len(o);
  while (*s && n < sz - 1) o[n++] = *s++;
  o[n] = 0;
}
/* Append [s] JSON-string-escaped (quote, backslash, control -> \uXXXX). */
static void pf_esc(char *o, unsigned sz, const char *s) {
  unsigned n = pf_len(o);
  for (; *s && n < sz - 7; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
    else if (c == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
    else if (c < 0x20) {
      static const char *h = "0123456789abcdef";
      o[n++] = '\\'; o[n++] = 'u'; o[n++] = '0'; o[n++] = '0';
      o[n++] = h[(c >> 4) & 0xf]; o[n++] = h[c & 0xf];
    } else o[n++] = (char)c;
  }
  o[n] = 0;
}
/* "key":"value" out of one flat object. */
static int pf_jstr(const char *obj, const char *key, char *out, unsigned cap) {
  out[0] = 0;
  char pat[40] = "\"";
  pf_cat(pat, sizeof pat, key); pf_cat(pat, sizeof pat, "\":\"");
  const char *p = pf_find(obj, pat);
  if (!p) return 0;
  p += pf_len(pat);
  unsigned o = 0;
  while (*p && *p != '"' && o < cap - 1) {
    if (*p == '\\' && p[1]) p++;
    out[o++] = *p++;
  }
  out[o] = 0;
  return 1;
}
/* The [idx]-th string of "key":["a","b",...] . */
static int pf_jarr(const char *obj, const char *key, int idx, char *out, unsigned cap) {
  out[0] = 0;
  char pat[40] = "\"";
  pf_cat(pat, sizeof pat, key); pf_cat(pat, sizeof pat, "\":[");
  const char *p = pf_find(obj, pat);
  if (!p) return 0;
  p += pf_len(pat);
  for (int i = 0;; i++) {
    while (*p == ' ' || *p == ',') p++;
    if (*p != '"') return 0;
    p++;
    unsigned o = 0;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) p++;
      if (i == idx && o < cap - 1) out[o++] = *p;
      p++;
    }
    if (*p == '"') p++;
    if (i == idx) { out[o] = 0; return 1; }
  }
}
/* Copy the next {...} object from an array cursor, tracking brace depth and
 * skipping braces inside strings. Advances [*cur] past it. */
static int pf_next_object(const char **cur, char *out, unsigned cap) {
  const char *p = *cur;
  while (*p && *p != '{') p++;
  if (*p != '{') return 0;
  int depth = 0, instr = 0;
  unsigned o = 0;
  for (; *p; p++) {
    char c = *p;
    if (o < cap - 1) out[o++] = c;
    if (instr) { if (c == '\\' && p[1]) { if (o < cap - 1) out[o++] = *++p; } else if (c == '"') instr = 0; continue; }
    if (c == '"') instr = 1;
    else if (c == '{') depth++;
    else if (c == '}') { if (--depth == 0) { p++; break; } }
  }
  out[o] = 0;
  *cur = p;
  return 1;
}
/* Does an XPRS address name a station rather than a group (XPRS.md 6.3)?
 * The exact rule the chat wapp uses, so the candidate filter is unchanged. */
static int pf_is_station(const char *addr) {
  if (!addr || !addr[0]) return 0;
  unsigned n = pf_len(addr);
  if (n >= 6 && addr[0] == 'X' &&
      (addr[1] == '1' || addr[1] == '3' || addr[1] == '5')) return 1;
  for (unsigned i = 0; i < n; i++) if (addr[i] == '-') return 1;
  for (unsigned i = 1; i < n && i < 3; i++) if (pf_digit(addr[i])) return 1;
  return 0;
}
static int pf_is_self(const char *call) {
  char me[24]; uint32_t n = hal_identity(me, sizeof me - 1);
  if (n == 0 || n >= sizeof me) return 0;
  me[n] = 0;
  return pf_streq(call, me);
}

/* ── the candidates ────────────────────────────────────────────────────── */

#ifndef PF_MAX
#define PF_MAX 128
#endif
typedef struct { char call[24]; char seen[24]; char bearer[12]; int local; } pf_person;
static pf_person pf_people[PF_MAX];
static int pf_people_n;

static void pf_collect(const char *q) {
  pf_people_n = 0;
  char want[24] = ""; int j = 0;
  for (int i = 0; q[i] && j < 23; i++) if (q[i] != ' ') want[j++] = pf_up(q[i]);
  want[j] = 0;
  static char st[16384];
  int n = hal_xprs_stations(st, sizeof st - 1);
  if (n <= 0) return;
  st[n] = 0;
  const char *p = st; int section = 0;
  while ((p = pf_find(p, "\"title\":\"")) != 0 && section < 3) {
    int local = !pf_pre(p + 9, "On Reticulum");
    p = pf_find(p, "\"items\":[");
    if (!p) break;
    p += 9;
    char row[600];
    while (*p) {
      while (*p == ' ' || *p == ',') p++;
      if (*p == ']' || !*p) break;
      const char *cur = p;
      if (!pf_next_object(&cur, row, sizeof row)) break;
      p = cur;
      char call[24]; pf_jstr(row, "id", call, sizeof call);
      if (!call[0] || !pf_is_station(call) || pf_is_self(call)) continue;
      if (want[0]) {
        char up[24]; unsigned k = 0;
        for (; call[k] && k < sizeof up - 1; k++) up[k] = pf_up(call[k]);
        up[k] = 0;
        if (!pf_find(up, want)) continue;
      }
      int dup = 0;
      for (int i = 0; i < pf_people_n; i++) if (pf_streq(pf_people[i].call, call)) { dup = 1; break; }
      if (dup || pf_people_n >= PF_MAX) continue;
      pf_person *e = &pf_people[pf_people_n++];
      unsigned k = 0; for (; call[k] && k < sizeof e->call - 1; k++) e->call[k] = call[k];
      e->call[k] = 0;
      pf_jarr(row, "tags", 0, e->seen, sizeof e->seen);
      pf_jarr(row, "tags", 1, e->bearer, sizeof e->bearer);
      e->local = local;
    }
    section++;
  }
}
static void pf_row(char *o, unsigned sz, const pf_person *e) {
  int grp = e->call[0] == 'X' && e->call[1] == '5';
  pf_cat(o, sz, "{\"id\":\"go:"); if (grp) pf_cat(o, sz, "#"); pf_esc(o, sz, e->call);
  pf_cat(o, sz, "\",\"title\":\""); pf_esc(o, sz, e->call);
  pf_cat(o, sz, "\",\"subtitle\":\"");
  if (e->seen[0]) pf_esc(o, sz, e->seen); else pf_cat(o, sz, "heard");
  if (e->bearer[0]) { pf_cat(o, sz, " - "); pf_esc(o, sz, e->bearer); }
  pf_cat(o, sz, "\",\"icon\":\""); pf_cat(o, sz, grp ? "tag" : "person");
  pf_cat(o, sz, "\"}");
}

/* ── the two public entry points ───────────────────────────────────────── */

/* Emit the ui.people.set panel for [field], filtered by [query]. When
 * [allow_group] and the query starts "#", a "Group" row opens that group. */
static __attribute__((unused)) void pf_render(const char *field, const char *q, int allow_group) {
  static char o[16384]; const unsigned sz = sizeof o;
  char want[24] = ""; int j = 0;
  for (int i = 0; q[i] && j < 23; i++) if (q[i] != ' ') want[j++] = pf_up(q[i]);
  want[j] = 0;
  pf_collect(q);
  o[0] = 0;
  pf_cat(o, sz, "{\"type\":\"ui.people.set\",\"field\":\"");
  pf_cat(o, sz, field);
  pf_cat(o, sz, "\",\"sections\":[");
  int first_section = 1;
  if (allow_group && want[0] == '#' && want[1]) {
    pf_cat(o, sz, "{\"title\":\"Group\",\"items\":[{\"id\":\"go:"); pf_esc(o, sz, want);
    pf_cat(o, sz, "\",\"title\":\""); pf_esc(o, sz, want);
    pf_cat(o, sz, "\",\"subtitle\":\"Open this group\",\"icon\":\"tag\"}]}");
    first_section = 0;
  }
  for (int pass = 1; pass >= 0; pass--) {
    int any = 0;
    for (int i = 0; i < pf_people_n; i++) if (pf_people[i].local == pass) { any = 1; break; }
    int typed = pass == 0 && want[0] != '#' && pf_is_station(want) && !pf_is_self(want);
    if (typed) { for (int i = 0; i < pf_people_n; i++) if (pf_streq(pf_people[i].call, want)) typed = 0; }
    if (!any && !typed) continue;
    if (!first_section) pf_cat(o, sz, ",");
    first_section = 0;
    pf_cat(o, sz, pass ? "{\"title\":\"Nearby\",\"items\":[" : "{\"title\":\"On Reticulum\",\"items\":[");
    int first = 1;
    for (int i = 0; i < pf_people_n; i++) {
      if (pf_people[i].local != pass) continue;
      if (!first) pf_cat(o, sz, ",");
      first = 0;
      pf_row(o, sz, &pf_people[i]);
    }
    if (typed) {
      if (!first) pf_cat(o, sz, ",");
      pf_cat(o, sz, "{\"id\":\"go:"); pf_esc(o, sz, want);
      pf_cat(o, sz, "\",\"title\":\""); pf_esc(o, sz, want);
      pf_cat(o, sz, "\",\"subtitle\":\"Not heard yet\",\"icon\":\"person_add\"}");
    }
    pf_cat(o, sz, "]}");
  }
  pf_cat(o, sz, "]}");
  hal_msg_send(o, pf_len(o));
}

/* Decode a tap: read "<field>_id" out of the host message [buf], require the
 * "go:" prefix and strip it, and write the bare target (a callsign, or
 * "#<group>") into [out]. Returns 1 on success. */
static __attribute__((unused)) int pf_pick(const char *buf, const char *field, char *out, unsigned cap) {
  out[0] = 0;
  char key[40]; key[0] = 0;
  pf_cat(key, sizeof key, field); pf_cat(key, sizeof key, "_id");
  char id[64];
  if (!pf_jstr(buf, key, id, sizeof id) || !pf_pre(id, "go:")) return 0;
  const char *t = id + 3;
  unsigned o = 0;
  while (*t && o < cap - 1) out[o++] = *t++;
  out[o] = 0;
  return out[0] != 0;
}

#endif /* PEOPLE_FINDER_H */
