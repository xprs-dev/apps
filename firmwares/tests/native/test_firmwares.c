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

/* 14.8: the LoRa mode is read from the station's answer, shown, and sent
 * only when it changes. */
static void test_lora_mode(void)
{
    cap_clear();
    command("{\"command\":\"stats\",\"fields\":{}}");
    char id[7];
    last_id(id);
    result(ST, id, "code:200 wifi:up ip:192.168.1.40 ap:off lora:meshtastic nick:roof zone:+01:00");
    const char *st = cap_last("\"field\":\"st_station\"");
    CHECK(st && strstr(st, "\"label\":\"LoRa\",\"value\":\"XPRS+Meshtastic\""),
          "the running mode, shown: %s", st ? st : "");

    cap_clear();
    command("{\"command\":\"open_name\",\"fields\":{}}");
    CHECK(cap_count("\"field\":\"lora__hidden\",\"value\":false") >= 1,
          "a station with a radio offers the choice");
    command("{\"command\":\"station_apply\",\"fields\":{\"nick\":\"\",\"zone\":\"+01:00\",\"hotspot\":\"same\",\"lora\":\"xprs\"}}");
    CHECK(strstr(last_aired(), " cmd:set lora:xprs") != 0, "a new mode is sent: %s", last_aired());
    /* The station takes it at once: 200, and the mode it is now running. */
    last_id(id);
    result(ST, id, "code:200 wifi:up ip:192.168.1.40 ap:off lora:xprs nick:roof zone:+01:00");

    cap_clear();
    int aired = g_aired_n;
    command("{\"command\":\"station_apply\",\"fields\":{\"nick\":\"\",\"zone\":\"+01:00\",\"hotspot\":\"same\",\"lora\":\"xprs\"}}");
    CHECK(g_aired_n == aired, "the mode it already runs is not sent");
    CHECK(cap_count("Nothing to change") >= 1, "and the person is told");

    /* The third mode is a mode like the others, not a special case. */
    cap_clear();
    command("{\"command\":\"station_apply\",\"fields\":{\"nick\":\"\",\"zone\":\"+01:00\",\"hotspot\":\"same\",\"lora\":\"meshcore\"}}");
    CHECK(strstr(last_aired(), " cmd:set lora:meshcore") != 0,
          "MeshCore is sent too: %s", last_aired());
    last_id(id);
    result(ST, id, "code:200 wifi:up ip:192.168.1.40 ap:off lora:meshcore nick:roof zone:+01:00");
    cap_clear();
    command("{\"command\":\"stats\",\"fields\":{}}");
    last_id(id);
    result(ST, id, "code:200 wifi:up ip:192.168.1.40 ap:off lora:meshcore nick:roof zone:+01:00");
    const char *st2 = cap_last("\"field\":\"st_station\"");
    CHECK(st2 && strstr(st2, "\"label\":\"LoRa\",\"value\":\"XPRS+MeshCore\""),
          "and shown by name: %s", st2 ? st2 : "");
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
     * listed under Others, with nothing but Stats and Answers offered. And
     * opening a station that the core can hear is what asks. */
    cap_clear();
    snprintf(g_station_json, sizeof g_station_json, "{\"call\":\"X3XYZ1\",\"bearer\":\"ble\",\"rssi\":-60,\"agoMs\":5000}");
    char mine[12];
    snprintf(mine, sizeof mine, "%s", g_st[g_sel].call);   /* renamed by the rekeys above */
    ask_owner("verified", "X3XYZ1", "npub1xyz1qpzry9x8gf2tvdw0s3jn54khce6mua7lqpzry9x8gf2tvdw0s3jnqp");
    char tap[200];
    snprintf(tap, sizeof tap, "{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"X3XYZ1\"}}");
    int aired = g_aired_n;
    command(tap);
    CHECK(g_aired_n == aired + 2 && strstr(g_aired[aired], " q:policy"), "opening it asks whose it is");
    g_station_json[0] = 0;
    deliver("xprs.observation", row_to("observation", "X3XYZ1", "verified", 1,
            "t:observation f:X3XYZ1 d:X1ME77 s:policy owner:X1OTHER use:listed first:none serve:relay ts:2026-09-10_12:06:00 sig:KKKK"));
    CHECK(cap_last("\"title\":\"Others\"") != 0, "listed under Others");
    CHECK(cap_count("Somebody else's") >= 1, "and the Now line says so");
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

/* ── Flashing over USB ────────────────────────────────────────────────── */
extern char g_flash_json[8192];
extern char g_flash_calls[16][160];
extern int g_flash_calln;
extern int g_flash_rc;

#define DEVS "\"devices\":[{\"id\":\"/dev/ttyACM1\",\"port\":\"/dev/ttyACM1\",\"product\":\"USB JTAG/serial debug unit\"," \
    "\"vid\":12346,\"pid\":4097,\"permitted\":true,\"nativeUsb\":true}," \
    "{\"id\":\"/dev/ttyUSB0\",\"port\":\"/dev/ttyUSB0\",\"product\":\"CP2102\",\"vid\":4292,\"pid\":60000," \
    "\"permitted\":false,\"nativeUsb\":false}]"
#define BOARDS(TDONGLE_LOCAL, TDONGLE_BYTES) "\"boards\":[{\"id\":\"tdeck\",\"name\":\"T-Deck\",\"family\":\"esp32s3\",\"flashMb\":16,\"port\":\"native-usb\"," \
    "\"version\":\"0.4.0\",\"flashable\":true,\"local\":\"\",\"localBytes\":0,\"photo\":\"https://x/tdeck.jpg\"}," \
    "{\"id\":\"tdongle-s3\",\"name\":\"T-Dongle-S3\",\"family\":\"esp32s3\",\"flashMb\":16,\"port\":\"native-usb\"," \
    "\"version\":\"0.4.0\",\"flashable\":true,\"local\":\"" TDONGLE_LOCAL "\",\"localBytes\":" TDONGLE_BYTES ",\"photo\":\"https://x/tdongle.jpg\"}," \
    "{\"id\":\"sensecap-p1-pro\",\"name\":\"P1\",\"family\":\"nrf52\",\"flashMb\":1,\"port\":\"uf2\"," \
    "\"version\":\"0.1.0\",\"flashable\":false,\"local\":\"\",\"localBytes\":0,\"photo\":\"https://x/p1.jpg\"}]"
#define PROBED "\"device\":{\"id\":\"/dev/ttyACM1\",\"chip\":\"esp32s3\",\"chipLabel\":\"ESP32-S3\",\"flashBytes\":16777216," \
    "\"runs\":\"tdongle_xprs\",\"runsVersion\":\"0.4.0\",\"suggested\":\"tdongle-s3\",\"likely\":\"tdongle-s3 tdeck\"}"
#define PROBED_TWO "\"device\":{\"id\":\"/dev/ttyACM1\",\"chip\":\"esp32s3\",\"chipLabel\":\"ESP32-S3\",\"flashBytes\":16777216," \
    "\"runs\":\"\",\"runsVersion\":\"\",\"suggested\":\"\",\"likely\":\"tdeck tdongle-s3\"}"

static const char *FLASH_IDLE =
    "{\"phase\":\"idle\",\"busy\":false,\"message\":\"\",\"error\":\"\",\"supported\":true," DEVS "," BOARDS("", "0") ","
    "\"device\":{},\"board\":\"\",\"part\":\"\",\"partIndex\":0,\"partCount\":0,\"done\":0,\"total\":0}";

static void flash_state(const char *json) { snprintf(g_flash_json, sizeof g_flash_json, "%s", json); }
static void flash_event(void) { event_push("core.flash", "{\"topic\":\"core.flash\",\"rev\":1}"); module_tick(); }

static void test_flash_tab_lists_the_cable(void)
{
    cap_clear();
    g_flash_calln = 0;
    flash_state(FLASH_IDLE);
    inbox_set("{\"command\":\"flash_refresh\",\"fields\":{}}");
    module_handle_event();
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "scan"), "Refresh asks the core to scan");
    CHECK(subscribed("core.flash"), "and listens for what it finds");
    const char *tiles = cap_last("\"field\":\"flash_hub\"");
    CHECK(tiles && strstr(tiles, "\"label\":\"Plugged in\",\"value\":\"2\",\"hint\":\"tap one\""), "two ports counted: %s", tiles ? tiles : "");
    CHECK(tiles && strstr(tiles, "\"label\":\"Firmwares\",\"value\":\"3\""), "three firmwares counted");
    CHECK(tiles && strstr(tiles, "\"label\":\"Status\",\"value\":\"ready\""), "and it stands ready");
    const char *list = cap_last("\"field\":\"flash\",");
    CHECK(list && strstr(list, "\"title\":\"Plugged in\""), "a section for the cable");
    CHECK(list && strstr(list, "\"id\":\"/dev/ttyACM1\",\"title\":\"USB JTAG/serial debug unit\",\"subtitle\":\"/dev/ttyACM1, native USB. Tap to identify\""), "a device row says what to do: %s", list ? list : "");
    CHECK(list && strstr(list, "\"asks first\""), "a device without permission says so");
    CHECK(list && !strstr(list, "T-Deck"), "the tab lists boards on the cable, not the catalogue");
}

static void test_device_is_identified_and_the_match_is_chosen(void)
{
    cap_clear();
    g_flash_calln = 0;
    flash_state(FLASH_IDLE);
    inbox_set("{\"command\":\"flash_tap\",\"fields\":{\"flash_id\":\"/dev/ttyACM1\"}}");
    module_handle_event();
    const char *open = cap_last("ui.screen.open");
    CHECK(open && strstr(open, "\"name\":\"Device\"") && strstr(open, "USB JTAG/serial debug unit"), "tapping a device opens it by name");
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "probe /dev/ttyACM1"), "opening a device identifies it: %s", g_flash_calls[0]);
    const char *now = cap_last("\"field\":\"dev_now\"");
    CHECK(now && strstr(now, "Not identified yet"), "the Next line says so: %s", now ? now : "");
    CHECK(cap_last("\"field\":\"flash_write__hidden\",\"value\":true"), "Flash hidden until there is a firmware");
    CHECK(cap_last("\"field\":\"flash_choose__hidden\",\"value\":false"), "Choose firmware offered");

    /* The core answers: an S3 running the dongle's own image. One match, so
     * it is chosen and fetched without a tap. */
    char st[4096];
    snprintf(st, sizeof st, "{\"phase\":\"idle\",\"busy\":false,\"message\":\"Looks like a T-Dongle-S3\",\"error\":\"\",\"supported\":true,"
             DEVS "," BOARDS("", "0") "," PROBED ",\"board\":\"\",\"part\":\"\",\"partIndex\":0,\"partCount\":0,\"done\":0,\"total\":0}");
    flash_state(st);
    cap_clear();
    g_flash_calln = 0;
    flash_event();
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "fetch tdongle-s3"), "the match is downloaded on its own: %s", g_flash_calln ? g_flash_calls[0] : "");
    const char *tiles = cap_last("\"field\":\"dev_hub\"");
    CHECK(tiles && strstr(tiles, "\"label\":\"Firmware\",\"value\":\"T-Dongle-S3\",\"hint\":\"v0.4.0\""), "and named on the tile: %s", tiles ? tiles : "");
    CHECK(tiles && strstr(tiles, "\"label\":\"Chip\",\"value\":\"ESP32-S3\""), "chip named");
    CHECK(tiles && strstr(tiles, "\"label\":\"Flash\",\"value\":\"16\",\"unit\":\"MB\""), "flash in MB");
    CHECK(tiles && strstr(tiles, "\"label\":\"Runs\",\"value\":\"tdongle_xprs\",\"hint\":\"0.4.0\""), "what runs there");
    CHECK(tiles && strstr(tiles, "\"label\":\"Port\",\"value\":\"ttyACM1\",\"hint\":\"USB JTAG/serial debug unit\""), "the port and the product");
    const char *l = cap_last("\"field\":\"flash\",");
    CHECK(l && strstr(l, "\"subtitle\":\"ESP32-S3, 16 MB, runs tdongle_xprs 0.4.0\""), "the tab's row now says what it is: %s", l ? l : "");

    /* Downloaded: Flash is offered and the line says so. */
    snprintf(st, sizeof st, "{\"phase\":\"idle\",\"busy\":false,\"message\":\"T-Dongle-S3 0.4.0 downloaded\",\"error\":\"\",\"supported\":true,"
             DEVS "," BOARDS("0.4.0", "1498736") "," PROBED ",\"board\":\"tdongle-s3\",\"part\":\"\",\"partIndex\":0,\"partCount\":0,\"done\":0,\"total\":0}");
    flash_state(st);
    cap_clear();
    g_flash_calln = 0;
    flash_event();
    CHECK(g_flash_calln == 0, "nothing more asked once it is here");
    now = cap_last("\"field\":\"dev_now\"");
    CHECK(now && strstr(now, "T-Dongle-S3 0.4.0 is downloaded. Tap Flash."), "the Next line: %s", now ? now : "");
    tiles = cap_last("\"field\":\"dev_hub\"");
    CHECK(tiles && strstr(tiles, "\"label\":\"Firmware\",\"value\":\"T-Dongle-S3\",\"hint\":\"v0.4.0, downloaded\""), "the tile says downloaded: %s", tiles ? tiles : "");
    CHECK(cap_last("\"field\":\"flash_write__hidden\",\"value\":false"), "Flash offered");
}

static void test_flash_writes_and_reports(void)
{
    /* Continues with the device identified and the firmware downloaded. */
    cap_clear();
    g_flash_calln = 0;
    inbox_set("{\"command\":\"flash_write\",\"fields\":{\"wipe\":true}}");
    module_handle_event();
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "write /dev/ttyACM1 tdongle-s3 1"), "Flash writes that board to that device, wiped: %s", g_flash_calls[0]);
    CHECK(cap_count("settings wiped") >= 1, "and says so in the log");

    char st[4096];
    snprintf(st, sizeof st, "{\"phase\":\"writing\",\"busy\":true,\"message\":\"Writing firmware.bin (1440 KB)...\",\"error\":\"\",\"supported\":true,"
             DEVS "," BOARDS("0.4.0", "1498736") "," PROBED ",\"board\":\"tdongle-s3\",\"part\":\"firmware.bin\",\"partIndex\":3,\"partCount\":3,\"done\":1000000,\"total\":1474736}");
    flash_state(st);
    cap_clear();
    flash_event();
    const char *tiles = cap_last("\"field\":\"dev_hub\"");
    CHECK(tiles && strstr(tiles, "\"label\":\"Firmware\",\"value\":\"T-Dongle-S3\",\"hint\":\"firmware.bin 3/3\",\"progress\":0.67"),
          "the firmware tile shows the part and its progress: %s", tiles ? tiles : "");
    CHECK(cap_last("\"field\":\"flash_write__hidden\",\"value\":true"), "Flash hidden while writing");
    CHECK(cap_last("\"field\":\"flash_choose__hidden\",\"value\":true"), "Choose hidden while writing");
    CHECK(cap_last("\"field\":\"flash_cancel__hidden\",\"value\":false"), "Stop offered");
    const char *hub = cap_last("\"field\":\"flash_hub\"");
    CHECK(hub && strstr(hub, "\"label\":\"Status\",\"value\":\"writing\",\"hint\":\"Writing firmware.bin (1440 KB)...\",\"progress\":0.67"), "the tab's status tile follows: %s", hub ? hub : "");

    snprintf(st, sizeof st, "{\"phase\":\"done\",\"busy\":false,\"message\":\"T-Dongle-S3 0.4.0 written and verified. It is restarting.\",\"error\":\"\",\"supported\":true,"
             "\"devices\":[]," BOARDS("0.4.0", "1498736") "," PROBED ",\"board\":\"tdongle-s3\",\"part\":\"firmware.bin\",\"partIndex\":3,\"partCount\":3,\"done\":1474736,\"total\":1474736}");
    flash_state(st);
    cap_clear();
    flash_event();
    const char *now = cap_last("\"field\":\"dev_now\"");
    CHECK(now && strstr(now, "written and verified"), "done is the line, before anything else: %s", now ? now : "");
    CHECK(cap_last("\"field\":\"flash_cancel__hidden\",\"value\":true"), "Stop put away");
    hub = cap_last("\"field\":\"flash_hub\"");
    CHECK(hub && strstr(hub, "written and verified"), "the tab says done: %s", hub ? hub : "");

    snprintf(st, sizeof st, "{\"phase\":\"failed\",\"busy\":false,\"message\":\"Writing firmware.bin\",\"error\":\"firmware.bin at 0x20000 did not verify\",\"supported\":true,"
             DEVS "," BOARDS("0.4.0", "1498736") "," PROBED ",\"board\":\"tdongle-s3\",\"part\":\"\",\"partIndex\":0,\"partCount\":0,\"done\":0,\"total\":0}");
    flash_state(st);
    cap_clear();
    flash_event();
    now = cap_last("\"field\":\"dev_now\"");
    CHECK(now && strstr(now, "did not verify"), "the error is the line: %s", now ? now : "");
    hub = cap_last("\"field\":\"flash_hub\"");
    CHECK(hub && strstr(hub, "\"label\":\"Status\",\"value\":\"failed\",\"hint\":\"firmware.bin at 0x20000 did not verify\"") && strstr(hub, "\"alert\":true"),
          "and the tab's Status tile carries it: %s", hub ? hub : "");

    cap_clear();
    g_flash_calln = 0;
    inbox_set("{\"command\":\"flash_cancel\",\"fields\":{}}");
    module_handle_event();
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "cancel"), "Stop cancels");
}

static void test_two_boards_fit_and_the_picture_picker(void)
{
    /* A fresh device: the probe says an S3 with nothing known on it and two
     * boards that fit. Nothing is chosen for the person. */
    cap_clear();
    g_flash_calln = 0;
    char st[4096];
    flash_state(FLASH_IDLE);
    inbox_set("{\"command\":\"flash_tap\",\"fields\":{\"flash_id\":\"/dev/ttyUSB0\"}}");
    module_handle_event();
    cap_clear();
    g_flash_calln = 0;
    inbox_set("{\"command\":\"flash_tap\",\"fields\":{\"flash_id\":\"/dev/ttyACM1\"}}");
    module_handle_event();
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "probe /dev/ttyACM1"), "a device tapped after another is identified");
    snprintf(st, sizeof st, "{\"phase\":\"idle\",\"busy\":false,\"message\":\"2 boards fit\",\"error\":\"\",\"supported\":true,"
             DEVS "," BOARDS("", "0") "," PROBED_TWO ",\"board\":\"\",\"part\":\"\",\"partIndex\":0,\"partCount\":0,\"done\":0,\"total\":0}");
    flash_state(st);
    cap_clear();
    g_flash_calln = 0;
    flash_event();
    CHECK(g_flash_calln == 0, "two fits: nothing chosen or fetched on its own");
    const char *now = cap_last("\"field\":\"dev_now\"");
    CHECK(now && strstr(now, "Several boards fit this chip. Tap Choose firmware"), "the Next line says to choose: %s", now ? now : "");
    CHECK(cap_last("\"field\":\"flash_write__hidden\",\"value\":true"), "Flash hidden with nothing chosen");

    /* The picker: pictures, the fitting boards first. */
    cap_clear();
    inbox_set("{\"command\":\"flash_choose\",\"fields\":{}}");
    module_handle_event();
    const char *open = cap_last("ui.screen.open");
    CHECK(open && strstr(open, "\"name\":\"Firmware\""), "the Firmware screen opens");
    const char *pick = cap_last("\"field\":\"flash_pick\"");
    CHECK(pick && strstr(pick, "\"title\":\"Fits this board\",\"items\":[{\"id\":\"tdeck\""), "the fitting boards lead: %s", pick ? pick : "");
    CHECK(pick && strstr(pick, "\"picture\":\"https://x/tdeck.jpg\""), "each with its picture from the website");
    CHECK(pick && strstr(pick, "\"title\":\"All boards\",\"items\":[{\"id\":\"sensecap-p1-pro\"") && strstr(pick, "\"not over USB\"") && strstr(pick, "\"dim\":true"),
          "the rest follow, a uf2 board dim and marked");

    /* Picking one chooses it, fetches it and goes back. */
    cap_clear();
    g_flash_calln = 0;
    inbox_set("{\"command\":\"flash_pick_tap\",\"fields\":{\"flash_pick_id\":\"tdeck\"}}");
    module_handle_event();
    CHECK(g_flash_calln == 1 && !strcmp(g_flash_calls[0], "fetch tdeck"), "the pick is downloaded: %s", g_flash_calln ? g_flash_calls[0] : "");
    CHECK(cap_count("ui.screen.close") >= 1, "and the picker closes");
    const char *tiles = cap_last("\"field\":\"dev_hub\"");
    CHECK(tiles && strstr(tiles, "\"label\":\"Firmware\",\"value\":\"T-Deck\""), "the Device screen names it: %s", tiles ? tiles : "");
    pick = cap_last("\"field\":\"flash_pick\"");
    CHECK(pick && strstr(pick, "\"id\":\"tdeck\"") && strstr(pick, "\"chosen\""), "the picker marks the choice");
    now = cap_last("\"field\":\"dev_now\"");
    CHECK(now && strstr(now, "Downloading"), "and the Next line waits for the download: %s", now ? now : "");
}

static void test_flash_refusals(void)
{
    cap_clear();
    g_flash_calln = 0;
    g_flash_rc = 0;
    flash_state(FLASH_IDLE);
    inbox_set("{\"command\":\"flash_identify\",\"fields\":{}}");
    module_handle_event();
    CHECK(cap_count("Busy, wait") >= 1, "a probe the core refused is said");
    g_flash_rc = 1;
    g_dev[0] = 0;
    g_board[0] = 0;
    cap_clear();
    inbox_set("{\"command\":\"flash_write\",\"fields\":{\"wipe\":false}}");
    module_handle_event();
    CHECK(cap_count("Choose the firmware first") >= 1, "no firmware, no write");
    CHECK(g_flash_calln == 1, "and the core was not asked");
    flash_state("{\"phase\":\"idle\",\"busy\":false,\"message\":\"\",\"error\":\"\",\"supported\":false,\"devices\":[],\"boards\":[],\"device\":{},\"board\":\"\"}");
    cap_clear();
    flash_event();
    const char *hub = cap_last("\"field\":\"flash_hub\"");
    CHECK(hub && strstr(hub, "\"label\":\"USB\",\"value\":\"none\",\"hint\":\"not on this device\",\"alert\":true"), "a platform without USB says so: %s", hub ? hub : "");
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
    test_lora_mode();
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
    test_flash_tab_lists_the_cable();
    test_device_is_identified_and_the_match_is_chosen();
    test_flash_writes_and_reports();
    test_two_boards_fit_and_the_picture_picker();
    test_flash_refusals();
    printf("firmwares: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
