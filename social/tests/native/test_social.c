/*
 * The Social wapp, driven by a mock HAL.
 *
 * What these are about: a post belongs on the screen the moment the packet
 * exists, and exactly once. Before this, the wapp did not handle the send at
 * all — the host published on its behalf — and the feed only learned of the
 * post when the archive's 20-second flush timer fired and the wapp re-read the
 * spool. The scenarios below pin both halves: the instant row, and the copies
 * that arrive afterwards collapsing onto its section 5 identifier.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* hal_mock.c */
void cap_clear(void);
int  cap_count(void);
int  cap_contains(const char *s);
int  cap_count_of(const char *s);
int  cap_time_numeric(void);
void inbox_set(const char *json);
void events_clear(void);
void subs_clear(void);
int  subscribed(const char *topic);
void event_push(const char *topic, const char *data);
void publish_clear(void);
void status_fails(int on);
const char *last_status_text(void);
const char *last_status_reply(void);
const char *last_status_id(void);
const char *last_wire(void);
int  status_calls(void);
int  send_calls(void);
void history_set(const char *json);
const char *last_query(void);
void ui_attached(int on);
void kv_clear(void);

/* main.c */
int32_t module_init(void);
int32_t module_tick(void);
int32_t module_tick_interval_ms(void);
int32_t module_handle_event(void);

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *what) {
    if (ok) { g_pass++; printf("  [PASS] %s\n", what); }
    else    { g_fail++; printf("  [FAIL] %s\n", what); }
}

static void reset(void) {
    cap_clear(); events_clear(); publish_clear(); kv_clear();
    status_fails(0); ui_attached(1); history_set("[]");
}

/* A delivered packet, shaped exactly as wapp_delivery.dart builds one —
 * `fields` array included, and `ts` as the WIRE timestamp rather than the
 * epoch the spool returns. The two doors disagree about that field, and a
 * fixture that quietly agreed with itself was how a live post came to be
 * dropped by the host as malformed JSON. */
static void deliver(const char *topic, const char *id, const char *from,
                    const char *wire) {
    char row[1600];
    snprintf(row, sizeof(row),
        "{\"id\":\"%s\",\"type\":\"%s\",\"from\":\"%s\",\"to\":\"\","
        "\"ts\":\"2026-09-10_09:00:00\","
        "\"fields\":[[\"t\",\"%s\"],[\"f\",\"%s\"],"
        "[\"ts\",\"2026-09-10_09:00:00\"]],"
        "\"forUs\":false,\"sealed\":false,\"obfuscated\":false,"
        "\"scope\":\"global\",\"bearer\":\"lan\",\"rssi\":0,\"via\":\"\","
        "\"link\":\"\",\"sig\":\"verified\",\"wire\":\"%s\"}",
        id, strcmp(topic, "xprs.reaction") == 0 ? "reaction" : "status",
        from, strcmp(topic, "xprs.reaction") == 0 ? "reaction" : "status",
        from, wire);
    event_push(topic, row);
}

int main(void) {
    printf("social native test\n");

    /* ── the subscriptions ── */
    reset(); subs_clear();
    module_init();
    check(subscribed("xprs.status"),
          "a status is subscribed to, so a post does not wait for a flush");
    check(subscribed("xprs.reaction"), "and a reaction with it");
    check(subscribed("core.archive"),
          "the spool event stays, for the backfill a restart needs");
    check(module_tick_interval_ms() == 0, "and no clock is asked for");

    /* ── a post is on the screen before anything is aired ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"hello mesh\"}");
    module_handle_event();
    check(status_calls() == 1, "the post reaches the core exactly once");
    check(strcmp(last_status_text(), "hello mesh") == 0,
          "with the words the person typed");
    check(last_status_reply()[0] == '\0', "and no parent: it is a new post");
    check(cap_contains("\"type\":\"ui.chat.append\""), "the row is drawn");
    check(cap_contains("\"dir\":\"out\""), "as ours");
    check(cap_contains("\"mid\":\"mine001\""),
          "keyed on the identifier the core handed back");
    check(cap_contains("hello mesh"), "and it says what was posted");
    check(cap_time_numeric(),
          "and its time is the epoch the feed sorts on, not a wire timestamp "
          "(which is not JSON, and the host drops the whole append)");

    /* ── and the copy that comes back later is not a second post ── */
    cap_clear();
    history_set("[{\"id\":\"mine001\",\"type\":\"status\",\"from\":\"X1SELF\","
                "\"ts\":\"2026-09-10_09:00:00\",\"sig\":\"verified\","
                "\"bearer\":\"lan\",\"own\":true,"
                "\"wire\":\"t:status f:X1SELF ts:2026-09-10_09:00:00 m:hello mesh\"}]");
    event_push("core.archive", "{\"topic\":\"core.archive\",\"rev\":7}");
    module_handle_event();
    check(cap_count_of("\"mid\":\"mine001\"") == 0,
          "the spool's copy of our own post is swallowed by the seen ring");

    /* ── an empty post is not a packet ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"\"}");
    module_handle_event();
    check(status_calls() == 0, "an empty composer sends nothing");
    check(!cap_contains("ui.chat.append"), "and draws nothing");

    /* ── a refusal is not a row ── */
    reset();
    status_fails(1);
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"nope\"}");
    module_handle_event();
    check(!cap_contains("ui.chat.append"),
          "a post the core refused is not shown as sent");
    check(cap_contains("\"type\":\"notify\""), "the person is told instead");

    /* ── somebody else's post, live ── */
    reset();
    deliver("xprs.status", "abc123", "X1FRND",
            "t:status f:X1FRND ts:2026-09-10_09:01:00 m:good morning");
    module_handle_event();
    check(cap_count_of("\"mid\":\"abc123\"") == 1,
          "a status from the air is appended once, with no spool read");
    check(cap_contains("\"dir\":\"in\""), "as theirs");
    check(cap_time_numeric(),
          "and its time is the epoch, converted from the wire timestamp the "
          "live door carries — the spool's rows already are one");
    check(cap_contains("\"t\":1789030800000"),
          "2026-09-10_09:00:00 UTC to the second");
    check(!cap_contains("\"type\":\"notify\""),
          "a stranger's post is not a notification");

    /* ── a reply in our conversation IS ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"anyone about?\"}");
    module_handle_event();
    cap_clear();
    deliver("xprs.status", "rep777", "X1FRND",
            "t:status f:X1FRND ts:2026-09-10_09:02:00 r:mine002 m:here");
    module_handle_event();
    check(cap_contains("\"type\":\"notify\""), "a reply to our post notifies");
    check(cap_contains("\"tag\":\"rep777\""),
          "tagged with the packet id, which the host dedupes once ever");
    check(cap_contains("X1FRND replied"), "and says who");
    check(cap_count_of("\"type\":\"notify\"") == 1, "exactly once");
    check(cap_contains("\"view\":\"post:mine002\""),
          "and names the THREAD to open — a tap that lands on the feed leaves "
          "the person to go and find what they were just told about");

    /* ── a like of our post is a card ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"a thought\"}");
    module_handle_event();
    cap_clear();
    deliver("xprs.reaction", "lik555", "X1FRND",
            "t:reaction f:X1FRND ts:2026-09-10_09:03:00 add:like r:mine003");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.activity.react\""), "the tally moves");
    check(cap_contains("liked your post"), "and the person is told");
    check(cap_contains("\"scope\":\"app\""),
          "in the app, not as a buzz in a pocket");
    check(cap_contains("\"view\":\"post:mine003\""),
          "opening the post that was liked");

    /* ── our own echo tells us nothing we did not know ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"mine\"}");
    module_handle_event();
    cap_clear();
    deliver("xprs.reaction", "own999", "X1SELF",
            "t:reaction f:X1SELF ts:2026-09-10_09:04:00 add:like r:mine004");
    module_handle_event();
    check(!cap_contains("\"type\":\"notify\""),
          "our own like of our own post raises nothing");

    /* ── the backfill never notifies ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"before\"}");
    module_handle_event();
    cap_clear();
    history_set("[{\"id\":\"old111\",\"type\":\"status\",\"from\":\"X1FRND\","
                "\"ts\":\"2026-09-10_08:00:00\",\"sig\":\"verified\","
                "\"bearer\":\"lan\",\"own\":false,"
                "\"wire\":\"t:status f:X1FRND ts:2026-09-10_08:00:00 r:mine005 m:hi\"}]");
    event_push("core.archive", "{\"topic\":\"core.archive\",\"rev\":9}");
    module_handle_event();
    check(cap_contains("\"mid\":\"old111\""), "the backfill still fills the feed");
    check(!cap_contains("\"type\":\"notify\""),
          "but a restart re-reading sixty rows is not sixty notifications");

    /* ── a station that was restarted still knows its own conversations ── */
    reset(); subs_clear();
    history_set("[{\"id\":\"old777\",\"type\":\"status\",\"from\":\"X1SELF\","
                "\"ts\":\"2026-09-09_20:00:00\",\"sig\":\"verified\","
                "\"bearer\":\"lan\",\"own\":true,"
                "\"wire\":\"t:status f:X1SELF ts:2026-09-09_20:00:00 m:yesterday\"}]");
    module_init();          /* a background engine: no page, ever */
    check(strcmp(last_query(), "{\"limit\":40,\"types\":[\"status\"]}") == 0,
          "the startup read asks for statuses, and says how long its query is "
          "(a wrong length is dropped by the host's parser, silently)");
    cap_clear();
    ui_attached(0);
    deliver("xprs.status", "ans777", "X1FRND",
            "t:status f:X1FRND ts:2026-09-10_09:07:00 r:old777 m:answering");
    module_handle_event();
    check(cap_contains("\"type\":\"notify\""),
          "a reply to a post made before this engine started still notifies: "
          "the threads that are ours are read from the spool at startup");
    check(cap_contains("\"tag\":\"ans777\""), "tagged by the packet");

    /* ...and the feed is NOT pre-marked as shown by that read. */
    reset();
    history_set("[{\"id\":\"old777\",\"type\":\"status\",\"from\":\"X1SELF\","
                "\"ts\":\"2026-09-09_20:00:00\",\"sig\":\"verified\","
                "\"bearer\":\"lan\",\"own\":true,"
                "\"wire\":\"t:status f:X1SELF ts:2026-09-09_20:00:00 m:yesterday\"}]");
    module_init();
    cap_clear();
    inbox_set("{\"command\":\"ready\"}");
    module_handle_event();
    check(cap_contains("\"mid\":\"old777\""),
          "opening the page still fills the feed with those same posts");

    /* ── the handover: a post made in the page, a reply after it closed ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"just posted\"}");
    module_handle_event();
    char mine[32];
    snprintf(mine, sizeof(mine), "%s", last_status_id());
    /* The page closes and a FRESH engine takes over. It shares no memory with
     * the one that made the post, and the post may not be in sqlite yet — the
     * archive flushes every 20 s — so the spool cannot answer for it either. */
    cap_clear();
    history_set("[]");
    module_init();
    ui_attached(0);
    char rwire[200];
    snprintf(rwire, sizeof(rwire),
             "t:status f:X1FRND ts:2026-09-10_09:09:00 r:%s m:hello?", mine);
    deliver("xprs.status", "han001", "X1FRND", rwire);
    module_handle_event();
    check(cap_contains("\"type\":\"notify\""),
          "a reply after the page closed still reaches the operator: the "
          "threads that are ours are written down, not held in one engine");
    check(cap_contains("\"view\":\"post:"),
          "and it still names the thread to open");

    /* ── following: the wapp owns the list and says so ── */
    reset();
    module_init();
    cap_clear();
    inbox_set("{\"command\":\"profile_follow\",\"profile_target\":\"X1ARKL\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"social.followstate\""),
          "following a callsign tells the host, which renders the tab");
    check(cap_contains("\"callsign\":\"X1ARKL\",\"on\":true"), "by name, on");
    check(cap_contains("\"field\":\"follows_list\""),
          "and the Following panel is redrawn");

    /* A post from them is labelled as theirs; a stranger's is not. */
    cap_clear();
    deliver("xprs.status", "fol001", "X1ARKL",
            "t:status f:X1ARKL ts:2026-09-10_11:00:00 m:hello from a friend");
    module_handle_event();
    check(cap_contains("\"source\":\"following\""),
          "a followed callsign's post is labelled following");
    cap_clear();
    deliver("xprs.status", "str001", "X3ZZZZ",
            "t:status f:X3ZZZZ ts:2026-09-10_11:00:01 m:hello from a stranger");
    module_handle_event();
    check(cap_contains("\"source\":\"xprs\"") &&
              !cap_contains("\"source\":\"following\""),
          "and a stranger's is not");

    /* Unfollowing says so too. */
    cap_clear();
    inbox_set("{\"command\":\"profile_unfollow\",\"profile_target\":\"X1ARKL\"}");
    module_handle_event();
    check(cap_contains("\"callsign\":\"X1ARKL\",\"on\":false"),
          "unfollowing tells the host too");

    /* ── the list survives, and is re-announced on ready ── */
    reset();                 /* a fresh engine, same kv */
    module_init();
    cap_clear();
    inbox_set("{\"command\":\"ready\"}");
    module_handle_event();
    check(!cap_contains("social.followstate"),
          "nobody followed: nothing to announce");

    reset();
    module_init();
    inbox_set("{\"command\":\"profile_follow\",\"profile_target\":\"X1ARKL\"}");
    module_handle_event();
    cap_clear();
    module_init();           /* the app restarts: a new engine, the same kv */
    check(cap_contains("\"callsign\":\"X1ARKL\",\"on\":true"),
          "a new engine announces the list AT INIT — the host's copy is memory "
          "only, and nothing in the host ever sends a `ready` command, so "
          "waiting for one left the Following tab empty on every launch");

    /* ── an npub is not a callsign ── */
    reset();
    module_init();
    cap_clear();
    inbox_set("{\"command\":\"follow_add\",\"follow_input\":"
              "\"npub1arklg83wnqy4s6stnl65hl9dt980xf\"}");
    module_handle_event();
    check(!cap_contains("social.followstate"),
          "a profile sheet handing over an npub follows nobody: it would sit "
          "in the list forever matching no post");

    /* ── a reply goes out as a status naming its parent ── */
    reset();
    inbox_set("{\"command\":\"activity_reply\",\"activity_target_mid\":\"abc123\","
              "\"activity_input\":\"answering you\"}");
    module_handle_event();
    check(status_calls() == 1, "a reply is published by the core, not composed here");
    check(send_calls() == 0, "no hand-built wire leaves this wapp");
    check(strcmp(last_status_reply(), "abc123") == 0, "and it names its parent");
    check(cap_contains("\"parent\":\"abc123\""),
          "the row is threaded on the parent at once");
    check(cap_contains("\"dir\":\"out\""), "as ours");

    /* ── nobody is looking ── */
    reset();
    ui_attached(0);
    deliver("xprs.status", "hid001", "X1FRND",
            "t:status f:X1FRND ts:2026-09-10_09:05:00 m:unseen");
    module_handle_event();
    check(!cap_contains("\"mid\":\"hid001\""),
          "with the page detached nothing is appended (opening it refills)");

    /* ── ...which is exactly when a notification matters ── */
    reset();
    inbox_set("{\"command\":\"activity_send\",\"activity_input\":\"out and about\"}");
    module_handle_event();
    char wire[200];
    snprintf(wire, sizeof(wire),
             "t:status f:X1FRND ts:2026-09-10_09:06:00 r:%s m:knock knock",
             last_status_id());
    cap_clear();
    ui_attached(0);
    deliver("xprs.status", "det001", "X1FRND", wire);
    module_handle_event();
    check(cap_contains("\"type\":\"notify\""),
          "a reply raises a notification with the page closed — which is what "
          "a notification is for");
    check(!cap_contains("ui.chat.append"), "and still draws nothing");

    /* ── a like still reaches the core as a reaction ── */
    reset();
    inbox_set("{\"command\":\"activity_like\",\"activity_mid\":\"abc123\","
              "\"activity_set\":\"1\"}");
    module_handle_event();
    check(send_calls() == 1, "a like is a t:reaction on the air");
    check(strstr(last_wire(), "add:like r:abc123") != NULL, "naming the post");
    check(cap_contains("\"type\":\"ui.activity.react\""),
          "and the tally moves without waiting for the spool");

    printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
