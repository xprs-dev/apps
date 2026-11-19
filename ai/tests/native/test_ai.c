/*
 * The AI add-on's wapp editor, native, against a mock HAL: what it shows
 * when opened on a wapp, the request it builds for each kind of service, how
 * it reads the answer, and Apply / Undo on real files.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hal_mock.h"

void module_init(void);
void module_handle_event(void);

static int g_fail, g_pass;
#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);              \
        }                                                                     \
    } while (0)

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}
static char *get(const char *path) {
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}
/* Some outbox message contains every one of the given strings. */
static int sent(const char *a, const char *b) {
    for (int i = 0; i < mock_out_n; i++)
        if (strstr(mock_out[i], a) && (!b || strstr(mock_out[i], b))) return 1;
    return 0;
}
static void deliver(const char *json) {
    mock_send_to_wapp(json);
    module_handle_event();
}

static char g_dir[256];

static void make_wapp(void) {
    snprintf(g_dir, sizeof(g_dir), "%s/ai_native_wapp",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    char p[512];
    mkdir(g_dir, 0755);
    snprintf(p, sizeof(p), "%s/screens", g_dir);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/manifest.json", g_dir);
    put(p, "{\"id\":\"tools.xprs.tasks\",\"title\":\"Task Monitor\","
           "\"version\":\"1.0.3\"}\n");
    snprintf(p, sizeof(p), "%s/main.c", g_dir);
    put(p, "int x = 1;\n");
    snprintf(p, sizeof(p), "%s/screens/home.ui.json", g_dir);
    put(p, "[]\n");
}

static void opened_on_a_wapp_shows_its_title(void) {
    char m[512];
    snprintf(m, sizeof(m), "{\"type\":\"host.edit_target\",\"dir\":\"%s/\"}",
             g_dir);
    mock_out_clear();
    deliver(m);
    CHECK(sent("\"field\":\"editing\"", "\"value\":\"Task Monitor\""),
          "Editing shows the manifest's title");
}

static void claude_request_is_well_formed(void) {
    deliver("{\"command\":\"save_settings\",\"fields\":{\"ai_provider\":"
            "\"claude\",\"ai_model\":\"\",\"ai_base_url\":\"\",\"ai_key\":"
            "\"sk-test\"}}");
    mock_out_clear();
    deliver("{\"command\":\"ai_send\",\"fields\":{\"ai_input\":"
            "\"Rename the \\\"Pause\\\" button\"}}");
    CHECK(strcmp(mock_url, "https://api.anthropic.com/v1/messages") == 0,
          "Claude's endpoint");
    CHECK(strstr(mock_headers, "x-api-key: sk-test\n") != 0, "the key header");
    CHECK(strstr(mock_headers, "anthropic-version: 2026-10-21") != 0,
          "the API version");
    CHECK(strstr(mock_body, "\"model\":\"claude-opus-5-5\"") != 0,
          "the default model");
    CHECK(strstr(mock_body, "\"max_tokens\":16000") != 0, "max_tokens");
    CHECK(strstr(mock_body, "=== FILE: main.c\\nint x = 1;") != 0,
          "the files go with it");
    CHECK(strstr(mock_body, "Rename the \\\"Pause\\\" button") != 0,
          "the request, escaped");
    CHECK(sent("ui.chat.append", "\"dir\":\"out\""), "the person's bubble");
}

static void claude_answer_is_read(void) {
    mock_reply =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"},"
        "{\"type\":\"text\",\"text\":\"Renamed it.\\n=== FILE: main.c\\n"
        "int x = 2;\\n=== END\\nBuild it.\"}],\"stop_reason\":\"end_turn\"}";
    mock_status = 200;
    mock_out_clear();
    deliver("{\"type\":\"http.done\",\"id\":7}");
    CHECK(sent("ui.chat.append", "Renamed it."), "the words reach the chat");
    CHECK(!sent("ui.chat.append", "int x = 2"),
          "the file is not dumped into the chat");
    CHECK(sent("\"field\":\"changes\"", "main.c"), "the change is listed");
}

static void apply_and_undo(void) {
    char p[512];
    snprintf(p, sizeof(p), "%s/main.c", g_dir);
    deliver("{\"command\":\"apply\",\"fields\":{}}");
    CHECK(strcmp(get(p), "int x = 2;\n") == 0, "Apply writes the file");
    deliver("{\"command\":\"undo\",\"fields\":{}}");
    CHECK(strcmp(get(p), "int x = 1;\n") == 0, "Undo puts it back");
}

static void outside_files_are_refused(void) {
    mock_reply = "{\"content\":[{\"type\":\"text\",\"text\":\"=== FILE: "
                 "../../etc/passwd\\nx\\n=== END\"}],\"stop_reason\":"
                 "\"end_turn\"}";
    deliver("{\"command\":\"ai_send\",\"fields\":{\"ai_input\":\"go\"}}");
    mock_out_clear();
    deliver("{\"type\":\"http.done\",\"id\":7}");
    CHECK(sent("\"field\":\"changes\"", "No changes proposed"),
          "a path outside the wapp's files is not proposed");
}

static void service_errors_are_shown(void) {
    mock_reply = "{\"type\":\"error\",\"error\":{\"type\":"
                 "\"authentication_error\",\"message\":\"invalid x-api-key\"}}";
    mock_status = 401;
    deliver("{\"command\":\"ai_send\",\"fields\":{\"ai_input\":\"go\"}}");
    mock_out_clear();
    deliver("{\"type\":\"http.done\",\"id\":7}");
    CHECK(sent("ui.chat.append", "invalid x-api-key"),
          "the service's own message");
    mock_status = 200;
}

static void openai_compatible(void) {
    deliver("{\"command\":\"save_settings\",\"fields\":{\"ai_provider\":"
            "\"compatible\",\"ai_model\":\"qwen3\",\"ai_base_url\":"
            "\"http://192.168.1.5:11434/v1\",\"ai_key\":\"\"}}");
    deliver("{\"command\":\"ai_send\",\"fields\":{\"ai_input\":\"hi\"}}");
    CHECK(strcmp(mock_url, "http://192.168.1.5:11434/v1/chat/completions") == 0,
          "the server's own address");
    CHECK(strstr(mock_headers, "x-api-key") == 0,
          "no Anthropic header to another service");
    CHECK(strstr(mock_headers, "Authorization: Bearer sk-test") != 0,
          "the saved key as a bearer token");
    CHECK(strstr(mock_body, "{\"role\":\"system\",\"content\":") != 0,
          "the instructions as a system message");
    mock_reply = "{\"choices\":[{\"message\":{\"role\":\"assistant\","
                 "\"content\":\"Hello \\u00e9\\ud83d\\ude00\"}}]}";
    mock_out_clear();
    deliver("{\"type\":\"http.done\",\"id\":7}");
    CHECK(sent("ui.chat.append", "Hello \xc3\xa9\xf0\x9f\x98\x80"),
          "the answer, with escapes decoded");
}

static void key_can_be_forgotten(void) {
    deliver("{\"command\":\"forget_key\",\"fields\":{}}");
    deliver("{\"command\":\"save_settings\",\"fields\":{\"ai_provider\":"
            "\"openai\",\"ai_model\":\"gpt-x\",\"ai_base_url\":\"\","
            "\"ai_key\":\"\"}}");
    mock_out_clear();
    deliver("{\"command\":\"ai_send\",\"fields\":{\"ai_input\":\"hi\"}}");
    CHECK(sent("ui.chat.append", "key"), "asks for a key, sends nothing");
}

/* ── ai.chat, asked by another wapp through the broker ─────────────────── */

#define ASK(id, args)                                                         \
    "{\"type\":\"widget.request\",\"widget\":\"ai.chat\",\"req_id\":\"" id    \
    "\",\"caller\":\"notes\",\"args\":" args "}"

static void service_needs_settings(void) {
    /* key_can_be_forgotten left an OpenAI setup with no key. */
    mock_out_clear();
    deliver(ASK("b1", "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"));
    CHECK(sent("widget.response", "\"req_id\":\"b1\""), "answered at once");
    CHECK(sent("widget.response", "not_configured"), "and says why");
}

static void service_answers_another_wapp(void) {
    deliver("{\"command\":\"save_settings\",\"fields\":{\"ai_provider\":"
            "\"claude\",\"ai_model\":\"\",\"ai_base_url\":\"\",\"ai_key\":"
            "\"sk-svc\"}}");
    mock_out_clear();
    deliver(ASK("b2",
                "{\"system\":\"Be brief.\",\"max_tokens\":500,\"messages\":["
                "{\"role\":\"user\",\"content\":\"Say \\\"hi\\\"\"},"
                "{\"role\":\"tool\",\"content\":\"x\"},"
                "{\"role\":\"assistant\",\"content\":\"hi\"},"
                "{\"role\":\"user\",\"content\":\"again\"}]}"));
    CHECK(!sent("widget.response", 0), "nothing until the service replies");
    CHECK(strcmp(mock_url, "https://api.anthropic.com/v1/messages") == 0,
          "the configured service");
    CHECK(strstr(mock_headers, "x-api-key: sk-svc\n") != 0, "this wapp's key");
    CHECK(strstr(mock_body, "\"max_tokens\":500") != 0, "the caller's limit");
    CHECK(strstr(mock_body, "\"system\":\"Be brief.\"") != 0,
          "the caller's instructions");
    CHECK(strstr(mock_body,
                 "\"messages\":[{\"role\":\"user\",\"content\":\"Say "
                 "\\\"hi\\\"\"},{\"role\":\"assistant\",\"content\":\"hi\"},"
                 "{\"role\":\"user\",\"content\":\"again\"}]") != 0,
          "its conversation, other roles dropped");
    CHECK(strstr(mock_body, "You edit a wapp") == 0,
          "none of the editor's own prompt");
    CHECK(strstr(mock_body, "int x = ") == 0, "none of the edited wapp's files");

    mock_reply = "{\"content\":[{\"type\":\"text\",\"text\":\"hi \\\"again\\\"\"}],"
                 "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":31,"
                 "\"output_tokens\":4}}";
    mock_status = 200;
    mock_out_clear();
    deliver("{\"type\":\"http.done\",\"id\":7}");
    CHECK(sent("\"type\":\"widget.response\",\"req_id\":\"b2\"",
               "\"result\":{\"text\":\"hi \\\"again\\\"\",\"stop\":\"end\","
               "\"input_tokens\":31,\"output_tokens\":4}}"),
          "the reply, how it ended and what it cost");
    CHECK(!sent("ui.chat.append", 0), "and nothing in this wapp's own chat");
}

static void service_reports_errors(void) {
    deliver(ASK("b3", "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"));
    mock_reply = "{\"error\":{\"type\":\"rate_limit_error\",\"message\":"
                 "\"slow down\"}}";
    mock_status = 429;
    mock_out_clear();
    deliver("{\"type\":\"http.done\",\"id\":7}");
    CHECK(sent("\"req_id\":\"b3\"", "\"error\":\"refused: slow down\""),
          "the service's own words");
    mock_status = 200;

    mock_out_clear();
    deliver(ASK("b4", "{\"messages\":[]}"));
    CHECK(sent("\"req_id\":\"b4\"", "bad_request"), "no message, no request");
    mock_out_clear();
    deliver("{\"type\":\"widget.request\",\"widget\":\"ai.paint\",\"req_id\":"
            "\"b5\",\"args\":{}}");
    CHECK(sent("\"req_id\":\"b5\"", "unsupported"), "an unknown request");
}

int main(void) {
    make_wapp();
    module_init();
    opened_on_a_wapp_shows_its_title();
    claude_request_is_well_formed();
    claude_answer_is_read();
    apply_and_undo();
    outside_files_are_refused();
    service_errors_are_shown();
    openai_compatible();
    key_can_be_forgotten();
    service_needs_settings();
    service_answers_another_wapp();
    service_reports_errors();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
