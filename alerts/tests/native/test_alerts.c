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

static void test_post_a_warning(void)
{
    mock_reset(); mock_kv_clear(); restart();
    command("{\"command\":\"post\",\"fields\":{\"a_type\":\"warning\",\"a_sev\":\"danger\","
            "\"a_kind\":\"flood\",\"a_text\":\"the lower car park floods\",\"a_for\":\"1w\","
            "\"a_here\":false,\"a_rad\":\"spot\"}}");
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
    CHECK(strstr(said_last("ui.people.set"), "Posted by you") != 0,
          "own alert not shown at once: %s", said_last("ui.people.set"));
    CHECK(said_count("\"notify\"") == 0, "announced its own alert");
}

static void test_wrong_type_is_refused(void)
{
    mock_reset(); mock_kv_clear(); restart();
    command("{\"command\":\"post\",\"fields\":{\"a_type\":\"info\",\"a_kind\":\"fire\","
            "\"a_text\":\"x\",\"a_for\":\"1d\"}}");
    CHECK(g_aired_n == 0, "a fire went out as a notice");
    CHECK(strstr(said_last("post_now"), "Not posted") != 0, "no reason shown");
    command("{\"command\":\"post\",\"fields\":{\"a_type\":\"warning\",\"a_sev\":\"watch\","
            "\"a_kind\":\"works\",\"a_text\":\"x\",\"a_for\":\"1d\"}}");
    CHECK(g_aired_n == 0, "works went out as a hazard");
}

static void test_too_long_is_refused(void)
{
    mock_reset(); mock_kv_clear(); restart();
    char cmd[1024], text[300];
    memset(text, 'a', 200);
    text[200] = 0;
    snprintf(cmd, sizeof cmd, "{\"command\":\"post\",\"fields\":{\"a_type\":\"info\","
             "\"a_kind\":\"fog\",\"a_text\":\"%s\",\"a_for\":\"1d\"}}", text);
    command(cmd);
    CHECK(g_aired_n == 0, "a packet too long to sign went out");
    CHECK(strstr(said_last("post_now"), "too long") != 0, "no reason: %s", said_last("post_now"));
}

static void test_post_with_a_place(void)
{
    mock_reset(); mock_kv_clear(); restart();
    command("{\"command\":\"post\",\"fields\":{\"a_type\":\"info\",\"a_kind\":\"fog\","
            "\"a_text\":\"\",\"a_for\":\"2d\",\"a_here\":true,\"a_rad\":\"500m\"}}");
    CHECK(g_aired_n == 0, "posted a place it does not have");
    g_lat = 387071234; g_lon = -91355432;
    command("{\"command\":\"post\",\"fields\":{\"a_type\":\"info\",\"a_kind\":\"fog\","
            "\"a_text\":\"\",\"a_for\":\"2d\",\"a_here\":true,\"a_rad\":\"500m\"}}");
    g_lat = g_lon = (int32_t)0x80000000;
    CHECK(g_aired_n == 1, "aired %d", g_aired_n);
    CHECK(strstr(g_aired[0], " pos:38.7071,-9.1355 rad:500m kind:fog scope:local ") != 0,
          "place: %s", g_aired[0]);
    CHECK(!strstr(g_aired[0], " m:"), "an empty m: went out: %s", g_aired[0]);
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
    command("{\"command\":\"post\",\"fields\":{\"a_type\":\"info\",\"a_kind\":\"closure\","
            "\"a_text\":\"east gate closed\",\"a_for\":\"1m\"}}");
    CHECK(g_aired_n == 1, "not posted");
    char id[7];
    al_id(g_aired[0], strlen(g_aired[0]), id);
    char until[24];
    al_field(g_aired[0], "until", until, sizeof until);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "{\"command\":\"alerts_tap\",\"fields\":{\"alerts_id\":\"%s\"}}", id);
    command(cmd);
    CHECK(strstr(said_last("withdraw__hidden"), "false") != 0, "withdraw not offered on our own");
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
}

int main(void)
{
    printf("alerts native tests\n");
    test_subscribes_both_topics();
    test_post_a_warning();
    test_wrong_type_is_refused();
    test_too_long_is_refused();
    test_post_with_a_place();
    test_announced_once();
    test_ended_is_hidden();
    test_planned();
    test_withdrawn_by_author_only();
    test_withdraw_own();
    test_time_round_trip();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
