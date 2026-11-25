/*
 * Check-in, natively: the packet rules (ci.c), then the whole module against
 * the mock HAL, playing both ends. The station answers an ask the way the
 * core hands it one (an xprs.command row); the chaser's own ask is carried
 * to the station and the station's certificate back, as the core would.
 */
#include <stdio.h>
#include <string.h>
#include "../../main.c"

extern char g_mock_call[16];
extern uint64_t g_now;
extern int32_t g_lat, g_lon;
extern char g_said[512][4096];
extern int g_said_n;
extern char g_aired[64][300];
extern int g_aired_n;
extern char g_msg_to[16], g_msg_text[200];
extern int g_msg_n;
extern char g_fetched[16][96];
extern int g_fetched_n;
extern char g_put_token[96];
extern char g_cmd[4096];
extern char g_stations[4096];
extern char g_history[8192];
extern char g_shared[96];
extern char g_followed_kind[64];
extern char g_identities[16384];
extern char g_id_query[256];
extern int g_id_queries;
int mock_subscribed(const char *t);
void mock_event(const char *topic, const char *json);
void mock_clear(void);

static int g_fail, g_pass;
#define CHECK(c, what) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static const char *REF = "Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.png";
static const char *EREF = "Ev3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.png";

static void as(const char *call) { snprintf(g_mock_call, sizeof g_mock_call, "%s", call); }
static void cmd(const char *json) { snprintf(g_cmd, sizeof g_cmd, "%s", json); module_handle_event(); }
static void event(const char *topic, const char *json) { mock_event(topic, json); module_handle_event(); }

static const char *last_aired(void) { return g_aired_n ? g_aired[g_aired_n - 1] : ""; }
static int said(const char *needle)
{
    for (int i = 0; i < g_said_n; i++) if (strstr(g_said[i], needle)) return 1;
    return 0;
}

/* The row the core publishes on xprs.command for [wire]. */
static void ask_row(char *out, unsigned cap, const char *wire, const char *sig,
                    const char *bearer, int in_range, const char *via)
{
    char id[7], from[16] = "", to[16] = "", ts[24] = "", scope[12] = "global";
    al_id(wire, al_len(wire), id);
    al_field(wire, "f", from, sizeof from);
    al_field(wire, "d", to, sizeof to);
    al_field(wire, "ts", ts, sizeof ts);
    al_field(wire, "scope", scope, sizeof scope);
    snprintf(out, cap,
             "{\"id\":\"%s\",\"type\":\"command\",\"from\":\"%s\",\"to\":\"%s\",\"ts\":\"%s\","
             "\"forUs\":true,\"scope\":\"%s\",\"inRange\":%s,\"bearer\":\"%s\",\"rssi\":-61,"
             "\"via\":\"%s\",\"sig\":\"%s\",\"wire\":\"%s\"}",
             id, from, to, ts, scope, in_range ? "true" : "false", bearer, via, sig, wire);
}

static void result_row(char *out, unsigned cap, const char *wire)
{
    char id[7], from[16] = "";
    al_id(wire, al_len(wire), id);
    al_field(wire, "f", from, sizeof from);
    snprintf(out, cap,
             "{\"id\":\"%s\",\"type\":\"result\",\"from\":\"%s\",\"forUs\":true,"
             "\"bearer\":\"ble\",\"sig\":\"verified\",\"wire\":\"%s\"}", id, from, wire);
}

static void test_rules(void)
{
    char w[700];
    int n = ci_build_ask(w, sizeof w, "X1CHSR", "X3STN1", 1786960000ULL, "local");
    CHECK(n > 0 && !strcmp(w, "t:command f:X1CHSR d:X3STN1 ts:2026-08-17_09:46:40 cmd:checkin scope:local"),
          "the ask is an ordinary command");
    ci_build_ask(w, sizeof w, "X1CHSR", "X3STN1", 1786960000ULL, "global");
    CHECK(strstr(w, "scope:local") != 0, "no ask wider than regional is ever built");

    ci_ask_t a;
    memset(&a, 0, sizeof a);
    strcpy(a.id, "abc123"); strcpy(a.from, "X1CHSR"); strcpy(a.scope, "local");
    strcpy(a.sig, "verified"); strcpy(a.bearer, "ble"); a.in_range = 1; a.ts = 1000;
    ci_rules_t r = { 1 };
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_OK, "a fresh, verified, in-range ask is certified");
    CHECK(ci_decide(&a, &r, 1100, 1) == CI_AGAIN_LATER, "once a day");
    CHECK(ci_decide(&a, &r, 1000 + CI_FRESH_S + 1, 0) == CI_STALE, "an old ask is stale");
    CHECK(ci_decide(&a, &r, 1000 - CI_SKEW_S - 1, 0) == CI_STALE, "an ask from the future is stale");
    strcpy(a.sig, "unverified");
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_WAIT, "an ask we cannot check yet waits for the key");
    strcpy(a.sig, "forged");
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_REFUSED, "a forged ask proves nobody");
    strcpy(a.sig, "unsigned");
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_REFUSED, "nor does an unsigned one");
    strcpy(a.sig, "verified"); a.in_range = 0;
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_REFUSED, "heard out of its scope's reach");
    a.in_range = 1; strcpy(a.scope, "global");
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_REFUSED, "a global ask is not a meeting");
    strcpy(a.scope, "regional"); strcpy(a.bearer, "lora");
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_REFUSED, "in person only refuses LoRa");
    r.local_only = 0;
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_OK, "regional allowed takes LoRa");
    strcpy(a.bearer, "rns");
    CHECK(ci_decide(&a, &r, 1100, 0) == CI_REFUSED, "Reticulum is never a meeting");

    strcpy(a.bearer, "lora"); strcpy(a.via, "X3RLY7"); a.rssi = -112;
    n = ci_build_result(w, sizeof w, "X3STN1", &a, 1786960001ULL, CI_OK,
                        "file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.png", "field-day", "38.7071,-9.1393");
    CHECK(n > 0 && n <= CI_WIRE_UNSIGNED, "the certificate fits once signed");
    CHECK(strstr(w, "t:result f:X3STN1 d:X1CHSR ts:2026-08-17_09:46:41 r:abc123 code:200 scope:regional link:lora pos:38.7071,-9.1393 route:X3RLY7 file:") == w,
          "the head, the radio and the place first");
    CHECK(ci_build_result(w, sizeof w, "X3STN1", &a, 1786960001ULL, CI_OK, REF, "", "") < 0 &&
          ci_build_result(w, sizeof w, "X3STN1", &a, 1786960001ULL, CI_OK, REF, "", "91,0") < 0,
          "no certificate without a valid place");
    n = ci_build_result(w, sizeof w, "X3STN1", &a, 1786960001ULL, CI_OK,
                        "file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.png", "field-day", "38.7071,-9.1393");
    CHECK(strstr(w, " route:X3RLY7 file:") != 0, "the relays always, then the badge");
    {
        ci_ask_t d = a;
        d.via[0] = 0;
        char w2[300];
        ci_build_result(w2, sizeof w2, "X3STN1", &d, 1786960001ULL, CI_OK, REF, "field-day", "38.7071,-9.1393");
        CHECK(strstr(w2, " title:field-day") != 0, "the event when it fits");
    }
    ci_cert_t c;
    CHECK(ci_read_cert(w, &c) && c.code == 200 && !strcmp(c.r, "abc123") && !strcmp(c.link, "lora")
          && !strcmp(c.file, REF) && !strcmp(c.route, "X3RLY7"), "a certificate reads back");
    CHECK(!strcmp(ci_grade(c.link, c.route), "relayed"), "through a relay");
    CHECK(!strcmp(ci_grade("ble", ""), "local") && !strcmp(ci_grade("lora", ""), "regional"), "grades");

    n = ci_build_result(w, sizeof w, "X3STN1", &a, 1786960001ULL, CI_REFUSED, REF, "x", "1,2");
    CHECK(n > 0 && !strstr(w, "link:") && !strstr(w, "file:") && strstr(w, "code:403"), "a refusal says only no");

    char pn[40];
    long la = 0, lo = 0;
    CHECK(ci_pos_parse("38.7071,-9.1393", &la, &lo) && la == 387071000L && lo == -91393000L, "a position reads");
    CHECK(!ci_pos_ok("38.7") && !ci_pos_ok("x,y") && !ci_pos_ok("12,190") && !ci_pos_ok("-90.5,0") && ci_pos_ok("-33,151.2"),
          "and nonsense does not");
    CHECK(ci_pos_norm("38.722345678,-9.13", pn, sizeof pn) && !strcmp(pn, "38.7223,-9.1300"), "four decimals: eleven metres");
    {
        /* Where a station says it is, bars and all (6.2.1), as the map
         * places it: the middle of the square its readable digits name. */
        char mid[40];
        long half = -1;
        CHECK(ci_pos_square("38\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88,-9\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88",
                            mid, sizeof mid, &half) && !strcmp(mid, "38.5000,-9.5000") && half == 55660,
              "a square of one degree: its middle, half of 111 km");
        CHECK(ci_pos_square("38.7\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88,-9.1\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88",
                            mid, sizeof mid, &half) && !strcmp(mid, "38.7500,-9.1500") && half == 5566,
              "a tenth of a degree: 11 km");
        CHECK(ci_pos_square("-0\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88,0\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88",
                            mid, sizeof mid, &half) && !strcmp(mid, "-0.5000,0.5000"),
              "south of the equator by its sign, even at zero");
        CHECK(ci_pos_square("38.7223,-9.1393", mid, sizeof mid, &half) && !strcmp(mid, "38.7223,-9.1393") && half == 0,
              "a plain point is itself");
        CHECK(!ci_pos_square("-", mid, sizeof mid, &half) && !ci_pos_square("38,x", mid, sizeof mid, &half) &&
              !ci_pos_square("91\xE2\x96\x88,0", mid, sizeof mid, &half), "nothing else is a place");
        CHECK(ci_xprs_call("X1ABCD") && ci_xprs_call("X3AB-7") && !ci_xprs_call("MT1234") &&
              !ci_xprs_call("X6ABC") && !ci_xprs_call("X1A") && !ci_xprs_call("X1ABCDEFG"),
              "XPRS callsigns only");
    }
    {
        long d = ci_dist_m("38.7071,-9.1393", "38.7161,-9.1393");
        CHECK(d > 990 && d < 1010, "a hundredth of a degree north is a kilometre");
        d = ci_dist_m("38.7071,-9.1393", "38.7071,-9.1278");
        CHECK(d > 985 && d < 1010, "and east, shortened by the latitude");
        CHECK(ci_dist_m("38.7", "1,1") == -1, "no distance without two places");
        char dt[24];
        ci_dist_text(447, dt, sizeof dt);
        CHECK(!strcmp(dt, "450 m"), "metres, to ten");
        ci_dist_text(2349, dt, sizeof dt);
        CHECK(!strcmp(dt, "2.3 km"), "kilometres, to a tenth");
        ci_dist_text(18400, dt, sizeof dt);
        CHECK(!strcmp(dt, "18 km"), "and whole kilometres further out");
    }
    char l[32];
    ci_label(l, sizeof l, "  Field Day 2026! ");
    CHECK(!strcmp(l, "field-day-2026"), "a name becomes a label");
    char m[200];
    memset(m, 'a', 150); m[150] = 0;
    n = ci_build_event(w, sizeof w, "X3STN1", 1786960000ULL, "38.7071,-9.1393", "Field Day",
                       1786960000ULL, 1786960000ULL + 86400, REF, m);
    CHECK(n > CI_WIRE_UNSIGNED && strstr(w, "tag:checkin") && strstr(w, "title:field-day")
          && strstr(w, REF) && strstr(w, "pos:38.7071,-9.1393") && !strstr(w, "since:") && strstr(w, m),
          "an event keeps all its words: the core airs a long one as parts (6.6)");
    n = ci_build_badge(w, sizeof w, "X3STN1", 1786960000ULL, REF, 640);
    CHECK(n > 0 && strstr(w, "t:file f:X3STN1") && strstr(w, " size:640B") && strstr(w, REF), "the badge's description");
}

static char g_cert[300];

static void test_station(void)
{
    char row_[4096], ask[300];
    as("X3STN1");
    mock_clear();
    module_init();
    CHECK(!mock_subscribed("xprs.command"), "off: listens to nothing");
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    CHECK(!mock_subscribed("xprs.command") && said("Share your location first"),
          "no shared place, no check-ins: it stays off and says why");
    /* The owner shares the station's position (Settings > Privacy). */
    strcpy(g_shared, "{\"pos\":\"38.7071,-9.1393\",\"acc\":10}");
    cmd("{\"command\":\"save_intro\",\"intro\":\"Hilltop shack, ring the bell\"}");
    CHECK(!said("Saved."), "an intro is saved as it is typed, without a toast per pause");
    mock_clear();
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    CHECK(mock_subscribed("xprs.command"), "on: listens for asks");
    CHECK(strstr(last_aired(), "t:event f:X3STN1") && strstr(last_aired(), "title:check-ins-open") &&
          strstr(last_aired(), "tag:checkin") && strstr(last_aired(), "pos:38.7071,-9.1393") &&
          strstr(last_aired(), "m:Hilltop shack, ring the bell"),
          "and it says so on the air: its check-in point, at its shared place, with its intro");
    mock_clear();
    module_tick();
    CHECK(g_aired_n == 0, "the point is not repeated within the quarter hour");
    g_now += 901;
    module_tick();
    CHECK(g_aired_n == 1 && strstr(last_aired(), "title:check-ins-open"), "and is renewed after it");
    g_now -= 901;
    cmd("{\"command\":\"save_greeting\",\"greeting\":\"Welcome to the hill, 73!\"}");
    cmd("{\"type\":\"file.open\",\"path\":\"/tmp/badge.png\"}");
    {
        int pub = 0;
        for (int i = 0; i < g_aired_n; i++)
            if (strstr(g_aired[i], "t:file f:X3STN1") && strstr(g_aired[i], REF)) pub = 1;
        CHECK(pub, "the badge is described as a publication");
    }

    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1CHSR", "X3STN1", g_now - 5, "local");
    ask_row(row_, sizeof row_, ask, "verified", "ble", 1, "");
    event("xprs.command", row_);
    CHECK(g_aired_n == 1 && strstr(last_aired(), "code:200 scope:local link:ble pos:38.7071,-9.1393 file:") && strstr(last_aired(), REF),
          "certified, over Bluetooth, at the station's place, with the badge's hash");
    CHECK(!strstr(last_aired(), " b:"), "the badge's bytes never ride the certificate");
    snprintf(g_cert, sizeof g_cert, "%s", last_aired());
    CHECK(g_msg_n == 1 && !strcmp(g_msg_to, "X1CHSR") && !strcmp(g_msg_text, "Welcome to the hill, 73!"),
          "the greeting goes as a message");

    mock_clear();
    event("xprs.command", row_);
    CHECK(g_aired_n == 1 && !strcmp(last_aired(), g_cert), "a repeated ask gets the same certificate");

    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1CHSR", "X3STN1", g_now + 30, "local");
    ask_row(row_, sizeof row_, ask, "verified", "ble", 1, "");
    event("xprs.command", row_);
    CHECK(strstr(last_aired(), "code:429") != 0, "once a day per visitor");

    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1STRG", "X3STN1", g_now, "local");
    ask_row(row_, sizeof row_, ask, "unverified", "ble", 1, "");
    event("xprs.command", row_);
    CHECK(g_aired_n == 0, "a key we do not hold yet: no answer, the repeat will check");
    {
        char ask2[300];
        ci_build_ask(ask2, sizeof ask2, "X1NEWK", "X3STN1", g_now, "local");
        ask_row(row_, sizeof row_, ask2, "unverified", "ble", 1, "");
        event("xprs.command", row_);
        ask_row(row_, sizeof row_, ask2, "verified", "ble", 1, "");
        event("xprs.command", row_);
        CHECK(g_aired_n == 1 && strstr(last_aired(), "code:200") != 0,
              "and the repeat, once the key is known, is certified");
    }

    mock_clear();
    ask_row(row_, sizeof row_, ask, "verified", "rns", 0, "");
    event("xprs.command", row_);
    CHECK(strstr(last_aired(), "code:403") != 0, "over the internet is not a meeting");

    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1LORA", "X3STN1", g_now, "regional");
    ask_row(row_, sizeof row_, ask, "verified", "lora", 1, "");
    event("xprs.command", row_);
    CHECK(strstr(last_aired(), "code:403") != 0, "in person only, by default");
    cmd("{\"command\":\"set_local\",\"local_only\":false}");
    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1LORA", "X3STN1", g_now + 1, "regional");
    ask_row(row_, sizeof row_, ask, "verified", "lora", 1, "X3RLY7");
    event("xprs.command", row_);
    CHECK(strstr(last_aired(), "code:200 scope:regional link:lora") && strstr(last_aired(), "route:X3RLY7"),
          "LoRa allowed: certified, relays named");

    /* An event: its name and its own badge on every certificate while it runs. */
    mock_clear();
    cmd("{\"command\":\"new_event\"}");
    cmd("{\"command\":\"pick_event_badge\"}");
    snprintf(g_put_token, sizeof g_put_token, "file:%s", EREF);
    cmd("{\"type\":\"file.open\",\"path\":\"/tmp/event.png\"}");
    g_lat = 387071000; g_lon = -91393000;
    cmd("{\"command\":\"publish_event\",\"ev_title\":\"Field Day 2026\",\"ev_desc\":\"on the hill all weekend\",\"ev_len\":\"week\"}");
    int saw_event = 0;
    for (int i = 0; i < g_aired_n; i++)
        if (strstr(g_aired[i], "t:event") && strstr(g_aired[i], "tag:checkin") &&
            strstr(g_aired[i], "title:field-day-2026") && strstr(g_aired[i], EREF) &&
            strstr(g_aired[i], "pos:38.7071,-9.1393")) saw_event = 1;
    CHECK(saw_event, "the event is announced, tagged, with its badge's hash");
    CHECK(said("\"title\":\"Field Day 2026\""), "and listed by its name, not its label");
    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1EVNT", "X3STN1", g_now, "local");
    ask_row(row_, sizeof row_, ask, "verified", "lan", 1, "");
    event("xprs.command", row_);
    CHECK(strstr(last_aired(), "link:lan") && strstr(last_aired(), EREF) && strstr(last_aired(), "title:field-day-2026"),
          "during the event: its name and its badge");
    CHECK(said("X1EVNT"), "the guestbook lists the visitor");
    CHECK(said("\"field\":\"guests\"") && !said("\"title\":\"Your badge\"") &&
          said("\"title\":\"Your events\"") && said("\"title\":\"Visitors\""),
          "one list, two sections: events, visitors");
    CHECK(said("\"field\":\"st_info\"") && said("\"label\":\"Location\"") &&
          said("\"field\":\"ev_info\"") && said("Field Day 2026"),
          "the station's location and the event running");
    CHECK(said("\"field\":\"share_location__label\",\"value\":\"Change location\""),
          "the button under the location opens the setting to change it");
    mock_clear();
    cmd("{\"command\":\"guests_open\"}");
    CHECK(said("\"field\":\"guests\"") && said("\"type\":\"ui.screen.open\",\"name\":\"Visitors\""),
          "Visitors opens the guestbook as a page of its own");
    snprintf(g_put_token, sizeof g_put_token, "file:%s", REF);

    cmd("{\"command\":\"set_bot\",\"bot_on\":false}");
    CHECK(!mock_subscribed("xprs.command"), "off again: listens to nothing");
    mock_clear();
    ci_build_ask(ask, sizeof ask, "X1LATE", "X3STN1", g_now, "local");
    ask_row(row_, sizeof row_, ask, "verified", "ble", 1, "");
    event("xprs.command", row_);
    CHECK(g_aired_n == 0, "an off station says nothing");
}

static void test_chaser(void)
{
    char row_[4096], ask[300], cert[300];
    g_now += 3 * 86400;                      /* another day: the guestbook allows again */
    as("X1CHSR");
    mock_clear();
    snprintf(g_stations, sizeof g_stations,
             "[{\"title\":\"Heard over the air (2)\",\"items\":["
             "{\"id\":\"X3STN1\",\"title\":\"X3STN1\",\"subtitle\":\"BLE\",\"tags\":[\"5s ago\",\"BLE\"],\"kind\":\"station\"},"
             "{\"id\":\"X4LAMP\",\"title\":\"X4LAMP\",\"subtitle\":\"LAN\",\"tags\":[],\"kind\":\"device\"}]},"
             "{\"title\":\"Heard this hour (1)\",\"items\":[{\"id\":\"X3FAR1\",\"title\":\"X3FAR1\",\"kind\":\"station\"}]}]");
    cmd("{\"command\":\"ready\"}");
    CHECK(said("\"id\":\"X3STN1\"") && said("\"action\":\"nearby_checkin\""),
          "a station in reach, with Check in (the host sends that action as the command)");
    CHECK(!said("\"id\":\"X4LAMP\"") && !said("\"id\":\"X3FAR1\""), "no devices, nobody out of reach");

    mock_clear();
    cmd("{\"command\":\"nearby_checkin\",\"nearby_id\":\"X3STN1\"}");
    CHECK(g_aired_n == 1 && strstr(last_aired(), "cmd:checkin scope:local") && strstr(last_aired(), "d:X3STN1"),
          "the ask goes to the core");
    snprintf(ask, sizeof ask, "%s", last_aired());
    CHECK(mock_subscribed("xprs.result") && mock_subscribed("xprs.status.tx"), "listening while it is out");
    CHECK(said("\"type\":\"ui.snack\"") && said("Asking X3STN1"), "the page says so at once, on the page");
    CHECK(said("\"action\":\"nearby_asking\",\"actionLabel\":\"Asking...\""), "and the row shows the ask is out");
    CHECK(mock_subscribed("core.monitor"), "a page is open: Nearby follows the air");
    cmd("{\"command\":\"nearby_checkin\",\"nearby_id\":\"X3STN1\"}");
    CHECK(g_aired_n == 1, "one ask at a time per station");

    /* The page closes before the answer: a new engine still knows the ask. */
    memset(g_pend, 0, sizeof g_pend);
    g_listening = 0;
    hal_event_unsubscribe("xprs.result", 11);       /* a new engine starts with none */
    hal_event_unsubscribe("xprs.status.tx", 14);
    hal_event_unsubscribe("xprs.message", 12);
    module_init();
    CHECK(mock_subscribed("xprs.result") && pend_find(g_pend[0].id) == 0 && !strcmp(g_pend[0].station, "X3STN1"),
          "an ask out survives the engine that sent it");

    /* The core carries it to the station. */
    as("X3STN1");
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    mock_clear();
    ask_row(row_, sizeof row_, ask, "verified", "ble", 1, "");
    event("xprs.command", row_);
    CHECK(strstr(last_aired(), "code:200") != 0, "the station certifies");
    snprintf(cert, sizeof cert, "%s", last_aired());
    cmd("{\"command\":\"set_bot\",\"bot_on\":false}");

    /* And the certificate back. */
    as("X1CHSR");
    mock_clear();
    result_row(row_, sizeof row_, cert);
    event("xprs.result", row_);
    CHECK(said("First check-in at X3STN1") && said("New badge!"), "the chaser is told, and it is a first");
    CHECK(said("\"type\":\"ui.map.marker\"") && said("\"lat\":38.7071,\"lon\":-9.1393") && said("\"id\":\"log:"),
          "the check-in is a pin on the map");
    CHECK(said("\"type\":\"ui.map.viewport\""), "the map centres on the check-ins");
    CHECK(said("\"type\":\"ui.map.prefetch\",\"bbox\":[") && said("\"minZoom\":3,\"maxZoom\":9"),
          "the region's overview is saved for off-grid use");
    char want[96];
    /* The week-long event announced above still runs: its badge, not the
     * station's, is the one handed out. */
    snprintf(want, sizeof want, "file:%s", EREF);
    CHECK(g_fetched_n >= 1 && !strcmp(g_fetched[0], want), "the event's badge is asked of the core by its hash");
    CHECK(said("\"value\":\"1\""), "the logbook counts it");
    CHECK(said("\"tab\":\"Badges\",\"title\":\"Badges\"") && said(EREF), "the logbook's Badges chip shows it");

    /* A certificate that names no place is not a check-in. */
    {
        char forged_ask[300], noplace[300], row2[4096];
        ci_build_ask(forged_ask, sizeof forged_ask, "X1CHSR", "X3NOPL", g_now, "local");
        snprintf(g_stations, sizeof g_stations,
                 "[{\"title\":\"Heard over the air (1)\",\"items\":[{\"id\":\"X3NOPL\",\"title\":\"X3NOPL\",\"kind\":\"station\"}]}]");
        cmd("{\"command\":\"nearby_checkin\",\"nearby_id\":\"X3NOPL\"}");
        char aid[7];
        al_id(last_aired(), al_len(last_aired()), aid);
        snprintf(noplace, sizeof noplace,
                 "t:result f:X3NOPL d:X1CHSR ts:2026-08-20_09:46:41 r:%s code:200 scope:local link:ble", aid);
        mock_clear();
        result_row(row2, sizeof row2, noplace);
        event("xprs.result", row2);
        CHECK(said("sent no location") && !said("Checked in at X3NOPL"), "no place, no check-in");
        (void)forged_ask;
    }

    /* The greeting follows as a message. */
    mock_clear();
    event("xprs.message", "{\"type\":\"message\",\"from\":\"npub1x\",\"call\":\"X3STN1\",\"content\":\"Welcome to the hill, 73!\"}");
    CHECK(said("Welcome to the hill, 73!"), "the greeting lands on the entry");

    /* The evidence. */
    char id[7];
    al_id(ask, al_len(ask), id);
    char tap[96];
    snprintf(tap, sizeof tap, "{\"command\":\"log_tap\",\"log_id\":\"%s\"}", id);
    mock_clear();
    cmd(tap);
    CHECK(said("Station's signed reply") && said("cmd:checkin") && said("code:200"),
          "the entry shows both packets");
    snprintf(tap, sizeof tap, "{\"command\":\"marker_tap\",\"id\":\"log:%s\"}", id);
    mock_clear();
    cmd(tap);
    CHECK(said("Station's signed reply") && said("\"name\":\"Entry\""), "a pin opens its check-in");

    /* Saving a city for off-grid use is the person's act, at a town's scale. */
    mock_clear();
    cmd("{\"type\":\"setViewport\",\"lat\":38.72,\"lon\":-9.14,\"zoom\":5}");
    cmd("{\"command\":\"save_area\"}");
    CHECK(said("Zoom in closer") && !said("ui.map.prefetch"), "not a country");
    mock_clear();
    cmd("{\"type\":\"setViewport\",\"lat\":38.72,\"lon\":-9.14,\"zoom\":13}");
    cmd("{\"command\":\"save_area\"}");
    CHECK(said("\"type\":\"ui.map.prefetch\",\"minZoom\":13,\"maxZoom\":16,\"max\":1500"), "this area, three levels closer");
    cmd("{\"type\":\"map.prefetch\",\"state\":\"done\",\"done\":120,\"total\":120,\"held\":118}");
    CHECK(said("Map saved offline (118 tiles)."), "and the person is told when it is saved");

    /* A station that never answers. */
    mock_clear();
    snprintf(g_stations, sizeof g_stations,
             "[{\"title\":\"Heard over the air (1)\",\"items\":[{\"id\":\"X3MUTE\",\"title\":\"X3MUTE\",\"kind\":\"station\"}]}]");
    cmd("{\"command\":\"nearby_checkin\",\"nearby_id\":\"X3MUTE\"}");
    al_id(last_aired(), al_len(last_aired()), id);
    char st[160];
    snprintf(st, sizeof st, "{\"id\":\"%s\",\"peer\":\"X3MUTE\",\"state\":\"unanswered\"}", id);
    event("xprs.status.tx", st);
    CHECK(said("X3MUTE did not answer"), "silence is reported, by the core's courier");
    g_now += 600;
    event("xprs.message", "{\"type\":\"message\",\"call\":\"X9OTHR\",\"content\":\"hi\"}");
    CHECK(!mock_subscribed("xprs.result") && !mock_subscribed("xprs.message"), "nothing out: listening to nothing");

    /* Stations giving check-ins that share where they are: listed nearest
     * first, pinned green, and their details carry coordinates to copy. */
    {
        char ev[400];
        g_lat = 387071000; g_lon = -91393000;          /* the visitor is here */
        ci_build_event(ev, sizeof ev, "X3PNT1", g_now - 60, "38.7161,-9.1393", CI_POINT_TITLE,
                       g_now - 60, g_now + 7200, "", "Rooftop station, ask at the bar");
        /* From a stranger: the core keeps it because this wapp follows the
         * type (heard here or replayed by an archiver), and the wapp reads it. */
        CHECK(strstr(g_followed_kind, "event") && mock_subscribed("xprs.event"),
              "check-in events are followed, and listened for to redraw");
        char evrow[600];
        snprintf(g_history, sizeof g_history, "[{\"id\":\"aa11bb\",\"wire\":\"%s\"}]", ev);
        snprintf(evrow, sizeof evrow, "{\"id\":\"aa11bb\",\"type\":\"event\",\"wire\":\"%s\"}", ev);
        event("xprs.event", evrow);
        mock_clear();
        cmd("{\"command\":\"events_refresh\"}");
        CHECK(said("\"title\":\"Stations\"") && said("\"st:X3PNT1\"") && said("1.0 km away") &&
              said("Rooftop station, ask at the bar"), "a station to visit, how far, and its intro");
        CHECK(!said("Check-ins Open"), "its point is not listed again as an event");
        CHECK(said("\"id\":\"st:X3PNT1\"") && said("#43A047"), "and it is a green pin");
        CHECK(said("\"field\":\"nearby\"") && said("\"title\":\"In reach now\""),
              "the stations heard are listed on Nearby, under the ones in reach");
        mock_clear();
        cmd("{\"command\":\"nearby_tap\",\"nearby_id\":\"st:X3PNT1\"}");
        CHECK(said("\"type\":\"ui.prompt\"") && said("\"copy\":\"38.7161,-9.1393\"") && g_aired_n == 0,
              "tapping one opens its details, and asks nobody for a check-in");
        mock_clear();
        cmd("{\"command\":\"events_tap\",\"events_id\":\"st:X3PNT1\"}");
        CHECK(said("\"type\":\"ui.prompt\"") && said("\"copy\":\"38.7161,-9.1393\"") &&
              said("1.0 km away") && said("Not visited yet"),
              "its details: the distance, and the coordinates to copy");
        mock_clear();
        cmd("{\"command\":\"prompt\",\"prompt_id\":\"pt:X3PNT1\",\"prompt_value\":\"map\"}");
        CHECK(said("\"type\":\"ui.map.locate\",\"lat\":38.7161,\"lon\":-9.1393"), "Show on map");
        mock_clear();
        cmd("{\"command\":\"marker_tap\",\"id\":\"st:X3PNT1\"}");
        CHECK(said("\"copy\":\"38.7161,-9.1393\""), "the pin opens the same details");
        /* An event's pin opens its sheet too, and Show on map goes there. */
        {
            char fd[400], hist[1400];
            ci_build_event(fd, sizeof fd, "X3EVT1", g_now - 30, "38.7300,-9.1500", "Hill Day",
                           g_now - 30, g_now + 3600, "", "Bring a radio");
            snprintf(hist, sizeof hist, "[{\"id\":\"ee33ff\",\"wire\":\"%s\"},{\"id\":\"aa11bb\",\"wire\":\"%s\"}]", fd, ev);
            strcpy(g_history, hist);
            mock_clear();
            cmd("{\"command\":\"marker_tap\",\"id\":\"ev:ee33ff\"}");
            CHECK(said("\"type\":\"ui.prompt\"") && said("\"title\":\"Hill Day\"") && said("By X3EVT1") &&
                  said("Bring a radio") && said("\"copy\":\"38.7300,-9.1500\""), "an event's pin opens its details");
            mock_clear();
            cmd("{\"command\":\"prompt\",\"prompt_id\":\"ev:ee33ff\",\"prompt_value\":\"map\"}");
            CHECK(said("\"type\":\"ui.map.locate\",\"lat\":38.7300,\"lon\":-9.1500"), "and Show on map goes there");
            mock_clear();
            cmd("{\"command\":\"events_tap\",\"events_id\":\"ee33ff\"}");
            CHECK(said("\"title\":\"Hill Day\""), "the Explore row opens it as well");
            snprintf(hist, sizeof hist, "[{\"id\":\"aa11bb\",\"wire\":\"%s\"}]", ev);
            strcpy(g_history, hist);
        }
        /* It closes: it leaves the list. */
        char closing[400];
        ci_build_event(closing, sizeof closing, "X3PNT1", g_now, "38.7161,-9.1393", CI_POINT_TITLE,
                       g_now, g_now + 60, "", "");
        snprintf(g_history, sizeof g_history,
                 "[{\"id\":\"cc22dd\",\"wire\":\"%s\"},{\"id\":\"aa11bb\",\"wire\":\"%s\"}]", closing, ev);
        g_now += 120;
        mock_clear();
        cmd("{\"command\":\"events_refresh\"}");
        CHECK(!said("\"st:X3PNT1\""), "a station that stopped giving check-ins leaves the list");
        g_now -= 120;
        strcpy(g_history, "[]");
    }

    /* A check-in the person no longer wants leaves the logbook, the map and
     * the badge wall. */
    {
        char rid[16] = "", first[300];
        mock_clear();
        cmd("{\"command\":\"fit_map\"}");
        CHECK(said("\"type\":\"ui.map.viewport\""), "Show all frames the map");
        const char *m = 0;
        for (int i = 0; i < g_said_n && !m; i++) m = strstr(g_said[i], "\"id\":\"log:");
        CHECK(m != 0, "there is a check-in to remove");
        if (m) {
            m += strlen("\"id\":\"log:");
            unsigned k = 0;
            while (m[k] && m[k] != '"' && k + 1 < sizeof rid) { rid[k] = m[k]; k++; }
            rid[k] = 0;
        }
        snprintf(first, sizeof first, "{\"command\":\"log_tap\",\"log_id\":\"%s\"}", rid);
        cmd(first);
        mock_clear();
        cmd("{\"command\":\"remove_entry\"}");
        CHECK(said("Removed check-in at X3STN1."), "removed, and the person is told");
        CHECK(said("ui.map.markers.clear") && !said(rid), "its pin is gone from the map");
        CHECK(!said("Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E"), "and its badge from the wall");
        CHECK(said("ui.screen.close"), "and the entry closes, back to where they came from");
        mock_clear();
        cmd("{\"command\":\"remove_entry\"}");
        CHECK(g_said_n == 0, "nothing selected: nothing removed");
    }
}


/* Every station that says where it is, on the map: the copy kept here is
 * drawn at once, says when it was brought up to date, and catches up with
 * the archive behind it. */
static const char *BAR5 = "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88";
static void id_row(char *out, unsigned cap, const char *call, unsigned long long ts,
                   const char *nick, const char *pos)
{
    char st[24], w[300];
    al_stamp(st, sizeof st, ts);
    snprintf(w, sizeof w, "t:identity f:%s ts:%s%s%s%s%s k:npub1test", call, st,
             nick ? " nick:" : "", nick ? nick : "", pos ? " pos:" : "", pos ? pos : "");
    snprintf(out, cap, "{\"id\":\"%.6s\",\"type\":\"identity\",\"sig\":\"verified\",\"wire\":\"%s\"}", call + 1, w);
}

static void test_places(void)
{
    as("X1CHSR");
    mock_clear();
    module_init();
    CHECK(strstr(g_followed_kind, "identity") != 0, "identities are followed, from anyone");

    char sq[64], r1[400], r2[400], r3[400], r4[400], r5[400];
    snprintf(sq, sizeof sq, "38%s,-9%s", BAR5, BAR5);
    id_row(r1, sizeof r1, "X3AAA1", g_now - 600, "Hill", sq);
    id_row(r2, sizeof r2, "X3AAA2-1", g_now - 500, 0, sq);
    id_row(r3, sizeof r3, "X3BBB1", g_now - 400, "Plaza", "40.4168,-3.7038");
    id_row(r4, sizeof r4, "MT0001", g_now - 300, 0, "41.0000,2.0000");     /* another network */
    id_row(r5, sizeof r5, "X1CHSR", g_now - 200, 0, "37.0000,-8.0000");    /* this station */
    snprintf(g_identities, sizeof g_identities, "[%s,%s,%s,%s,%s]", r5, r4, r3, r2, r1);

    mock_clear();
    cmd("{\"command\":\"ready\"}");
    CHECK(said("\"id\":\"pl_at\"") && !said("sq:38.5000"), "the map says how fresh its copy is, before the archive is read");
    mock_clear();
    module_tick();   /* the walk goes on behind the page */
    CHECK(said("\"id\":\"sq:38.5000,-9.5000\"") && said("\"label\":\"2 stations\"") && said("within 111 km"),
          "two stations in one square: one marker with how many, and how big the square is");
    CHECK(said("\"id\":\"pl:X3BBB1\"") && said("\"lat\":40.4168,\"lon\":-3.7038") && said("Plaza"),
          "a station at its point, with its name");
    CHECK(!said("MT0001") && !said("pl:X1CHSR"), "not other networks' nodes, not this station");
    CHECK(said("\"label\":\"3 stations\"") && said("\"label\":\"Updated "), "how many, and when it was updated");

    /* The page opens again: the copy is drawn before the archive is read. */
    int q0 = g_id_queries;
    strcpy(g_identities, "[]");
    mock_clear();
    cmd("{\"command\":\"ready\"}");
    CHECK(said("\"id\":\"pl:X3BBB1\"") && said("\"id\":\"sq:38.5000,-9.5000\"") && said("\"label\":\"Updated "),
          "the map opens on the kept copy, saying how old it is");
    CHECK(g_id_queries == q0, "and nothing was read from the archive to draw it");
    module_tick();
    CHECK(g_id_queries > q0 && strstr(g_id_query, "\"since\":"), "then the last hour is read, not everything");

    /* Taps: a square lists who is in it; Show on map goes there. */
    mock_clear();
    cmd("{\"command\":\"marker_tap\",\"id\":\"sq:38.5000,-9.5000\"}");
    CHECK(said("\"type\":\"ui.prompt\"") && said("\"title\":\"2 stations\"") && said("X3AAA1 Hill") &&
          said("X3AAA2") && said("Somewhere in this 111 km square") && said("\"copy\":\"38.5000,-9.5000\""),
          "a square's sheet: who is there, how approximate, the point to copy");
    mock_clear();
    cmd("{\"command\":\"prompt\",\"prompt_id\":\"sq:38.5000,-9.5000\",\"prompt_value\":\"map\"}");
    CHECK(said("\"type\":\"ui.map.locate\",\"lat\":38.5000,\"lon\":-9.5000"), "Show on map");
    mock_clear();
    cmd("{\"command\":\"marker_tap\",\"id\":\"pl:X3BBB1\"}");
    CHECK(said("\"title\":\"X3BBB1\"") && said("Plaza") && said("Heard 6 min ago"), "a station's sheet");

    /* A station that stops saying where it is leaves the map. */
    char gone[400];
    g_now += 200;
    id_row(gone, sizeof gone, "X3BBB1", g_now - 10, 0, "-");
    snprintf(g_identities, sizeof g_identities, "[%s]", gone);
    mock_clear();
    module_tick();
    CHECK(said("ui.map.markers.clear") && !said("pl:X3BBB1") && said("\"label\":\"2 stations\""),
          "pos:- takes a station off the map");

    /* A station giving check-ins is the green marker, not a grey one too. */
    {
        char ev[400], one[400];
        ci_build_event(ev, sizeof ev, "X3AAA1", g_now - 60, "38.7161,-9.1393", CI_POINT_TITLE,
                       g_now - 60, g_now + 7200, "", "");
        snprintf(g_history, sizeof g_history, "[{\"id\":\"ab12cd\",\"wire\":\"%s\"}]", ev);
        id_row(one, sizeof one, "X3AAA2", g_now - 5, 0, sq);
        snprintf(g_identities, sizeof g_identities, "[%s]", one);
        g_now += 200;
        mock_clear();
        cmd("{\"command\":\"ready\"}");
        CHECK(said("\"id\":\"st:X3AAA1\"") && said("#43A047"), "its check-in point is green");
        CHECK(said("\"id\":\"pl:X3AAA2\"") && !said("sq:38.5000,-9.5000"),
              "and its square holds only the other station");
        strcpy(g_history, "[]");
        strcpy(g_identities, "[]");
    }

    /* Two rows exactly as the live archive gave them on 2026-11-25 (every
     * field the host sends, nick:- file:- acc: sig: m:-), both in the
     * 49,8 square: one marker, "2 stations". */
    {
        static const char *real[2][4] = {
            {"d79e2f", "1795602150", "X1R5J9", "t:identity f:X1R5J9 ts:2026-11-25_10:22:30 nick:- file:- pos:49%s,8%s acc:100000 sig:i,dJ#>3c#^8/07)*9#.((?i0fQ9fF/fk/R>vvtvynN*J2l5;_d51,TiVgTcV m:-"},
            {"1bacd2", "1795549840", "X1Q6Y6", "t:identity f:X1Q6Y6 ts:2026-11-24_19:50:40 nick:- file:- pos:49%s,8%s acc:100000 sig:>0RvwP!i0/>E&e;2*(_?u=jK!Hu]rA0L%%B09Dd^>7GY$[VS_y*yk2(4UBH)C m:-"},
        };
        char rows[2][900];
        for (int i = 0; i < 2; i++) {
            char w[400];
            snprintf(w, sizeof w, real[i][3], BAR5, BAR5);
            snprintf(rows[i], sizeof rows[i],
                     "{\"id\":\"%s\",\"ts\":%s,\"heardTs\":%s,\"bearer\":\"lan\",\"rssi\":0,\"from\":\"%s\","
                     "\"to\":\"\",\"type\":\"identity\",\"mine\":false,\"own\":false,\"sig\":\"verified\","
                     "\"heard\":20,\"wire\":\"%s\",\"scope\":\"global\",\"inRange\":true}",
                     real[i][0], real[i][1], real[i][1], real[i][2], w);
        }
        snprintf(g_identities, sizeof g_identities, "[%s,%s]", rows[0], rows[1]);
        g_now = 1795602150ULL + 3600;
        mock_clear();
        cmd("{\"command\":\"ready\"}");
        module_tick();
        CHECK(said("\"id\":\"sq:49.5000,8.5000\"") && said("\"label\":\"2 stations\""),
              "the live archive's two stations: one marker in their square");
        strcpy(g_identities, "[]");
    }
}

int main(void)
{
    test_rules();
    test_station();
    test_chaser();
    test_places();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
