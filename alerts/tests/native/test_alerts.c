/*
 * The Alerts wapp against a mock HAL (XPRS.md 24, 25, 25.3).
 *
 * What these hold the wapp to:
 *   - a posted alert is scope:local with until: from the duration picked,
 *     fits 250 bytes once signed, and shows at once as the phone's own;
 *   - a hazard is not posted as a notice, nor a notice as a hazard;
 *   - a heard alert is announced once per id, across a restart too, and a
 *     second copy of it changes nothing;
 *   - an ended alert is not shown and not announced, and a planned one is
 *     shown as planned;
 *   - a withdrawal hides an alert only when its author sent it, and the
 *     phone's own withdrawal carries the alert's until:;
 *   - with no clock nothing is ended by time.
 */
#include <stdio.h>
#include <string.h>

#include "../../main.c"

extern char g_said[256][2048];
extern int g_said_n;
extern char g_aired[16][300];
extern int g_aired_n;
extern int32_t g_send_rc;
extern char g_cmd[4096];
extern uint64_t g_now;
extern int32_t g_lat, g_lon;
extern char g_history[8192];
extern char g_subs[8][64];
extern int g_subs_n;
void mock_reset(void);
void mock_kv_clear(void);
void mock_event(const char *topic, const char *json);

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    printf("  FAIL %s:%d  ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int said_count(const char *needle)
{
    int n = 0;
    for (int i = 0; i < g_said_n; i++) if (strstr(g_said[i], needle)) n++;
    return n;
}

static const char *said_last(const char *needle)
{
    for (int i = g_said_n - 1; i >= 0; i--) if (strstr(g_said[i], needle)) return g_said[i];
    return "";
}

/* A fresh engine: the wapp's statics as module_init finds them, the kv
 * kept unless the test clears it (a restart keeps it). */
static void restart(void)
{
    memset(g_al, 0, sizeof g_al);
    g_nal = 0;
    g_sel = -1;
    memset(g_gone, 0, sizeof g_gone);
    g_gone_w = 0;
    memset(g_ver, 0, sizeof g_ver);
    g_ver_w = 0;
    g_hist_open = 0;
    memset(g_seen, 0, sizeof g_seen);
    g_seen_w = 0;
    g_said_n = 0;
    module_init();
}

static void command(const char *json)
{
    snprintf(g_cmd, sizeof g_cmd, "%s", json);
    module_handle_event();
}

static void heard(const char *topic, const char *id, const char *wire)
{
    char row[1024];
    snprintf(row, sizeof row,
             "{\"id\":\"%s\",\"type\":\"%s\",\"sig\":\"verified\",\"wire\":\"%s\"}",
             id, topic + 5, wire);
    mock_event(topic, row);
    module_handle_event();
}

static void test_subscribes_both_topics(void)
{
    mock_reset(); mock_kv_clear(); restart();
    int w = 0, i = 0;
    for (int k = 0; k < g_subs_n; k++) {
        if (!strcmp(g_subs[k], "xprs.warning")) w = 1;
        if (!strcmp(g_subs[k], "xprs.info")) i = 1;
    }
    CHECK(w && i, "not subscribed to xprs.warning and xprs.info");
}

/* The Post screens, one answer each, as a person taps through them. */
static void tap(const char *field, const char *id)
{
    char c[300];
    snprintf(c, sizeof c, "{\"command\":\"%s_tap\",\"fields\":{\"%s_id\":\"%s\"}}", field, field, id);
    command(c);
}

static void write_msg(const char *text)
{
    char c[600];
    snprintf(c, sizeof c, "{\"command\":\"p_next\",\"fields\":{\"a_text\":\"%s\"}}", text);
    command(c);
}

static void test_post_a_warning(void)
{
    mock_reset(); mock_kv_clear(); restart();
    /* The + opens New alert by itself: its tiles must already be there. */
    CHECK(strstr(said_last("\"p_type\""), "\"Warning\"") != 0, "no type tiles: %s", said_last("p_type"));
    tap("p_type", "warning");
    CHECK(strstr(said_last("ui.screen.open"), "\"Kind\"") != 0, "not asked what it is");
    CHECK(strstr(said_last("\"p_kind\""), "\"Flood\"") != 0, "no flood tile");
    tap("p_kind", "flood");
    CHECK(strstr(said_last("ui.screen.open"), "\"Severity\"") != 0, "not asked how bad");
    tap("p_sev", "danger");
    CHECK(strstr(said_last("ui.screen.open"), "\"Message\"") != 0, "not asked the message");
    write_msg("the lower car park floods");
    CHECK(strstr(said_last("ui.screen.open"), "\"Duration\"") != 0, "not asked how long");
    CHECK(strstr(said_last("\"p_for\""), "until ") != 0, "no end time on the tiles: %s", said_last("p_for"));
    CHECK(strstr(said_last("\"p_for\""), "2027") != 0, "a year on does not say which year");
    tap("p_for", "1w");
    CHECK(strstr(said_last("ui.screen.open"), "\"Place\"") != 0, "not asked where");
    tap("p_where", "none");
    CHECK(strstr(said_last("ui.screen.open"), "\"Review\"") != 0, "no review");
    CHECK(strstr(said_last("post__hidden"), "false") != 0, "Post hidden on a good draft");
    CHECK(g_aired_n == 0, "aired before Post was pressed");
    command("{\"command\":\"post\"}");
    CHECK(g_aired_n == 1, "aired %d wires", g_aired_n);
    const char *w = g_aired[0];
    CHECK(!strncmp(w, "t:warning f:X1ME77 kind:flood sev:danger scope:local ts:", 56),
          "wire head: %s", w);
    char ts[24], until[24];
    al_field(w, "ts", ts, sizeof ts);
    al_field(w, "until", until, sizeof until);
    CHECK(al_epoch(until) - al_epoch(ts) == 7ULL * 86400ULL, "until is not a week on: %s", w);
    CHECK(strlen(w) + 65 <= 250, "does not fit signed: %zu", strlen(w));
    CHECK(!strstr(w, "pos:"), "a position without being asked: %s", w);
    CHECK(strstr(w, " m:the lower car park floods") != 0, "message lost: %s", w);
    const char *l = said_last("ui.people.set");
    CHECK(strstr(l, "\"Yours\"") && strstr(l, "Danger: flood") && strstr(l, "\"block\""),
          "own alert not shown at once, as itself: %s", l);
    CHECK(strstr(said_last("ui.screen"), "ui.screen.close") != 0, "the review stayed open");
    CHECK(said_count("\"notify\"") == 0, "announced its own alert");
}

static void test_notice_skips_how_bad(void)
{
    mock_reset(); mock_kv_clear(); restart();
    tap("p_type", "info");
    tap("p_kind", "fire");                 /* a hazard is not a notice */
    CHECK(!strstr(said_last("ui.screen.open"), "Severity") && !strstr(said_last("ui.screen.open"), "Message"),
          "a fire was taken as a notice");
    tap("p_kind", "closure");
    CHECK(strstr(said_last("ui.screen.open"), "\"Message\"") != 0, "a notice was asked how bad");
}

static void test_review_changes_one_answer(void)
{
    mock_reset(); mock_kv_clear(); restart();
    tap("p_type", "info"); tap("p_kind", "works"); write_msg("east path");
    tap("p_for", "1d"); tap("p_where", "none");
    tap("p_review", "Duration");
    CHECK(strstr(said_last("ui.screen.open"), "\"Duration\"") != 0, "the tile did not lead to its question");
    tap("p_for", "1m");
    CHECK(strstr(said_last("ui.screen.open"), "\"Review\"") != 0, "did not come back to the review");
    CHECK(strstr(said_last("\"p_review\""), "1 month") != 0, "the change is not shown");
    command("{\"command\":\"post\"}");
    char ts[24], until[24];
    al_field(g_aired[0], "ts", ts, sizeof ts);
    al_field(g_aired[0], "until", until, sizeof until);
    CHECK(al_epoch(until) - al_epoch(ts) == 30ULL * 86400ULL, "a month was not posted: %s", g_aired[0]);
}

static void test_too_long_is_refused(void)
{
    mock_reset(); mock_kv_clear(); restart();
    g_lat = 387071234; g_lon = -91355432;
    tap("p_type", "warning"); tap("p_kind", "landslide"); tap("p_sev", "warning");
    write_msg("rocks are coming down onto the coastal path past the second bend");
    tap("p_for", "6m"); tap("p_where", "500m");
    g_lat = g_lon = (int32_t)0x80000000;
    CHECK(strstr(said_last("post__hidden"), "true") != 0, "Post offered for a packet too long to sign");
    CHECK(strstr(said_last("\"p_msg\""), "too long") != 0, "no reason: %s", said_last("p_msg"));
    command("{\"command\":\"post\"}");
    CHECK(g_aired_n == 0, "a packet too long to sign went out");
}

static void test_post_with_a_place(void)
{
    mock_reset(); mock_kv_clear(); restart();
    tap("p_type", "info"); tap("p_kind", "fog"); write_msg("");
    tap("p_for", "2d"); tap("p_where", "500m");
    CHECK(strstr(said_last("post__hidden"), "true") != 0, "offered to post a place it does not have");
    g_lat = 387071234; g_lon = -91355432;
    tap("p_review", "Place"); tap("p_where", "500m");
    command("{\"command\":\"post\"}");
    g_lat = g_lon = (int32_t)0x80000000;
    CHECK(g_aired_n == 1, "aired %d", g_aired_n);
    CHECK(strstr(g_aired[0], " pos:38.7071,-9.1355 rad:500m kind:fog scope:local ") != 0,
          "place: %s", g_aired[0]);
    CHECK(!strstr(g_aired[0], " m:"), "an empty m: went out: %s", g_aired[0]);
}

static void test_closing_abandons_the_draft(void)
{
    mock_reset(); mock_kv_clear(); restart();
    tap("p_type", "warning"); tap("p_kind", "fire");
    command("{\"command\":\"screen_closed\"}");
    command("{\"command\":\"post\"}");
    CHECK(g_aired_n == 0, "an abandoned draft was posted");
}

#define HEARD_FLOOD "t:warning f:X1QZ3N kind:flood sev:warning scope:local " \
                    "ts:2026-08-16_10:00:00 until:2026-08-23_10:00:00 sig:abc m:car park"

static void test_announced_once(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    CHECK(said_count("\"notify\"") == 1, "announced %d times", said_count("\"notify\""));
    CHECK(strstr(said_last("\"notify\""), "\"level\":\"warning\"") != 0, "not loud: %s",
          said_last("\"notify\""));
    CHECK(strstr(said_last("\"notify\""), "alerts.a1b2c3") != 0, "no tag");
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD " via:X3HW9U");
    CHECK(said_count("\"notify\"") == 1, "a second copy was announced again");
    /* A restart reads it back from the archive and says nothing. */
    snprintf(g_history, sizeof g_history,
             "[{\"id\":\"a1b2c3\",\"sig\":\"verified\",\"wire\":\"%s\"}]", HEARD_FLOOD);
    restart();
    CHECK(said_count("\"notify\"") == 0, "announced again after a restart");
    CHECK(strstr(said_last("ui.people.set"), "\"Now\"") != 0, "not listed after a restart");
}

/* A start after the first reads what the wapp kept, not the archive: the
 * archive walks every row for a rare type, on the UI isolate. */
extern int g_history_calls;
static void test_restart_does_not_read_the_archive(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    heard("xprs.warning", "eeeeee", "t:warning f:X1QZ3N scope:local ts:2026-08-16_11:00:00 "
          "until:2026-08-23_10:00:00 r:a1b2c3 remove:warning");
    heard("xprs.info", "0d0e0f", "t:info f:X1RD89 kind:fog scope:local "
          "ts:2026-08-16_10:00:00 until:2026-08-18_10:00:00 m:fog on the hill");
    g_history_calls = 0;
    strcpy(g_history, "[]");
    restart();
    CHECK(g_history_calls == 0, "the archive was read on a second start");
    const char *l = said_last("ui.people.set");
    CHECK(strstr(l, "fog on the hill") != 0, "a kept alert was lost: %s", l);
    CHECK(!strstr(l, "car park"), "a kept withdrawal was lost: %s", l);
    CHECK(said_count("\"notify\"") == 0, "a kept alert was announced again");
}

static void test_ended_is_hidden(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.info", "d4e5f6", "t:info f:X1QZ3N kind:fog scope:local "
          "ts:2026-08-01_10:00:00 until:2026-08-02_10:00:00 m:old fog");
    CHECK(said_count("\"notify\"") == 0, "an ended alert was announced");
    CHECK(!strstr(said_last("ui.people.set"), "old fog"), "an ended alert is listed");
    /* No clock: nothing is ended by time. */
    al_alert_t a;
    al_parse("t:info f:X1QZ3N kind:fog ts:2026-08-01_10:00:00 until:2026-08-02_10:00:00", &a);
    CHECK(al_state(&a, 0) == AL_NOW, "ended without a clock");
}

static void test_planned(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.info", "0a0b0c", "t:info f:X1QZ3N kind:works scope:local "
          "ts:2026-08-16_10:00:00 since:2026-08-20_07:00:00 until:2026-08-27_18:00:00 m:roadworks");
    const char *l = said_last("ui.people.set");
    CHECK(strstr(l, "\"Planned\"") && strstr(l, "roadworks"), "not planned: %s", l);
}

static void test_withdrawn_by_author_only(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    heard("xprs.warning", "ffffff", "t:warning f:X1EVIL scope:local ts:2026-08-16_11:00:00 "
          "until:2026-08-23_10:00:00 r:a1b2c3 remove:warning");
    CHECK(strstr(said_last("ui.people.set"), "car park") != 0, "a stranger withdrew it");
    heard("xprs.warning", "eeeeee", "t:warning f:X1QZ3N scope:local ts:2026-08-16_11:00:00 "
          "until:2026-08-23_10:00:00 r:a1b2c3 remove:warning");
    CHECK(!strstr(said_last("ui.people.set"), "car park"), "its author's withdrawal ignored");
    /* The withdrawal can come first, as the archive reads newest first. */
    snprintf(g_history, sizeof g_history,
             "[{\"id\":\"eeeeee\",\"sig\":\"verified\",\"wire\":\"t:warning f:X1QZ3N scope:local "
             "ts:2026-08-16_11:00:00 until:2026-08-23_10:00:00 r:a1b2c3 remove:warning\"},"
             "{\"id\":\"a1b2c3\",\"sig\":\"verified\",\"wire\":\"%s\"}]", HEARD_FLOOD);
    mock_kv_clear();
    restart();
    CHECK(!strstr(said_last("ui.people.set"), "car park"), "withdrawn, then read back alive");
    CHECK(said_count("\"notify\"") == 0, "a withdrawn alert was announced");
}

static void test_withdraw_own(void)
{
    mock_reset(); mock_kv_clear(); restart();
    tap("p_type", "info"); tap("p_kind", "closure"); write_msg("east gate closed");
    tap("p_for", "1m"); tap("p_where", "none");
    command("{\"command\":\"post\"}");
    CHECK(g_aired_n == 1, "not posted");
    char id[7];
    al_id(g_aired[0], strlen(g_aired[0]), id);
    char until[24];
    al_field(g_aired[0], "until", until, sizeof until);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "{\"command\":\"alerts_tap\",\"fields\":{\"alerts_id\":\"%s\"}}", id);
    command(cmd);
    CHECK(strstr(said_last("withdraw__hidden"), "false") != 0, "withdraw not offered on our own");
    CHECK(strstr(said_last("\"a_msg\""), "east gate closed") != 0, "the message is not shown");
    CHECK(strstr(said_last("\"a_hub\""), "1 month") || strstr(said_last("\"a_hub\""), "30 days"),
          "time left not shown: %s", said_last("a_hub"));
    command("{\"command\":\"withdraw\"}");
    CHECK(g_aired_n == 2, "no withdrawal aired");
    char want[80];
    snprintf(want, sizeof want, " until:%s r:%s remove:info", until, id);
    CHECK(strstr(g_aired[1], want) != 0, "withdrawal: %s (want %s)", g_aired[1], want);
    CHECK(!strstr(said_last("ui.people.set"), "east gate"), "still listed after withdrawing");
}

static void test_time_round_trip(void)
{
    char ts[24];
    for (unsigned long long e = 951782400ULL; e < 4102444800ULL; e += 86400ULL * 37 + 3601) {
        al_stamp(ts, sizeof ts, e);
        if (al_epoch(ts) != e) { CHECK(0, "%llu -> %s -> %llu", e, ts, al_epoch(ts)); break; }
    }
    CHECK(al_epoch("2026-08-08_14:26:40") == 1786199200ULL, "a known stamp");
    CHECK(al_epoch("not a time") == 0, "nonsense read as a time");
    char w[32];
    al_when(w, sizeof w, 1786199200ULL, 120);          /* 2026-08-08 14:26:40 UTC */
    CHECK(!strcmp(w, "Sat 8 Aug, 16:26"), "local time: %s", w);
    al_span(w, sizeof w, 47ULL * 3600ULL);
    CHECK(!strcmp(w, "2 days"), "47 hours read as %s", w);
    al_span(w, sizeof w, 3600ULL);
    CHECK(!strcmp(w, "60 minutes"), "an hour read as %s", w);
}

#define EDIT_FLOOD(ts, sev, who, m) "t:warning f:" who " kind:flood sev:" sev " scope:local " \
        "ts:" ts " until:2026-08-23_10:00:00 edit:a1b2c3 sig:abc m:" m

static void test_edit_by_author_only(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    /* A stranger's correction, and the author's own on a different type,
     * change nothing (7.8.1). */
    heard("xprs.warning", "e00001", EDIT_FLOOD("2026-08-16_12:00:00", "danger", "X1EVIL", "gone"));
    CHECK(strstr(said_last("ui.people.set"), "car park") != 0, "a stranger edited it");
    heard("xprs.warning", "e00002", EDIT_FLOOD("2026-08-16_12:00:00", "danger", "X1QZ3N", "lower car park floods"));
    CHECK(strstr(said_last("ui.people.set"), "lower car park floods") != 0, "the edit is not shown");
    CHECK(strstr(said_last("ui.people.set"), "edited") != 0, "no edited mark on the row");
    CHECK(g_nal == 1, "an edit became a second alert");
    /* An edit not later than the original is discarded. */
    heard("xprs.warning", "e00003", EDIT_FLOOD("2026-08-16_09:00:00", "watch", "X1QZ3N", "earlier"));
    CHECK(strstr(said_last("ui.people.set"), "earlier") == 0, "an earlier edit won");
    /* The newest wins, whatever the order it arrived in. */
    heard("xprs.warning", "e00005", EDIT_FLOOD("2026-08-16_14:00:00", "danger", "X1QZ3N", "third"));
    heard("xprs.warning", "e00004", EDIT_FLOOD("2026-08-16_13:00:00", "danger", "X1QZ3N", "second"));
    CHECK(strstr(said_last("ui.people.set"), "third") != 0, "the newest edit is not shown");
    /* A copy of an edit changes nothing. */
    int before = g_said_n;
    heard("xprs.warning", "e00005", EDIT_FLOOD("2026-08-16_14:00:00", "danger", "X1QZ3N", "third"));
    CHECK(g_said_n == before, "a copy redrew");
}

static void test_edit_before_original(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "e00002", EDIT_FLOOD("2026-08-16_12:00:00", "danger", "X1QZ3N", "lower car park floods"));
    CHECK(g_nal == 0, "an edit alone became an alert");
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    CHECK(strstr(said_last("ui.people.set"), "lower car park floods") != 0, "the early edit was lost");
    /* ... and a forged-looking one from another author arriving first is
     * dropped when the original shows whose it is. */
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "e00001", EDIT_FLOOD("2026-08-16_12:00:00", "danger", "X1EVIL", "gone"));
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    CHECK(strstr(said_last("ui.people.set"), "car park") != 0, "another author's edit won");
}

static void test_edit_own_and_history(void)
{
    mock_reset(); mock_kv_clear(); restart();
    tap("p_type", "info"); tap("p_kind", "closure"); write_msg("east gate closed");
    tap("p_for", "1m"); tap("p_where", "none");
    command("{\"command\":\"post\"}");
    CHECK(g_aired_n == 1, "not posted");
    char id[7];
    al_id(g_aired[0], strlen(g_aired[0]), id);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "{\"command\":\"alerts_tap\",\"fields\":{\"alerts_id\":\"%s\"}}", id);
    command(cmd);
    CHECK(strstr(said_last("edit__hidden"), "false") != 0, "edit not offered on our own");
    CHECK(strstr(said_last("history__hidden"), "true") != 0, "history offered before any edit");
    command("{\"command\":\"edit\"}");
    CHECK(strstr(said_last("\"p_review\""), "Unchanged") != 0, "the review does not keep duration");
    CHECK(strstr(said_last("save__hidden"), "false") != 0, "save not offered");
    CHECK(strstr(said_last("post__hidden"), "true") != 0, "post offered while editing");
    /* Change the words through the review's Message tile. */
    tap("p_review", "Message");
    g_now += 120;
    write_msg("east gate open again");
    command("{\"command\":\"save\"}");
    CHECK(g_aired_n == 2, "no edit aired");
    char want[40];
    snprintf(want, sizeof want, " edit:%s", id);
    CHECK(strstr(g_aired[1], want) != 0, "edit does not name the original: %s", g_aired[1]);
    CHECK(strstr(g_aired[1], "m:east gate open again") != 0, "edit words: %s", g_aired[1]);
    CHECK(strlen(g_aired[1]) <= AL_WIRE_UNSIGNED, "edit too long to sign");
    CHECK(strstr(said_last("ui.screen.open"), "\"name\":\"Alert\"") != 0,
          "saving did not land back on the alert");
    CHECK(g_nal == 1, "the edit became a second row");
    CHECK(strstr(said_last("ui.people.set"), "east gate open again") != 0, "row shows the old words");
    CHECK(strstr(said_last("\"a_hub\""), "Edited") != 0, "alert does not say edited");
    command("{\"command\":\"history\"}");
    const char *h = said_last("\"a_hist\"");
    CHECK(strstr(h, "Original") && strstr(h, "east gate closed") &&
          strstr(h, "Edit -") && strstr(h, "east gate open again"), "history: %s", h);
    CHECK(strstr(h, "east gate closed") < strstr(h, "east gate open again"), "history out of order");
    /* The History screen closing leaves the alert selected. */
    command("{\"command\":\"screen_closed\"}");
    CHECK(g_sel >= 0, "closing the history dropped the alert");
    CHECK(strstr(said_last("ui.screen.open"), "\"name\":\"Alert\"") != 0,
          "closing the history did not put the alert back in front");
    /* A restart replays the ring: still edited. */
    restart();
    CHECK(strstr(said_last("ui.people.set"), "east gate open again") != 0, "restart lost the edit");
    /* Withdrawing destroys the words of every version. */
    snprintf(cmd, sizeof cmd, "{\"command\":\"alerts_tap\",\"fields\":{\"alerts_id\":\"%s\"}}", id);
    command(cmd);
    command("{\"command\":\"withdraw\"}");
    for (int k = 0; k < VER_MAX; k++)
        CHECK(!g_ver[k].id[0] || !g_ver[k].m[0], "a version kept its words after withdrawal");
}

static void test_edit_announced_and_withdrawal_scrubs_ring(void)
{
    mock_reset(); mock_kv_clear(); restart();
    heard("xprs.warning", "a1b2c3", HEARD_FLOOD);
    int n0 = said_count("\"notify\"");
    heard("xprs.warning", "e00002", EDIT_FLOOD("2026-08-16_12:00:00", "danger", "X1QZ3N", "lower car park floods"));
    CHECK(said_count("\"notify\"") == n0 + 1, "a correction by somebody else was not announced");
    CHECK(strstr(said_last("\"notify\""), "updated") != 0, "and it does not say updated");
    heard("xprs.warning", "e00002", EDIT_FLOOD("2026-08-16_12:00:00", "danger", "X1QZ3N", "lower car park floods"));
    CHECK(said_count("\"notify\"") == n0 + 1, "a copy of it was announced again");
    heard("xprs.warning", "eeeeee", "t:warning f:X1QZ3N scope:local ts:2026-08-16_13:00:00 "
          "until:2026-08-23_10:00:00 r:a1b2c3 remove:warning");
    for (int i = 0; i < KEEP_MAX; i++) {
        char k[4], v[AL_WIRE_MAX + 16];
        keep_key(k, i);
        uint32_t l = hal_kv_get(k, 3, v, sizeof v - 1);
        if (l >= sizeof v) continue;
        v[l] = 0;
        CHECK(!strstr(v, "car park"), "slot %d still holds the words of a withdrawn alert", i);
    }
}

int main(void)
{
    printf("alerts native tests\n");
    test_subscribes_both_topics();
    test_post_a_warning();
    test_notice_skips_how_bad();
    test_review_changes_one_answer();
    test_too_long_is_refused();
    test_post_with_a_place();
    test_closing_abandons_the_draft();
    test_announced_once();
    test_restart_does_not_read_the_archive();
    test_ended_is_hidden();
    test_planned();
    test_withdrawn_by_author_only();
    test_withdraw_own();
    test_edit_by_author_only();
    test_edit_before_original();
    test_edit_own_and_history();
    test_edit_announced_and_withdrawal_scrubs_ring();
    test_time_round_trip();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
