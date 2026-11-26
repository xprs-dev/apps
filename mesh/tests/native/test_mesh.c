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

    check(cap_contains("\"type\":\"ui.map.markers.clear\"") && cap_contains("\"type\":\"ui.map.status\""),
          "the stations map is drawn at start: a JSON page never says ready");

    /* ── it redraws on a core event, and on nothing else ── */
    check(event_subscribed("core.rns.graph"), "subscribed to the announce topic");
    check(event_subscribed("core.monitor"),
          "subscribed to the station table — most XPRS devices are heard on "
          "the air and never announce on Reticulum");

    cap_clear();
    module_tick();
    check(!cap_contains("\"type\":\"ui.graph.set\""),
          "a tick with nothing new does not redraw the graph (a poll here is the regression)");

    cap_clear();
    event_push("core.rns.graph");
    module_handle_event();
    check(!cap_contains("\"type\":\"ui.graph.set\""),
          "an announce does not redraw the graph at once (two a second on a busy hub)");
    module_tick();
    check(cap_contains("\"type\":\"ui.graph.set\""), "an announce redraws on the next tick");

    cap_clear();
    event_push("core.monitor");
    module_handle_event();
    module_tick();
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

    /* ── The stations as a list: every one known, freshest first ── */
    extern char g_station_req[8][256];
    extern int g_station_reqn;
    void module_tick(void);
    cap_clear();
    inbox_set("{\"command\":\"ready\"}");
    module_handle_event();
    check(cap_contains("\"field\":\"stations\",\"sections\":[{\"title\":\"3 stations\""),
          "the list says how many stations are known");
    check(cap_contains("\"title\":\"X3PTZM\",\"subtitle\":\"Station \xc2\xb7 LAN, Bluetooth \xc2\xb7 420 m\""),
          "a station heard here says what it is, how it is heard and how far");
    check(cap_contains("\"just now\",\"Browse\""), "a station with an API is marked to browse");
    check(cap_contains("\"type\":\"ui.map.markers.clear\"") && cap_contains("\"type\":\"ui.map.status\""),
          "the map is drawn when the page opens");
    check(cap_contains("\"type\":\"ui.map.viewport\",\"lat\":38.7077,\"lon\":-9.1365"),
          "and starts where this phone says it is");

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

    /* ── Back goes up one level ── */
    cap_clear();
    inbox_set("{\"command\":\"screen_closed\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.screen.open\",\"name\":\"Room\""), "back from a folder shows its room");
    cap_clear();
    inbox_set("{\"command\":\"screen_closed\"}");
    module_handle_event();
    check(cap_contains("\"type\":\"ui.screen.open\",\"name\":\"Station\""), "back from a room shows its station");
    cap_clear();
    inbox_set("{\"command\":\"screen_closed\"}");
    module_handle_event();
    check(!cap_contains("ui.screen.open"), "back from the station is the list");
    inbox_set("{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"X3PTZM\"}}");
    module_handle_event();
    inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"room:X562EL:gallery\"}}");
    module_handle_event();
    inbox_set("{\"command\":\"r_folders_tap\",\"fields\":{\"r_folders_id\":\"f:ab12cd\"}}");
    module_handle_event();

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

    /* ── this phone is a station too ─────────────────────────────────── */
    {
        extern int g_host_serving;
        extern char g_set_pref[];
        cap_clear();
        inbox_set("{\"command\":\"screen_closed\"}"); module_handle_event();
        inbox_set("{\"command\":\"screen_closed\"}"); module_handle_event();
        inbox_set("{\"command\":\"screen_closed\"}"); module_handle_event();
        cap_clear();
        event_push("core.monitor"); module_handle_event(); module_tick();
        check(cap_contains("\"id\":\"X1MINE\"") && cap_contains("\"title\":\"This phone\"") &&
              cap_contains("\"Private\""), "the list starts with this phone, private until its owner says");
        cap_clear();
        inbox_set("{\"command\":\"stations_tap\",\"fields\":{\"stations_id\":\"X1MINE\"}}");
        module_handle_event();
        check(cap_contains("host:toggle") && cap_contains("host:gallery") && cap_contains("host:music") &&
              cap_contains("host:library") && !cap_contains("\"id\":\"profile\""),
              "its page has the switch and the three things to share, not a profile row");
        check(cap_contains("\"here\":true"), "and no route to itself");
        inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"host:toggle\"}}");
        module_handle_event();
        check(strcmp(g_set_pref, "hostStation=1") == 0 && g_host_serving, "the switch asks the core to answer visitors");
        cap_clear();
        event_push("core.station"); module_handle_event();
        check(cap_contains("Others can browse this phone"), "and the page says so when the core did");
        cap_clear();
        inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"host:music\"}}");
        module_handle_event();
        check(cap_contains("\"type\":\"station.share\",\"kind\":\"music\""), "sharing music asks the host to pick and file it");
        /* A room of its own: who sees it, the people added, stop sharing. */
        cap_clear();
        inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"room:X5OWNR:gallery\"}}");
        module_handle_event();
        check(cap_contains("own:vis") && cap_contains("Only people you add see this") &&
              cap_contains("\"id\":\"m:X1PAL\"") && cap_contains("own:add") && cap_contains("own:close"),
              "the owner's room shows its audience, the people added and how to stop");
        cap_clear();
        inbox_set("{\"command\":\"r_folders_tap\",\"fields\":{\"r_folders_id\":\"own:vis\"}}");
        module_handle_event();
        check(cap_contains("\"type\":\"station.manage\",\"room\":\"X5OWNR\",\"action\":\"public\""),
              "a tap on the audience opens a private room to everybody");
        cap_clear();
        inbox_set("{\"command\":\"r_folders_tap\",\"fields\":{\"r_folders_id\":\"m:X1PAL\"}}");
        module_handle_event();
        check(cap_contains("ui.prompt") && !cap_contains("station.manage"), "removing a person is asked first");
        cap_clear();
        inbox_set("{\"command\":\"prompt\",\"fields\":{\"prompt_id\":\"m:X1PAL\",\"prompt_input\":\"\"}}");
        module_handle_event();
        check(cap_contains("\"action\":\"remove\",\"member\":\"X1PAL\""), "and done on yes");
        cap_clear();
        inbox_set("{\"command\":\"prompt\",\"fields\":{\"prompt_id\":\"own:add\",\"prompt_input\":\"X1NEW\"}}");
        module_handle_event();
        check(cap_contains("\"action\":\"add\",\"member\":\"X1NEW\""), "a person is added by callsign");
    }

    /* ── The map of every station, and the drawer's search and chips ── */
    {
        extern unsigned long long g_now;
        extern char g_archive[16384], g_names[2048], g_names_q[256], g_followed_kind[64];
        void al_stamp(char *out, unsigned cap, unsigned long long epoch);
        char st[24], w1[300], w2[300], w3[300], w4[300], w5[300];
        const char *bar = "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88";
        al_stamp(st, sizeof st, g_now - 3600);
        snprintf(w1, sizeof w1, "t:identity f:X3ARCH ts:%s nick:Hill pos:38.7200,-9.1400 k:npub1arch sig:x", st);
        snprintf(w2, sizeof w2, "t:service f:X3ARCH ts:%s serve:archive,relay source:solar sig:x", st);
        snprintf(w3, sizeof w3, "t:identity f:X3SQR1 ts:%s pos:38%s,-9%s sig:x", st, bar, bar);
        snprintf(w4, sizeof w4, "t:identity f:X3SQR2-1 ts:%s nick:Twin pos:38%s,-9%s sig:x", st, bar, bar);
        snprintf(w5, sizeof w5, "t:identity f:X1KEYD ts:%s nick:Ana k:npub1keyd sig:x", st);
        snprintf(g_archive, sizeof g_archive,
                 "[{\"id\":\"a1\",\"type\":\"identity\",\"sig\":\"verified\",\"wire\":\"%s\"},"
                 "{\"id\":\"a2\",\"type\":\"service\",\"sig\":\"verified\",\"wire\":\"%s\"},"
                 "{\"id\":\"a3\",\"type\":\"identity\",\"sig\":\"verified\",\"wire\":\"%s\"},"
                 "{\"id\":\"a4\",\"type\":\"identity\",\"sig\":\"verified\",\"wire\":\"%s\"},"
                 "{\"id\":\"a5\",\"type\":\"identity\",\"sig\":\"verified\",\"wire\":\"%s\"}]",
                 w1, w2, w3, w4, w5);
        check(strstr(g_followed_kind, "identity") && strstr(g_followed_kind, "service"),
              "identities and service announcements are followed, from anyone");

        /* Two minutes on, the copy is brought up to date behind the page. */
        g_now += 300;
        cap_clear();
        module_tick();
        module_tick();
        module_tick();
        check(cap_contains("\"id\":\"pl:X3ARCH\"") && cap_contains("\"lat\":38.7200,\"lon\":-9.1400") &&
              cap_contains("{\"id\":\"message\",\"label\":\"Message\"}"),
              "a station at its point, offering Open and Message (it published a key)");
        check(cap_contains("\"id\":\"sq:38.5000,-9.5000\"") && cap_contains("\"label\":\"2 stations\"") &&
              cap_contains("Show all 2 here") && cap_contains("X3SQR2 Twin"),
              "two in one square: one marker naming them, Show all 2 here");
        check(cap_contains("Hill \\u00b7 Archivers \\u00b7 Relays \\u00b7 Solar") ||
              cap_contains("Hill \xc2\xb7 Archivers \xc2\xb7 Relays \xc2\xb7 Solar"),
              "a row says what the station serves and runs on");
        check(cap_contains("\"title\":\"X1KEYD\"") && cap_contains("\"action\":\"st_msg\",\"actionLabel\":\"Message\""),
              "a station never placed is listed, with Message when it can be messaged");
        check(cap_contains("\"id\":\"serve:archive\",\"label\":\"Archivers\",\"count\":1") &&
              cap_contains("\"id\":\"reach:local\",\"label\":\"Nearby\",\"count\":2") &&
              cap_contains("\"id\":\"source:solar\",\"label\":\"Solar\",\"count\":1"),
              "the chips: how stations are heard, what they serve and run on, with how many");

        cap_clear();
        inbox_set("{\"command\":\"st_tags_tap\",\"fields\":{\"st_tags_id\":\"serve:archive\"}}");
        module_handle_event();
        check(cap_contains("\"title\":\"1 of 7 stations\"") && cap_contains("\"title\":\"X3ARCH\"") &&
              !cap_contains("\"title\":\"X3SQR1\"") && cap_contains("\"id\":\"serve:archive\",\"label\":\"Archivers\",\"count\":1,\"on\":true"),
              "Archivers: only the archivers, the chip on");
        cap_clear();
        inbox_set("{\"command\":\"st_tags_tap\",\"fields\":{\"st_tags_id\":\"serve:archive\"}}");
        module_handle_event();
        check(cap_contains("\"title\":\"7 stations\""), "tapped again, every station");

        cap_clear();
        inbox_set("{\"command\":\"stations_search\",\"fields\":{\"stations_query\":\"sqr\"}}");
        module_handle_event();
        check(cap_contains("\"title\":\"2 of 7 stations\"") && cap_contains("\"title\":\"X3SQR1\"") &&
              cap_contains("\"title\":\"X3SQR2\"") && !cap_contains("\"title\":\"X3ARCH\""),
              "a search finds by part of a callsign, any case");
        cap_clear();
        inbox_set("{\"command\":\"stations_search\",\"fields\":{\"stations_query\":\"twin\"}}");
        module_handle_event();
        check(cap_contains("\"title\":\"X3SQR2\"") && !cap_contains("\"title\":\"X3SQR1\""), "and by name");

        snprintf(g_names, sizeof g_names, "[{\"call\":\"X3ZZZZ\",\"nick\":\"Zed\",\"kind\":\"station\",\"ts\":%llu}]", g_now - 100);
        cap_clear();
        inbox_set("{\"command\":\"stations_search\",\"fields\":{\"stations_query\":\"zed\"}}");
        module_handle_event();
        check(strstr(g_names_q, "\"q\":\"zed\"") && strstr(g_names_q, "\"ask\":true") && cap_contains("\"title\":\"X3ZZZZ\""),
              "what is known nowhere here is asked of the archivers by name");
        cap_clear();
        inbox_set("{\"command\":\"stations_search\",\"fields\":{\"stations_query\":\"\"}}");
        module_handle_event();

        cap_clear();
        inbox_set("{\"command\":\"marker_action\",\"id\":\"sq:38.5000,-9.5000\",\"action\":\"area\"}");
        module_handle_event();
        check(cap_contains("\"title\":\"2 of 8 stations\"") && cap_contains("\"id\":\"area\"") &&
              cap_contains("\"type\":\"ui.drawer\",\"state\":\"half\""),
              "Show all here: the drawer opens on the square's stations");
        cap_clear();
        inbox_set("{\"command\":\"st_tags_tap\",\"fields\":{\"st_tags_id\":\"area\"}}");
        module_handle_event();
        check(cap_contains("\"title\":\"8 stations\"") && !cap_contains("\"id\":\"area\""), "its chip lets go of the square");

        cap_clear();
        inbox_set("{\"command\":\"marker_action\",\"id\":\"pl:X3ARCH\",\"action\":\"message\"}");
        module_handle_event();
        check(cap_contains("\"type\":\"mesh.message\",\"callsign\":\"X3ARCH\""), "Message opens Chat with it");
        cap_clear();
        inbox_set("{\"command\":\"marker_action\",\"id\":\"pl:X3ARCH\",\"action\":\"open\"}");
        module_handle_event();
        check(cap_contains("\"name\":\"Station\",\"title\":\"X3ARCH\"") && cap_contains("\"id\":\"msg\"") &&
              cap_contains("\"title\":\"Show on map\""),
              "Open: its panel, with Message and Show on map");
        cap_clear();
        inbox_set("{\"command\":\"s_offers_tap\",\"fields\":{\"s_offers_id\":\"map\"}}");
        module_handle_event();
        check(cap_contains("\"type\":\"ui.map.locate\",\"lat\":38.7200,\"lon\":-9.1400,\"zoom\":14"),
              "Show on map goes to it");
        cap_clear();
        inbox_set("{\"command\":\"st_msg\",\"fields\":{\"stations_id\":\"X1KEYD\"}}");
        module_handle_event();
        check(cap_contains("\"type\":\"mesh.message\",\"callsign\":\"X1KEYD\""), "a row's Message button too");
    }

    printf(g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
    return g_fail;
}
