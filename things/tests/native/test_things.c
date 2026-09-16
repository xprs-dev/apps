/*
 * The Things wapp against a mock HAL, as the core would drive it.
 *
 * What these hold the wapp to:
 *   - the core decides what is a device (the "kind" it hands over), never a
 *     prefix the wapp tests itself;
 *   - pinning is the core's follow, and a pinned device that has gone quiet
 *     keeps showing what the archive holds for it, dimmed;
 *   - readings are shown as sent, energy split by source per XPRS.md 15.5.2;
 *   - with no page it subscribes to nothing, and with one it redraws at most
 *     every two seconds and re-reads the archive only when the archive moved.
 */
#include <stdio.h>
#include <string.h>

#include "../../main.c"

/* from hal_mock.c */
extern char g_stations_json[8192];
extern int g_station_n;
extern int g_hist_n, g_hist_total;
extern int g_followed_n, g_follow_calls;
extern char g_followed[8][16];
extern uint64_t g_ms, g_epoch;
extern int g_ui_attached;
extern int g_subn;
extern int g_discover_calls, g_discover_rc;
void station_set(const char *call, const char *json);
void history_set(const char *key, const char *answer);
int history_calls(const char *key);
void cap_clear(void);
int cap_count(const char *s);
const char *cap_last(const char *s);
void inbox_set(const char *s);
void event_push(const char *t, const char *d);
int subscribed(const char *t);

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void deliver(const char *topic) { event_push(topic, "{\"rev\":1}"); module_handle_event(); }
static void command(const char *json) { inbox_set(json); module_handle_event(); }

static const char *NEARBY =
    "[{\"title\":\"Heard over the air (3)\",\"items\":["
    "{\"id\":\"X4PL3M\",\"title\":\"X4PL3M\",\"subtitle\":\"LAN - 3 packets\",\"tags\":[\"seen 5s ago\",\"LAN\"],\"kind\":\"device\"},"
    "{\"id\":\"X1RD89\",\"title\":\"X1RD89\",\"subtitle\":\"BLE\",\"tags\":[],\"kind\":\"user\"},"
    "{\"id\":\"X3RLY7\",\"title\":\"X3RLY7\",\"subtitle\":\"LAN\",\"tags\":[],\"kind\":\"station\"}]},"
    "{\"title\":\"Heard this hour (1)\",\"items\":["
    "{\"id\":\"X4DOOR\",\"title\":\"X4DOOR\",\"subtitle\":\"BLE\",\"tags\":[],\"kind\":\"device\"}]}]";

static const char *PUMP_HERE =
    "{\"call\":\"X4PL3M\",\"kind\":\"device\",\"bearer\":\"lan\",\"bearers\":[\"lan\"],"
    "\"rssi\":0,\"lastMs\":1,\"agoMs\":5000,\"lastDirectMs\":1,\"packets\":3,"
    "\"sig\":\"verified\",\"readings\":{\"state\":\"on\",\"volt\":\"12.1V\"}}";

static char g_rows[8192];
/* One archive row the way hal_xprs_history returns it. [ago_s] before now. */
static void row(char *out, unsigned cap, const char *from, int ago_s, const char *tail)
{
    char r[700];
    snprintf(r, sizeof r,
             "%s{\"id\":\"a%d\",\"ts\":%llu,\"heardTs\":%llu,\"bearer\":\"lan\",\"rssi\":0,"
             "\"from\":\"%s\",\"to\":\"\",\"type\":\"observation\",\"mine\":false,"
             "\"own\":false,\"sig\":\"unsigned\",\"heard\":1,"
             "\"wire\":\"t:observation f:%s %s\"}",
             out[1] ? "," : "", ago_s,
             (unsigned long long)(g_epoch - (uint64_t)ago_s),
             (unsigned long long)(g_epoch - (uint64_t)ago_s), from, from, tail);
    if (strlen(out) + strlen(r) + 2 < cap) strcat(out, r);
}
static void rows_begin(void) { strcpy(g_rows, "["); }
static const char *rows_end(void) { strcat(g_rows, "]"); return g_rows; }

static void reset(void)
{
    cap_clear();
    g_stations_json[0] = 0;
    strcpy(g_stations_json, "[]");
    g_station_n = 0;
    g_hist_n = 0;
    g_hist_total = 0;
    g_followed_n = 0;
    g_follow_calls = 0;
    g_subn = 0;
    g_ui_attached = 1;
    g_discover_calls = 0;
    g_discover_rc = 1;
    g_asked = -1;
    g_ms += 60000;
    /* the wapp's own state, as a fresh engine has it */
    g_nth = 0;
    g_sel[0] = 0;
    g_npinned = 0;
    g_dirty = 0;
    g_drawn = 0;
    g_arch_gen++;
    g_nknown = 0;
    while (hal_event_available()) { char t[64], d[64]; hal_event_recv(t, sizeof t, d, sizeof d); }
}

/* ── the tests ────────────────────────────────────────────────────────── */
static void test_no_page_no_subscriptions(void)
{
    reset();
    g_ui_attached = 0;
    module_init();
    CHECK(g_subn == 0, "subscribed to %d topics with no page", g_subn);
    CHECK(g_discover_calls == 0, "asked the network with nobody looking");
    CHECK(cap_count("ui.people.set") == 0, "drew with no page");
}

static void test_nearby_is_what_the_core_calls_a_device(void)
{
    reset();
    strcpy(g_stations_json, NEARBY);
    station_set("X4PL3M", PUMP_HERE);
    module_init();
    CHECK(subscribed("core.monitor") && subscribed("core.archive"), "listens to the core's state");
    const char *l = cap_last("ui.people.set");
    CHECK(l && strstr(l, "\"title\":\"Nearby\""), "a Nearby section: %s", l ? l : "(none)");
    CHECK(l && strstr(l, "X4PL3M") && strstr(l, "X4DOOR"), "both devices listed");
    CHECK(l && !strstr(l, "X1RD89") && !strstr(l, "X3RLY7"), "a person and a station are not things");
    CHECK(l && strstr(l, "on, 12.1V"), "the pump's state from the core: %s", l ? l : "");
}

static void test_pin_is_the_cores_follow(void)
{
    reset();
    strcpy(g_stations_json, NEARBY);
    station_set("X4PL3M", PUMP_HERE);
    module_init();
    command("{\"command\":\"things_tap\",\"fields\":{\"things_id\":\"X4PL3M\"}}");
    CHECK(cap_count("\"name\":\"Thing\"") == 1, "the Thing screen opened");
    CHECK(cap_last("\"field\":\"pin__hidden\",\"value\":false") != 0, "Pin offered");
    cap_clear();
    command("{\"command\":\"pin\",\"fields\":{}}");
    CHECK(g_follow_calls == 1 && g_followed_n == 1 && !strcmp(g_followed[0], "X4PL3M"),
          "hal_xprs_follow(X4PL3M, 1)");
    const char *l = cap_last("ui.people.set");
    CHECK(l && strstr(l, "\"title\":\"Pinned\",\"items\":[{\"id\":\"X4PL3M\""), "moved to Pinned: %s", l ? l : "");
    CHECK(l && !strstr(l, "\"title\":\"Nearby\",\"items\":[{\"id\":\"X4PL3M\""), "and not also Nearby");
    CHECK(cap_last("\"field\":\"unpin__hidden\",\"value\":false") != 0, "Unpin offered now");
    command("{\"command\":\"unpin\",\"fields\":{}}");
    CHECK(g_followed_n == 0, "unpinned");
}

static void test_a_silent_pinned_device_shows_the_archive(void)
{
    reset();
    g_followed_n = 1;
    strcpy(g_followed[0], "X4PL3M");
    rows_begin();
    row(g_rows, sizeof g_rows, "X4PL3M", 600, "state:off volt:23.8V ts:x");
    row(g_rows, sizeof g_rows, "X4PL3M", 3600, "state:on volt:24.1V batt:64% ts:x");
    history_set("\"from\":\"X4PL3M\",\"types\":[\"observation\"]", rows_end());
    module_init();
    const char *l = cap_last("ui.people.set");
    CHECK(l && strstr(l, "off, 23.8V, 64%"), "the newest of each key: %s", l ? l : "");
    CHECK(l && strstr(l, "\"dim\":true"), "dimmed: not in earshot");
    CHECK(l && strstr(l, "read 10 min ago"), "says how old the reading is");
}

static void test_the_name_is_its_identity(void)
{
    reset();
    g_followed_n = 1;
    strcpy(g_followed[0], "X4PL3M");
    history_set("\"from\":\"X4PL3M\",\"types\":[\"identity\"]",
                "[{\"id\":\"i1\",\"ts\":1788999000,\"from\":\"X4PL3M\",\"type\":\"identity\","
                "\"sig\":\"verified\",\"wire\":\"t:identity f:X4PL3M k:npub1pl3m nick:yard-pump\"}]");
    module_init();
    const char *l = cap_last("ui.people.set");
    CHECK(l && strstr(l, "\"title\":\"yard-pump\""), "named by its t:identity: %s", l ? l : "");
    /* A second draw does not ask again: the name is cached. */
    int asked = history_calls("\"from\":\"X4PL3M\",\"types\":[\"identity\"]");
    command("{\"command\":\"refresh\",\"fields\":{}}");
    CHECK(history_calls("\"from\":\"X4PL3M\",\"types\":[\"identity\"]") == asked, "the name is not asked twice");
}

static void test_energy_is_split_by_source(void)
{
    reset();
    g_followed_n = 1;
    strcpy(g_followed[0], "X3FARM");
    /* XPRS.md 15.5.2's farm: the total, then one packet per source. */
    rows_begin();
    row(g_rows, sizeof g_rows, "X3FARM", 60, "source:wind produces:1400W ts:x");
    row(g_rows, sizeof g_rows, "X3FARM", 60, "source:solar produces:3200W ts:x");
    row(g_rows, sizeof g_rows, "X3FARM", 60, "source:mixed produces:4600W consumes:2900W grid:-1700W ts:x");
    history_set("\"from\":\"X3FARM\",\"types\":[\"observation\"]", rows_end());
    module_init();
    command("{\"command\":\"things_tap\",\"fields\":{\"things_id\":\"X3FARM\"}}");
    const char *t = cap_last("\"field\":\"th_now\"");
    CHECK(t && strstr(t, "\"label\":\"Producing\",\"value\":\"4600\",\"unit\":\"W\""), "the total produces: %s", t ? t : "");
    CHECK(t && strstr(t, "\"label\":\"Grid\",\"value\":\"-1700\",\"unit\":\"W\""), "an export is negative");
    CHECK(t && strstr(t, "\"label\":\"From solar\",\"value\":\"3200\""), "solar's share");
    CHECK(t && strstr(t, "\"label\":\"From wind\",\"value\":\"1400\""), "wind's share");
    CHECK(t && strstr(t, "\"label\":\"Source\",\"value\":\"mixed\""), "the total says mixed");
}

static void test_redraws_are_throttled_and_the_archive_cached(void)
{
    reset();
    g_followed_n = 1;
    strcpy(g_followed[0], "X4PL3M");
    rows_begin();
    row(g_rows, sizeof g_rows, "X4PL3M", 60, "state:on ts:x");
    history_set("\"from\":\"X4PL3M\",\"types\":[\"observation\"]", rows_end());
    history_set("\"kind\":\"device\"", "[]");
    module_init();
    int draws = cap_count("ui.people.set");
    int reads = history_calls("\"from\":\"X4PL3M\",\"types\":[\"observation\"]");

    g_ms += 500;
    deliver("core.monitor");
    CHECK(cap_count("ui.people.set") == draws, "a second draw inside two seconds waits");
    g_ms += 2000;
    module_tick();
    CHECK(cap_count("ui.people.set") == draws + 1, "the tick finishes it");
    CHECK(history_calls("\"from\":\"X4PL3M\",\"types\":[\"observation\"]") == reads,
          "the monitor moving does not re-read the archive");

    g_ms += 2500;
    deliver("core.archive");
    CHECK(cap_count("ui.people.set") == draws + 2, "a quiet room draws at once");
    CHECK(history_calls("\"from\":\"X4PL3M\",\"types\":[\"observation\"]") == reads + 1,
          "the archive moving does");

    g_ms += 2500;
    module_tick();
    CHECK(cap_count("ui.people.set") == draws + 2, "nothing moved, nothing drawn");
    CHECK(history_calls("\"kind\":\"device\"") == 2,
          "the archive's devices asked once per archive change, not per draw");
}

static void test_the_archive_knows_devices_not_heard_now(void)
{
    reset();
    history_set("\"kind\":\"device\"",
                "[{\"id\":\"i2\",\"ts\":1788990000,\"from\":\"X4GEN1\",\"type\":\"identity\","
                "\"sig\":\"verified\",\"wire\":\"t:identity f:X4GEN1 k:npub1gen1 nick:cabin-generator\"}]");
    module_init();
    const char *l = cap_last("ui.people.set");
    CHECK(l && strstr(l, "\"title\":\"In the archive\""), "a section for them: %s", l ? l : "");
    CHECK(l && strstr(l, "\"title\":\"cabin-generator\""), "named from the identity row itself");
}

static void test_opening_the_page_asks_the_core_to_look(void)
{
    reset();
    module_init();
    CHECK(g_discover_calls == 1, "one ask on opening, got %d", g_discover_calls);
    const char *l = cap_last("\"field\":\"th_scan\"");
    CHECK(l && strstr(l, "Asked the local network who is there"), "says it asked: %s", l ? l : "");

    /* Scan again inside the core's half minute: the core says no, the
     * screen does not pretend otherwise. */
    g_discover_rc = 0;
    command("{\"command\":\"scan\",\"fields\":{}}");
    CHECK(g_discover_calls == 2, "Scan asks the core");
    l = cap_last("\"field\":\"th_scan\"");
    CHECK(l && strstr(l, "Asked the local network"), "a recent ask still stands: %s", l ? l : "");

    g_ms += 40000;
    command("{\"command\":\"scan\",\"fields\":{}}");
    l = cap_last("\"field\":\"th_scan\"");
    CHECK(l && strstr(l, "Not asked now"), "and says when it was not asked: %s", l ? l : "");

    /* A device answering is just a station the core now lists. */
    strcpy(g_stations_json, NEARBY);
    g_ms += 2500;
    deliver("core.monitor");
    const char *p = cap_last("ui.people.set");
    CHECK(p && strstr(p, "X4PL3M"), "the answer shows up as a Nearby device");
}

int main(void)
{
    test_no_page_no_subscriptions();
    test_nearby_is_what_the_core_calls_a_device();
    test_pin_is_the_cores_follow();
    test_a_silent_pinned_device_shows_the_archive();
    test_the_name_is_its_identity();
    test_energy_is_split_by_source();
    test_redraws_are_throttled_and_the_archive_cached();
    test_the_archive_knows_devices_not_heard_now();
    test_opening_the_page_asks_the_core_to_look();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
