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
 * Two sources, never this device's ears alone:
 *  - "who has this device heard?" (hal_xprs_stations): stations in earshot
 *    now and this hour on a radio or the LAN, then XPRS stations heard over
 *    Reticulum;
 *  - "who is there by name?" (hal_xprs_names): every person, station and
 *    group known by a verified t:identity, which the core also asks its
 *    archivers for (XPRS.md 12.9.4) -- so somebody, or a group, announced on
 *    the other side of the internet is found by name on a phone that never
 *    heard them.
 * Rows are sorted into three tabs by what the core says each callsign is:
 * "People" (a person), "Stations" (a mast, a vehicle, a device), "Groups".
 * A callsign is all a wapp needs (XPRS.md 3); the lane and the key are the
 * core's business.
 */
#ifndef PEOPLE_FINDER_H
#define PEOPLE_FINDER_H

#include <stdint.h>
#include "xprs_wasm_hal.h"

/* ── small self-contained helpers ──────────────────────────────────────── */

static unsigned pf_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static char pf_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
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
/* A callsign rather than an open group's name: the core's verdict
 * (hal_xprs_kind), never a prefix test of the finder's own. */
static int pf_is_station(const char *addr) {
  if (!addr || !addr[0]) return 0;
  char kind[32];   /* `foreign:meshtastic` is the longest the core writes */
  int32_t n = hal_xprs_kind(addr, pf_len(addr), kind, sizeof kind - 1);
  if (n <= 0) return 0;
  kind[n] = 0;
  return !(kind[0] == 'o' && kind[1] == 'p');   /* "open" */
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
/* tab: 0 People, 1 Stations, 2 Groups. local: 1 heard nearby, 0 heard over
 * Reticulum, -1 known by name only (hal_xprs_names). */
typedef struct { char call[24]; char seen[24]; char bearer[16]; char nick[40]; int local; int foreign; int tab; } pf_person;
static pf_person pf_people[PF_MAX];
static int pf_people_n;
/* Set by a caller whose picker can only take a station (an invitation to a
 * group, say): groups are left out of the candidates. Off by default. */
static int pf_hide_groups;
/* Set by a caller whose picker takes a PERSON only (an invitation to a closed
 * group: only a person can sign the acceptance). Stations, devices and other
 * networks' nodes are left out. Off by default. */
static int pf_people_only;
/* Off for a caller that wants only who this device heard. On by default:
 * a person or a group is found by name wherever it announced itself. */
static int pf_names_on = 1;

/* The section words. English by default; a wapp with translations sets them
 * (the tab VALUES stay "People", "Stations", "Groups": the host matches them
 * byte for byte against the screen's `tabs`). */
static const char *pf_t_nearby = "Nearby";
static const char *pf_t_rns = "On Reticulum";
static const char *pf_t_known = "Found by name";
static const char *pf_t_group = "Group";
static const char *pf_t_typed = "Callsign";
static const char *const pf_tabs[3] = { "People", "Stations", "Groups" };

/* Which tab a callsign belongs in, by the core's word for it. */
static int pf_tab_of(const char *call, const char *kind) {
  if (call[0] == 'X' && call[1] == '5') return 2;
  if (pf_streq(kind, "user")) return 0;
  if (kind[0]) return 1;
  /* No word from the stations table: ask the core's one rule. */
  char k[32];
  int32_t n = hal_xprs_kind(call, pf_len(call), k, sizeof k - 1);
  if (n <= 0) return 1;
  k[n] = 0;
  if (pf_streq(k, "user")) return 0;
  if (pf_streq(k, "closed")) return 2;
  return 1;
}

/* Rows the wapp drew itself above ours (pf_render_ex's first_sections):
 * a callsign there is not offered twice. */
static const char *pf_skip;

static int pf_listed(const char *call) {
  for (int i = 0; i < pf_people_n; i++) if (pf_streq(pf_people[i].call, call)) return 1;
  if (pf_skip && pf_skip[0]) {
    char id[40] = "\"go:"; pf_cat(id, sizeof id, call); pf_cat(id, sizeof id, "\"");
    char gid[40] = "\"go:#"; pf_cat(gid, sizeof gid, call); pf_cat(gid, sizeof gid, "\"");
    if (pf_find(pf_skip, id) || pf_find(pf_skip, gid)) return 1;
  }
  return 0;
}

/* Short age of an epoch-second time: "5m", "3h", "12d". */
static void pf_ago(uint64_t ts, char *out, unsigned cap) {
  out[0] = 0;
  uint64_t now = hal_time_epoch();
  if (!ts || ts > now + 60) return;
  uint64_t d = now > ts ? now - ts : 0;
  uint64_t v; char u;
  if (d < 3600) { v = d / 60; u = 'm'; }
  else if (d < 86400) { v = d / 3600; u = 'h'; }
  else { v = d / 86400; u = 'd'; }
  char num[24]; int k = 0;
  if (v == 0) num[k++] = '0';
  while (v && k < 20) { num[k++] = (char)('0' + v % 10); v /= 10; }
  unsigned o = 0;
  while (k && o < cap - 2) out[o++] = num[--k];
  out[o++] = u; out[o] = 0;
}

/* The people, stations and groups known by name (hal_xprs_names) that the
 * heard list did not already bring. [ask]: let the core ask its archivers
 * for names now (it bounds how often). */
static void pf_collect_names(const char *q, int ask) {
  static char nq[160], nb[16384];
  nq[0] = 0;
  pf_cat(nq, sizeof nq, "{\"limit\":60,\"ask\":");
  pf_cat(nq, sizeof nq, ask ? "true" : "false");
  pf_cat(nq, sizeof nq, ",\"q\":\"");
  pf_esc(nq, sizeof nq, q[0] == '#' ? q + 1 : q);
  pf_cat(nq, sizeof nq, "\"}");
  int n = hal_xprs_names(nq, pf_len(nq), nb, sizeof nb - 1);
  if (n <= 0 || n >= (int)sizeof nb) return;
  nb[n] = 0;
  const char *cur = nb; char row[400];
  while (pf_people_n < PF_MAX && pf_next_object(&cur, row, sizeof row)) {
    char call[24], kind[16], nick[40], ts[24];
    pf_jstr(row, "call", call, sizeof call);
    pf_jstr(row, "kind", kind, sizeof kind);
    if (!call[0] || pf_listed(call) || pf_is_self(call)) continue;
    int tab = pf_streq(kind, "group") ? 2 : pf_streq(kind, "user") ? 0 : 1;
    if (pf_hide_groups && tab == 2) continue;
    if (pf_people_only && tab != 0) continue;
    pf_jstr(row, "nick", nick, sizeof nick);
    /* "ts":12345 is a number: read the digits after it. */
    uint64_t t = 0;
    const char *tp = pf_find(row, "\"ts\":");
    if (tp) { tp += 5; while (*tp >= '0' && *tp <= '9') t = t * 10 + (uint64_t)(*tp++ - '0'); }
    (void)ts;
    pf_person *e = &pf_people[pf_people_n++];
    unsigned k = 0; for (; call[k] && k < sizeof e->call - 1; k++) e->call[k] = call[k];
    e->call[k] = 0;
    k = 0; for (; nick[k] && k < sizeof e->nick - 1; k++) e->nick[k] = nick[k];
    e->nick[k] = 0;
    e->seen[0] = 0;
    pf_ago(t, e->seen, sizeof e->seen);
    e->bearer[0] = 0;
    e->foreign = 0;
    e->local = -1;
    e->tab = tab;
  }
}

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
      /* The core names what a callsign is (`kind`, XPRS.md 3), so a row that
       * carries one is somebody to talk to whatever its prefix: a Meshtastic
       * node (MTA1B2C3D4, 3.2) has no digit where a licence has one. The
       * prefix test stays for a row the core gave no kind, a group. */
      char kind[12]; pf_jstr(row, "kind", kind, sizeof kind);
      if (!call[0] || (!kind[0] && !pf_is_station(call)) || pf_is_self(call)) continue;
      if (pf_hide_groups && call[0] == 'X' && call[1] == '5') continue;
      if (pf_people_only && !pf_streq(kind, "user")) continue;
      if (want[0]) {
        char up[24]; unsigned k = 0;
        for (; call[k] && k < sizeof up - 1; k++) up[k] = pf_up(call[k]);
        up[k] = 0;
        char nk[24] = ""; pf_jstr(row, "nick", nk, sizeof nk);
        for (k = 0; nk[k]; k++) nk[k] = pf_up(nk[k]);
        if (!pf_find(up, want) && !(nk[0] && pf_find(nk, want))) continue;
      }
      int dup = 0;
      for (int i = 0; i < pf_people_n; i++) if (pf_streq(pf_people[i].call, call)) { dup = 1; break; }
      if (dup || pf_people_n >= PF_MAX) continue;
      pf_person *e = &pf_people[pf_people_n++];
      unsigned k = 0; for (; call[k] && k < sizeof e->call - 1; k++) e->call[k] = call[k];
      e->call[k] = 0;
      pf_jarr(row, "tags", 0, e->seen, sizeof e->seen);
      pf_jarr(row, "tags", 1, e->bearer, sizeof e->bearer);
      /* The name a person gave themselves (their verified t:identity), or a
       * node of another network's, relayed by a gateway (6.3.1). */
      pf_jstr(row, "nick", e->nick, sizeof e->nick);
      e->foreign = pf_streq(kind, "foreign");
      e->local = local;
      e->tab = pf_tab_of(call, kind);
    }
    section++;
  }
}
static void pf_row(char *o, unsigned sz, const pf_person *e) {
  int grp = e->call[0] == 'X' && e->call[1] == '5';
  pf_cat(o, sz, "{\"id\":\"go:"); if (grp) pf_cat(o, sz, "#"); pf_esc(o, sz, e->call);
  /* A person's own name leads, the callsign beside it: the name is what
   * people know each other by, the callsign what identifies them (3). */
  pf_cat(o, sz, "\",\"title\":\"");
  if (e->nick[0] && !e->foreign) { pf_esc(o, sz, e->nick); pf_cat(o, sz, " ("); pf_esc(o, sz, e->call); pf_cat(o, sz, ")"); }
  else pf_esc(o, sz, e->call);
  pf_cat(o, sz, "\",\"subtitle\":\"");
  if (e->local < 0) {
    /* Known by name: what it is, and when it last announced itself. */
    if (grp) pf_esc(o, sz, pf_t_group);
    if (e->seen[0]) { if (grp) pf_cat(o, sz, " - "); pf_esc(o, sz, e->seen); }
  } else if (e->seen[0]) pf_esc(o, sz, e->seen); else pf_cat(o, sz, "heard");
  if (e->bearer[0]) { pf_cat(o, sz, " - "); pf_esc(o, sz, e->bearer); }
  if (e->nick[0] && e->foreign) { pf_cat(o, sz, " - "); pf_esc(o, sz, e->nick); }
  pf_cat(o, sz, "\",\"icon\":\"");
  pf_cat(o, sz, grp ? "tag" : e->tab == 1 ? "cell_tower" : "person");
  pf_cat(o, sz, "\"}");
}

/* ── the two public entry points ───────────────────────────────────────── */

/* Emit the ui.people.set panel for [field], filtered by [query]. When
 * [allow_group] and the query starts "#", a "Group" row opens that group. */
static void pf_render_ex(const char *field, const char *q, int allow_group, const char *first_sections);
static __attribute__((unused)) void pf_render(const char *field, const char *q, int allow_group) {
  pf_render_ex(field, q, allow_group, 0);
}

/* pf_render with [first_sections] (one or more `{"title":..,"items":[..]}`
 * objects, comma-separated, or empty) drawn above the heard stations: what the
 * wapp itself knows matches the query, its own conversations say. */
static __attribute__((unused)) void pf_render_ex(const char *field, const char *q, int allow_group,
                                                 const char *first_sections) {
  static char o[32768]; const unsigned sz = sizeof o;
  char want[24] = ""; int j = 0;
  for (int i = 0; q[i] && j < 23; i++) if (q[i] != ' ') want[j++] = pf_up(q[i]);
  want[j] = 0;
  pf_skip = first_sections;
  pf_collect(q);
  if (pf_names_on) pf_collect_names(q, 1);
  o[0] = 0;
  pf_cat(o, sz, "{\"type\":\"ui.people.set\",\"field\":\"");
  pf_cat(o, sz, field);
  pf_cat(o, sz, "\",\"sections\":[");
  int first_section = 1;
  if (first_sections && first_sections[0]) {
    pf_cat(o, sz, first_sections);
    first_section = 0;
  }
  if (allow_group && want[0] == '#' && want[1]) {
    if (!first_section) pf_cat(o, sz, ",");
    pf_cat(o, sz, "{\"title\":\""); pf_esc(o, sz, pf_t_group);
    pf_cat(o, sz, "\",\"tab\":\"Groups\",\"items\":[{\"id\":\"go:"); pf_esc(o, sz, want);
    pf_cat(o, sz, "\",\"title\":\""); pf_esc(o, sz, want);
    pf_cat(o, sz, "\",\"subtitle\":\"Open this group\",\"icon\":\"tag\"}]}");
    first_section = 0;
  }
  /* A callsign typed whole that nobody listed: offered in the tab it belongs
   * to, as somebody not heard yet (or a closed group, opened by its X5). */
  int typed = want[0] && want[0] != '#' && pf_is_station(want) && !pf_is_self(want) &&
              !pf_listed(want);
  int typed_tab = typed ? pf_tab_of(want, "") : -1;
  if (typed && pf_people_only && typed_tab != 0) typed = 0;
  if (typed && pf_hide_groups && typed_tab == 2) typed = 0;
  static const int passes[4] = { 1, 0, -1, -2 };
  for (int tab = 0; tab < 3; tab++) {
    for (int pi = 0; pi < 4; pi++) {
      int pass = passes[pi];
      int any = 0;
      for (int i = 0; i < pf_people_n; i++)
        if (pf_people[i].local == pass && pf_people[i].tab == tab) { any = 1; break; }
      /* pass -2: the callsign typed whole, in a section of its own. */
      int with_typed = typed && typed_tab == tab && pass == -2;
      if (!any && !with_typed) continue;
      if (!first_section) pf_cat(o, sz, ",");
      first_section = 0;
      pf_cat(o, sz, "{\"title\":\"");
      pf_esc(o, sz, pass == 1 ? pf_t_nearby : pass == 0 ? pf_t_rns
                    : pass == -1 ? pf_t_known : pf_t_typed);
      pf_cat(o, sz, "\",\"tab\":\""); pf_cat(o, sz, pf_tabs[tab]);
      pf_cat(o, sz, "\",\"items\":[");
      int first = 1;
      for (int i = 0; i < pf_people_n; i++) {
        if (pf_people[i].local != pass || pf_people[i].tab != tab) continue;
        if (!first) pf_cat(o, sz, ",");
        first = 0;
        pf_row(o, sz, &pf_people[i]);
      }
      if (with_typed) {
        if (!first) pf_cat(o, sz, ",");
        int grp = tab == 2;
        pf_cat(o, sz, "{\"id\":\"go:"); if (grp) pf_cat(o, sz, "#"); pf_esc(o, sz, want);
        pf_cat(o, sz, "\",\"title\":\""); pf_esc(o, sz, want);
        pf_cat(o, sz, grp ? "\",\"subtitle\":\"Closed group\",\"icon\":\"tag\"}"
                          : "\",\"subtitle\":\"Not heard yet\",\"icon\":\"person_add\"}");
      }
      pf_cat(o, sz, "]}");
    }
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
