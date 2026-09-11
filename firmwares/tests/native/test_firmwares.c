/*
 * The Firmwares wapp against a mock HAL: every step of setting a station up
 * (XPRS.md 11.9, 11.10), as the core would drive it, in one process.
 *
 * What these hold the wapp to:
 *   - it believes an ask to be claimed only when the core verified it and
 *     the callsign derives from the key it carries, and says so once;
 *   - every wire it builds fits 250 bytes once the host has signed it;
 *   - a password reaches hal_encrypt and nothing else: not a wire, not the
 *     KV, not the screen, and the field is cleared;
 *   - it re-sends the identical wire, gives up at five minutes, re-stamps a
 *     408 once, follows a new key, and reads the stats it asked for.
 */
#include <stdio.h>
#include <string.h>

#include "../../main.c"

/* from hal_mock.c */
extern char g_sealed_plain[16][256];
extern char g_sealed_to[16][80];
extern int g_sealed_n;
extern char g_aired[64][260];
extern int g_aired_n;
extern int g_send_rc;
extern uint64_t g_ms, g_epoch;
extern const char *g_mock_npub;
void cap_clear(void);
int cap_count(const char *s);
const char *cap_last(const char *s);
void inbox_set(const char *s);
void event_push(const char *t, const char *d);
int subscribed(const char *t);
const char *kv_dump(void);

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A station key and the callsign it derives to: X3AB3D. */
static const char *ST_NPUB = "npub1ab3dqpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jnqp";
static const char *ST = "X3AB3D";
static const char *NEW_NPUB = "npub1k7w2qpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jnqp";

static char g_row[1200];
static const char *row(const char *type, const char *from, const char *sig, const char *wire)
{
    snprintf(g_row, sizeof g_row,
             "{\"id\":\"000000\",\"type\":\"%s\",\"from\":\"%s\",\"to\":\"\",\"ts\":\"\","
             "\"fields\":[],\"forUs\":false,\"sealed\":false,\"scope\":\"local\","
             "\"bearer\":\"ble\",\"rssi\":-71,\"via\":\"\",\"link\":\"\",\"sig\":\"%s\","
             "\"wire\":\"%s\"}", type, from, sig, wire);
    return g_row;
}

static void deliver(const char *topic, const char *r) { event_push(topic, r); module_handle_event(); }
static void command(const char *json) { inbox_set(json); module_handle_event(); }

static void ask_owner(const char *sig, const char *call, const char *npub)
{
    char w[300];
    snprintf(w, sizeof w, "t:request f:%s q:owner scope:local ts:2026-09-10_12:00:00 k:%s sig:KKKK", call, npub);
    deliver("xprs.request", row("request", call, sig, w));
}

static void result(const char *from, const char *id, const char *tail)
{
    char w[300];
    snprintf(w, sizeof w, "t:result f:%s d:X1ME77 ts:2026-09-10_12:00:01 r:%s %s", from, id, tail);
    deliver("xprs.result", row("result", from, "verified", w));
}

static const char *last_aired(void) { return g_aired_n ? g_aired[g_aired_n - 1] : ""; }
static void last_id(char out[7]) { fw_id(last_aired(), fw_len(last_aired()), out); }

static void test_keys_are_real_length(void)
{
    CHECK(strlen(ST_NPUB) == 63 && strlen(NEW_NPUB) == 63 && strlen(g_mock_npub) == 63,
          "fixtures are npub-length");
}

static void test_ask_is_believed_only_when_it_should_be(void)
{
    ask_owner("unverified", ST, ST_NPUB);
    CHECK(cap_count("Waiting for an owner") == 0, "an unverified ask is not a station");
    ask_owner("verified", "X3ZZZZ", ST_NPUB);
    CHECK(cap_count("Waiting for an owner") == 0, "a callsign the key does not derive to is nobody");
    ask_owner("verified", ST, ST_NPUB);
    CHECK(cap_count("Waiting for an owner") >= 1, "a verified ask lists the station");
    CHECK(cap_count("\"type\":\"notify\"") == 1, "and the person is told");
    CHECK(cap_count("firmwares.unowned.X3AB3D") == 1, "tagged, so the host tells them once ever");
    ask_owner("verified", ST, ST_NPUB);
    CHECK(cap_count("\"type\":\"notify\"") == 1, "a second ask is not a second notification");
    CHECK(strstr(kv_dump(), "st.X3AB3D=npub1ab3d") != 0, "kept across the engine's restarts");
}

static void test_claim(void)
{
    char tap[200];
    snprintf(tap, sizeof tap, "{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"%s\"}}", ST);
    command(tap);
    CHECK(cap_count("\"ui.screen.open\",\"name\":\"Station\"") == 1, "the Station screen opens");
    CHECK(cap_last("\"field\":\"detail\"") != 0, "with the station in it");

    int before = g_aired_n;
    command("{\"command\":\"claim\",\"fields\":{}}");
    CHECK(g_aired_n == before + 1, "one claim aired");
    const char *w = last_aired();
    CHECK(strstr(w, "t:command f:X1ME77 d:X3AB3D ts:") == w, "addressed to the station: %s", w);
    CHECK(strstr(w, " cmd:set owner:X1ME77 k:npub1me77") != 0, "claims with our key: %s", w);
    CHECK(strlen(w) + 65 <= 250, "fits once signed (%zu)", strlen(w) + 65);
    CHECK(subscribed("xprs.observation"), "watching for a clock while it waits");

    char id[7];
    last_id(id);
    result(ST, id, "code:200 owner:X1ME77 use:all first:none serve:archive");
    CHECK(cap_count("Claimed.") == 1, "the person is told it is theirs");
    CHECK(cap_last("\"title\":\"Yours\"") != 0, "and it moves to Yours");
    CHECK(!subscribed("xprs.observation"), "and the clock is let go");
}

static void test_wifi_sealed_in_one(void)
{
    g_sealed_n = 0;
    cap_clear();
    command("{\"command\":\"wifi_apply\",\"fields\":{\"ssid\":\"Casa do Mar\",\"wifi_pass\":\"sardinha na brasa 2026\"}}");
    CHECK(g_sealed_n == 1, "sealed once");
    CHECK(strcmp(g_sealed_plain[0], "cmd:set\nssid:Casa do Mar\npass:sardinha na brasa 2026") == 0,
          "the body is 11.10's lines: %s", g_sealed_plain[0]);
    CHECK(strcmp(g_sealed_to[0], ST_NPUB) == 0, "sealed to the station's key");
    const char *w = last_aired();
    CHECK(strstr(w, " x:") != 0 && !strstr(w, "cmd:"), "cmd: rides inside x:: %s", w);
    CHECK(!strstr(w, "sardinha") && !strstr(w, "Casa"), "no plaintext on the air");
    CHECK(strlen(w) + 65 <= 250, "fits once signed (%zu)", strlen(w) + 65);
    CHECK(!strstr(kv_dump(), "sardinha"), "not in the KV");
    CHECK(cap_count("sardinha") == 0, "not on any screen");
    CHECK(cap_count("\"field\":\"wifi_pass\",\"value\":\"\"") == 1, "and the field is cleared");
    CHECK(g_buf[0] == 0, "and the command's buffer holds nothing after it");

    char id[7];
    last_id(id);
    result(ST, id, "code:202 wifi:joining ap:on zone:auto");
    CHECK(cap_count("Joining the network") == 1, "202 says it is joining");
    result(ST, id, "code:200 wifi:up ip:192.168.1.40 ap:on zone:auto");
    CHECK(cap_count("On the network at 192.168.1.40") == 1, "200 says where");
}

static void test_wifi_long_goes_in_two(void)
{
    g_sealed_n = 0;
    cap_clear();
    command("{\"command\":\"wifi_apply\",\"fields\":{\"ssid\":\"abcdefghijklmnopqrstuvwxyz012345\","
            "\"wifi_pass\":\"ppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp\"}}");
    CHECK(g_sealed_n == 2, "two bodies sealed: %d", g_sealed_n);
    CHECK(strncmp(g_sealed_plain[1], "cmd:set\nssid:", 13) == 0 &&
          strncmp(g_sealed_plain[0], "cmd:set\npass:", 13) == 0, "the name on its own, the password on its own");
    int aired = g_aired_n;
    char id[7];
    last_id(id);
    CHECK(strlen(last_aired()) + 65 <= 250, "the first fits");
    result(ST, id, "code:200 wifi:off ap:on zone:auto");
    CHECK(g_aired_n == aired + 1, "the password follows the name's 200");
    CHECK(strlen(last_aired()) + 65 <= 250, "and fits (%zu)", strlen(last_aired()) + 65);
    last_id(id);
    result(ST, id, "code:202 wifi:joining ap:on zone:auto");
    result(ST, id, "code:500 wifi:failed ap:on zone:auto sig:KKKK m:wrong password");
    CHECK(cap_count("Could not join: wrong password") == 1, "the reason, in its own words");
}

static void test_retry_and_give_up(void)
{
    cap_clear();
    command("{\"command\":\"stats\",\"fields\":{}}");
    int aired = g_aired_n;
    char first[260];
    snprintf(first, sizeof first, "%s", last_aired());
    g_ms += 31000;
    deliver("xprs.observation", row("observation", ST, "verified", "t:observation f:X3AB3D link:ble"));
    CHECK(g_aired_n == aired + 1, "re-sent after 30 s");
    CHECK(strcmp(last_aired(), first) == 0, "the identical wire");
    g_ms += 10000;
    deliver("xprs.observation", row("observation", ST, "verified", "t:observation f:X3AB3D link:ble"));
    CHECK(g_aired_n == aired + 1, "not before its time");
    g_ms += 300000;
    deliver("xprs.observation", row("observation", ST, "verified", "t:observation f:X3AB3D link:ble"));
    CHECK(cap_count("No answer in five minutes") == 1, "and gives up, out loud");
    CHECK(!subscribed("xprs.observation"), "letting the clock go");
}

static void test_408_restamps_once(void)
{
    cap_clear();
    command("{\"command\":\"station_apply\",\"fields\":{\"nick\":\"roof\",\"zone\":\"+01:00\",\"hotspot\":\"off\"}}");
    const char *w = last_aired();
    CHECK(strstr(w, " cmd:set nick:roof zone:+01:00 ap:off") != 0, "the settings, in the clear: %s", w);
    char id[7];
    last_id(id);
    int aired = g_aired_n;
    result(ST, id, "code:408 m:not newer than the last cmd:set");
    CHECK(g_aired_n == aired + 1, "said again, re-stamped");
    CHECK(strcmp(last_aired(), w) != 0, "a new ts:, a new command");
    CHECK(strstr(last_aired(), " cmd:set nick:roof zone:+01:00 ap:off") != 0, "the same settings");
    last_id(id);
    result(ST, id, "code:408 m:not newer than the last cmd:set");
    CHECK(cap_count("phone's clock") == 1, "the second 408 is the clock, said once");
}

static void test_zdiag(void)
{
    cap_clear();
    command("{\"command\":\"stats\",\"fields\":{}}");
    CHECK(strstr(last_aired(), " cmd:zdiag") != 0, "asks zdiag");
    char id[7];
    last_id(id);
    result(ST, id, "code:200 fw:0.1.0 uptime:2h peers:4 zr:power-on zm:58/31/35 zh:1f/1f zn:1/2/3/4 zs:1/2/3 zp:ota_0/2");
    const char *d = cap_last("\"field\":\"detail\"");
    CHECK(d && strstr(d, "0.1.0") && strstr(d, "58/31/35") && strstr(d, "power-on"),
          "the stats are on the screen");
}

static void test_new_key(void)
{
    cap_clear();
    command("{\"command\":\"identity_apply\",\"fields\":{\"identity\":\"new\",\"nsec\":\"\"}}");
    CHECK(strstr(last_aired(), " cmd:set key:new") != 0, "key:new, in the clear");
    char id[7];
    last_id(id);
    char tail[160];
    snprintf(tail, sizeof tail, "code:202 k:%s", NEW_NPUB);
    result(ST, id, tail);
    CHECK(cap_count("Restarting under a new identity, X3K7W2") == 1, "it names the new callsign");
    char w[300];
    snprintf(w, sizeof w, "t:identity f:X3K7W2 ts:2026-09-10_12:00:20 k:%s sig:KKKK", NEW_NPUB);
    deliver("xprs.identity", row("identity", "X3K7W2", "verified", w));
    CHECK(find("X3K7W2") >= 0 && find(ST) < 0, "the station is followed to its new callsign");
    result("X3K7W2", id, "code:200 wifi:up ip:192.168.1.40 ap:off nick:roof zone:+01:00");
    CHECK(cap_count("New identity in use") == 1, "and its 200 closes it");
}

static void test_new_key_202_missed(void)
{
    cap_clear();
    command("{\"command\":\"identity_apply\",\"fields\":{\"identity\":\"new\",\"nsec\":\"\"}}");
    char id[7];
    last_id(id);
    /* The 202 never arrives; the restarted station announces itself and
     * answers under its new callsign. */
    const char *nk = "npub1h8zqqpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jnqp";
    char w[300];
    snprintf(w, sizeof w, "t:identity f:X3H8ZQ ts:2026-09-10_12:01:00 k:%s sig:KKKK", nk);
    deliver("xprs.identity", row("identity", "X3H8ZQ", "verified", w));
    result("X3H8ZQ", id, "code:200 wifi:up ip:192.168.1.40 ap:off zone:+01:00");
    CHECK(find("X3H8ZQ") >= 0, "followed by its announced key");
    CHECK(cap_count("New identity in use") == 1, "and closed");
}

static void test_new_key_answer_brings_its_key(void)
{
    cap_clear();
    command("{\"command\":\"identity_apply\",\"fields\":{\"identity\":\"new\",\"nsec\":\"\"}}");
    char id[7];
    last_id(id);
    /* Neither the 202 nor the t:identity arrived: only the 200, which says
     * which key it is under. */
    const char *nk = "npub1t9vzqpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jnqp";
    char tail[160];
    snprintf(tail, sizeof tail, "code:200 k:%s wifi:up ip:192.168.1.40", nk);
    result("X3T9VZ", id, tail);
    CHECK(find("X3T9VZ") >= 0, "followed by the key in its answer");
    CHECK(cap_count("New identity in use") == 1, "and closed");
}

static void test_reset_station_starts_over(void)
{
    int i = g_sel;
    cap_clear();
    char w[300];
    snprintf(w, sizeof w, "t:request f:%s q:owner scope:local ts:2026-09-10_13:00:00 k:%s sig:KKKK",
             g_st[i].call, g_st[i].npub);
    deliver("xprs.request", row("request", g_st[i].call, "verified", w));
    CHECK(!g_st[i].mine && g_st[i].unowned, "back to waiting for an owner");
    CHECK(!g_st[i].nick[0] && !g_st[i].fw[0], "and nothing stale kept");
}

static void test_import_is_sealed(void)
{
    g_sealed_n = 0;
    cap_clear();
    command("{\"command\":\"identity_apply\",\"fields\":{\"identity\":\"import\",\"nsec\":"
            "\"nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq\"}}");
    CHECK(g_sealed_n == 1 && strncmp(g_sealed_plain[0], "cmd:set\nnsec:nsec1", 18) == 0, "sealed");
    CHECK(!strstr(last_aired(), "nsec1"), "never on the air in the clear");
    CHECK(strlen(last_aired()) + 65 <= 250, "fits (%zu)", strlen(last_aired()) + 65);
    CHECK(cap_count("\"field\":\"nsec\",\"value\":\"\"") == 1, "the field is cleared");
}

static void test_refusal_words(void)
{
    cap_clear();
    char id[7];
    last_id(id);
    command("{\"command\":\"stats\",\"fields\":{}}");
    last_id(id);
    result(g_st[g_sel].call, id, "code:403 sig:KKKK m:not the owner");
    CHECK(cap_count("Refused: not the owner") == 1, "a 403 in its own words");
}

int main(void)
{
    module_init();
    test_keys_are_real_length();
    test_ask_is_believed_only_when_it_should_be();
    test_claim();
    test_wifi_sealed_in_one();
    test_wifi_long_goes_in_two();
    test_retry_and_give_up();
    test_408_restamps_once();
    test_zdiag();
    test_new_key();
    test_new_key_202_missed();
    test_new_key_answer_brings_its_key();
    test_import_is_sealed();
    test_reset_station_starts_over();
    test_refusal_words();
    printf("firmwares: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
