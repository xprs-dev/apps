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
 *   - it sends a command once and leaves the rest to the core: no clock, no
 *     re-send, and it hears answers only while it has asked something;
 *   - it believes an answer only when it is for this phone and verified,
 *     and a station only under the key it first came with;
 *   - it re-stamps a 408 once, follows a new key, and reads the stats it
 *     asked for.
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
extern char g_station_json[1024];
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
static const char *row_to(const char *type, const char *from, const char *sig, int for_us,
                          const char *wire)
{
    snprintf(g_row, sizeof g_row,
             "{\"id\":\"000000\",\"type\":\"%s\",\"from\":\"%s\",\"to\":\"\",\"ts\":\"\","
             "\"fields\":[],\"forUs\":%s,\"sealed\":false,\"scope\":\"local\","
             "\"bearer\":\"ble\",\"rssi\":-71,\"via\":\"\",\"link\":\"\",\"sig\":\"%s\","
             "\"wire\":\"%s\"}", type, from, for_us ? "true" : "false", sig, wire);
    return g_row;
}
static const char *row(const char *type, const char *from, const char *sig, const char *wire)
{
    return row_to(type, from, sig, 0, wire);
}

static void deliver(const char *topic, const char *r) { event_push(topic, r); module_handle_event(); }
static void command(const char *json) { inbox_set(json); module_handle_event(); }

static void ask_owner(const char *sig, const char *call, const char *npub)
{
    char w[300];
    snprintf(w, sizeof w, "t:request f:%s q:owner scope:local ts:2026-09-10_12:00:00 k:%s sig:KKKK", call, npub);
    deliver("xprs.request", row("request", call, sig, w));
}

static void result_as(const char *from, const char *id, const char *sig, int for_us,
                      const char *tail)
{
    char w[300];
    snprintf(w, sizeof w, "t:result f:%s d:X1ME77 ts:2026-09-10_12:00:01 r:%s %s", from, id, tail);
    deliver("xprs.result", row_to("result", from, sig, for_us, w));
}
static void result(const char *from, const char *id, const char *tail)
{
    result_as(from, id, "verified", 1, tail);
}

static void status_tx(const char *id, const char *state)
{
    char r[160];
    snprintf(r, sizeof r, "{\"id\":\"%s\",\"peer\":\"X3AB3D\",\"state\":\"%s\"}", id, state);
    deliver("xprs.status.tx", r);
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
    int said = cap_count("ui.people.set");
    ask_owner("verified", ST, ST_NPUB);
    CHECK(cap_count("\"type\":\"notify\"") == 1, "a second ask is not a second notification");
    CHECK(cap_count("ui.people.set") == said, "nor a second list: a repeat is not news");
    CHECK(strstr(kv_dump(), "st.X3AB3D=npub1ab3d") != 0, "kept across the engine's restarts");
    CHECK(subscribed("xprs.request") && !subscribed("xprs.result") &&
          !subscribed("xprs.identity") && !subscribed("xprs.observation"),
          "an idle phone listens for asks and nothing else");
}

static void test_a_callsign_is_its_key(void)
{
    /* Somebody ground a key whose callsign is X3AB3D too: the wapp keeps the
     * one it knew. The mock's derivation check is real, so the ground key
     * has to derive for the test to mean anything; here it stands in. */
    const char *other = "npub1ab3dqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq";
    int i = find(ST);
    ask_owner("verified", ST, other);
    CHECK(fw_eq(g_st[i].npub, ST_NPUB), "the station keeps the key it came with");
}

static void test_claim(void)
{
    char tap[200];
    snprintf(tap, sizeof tap, "{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"%s\"}}", ST);
    command(tap);
    CHECK(cap_count("\"ui.screen.open\",\"name\":\"Station\"") == 1, "the Station screen opens");
    CHECK(cap_last("\"field\":\"hub\"") != 0, "with the station's tiles in it");
    CHECK(cap_count("\"field\":\"claim__hidden\",\"value\":false") == 1, "Claim shows while nobody owns it");
    CHECK(cap_count("\"field\":\"open_wifi__hidden\",\"value\":true") == 1, "and WiFi does not");

    int before = g_aired_n;
    command("{\"command\":\"claim\",\"fields\":{}}");
    CHECK(g_aired_n == before + 1, "one claim aired");
    const char *w = last_aired();
    CHECK(strstr(w, "t:command f:X1ME77 d:X3AB3D ts:") == w, "addressed to the station: %s", w);
    CHECK(strstr(w, " cmd:set owner:X1ME77 k:npub1me77") != 0, "claims with our key: %s", w);
    CHECK(strlen(w) + 65 <= 250, "fits once signed (%zu)", strlen(w) + 65);
    CHECK(subscribed("xprs.result") && subscribed("xprs.status.tx"), "listening for its answer");
    CHECK(!subscribed("xprs.observation"), "and not for a clock");

    char id[7];
    last_id(id);
    result_as(ST, id, "verified", 0, "code:200 owner:X1ME77 use:all first:none serve:archive");
    CHECK(cap_count("Claimed.") == 0, "an answer to somebody else is not ours");
    result_as(ST, id, "unverified", 1, "code:200 owner:X1ME77 use:all first:none serve:archive");
    CHECK(cap_count("Claimed.") == 0, "an answer nobody can check is not believed");
    result(ST, id, "code:200 owner:X1ME77 use:all first:none serve:archive");
    CHECK(cap_count("Claimed.") == 1, "the person is told it is theirs");
    CHECK(cap_last("\"title\":\"Yours\"") != 0, "and it moves to Yours");
    CHECK(cap_count("\"field\":\"claim__hidden\",\"value\":true") >= 1, "Claim goes");
    CHECK(cap_count("\"field\":\"open_wifi__hidden\",\"value\":false") >= 1, "WiFi, Name and Identity come");
    CHECK(!subscribed("xprs.result") && !subscribed("xprs.status.tx"), "and stops listening");
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
    CHECK(cap_count("Joining the network") >= 1, "202 says it is joining");
    result(ST, id, "code:200 wifi:up ip:192.168.1.40 ap:on zone:auto");
    CHECK(cap_count("On the network at 192.168.1.40") >= 1, "200 says where");
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
    CHECK(cap_count("Could not join: wrong password") >= 1, "the reason, in its own words");
}

static void test_the_core_delivers(void)
{
    cap_clear();
    command("{\"command\":\"stats\",\"fields\":{}}");
    int aired = g_aired_n;
    char id[7];
    last_id(id);
    g_ms += 400000;
    ask_owner("verified", "X3ZZZZ", ST_NPUB);  /* anything at all arriving */
    CHECK(g_aired_n == aired, "the wapp never says it twice: re-airing is the core's");
    status_tx("abcdef", "unanswered");
    CHECK(cap_count("No answer in five minutes") == 0, "another command's fate is not this one's");
    status_tx(id, "unanswered");
    CHECK(cap_count("No answer in five minutes") >= 1, "the core gave up, and the person is told");
    CHECK(!subscribed("xprs.result"), "and nothing is listened for any more");
    command("{\"command\":\"stats\",\"fields\":{}}");
    last_id(id);
    result(ST, id, "code:202");
    status_tx(id, "unfinished");
    CHECK(cap_count("never said how it ended") >= 1, "taken and never finished is said so");
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
    CHECK(cap_count("phone's clock") >= 1, "the second 408 is the clock, said once");
}

static void test_zdiag(void)
{
    cap_clear();
    command("{\"command\":\"stats\",\"fields\":{}}");
    CHECK(strstr(last_aired(), " cmd:zdiag") != 0, "asks zdiag");
    char id[7];
    last_id(id);
    result(ST, id, "code:200 fw:0.1.0 uptime:2h peers:4 zr:power-on zm:58/31/35 zh:1e/1f zn:1/2/3/4 zs:5/6/0 zp:ota_0/2 zc:idx");
    const char *d = cap_last("\"field\":\"st_diag\"");
    CHECK(d && strstr(d, "\"value\":\"58\",\"unit\":\"KB\",\"hint\":\"largest 31, lowest 35\""),
          "memory: now, with the largest block and the lowest ever: %s", d ? d : "");
    CHECK(d && strstr(d, "power-on"), "the reset reason");
    CHECK(d && strstr(d, "\"value\":\"1 part down\""), "zh: one required part is down");
    CHECK(d && strstr(d, "\"label\":\"Slot\",\"value\":\"ota_0\""), "the OTA slot");
    CHECK(d && strstr(d, "\"label\":\"Sent\",\"value\":\"5\",\"hint\":\"of 6, failed 0\""),
          "zs is done/issued/fail on the wire");
    CHECK(d && strstr(d, "\"label\":\"Crashed in\",\"value\":\"idx\""), "a crash, shown");
    CHECK(cap_count("\"field\":\"crash__hidden\",\"value\":false") >= 1, "and its button");
    const char *st = cap_last("\"field\":\"st_station\"");
    CHECK(st && strstr(st, "\"label\":\"Firmware\",\"value\":\"0.1.0\""), "15.5 tiles from the answer");
}

static void test_new_key(void)
{
    cap_clear();
    command("{\"command\":\"identity_apply\",\"fields\":{\"identity\":\"new\",\"nsec\":\"\"}}");
    CHECK(strstr(last_aired(), " cmd:set key:new") != 0, "key:new, in the clear");
    CHECK(subscribed("xprs.identity"), "identities are heard while the key changes");
    char id[7];
    last_id(id);
    char tail[160];
    snprintf(tail, sizeof tail, "code:202 k:%s", NEW_NPUB);
    result(ST, id, tail);
    CHECK(cap_count("Restarting under a new identity, X3K7W2") >= 1, "it names the new callsign");
    char w[300];
    snprintf(w, sizeof w, "t:identity f:X3K7W2 ts:2026-09-10_12:00:20 k:%s sig:KKKK", NEW_NPUB);
    deliver("xprs.identity", row("identity", "X3K7W2", "verified", w));
    CHECK(find("X3K7W2") >= 0 && find(ST) < 0, "the station is followed to its new callsign");
    result("X3K7W2", id, "code:200 wifi:up ip:192.168.1.40 ap:off nick:roof zone:+01:00");
    CHECK(cap_count("New identity in use") >= 1, "and its 200 closes it");
    CHECK(!subscribed("xprs.identity"), "and identities are nobody's business again");
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
    CHECK(cap_count("New identity in use") >= 1, "and closed");
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
    CHECK(cap_count("New identity in use") >= 1, "and closed");
}

static void test_stats_from_the_core(void)
{
    /* What the core heard on beacons and service announcements (15.5) is
     * read through hal_xprs_station, and the policy comes back as an
     * observation anybody may ask for (11.9). */
    cap_clear();
    /* The import above is still waiting; the core gives up on it. */
    if (g_st[g_sel].pend_id[0]) status_tx(g_st[g_sel].pend_id, "unanswered");
    snprintf(g_station_json, sizeof g_station_json,
             "{\"call\":\"%s\",\"bearer\":\"ble\",\"bearers\":[\"ble\",\"lan\"],\"rssi\":-71,"
             "\"lastMs\":5,\"agoMs\":12000,\"lastDirectMs\":5,\"packets\":9,\"peers\":4,\"mail\":3,"
             "\"uptime\":\"26h\",\"lifetime\":\"38day\",\"fw\":\"0.4.0\",\"count\":1234,"
             "\"serve\":[\"archive\"],\"hears\":[\"X1WATT\",\"X3MEAV\"],\"sig\":\"verified\"}",
             g_st[g_sel].call);
    int before = g_aired_n;
    command("{\"command\":\"open_stats\",\"fields\":{}}");
    CHECK(g_aired_n == before + 3, "opening the screen asks: policy, mail, zdiag (%d)", g_aired_n - before);
    {
        /* The station's earlier figures stay on the screen; a tile it never
         * filled says the ask is out. */
        const char *d = cap_last("\"field\":\"st_policy\"");
        CHECK(d && strstr(d, "\"value\":\"...\""), "an empty tile says it is being asked for: %s", d ? d : "-");
    }
    {   /* the answers land, and the screen is redrawn with them */
        char id[7]; last_id(id);
        result(g_st[g_sel].call, id, "code:200 fw:0.4.0 uptime:26h peers:4 zr:sw zm:60/40/30 zh:3/3 zn:0/0/0/0 zs:0/0/0 zp:ota_1/1");
        const char *d = cap_last("\"field\":\"st_diag\"");
        CHECK(d && strstr(d, "\"value\":\"60\",\"unit\":\"KB\""), "and the figures are on the screen");
        CHECK(strstr(kv_dump(), "stx.") != 0, "kept for the next open");
    }
    cap_clear();
    command("{\"command\":\"open_stats\",\"fields\":{}}");
    const char *st = cap_last("\"field\":\"st_station\"");
    CHECK(st && strstr(st, "\"label\":\"Lifetime\",\"value\":\"38day\""), "lifetime, as aired");
    CHECK(st && strstr(st, "\"label\":\"Records\",\"value\":\"1234\""), "the archive's count");
    CHECK(st && strstr(st, "\"label\":\"Signal\",\"value\":\"-71\",\"unit\":\"dBm\",\"hint\":\"BLE, 12 s ago\""),
          "signal and freshness from the core: %s", st ? st : "");
    CHECK(st && strstr(st, "\"label\":\"Lanes\",\"value\":\"2\",\"hint\":\"ble, lan\""), "every bearer it came in on");
    CHECK(cap_last("\"field\":\"st_hears\"") && strstr(cap_last("\"field\":\"st_hears\""), "X1WATT, X3MEAV"),
          "who it hears");
    CHECK(cap_count("\"ui.screen.open\",\"name\":\"Stats\"") == 1, "the Stats screen opens");
    {   /* close the open's zdiag so Refresh may send */
        char id[7]; last_id(id);
        result(g_st[g_sel].call, id, "code:200 fw:0.4.0 uptime:26h peers:4 zr:sw zm:60/40/30 zh:3/3 zn:0/0/0/0 zs:0/0/0 zp:ota_1/1");
    }

    int aired = g_aired_n;
    command("{\"command\":\"stats\",\"fields\":{}}");
    CHECK(g_aired_n == aired + 3, "Refresh asks policy, mail and zdiag: %d", g_aired_n - aired);
    CHECK(strstr(g_aired[aired], " q:policy") && strstr(g_aired[aired], "t:request "), "q:policy is a request: %s", g_aired[aired]);
    CHECK(strstr(g_aired[aired + 1], " q:mail") != 0, "then q:mail");
    CHECK(subscribed("xprs.observation"), "observations are heard while the ask is out");
    char w[300];
    snprintf(w, sizeof w, "t:observation f:%s d:X1ME77 s:policy owner:X1ME77 use:all first:none serve:archive ts:2026-09-10_12:05:00 sig:KKKK",
             g_st[g_sel].call);
    deliver("xprs.observation", row_to("observation", g_st[g_sel].call, "verified", 1, w));
    const char *po = cap_last("\"field\":\"st_policy\"");
    CHECK(po && strstr(po, "\"label\":\"Owner\",\"value\":\"X1ME77\"") && strstr(po, "\"label\":\"Serve\",\"value\":\"archive\""),
          "the policy, on the screen");
    CHECK(!subscribed("xprs.observation"), "and observations are let go");
    snprintf(w, sizeof w, "t:observation f:%s d:X1ME77 s:mail mail:7 ts:2026-09-10_12:05:01 sig:KKKK", g_st[g_sel].call);
    deliver("xprs.observation", row_to("observation", g_st[g_sel].call, "verified", 1, w));
    st = cap_last("\"field\":\"st_station\"");
    CHECK(st && strstr(st, "\"label\":\"Mail held\",\"value\":\"3\""), "the core's mail count stands while it has one");
    char id[7];
    last_id(id);
    result(g_st[g_sel].call, id, "code:200 fw:0.4.0 uptime:26h peers:4 zr:sw zm:60/40/30 zh:3/3 zn:0/0/0/0 zs:0/0/0 zp:ota_1/1");
    CHECK(cap_count("\"field\":\"crash__hidden\",\"value\":true") >= 1, "no crash, no crash button");
    g_station_json[0] = 0;
}

static void test_somebody_elses_station(void)
{
    /* A station that answers a policy ask naming another owner is theirs:
     * listed under Others, with nothing but Stats and Answers offered. */
    cap_clear();
    char mine[12];
    snprintf(mine, sizeof mine, "%s", g_st[g_sel].call);   /* renamed by the rekeys above */
    ask_owner("verified", "X3XYZ1", "npub1xyz1qpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jnqp");
    char tap[200];
    snprintf(tap, sizeof tap, "{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"X3XYZ1\"}}");
    command(tap);
    command("{\"command\":\"stats\",\"fields\":{}}");
    deliver("xprs.observation", row_to("observation", "X3XYZ1", "verified", 1,
            "t:observation f:X3XYZ1 d:X1ME77 s:policy owner:X1OTHER use:listed first:none serve:relay ts:2026-09-10_12:06:00 sig:KKKK"));
    CHECK(cap_last("\"title\":\"Others\"") != 0, "listed under Others");
    CHECK(cap_count("\"field\":\"claim__hidden\",\"value\":true") >= 1, "no Claim");
    CHECK(cap_count("\"field\":\"open_wifi__hidden\",\"value\":true") >= 1, "no WiFi");
    command("{\"command\":\"forget\",\"fields\":{}}");
    CHECK(find("X3XYZ1") < 0, "forgotten on request");
    CHECK(cap_count("\"type\":\"ui.screen.close\"") >= 1, "and its screen closes");
    snprintf(tap, sizeof tap, "{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"%s\"}}", mine);
    command(tap);
    CHECK(g_sel >= 0 && strcmp(g_st[g_sel].call, mine) == 0, "back to our own station");
}

static void test_open_network_clears_the_box(void)
{
    cap_clear();
    g_sealed_n = 0;
    command("{\"command\":\"wifi_apply\",\"fields\":{\"ssid\":\"Cafe\",\"wifi_pass\":\"\"}}");
    CHECK(strncmp(g_sealed_plain[g_sealed_n - 1], "cmd:set\nssid:Cafe\nwifi:join", 27) == 0, "an open network joins by name: %s / %s",
          g_sealed_plain[g_sealed_n - 1], cap_last("ui.log.append") ? cap_last("ui.log.append") : "-");
    CHECK(cap_count("\"field\":\"wifi_pass\",\"value\":\"\"") == 1, "the password box is cleared either way");
    char id[7];
    last_id(id);
    result(g_st[g_sel].call, id, "code:200 wifi:up ip:10.0.0.9 ap:on zone:auto");
    CHECK(cap_count("On the network at 10.0.0.9") >= 1, "and it joins");
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
    CHECK(cap_count("Refused: not the owner") >= 1, "a 403 in its own words: %s",
          cap_last("ui.log.append") ? cap_last("ui.log.append") : "-");
}

int main(void)
{
    module_init();
    test_keys_are_real_length();
    test_ask_is_believed_only_when_it_should_be();
    test_a_callsign_is_its_key();
    test_claim();
    test_wifi_sealed_in_one();
    test_wifi_long_goes_in_two();
    test_the_core_delivers();
    test_408_restamps_once();
    test_zdiag();
    test_new_key();
    test_new_key_202_missed();
    test_new_key_answer_brings_its_key();
    test_import_is_sealed();
    test_stats_from_the_core();
    test_somebody_elses_station();
    test_open_network_clears_the_box();
    test_refusal_words();
    test_reset_station_starts_over();
    printf("firmwares: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
