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
    CHECK(!mock_subscribed("xprs.command") && said("Share your station's position first"),
          "no shared place, no check-ins: it stays off and says why");
    /* The owner shares the station's position (Settings > Privacy). */
    strcpy(g_shared, "{\"pos\":\"38.7071,-9.1393\",\"acc\":10}");
    cmd("{\"command\":\"save_intro\",\"intro\":\"Hilltop shack, ring the bell\"}");
    CHECK(said("Intro saved"), "an intro is saved");
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
    CHECK(said("\"field\":\"guests\"") && said("\"title\":\"Your badge\"") &&
          said("\"title\":\"Your events\"") && said("\"title\":\"Guestbook\""),
          "one list, three sections: badge, events, visitors");
    CHECK(said("\"field\":\"st_info\"") && said("\"label\":\"Visitors\""),
          "the station's status card");
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
    CHECK(said("First check-in at X3STN1") && said("New badge on your wall!"), "the chaser is told, and it is a first");
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
    CHECK(said("\"title\":\"Badge wall\"") && said(EREF), "the badge wall shows it");

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
        CHECK(said("without saying where it is") && !said("Checked in at X3NOPL"), "no place, no check-in");
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
    CHECK(said("Certificate (signed by the station)") && said("cmd:checkin") && said("code:200"),
          "the entry shows both packets");
    snprintf(tap, sizeof tap, "{\"command\":\"marker_tap\",\"id\":\"log:%s\"}", id);
    mock_clear();
    cmd(tap);
    CHECK(said("Certificate (signed by the station)") && said("\"name\":\"Entry\""), "a pin opens its check-in");

    /* Saving a city for off-grid use is the person's act, at a town's scale. */
    mock_clear();
    cmd("{\"type\":\"setViewport\",\"lat\":38.72,\"lon\":-9.14,\"zoom\":5}");
    cmd("{\"command\":\"save_area\"}");
    CHECK(said("Zoom in to the town") && !said("ui.map.prefetch"), "not a country");
    mock_clear();
    cmd("{\"type\":\"setViewport\",\"lat\":38.72,\"lon\":-9.14,\"zoom\":13}");
    cmd("{\"command\":\"save_area\"}");
    CHECK(said("\"type\":\"ui.map.prefetch\",\"minZoom\":13,\"maxZoom\":16,\"max\":1500"), "this area, three levels closer");
    cmd("{\"type\":\"map.prefetch\",\"state\":\"done\",\"done\":120,\"total\":120,\"held\":118}");
    CHECK(said("Map saved for off-grid use: 118 tiles."), "and the person is told when it is saved");

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
        /* Heard on the air from a stranger: the core's archive keeps none of
         * it on a private phone, so the wapp keeps it. */
        CHECK(mock_subscribed("xprs.event"), "check-in events are listened for");
        char evrow[600];
        snprintf(evrow, sizeof evrow, "{\"id\":\"aa11bb\",\"type\":\"event\",\"wire\":\"%s\"}", ev);
        event("xprs.event", evrow);
        mock_clear();
        cmd("{\"command\":\"events_refresh\"}");
        CHECK(said("Stations giving check-ins") && said("\"st:X3PNT1\"") && said("1.0 km away") &&
              said("Rooftop station, ask at the bar"), "a station to visit, how far, and its intro");
        CHECK(!said("Check-ins Open"), "its point is not listed again as an event");
        CHECK(said("\"id\":\"st:X3PNT1\"") && said("#43A047"), "and it is a green pin");
        mock_clear();
        cmd("{\"command\":\"events_tap\",\"events_id\":\"st:X3PNT1\"}");
        CHECK(said("\"type\":\"ui.prompt\"") && said("\"copy\":\"38.7161,-9.1393\"") &&
              said("About 1.0 km from you") && said("Not visited yet"),
              "its details: the distance, and the coordinates to copy");
        mock_clear();
        cmd("{\"command\":\"prompt\",\"prompt_id\":\"pt:X3PNT1\",\"prompt_value\":\"map\"}");
        CHECK(said("\"type\":\"ui.map.locate\",\"lat\":38.7161,\"lon\":-9.1393"), "Show on map");
        mock_clear();
        cmd("{\"command\":\"marker_tap\",\"id\":\"st:X3PNT1\"}");
        CHECK(said("\"copy\":\"38.7161,-9.1393\""), "the pin opens the same details");
        /* It closes: it leaves the list. */
        char closing[400];
        ci_build_event(closing, sizeof closing, "X3PNT1", g_now, "38.7161,-9.1393", CI_POINT_TITLE,
                       g_now, g_now + 60, "", "");
        snprintf(evrow, sizeof evrow, "{\"id\":\"cc22dd\",\"type\":\"event\",\"wire\":\"%s\"}", closing);
        event("xprs.event", evrow);
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
        CHECK(said("Removed your check-in at X3STN1."), "removed, and the person is told");
        CHECK(said("ui.map.markers.clear") && !said(rid), "its pin is gone from the map");
        CHECK(!said("Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E"), "and its badge from the wall");
        CHECK(said("ui.screen.close"), "and the entry closes, back to where they came from");
        mock_clear();
        cmd("{\"command\":\"remove_entry\"}");
        CHECK(g_said_n == 0, "nothing selected: nothing removed");
    }
}

int main(void)
{
    test_rules();
    test_station();
    test_chaser();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
