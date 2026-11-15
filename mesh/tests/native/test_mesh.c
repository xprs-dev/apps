/*
 * Native integration test for the mesh wapp. Drives module_init/tick/
 * handle_event against the canned mock HAL and asserts the messages the wapp
 * emits (graph/status pushes, hub list, host-action forwarding).
 *
 * Run:  sh tests/native/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* From the wapp */
void module_init(void);
void module_tick(void);
void module_handle_event(void);

/* From hal_mock.c */
void cap_clear(void);
int  cap_contains(const char *s);
int  cap_count(void);
const char *cap_at(int i);
void inbox_set(const char *s);
void event_push(const char *topic);
const char *last_filter(void);
uint32_t hal_kv_get(const char *k, uint32_t kl, char *out, uint32_t cap);

static int g_fail = 0;
static void check(int cond, const char *name) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail = 1;
}

/* hal_mock.c */
void event_push(const char* topic);
int  event_subscribed(const char* topic);

int main(void) {
    printf("mesh native test\n");

    module_init();

    /* ── boot pushes a frame ──
     *
     * This used to clear the capture buffer and then assert on module_tick(),
     * which has been empty since the wapp stopped polling: six assertions on
     * nothing. What module_init() emitted is the frame that matters. */
    check(cap_contains("\"type\":\"ui.graph.set\""), "boot pushes graph to host");
    check(cap_contains("\"nodes\""), "graph carries nodes");
    check(cap_contains("\"edges\""), "graph carries edges");
    check(cap_contains("\"type\":\"ui.graph.hubs\""), "hubs forwarded to host");
    check(cap_contains("rns.beleth.net:4242"), "hub endpoint forwarded");
    check(cap_contains("\"connected\":true"), "hub connected flag forwarded");

    /* ── it redraws on a core event, and on nothing else ── */
    check(event_subscribed("core.rns.graph"), "subscribed to the announce topic");
    check(event_subscribed("core.monitor"),
          "subscribed to the station table — most XPRS devices are heard on "
          "the air and never announce on Reticulum");

    cap_clear();
    module_tick();
    check(cap_count() == 0,
          "a tick draws nothing: tick_interval_ms is 0 and the screen is "
          "event-driven (a poll here is the regression)");

    cap_clear();
    event_push("core.rns.graph");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""), "an announce redraws");

    cap_clear();
    event_push("core.monitor");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""),
          "a station heard on the air redraws");

    cap_clear();
    module_handle_event();
    check(cap_count() == 0, "an empty queue draws nothing");

    /* ── graph_filter from the page persists + re-fetches with the filter ── */
    cap_clear();
    inbox_set("{\"command\":\"graph_filter\",\"xprsOnly\":true,"
              "\"service\":\"files\",\"search\":\"ab\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""), "filter triggers graph re-push");
    check(strstr(last_filter(), "\"xprsOnly\":true") != NULL,
          "filter forwards xprsOnly");
    check(strstr(last_filter(), "\"service\":\"files\"") != NULL,
          "filter forwards service");
    check(strstr(last_filter(), "\"search\":\"ab\"") != NULL,
          "filter forwards search");
    char kv[16];
    /* "xonly", not "geo": save_state renamed the key (main.c) and this check
     * was left behind, so it read a key nothing writes and passed on nothing. */
    check(hal_kv_get("xonly", 5, kv, sizeof(kv) - 1) > 0 && kv[0] == '1',
          "xprsOnly persisted to kv");
    /* The role bucket rides the same filter, and is OMITTED when unset so the
     * common frame stays byte-identical to what earlier builds sent. */
    check(strstr(last_filter(), "\"role\"") == NULL,
          "no role key in the filter when no role is picked");

    cap_clear();
    inbox_set("{\"command\":\"graph_filter\",\"xprsOnly\":true,"
              "\"service\":\"\",\"search\":\"\",\"role\":\"super\"}");
    module_handle_event();
    check(strstr(last_filter(), "\"role\":\"super\"") != NULL,
          "filter forwards role");

    /* An undefined bucket is forwarded like any other string -- the host
     * whitelists it. What must NOT happen is the wapp choking on it. */
    cap_clear();
    inbox_set("{\"command\":\"graph_filter\",\"role\":\"nonsense\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""),
          "unknown role still re-pushes the graph");

    /* Buffer regression. json_str UNESCAPES, so filling g_search to its full
     * 63 characters with quotes takes 126 escaped bytes on the way in -- and
     * every one of them escapes again on the way out. service does the same.
     * That is 17 + 12+62 + 12+126 + 16 + 2 = 247 bytes, and at the old
     * filter[160] it truncated MID-STRING. str_cat and json_cat_escaped both
     * stop silently, so the wapp shipped invalid JSON, the host's decode threw
     * (wapp_engine.dart wraps it in catch), every field fell back to its
     * default and the graph came back UNFILTERED with no error anywhere. */
    {
        char cmd[600];
        unsigned c = 0;
        const char *head =
            "{\"command\":\"graph_filter\",\"xprsOnly\":true,\"service\":\"";
        for (const char *h = head; *h; h++) cmd[c++] = *h;
        for (int i = 0; i < 31; i++) { cmd[c++] = '\\'; cmd[c++] = '"'; }
        const char *mid = "\",\"search\":\"";
        for (const char *h = mid; *h; h++) cmd[c++] = *h;
        for (int i = 0; i < 63; i++) { cmd[c++] = '\\'; cmd[c++] = '"'; }
        const char *tail = "\",\"role\":\"normal\"}";
        for (const char *h = tail; *h; h++) cmd[c++] = *h;
        cmd[c] = '\0';

        cap_clear();
        inbox_set(cmd);
        module_handle_event();
        const char *f = last_filter();
        size_t fl = strlen(f);
        check(fl > 200, "worst-case filter is actually long (guard is real)");
        check(fl > 0 && f[fl - 1] == '}', "worst-case filter is still closed JSON");
        check(strstr(f, "\"role\":\"normal\"") != NULL,
              "role survives a worst-case escaped search");
    }

    /* ── hub_add forwards a host action carrying the endpoint field ── */
    cap_clear();
    inbox_set("{\"command\":\"hub_add\",\"fields\":{\"hub_endpoint\":\"newhub:4242\"}}");
    module_handle_event();
    check(cap_contains("\"type\":\"rns.hub.add\"") && cap_contains("newhub:4242"),
          "hub_add forwards rns.hub.add");

    /* ── hub_remove ── */
    cap_clear();
    inbox_set("{\"command\":\"hub_remove\",\"fields\":{\"hub_endpoint\":\"newhub:4242\"}}");
    module_handle_event();
    check(cap_contains("\"type\":\"rns.hub.remove\""), "hub_remove forwards rns.hub.remove");

    /* ── apply_settings forwards the passive toggle ── */
    cap_clear();
    inbox_set("{\"command\":\"apply_settings\",\"fields\":{\"passive\":true}}");
    module_handle_event();
    check(cap_contains("\"type\":\"rns.passive.set\"") && cap_contains("true"),
          "apply_settings forwards rns.passive.set true");

    /* ── ready re-streams a frame ── */
    cap_clear();
    inbox_set("{\"command\":\"ready\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.graph.set\""), "ready streams a graph frame");

    /* ── The stations as a list, by scope and by page ── */
    extern char g_station_req[8][256];
    extern int g_station_reqn;
    void module_tick(void);
    cap_clear();
    inbox_set("{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"more:global\"}}");
    module_handle_event();
    check(cap_contains("\"tab\":\"Local\",\"title\":\"Local (2)\""), "the list has a Local section with its count");
    check(cap_contains("\"title\":\"X3PTZM\",\"subtitle\":\"Station \\u00b7 LAN, Bluetooth \\u00b7 420 m\""),
          "a row says what it is, how it is heard and how far");
    check(cap_contains("\"12 s ago\",\"Browse\""), "a station with an API is marked to browse");
    check(cap_contains("\"tab\":\"Global\",\"title\":\"Global (300)\""), "300 global stations are counted");
    check(cap_contains("X1PAGE2") && !cap_contains("X1FAR"), "Show the next ones turns the page");
    check(cap_contains("61 to 61 of 300"), "the page says where it is");
    check(cap_contains("\"type\":\"ui.stats.set\",\"field\":\"st_stats\"") && cap_contains("\"value\":\"300\""),
          "the strip counts each scope");

    /* ── One station: how it is reached, and what it shares ── */
    cap_clear();
    inbox_set("{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"X3PTZM\"}}");
    module_handle_event();
    check(cap_contains("\"field\":\"s_ways\",\"value\":{\"call\":\"X3PTZM\"") && cap_contains("\"know\":\"heard\""),
          "the ways are handed to the screen as the core gave them");
    check(cap_contains("\"id\":\"room:X562EL:gallery\"") && cap_contains("\"title\":\"Landscapes\""),
          "its public gallery is listed");
    check(cap_contains("\"id\":\"room:X5VK3U:music\",\"icon\":\"lock\"") && cap_contains("private, you are a member"),
          "a private room this profile belongs to is listed, with a lock");
    check(cap_contains("\"type\":\"ui.screen.open\",\"name\":\"Station\",\"title\":\"X3PTZM\""), "the station screen opens");

    cap_clear();
    inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"profile\"}}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.profile.open\",\"callsign\":\"X3PTZM\""), "Profile and messages opens the host profile");

    cap_clear();
    inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"room:X562EL:gallery\"}}");
    module_handle_event();
    check(cap_contains("\"field\":\"r_folders\"") && cap_contains("\"title\":\"Coast and city\",\"subtitle\":\"2 items\""),
          "a gallery lists its events by name and count");
    check(cap_contains("\"name\":\"Room\",\"title\":\"Gallery\""), "the room screen opens");

    cap_clear();
    g_station_reqn = 0;
    inbox_set("{\"command\":\"r_folders_tap\",\"fields\":{\"r_folders_id\":\"f:ab12cd\"}}");
    module_handle_event();
    check(cap_contains("\"field\":\"f_media\",\"value\":{\"layout\":\"grid\"") &&
          cap_contains("\"token\":\"file:Uc3nRw8kFa5xPd1qGz7mYb0tJe6vHs2iLoA9XfCqK4E.jpg\",\"preview\":\"file:nYxKzGm4vT2pQ8dW5jR7cL0aFbNs9hUe3oXiC6EkM1w.jpg\",\"title\":\"the mast\""),
          "a gallery folder is a grid of its files by token");
    check(cap_contains("\"label\":\"Coast and city\",\"value\":\"1 to 2 of 2\""), "the folder says its name and count");
    check(cap_contains("\"field\":\"f_more__hidden\",\"value\":true"), "no next page when everything is shown");
    {
        int named = 0;
        for (int i = 0; i < g_station_reqn; i++)
            if (strstr(g_station_req[i], "ble") || strstr(g_station_req[i], "lan") || strstr(g_station_req[i], "rns")) named = 1;
        check(!named, "no request names a bearer");
    }

    /* ── core.station redraws the screen that is open, the room does not ── */
    cap_clear();
    event_push("core.station");
    module_handle_event();
    check(cap_contains("\"field\":\"f_media\"") && !cap_contains("ui.graph.set"),
          "core.station redraws the open folder and not the graph");
    cap_clear();
    event_push("core.monitor");
    module_handle_event();
    check(!cap_contains("\"field\":\"stations\""), "a heard packet does not redraw the list at once");
    module_tick();
    check(cap_contains("\"field\":\"stations\""), "the list redraws on the tick after something moved");
    cap_clear();
    module_tick();
    check(cap_count() == 0, "and not again while nothing moves");

    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
    return g_fail;
}
