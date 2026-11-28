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
extern char g_mock_dev[16];
extern char g_owned[200];
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
extern char g_names[2048];
extern char g_station_one[2048];
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
    CHECK(wst_pos_parse("38.7071,-9.1393", &la, &lo) && la == 387071000L && lo == -91393000L, "a position reads");
    CHECK(!wst_pos_ok("38.7") && !wst_pos_ok("x,y") && !wst_pos_ok("12,190") && !wst_pos_ok("-90.5,0") && wst_pos_ok("-33,151.2"),
          "and nonsense does not");
    CHECK(wst_pos_norm("38.722345678,-9.13", pn, sizeof pn) && !strcmp(pn, "38.7223,-9.1300"), "four decimals: eleven metres");
    {
        /* Where a station says it is, bars and all (6.2.1), as the map
         * places it: the middle of the square its readable digits name. */
        char mid[40];
        long half = -1;
        CHECK(wst_pos_square("38\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88,-9\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88",
                            mid, sizeof mid, &half) && !strcmp(mid, "38.5000,-9.5000") && half == 55660,
              "a square of one degree: its middle, half of 111 km");
        CHECK(wst_pos_square("38.7\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88,-9.1\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88",
                            mid, sizeof mid, &half) && !strcmp(mid, "38.7500,-9.1500") && half == 5566,
              "a tenth of a degree: 11 km");
        CHECK(wst_pos_square("-0\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88,0\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88",
                            mid, sizeof mid, &half) && !strcmp(mid, "-0.5000,0.5000"),
              "south of the equator by its sign, even at zero");
        CHECK(wst_pos_square("38.7223,-9.1393", mid, sizeof mid, &half) && !strcmp(mid, "38.7223,-9.1393") && half == 0,
              "a plain point is itself");
        CHECK(!wst_pos_square("-", mid, sizeof mid, &half) && !wst_pos_square("38,x", mid, sizeof mid, &half) &&
              !wst_pos_square("91\xE2\x96\x88,0", mid, sizeof mid, &half), "nothing else is a place");
        CHECK(wst_xprs_call("X1ABCD") && wst_xprs_call("X3AB-7") && !wst_xprs_call("MT1234") &&
              !wst_xprs_call("X6ABC") && !wst_xprs_call("X1A") && !wst_xprs_call("X1ABCDEFG"),
              "XPRS callsigns only");
    }
    {
        long d = wst_dist_m("38.7071,-9.1393", "38.7161,-9.1393");
        CHECK(d > 990 && d < 1010, "a hundredth of a degree north is a kilometre");
        d = wst_dist_m("38.7071,-9.1393", "38.7071,-9.1278");
        CHECK(d > 985 && d < 1010, "and east, shortened by the latitude");
        CHECK(wst_dist_m("38.7", "1,1") == -1, "no distance without two places");
        char dt[24];
        wst_dist_text(447, dt, sizeof dt);
        CHECK(!strcmp(dt, "450 m"), "metres, to ten");
        wst_dist_text(2349, dt, sizeof dt);
        CHECK(!strcmp(dt, "2.3 km"), "kilometres, to a tenth");
        wst_dist_text(18400, dt, sizeof dt);
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

/* The station's answer to the owner's last setting, as the core hands it. */
static void answer_set(const char *station, const char *code, const char *keys)
{
    char id[8], w[300], row_[1024];
    snprintf(id, sizeof id, "%s", g_set_id);
    snprintf(w, sizeof w, "t:result f:%s d:X1OWNR ts:2026-08-17_09:46:41 r:%s code:%s%s%s",
             station, id, code, keys[0] ? " " : "", keys);
    result_row(row_, sizeof row_, w);
    event("xprs.result", row_);
}

/* Setting a check-in point up (XPRS.md 11.9.1): the station gives check-ins
 * itself; this wapp sends its owner's settings to it, this device's or any
 * other, and reads back what it is. */
static void test_station(void)
{
    as("X1OWNR");
    snprintf(g_mock_dev, sizeof g_mock_dev, "X3STN1");
    mock_clear();
    module_init();
    CHECK(!mock_subscribed("xprs.command"), "the station answers asks, not this wapp: it listens to none");
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    CHECK(g_aired_n == 0 && said("Share your location first"),
          "this device shares no place: no check-ins, and it says why");
    strcpy(g_shared, "{\"pos\":\"38.7071,-9.1393\",\"acc\":10}");
    cmd("{\"command\":\"save_greeting\",\"greeting\":\"Welcome to the hill, 73!\"}");
    CHECK(g_aired_n == 0, "a welcome is kept as it is typed, not aired per pause");

    mock_clear();
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    CHECK(g_aired_n == 1 && strstr(last_aired(), "t:command f:X1OWNR d:X3STN1 ") &&
          strstr(last_aired(), " cmd:set tag:checkin,in-person m:Welcome to the hill, 73!"),
          "on: its owner's cmd:set to this device's station, the welcome riding last");
    CHECK(mock_subscribed("xprs.result"), "and the answer is listened for");
    mock_clear();
    answer_set("X3STN1", "200", "tag:checkin,in-person");
    CHECK(said("X3STN1: check-ins on."), "the station says it is on, and the person is told");
    CHECK(!mock_subscribed("xprs.result"), "and nothing more is listened for");

    mock_clear();
    cmd("{\"command\":\"set_local\",\"local_only\":false}");
    CHECK(strstr(last_aired(), " cmd:set tag:checkin") && !strstr(last_aired(), "in-person") &&
          !strstr(last_aired(), " m:"), "LoRa range too: the labels again, the welcome already sent");
    answer_set("X3STN1", "200", "tag:checkin");
    cmd("{\"command\":\"save_type\",\"place_type\":\"park\"}");
    CHECK(strstr(last_aired(), " cmd:set tag:checkin,park") != 0, "the kind of place is a label");
    answer_set("X3STN1", "200", "tag:checkin,park");

    mock_clear();
    cmd("{\"type\":\"file.open\",\"path\":\"/tmp/badge.png\"}");
    {
        int pub = 0, set = 0;
        for (int i = 0; i < g_aired_n; i++) {
            if (strstr(g_aired[i], "t:file f:X1OWNR") && strstr(g_aired[i], REF)) pub = 1;
            if (strstr(g_aired[i], "d:X3STN1") && strstr(g_aired[i], " cmd:set file:") && strstr(g_aired[i], REF)) set = 1;
        }
        CHECK(pub, "the badge is published by this device");
        CHECK(set, "and the station is given its reference, never its bytes");
    }
    answer_set("X3STN1", "200", "tag:checkin,park");

    /* An event: announced, and its title and badge set on the station. */
    mock_clear();
    cmd("{\"command\":\"new_event\"}");
    cmd("{\"command\":\"pick_event_badge\"}");
    snprintf(g_put_token, sizeof g_put_token, "file:%s", EREF);
    cmd("{\"type\":\"file.open\",\"path\":\"/tmp/event.png\"}");
    g_lat = 387071000; g_lon = -91393000;
    cmd("{\"command\":\"publish_event\",\"ev_title\":\"Field Day 2026\",\"ev_desc\":\"on the hill all weekend\",\"ev_len\":\"week\"}");
    {
        int saw_event = 0, set = 0;
        for (int i = 0; i < g_aired_n; i++) {
            if (strstr(g_aired[i], "t:event") && strstr(g_aired[i], "tag:checkin") &&
                strstr(g_aired[i], "title:field-day-2026") && strstr(g_aired[i], EREF)) saw_event = 1;
            if (strstr(g_aired[i], " cmd:set title:field-day-2026 until:") && strstr(g_aired[i], EREF)) set = 1;
        }
        CHECK(saw_event, "the event is announced, tagged, with its badge's hash");
        CHECK(set, "and the station carries its title and badge while it runs");
    }
    answer_set("X3STN1", "200", "tag:checkin,park");
    CHECK(said("\"field\":\"st_info\"") && said("X3STN1 (this device)") && said("\"label\":\"Location\""),
          "which station, and where");
    snprintf(g_put_token, sizeof g_put_token, "file:%s", REF);

    /* Another station of the person's, from afar: Devices opens it here. */
    mock_clear();
    cmd("{\"type\":\"view.open\",\"view\":\"station:X3ROOF\"}");
    CHECK(said("\"type\":\"ui.screen.open\",\"name\":\"Settings\"") && said("\"value\":\"X3ROOF\""),
          "opened on that station");
    CHECK(said("\"field\":\"bot_on\",\"value\":false"), "its own settings, not this device's");
    CHECK(said("\"field\":\"share_location__hidden\",\"value\":true"), "its place is its own to share");
    mock_clear();
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    CHECK(strstr(last_aired(), "d:X3ROOF") && strstr(last_aired(), " cmd:set tag:checkin,in-person"),
          "set from here, wherever it is");
    answer_set("X3ROOF", "403", "");
    CHECK(said("only its owners may change it"), "a station that is not the person's says so");
    cmd("{\"command\":\"set_bot\",\"bot_on\":true}");
    {
        char id[7], row_[200];
        al_id(last_aired(), al_len(last_aired()), id);
        snprintf(row_, sizeof row_, "{\"id\":\"%s\",\"state\":\"unanswered\"}", id);
        mock_clear();
        event("xprs.status.tx", row_);
        CHECK(said("X3ROOF did not answer"), "and one out of reach is reported, not waited on");
    }

    /* The stations to choose from: this device's, then every one owned. */
    snprintf(g_owned, sizeof g_owned, "[\"X3ROOF\",\"X3SRVR\"]");
    mock_clear();
    cmd("{\"command\":\"stations_open\"}");
    CHECK(said("\"field\":\"stations\"") && said("\"id\":\"X3STN1\"") && said("\"id\":\"X3SRVR\""),
          "every station the person owns");
    cmd("{\"command\":\"stations_tap\",\"stations_id\":\"X3STN1\"}");
    CHECK(said("X3STN1 (this device)"), "and back to this device's");
}

/* Before 11.9.1 the wapp was the point; a device that had check-ins on
 * hands its own station the same settings once. */
static void test_migration(void)
{
    as("X1OWNR");
    snprintf(g_mock_dev, sizeof g_mock_dev, "X3OLDD");
    hal_kv_set("bot", 3, "1", 1);
    hal_kv_set("local", 5, "1", 1);
    hal_kv_set("ptype", 5, "coffee", 6);
    hal_kv_set("greet", 5, "Hi", 2);
    hal_kv_set("migrated", 8, "0", 1);
    hal_kv_set("X3OLDD.bot", 10, "", 0);
    mock_clear();
    module_init();
    CHECK(g_aired_n == 1 && strstr(last_aired(), "d:X3OLDD") &&
          strstr(last_aired(), " cmd:set tag:checkin,coffee,in-person") && strstr(last_aired(), " m:Hi"),
          "the old settings, given to the station");
    answer_set("X3OLDD", "200", "tag:checkin,coffee,in-person");
    mock_clear();
    module_init();
    CHECK(g_aired_n == 0, "once");
    hal_kv_set("bot", 3, "0", 1);
    snprintf(g_mock_dev, sizeof g_mock_dev, "X3STN1");
}

static void test_chaser(void)
{
    char row_[4096], ask[300], cert[300];
    g_now += 3 * 86400;                      /* another day: the guestbook allows again */
    as("X1CHSR");
    mock_clear();
    snprintf(g_stations, sizeof g_stations,
             "[{\"title\":\"Heard over the air (5)\",\"items\":["
             "{\"id\":\"X3STN1\",\"title\":\"X3STN1\",\"subtitle\":\"BLE\",\"tags\":[\"5s ago\",\"BLE\"],\"kind\":\"station\",\"heard\":{\"ble\":5},\"rssi\":-61},"
             "{\"id\":\"X3BOTH\",\"title\":\"X3BOTH\",\"subtitle\":\"LAN\",\"tags\":[],\"kind\":\"station\",\"heard\":{\"ble\":200,\"lan\":2},\"rssi\":-80},"
             "{\"id\":\"X3LAN1\",\"title\":\"X3LAN1\",\"subtitle\":\"LAN\",\"tags\":[],\"kind\":\"station\",\"heard\":{\"lan\":1}},"
             "{\"id\":\"X3OLD1\",\"title\":\"X3OLD1\",\"subtitle\":\"BLE\",\"tags\":[],\"kind\":\"station\",\"heard\":{\"ble\":601}},"
             "{\"id\":\"X4LAMP\",\"title\":\"X4LAMP\",\"subtitle\":\"LAN\",\"tags\":[],\"kind\":\"device\",\"heard\":{\"ble\":3}}]},"
             "{\"title\":\"Heard this hour (1)\",\"items\":[{\"id\":\"X3FAR1\",\"title\":\"X3FAR1\",\"kind\":\"station\"}]}]");
    cmd("{\"command\":\"ready\"}");
    CHECK(said("\"id\":\"X3STN1\"") && said("\"action\":\"nearby_checkin\""),
          "a station in reach, with Check in (the host sends that action as the command)");
    CHECK(!said("\"id\":\"X4LAMP\"") && !said("\"id\":\"X3FAR1\""), "no devices, nobody out of reach");
    CHECK(said("\"id\":\"X3BOTH\"") && said("heard 3 min ago"),
          "heard on Bluetooth 3 minutes ago and on the LAN just now: in reach, by its Bluetooth time");
    CHECK(!said("\"id\":\"X3LAN1\""), "only on the LAN: not in reach");
    CHECK(!said("\"id\":\"X3OLD1\""), "Bluetooth more than ten minutes ago: not in reach");
    CHECK(said("Bluetooth \xc2\xb7 -61 dBm \xc2\xb7 heard just now"), "how it is heard, its signal, how fresh");
    {
        const char *a = 0, *b = 0;
        for (int i = g_said_n - 1; i >= 0 && !a; i--)
            if (strstr(g_said[i], "\"field\":\"log\"")) {
                a = strstr(g_said[i], "\"id\":\"X3STN1\"");
                b = strstr(g_said[i], "\"id\":\"X3BOTH\"");
            }
        CHECK(a && b && a < b, "the strongest signal first");
    }
    CHECK(said("\"field\":\"log\",\"sections\":[{\"title\":\"In reach\""),
          "In reach is the first section of the one screen");
    {
        char keep[4096];
        snprintf(keep, sizeof keep, "%s", g_stations);
        snprintf(g_stations, sizeof g_stations, "[{\"title\":\"Heard over the air (0)\",\"items\":[]}]");
        mock_clear();
        cmd("{\"command\":\"ready\"}");
        CHECK(!said("\"title\":\"In reach\"") && said("\"title\":\"Places\""),
              "nobody in reach: the list is the places");
        snprintf(g_stations, sizeof g_stations, "%s", keep);
    }

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

    /* The core carries it to the station, which certifies it (its own
     * XprsCheckinPoint or firmware, with ci.c's words). */
    {
        ci_ask_t a;
        ask_row(row_, sizeof row_, ask, "verified", "ble", 1, "");
        CHECK(ci_read_ask(row_, &a), "the ask, as the station reads it");
        CHECK(ci_build_result(cert, sizeof cert, "X3STN1", &a, g_now, CI_OK, EREF,
                              "field-day-2026", "38.7071,-9.1393") > 0, "the station certifies");
    }

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
    CHECK(said("{\"id\":\"badge\",\"label\":\"Badges\",\"count\":1"), "the Badges tag counts it");
    mock_clear();
    cmd("{\"command\":\"log_tags_tap\",\"log_tags_id\":\"badge\"}");
    CHECK(said("\"title\":\"Badges\"") && said(EREF), "and shows the badge wall");
    cmd("{\"command\":\"log_tags_tap\",\"log_tags_id\":\"badge\"}");

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
    {
        char gr[160] = "";
        db_one("SELECT greeting FROM log WHERE station = 'X3STN1' ORDER BY ts DESC LIMIT 1", 0, "greeting", gr, sizeof gr);
        CHECK(!strcmp(gr, "Welcome to the hill, 73!"), "the greeting lands on the entry");
    }

    /* The evidence. */
    char id[7];
    al_id(ask, al_len(ask), id);
    char tap[96];
    /* What the archive keeps of the station: its event's announcement and
     * its check-in point, with their words. */
    {
        static char evw[700], ptw[400];
        ci_build_event(evw, sizeof evw, "X3STN1", g_now - 3600, "38.7071,-9.1393", "Field Day 2026",
                       0, g_now + 6 * 86400ULL, EREF, "On the hill all weekend");
        ci_build_event(ptw, sizeof ptw, "X3STN1", g_now - 60, "38.7071,-9.1393", CI_POINT_TITLE,
                       0, g_now + 3600, "", "Hilltop shack, ring the bell");
        snprintf(g_history, sizeof g_history, "[{\"id\":\"ev0001\",\"wire\":\"%s\"},{\"id\":\"pt0001\",\"wire\":\"%s\"}]", evw, ptw);
    }
    snprintf(tap, sizeof tap, "{\"command\":\"log_tap\",\"log_id\":\"%s\"}", id);
    mock_clear();
    cmd(tap);
    CHECK(said("The station's signed reply") && said("cmd:checkin") && said("code:200"),
          "the entry shows both packets");
    CHECK(said("\"field\":\"e_place\",\"value\":{\"lat\":38.7071,\"lon\":-9.1393,\"label\":\"X3STN1\"}"),
          "the entry shows where it was on a small map");
    CHECK(said("\"label\":\"About the event\",\"value\":\"On the hill all weekend\"") &&
          said("\"label\":\"About the station\",\"value\":\"Hilltop shack, ring the bell\""),
          "with the event's description and the station's own words");
    CHECK(said("\"label\":\"When\""), "and when");
    CHECK(said("\"field\":\"e_badge\",\"value\":\"file:"), "the entry shows the badge it collected");
    CHECK(said("\"label\":\"Radio\",\"value\":\"Bluetooth\""), "and the radio by its name");
    CHECK(said("\"name\":\"Entry\",\"title\":\"Field Day 2026\""), "titled with its event");
    mock_clear();
    cmd("{\"command\":\"entry_event\"}");
    CHECK(said("\"name\":\"About\",\"title\":\"Field Day 2026\"") &&
          said("\"label\":\"Run by\",\"value\":\"X3STN1\"") && said("\"label\":\"Ends\"") &&
          said("\"label\":\"Now\",\"value\":\"On now\"") && said("\"field\":\"a_place\""),
          "the entry's event opens what its announcement says");
    strcpy(g_names, "[{\"call\":\"X3STN1\",\"nick\":\"Hilltop club\",\"kind\":\"station\",\"ts\":1786960000}]");
    strcpy(g_station_one, "{\"call\":\"X3STN1\",\"kind\":\"station\",\"bearer\":\"ble5\",\"bearers\":[\"ble5\",\"ble\",\"lora\"],"
                          "\"rssi\":-61,\"agoMs\":180000,\"serve\":[\"archive\"],\"sig\":\"verified\"}");
    mock_clear();
    cmd("{\"command\":\"about_station\"}");
    CHECK(said("\"name\":\"About\",\"title\":\"Hilltop club\"") &&
          said("\"label\":\"Gives check-ins\",\"value\":\"Yes, until") &&
          said("\"label\":\"Events\",\"value\":\"Field Day 2026\""),
          "and from it, the station that runs it");
    CHECK(said("\"label\":\"Name\",\"value\":\"Hilltop club\"") &&
          said("\"label\":\"Heard this hour\",\"value\":\"Bluetooth, LoRa, 3 min ago (-61 dBm)\"") &&
          said("\"label\":\"Serves\",\"value\":\"Archive (keeps messages)\"") && said("\"label\":\"Signatures\",\"value\":\"verified\""),
          "its name from its identity, and how it is heard now");
    strcpy(g_names, "[]");
    g_station_one[0] = 0;
    /* The score's tiles open what they count. */
    mock_clear();
    cmd("{\"command\":\"ready\"}");
    CHECK(said("\"id\":\"local\",\"label\":\"In person\"") && said("\"tap\":true"),
          "the tiles say they can be tapped (the host takes no tap otherwise)");
    mock_clear();
    cmd("{\"command\":\"lb_stats_tap\",\"lb_stats_id\":\"local\"}");
    CHECK(said("\"tags\":[\"in person\",\"X3STN1\",\"Bluetooth"), "a row says how it was heard");
    CHECK(said("\"field\":\"detail\"") && said("\"call\":\"X3STN1\"") && said("\"in person\"") &&
          said("\"name\":\"Score detail\",\"title\":\"In person\""),
          "In person: the check-ins face to face (this one during an event, under its name)");
    mock_clear();
    cmd("{\"command\":\"lb_stats_tap\",\"lb_stats_id\":\"stations\"}");
    CHECK(said("\"id\":\"st:X3STN1\"") && said("1 check-in") && said("1 in person"),
          "Stations: each once, how many check-ins, how many in person");
    mock_clear();
    cmd("{\"command\":\"detail_tap\",\"detail_id\":\"st:X3STN1\"}");
    CHECK(said("\"name\":\"About\",\"title\":\"X3STN1\"") && said("\"label\":\"Callsign\""),
          "a station opens what is known of it");
    mock_clear();
    cmd("{\"command\":\"about_list\"}");
    CHECK(said("\"title\":\"X3STN1\"") && said("\"name\":\"Score detail\",\"title\":\"X3STN1\""),
          "and from there its check-ins");
    snprintf(tap, sizeof tap, "{\"command\":\"detail_tap\",\"detail_id\":\"%s\"}", id);
    mock_clear();
    cmd(tap);
    CHECK(said("The station's signed reply") && said("\"name\":\"Entry\""), "and a check-in opens its entry");
    mock_clear();
    cmd("{\"command\":\"lb_stats_tap\",\"lb_stats_id\":\"streak\"}");
    CHECK(said("\"id\":\"day:") && said("\"title\":\"Streak\""), "Streak: its days");
    mock_clear();
    cmd("{\"command\":\"lb_stats_tap\",\"lb_stats_id\":\"events\"}");
    CHECK(said("\"id\":\"ev:field-day-2026\"") && said("\"title\":\"Events attended\""),
          "Events: each event attended, with its check-ins");
    mock_clear();
    cmd("{\"command\":\"detail_tap\",\"detail_id\":\"ev:field-day-2026\"}");
    CHECK(said("\"name\":\"About\",\"title\":\"Field Day 2026\"") &&
          said("\"label\":\"About\",\"value\":\"On the hill all weekend\""), "an event opens its announcement");
    mock_clear();
    cmd("{\"command\":\"about_list\"}");
    CHECK(said("\"title\":\"Field Day 2026\"") && said("\"name\":\"Score detail\""), "and from there its check-ins");
    snprintf(tap, sizeof tap, "{\"command\":\"marker_tap\",\"id\":\"log:%s\"}", id);
    mock_clear();
    cmd(tap);
    CHECK(said("The station's signed reply") && said("\"name\":\"Entry\""), "a pin opens its check-in");
    strcpy(g_history, "[]");

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
        CHECK(!said("Check-ins Open"), "its point is not listed again as an event");
        CHECK(said("\"id\":\"st:X3PNT1\"") && said("#43A047"), "and it is a green pin");
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

/* ── Places: time in range, automatic check-ins, kinds of place, tags ── */

/* [chaser] asks [station] in person and the station certifies: the whole
 * round, as the core carries it. 1 when the chaser got its check-in. */
/* The station's side, as the core or the firmware gives it: ci.c's rule
 * and words, once a day per visitor, at the station's shared place. */
static struct { char st[16], who[16]; unsigned long long day; } g_sim[16];
static void station_answer(const char *station, const char *ask, char *cert, unsigned cap)
{
    char row_[4096];
    ci_ask_t a;
    ask_row(row_, sizeof row_, ask, "verified", "ble", 1, "");
    ci_read_ask(row_, &a);
    unsigned long long day = ci_day(g_now, 0);
    int today = 0, free_at = -1;
    for (int i = 0; i < 16; i++) {
        if (!g_sim[i].st[0]) { if (free_at < 0) free_at = i; continue; }
        if (!strcmp(g_sim[i].st, station) && !strcmp(g_sim[i].who, a.from) && g_sim[i].day == day) today = 1;
    }
    ci_rules_t r = { 1 };
    int code = ci_decide(&a, &r, g_now, today);
    if (code == CI_OK && free_at >= 0) {
        snprintf(g_sim[free_at].st, sizeof g_sim[free_at].st, "%s", station);
        snprintf(g_sim[free_at].who, sizeof g_sim[free_at].who, "%s", a.from);
        g_sim[free_at].day = day;
    }
    ci_build_result(cert, cap, station, &a, g_now, code, "", "", "38.7071,-9.1393");
}

static int round_trip(const char *chaser, const char *station, int automatic)
{
    char row_[4096], ask[300], cert[300];
    as(chaser);
    int before = g_aired_n;
    if (!automatic) {
        char c[120];
        snprintf(c, sizeof c, "{\"command\":\"nearby_checkin\",\"log_id\":\"%s\"}", station);
        cmd(c);
    }
    if (g_aired_n == before) return 0;
    snprintf(ask, sizeof ask, "%s", last_aired());
    station_answer(station, ask, cert, sizeof cert);
    result_row(row_, sizeof row_, cert);
    event("xprs.result", row_);
    return strstr(cert, "code:200") != 0;
}

static void heard_ble(const char *call, int age)
{
    snprintf(g_stations, sizeof g_stations,
             "[{\"title\":\"Heard over the air (1)\",\"items\":["
             "{\"id\":\"%s\",\"title\":\"%s\",\"kind\":\"station\",\"heard\":{\"ble\":%d},\"rssi\":-58}]}]",
             call, call, age);
}
static void heard_nobody(void)
{
    snprintf(g_stations, sizeof g_stations, "[{\"title\":\"Heard over the air (0)\",\"items\":[]}]");
}

static void test_place_rules(void)
{
    unsigned long long s = 0, l = 0;
    CHECK(ci_visit_step(&s, &l, 0, 1000, 600) == CI_V_NONE && !s, "not heard, no visit");
    CHECK(ci_visit_step(&s, &l, 995, 1000, 600) == CI_V_OPEN && s == 995 && l == 995, "heard: a visit opens then");
    CHECK(ci_visit_step(&s, &l, 1175, 1180, 600) == CI_V_EXTEND && l == 1175, "heard again: it goes on");
    CHECK(ci_visit_step(&s, &l, 1175, 1360, 600) == CI_V_NONE && l == 1175, "a short silence: still there");
    CHECK(ci_visit_step(&s, &l, 1175, 1800, 600) == CI_V_CLOSE && s == 995 && l == 1175,
          "ten minutes out of range: it ended when last heard");
    s = l = 0;
    CHECK(ci_visit_step(&s, &l, 100, 1000, 600) == CI_V_NONE && !s, "an old hearing opens nothing");

    char t[40];
    ci_span(t, sizeof t, 45 * 60);          CHECK(!strcmp(t, "45 min"), "minutes");
    ci_span(t, sizeof t, 3 * 3600 + 1200);  CHECK(!strcmp(t, "3 h 20 min"), "hours and minutes");
    ci_span(t, sizeof t, 2 * 86400 + 4 * 3600); CHECK(!strcmp(t, "2 d 4 h"), "days and hours");
    ci_span(t, sizeof t, 23 * 86400);       CHECK(!strcmp(t, "3 wk 2 d"), "weeks and days");
    ci_span(t, sizeof t, 130 * 86400);      CHECK(!strcmp(t, "4 mo 1 wk"), "months and weeks");
    ci_span(t, sizeof t, 31 * 86400 + 3600); CHECK(!strcmp(t, "1 mo 1 d"), "a month and the days left");

    char w[400], k[24];
    int n = ci_build_point(w, sizeof w, "X3CAFE", g_now, "38.7100,-9.1400", g_now + 3600, "", "By the river", "coffee");
    CHECK(n > 0 && strstr(w, "title:check-ins-open tag:checkin,coffee "), "the kind rides on tag:, no new word");
    ci_point_type(w, k, sizeof k);
    CHECK(!strcmp(k, "coffee"), "and is read back");
    ci_build_point(w, sizeof w, "X3CAFE", g_now, "38.7100,-9.1400", g_now + 3600, "", "", "spaceship");
    CHECK(strstr(w, "tag:checkin ") != 0, "a kind not on the list is left out");
    CHECK(ci_places_n >= 50 && ci_place("campsite") && ci_place("boat") && ci_place("car") && ci_place("bus"),
          "some fifty kinds, the moving ones too");
    CHECK(!ci_place("event") && !ci_place("badge") && !ci_place("auto"),
          "no kind takes the name of an automatic tag");
    for (unsigned i = 0; i < ci_places_n; i++) {
        char lbl[32];
        ci_label(lbl, sizeof lbl, ci_places[i].id);
        if (strcmp(lbl, ci_places[i].id)) { CHECK(0, "every kind is a tag: label"); break; }
    }
}

static void test_places(void)
{
    g_now += 5 * 86400;
    heard_ble("X3CAFE", 5);
    g_lat = 387100000; g_lon = -91400000;
    {
        /* The station says what kind of place it is on its check-in point,
         * and the visitor's archive holds that announcement. */
        char w[400], hist[900];
        int n = ci_build_point(w, sizeof w, "X3CAFE", g_now, "38.7071,-9.1393",
                               g_now + 3600, "", "", "coffee");
        CHECK(n > 0 && strstr(w, "title:check-ins-open tag:checkin,coffee") != 0,
              "the station says what kind of place it is on its check-in point");
        snprintf(hist, sizeof hist, "[{\"id\":\"cf0001\",\"wire\":\"%s\"}]", w);
        strcpy(g_history, hist);
    }

    /* The first check-in is asked by hand. */
    mock_clear();
    CHECK(round_trip("X1CHSR", "X3CAFE", 0), "first check-in, by hand");
    CHECK(said("First check-in at X3CAFE") && said("Next time here you are checked in automatically"),
          "and the person is told the next ones are automatic");
    mock_clear();
    cmd("{\"command\":\"ready\"}");
    CHECK(said("\"id\":\"st:X3CAFE\"") && said("\"Coffee\""), "it is a place, of its kind");
    CHECK(said("\"field\":\"log_tags\"") && said("{\"id\":\"coffee\",\"label\":\"Coffee\",\"count\":1"),
          "its kind is a tag over the list, counted");

    /* Time in its Bluetooth range: read from the core every few minutes. */
    unsigned long long t0 = g_now;
    heard_ble("X3CAFE", 5);
    module_tick();
    g_now += 180; heard_ble("X3CAFE", 10); module_tick();
    g_now += 180; heard_ble("X3CAFE", 10); module_tick();
    mock_clear();
    cmd("{\"command\":\"ready\"}");
    CHECK(said("here now"), "in range: here now");
    g_now += 900; heard_nobody(); module_tick();
    unsigned long long secs = (t0 + 360 - 10) - (t0 - 5);
    CHECK(one_u("SELECT COUNT(*) AS n FROM visits WHERE call = 'X3CAFE'", 0, "n") == 1 &&
          one_u("SELECT secs FROM visits WHERE call = 'X3CAFE'", 0, "secs") == secs,
          "out of range ten minutes: one visit, ending when last heard");
    mock_clear();
    cmd("{\"command\":\"ready\"}");
    CHECK(said("1 visit \xc2\xb7 5 min"), "the place says its visits and the time (355 s)");
    CHECK(said("\"id\":\"time\",\"label\":\"Time\""), "and a tile counts all the time");

    /* Passing by is not a visit. */
    g_now += 3600; heard_ble("X3CAFE", 1); module_tick();
    g_now += 60; heard_nobody(); g_now += 700; module_tick();
    CHECK(one_u("SELECT COUNT(*) AS n FROM visits WHERE call = 'X3CAFE'", 0, "n") == 1, "a minute in range is passing by");

    /* The next day it checks in by itself, once. */
    g_now += 86400;
    heard_ble("X3CAFE", 5);
    mock_clear();
    as("X1CHSR");
    module_tick();
    CHECK(g_aired_n == 1 && strstr(last_aired(), "cmd:checkin") && strstr(last_aired(), "d:X3CAFE") &&
          !said("Asking X3CAFE"), "in reach the next day: asked automatically, quietly");
    {
        char row_[4096], ask[300], cert[300];
        snprintf(ask, sizeof ask, "%s", last_aired());
        station_answer("X3CAFE", ask, cert, sizeof cert);
        mock_clear();
        result_row(row_, sizeof row_, cert);
        event("xprs.result", row_);
        CHECK(said("Checked in automatically at X3CAFE"), "and told when it is in");
    }
    mock_clear();
    g_now += 600; module_tick();
    CHECK(g_aired_n == 0, "once a day");

    /* The person turns it off for this place. */
    cmd("{\"command\":\"log_tap\",\"log_id\":\"st:X3CAFE\"}");
    CHECK(said("\"field\":\"a_auto\",\"value\":true") && said("\"label\":\"Kind of place\",\"value\":\"Coffee\"") &&
          said("\"title\":\"Time here\"") && said("\"label\":\"Visits\""),
          "its page: automatic on, its kind, the time spent there");
    cmd("{\"command\":\"set_auto\",\"a_auto\":false}");
    g_now += 86400;
    heard_ble("X3CAFE", 5);
    mock_clear();
    module_tick();
    CHECK(g_aired_n == 0, "off: no automatic check-in");

    /* Tags of your own, and the list filtered by them. */
    cmd("{\"command\":\"add_tag\"}");
    CHECK(said("\"id\":\"tag:X3CAFE\"") && said("\"input\""), "Add a tag asks for a word");
    mock_clear();
    cmd("{\"command\":\"prompt\",\"prompt_id\":\"tag:X3CAFE\",\"prompt_input\":\"Morning Spot\"}");
    CHECK(said("{\"id\":\"morning-spot\",\"label\":\"morning-spot\",\"count\":1"), "a tag of your own, written as a label");
    mock_clear();
    cmd("{\"command\":\"log_tags_tap\",\"log_tags_id\":\"morning-spot\"}");
    CHECK(said("\"id\":\"st:X3CAFE\"") && said("\"on\":true"), "the list filtered by it");
    CHECK(said("\"field\":\"log_tags\",\"value\":[{\"id\":\"morning-spot\""), "a chosen tag comes first, in sight");
    mock_clear();
    cmd("{\"command\":\"log_tags_tap\",\"log_tags_id\":\"in-person\"}");
    CHECK(said("\"id\":\"st:X3CAFE\""), "two tags: both must hold");
    mock_clear();
    cmd("{\"command\":\"log_search\",\"log_query\":\"nothing-like-this\"}");
    CHECK(!said("\"id\":\"st:X3CAFE\""), "the search narrows too");
    cmd("{\"command\":\"log_search\",\"log_query\":\"\"}");
    mock_clear();
    cmd("{\"command\":\"tags_open\"}");
    CHECK(said("\"field\":\"tags_list\"") && said("\"id\":\"tag:coffee\"") && said("1 place"),
          "the Tags page: every tag, its places");
    mock_clear();
    cmd("{\"command\":\"tags_list_tap\",\"tags_list_id\":\"tag:coffee\"}");
    CHECK(said("{\"id\":\"coffee\",\"label\":\"Coffee\",\"count\":1,\"on\":true}") &&
          said("{\"id\":\"morning-spot\",\"label\":\"morning-spot\",\"count\":1,\"on\":false}"),
          "a tag there filters the list by it alone");
    cmd("{\"command\":\"log_tap\",\"log_id\":\"st:X3CAFE\"}");
    cmd("{\"command\":\"prompt\",\"prompt_id\":\"untag:X3CAFE\",\"prompt_value\":\"morning-spot\"}");
    CHECK(!one_u("SELECT COUNT(*) AS n FROM mytags", 0, "n"), "and it can be taken off");
    cmd("{\"command\":\"log_tags_tap\",\"log_tags_id\":\"coffee\"}");
    strcpy(g_history, "[]");
    heard_nobody();
}


int main(void)
{
    test_rules();
    test_station();
    test_migration();
    test_chaser();
    test_place_rules();
    test_places();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
