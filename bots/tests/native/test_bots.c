/*
 * Native test of the Bots wapp: the real main.c against a stand-in host
 * (docs/architecture.md 6). The host here keeps a list of bots, a key-value
 * store, an event queue and a message queue, and records what the wapp says.
 *
 *   sh tests/native/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int32_t module_init(void);
int32_t module_handle_event(void);

/* ── the stand-in host ────────────────────────────────────────────────── */
static char g_list[2048] = "[]";
static char g_last_req[1024];
static char g_said[32][1024];
static int g_said_n;
static char g_ui[64][8192];
static int g_ui_n;
static struct { char k[40]; char v[2048]; } g_kv[32];
static int g_kv_n;
static struct { char t[48]; char d[4096]; } g_evq[8];
static int g_ev_n;
static char g_msgq[8][4096];
static int g_msg_n;

int32_t hal_ui_attached(void) { return 1; }

/* The owner's People list and who was heard, as the host hands them. */
int32_t hal_people_find(const char *tag, uint32_t tl, const char *q, uint32_t ql, char *out, uint32_t cap)
{
    (void)tag; (void)tl;
    const char *all = "[{\"id\":\"X1TAXA\",\"ids\":[\"MT1A2B3C4D\",\"X1RUI9\"],\"nick\":\"Rui\","
                      "\"place\":{\"lat\":45.6,\"lon\":0.1,\"r\":5}},"
                      "{\"id\":\"X1ZZ00\",\"ids\":[\"MTFFFF0000\"],\"nick\":\"Only on Meshtastic\"}]";
    const char *rui = "[{\"id\":\"X1TAXA\",\"ids\":[\"MT1A2B3C4D\",\"X1RUI9\"],\"nick\":\"Rui\"}]";
    const char *s = ql ? (q[0] == 'r' || q[0] == 'R' ? rui : "[]") : all;
    uint32_t l = (uint32_t)strlen(s);
    if (l > cap) return -2;
    memcpy(out, s, l);
    return (int32_t)l;
}

int32_t hal_xprs_stations(char *out, uint32_t cap)
{
    const char *s = "[{\"title\":\"Heard over the air (3)\",\"items\":["
                    "{\"id\":\"X1EFGH\",\"title\":\"Ana\",\"subtitle\":\"seen 5s ago\",\"tags\":[\"LAN\"],\"kind\":\"user\"},"
                    "{\"id\":\"X3RLY7\",\"title\":\"X3RLY7\",\"kind\":\"station\"},"
                    "{\"id\":\"X1RUI9\",\"title\":\"X1RUI9\",\"kind\":\"user\"}]}]";
    uint32_t l = (uint32_t)strlen(s);
    if (l > cap) return -2;
    memcpy(out, s, l);
    return (int32_t)l;
}

int32_t hal_bots(const char *req, uint32_t n, char *out, uint32_t cap)
{
    if (strstr(req, "\"list\"")) {
        uint32_t l = (uint32_t)strlen(g_list);
        if (l > cap) return -(int32_t)l;
        memcpy(out, g_list, l);
        return (int32_t)l;
    }
    snprintf(g_last_req, sizeof g_last_req, "%.*s", (int)n, req);
    return 0;
}

int32_t hal_bot_say(const char *w, uint32_t n)
{
    if (g_said_n < 32) snprintf(g_said[g_said_n++], sizeof g_said[0], "%.*s", (int)n, w);
    return 0;
}

uint32_t hal_kv_get(const char *k, uint32_t kl, char *v, uint32_t cap)
{
    for (int i = 0; i < g_kv_n; i++)
        if (strlen(g_kv[i].k) == kl && !memcmp(g_kv[i].k, k, kl)) {
            uint32_t l = (uint32_t)strlen(g_kv[i].v);
            if (l > cap) l = cap;
            memcpy(v, g_kv[i].v, l);
            return l;
        }
    return 0;
}

int32_t hal_kv_set(const char *k, uint32_t kl, const char *v, uint32_t vl)
{
    int i;
    for (i = 0; i < g_kv_n; i++)
        if (strlen(g_kv[i].k) == kl && !memcmp(g_kv[i].k, k, kl)) break;
    if (i == g_kv_n) { g_kv_n++; snprintf(g_kv[i].k, sizeof g_kv[i].k, "%.*s", (int)kl, k); }
    snprintf(g_kv[i].v, sizeof g_kv[i].v, "%.*s", (int)vl, v);
    return 0;
}

void hal_msg_send(const char *j, uint32_t n)
{
    if (g_ui_n < 64) snprintf(g_ui[g_ui_n++], sizeof g_ui[0], "%.*s", (int)n, j);
}

uint32_t hal_msg_recv(char *b, uint32_t cap)
{
    if (!g_msg_n) return 0;
    uint32_t l = (uint32_t)strlen(g_msgq[0]);
    if (l > cap) l = cap;
    memcpy(b, g_msgq[0], l);
    memmove(g_msgq, g_msgq + 1, sizeof g_msgq[0] * 7);
    g_msg_n--;
    return l;
}

int32_t hal_event_subscribe(const char *t, uint32_t n) { (void)t; (void)n; return 0; }
uint32_t hal_event_available(void) { return g_ev_n ? 1 : 0; }

uint32_t hal_event_recv(char *t, uint32_t tc, char *d, uint32_t dc)
{
    if (!g_ev_n) return 0;
    snprintf(t, tc, "%s", g_evq[0].t);
    uint32_t l = (uint32_t)strlen(g_evq[0].d);
    if (l > dc) l = dc;
    memcpy(d, g_evq[0].d, l);
    memmove(g_evq, g_evq + 1, sizeof g_evq[0] * 7);
    g_ev_n--;
    return l;
}

/* ── driving it ───────────────────────────────────────────────────────── */
static int g_fail;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); g_fail++; } else printf("ok:   %s\n", what); } while (0)

static void event(const char *topic, const char *data)
{
    snprintf(g_evq[g_ev_n].t, sizeof g_evq[0].t, "%s", topic);
    snprintf(g_evq[g_ev_n].d, sizeof g_evq[0].d, "%s", data);
    g_ev_n++;
    module_handle_event();
}

static void message(const char *json)
{
    snprintf(g_msgq[g_msg_n++], sizeof g_msgq[0], "%s", json);
    module_handle_event();
}

static int ui_has(const char *needle)
{
    for (int i = 0; i < g_ui_n; i++) if (strstr(g_ui[i], needle)) return 1;
    return 0;
}

static const char *last_said(void) { return g_said_n ? g_said[g_said_n - 1] : ""; }

static void packet(const char *type, const char *id, const char *from,
                   const char *fields, const char *text)
{
    char row[2048];
    snprintf(row, sizeof row,
             "{\"bot\":\"X2WX7K\",\"id\":\"%s\",\"type\":\"%s\",\"from\":\"%s\","
             "\"ts\":\"2026-11-28_12:00:00\",\"sig\":\"verified\",\"sealed\":false,"
             "\"bearer\":\"ble\"%s%s%s,\"fields\":[[\"t\",\"%s\"],[\"f\",\"%s\"]%s]}",
             id, type, from, text ? ",\"text\":\"" : "", text ? text : "",
             text ? "\"" : "", type, from, fields);
    event("xprs.bot", row);
}

int main(void)
{
    module_init();
    CHECK(ui_has("\"field\":\"bots\",\"sections\":[]"), "no bots: an empty list is drawn");

    /* New bot: the wapp asks the core, and opens the one that appears. */
    message("{\"command\":\"new_bot\"}");
    CHECK(strstr(g_last_req, "\"op\":\"create\""), "New bot asks the core to create one");
    strcpy(g_list, "[{\"call\":\"X2WX7K\",\"nick\":\"bot\",\"about\":\"\",\"on\":false,"
                   "\"allow\":\"owner\",\"perHour\":30}]");
    event("core.bots", "{\"bots\":1}");
    CHECK(ui_has("\"id\":\"X2WX7K\"") && ui_has("\"tags\":[\"stopped\"]"), "the list shows it, stopped");
    CHECK(ui_has("\"ui.screen.open\",\"name\":\"Bot\""), "and its page opens");

    /* Teach it and save. */
    message("{\"command\":\"b_save\",\"fields\":{\"b_nick\":\"Weather\",\"b_about\":\"Says \\\"rain\\\"\","
            "\"b_rules\":\"forecast = Rain from 16:00\\nOpen=The office opens at nine\","
            "\"b_hello\":\"I only know commands. Write help.\",\"b_ai\":false,\"b_prompt\":\"\","
            "\"b_allow\":\"list\",\"b_talk\":\"owner\",\"b_reach\":\"local\",\"b_list\":\"x1abcd; X1EFGH\"}}");
    CHECK(strstr(g_last_req, "\"nick\":\"Weather\""), "Save sends the name to the core");
    CHECK(strstr(g_last_req, "\"about\":\"Says \\\"rain\\\"\""), "a quote in a field survives");
    CHECK(strstr(g_last_req, "\"allow\":\"X1ABCD,X1EFGH\""), "the allow list is tidied to callsigns");
    CHECK(strstr(g_last_req, "\"talk\":\"owner\""), "who it answers goes to the core");
    CHECK(strstr(g_last_req, "\"reach\":\"local\""), "and so does how far it reaches");
    CHECK(ui_has("\"ui.snack\",\"text\":\"Saved\""), "and says Saved");

    /* Stopped: it answers nothing. */
    packet("command", "aaaaaa", "X1ABCD", ",[\"cmd\",\"forecast\"]", 0);
    CHECK(g_said_n == 0, "a stopped bot says nothing");

    message("{\"command\":\"b_start\"}");
    CHECK(strstr(g_last_req, "\"on\":true"), "Start switches it on in the core");
    strcpy(g_list, "[{\"call\":\"X2WX7K\",\"nick\":\"Weather\",\"about\":\"\",\"on\":true,"
                   "\"allow\":\"X1ABCD,X1EFGH\",\"perHour\":30}]");
    event("core.bots", "{\"bots\":1}");
    CHECK(ui_has("\"tags\":[\"running\"]"), "the list shows it running");

    /* Commands. */
    packet("command", "747ae8", "X1ABCD", ",[\"cmd\",\"forecast\"],[\"arg\",\"lisbon\"]", 0);
    CHECK(!strcmp(last_said(), "t:result f:X2WX7K d:X1ABCD r:747ae8 code:200 m:Rain from 16:00"),
          "a taught word is answered 200 with its reply");
    packet("command", "111111", "X1ABCD", ",[\"cmd\",\"OPEN\"]", 0);
    CHECK(strstr(last_said(), "code:200 m:The office opens at nine"), "words match whatever their case");
    packet("command", "222222", "X1ABCD", ",[\"cmd\",\"door-open\"]", 0);
    CHECK(strstr(last_said(), "r:222222 code:404"), "an unknown word is 404");
    packet("command", "333333", "X1ABCD", ",[\"cmd\",\"help\"]", 0);
    CHECK(strstr(last_said(), "code:200 m:I know: forecast, Open"), "help lists the words");
    packet("command", "444444", "X1ABCD", ",[\"cmd\",\"interpret\"],[\"m\",\"open the door\"]", 0);
    CHECK(strstr(last_said(), "r:444444 code:404"), "interpret is 404 while the AI is off");

    /* Messages. */
    packet("message", "555555", "X1EFGH", "", "/forecast please");
    CHECK(!strcmp(last_said(), "t:message f:X2WX7K d:X1EFGH r:555555 m:Rain from 16:00"),
          "a message starting with a word gets the reply as a message");
    packet("message", "666666", "X1EFGH", "", "hello there");
    CHECK(strstr(last_said(), "r:666666 m:I only know commands. Write help."), "any other message gets the fixed line");
    packet("message", "676767", "X1EFGH", "", "/");
    CHECK(strstr(last_said(), "r:676767 m:I know: /forecast /open /help"),
          "a bare slash is answered with the menu, as slash words");
    packet("message", "686868", "X1EFGH", "", "/start");
    CHECK(strstr(last_said(), "r:686868 m:I know: /forecast /open /help"), "and so is /start");
    int before = g_said_n;
    packet("message", "777777", "X2OTHR", "", "forecast");
    CHECK(g_said_n == before, "another bot's message is never answered");

    /* The AI: asked, and its answer sent when it comes. */
    g_said_n = 0;
    g_ui_n = 0;
    message("{\"command\":\"b_save\",\"fields\":{\"b_nick\":\"Weather\",\"b_about\":\"\","
            "\"b_rules\":\"forecast = Rain from 16:00\",\"b_hello\":\"\",\"b_ai\":true,"
            "\"b_prompt\":\"You know the weather in Lisbon.\",\"b_allow\":\"owner\",\"b_list\":\"\"}}");
    g_ui_n = 0;
    packet("message", "888888", "X1EFGH", "", "will it rain tomorrow?");
    CHECK(g_said_n == 0, "with the AI on, nothing is said until it answers");
    CHECK(ui_has("\"widget\":\"ai.chat\"") && ui_has("will it rain tomorrow?") &&
          ui_has("You know the weather in Lisbon."), "the AI is asked, with what the owner told it");
    {
        /* The request id the wapp chose, handed back as the broker does. */
        char req[32] = "", resp[512];
        for (int i = 0; i < g_ui_n; i++) {
            const char *p = strstr(g_ui[i], "\"req_id\":\"");
            if (p) sscanf(p + 10, "%31[^\"]", req);
        }
        snprintf(resp, sizeof resp,
                 "{\"type\":\"widget.response\",\"req_id\":\"%s\",\"result\":{\"text\":"
                 "\"Yes,\\nfrom noon.\",\"stop\":\"end\"}}", req);
        message(resp);
    }
    CHECK(!strcmp(last_said(), "t:message f:X2WX7K d:X1EFGH r:888888 m:Yes, from noon."),
          "the AI's answer leaves as one line, replying to the message");
    packet("command", "999999", "X1ABCD", ",[\"cmd\",\"interpret\"],[\"m\",\"is it cold\"]", 0);
    CHECK(strstr(last_said(), "r:999999 code:202"), "interpret is accepted at once (202) with the AI on");

    /* Choosing people instead of typing callsigns. */
    g_ui_n = 0;
    message("{\"command\":\"pick_open\",\"fields\":{\"b_list\":\"x1abcd\",\"b_talk\":\"list\",\"b_allow\":\"owner\"}}");
    CHECK(ui_has("\"ui.screen.open\",\"name\":\"People\""), "the picker opens");
    CHECK(ui_has("\"title\":\"People I know\"") && ui_has("\"title\":\"Rui\",\"subtitle\":\"X1RUI9\""),
          "a person from the People list is offered under the callsign they were met with");
    CHECK(!ui_has("MT1A2B3C4D\"}") && !ui_has("Only on Meshtastic"), "a node of another network is not offered");
    CHECK(ui_has("\"title\":\"Heard lately\"") && ui_has("\"title\":\"Ana\",\"subtitle\":\"X1EFGH\""),
          "people heard lately are offered too");
    CHECK(!ui_has("X3RLY7"), "a station is not a person to choose");
    g_ui_n = 0;
    message("{\"command\":\"pick_tap\",\"fields\":{\"pick_id\":\"X1RUI9\"}}");
    CHECK(ui_has("\"field\":\"b_list\",\"value\":\"X1ABCD,X1RUI9\""), "a tap adds them to the list");
    CHECK(ui_has("\"subtitle\":\"X1RUI9\",\"tags\":[\"chosen\"]"), "and marks them chosen");
    message("{\"command\":\"pick_tap\",\"fields\":{\"pick_id\":\"X1ABCD\"}}");
    g_ui_n = 0;
    message("{\"command\":\"pick_search\",\"fields\":{\"pick_query\":\"rui\"}}");
    CHECK(ui_has("X1RUI9") && !ui_has("X1EFGH"), "the search narrows both lists");
    g_ui_n = 0;
    message("{\"command\":\"pick_done\"}");
    CHECK(ui_has("\"ui.screen.open\",\"name\":\"Bot\"") && ui_has("\"field\":\"b_list\",\"value\":\"X1RUI9\""),
          "Done returns to the bot's page with the chosen people in it");
    CHECK(ui_has("\"field\":\"b_talk\",\"value\":\"list\""), "and the choice made before picking is still made");
    message("{\"command\":\"b_save\",\"fields\":{\"b_nick\":\"Weather\",\"b_about\":\"\","
            "\"b_rules\":\"forecast = Rain from 16:00\",\"b_hello\":\"\",\"b_ai\":true,"
            "\"b_prompt\":\"\",\"b_allow\":\"owner\",\"b_talk\":\"list\",\"b_reach\":\"global\",\"b_list\":\"X1RUI9\"}}");
    CHECK(strstr(g_last_req, "\"talk\":\"X1RUI9\""), "and the bot is saved answering only them (and me)");

    /* Delete. */
    message("{\"command\":\"b_delete\"}");
    CHECK(strstr(g_last_req, "\"op\":\"remove\",\"call\":\"X2WX7K\""), "Delete removes it in the core");

    printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
