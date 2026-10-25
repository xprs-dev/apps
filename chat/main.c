/*
 * chat — conversations over XPRS, and nothing else.
 *
 * The rooms by range (XPRS.md 9.11: "#LOCAL", "#REGIONAL", "#GLOBAL"), an
 * open group (6.3, "#NAME"), a
 * closed group (26, "#X5ABCD") and a 1:1 with a station ("X16JK8"). Replies,
 * hearts, blocking, hiding, read receipts, the tick on a bubble.
 *
 * ── ONE DOOR IN, ONE STORE PER ROOM ─────────────────────────────────────
 *
 * Every message -- heard live, sent by us, replayed from the core's archive,
 * or a system note -- enters through room_admit() in room.c, which decides
 * once whether it is blocked, hidden or already held, writes it to that
 * room's own sqlite database, and paints the host's view from what it wrote.
 * The host draws; it remembers nothing. "Already held" is the message id
 * being a primary key. There is no seen-ring, no membership list in RAM and
 * no key in KV: the database is the only memory this wapp has, and it is
 * the same one whichever engine (the page's or the headless one) is running.
 *
 * The three rooms by range exist by default. Any other room appears when a
 * message arrives for it or the user starts one.
 *
 * ── THIS WAPP OWNS NO TRANSPORT ─────────────────────────────────────────
 *
 * It says what it wants said and to whom:
 *
 *     hal_xprs_broadcast(text, scope, reply_to, &id)     a room by range
 *     hal_xprs_message(to, text, private, &id)           a 1:1
 *     hal_xprs_send(wire)                                a composed packet
 *     hal_xprs_read(id)                                  a person read it
 *
 * and the core composes, seals (9.2), signs (9.1), splits (6.6), ranks the
 * bearers (36.0), parks a custody copy and reports back. What arrives comes
 * on the event bus, reassembled, unsealed, verified, with its section 5
 * identifier, and only for the packet types this wapp subscribed to.
 *
 * There is no clock. module_tick is empty and the interval is 0: everything
 * here starts with something happening -- a packet from the core, or a
 * person typing.
 */
#include <stdint.h>
#include "xprs_wasm_hal.h"
#include "../sdk/wapp_i18n.h"
#include "db.h"
#include "room.h"
#include "thread.h"
#include "xprs.h"
#include "people_finder.h"

/* The rooms by range (XPRS.md 9.11): undirected messages, filed by the scope
 * their author wrote -- never by the bearer that delivered them, which is the
 * core's business. Local is whoever is in earshot of Bluetooth, ESP-NOW or
 * the LAN; Regional adds LoRa and radio; Global goes anywhere. A packet the
 * core marks out of range (inRange false: heard on a bearer wider than its
 * scope) is in none of them. */
#define XROOM_LOCAL "#LOCAL"
#define XROOM_REGIONAL "#REGIONAL"
#define XROOM_GLOBAL "#GLOBAL"

/* ── state ──────────────────────────────────────────────────────────── */
static char g_call[16] = "N0CALL";   /* replaced at init by hal_identity() */
static int  g_chan_local = 1;        /* Settings: the Local room switch (KV "chan") */
static char g_redact_text[900];      /* marked ((..)) text awaiting a compose passphrase (9.2.1) */
static const char *s_find(const char *hay, const char *needle);
static uint64_t g_xroom_fill_at;     /* epoch of the last archive refill */

static int is_self_call(const char *c) {
  if (!c || !c[0]) return 0;
  for (int i = 0; g_call[i] || c[i]; i++)
    if (s_up(g_call[i]) != s_up(c[i])) return 0;
  return 1;
}
static int xroom_is(const char *id) {
  return s_eq(id, XROOM_LOCAL) || s_eq(id, XROOM_REGIONAL) || s_eq(id, XROOM_GLOBAL);
}
/* The room a scope files into, as the core states it (local, regional,
 * global, country); an unknown word is in no room. */
static const char *xroom_of(const char *scope) {
  if (s_eq(scope, "local")) return XROOM_LOCAL;
  if (s_eq(scope, "regional")) return XROOM_REGIONAL;
  if (s_eq(scope, "global") || s_eq(scope, "country") || !scope[0]) return XROOM_GLOBAL;
  return 0;
}
/* And the scope a room says things with. */
static const char *xroom_scope(const char *id) {
  if (s_eq(id, XROOM_LOCAL)) return "local";
  if (s_eq(id, XROOM_REGIONAL)) return "regional";
  return "global";
}

static void notify(const char *level, const char *body) {
  char m[300] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Chat\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void screen_open(const char *name) {
  char m[100] = "{\"type\":\"ui.screen.open\",\"name\":\"";
  s_cat(m, name, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void screen_close(void) {
  const char *m = "{\"type\":\"ui.screen.close\"}";
  hal_msg_send(m, s_len(m));
}
/* Tell the host's profile panel whether we block [call]. */
static void host_block_state(const char *call, int on) {
  char m[120] = "{\"type\":\"social.blockstate\",\"callsign\":\"";
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"on\":", sizeof(m));
  s_cat(m, on ? "true}" : "false}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* The one key left in KV: the Local room switch, which the host's settings
 * form drives through chan_apply. Everything else this wapp remembers is in
 * its databases. */
static void chan_save(void) { hal_kv_set("chan", 4, g_chan_local ? "1" : "0", 1); }
static void chan_load(void) {
  char b[4];
  if (hal_kv_get("chan", 4, b, 3) >= 1) g_chan_local = b[0] == '1';
}

/* ── Closed groups (26): the roster is the core's ─────────────────────
 *
 * A closed group's id here is "#" + its X5 callsign. Membership is a replay
 * of signed acts against a key map the host owns; hal_xprs_groups hands the
 * answer over and this wapp only renders it and refuses to compose where it
 * has no standing. It does NOT create a room per group: a room appears when
 * somebody speaks in it or the user opens it. */
#define XGROUP_MAX 48
/* `parent`: the group that lists this one as a folder (10.2), "" for a root.
 * The tree is the core's answer (hal_xprs_groups), five levels and no cycles
 * already applied; this wapp only draws it. */
static struct { char call[8]; char nick[24]; char role[8]; char parent[8]; } g_xgroup[XGROUP_MAX];
static int g_xgroup_n;

static int xgroup_is(const char *id) {
  return id[0] == '#' && id[1] == 'X' && id[2] == '5' && s_len(id) == 7;
}
static int xgroup_find(const char *call) {
  for (int i = 0; i < g_xgroup_n; i++) if (s_eq(g_xgroup[i].call, call)) return i;
  return -1;
}
/* 26.3.1: a grant confers nothing until the person accepts it. */
static int xgroup_may_post(const char *call) {
  int i = xgroup_find(call);
  if (i < 0) return 0;
  return s_eq(g_xgroup[i].role, "member") || s_eq(g_xgroup[i].role, "mod") ||
         s_eq(g_xgroup[i].role, "admin");
}
/* What a room is called on the rail. */
static void room_title(const char *id, char *out, unsigned osz) {
  if (s_eq(id, XROOM_LOCAL)) { s_cpy(out, wtr("room.local", "Local chat"), osz); return; }
  if (s_eq(id, XROOM_REGIONAL)) { s_cpy(out, wtr("room.regional", "Regional chat"), osz); return; }
  if (s_eq(id, XROOM_GLOBAL)) { s_cpy(out, wtr("room.global", "Global chat"), osz); return; }
  if (xgroup_is(id)) {
    /* Name AND callsign: the name is a label anybody can choose; the X5
     * callsign is derived from the key and is what identifies the group. */
    int i = xgroup_find(id + 1);
    if (i >= 0 && g_xgroup[i].nick[0]) {
      s_cpy(out, g_xgroup[i].nick, osz);
      s_cat(out, " (", osz); s_cat(out, id + 1, osz); s_cat(out, ")", osz);
      return;
    }
  }
  s_cpy(out, id, osz);
}
/* How deep [call] sits in the tree, a root being 1 (10.2). Bounded by the
 * table, so a malformed answer cannot loop. */
static int xgroup_depth(const char *call) {
  int d = 1, i = xgroup_find(call);
  while (i >= 0 && g_xgroup[i].parent[0] && d <= XGROUP_MAX) {
    i = xgroup_find(g_xgroup[i].parent);
    d++;
  }
  return d;
}
/* Is [call] a folder somewhere below [root]? */
static int xgroup_below(const char *call, const char *root) {
  int i = xgroup_find(call), hops = 0;
  while (i >= 0 && g_xgroup[i].parent[0] && hops++ < XGROUP_MAX) {
    if (s_eq(g_xgroup[i].parent, root)) return 1;
    i = xgroup_find(g_xgroup[i].parent);
  }
  return 0;
}
/* room.h: where a room hangs on the rail. A folder names its parent's room
 * and how deep it sits below the root; everything else is a root (0). */
int room_tree(const char *id, char *parent, unsigned cap) {
  parent[0] = 0;
  if (!(id[0] == '#' && id[1] == 'X' && id[2] == '5')) return 0;
  int i = xgroup_find(id + 1);
  if (i < 0 || !g_xgroup[i].parent[0]) return 0;
  s_cpy(parent, "#", cap); s_cat(parent, g_xgroup[i].parent, cap);
  return xgroup_depth(id + 1) - 1;
}
/* Do we hold this group's key? The core's answer (hal_xprs_groups `role`),
 * remembered; the wapp only uses it to decide what to OFFER on screen. */
static int xgroup_is_admin(const char *call) {
  int i = xgroup_find(call);
  return i >= 0 && s_eq(g_xgroup[i].role, "admin");
}
/* Accepting an invitation (26.3.1). The core finds the offer and signs the
 * acceptance with our profile key; the wapp only asks the person. Asked once
 * per run when it arrives (a refresh must not nag), and again whenever its
 * room is opened. RAM on purpose: an unanswered invitation is asked again on
 * the next start, which is what it should do. */
static char g_join_asked[XGROUP_MAX][8];
static void join_offer(const char *call, int force) {
  if (!hal_ui_attached()) return;
  int slot = -1;
  for (int i = 0; i < XGROUP_MAX; i++) {
    if (s_eq(g_join_asked[i], call)) { if (!force) return; slot = i; break; }
    if (slot < 0 && !g_join_asked[i][0]) slot = i;
  }
  if (slot >= 0) s_cpy(g_join_asked[slot], call, sizeof(g_join_asked[slot]));
  char id[10] = "#"; s_cat(id, call, sizeof(id));
  /* Its name may not have arrived yet; the callsign alone is still a name. */
  char t[48]; room_title(id, t, sizeof(t));
  if (t[0] == '#') { s_cpy(t, "the group ", sizeof(t)); s_cat(t, call, sizeof(t)); }
  /* Folders below it that we were offered too: one answer joins them all,
   * and the prompt says so, because consent is to what was named. */
  int folders = 0;
  for (int i = 0; i < g_xgroup_n; i++)
    if (s_eq(g_xgroup[i].role, "invited") && xgroup_below(g_xgroup[i].call, call)) folders++;
  char m[600] = "{\"type\":\"ui.prompt\",\"id\":\"xgjoin:";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), wtr("prompt.invite_title", "Group invitation"));
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), wtr("prompt.invite_body", "You were invited to "));
  jesc(m, sizeof(m), t);
  if (folders) {
    char nb[8]; u_itoa((unsigned)folders, nb);
    jesc(m, sizeof(m), wtr("prompt.invite_folders", ". Join to read and post in it and in its "));
    s_cat(m, nb, sizeof(m));
    jesc(m, sizeof(m), folders == 1 ? wtr("prompt.folder_one", " folder")
                                    : wtr("prompt.folder_many", " folders"));
    jesc(m, sizeof(m), wtr("prompt.invite_until", "; until then you are only invited."));
  } else {
    jesc(m, sizeof(m), wtr("prompt.invite_plain", ". Join to read and post in it; until then you are only invited."));
  }
  s_cat(m, "\",\"confirm\":\"", sizeof(m));
  jesc(m, sizeof(m), wtr("prompt.join", "Join"));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void xgroup_room(const char *id, const char *t) {
  int r = room_ensure(id, t);
  if (r == 1) { room_bump(id); room_rail(); }
  else if (r == 0) { room_set_title(id, t); room_bump_idle(id); }
}
static void xgroups_refresh(void) {
  static char gb[12288];
  int ask = 0;
  int n = hal_xprs_groups(gb, sizeof(gb) - 1);
  if (n <= 0 || n >= (int)sizeof(gb)) return;
  gb[n] = 0;
  /* The tree as it was, so a folder whose listing arrives after its room was
   * made moves under its group on the rail. */
  static char was[XGROUP_MAX * 18], now[XGROUP_MAX * 18];
  was[0] = 0;
  for (int i = 0; i < g_xgroup_n; i++) {
    s_cat(was, g_xgroup[i].call, sizeof(was)); s_cat(was, ">", sizeof(was));
    s_cat(was, g_xgroup[i].parent, sizeof(was)); s_cat(was, ";", sizeof(was));
  }
  g_xgroup_n = 0;
  const char *cur = gb; char obj[600];
  while (g_xgroup_n < XGROUP_MAX && next_object(&cur, obj, sizeof(obj))) {
    int k = g_xgroup_n;
    jstr(obj, "call", g_xgroup[k].call, sizeof(g_xgroup[k].call));
    jstr(obj, "nick", g_xgroup[k].nick, sizeof(g_xgroup[k].nick));
    jstr(obj, "role", g_xgroup[k].role, sizeof(g_xgroup[k].role));
    if (!jstr(obj, "parent", g_xgroup[k].parent, sizeof(g_xgroup[k].parent)))
      g_xgroup[k].parent[0] = 0;
    if (!g_xgroup[k].call[0]) continue;
    g_xgroup_n++;
  }
  now[0] = 0;
  for (int i = 0; i < g_xgroup_n; i++) {
    s_cat(now, g_xgroup[i].call, sizeof(now)); s_cat(now, ">", sizeof(now));
    s_cat(now, g_xgroup[i].parent, sizeof(now)); s_cat(now, ";", sizeof(now));
  }
  int moved = !s_eq(was, now);
  for (int k = 0; k < g_xgroup_n; k++) {
    /* A group we BELONG to gets a room, so it appears the moment it exists --
     * an admin who just created a group, or a member who just accepted, sees
     * the conversation without waiting for the first message (26). A
     * stranger's group is not ours to open: an existing room learns its name,
     * but none is created. */
    char id[10] = "#"; s_cat(id, g_xgroup[k].call, sizeof(id));
    char t[48]; room_title(id, t, sizeof(t));
    /* room_ensure only writes the row, so a NEW room is drawn on the rail
     * here; an existing one is renamed when the group's name arrives after
     * the room was made (room_set_title redraws only on a change). */
    if (xgroup_may_post(g_xgroup[k].call)) {
      xgroup_room(id, t);
    }
    else if (s_eq(g_xgroup[k].role, "invited")) {
      /* An invitation is ours to answer, so it gets a room too: the place it
       * is seen and accepted, durable until it is (26.3.1). A folder of a
       * group we already belong to is asked when it is opened, not on
       * arrival: it sits in the tree under the group, and a prompt for every
       * folder the admin adds would be a prompt per topic. */
      xgroup_room(id, t);
      const char *up = g_xgroup[k].parent;
      if (!ask && !(up[0] && xgroup_may_post(up))) ask = k + 1;
    }
    else if (room_known(id)) room_set_title(id, t);
  }
  if (moved) room_rail();
  if (ask) join_offer(g_xgroup[ask - 1].call, 0);
}

/* ── Read receipts (13.7): the one half a wapp owns ─────────────────────
 * The core composes and signs the receipt; what it cannot observe is that a
 * person opened the thread. Ids wait here until they do. RAM on purpose: a
 * receipt lost to a restart is a tick that stays grey, not a message lost. */
/* ── Admitting, in this file's vocabulary ─────────────────────────────── */
static int admit(const char *room, const char *mid, const char *dir,
                 const char *sender, const char *body, const char *parent,
                 const char *via, const char *auth, int enc, uint64_t ts,
                 const char *rid, const char *status, int replay, int obf,
                 const char *path) {
  char title[48]; room_title(room, title, sizeof(title));
  room_msg_t m = { room, title, mid, dir, sender, body, parent, via, auth,
                   rid, status, ts, enc, obf, 0, replay, 0 };
  m.path = path;
  return room_admit(&m);
}
/* A muted, centered line inside a conversation -- not words anybody said. */
static void sysnote(const char *room, const char *text) {
  char h[5]; msg_id(room, text, h);
  char mid[40] = "sys:"; char nb[24]; u_lltoa(hal_time_epoch(), nb);
  s_cat(mid, nb, sizeof(mid)); s_cat(mid, ":", sizeof(mid)); s_cat(mid, h, sizeof(mid));
  room_msg_t m = { room, 0, mid, "in", "", text, "", "", "", "", "", 0, 0, 0, 1, 0, 0 };
  room_admit(&m);
}
/* A reply marker on a wire with room for a section 5 id: "+<6hex> text". */
static int strip_reply6(char *text, char parent[8]) {
  parent[0] = 0;
  if (!(text[0] == '+' && s_len(text) > 8 && text[7] == ' ')) return 0;
  for (int i = 1; i <= 6; i++) {
    char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  for (int i = 0; i < 6; i++) parent[i] = text[1 + i];
  parent[6] = 0;
  unsigned k = 0, n = 8;
  while (text[n]) text[k++] = text[n++];
  text[k] = 0;
  return 1;
}

/* ── Refilling the two broadcast rooms from the core's archive ────────
 *
 * XprsArchive keeps every heard packet for a year, and an undirected message
 * is stored there whatever its scope. Whatever arrived while this wapp was
 * not running is read back from there -- once at init, and again when a
 * person opens either room, which is the one moment they are asking to see
 * it. Never on a clock.
 *
 * ONE QUERY FOR BOTH ROOMS. The archive ask is identical for them (undirected
 * messages, newest first) and only the `scope:` on each row says which room
 * it belongs to, so filling them one at a time means asking the host twice
 * and parsing the same sixty rows twice for nothing. The watermark is the
 * OLDER of the two rooms' tails, so neither can miss a row the other already
 * has. The read starts there (five minutes of overlap for clock skew between
 * sender stamp and archive stamp); anything re-read is a duplicate by primary
 * key and costs one indexed lookup. */
static char g_hist[32768];

/* The value of `key:` in an XPRS wire, up to the next space (section 4). */
static int wire_key(const char *wire, const char *key, char *out, unsigned cap) {
  out[0] = 0;
  unsigned kl = s_len(key);
  for (const char *p = wire; *p; p++) {
    if (p != wire && p[-1] != ' ') continue;
    unsigned i = 0;
    while (i < kl && p[i] == key[i]) i++;
    if (i != kl || p[kl] != ':') continue;
    p += kl + 1;
    unsigned o = 0;
    while (*p && *p != ' ' && o < cap - 1) out[o++] = *p++;
    out[o] = 0;
    return 1;
  }
  return 0;
}
/* Everything after " m:" -- greedy, because m: is last. */
static int wire_body(const char *wire, char *out, unsigned cap) {
  out[0] = 0;
  for (const char *p = wire; *p; p++) {
    if (p != wire && p[-1] != ' ') continue;
    if (p[0] == 'm' && p[1] == ':') { s_cpy(out, p + 2, cap); return out[0] != 0; }
  }
  return 0;
}

static int xroom_backfill(void) {
  char q[160] = "{\"limit\":60,\"types\":[\"message\"],\"to\":[\"\"]";
  /* From the oldest of the three rooms' newest messages: one ask fills all. */
  uint64_t since = room_max_ts(XROOM_LOCAL);
  { uint64_t r = room_max_ts(XROOM_REGIONAL), g = room_max_ts(XROOM_GLOBAL);
    if (r < since) since = r;
    if (g < since) since = g; }
  if (since > 300) {
    char st[24]; xprs_stamp(st, sizeof(st), since - 300);
    s_cat(q, ",\"since\":\"", sizeof(q)); s_cat(q, st, sizeof(q)); s_cat(q, "\"", sizeof(q));
  }
  s_cat(q, "}", sizeof(q));
  int32_t n = hal_xprs_history(q, s_len(q), g_hist, sizeof(g_hist) - 1);
  if (n < 0) {
    char lg[112] = "[chat] backfill: reply needs ";
    char nb[16]; u_itoa((unsigned)(-n), nb);
    s_cat(lg, nb, sizeof(lg)); s_cat(lg, " bytes, buffer is 32767 -- skipped", sizeof(lg));
    log1(lg);
    return 0;
  }
  if (n <= 0 || n >= (int)sizeof(g_hist)) return 0;
  g_hist[n] = 0;

  /* The archive answers newest first; the room stores in arrival order.
   * Find every row, then walk them backwards. */
  static const char *at[60]; int rows = 0;
  { const char *cur = g_hist;
    while (rows < 60) {
      while (*cur && *cur != '{') cur++;
      if (!*cur) break;
      at[rows++] = cur;
      char skip[1400]; const char *c2 = cur;
      if (!next_object(&c2, skip, sizeof(skip))) break;
      cur = c2;
    } }
  int kept = 0;
  for (int k = rows - 1; k >= 0; k--) {
    static char slice[1400];
    const char *c2 = at[k];
    if (!next_object(&c2, slice, sizeof(slice))) continue;
    char wire[400] = "", from[24] = "", id[24] = "", bearer[12] = "", sig[16] = "";
    jstr(slice, "wire", wire, sizeof(wire));
    jstr(slice, "from", from, sizeof(from));
    jstr(slice, "id", id, sizeof(id));
    jstr(slice, "bearer", bearer, sizeof(bearer));
    jstr(slice, "sig", sig, sizeof(sig));
    if (!wire[0] || !from[0] || !id[0]) continue;
    /* The same tests the live path applies (on_core_packet). */
    char tmp[24];
    if (wire_key(wire, "n", tmp, sizeof(tmp))) continue;   /* a part (6.6) */
    if (wire_key(wire, "x", tmp, sizeof(tmp))) continue;   /* sealed (9.2) */
    if (wire_key(wire, "d", tmp, sizeof(tmp))) continue;   /* addressed */
    /* The room its scope names, as the core read it; none when it was heard
     * out of range (9.11). */
    if (!jbool_def(slice, "inRange", 1)) continue;
    char scope[16] = ""; jstr(slice, "scope", scope, sizeof(scope));
    if (!scope[0]) {
      /* A core older than the field: the wire says it. */
      if (!wire_key(wire, "scope", scope, sizeof(scope))) s_cpy(scope, "global", sizeof(scope));
      else if (scope[0] >= 'A' && scope[0] <= 'Z') s_cpy(scope, "country", sizeof(scope));
    }
    const char *xroom = xroom_of(scope);
    if (!xroom) continue;
    int mine = jbool_def(slice, "own", 0) || is_self_call(from);
    char body[400];
    if (!wire_body(wire, body, sizeof(body))) continue;
    char parent[8] = ""; wire_key(wire, "r", parent, sizeof(parent));
    /* 9.2.1: an `xr:` field means part of this is behind a passphrase. The live
     * path is told so by the core (`obfuscated`); the archive hands over the
     * wire, so read it from there or the bars stop being tappable after a
     * restart. wire_key truncates into a small buffer and still answers 1. */
    char xrv[8]; int obf = wire_key(wire, "xr", xrv, sizeof(xrv)) ? 1 : 0;
    if (admit(xroom, id, mine ? "out" : "in", mine ? g_call : from, body, parent,
              bearer, s_eq(sig, "verified") ? "verified" : "", 0,
              (uint64_t)jint(slice, "ts"), "", "", 1, obf, "") == 1)
      kept++;
  }
  /* One line for the pass, not one per row: performance.md 8.10, "any
   * per-event log line is a bet that the event is rare", and this one reads
   * sixty. The drop line this replaced was exactly that bet, lost. */
  char lg[96] = "[chat] backfill: read=";
  char nb[16]; u_itoa((unsigned)rows, nb); s_cat(lg, nb, sizeof(lg));
  s_cat(lg, " kept=", sizeof(lg)); u_itoa((unsigned)kept, nb); s_cat(lg, nb, sizeof(lg));
  log1(lg);
  return kept;
}

/* ── Sending ──────────────────────────────────────────────────────────── */
static void send_message(const char *id, const char *text_in) {
  char text[900];
  s_cpy(text, text_in, sizeof(text));
  if (!id[0] || !text[0] || !room_renderable(id)) return;

  /* 9.2.1: a message with ((...)) spans is aired obfuscated. The core builds
   * the bars and the `xr:` blob (it needs a passphrase); the wapp only asks for
   * one and hands over the marked text. Every room it can be said in: a 1:1, a
   * closed group, and the Local room -- where the core airs it undirected
   * (scope:local, no d:) instead of addressed. Local used to fall through to
   * the plain broadcast below, which aired the secret in the clear. */
  if ((xroom_is(id) || xgroup_is(id) || xprs_is_station(id)) &&
      s_find(text, "((") && s_find(text, "))")) {
    s_cpy(g_redact_text, text, sizeof(g_redact_text));
    char pm[560] = "{\"type\":\"ui.prompt\",\"id\":\"xrmk:";
    s_cat(pm, id, sizeof(pm));
    s_cat(pm, "\",\"title\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.hide_title", "Hide part of this message"));
    s_cat(pm, "\",\"body\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.hide_body", "Enter a passphrase (leave blank for the default), or pick one you have used before."));
    s_cat(pm, "\",\"passphrases\":true,\"confirm\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.hide", "Hide"));
    s_cat(pm, "\",\"input\":{\"hint\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.passphrase_hint", "passphrase"));
    s_cat(pm, "\",\"max\":64}}", sizeof(pm));
    hal_msg_send(pm, s_len(pm));
    return;
  }

  /* A room by range: a t:message the core composes undirected with the
   * room's scope, or a t:reaction (6.5) naming a bubble's section 5 id. */
  if (xroom_is(id)) {
    const char *scope = xroom_scope(id);
    char lmid[70]; int unlike; const char *ck;
    if (votemark_parse(text, lmid, &unlike, &ck)) {
      char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
      char wire[300] = "t:reaction f:";
      s_cat(wire, g_call, sizeof(wire));
      s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
      if (!s_eq(scope, "global")) {      /* global is the absent field */
        s_cat(wire, " scope:", sizeof(wire)); s_cat(wire, scope, sizeof(wire));
      }
      s_cat(wire, unlike ? " remove:like" : " add:like", sizeof(wire));
      s_cat(wire, " r:", sizeof(wire)); s_cat(wire, lmid, sizeof(wire));
      if (hal_xprs_send(wire, s_len(wire)) != 0) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
      room_react(id, lmid, g_call, unlike, 1);
      return;
    }
    char parent[8]; strip_reply6(text, parent);
    if (!text[0]) return;
    char mid[8] = "";
    if (hal_xprs_broadcast(text, s_len(text), scope, s_len(scope), parent, s_len(parent),
                           mid, sizeof(mid)) != 2 || !mid[0]) {
      notify("warning", wtr("msg.could_not_send", "Could not send"));
      return;
    }
    admit(id, mid, "out", g_call, text, parent, "", "verified", 0, 0, "", "", 0, 0, "");
    return;
  }

  /* A closed group (26): an ordinary t:message addressed to the GROUP's
   * callsign. The core signs after us and the section 5 id is taken with
   * sig: removed, so these bytes are the id. */
  if (xgroup_is(id)) {
    /* A heart on a group post is a REACTION (6.5): the same directed
     * t:reaction a 1:1 sends, with the group as d: -- a closed group is an
     * address (13.11.3). It used to ride inside m: as "+like:..", which made
     * it a message: filed as a bubble here (hidden as noise, so the sender's
     * own heart never appeared) and a message row on the far side. */
    {
      char lmid[70]; int unlike; const char *ck;
      if (votemark_parse(text, lmid, &unlike, &ck)) {
        char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
        char wire[300] = "t:reaction f:";
        s_cat(wire, g_call, sizeof(wire));
        s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
        s_cat(wire, " d:", sizeof(wire)); s_cat(wire, id + 1, sizeof(wire));
        s_cat(wire, unlike ? " remove:like" : " add:like", sizeof(wire));
        s_cat(wire, " r:", sizeof(wire)); s_cat(wire, lmid, sizeof(wire));
        int32_t rc = hal_xprs_send(wire, s_len(wire));
        if (rc == -2) { notify("warning", wtr("msg.accept_first", "You can post here once you accept the invitation")); return; }
        if (rc != 0) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
        room_react(id, lmid, g_call, unlike, 1);   /* our own heart, now */
        return;
      }
    }
    char parent[8]; strip_reply6(text, parent);
    if (!text[0]) return;
    char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
    char wire[400] = "t:message f:";
    s_cat(wire, g_call, sizeof(wire));
    s_cat(wire, " d:", sizeof(wire)); s_cat(wire, id + 1, sizeof(wire));
    s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
    if (parent[0]) { s_cat(wire, " r:", sizeof(wire)); s_cat(wire, parent, sizeof(wire)); }
    s_cat(wire, " m:", sizeof(wire)); s_cat(wire, text, sizeof(wire));
    if (s_len(wire) > 250) { notify("warning", wtr("msg.too_long", "Message too long")); return; }
    /* Whether we may post here is the CORE's decision (26.7), given back as
     * the send result; this wapp only says it in words. */
    int32_t rc = hal_xprs_send(wire, s_len(wire));
    if (rc == -2) { notify("warning", wtr("msg.accept_first", "You can post here once you accept the invitation")); return; }
    if (rc != 0) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
    char gmid[7]; xprs_id(wire, s_len(wire), gmid);
    admit(id, gmid, "out", g_call, text, parent, "", "verified", 0, 0, "", "", 0, 0, "");
    return;
  }

  /* An open group (6.3): a bulletin addressed to the group name. A heart
   * rides the same path as text ("<id>:like"), and is a reaction, not words. */
  if (id[0] == '#') {
    char wire[900];
    if (!xprs_pack(wire, sizeof(wire), g_call, id, text, hal_time_epoch())) {
      notify("warning", wtr("msg.could_not_send", "Could not send")); return;
    }
    if (hal_xprs_send(wire, s_len(wire)) != 0) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
    char lmid[70]; int unlike; const char *ck; char tgt[5];
    if (votemark_parse(text, lmid, &unlike, &ck)) { room_react(id, lmid, g_call, unlike, 1); return; }
    if (like_parse(text, tgt, &unlike)) { room_react(id, tgt, g_call, unlike, 1); return; }
    char gmid[7]; xprs_id(wire, s_len(wire), gmid);
    char parent[5]; const char *disp;
    thread_parse(text, parent, &disp);
    admit(id, gmid, "out", g_call, disp, parent, "", "verified", 0, 0, "", "", 0, 0, "");
    return;
  }

  /* A 1:1 like/repost is a DIRECTED reaction (XPRS.md 6.5): the same packet as
   * a Local-room vote, addressed with d: the peer instead of scope:local.
   * `r:` names the bubble's section 5 id; the core signs (9.1) and delivers it.
   * The heart rides the send path as a votemark ("+like:<id>"), so it is caught
   * here before the text becomes a message. Stored in THIS conversation's own
   * reactions table via room_react, exactly like every other room. */
  {
    char lmid[70]; int unlike; const char *ck;
    if (votemark_parse(text, lmid, &unlike, &ck)) {
      char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
      char wire[300] = "t:reaction f:";
      s_cat(wire, g_call, sizeof(wire));
      s_cat(wire, " ts:", sizeof(wire)); s_cat(wire, ts, sizeof(wire));
      s_cat(wire, " d:", sizeof(wire)); s_cat(wire, id, sizeof(wire));
      s_cat(wire, unlike ? " remove:like" : " add:like", sizeof(wire));
      s_cat(wire, " r:", sizeof(wire)); s_cat(wire, lmid, sizeof(wire));
      if (hal_xprs_send(wire, s_len(wire)) != 0) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
      room_react(id, lmid, g_call, unlike, 1);
      return;
    }
  }

  /* A 1:1. The return value says what ACTUALLY happened, and the bubble is
   * labelled with that: a message that could not be sealed is never drawn
   * as private (36.8). */
  int priv = room_is_private(id);
  char mid[8] = "";
  int32_t form = hal_xprs_message(id, s_len(id), text, s_len(text),
                                  priv ? 1u : 0u, mid, sizeof(mid));
  if (form == -1) {
    sysnote(id, wtr("msg.no_key_note", "No key for this contact yet - asked for it. Your message was NOT sent; try again in a moment."));
    notify("warning", wtr("msg.no_key", "No key for this contact yet"));
    return;
  }
  if (form == -3) {
    /* The core's verdict, not ours: the recipient is on another network
     * (XPRS.md 3.2) and nothing to it can ever be sealed, so asking again
     * would be refused again. Turn the room plain and say so; the words the
     * person typed were not sent and they choose whether to send them. */
    room_set_private(id, 0);
    {
      const char *net = xprs_network_name(id);
      char line[400], warn[120];
      s_cpy(line, wtr("msg.foreign_sealed_a", "This contact is on "), sizeof line);
      s_cat(line, net, sizeof line);
      s_cat(line, wtr("msg.foreign_sealed_b", ", which cannot open a sealed message. Your message was NOT sent. The conversation is now plain text and crosses a public channel anyone nearby can read; send again if that is fine."), sizeof line);
      s_cpy(warn, wtr("msg.not_sent_foreign_a", "Not sent: a "), sizeof warn);
      s_cat(warn, net, sizeof warn);
      s_cat(warn, wtr("msg.not_sent_foreign_b", " contact cannot be sealed to"), sizeof warn);
      sysnote(id, line);
      notify("warning", warn);
    }
    return;
  }
  if (form <= 0) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
  if (form == 3) {
    /* Plain, through a gateway onto another network's public channel. Said
     * once per conversation while the wapp runs, not on every bubble. */
    static char told[8][16]; static unsigned told_n;
    int seen = 0;
    for (unsigned i = 0; i < told_n && i < 8; i++) if (s_eq(told[i], id)) { seen = 1; break; }
    if (!seen) {
      s_cpy(told[told_n % 8], id, sizeof told[0]);
      told_n++;
      const char *net = xprs_network_name(id);
      char line[320];
      s_cpy(line, wtr("msg.gateway_a", "Sent through a gateway to "), sizeof line);
      s_cat(line, net, sizeof line);
      s_cat(line, wtr("msg.gateway_b", ", on a public channel: anyone nearby with a "), sizeof line);
      s_cat(line, net, sizeof line);
      s_cat(line, wtr("msg.gateway_c", " radio can read it."), sizeof line);
      sysnote(id, line);
    }
  }
  if (mid[0]) room_tx_note(mid, id);
  admit(id, mid[0] ? mid : "", "out", g_call, text, "", "", "verified", form == 1, 0,
        mid, "sent", 0, 0, "");
}

/* ── Correcting and withdrawing what was said (XPRS.md 7.8, 10.4.1) ───── */

/* What we are in closed group [gid] now: 0 none .. 4 admin. */
static int my_role_in(const char *gid) {
  return (int)hal_xprs_group_role_at(gid + 1, s_len(gid + 1), g_call, s_len(g_call),
                                     (uint32_t)hal_time_epoch());
}

/* The wire of our own correction or withdrawal of something said to a closed
 * group or a room by range. The core signs it, like every post made this way.
 * [edit] gives the new words; without it this is a withdrawal. */
static int wire_for_room(const char *id, const char *orig, const char *text, char *wire, unsigned cap) {
  char ts[24]; xprs_stamp(ts, sizeof(ts), hal_time_epoch());
  s_cpy(wire, "t:message f:", cap); s_cat(wire, g_call, cap);
  if (xgroup_is(id)) { s_cat(wire, " d:", cap); s_cat(wire, id + 1, cap); }
  s_cat(wire, " ts:", cap); s_cat(wire, ts, cap);
  if (xroom_is(id) && !s_eq(xroom_scope(id), "global")) {
    s_cat(wire, " scope:", cap); s_cat(wire, xroom_scope(id), cap);
  }
  if (text) { s_cat(wire, " edit:", cap); s_cat(wire, orig, cap); s_cat(wire, " m:", cap); s_cat(wire, text, cap); }
  else { s_cat(wire, " r:", cap); s_cat(wire, orig, cap); s_cat(wire, " remove:message", cap); }
  return s_len(wire) <= 250;
}

static void edit_message(const char *id, const char *orig, const char *text_in) {
  static char text[900];
  s_cpy(text, text_in, sizeof(text));
  if (!id[0] || !orig[0] || !text[0] || !room_renderable(id)) return;
  char author[24] = "";
  if (!room_message_sender(id, orig, author, sizeof(author))) {
    notify("warning", wtr("msg.gone", "That message is gone"));
    return;
  }
  uint64_t now = hal_time_epoch();
  if (!is_self_call(author)) {
    /* Somebody else's: only a moderator or the admin of a closed group may
     * (10.4.1), and only for somebody they outrank. */
    if (!xgroup_is(id)) { notify("warning", wtr("msg.only_author_edit", "Only its author can edit this")); return; }
    int mine = my_role_in(id);
    int theirs = (int)hal_xprs_group_role_at(id + 1, s_len(id + 1), author, s_len(author), (uint32_t)now);
    if (mine < 3 || mine <= theirs) { notify("warning", wtr("msg.cannot_edit", "You cannot edit that message")); return; }
    int rc = hal_xprs_group_moderate(id + 1, s_len(id + 1), orig, s_len(orig), text, s_len(text), 1);
    if (rc == -2) { notify("warning", wtr("msg.mod_only", "Only a moderator or the admin can do that")); return; }
    if (rc != 0) { notify("warning", wtr("msg.could_not_edit_long", "Could not edit (too long?)")); return; }
    room_moderation_sync(id);
    return;
  }
  char eid[8] = "";
  int enc = 0;
  if (xprs_is_station(id) && !xgroup_is(id)) {
    int32_t form = hal_xprs_edit(id, s_len(id), orig, s_len(orig), text, s_len(text),
                                 room_is_private(id) ? 1u : 0u, eid, sizeof(eid));
    if (form == -1) { notify("warning", wtr("msg.no_key_not_edited", "No key for this contact yet. Not edited")); return; }
    if (form == -3) { notify("warning", wtr("msg.sealed_not_edited", "This contact cannot open a sealed message. Not edited")); return; }
    if (form <= 0 || !eid[0]) { notify("warning", wtr("msg.could_not_edit", "Could not edit")); return; }
    enc = form == 1;
    room_tx_note(eid, id);
  } else if (xgroup_is(id) || xroom_is(id)) {
    char wire[400];
    if (!wire_for_room(id, orig, text, wire, sizeof(wire))) { notify("warning", wtr("msg.too_long", "Message too long")); return; }
    int32_t rc = hal_xprs_send(wire, s_len(wire));
    if (rc == -2) { notify("warning", wtr("msg.accept_first", "You can post here once you accept the invitation")); return; }
    if (rc != 0) { notify("warning", wtr("msg.could_not_edit", "Could not edit")); return; }
    xprs_id(wire, s_len(wire), eid);
  } else {
    notify("warning", wtr("msg.no_edit_here", "Editing is not available in this room"));
    return;
  }
  room_version_add(id, orig, eid, VER_EDIT, g_call, now, text, enc, eid[0] && xprs_is_station(id) ? eid : "",
                   xprs_is_station(id) ? "sent" : "", 0);
}

static void unsend_message(const char *id, const char *orig) {
  if (!id[0] || !orig[0] || !room_renderable(id)) return;
  char author[24] = "";
  if (!room_message_sender(id, orig, author, sizeof(author))) { notify("warning", wtr("msg.gone", "That message is gone")); return; }
  uint64_t now = hal_time_epoch();
  if (!is_self_call(author)) {
    if (!xgroup_is(id)) { notify("warning", wtr("msg.only_author_delete", "Only its author can delete this")); return; }
    int mine = my_role_in(id);
    int theirs = (int)hal_xprs_group_role_at(id + 1, s_len(id + 1), author, s_len(author), (uint32_t)now);
    if (mine < 3 || mine <= theirs) { notify("warning", wtr("msg.cannot_delete", "You cannot delete that message")); return; }
    int rc = hal_xprs_group_moderate(id + 1, s_len(id + 1), orig, s_len(orig), "", 0, 2);
    if (rc == -2) { notify("warning", wtr("msg.mod_only", "Only a moderator or the admin can do that")); return; }
    if (rc != 0) { notify("warning", wtr("msg.could_not_delete", "Could not delete")); return; }
    room_moderation_sync(id);
    return;
  }
  if (xprs_is_station(id) && !xgroup_is(id)) {
    if (hal_xprs_withdraw(id, s_len(id), orig, s_len(orig)) != 0) { notify("warning", wtr("msg.could_not_delete", "Could not delete")); return; }
  } else if (xgroup_is(id) || xroom_is(id)) {
    char wire[300];
    if (!wire_for_room(id, orig, 0, wire, sizeof(wire))) { notify("warning", wtr("msg.could_not_delete", "Could not delete")); return; }
    int32_t rc = hal_xprs_send(wire, s_len(wire));
    if (rc == -2) { notify("warning", wtr("msg.accept_first", "You can post here once you accept the invitation")); return; }
    if (rc != 0) { notify("warning", wtr("msg.could_not_delete", "Could not delete")); return; }
  } else {
    notify("warning", wtr("msg.no_delete_here", "Deleting is not available in this room"));
    return;
  }
  char vid[12] = "w"; s_cat(vid, orig, sizeof(vid));
  room_version_add(id, orig, vid, VER_WITHDRAW, g_call, now, "", 0, "", "", 0);
}

/* ── What the core routes to us ───────────────────────────────────────── */

/* A shared file's reference travels in the `file:` FIELD, outside the caption
 * (XPRS.md 7.7.7), so it survives a sealed 1:1 where the caption does not. The
 * host draws an attachment from a token in the message text, so put it back
 * there -- once, and only if the caption does not already carry it (older
 * builds left it in the text). The wapp never sees a byte: the token names
 * bytes the CORE fetches, stores and reports progress on. */
static int s_in(const char *hay, const char *needle) {
  if (!needle[0]) return 1;
  for (const char *h = hay; *h; h++) {
    const char *a = h, *b = needle;
    while (*a && *b && *a == *b) { a++; b++; }
    if (!*b) return 1;
  }
  return 0;
}

static void body_add_file(char *body, unsigned cap, const char *ref) {
  if (!ref[0] || s_in(body, ref)) return;
  if (body[0]) s_cat(body, " ", cap);
  s_cat(body, "file:", cap);
  s_cat(body, ref, cap);
}

/* A packet delivered as HEARD: the wire's fields, its section 5 identifier
 * and its provenance. Undirected traffic and open-group bulletins render from
 * here. A 1:1 and a closed-group post arrive in the other shape, with
 * `content`, after the core has reassembled and unsealed them. */
static void on_core_packet(const char *topic, const char *row) {
  char from[24] = "", to[40] = "", id[24] = "", bearer[12] = "", sigv[12] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "to", to, sizeof(to));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "bearer", bearer, sizeof(bearer));
  jstr(row, "sig", sigv, sizeof(sigv));
  /* The section 13 relay chain (`via:` on the wire, the row's `via` -- NOT the
   * bearer, which the core spells `bearer`): who carried this packet here. For
   * a bridged Meshtastic sender it names the bridge. Shown in the info dialog. */
  char path[96] = ""; jstr(row, "via", path, sizeof(path));
  /* 9.2.1: a packet carrying an `xr:` field has obfuscated content -- the only
   * honest marker (bars in the text are not). It rides through to the row so a
   * bubble renders tappable with a tip; the reveal itself is the core's. */
  int obf = jbool(row, "obfuscated");
  if (!from[0] || is_self_call(from)) return;
  /* A part is not a message (6.6) and a sealed body is not readable: both
   * are the core's to finish, and it re-delivers the result. */
  { char n[8]; if (jfield(row, "n", n, sizeof(n))) return; }
  if (jbool(row, "sealed")) return;
  if (!id[0]) return;   /* no identifier, no dedup, no bubble */
  const char *auth = s_eq(sigv, "verified") ? "verified" : "";

  if (s_eq(topic, "xprs.reaction")) {
    /* Three shapes of the SAME packet (6.5): a Local-room vote (scope:local,
     * no d:) lands in #LOCAL; a 1:1 reaction addressed to us (d: our callsign)
     * lands in the conversation with the reactor; a reaction addressed to a
     * closed group we have a room for (d: X5...) lands in that group. Each is
     * stored in that room's own reactions table; `r:` names the bubble's
     * section 5 id either way. Who may react in a group is the core's door,
     * like a post. */
    char scope[16] = "", r[16] = "", add[16] = "", rem[16] = "";
    jstr(row, "scope", scope, sizeof(scope));
    jfield(row, "r", r, sizeof(r));
    jfield(row, "add", add, sizeof(add));
    jfield(row, "remove", rem, sizeof(rem));
    if (!r[0]) return;
    if (!(s_eq(add, "like") || s_eq(rem, "like"))) return;
    if (is_blocked(from)) return;
    if (!to[0]) {
      const char *xroom = jbool_def(row, "inRange", 1) ? xroom_of(scope) : 0;
      if (xroom) room_react(xroom, r, from, rem[0] ? 1 : 0, 0);
    } else if (to[0] && is_self_call(to)) {
      room_react(from, r, from, rem[0] ? 1 : 0, 0);
    } else if (to[0]) {
      char gid[24] = "#"; s_cat(gid, to, sizeof(gid));
      if (xgroup_is(gid) && room_known(gid)) room_react(gid, r, from, rem[0] ? 1 : 0, 0);
    }
    return;
  }
  if (!s_eq(topic, "xprs.message")) return;
  /* 7.8: a withdrawal, or a correction, of something said to a room or a
   * group. Only a verified one counts (9.7.1 gives the reason: a withdrawal
   * that can be forged deletes other people's words), and whether its sender
   * wrote the original is judged against the original when the message is
   * drawn. */
  {
    char rm[12] = "", ed[12] = "", rr[12] = "";
    jfield(row, "remove", rm, sizeof(rm));
    jfield(row, "edit", ed, sizeof(ed));
    jfield(row, "r", rr, sizeof(rr));
    if (s_eq(rm, "message") || ed[0]) {
      if (!s_eq(sigv, "verified") || is_blocked(from)) return;
      char gid2[24] = "#"; s_cat(gid2, to, sizeof(gid2));
      const char *room2 = 0;
      if (!to[0]) {
        char scope[16] = ""; jstr(row, "scope", scope, sizeof(scope));
        room2 = jbool_def(row, "inRange", 1) ? xroom_of(scope) : 0;
      } else if (room_known(gid2)) {
        room2 = gid2;
      }
      if (!room2) return;
      uint64_t ts2 = 0;
      { char t[24]; if (jfield(row, "ts", t, sizeof(t))) ts2 = xprs_parse_stamp(t); }
      if (s_eq(rm, "message")) {
        if (rr[0]) room_version_add(room2, rr, id, VER_WITHDRAW, from, ts2, "", 0, "", "", 0);
      } else {
        static char em[900]; em[0] = 0;
        if (!jfield(row, "m", em, sizeof(em)) || !em[0]) return;
        room_version_add(room2, ed, id, VER_EDIT, from, ts2, em, 0, "", "", 0);
      }
      return;
    }
  }
  char m[900] = "";
  char fref[80] = "";
  jfield(row, "file", fref, sizeof(fref));
  /* A picture sent without a caption is a message. It has no `m:` at all
   * (7.6 splits only the caption), so requiring one dropped it silently. */
  if (!jfield(row, "m", m, sizeof(m)) && !fref[0]) return;
  if (!m[0] && !fref[0]) return;
  body_add_file(m, sizeof(m), fref);
  /* A closed group's callsign is an X5 station by 6.3's naming, but a post to
   * it is a PLAIN BROADCAST (13.11.3, "a group is an address, not a boundary")
   * and renders HERE like an open-group bulletin, filtered by the roster
   * (26.7). The one that renders from the other shape (content) is a 1:1 with
   * an ordinary station, so only that bails. */
  char gid[24] = "#"; s_cat(gid, to, sizeof(gid));
  int closed = xgroup_is(gid);
  if (xprs_is_station(to) && !closed) return;
  char parent[8] = ""; jfield(row, "r", parent, sizeof(parent));
  uint64_t ts = 0;
  { char t[24]; if (jfield(row, "ts", t, sizeof(t))) ts = xprs_parse_stamp(t); }

  if (!to[0]) {
    /* UNDIRECTED TRAFFIC GOES TO THE ROOM ITS SCOPE NAMES (9.11). With no
     * d: there is no custody, no ack and no retry. A packet heard out of
     * range, or with a scope word nobody knows, is in no room. Dropped
     * without a log line: on a busy mesh that is a lot of the air, and a
     * line per packet is a bet that the event is rare (performance.md
     * 8.10). */
    char scope[16] = "";
    jstr(row, "scope", scope, sizeof(scope));
    const char *xroom = jbool_def(row, "inRange", 1) ? xroom_of(scope) : 0;
    if (!xroom) return;
    admit(xroom, id, "in", from, m, parent, bearer, auth, 0, ts, "", "", 0, obf, path);
    return;
  }

  /* An open-group bulletin, or a closed group's plain post. Only for a group
   * we are in: the room exists (a closed group we belong to got its room the
   * moment it existed). Who may post in a closed group is the CORE's to
   * decide, at its receive door (26.7): a post that reaches this wapp is one
   * it may show. */
  if (!room_known(gid)) return;
  char lmid[70]; int unlike; const char *ck; char tgt[5];
  if (votemark_parse(m, lmid, &unlike, &ck)) { room_react(gid, lmid, from, unlike, 0); return; }
  if (like_parse(m, tgt, &unlike)) { room_react(gid, tgt, from, unlike, 0); return; }
  char par4[5]; const char *disp;
  thread_parse(m, par4, &disp);
  admit(gid, id, "in", from, disp, parent[0] ? parent : par4, bearer, auth, 0, ts, "", "", 0, obf, path);
}

static void on_core_event(const char *topic, const char *row) {
  if (!topic[0] || !row[0]) return;

  /* The outbound side of 13.7: what the core learned about a message WE
   * sent, keyed on the same section 5 identifier the bubble carries. */
  if (s_eq(topic, "xprs.status.tx")) {
    char sid[24] = "", state[16] = "";
    jstr(row, "id", sid, sizeof(sid));
    jstr(row, "state", state, sizeof(state));
    room_status(sid, state);
    return;
  }

  static char content[900];
  char from[24] = "", title[40] = "", id[24] = "", call[24] = "", sigv[12] = "";
  char bearer[12] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "title", title, sizeof(title));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "call", call, sizeof(call));
  jstr(row, "sig", sigv, sizeof(sigv));
  /* The lane the core says it arrived on -- ble, lan, lora, rns. NOT a
   * literal: this branch used to hardcode "rns", so every 1:1 read
   * "Reticulum" whatever carried it. Empty = no tag, never a guess. */
  jstr(row, "bearer", bearer, sizeof(bearer));
  /* The attachment the core named for us (7.7.7). Present on the decoded
   * shape as its own key, because a sealed 1:1's caption is not where a
   * reference can travel. */
  char fref[80] = "";
  jstr(row, "file", fref, sizeof(fref));
  /* 7.8: a withdrawal of something the sender said to us has no body, so it
   * is read before the content test sends it to the packet shape. */
  {
    char rm[12] = "";
    jstr(row, "remove", rm, sizeof(rm));
    if (rm[0] && call[0] && xprs_is_station(call) && !is_self_call(call)) {
      if (!s_eq(sigv, "verified") || is_blocked(call)) return;
      uint64_t wts = (uint64_t)jint(row, "ts");
      if (wts > 100000000000ULL) wts /= 1000;
      /* To a closed group it is the group's room, to us the conversation. */
      const char *wroom = (title[0] == '#' && xgroup_is(title)) ? title : call;
      room_version_add(wroom, rm, id, VER_WITHDRAW, call, wts, "", 0, "", "", 0);
      return;
    }
  }
  /* Two shapes reach us on xprs.message. `content` marks the finished one:
   * a 1:1 or a closed-group post the core reassembled and unsealed. */
  if (!jstr(row, "content", content, sizeof(content)) ||
      (!content[0] && !fref[0])) {
    on_core_packet(topic, row);
    return;
  }
  /* XPRS only: the sender is a station with a callsign, or nobody. */
  if (!call[0] || !xprs_is_station(call) || is_self_call(call)) return;
  /* Control traffic from an older build's private-mode handshake is not
   * correspondence. */
  if (s_eq(content, "?PRIV1") || s_eq(content, "?PRIV0") || s_pre(content, "?ACK ")) return;

  const char *room;
  if (title[0] == '#') {
    /* A closed group's post, addressed to the group. Membership is the
     * core's answer; a post it delivered is one we may see. */
    if (!xgroup_is(title)) return;
    room = title;
  } else {
    room = call;
  }
  char mid[24];
  if (id[0]) s_cpy(mid, id, sizeof(mid));
  /* No identifier from the core: derive one. A caption-less picture has no
   * text to derive from, so the reference is what names it. */
  else { char dk[5]; msg_id(call, content[0] ? content : fref, dk); s_cpy(mid, "c:", sizeof(mid)); s_cat(mid, dk, sizeof(mid)); }
  uint64_t ts = (uint64_t)jint(row, "ts");
  if (ts > 100000000000ULL) ts /= 1000;   /* milliseconds from some sources */
  char parent[8] = "";
  static char body[900];
  s_cpy(body, content, sizeof(body));
  if (room[0] == '#') strip_reply6(body, parent);
  body_add_file(body, sizeof(body), fref);
  /* 7.8.1: a correction of something the sender said, not a message. The core
   * has opened it and says whether it verified (a sealed one always does: it
   * only opens against the sender's key). */
  { char ed[12] = ""; jstr(row, "edit", ed, sizeof(ed));
    if (ed[0]) {
      if (!s_eq(sigv, "verified") || is_blocked(call)) return;
      room_version_add(room, ed, mid, VER_EDIT, call, ts, body, jbool(row, "sealed"), "", "", 0);
      return;
    } }
  /* The read half of 13.7 is recorded by room_admit (a live inbound 1:1 is
   * queued in the room's database) and aired when the user opens the thread
   * (room_open -> room_flush_reads). Nothing to do here. */
  int obf = jbool(row, "obfuscated");
  char cpath[96] = ""; jstr(row, "via", cpath, sizeof(cpath));
  admit(room, mid, "in", call, body, parent, bearer,
        s_eq(sigv, "verified") ? "verified" : "", jbool(row, "sealed"), ts, "", "", 0, obf, cpath);
}

static void push_group_members(const char *gid);

/* 9.2.1: the core answered a reveal request. On success it reassembled the
 * hidden text; show it in place, transiently, for the open room only. On
 * failure no remembered passphrase fit -- ask the reader for one. */
static void on_xr_unlock(const char *row) {
  char id[24] = "";
  jstr(row, "id", id, sizeof(id));
  if (!id[0]) return;
  if (jbool(row, "ok")) {
    static char text[900];
    jstr(row, "text", text, sizeof(text));
    room_reveal(id, text);
    return;
  }
  char pm[400] = "{\"type\":\"ui.prompt\",\"id\":\"xrpw:";
  s_cat(pm, id, sizeof(pm));
  s_cat(pm, "\",\"title\":\"", sizeof(pm));
  jesc(pm, sizeof(pm), wtr("prompt.reveal_title", "Hidden text"));
  s_cat(pm, "\",\"body\":\"", sizeof(pm));
  jesc(pm, sizeof(pm), wtr("prompt.reveal_body", "Enter the passphrase to reveal this message."));
  s_cat(pm, "\",\"confirm\":\"", sizeof(pm));
  jesc(pm, sizeof(pm), wtr("prompt.reveal", "Reveal"));
  s_cat(pm, "\",\"input\":{\"hint\":\"", sizeof(pm));
  jesc(pm, sizeof(pm), wtr("prompt.passphrase_hint", "passphrase"));
  s_cat(pm, "\",\"max\":64}}", sizeof(pm));
  hal_msg_send(pm, s_len(pm));
}

/* 9.2.1: the core aired the author's redacted message; admit our own barred
 * copy, obfuscated like everyone sees it and openable at a tap (our passphrase
 * is now remembered). */
static void on_xr_redacted(const char *row) {
  char convo[48] = "", id[24] = "";
  static char m[900];
  jstr(row, "convo", convo, sizeof(convo));
  jstr(row, "id", id, sizeof(id));
  jstr(row, "m", m, sizeof(m));
  /* The core refused it (too long to fit, or no standing in that group): there
   * is no packet and there will be no bubble, so say so rather than leave the
   * composer looking like it swallowed the message. */
  if (!jbool_def(row, "ok", 1)) { notify("warning", wtr("msg.could_not_send", "Could not send")); return; }
  if (!convo[0] || !id[0]) return;
  admit(convo, id, "out", g_call, m, "", "", "verified", 0, 0, "", "", 0, 1, "");
}

/* ── Statuses at the head of the list (XPRS.md 27) ────────────────────
 *
 * The newest status of each person this station talks with, from the last
 * day, and a box to post one's own. Told by `xprs.status` as packets arrive,
 * filled once from the spool at start. The full feed, follows and search are
 * the Status wapp's; this is the glance. A multi-part status (6.6) is the
 * Status wapp's to assemble, so a part is skipped here. */
static void on_status_packet(const char *row) {
  char from[24] = "", id[24] = "", m[900] = "", t[24] = "", n[8] = "";
  jstr(row, "from", from, sizeof(from));
  jstr(row, "id", id, sizeof(id));
  if (!from[0] || !id[0] || jbool(row, "sealed")) return;
  if (jfield(row, "n", n, sizeof(n))) return;
  if (!jfield(row, "m", m, sizeof(m)) || !m[0]) return;
  uint64_t ts = jfield(row, "ts", t, sizeof(t)) ? xprs_parse_stamp(t) : 0;
  if (!ts) ts = hal_time_epoch();
  if (status_admit(id, from, ts, m, is_self_call(from))) status_publish();
}

static void status_backfill(void) {
  char q[160] = "{\"limit\":40,\"types\":[\"status\"]";
  uint64_t now = hal_time_epoch();
  if (now > 86400) {
    char st[24]; xprs_stamp(st, sizeof(st), now - 86400);
    s_cat(q, ",\"since\":\"", sizeof(q)); s_cat(q, st, sizeof(q)); s_cat(q, "\"", sizeof(q));
  }
  s_cat(q, "}", sizeof(q));
  int32_t n = hal_xprs_history(q, s_len(q), g_hist, sizeof(g_hist) - 1);
  if (n <= 0 || n >= (int)sizeof(g_hist)) return;
  g_hist[n] = 0;
  const char *cur = g_hist;
  static char slice[1400];
  while (next_object(&cur, slice, sizeof(slice))) {
    char type[16] = "", wire[1100] = "", from[24] = "", id[24] = "", tmp[8];
    jstr(slice, "type", type, sizeof(type));
    if (!s_eq(type, "status")) continue;
    jstr(slice, "wire", wire, sizeof(wire));
    jstr(slice, "from", from, sizeof(from));
    jstr(slice, "id", id, sizeof(id));
    if (!wire[0] || !from[0] || !id[0]) continue;
    if (wire_key(wire, "n", tmp, sizeof(tmp))) continue;   /* a part (6.6) */
    if (wire_key(wire, "x", tmp, sizeof(tmp))) continue;   /* sealed (9.2) */
    char body[900];
    if (!wire_body(wire, body, sizeof(body))) continue;
    int mine = jbool_def(slice, "own", 0) || is_self_call(from);
    status_admit(id, mine ? g_call : from, (uint64_t)jint(slice, "ts"), body, mine);
  }
}

static void drain_core_events(void) {
  static char row[3200];
  char topic[64];
  for (int guard = 0; guard < 32; guard++) {
    if (hal_event_available() == 0) break;
    uint32_t n = hal_event_recv(topic, sizeof(topic) - 1, row, sizeof(row) - 1);
    if (n == 0) break;
    row[n] = 0;
    if (s_eq(topic, "core.groups")) {
      xgroups_refresh();
      /* A roster that changed while its room is open updates the panel now. */
      const char *open = room_open_id();
      if (xgroup_is(open)) {
        push_group_members(open);
        room_set_mod(my_role_in(open) >= 3, 1);
        room_moderation_sync(open);
      }
      continue;
    }
    if (s_eq(topic, "xprs.unlock")) { on_xr_unlock(row); continue; }
    if (s_eq(topic, "xprs.status")) { on_status_packet(row); continue; }
    if (s_eq(topic, "xprs.redacted")) { on_xr_redacted(row); continue; }
    on_core_event(topic, row);
  }
}

/* ── Finding somebody: the New chat and Search screens ──────────────────
 *
 * ONE question, ONE door: who has this device heard? The core keeps that
 * table (XprsMonitor -- every reachability decision it makes reads the same
 * one) and hands it over as hal_xprs_stations: stations in earshot now, then
 * stations heard this hour but quiet since -- both on a radio or the local
 * network, a relayed packet counting for the radio it arrived on -- then
 * XPRS stations that reached us over Reticulum this hour. Newest first, each
 * row tagged `seen Xm ago` and its bearer. This wapp draws the first two as
 * "Nearby" and the third as "On Reticulum", and derives nothing.
 *
 * It used to ask hal_mesh_devices and hal_people_directory. The first is fed
 * by a beacon phones do not air, the second is a Reticulum view that lists a
 * station only after it is heard on the air -- so on a phone hearing two others
 * over Bluetooth the panel was empty. A callsign is all Chat needs
 * (XPRS.md 3); the lane and the key are the core's business at send time.
 * In Search, "#GROUP" typed in opens that group. A tap starts the room. */
static char g_find_q[64], g_sa_q[64];

/* First occurrence of [needle] in [hay], or 0. Used by the ((..)) redaction
 * check (send_message) and by the shared finder's include; kept local. */
static const char *s_find(const char *hay, const char *needle) {
  for (const char *p = hay; *p; p++) if (s_pre(p, needle)) return p;
  return 0;
}

/* New-chat / Search: the generic callsign finder, now shared with other wapps
 * (../hal/people_finder.h). Same ui.people.set panel and the same `go:` ids as
 * before -- only the implementation moved out so any wapp can reuse it. */
static void render_people(const char *field, const char *q, int allow_group) {
  pf_render(field, q, allow_group);
}
/* THE MEMBERS PANEL (the host's reusable `members` field, screen "Group").
 * A group can have hundreds of members, so nothing here is pushed whole: the
 * roster is read from the core once into a buffer and the host asks for one
 * page at a time (members_page: query, filter, offset), drawing them lazily.
 * Search and the filter chips are answered here for the same reason, a filter
 * over the rows the host holds would miss everyone on a page not yet fetched.
 * Membership decides what a client SHOWS (26.7); the roster is the core's.
 * `gid` is "#X5....". */
#define MP_PAGE 40
#define MP_ROSTER 40960   /* about 780 members; a bigger group is cut here */
static char g_mp_gid[12];       /* the group the panel is open for, or "" */
static char g_mp_q[24];
static char g_mp_filter[10];    /* "", "admin" or "invited" */
static char g_mp_roster[MP_ROSTER];
static int  g_mp_roster_ok;     /* the buffer holds g_mp_gid's roster */

static int mp_load(const char *gid) {
  if (g_mp_roster_ok) return 1;
  int n = hal_xprs_group_roster(gid, s_len(gid), g_mp_roster, sizeof(g_mp_roster) - 1);
  if (n < 0 || n >= (int)sizeof(g_mp_roster)) { g_mp_roster[0] = 0; return 0; }
  g_mp_roster[n] = 0;
  g_mp_roster_ok = 1;
  return 1;
}

/* Does this roster entry belong to the page asked for? Same test for the
 * count and for the rows, so the total always matches what pages through. */
static int mp_match(const char *call, const char *role, const char *st) {
  if (g_mp_filter[0]) {
    if (s_eq(g_mp_filter, "invited") ? !s_eq(st, "invited")
                                     : !(s_eq(st, "member") && s_eq(role, "admin"))) return 0;
  }
  if (g_mp_q[0]) {
    char hay[16], needle[24];
    unsigned i = 0;
    for (; call[i] && i < sizeof(hay) - 1; i++) hay[i] = (call[i] >= 'a' && call[i] <= 'z') ? call[i] - 32 : call[i];
    hay[i] = 0;
    for (i = 0; g_mp_q[i] && i < sizeof(needle) - 1; i++)
      needle[i] = (g_mp_q[i] >= 'a' && g_mp_q[i] <= 'z') ? g_mp_q[i] - 32 : g_mp_q[i];
    needle[i] = 0;
    if (!s_find(hay, needle)) return 0;
  }
  return 1;
}

/* Admins first, then moderators, members, and the invited last: four passes
 * over the same buffer, so the order is stable from one page to the next. */
static void push_members_page(int offset, int header, int reset) {
  if (!g_mp_gid[0] || !mp_load(g_mp_gid)) return;
  static char o[6144]; const unsigned sz = sizeof(o);
  int total = 0, sent = 0, seen = 0, n_all = 0, n_adm = 0, n_inv = 0;
  s_cpy(o, "{\"type\":\"ui.members.set\",\"field\":\"members\",\"offset\":", sz);
  { char nb[12]; u_lltoa((unsigned long long)offset, nb); s_cat(o, nb, sz); }
  if (reset) s_cat(o, ",\"reset\":true", sz);
  static char rows[4608]; rows[0] = 0;
  for (int rank = 0; rank < 4; rank++) {
    const char *cur = g_mp_roster; char obj[128];
    while (next_object(&cur, obj, sizeof(obj))) {
      char st[12] = "", call[16] = "", role[12] = "";
      jstr(obj, "state", st, sizeof(st));
      jstr(obj, "call", call, sizeof(call));
      jstr(obj, "role", role, sizeof(role));
      if (!call[0]) continue;
      int inv = s_eq(st, "invited");
      int r = inv ? 3 : s_eq(role, "admin") ? 0 : s_eq(role, "mod") ? 1 : 2;
      if (r != rank) continue;
      if (header) {
        /* The chip counts ignore the query on purpose: they say how many of
         * each kind the group has, whatever is typed. */
        n_all += !inv; n_adm += (!inv && s_eq(role, "admin")); n_inv += inv;
      }
      if (!mp_match(call, role, st)) continue;
      total++;
      if (seen++ < offset || sent >= MP_PAGE) continue;
      if (sent) s_cat(rows, ",", sizeof(rows));
      sent++;
      s_cat(rows, "{\"id\":\"go:", sizeof(rows)); jesc(rows, sizeof(rows), call);
      s_cat(rows, "\",\"title\":\"", sizeof(rows)); jesc(rows, sizeof(rows), call);
      if (is_self_call(call)) s_cat(rows, " (you)", sizeof(rows));
      s_cat(rows, "\",\"subtitle\":\"", sizeof(rows));
      jesc(rows, sizeof(rows), inv ? "invited" : (role[0] ? role : "member"));
      s_cat(rows, "\",\"icon\":\"person\"}", sizeof(rows));
    }
  }
  s_cat(o, ",\"total\":", sz);
  { char nb[12]; u_lltoa((unsigned long long)total, nb); s_cat(o, nb, sz); }
  if (header) {
    char t[48]; room_title(g_mp_gid, t, sizeof(t));
    s_cat(o, ",\"title\":\"", sz); jesc(o, sz, t); s_cat(o, "\"", sz);
    char sub[48] = ""; char nb[12];
    u_lltoa((unsigned long long)n_all, nb); s_cat(sub, nb, sizeof(sub));
    s_cat(sub, n_all == 1 ? " member" : " members", sizeof(sub));
    s_cat(o, ",\"subtitle\":\"", sz); s_cat(o, sub, sz); s_cat(o, "\"", sz);
    /* Only the admin can offer membership (26.3), and the group's own things
     * are signed with the group key: its picture (6.3.2, 10.1) and listing a
     * folder (10.2), which stops at the fifth level because every client
     * ignores deeper. A member gets the list alone. */
    s_cat(o, ",\"actions\":[", sz);
    if (xgroup_is_admin(g_mp_gid + 1)) {
      s_cat(o, "{\"id\":\"inv\",\"label\":\"", sz); jesc(o, sz, wtr("label.invite", "Invite")); s_cat(o, "\",\"icon\":\"person_add\",\"primary\":true}", sz);
      if (xgroup_depth(g_mp_gid + 1) < 5) {
        s_cat(o, ",{\"id\":\"fold\",\"label\":\"", sz);
        jesc(o, sz, wtr("label.new_folder", "New folder"));
        s_cat(o, "\",\"icon\":\"create_new_folder\"}", sz);
      }
      s_cat(o, ",{\"id\":\"pic\",\"label\":\"", sz); jesc(o, sz, wtr("label.picture", "Picture")); s_cat(o, "\",\"icon\":\"image\"}", sz);
    }
    s_cat(o, "],\"filters\":[{\"id\":\"\",\"label\":\"", sz); jesc(o, sz, wtr("label.all", "All")); s_cat(o, "\",\"count\":", sz);
    u_lltoa((unsigned long long)(n_all + n_inv), nb); s_cat(o, nb, sz);
    /* A chip only for a kind the group has: the core can list its admin as a
     * plain member, and "Admins 0" would only be wrong. */
    if (n_adm) {
      s_cat(o, "},{\"id\":\"admin\",\"label\":\"", sz); jesc(o, sz, wtr("label.admins", "Admins")); s_cat(o, "\",\"count\":", sz);
      u_lltoa((unsigned long long)n_adm, nb); s_cat(o, nb, sz);
    }
    if (n_inv) {
      s_cat(o, "},{\"id\":\"invited\",\"label\":\"", sz); jesc(o, sz, wtr("label.invited", "Invited")); s_cat(o, "\",\"count\":", sz);
      u_lltoa((unsigned long long)n_inv, nb); s_cat(o, nb, sz);
    }
    s_cat(o, "}]", sz);
  }
  s_cat(o, ",\"rows\":[", sz); s_cat(o, rows, sz); s_cat(o, "]}", sz);
  hal_msg_send(o, s_len(o));
}

/* The core said the roster moved: if the panel is open for this group, it
 * re-reads and shows the first page again. The call sites were the old side
 * panel's, they stay the one place a roster change is announced. */
static void push_group_members(const char *gid) {
  if (!g_mp_gid[0] || !s_eq(gid, g_mp_gid)) return;
  g_mp_roster_ok = 0;
  push_members_page(0, 1, 0);
}

static void members_open(const char *gid) {
  if (!xgroup_is(gid)) return;
  s_cpy(g_mp_gid, gid, sizeof(g_mp_gid));
  g_mp_q[0] = 0; g_mp_filter[0] = 0; g_mp_roster_ok = 0;
  push_members_page(0, 1, 1);
  screen_open("@screen.group");
}

/* A new closed group (26.1). The name is all the wapp has to say: the core
 * mints the key, keeps it, records us as a member and announces the group,
 * and hands back the X5 callsign before anything is aired. The room opens on
 * that callsign at once, as the admin's own; core.groups follows and finds
 * it already there. Inviting people is the member panel's business
 * (invite_open). */
static void group_create(const char *nick) {
  char call[16] = "";
  int n = hal_xprs_group_create(nick, s_len(nick), call, sizeof(call) - 1);
  if (n <= 0 || n >= (int)sizeof(call)) {
    notify("warning", wtr("msg.group_failed", "Could not create the group"));
    return;
  }
  call[n] = 0;
  xgroups_refresh();
  char id[10] = "#"; s_cat(id, call, sizeof(id));
  char title[48]; room_title(id, title, sizeof(title));
  room_start(id, title);
  push_group_members(id);
  notify("info", wtr("msg.group_created", "Group created"));
}

/* Offer [folder] to everyone who belongs to [gid] now (10.2: membership
 * does not travel down, so a folder starts with its admin alone). One call,
 * the callsigns comma separated: the core grants them in as few packets as
 * fit. Each still has to accept (26.3.1). */
static void folder_offer_members(const char *gid, const char *folder) {
  static char rb[4096];
  int n = hal_xprs_group_roster(gid, s_len(gid), rb, sizeof(rb) - 1);
  if (n <= 0 || n >= (int)sizeof(rb)) return;
  rb[n] = 0;
  static char calls[1024]; calls[0] = 0;
  const char *cur = rb; char obj[128];
  while (next_object(&cur, obj, sizeof(obj))) {
    char st[12] = "", call[16] = "";
    jstr(obj, "state", st, sizeof(st));
    jstr(obj, "call", call, sizeof(call));
    if (!s_eq(st, "member") || !call[0] || is_self_call(call)) continue;
    if (calls[0]) s_cat(calls, ",", sizeof(calls));
    s_cat(calls, call, sizeof(calls));
  }
  if (calls[0]) hal_xprs_group_invite(folder, s_len(folder), calls, s_len(calls));
}

/* A folder inside a group, for one topic: a subgroup (10.2) the core mints
 * and the parent lists. The group's members are offered it at once, and the
 * folder's room opens under the group on the rail. */
static void folder_create(const char *gid, const char *nick) {
  if (!xgroup_is(gid)) return;
  char call[16] = "";
  int n = hal_xprs_group_folder(gid + 1, s_len(gid + 1), nick, s_len(nick),
                                call, sizeof(call) - 1);
  if (n <= 0 || n >= (int)sizeof(call)) {
    notify("warning", wtr("msg.folder_failed", "Could not create the folder"));
    return;
  }
  call[n] = 0;
  folder_offer_members(gid, call);
  xgroups_refresh();
  char id[10] = "#"; s_cat(id, call, sizeof(id));
  char title[48]; room_title(id, title, sizeof(title));
  room_start(id, title);
  push_group_members(id);
  notify("info", wtr("msg.folder_created", "Folder created"));
}

/* A group's or a folder's picture. The host picks it and makes the
 * thumbnail (ui.pick.picture); the core signs it with the group key and
 * announces it (6.3.2). The host draws it wherever it draws the group. */
static void picture_set(const char *gid, const char *token) {
  if (!xgroup_is(gid) || !token[0]) return;
  if (hal_xprs_group_picture(gid + 1, s_len(gid + 1), token, s_len(token)) != 0) {
    notify("warning", wtr("msg.admin_picture", "Only the group's admin can change its picture"));
    return;
  }
  notify("info", wtr("msg.picture_changed", "Picture changed"));
}

/* The answer to join_offer: the core signs our acceptance, and the roster
 * that moves fires core.groups, which refreshes the rail and the panel. */
static void group_join(const char *gid) {
  if (!xgroup_is(gid)) return;
  if (hal_xprs_group_accept(gid + 1, s_len(gid + 1)) != 0) {
    notify("warning", wtr("msg.invite_closed", "Could not join: the invitation is no longer open"));
    return;
  }
  /* The folders below it that we were offered too, which the prompt named. */
  for (int i = 0; i < g_xgroup_n; i++)
    if (s_eq(g_xgroup[i].role, "invited") && xgroup_below(g_xgroup[i].call, gid + 1))
      hal_xprs_group_accept(g_xgroup[i].call, s_len(g_xgroup[i].call));
  xgroups_refresh();
  /* Accepting is the newest thing that happened here: the group goes to the
   * top of the chat list, where somebody new to the app will look. */
  room_bump(gid); room_rail();
  if (s_eq(room_open_id(), gid)) push_group_members(gid);
  char t[48]; room_title(gid, t, sizeof(t));
  char m[80] = "Joined "; s_cat(m, t, sizeof(m));
  notify("info", m);
}

/* Inviting (26.3). The member panel's Invite row opens a station picker for
 * the group it names; a tap there asks the core to offer membership. The
 * core signs with the group key and refuses if we are not the admin. */
static char g_invite_gid[12];
static char g_invite_q[24];
static void invite_open(const char *gid) {
  if (!xgroup_is(gid)) return;
  s_cpy(g_invite_gid, gid, sizeof(g_invite_gid));
  g_invite_q[0] = 0;
  pf_hide_groups = 1;
  render_people("inviteuser", "", 0);
  pf_hide_groups = 0;
  screen_open("@screen.invite");
}
static void invite_pick(const char *buf) {
  char call[24] = "";
  if (!pf_pick(buf, "inviteuser", call, sizeof(call)) || call[0] == '#') return;
  if (!g_invite_gid[0]) return;
  if (call[0] == 'X' && call[1] == '5') {
    notify("warning", wtr("msg.invite_station_only", "A group cannot be invited, only a station"));
    return;
  }
  if (hal_xprs_group_invite(g_invite_gid, s_len(g_invite_gid), call, s_len(call)) != 0) {
    notify("warning", wtr("msg.admin_invite", "Only the group's admin can invite"));
    return;
  }
  /* And into the folders below it that we keep: a group's topics are part of
   * the group, though each roster is its own (10.2). */
  for (int i = 0; i < g_xgroup_n; i++)
    if (s_eq(g_xgroup[i].role, "admin") && xgroup_below(g_xgroup[i].call, g_invite_gid + 1))
      hal_xprs_group_invite(g_xgroup[i].call, s_len(g_xgroup[i].call), call, s_len(call));
  char m[64] = "Invited "; s_cat(m, call, sizeof(m));
  notify("info", m);
  screen_close();
  push_group_members(g_invite_gid);
}

static void go_tap(const char *buf, const char *key) {
  char id[64] = "";
  jstr(buf, key, id, sizeof(id));
  if (!s_pre(id, "go:")) return;
  const char *target = id + 3;
  /* A closed group's callsign (X5..., 6.3's naming) opens the GROUP room
   * (#X5), never a 1:1 with the group address. This is the difference between
   * a post gated by membership (26.7) and an ungated direct message: a
   * non-member who finds a group must land in the group room, where the send
   * path refuses their post, not in a private thread that lets it out. */
  char gid[24];
  if (target[0] == 'X' && target[1] == '5') {
    gid[0] = '#'; s_cpy(gid + 1, target, sizeof(gid) - 1); target = gid;
  }
  if (!room_renderable(target)) return;
  char title[48]; room_title(target, title, sizeof(title));
  room_start(target, title);
  screen_close();
}

/* The sender's name tapped in a room: block or unblock them. */
static void profile_show(const char *call) {
  char c[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) c[j++] = s_up(call[i]);
  c[j] = 0;
  if (!c[0] || is_self_call(c)) return;
  int blocked = is_blocked(c);
  char m[400] = "{\"type\":\"ui.prompt\",\"id\":\"prof:";
  jesc(m, sizeof(m), c);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), c);
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), blocked ? wtr("msg.blocked_hidden", "Blocked - their messages are hidden.")
                             : wtr("msg.a_station", "An XPRS station."));
  s_cat(m, "\",\"chips\":[{\"label\":\"", sizeof(m));
  jesc(m, sizeof(m), blocked ? wtr("label.unblock", "Unblock") : wtr("label.block", "Block"));
  s_cat(m, blocked ? "\",\"value\":\"unblock\"}" : "\",\"value\":\"block\"}", sizeof(m));
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void do_block(const char *call) {
  if (block_add(call)) { host_block_state(call, 1); notify("info", wtr("msg.blocked_now", "Blocked - you won't see their messages")); }
}
static void do_unblock(const char *call) {
  if (block_remove(call)) { host_block_state(call, 0); notify("info", wtr("msg.unblocked", "Unblocked")); }
}

/* THE WIDGET IS CALLED "rooms", SO ITS FIELDS ARE rooms_*. The older
 * conversations_* names stay accepted so nothing driving this wapp from
 * outside has to change. */
static int cmd_field(const char *buf, const char *key, char *out, unsigned m) {
  char k[48] = "rooms_"; s_cat(k, key, sizeof(k));
  if (jstr(buf, k, out, m) && out[0]) return 1;
  s_cpy(k, "conversations_", sizeof(k)); s_cat(k, key, sizeof(k));
  return jstr(buf, k, out, m);
}

static void do_open(const char *buf) {
  char convo[48] = ""; cmd_field(buf, "convo", convo, sizeof(convo));
  if (!convo[0]) return;
  /* Whether the host offers moderation, stated by the row the open sends. */
  room_set_mod(xgroup_is(convo) && my_role_in(convo) >= 3, 0);
  /* And the acts of authority already in force on this group's posts, folded
   * in BEFORE the tail is drawn, so the first paint is the corrected one. */
  if (xgroup_is(convo)) room_moderation_sync(convo);
  room_open(convo);
  /* Opening the room is the one moment somebody is asking to SEE it. The
   * guard is a memory of the last refill, not a schedule. */
  if (xroom_is(convo)) {
    uint64_t now = hal_time_epoch();
    if (now - g_xroom_fill_at >= 60) { g_xroom_fill_at = now; xroom_backfill(); }
  }
  /* A closed group's member panel is the roster, pushed now so it is ready
   * the moment the header's group button is tapped. */
  /* Moderation: whether the host offers it on other people's messages, and
   * the acts already in force on this group's (10.4.1). */
  if (xgroup_is(convo)) {
    push_group_members(convo);
    int i = xgroup_find(convo + 1);
    if (i >= 0 && s_eq(g_xgroup[i].role, "invited")) join_offer(convo + 1, 1);
  }
}

static int g_catchup_pending;

/* ── module entry points ──────────────────────────────────────────────── */
void module_init(void) {
  static const char *topics[] = { "xprs.message", "xprs.reaction", "xprs.status.tx",
                                  "core.groups", "xprs.unlock", "xprs.redacted",
                                  "xprs.status" };
  for (unsigned i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
    hal_event_subscribe(topics[i], s_len(topics[i]));
  char id[16];
  uint32_t n = hal_identity(id, sizeof(id) - 1);
  if (n > 0 && n < sizeof(id)) { id[n] = 0; if (id[0]) s_cpy(g_call, id, sizeof(g_call)); }
  chan_load();
  room_set_local_enabled(g_chan_local);
  room_init(g_call);
  xgroups_refresh();
  /* Only what is already on this device, and only the index: the first frame
   * waits for this function (the host runs it on the UI isolate), so it draws
   * the chats as they were left. What was said while we were not listening is
   * fetched by the first event, which the page sends once that frame is up
   * (performance.md 8.4: a wapp's cost is what it does per event). */
  g_xroom_fill_at = hal_time_epoch();
  room_hydrate();
  status_publish();
  g_catchup_pending = 1;
}

/* The archive catch-up, kept out of module_init. A background engine has no
 * first frame to wait for, and gets it with its first event. */
static void catchup_once(void) {
  g_catchup_pending = 0;
  g_xroom_fill_at = hal_time_epoch();
  int kept = xroom_backfill();
  status_backfill();
  if (kept > 0) room_hydrate();
  status_publish();
}

/* NO CLOCK. Everything is a call: the core delivering, or a person typing. */
void module_tick(void) {}
int32_t module_tick_interval_ms(void) { return 0; }

void module_handle_event(void) {
  static char buf[4096];
  if (g_catchup_pending) catchup_once();
  /* What the core routed to us, first: a host command is not a precondition
   * for a packet. */
  drain_core_events();
  if (hal_msg_available() == 0) return;
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  /* A form command is {command:name}; an app-bar or popup-menu item is
   * {type:"action",action:name}. Resolve the name, THEN dispatch. */
  char cmd[40] = "";
  if (!jstr(buf, "command", cmd, sizeof(cmd)) || !cmd[0]) {
    char typ[24] = ""; jstr(buf, "type", typ, sizeof(typ));
    if (!s_eq(typ, "action") || !jstr(buf, "action", cmd, sizeof(cmd))) return;
  }

  if (s_eq(cmd, "rooms_send") || s_eq(cmd, "conversations_send")) {
    char id[48] = ""; static char text[900];
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "input", text, sizeof(text));
    if (id[0] && text[0]) send_message(id, text);
  }
  else if (s_eq(cmd, "rooms_edit")) {
    char id[48] = "", mid[24] = ""; static char text[900];
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "mid", mid, sizeof(mid));
    cmd_field(buf, "input", text, sizeof(text));
    edit_message(id, mid, text);
  }
  else if (s_eq(cmd, "rooms_unsend")) {
    char id[48] = "", mid[24] = "";
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "mid", mid, sizeof(mid));
    unsend_message(id, mid);
  }
  else if (s_eq(cmd, "rooms_history")) {
    char id[48] = "", mid[24] = "";
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "mid", mid, sizeof(mid));
    if (id[0] && mid[0]) room_history(id, mid);
  }
  else if (s_eq(cmd, "rooms_open") || s_eq(cmd, "conversations_open")) do_open(buf);
  /* The host names a thread it just deep-linked into: "open X1WATT, and by the
   * way that is what to call it". It has been sending this since the graph
   * grew a Message button and nothing here read it, so the name fell on the
   * floor and a thread opened from a profile showed whatever the rail had.
   * Mail has had the handler all along; chat never did. */
  else if (s_eq(cmd, "convo_name")) {
    char id[48] = "", title[64] = "";
    jstr(buf, "convo_name_id", id, sizeof(id));
    jstr(buf, "convo_name", title, sizeof(title));
    if (id[0] && title[0]) { room_set_title(id, title); room_rail(); }
  }
  else if (s_eq(cmd, "nav_back")) room_left();
  else if (s_eq(cmd, "rooms_close") || s_eq(cmd, "conversations_close")) {
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0]) room_close(id);
  }
  /* The status box at the head of the list: post one's own, or open one. */
  else if (s_eq(cmd, "rooms_status_post")) {
    static char text[900];
    cmd_field(buf, "status_input", text, sizeof(text));
    if (text[0]) {
      char mid[24] = "";
      if (hal_xprs_status(text, s_len(text), 0, 0, 0, 0, mid, sizeof(mid) - 1) == 0 && mid[0]) {
        /* Drawn at once, keyed on the id the core named it with; the copy that
         * comes back collapses onto it (architecture.md 4). */
        status_admit(mid, g_call, hal_time_epoch(), text, 1);
        status_publish();
      } else {
        notify("warning", wtr("msg.status_failed", "Could not post the status"));
      }
    }
  }
  else if (s_eq(cmd, "rooms_status_seen")) {
    char id[24] = ""; cmd_field(buf, "status_id", id, sizeof(id));
    if (id[0]) { status_seen(id); status_publish(); }
  }
  /* The three-dots menu on a row of the list. */
  else if (s_eq(cmd, "rooms_archive")) {
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0]) { room_archive(id, 1); notify("info", wtr("msg.archived", "Chat archived")); }
  }
  else if (s_eq(cmd, "rooms_unarchive")) {
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0]) { room_archive(id, 0); notify("info", wtr("msg.unarchived", "Chat unarchived")); }
  }
  else if (s_eq(cmd, "rooms_delete")) {
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0]) { room_delete(id); notify("info", wtr("msg.deleted", "Chat deleted")); }
  }
  /* The "Archived" row at the top of the list, and a tap on one of them:
   * it comes back to the list and opens. */
  else if (s_eq(cmd, "rooms_archived")) {
    room_archived_publish();
    screen_open("@screen.archived");
  }
  else if (s_eq(cmd, "archived_tap")) {
    char id[64] = ""; jstr(buf, "archived_id", id, sizeof(id));
    if (s_pre(id, "arc:") && room_renderable(id + 4)) {
      room_archive(id + 4, 0);
      room_start(id + 4, 0);
      screen_close();
    }
  }
  else if (s_eq(cmd, "rooms_hide") || s_eq(cmd, "conversations_hide")) {
    char id[48] = "", key[40] = "";
    cmd_field(buf, "convo", id, sizeof(id));
    cmd_field(buf, "hidekey", key, sizeof(key));
    if (id[0] && key[0]) room_hide(id, key);
  }
  else if (s_eq(cmd, "rooms_block") || s_eq(cmd, "conversations_block")) {
    char c[16] = ""; cmd_field(buf, "blockcall", c, sizeof(c));
    if (c[0]) do_block(c);
  }
  else if (s_eq(cmd, "rooms_private") || s_eq(cmd, "conversations_private")) {
    /* Privacy is a property of each packet (9.2): nothing to negotiate with
     * the peer, and the wire says which form was used. */
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    if (id[0] && id[0] != '#') {
      int on = room_set_private(id, !room_is_private(id));
      if (on >= 0) notify("info", on ? wtr("msg.sealed_on", "Messages to this station are sealed")
                                    : wtr("msg.sealed_off", "Messages to this station are plain text"));
    }
  }
  else if (s_eq(cmd, "rooms_newchat")) {
    g_find_q[0] = 0;
    render_people("finduser", "", 0);
    screen_open("@screen.newchat");
  }
  else if (s_eq(cmd, "rooms_newgroup")) {
    char pm[560] = "{\"type\":\"ui.prompt\",\"id\":\"xgnew:\",\"title\":\"";
    jesc(pm, sizeof(pm), wtr("prompt.newgroup_title", "New group"));
    s_cat(pm, "\",\"body\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.newgroup_body", "A closed group: only the people you invite can post in it. The name is a label; the group's callsign comes from its own key."));
    s_cat(pm, "\",\"confirm\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.create", "Create"));
    s_cat(pm, "\",\"input\":{\"hint\":\"", sizeof(pm));
    jesc(pm, sizeof(pm), wtr("prompt.group_name_hint", "group name"));
    s_cat(pm, "\",\"max\":24}}", sizeof(pm));
    hal_msg_send(pm, s_len(pm));
  }
  else if (s_eq(cmd, "rooms_members")) {
    char id[48] = ""; cmd_field(buf, "convo", id, sizeof(id));
    members_open(id);
  }
  else if (s_eq(cmd, "members_page")) {
    jstr(buf, "members_query", g_mp_q, sizeof(g_mp_q));
    jstr(buf, "members_filter", g_mp_filter, sizeof(g_mp_filter));
    push_members_page(jint(buf, "members_offset"), 0, 0);
  }
  else if (s_eq(cmd, "members_tap")) {
    char id[48] = ""; jstr(buf, "members_id", id, sizeof(id));
    if (s_pre(id, "go:") && !is_self_call(id + 3)) profile_show(id + 3);
  }
  else if (s_eq(cmd, "members_action") && g_mp_gid[0]) {
    char id[16] = ""; jstr(buf, "members_id", id, sizeof(id));
    if (s_eq(id, "inv")) invite_open(g_mp_gid);
    else if (s_eq(id, "fold") && xgroup_is(g_mp_gid)) {
      char pm[560] = "{\"type\":\"ui.prompt\",\"id\":\"xgfold:";
      jesc(pm, sizeof(pm), g_mp_gid);
      s_cat(pm, "\",\"title\":\"", sizeof(pm));
      jesc(pm, sizeof(pm), wtr("prompt.newfolder_title", "New folder"));
      s_cat(pm, "\",\"body\":\"", sizeof(pm));
      jesc(pm, sizeof(pm), wtr("prompt.newfolder_body", "A folder for one topic inside this group. Its members are invited to it."));
      s_cat(pm, "\",\"confirm\":\"", sizeof(pm));
      jesc(pm, sizeof(pm), wtr("prompt.create", "Create"));
      s_cat(pm, "\",\"input\":{\"hint\":\"", sizeof(pm));
      jesc(pm, sizeof(pm), wtr("prompt.folder_name_hint", "folder name"));
      s_cat(pm, "\",\"max\":24}}", sizeof(pm));
      hal_msg_send(pm, s_len(pm));
    }
    else if (s_eq(id, "pic") && xgroup_is(g_mp_gid)) {
      char pm[96] = "{\"type\":\"ui.pick.picture\",\"id\":\"gpic:";
      jesc(pm, sizeof(pm), g_mp_gid);
      s_cat(pm, "\"}", sizeof(pm));
      hal_msg_send(pm, s_len(pm));
    }
  }
  else if (s_eq(cmd, "inviteuser_search")) {
    jstr(buf, "inviteuser_query", g_invite_q, sizeof(g_invite_q));
    pf_hide_groups = 1;
    render_people("inviteuser", g_invite_q, 0);
    pf_hide_groups = 0;
  }
  else if (s_eq(cmd, "inviteuser_tap")) invite_pick(buf);
  else if (s_eq(cmd, "finduser_search")) {
    jstr(buf, "finduser_query", g_find_q, sizeof(g_find_q));
    render_people("finduser", g_find_q, 0);
  }
  else if (s_eq(cmd, "finduser_tap")) go_tap(buf, "finduser_id");
  else if (s_eq(cmd, "rooms_search")) {
    g_sa_q[0] = 0;
    render_people("searchall", "", 1);
    screen_open("@screen.search");
  }
  else if (s_eq(cmd, "searchall_search")) {
    jstr(buf, "searchall_query", g_sa_q, sizeof(g_sa_q));
    static char chats[8192], msgs[6144];
    room_search_section(g_sa_q, chats, sizeof(chats));
    room_search_messages(g_sa_q, msgs, sizeof(msgs));
    if (msgs[0]) {
      if (chats[0]) s_cat(chats, ",", sizeof(chats));
      s_cat(chats, msgs, sizeof(chats));
    }
    pf_render_ex("searchall", g_sa_q, 1, chats);
  }
  else if (s_eq(cmd, "searchall_tap")) go_tap(buf, "searchall_id");
  else if (s_eq(cmd, "rooms_settings")) screen_open("@screen.settings");
  else if (s_eq(cmd, "profile")) {
    char c[16] = ""; jstr(buf, "profile_call", c, sizeof(c));
    profile_show(c);
  }
  else if (s_eq(cmd, "profile_block")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) do_block(c);
  }
  else if (s_eq(cmd, "profile_unblock")) {
    char c[16] = ""; jstr(buf, "profile_target", c, sizeof(c));
    if (c[0]) do_unblock(c);
  }
  else if (s_eq(cmd, "reveal")) {
    /* 9.2.1: a person tapped a redacted bubble. Ask the core to open it by its
     * section 5 id, trying the remembered passphrases first (empty pass). The
     * answer arrives on xprs.unlock -- reveal, or a prompt for a passphrase. */
    char mid[24] = ""; jstr(buf, "reveal_mid", mid, sizeof(mid));
    if (mid[0]) hal_xprs_unlock(mid, s_len(mid), "", 0);
  }
  else if (s_eq(cmd, "prompt")) {
    char pid[56] = "", val[24] = "";
    jstr(buf, "prompt_id", pid, sizeof(pid));
    jstr(buf, "prompt_value", val, sizeof(val));
    if (s_pre(pid, "prof:")) {
      if (s_eq(val, "block")) do_block(pid + 5);
      else if (s_eq(val, "unblock")) do_unblock(pid + 5);
    }
    else if (s_pre(pid, "xgjoin:")) group_join(pid + 7);
    else if (s_eq(pid, "xgnew:")) {
      char nick[64] = ""; jstr(buf, "prompt_input", nick, sizeof(nick));
      group_create(nick);
    }
    else if (s_pre(pid, "xgfold:")) {
      char nick[64] = ""; jstr(buf, "prompt_input", nick, sizeof(nick));
      folder_create(pid + 7, nick);
    }
    else if (s_pre(pid, "xrpw:")) {
      /* A passphrase for a redacted message: try it, and the core remembers it
       * if it opens the message (9.2.1). */
      char pw[80] = ""; jstr(buf, "prompt_input", pw, sizeof(pw));
      hal_xprs_unlock(pid + 5, s_len(pid + 5), pw, s_len(pw));
    }
    else if (s_pre(pid, "xrmk:")) {
      /* The passphrase to obfuscate the message the author is composing. */
      char pw[80] = ""; jstr(buf, "prompt_input", pw, sizeof(pw));
      if (g_redact_text[0])
        hal_xprs_redact(pid + 5, s_len(pid + 5), g_redact_text,
                        s_len(g_redact_text), pw, s_len(pw));
      g_redact_text[0] = 0;
    }
  }
  /* The host's answer to ui.pick.picture: a thumbnail it made, by token. */
  else if (s_eq(cmd, "picture_picked")) {
    char pid[24] = "", token[96] = "";
    jstr(buf, "picture_id", pid, sizeof(pid));
    jstr(buf, "picture_token", token, sizeof(token));
    if (s_pre(pid, "gpic:")) picture_set(pid + 5, token);
  }
  else if (s_eq(cmd, "chan_apply")) {
    /* Explicit apply, so an unset checkbox serialised as false on some other
     * command never clobbers the on-by-default switch. */
    g_chan_local = jbool_def(buf, "chan_local", 1);
    chan_save();
    room_set_local_enabled(g_chan_local);
    notify("info", g_chan_local ? wtr("msg.local_on", "Local chat is on") : wtr("msg.local_off", "Local chat is off"));
  }
}

void module_destroy(void) { room_destroy(); }
