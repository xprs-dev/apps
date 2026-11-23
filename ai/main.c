/*
 * tools.xprs.ai — the AI add-on.
 *
 * An ordinary wapp, installed and shared like any other and never part of
 * the app: an AI that edits a wapp. The editor opens the add-on "on" the
 * wapp being edited; the host says which with
 * {"type":"host.edit_target","dir":"<absolute package dir>"}. The AI reads
 * the wapp's files, answers in the chat, and proposes whole-file
 * replacements. Nothing is written until the person taps Apply, and Undo
 * puts the previous files back. Building happens in the editor, with the
 * compiler the app bundles.
 *
 * Services: Claude (Anthropic Messages API), Codex / OpenAI, OpenCode Zen and
 * any OpenAI-compatible server (Ollama, LM Studio, OpenRouter, ...). The key
 * stays in this add-on's own storage on the device and goes only to the
 * chosen service.
 *
 * It also answers other wapps: ai.chat, through the host's functionality
 * broker (see "ai.chat" below).
 *
 * Wire protocol, host -> wapp:
 *   {"type":"host.edit_target","dir":...}
 *   {"type":"widget.request","widget":"ai.chat",...}
 *   {"command":"ai_send"|"apply"|"discard"|"undo"|"new_chat"|
 *              "save_settings"|"forget_key", "fields":{...}}
 *   {"type":"http.done","id":N}   (the answer to hal_http_request2 is in)
 * wapp -> host: ui.field.set, ui.chat.append, ui.chat.clear, ui.toast.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hal/xprs_wasm_hal.h"

#include "../sdk/wapp_json.h"

/* The string at fields.<name> of a host command, into [out] (cleared). */
static int field_str(const char *msg, size_t n, const char *name, sb_t *out) {
    sb_clear(out);
    const char *e = msg + n;
    const char *f = j_get(msg, e, "fields");
    if (!f) return 0;
    const char *v = j_get(f, e, name);
    return v ? j_str(v, e, out) : 0;
}

/* ── Host messages ───────────────────────────────────────────────────── */

static void send_sb(const sb_t *b) { hal_msg_send(sb_str(b), (uint32_t)b->n); }

static void set_field(const char *name, const char *value) {
    sb_t b = {0};
    sb_puts(&b, "{\"type\":\"ui.field.set\",\"field\":");
    sb_jsonz(&b, name);
    sb_puts(&b, ",\"value\":");
    sb_jsonz(&b, value);
    sb_putc(&b, '}');
    send_sb(&b);
    sb_free(&b);
}

static void toast(const char *text) {
    sb_t b = {0};
    sb_puts(&b, "{\"type\":\"ui.toast\",\"message\":");
    sb_jsonz(&b, text);
    sb_putc(&b, '}');
    send_sb(&b);
    sb_free(&b);
}

/* One chat bubble: dir "out" for the person, "in" for the AI. */
static void chat(const char *dir, const char *from, const char *text) {
    sb_t b = {0};
    sb_puts(&b, "{\"type\":\"ui.chat.append\",\"field\":\"ai\",\"message\":{");
    sb_puts(&b, "\"dir\":");
    sb_jsonz(&b, dir);
    sb_puts(&b, ",\"from\":");
    sb_jsonz(&b, from);
    sb_puts(&b, ",\"text\":");
    sb_jsonz(&b, text);
    sb_puts(&b, "}}");
    send_sb(&b);
    sb_free(&b);
}

/* A translated string, or [fallback]. Static buffer: use before the next. */
static const char *t(const char *key, const char *fallback) {
    static char buf[512];
    uint32_t n = hal_i18n_get(key, (uint32_t)strlen(key), buf, sizeof(buf) - 1);
    if (n == 0) return fallback;
    buf[n] = 0;
    return buf;
}

/* ── Settings (this add-on's own storage) ────────────────────────────── */

static sb_t s_provider, s_model, s_base, s_key;

static void kv_load(const char *key, sb_t *out) {
    sb_clear(out);
    uint32_t sz = hal_kv_size(key, (uint32_t)strlen(key));
    if (sz == 0) return;
    sb_reserve(out, sz);
    if (out->cap < sz + 1) return;
    uint32_t n = hal_kv_get(key, (uint32_t)strlen(key), out->p, sz);
    out->n = n;
    out->p[n] = 0;
}
static void kv_save(const char *key, const sb_t *v) {
    hal_kv_set(key, (uint32_t)strlen(key), sb_str(v), (uint32_t)v->n);
}

static int is(const sb_t *b, const char *s) { return strcmp(sb_str(b), s) == 0; }

static const char *provider_base(void) {
    if (s_base.n) return sb_str(&s_base);
    if (is(&s_provider, "openai")) return "https://api.openai.com/v1";
    if (is(&s_provider, "opencode")) return "https://opencode.ai/zen/v1";
    if (is(&s_provider, "compatible")) return "";
    return "https://api.anthropic.com/v1";
}
static const char *provider_model(void) {
    if (s_model.n) return sb_str(&s_model);
    if (is(&s_provider, "claude") || s_provider.n == 0) return "claude-opus-5-5";
    return "";
}
static int provider_is_claude(void) {
    return s_provider.n == 0 || is(&s_provider, "claude");
}

static void push_settings(void) {
    set_field("ai_provider", s_provider.n ? sb_str(&s_provider) : "claude");
    set_field("ai_model", sb_str(&s_model));
    set_field("ai_base_url", sb_str(&s_base));
}

/* ── The wapp being edited ───────────────────────────────────────────── */

#define MAX_FILES 12
typedef struct {
    sb_t path; /* relative to the package, e.g. "screens/home.ui.json" */
    sb_t text;
} file_t;

static sb_t g_dir;                 /* absolute package dir, no trailing / */
static file_t g_files[MAX_FILES];  /* what the AI is shown */
static int g_nfiles;
static file_t g_pending[MAX_FILES]; /* proposed by the AI */
static int g_npending;
static file_t g_undo[MAX_FILES];    /* what Apply replaced */
static int g_nundo;

static void files_free(file_t *f, int *n) {
    for (int i = 0; i < *n; i++) {
        sb_free(&f[i].path);
        sb_free(&f[i].text);
    }
    *n = 0;
}

static int read_file(const char *abs, sb_t *out) {
    sb_clear(out);
    int32_t h = hal_file_open(abs, (uint32_t)strlen(abs), 0);
    if (h < 0) return 0;
    char buf[4096];
    for (;;) {
        int32_t n = hal_file_read(h, buf, sizeof(buf));
        if (n <= 0) break;
        sb_putn(out, buf, (size_t)n);
    }
    hal_file_close(h);
    return 1;
}
static int write_file(const char *abs, const sb_t *text) {
    int32_t h = hal_file_open(abs, (uint32_t)strlen(abs), 1);
    if (h < 0) return 0;
    int ok = hal_file_write(h, sb_str(text), (uint32_t)text->n) >= 0;
    hal_file_close(h);
    return ok;
}
static void abs_path(const char *rel, sb_t *out) {
    sb_clear(out);
    sb_puts(out, sb_str(&g_dir));
    sb_putc(out, '/');
    sb_puts(out, rel);
}

/* The files an edit may touch. Anything else the AI names is refused. */
static const char *const k_files[] = {
    "main.c", "screens/home.ui.json", "manifest.json", "lang/en.json",
    "lang/de.json", "lang/es.json", "lang/fr.json", "lang/pt.json",
};
#define K_NFILES ((int)(sizeof(k_files) / sizeof(k_files[0])))

static int allowed_path(const char *p, size_t n) {
    for (int i = 0; i < K_NFILES; i++)
        if (strlen(k_files[i]) == n && memcmp(k_files[i], p, n) == 0) return 1;
    return 0;
}

static void load_target(void) {
    files_free(g_files, &g_nfiles);
    sb_t abs = {0};
    for (int i = 0; i < K_NFILES && g_nfiles < MAX_FILES; i++) {
        abs_path(k_files[i], &abs);
        file_t *f = &g_files[g_nfiles];
        memset(f, 0, sizeof(*f));
        if (read_file(sb_str(&abs), &f->text)) {
            sb_puts(&f->path, k_files[i]);
            g_nfiles++;
        } else {
            sb_free(&f->text);
        }
    }
    sb_free(&abs);
    /* "Editing <title>": the manifest's title, else the folder. */
    const char *name = strrchr(sb_str(&g_dir), '/');
    sb_t title = {0};
    for (int i = 0; i < g_nfiles; i++) {
        if (!is(&g_files[i].path, "manifest.json")) continue;
        const char *e = g_files[i].text.p + g_files[i].text.n;
        const char *v = j_get(g_files[i].text.p, e, "title");
        if (v) j_str(v, e, &title);
    }
    set_field("editing", title.n ? sb_str(&title) : (name ? name + 1 : ""));
    sb_free(&title);
}

/* ── The conversation ────────────────────────────────────────────────── */

#define MAX_TURNS 12 /* person + AI messages kept for context */
static sb_t g_role[MAX_TURNS], g_text[MAX_TURNS];
static int g_turns;

static void turn_add(const char *role, const char *text, size_t n) {
    if (g_turns == MAX_TURNS) {
        /* Drop the oldest exchange (two messages) to keep the order valid. */
        sb_free(&g_role[0]);
        sb_free(&g_text[0]);
        sb_free(&g_role[1]);
        sb_free(&g_text[1]);
        memmove(g_role, g_role + 2, sizeof(sb_t) * (MAX_TURNS - 2));
        memmove(g_text, g_text + 2, sizeof(sb_t) * (MAX_TURNS - 2));
        g_turns -= 2;
        memset(&g_role[g_turns], 0, sizeof(sb_t) * 2);
        memset(&g_text[g_turns], 0, sizeof(sb_t) * 2);
    }
    sb_puts(&g_role[g_turns], role);
    sb_putn(&g_text[g_turns], text, n);
    g_turns++;
}
static void turns_clear(void) {
    for (int i = 0; i < g_turns; i++) {
        sb_free(&g_role[i]);
        sb_free(&g_text[i]);
    }
    g_turns = 0;
}

static const char k_system[] =
    "You edit a wapp: a small app for XPRS, written in C compiled to "
    "WebAssembly, with its screens declared in GeoUI JSON "
    "(screens/home.ui.json), its identity in manifest.json and its words in "
    "lang/<code>.json (keys used as @key in the screens). The C talks to the "
    "host only through the hal_* functions of xprs_wasm_hal.h and the JSON "
    "messages it exchanges with hal_msg_send / hal_msg_recv; follow the "
    "patterns already in main.c. Keep changes small and in the style of the "
    "code around them. Every string a person reads goes through lang/, in "
    "every language file the wapp has.\n\n"
    "Answer briefly in plain words. To change a file, give its WHOLE new "
    "content in a block exactly like this, one block per file:\n"
    "=== FILE: screens/home.ui.json\n<the whole file>\n=== END\n"
    "Only these files may be changed: main.c, screens/home.ui.json, "
    "manifest.json, lang/<code>.json. Change nothing when the request is "
    "only a question. Bump manifest.json's version when the code changes.";

/* The first user message: the files as they are now. */
static void put_files_message(sb_t *b) {
    sb_t m = {0};
    sb_puts(&m, "The wapp's files as they are now:\n\n");
    for (int i = 0; i < g_nfiles; i++) {
        sb_puts(&m, "=== FILE: ");
        sb_puts(&m, sb_str(&g_files[i].path));
        sb_putc(&m, '\n');
        sb_putn(&m, sb_str(&g_files[i].text), g_files[i].text.n);
        sb_puts(&m, "\n=== END\n\n");
    }
    sb_json(b, sb_str(&m), m.n);
    sb_free(&m);
}

static void build_request(sb_t *body) {
    int claude = provider_is_claude();
    sb_clear(body);
    sb_puts(body, "{\"model\":");
    sb_jsonz(body, provider_model());
    if (claude) {
        /* Non-streaming: one reply of up to 16k tokens. Medium effort is
         * Opus 5.5's default, set explicitly; on a decline the API re-runs
         * the request on a fallback model by itself. */
        sb_puts(body, ",\"max_tokens\":16000,\"output_config\":"
                      "{\"effort\":\"medium\"},\"fallbacks\":\"default\","
                      "\"system\":");
        sb_jsonz(body, k_system);
        sb_puts(body, ",\"messages\":[");
    } else {
        sb_puts(body, ",\"messages\":[{\"role\":\"system\",\"content\":");
        sb_jsonz(body, k_system);
        sb_puts(body, "},");
    }
    /* The files, then an acknowledgement, keep the turns alternating. */
    sb_puts(body, "{\"role\":\"user\",\"content\":");
    put_files_message(body);
    sb_puts(body, "},{\"role\":\"assistant\",\"content\":\"Understood.\"}");
    for (int i = 0; i < g_turns; i++) {
        sb_puts(body, ",{\"role\":");
        sb_jsonz(body, sb_str(&g_role[i]));
        sb_puts(body, ",\"content\":");
        sb_json(body, sb_str(&g_text[i]), g_text[i].n);
        sb_putc(body, '}');
    }
    sb_puts(body, "]}");
}

static void build_headers(sb_t *h) {
    sb_clear(h);
    sb_puts(h, "Content-Type: application/json\n");
    if (provider_is_claude()) {
        sb_puts(h, "x-api-key: ");
        sb_puts(h, sb_str(&s_key));
        sb_puts(h, "\nanthropic-version: 2026-10-21\n"
                   "anthropic-beta: server-side-fallback-2026-07-01\n");
    } else if (s_key.n) {
        sb_puts(h, "Authorization: Bearer ");
        sb_puts(h, sb_str(&s_key));
        sb_putc(h, '\n');
    }
}

/* ── One request in flight ───────────────────────────────────────────── */

static int32_t g_req = -1;
static uint64_t g_req_started;

static void ai_send(const char *text, size_t n) {
    if (g_req >= 0) {
        toast(t("msg.busy", "Still answering the last message."));
        return;
    }
    if (g_dir.n == 0) {
        chat("in", "AI",
             t("msg.no_target", "Open this from the editor of a wapp."));
        return;
    }
    if (!provider_is_claude() && (provider_model()[0] == 0 ||
                                  provider_base()[0] == 0)) {
        chat("in", "AI",
             t("msg.need_model", "Set the model and address in Settings."));
        return;
    }
    if (s_key.n == 0 && !is(&s_provider, "compatible")) {
        chat("in", "AI",
             t("msg.need_key", "Add the service's key in Settings."));
        return;
    }
    turn_add("user", text, n);
    load_target(); /* the files as they are now, edits in the editor too */
    sb_t url = {0}, body = {0}, hdr = {0};
    sb_puts(&url, provider_base());
    sb_puts(&url, provider_is_claude() ? "/messages" : "/chat/completions");
    build_request(&body);
    build_headers(&hdr);
    g_req = hal_http_request2(1, sb_str(&url), (uint32_t)url.n, sb_str(&hdr),
                              (uint32_t)hdr.n, sb_str(&body),
                              (uint32_t)body.n, 600000);
    g_req_started = hal_time_ms();
    if (g_req < 0)
        chat("in", "AI", t("msg.send_failed", "Could not send it."));
    sb_free(&url);
    sb_free(&body);
    sb_free(&hdr);
}

/* The reply's text: Claude's text blocks, or OpenAI's message content. */
static int reply_text(const char *p, const char *e, sb_t *out) {
    sb_clear(out);
    if (provider_is_claude()) {
        const char *c = j_get(p, e, "content");
        if (!c || *(c = j_ws(c, e)) != '[') return 0;
        const char *q = j_ws(c + 1, e);
        while (q && q < e && *q == '{') {
            const char *end = j_skip(q, e);
            const char *ty = j_get(q, e, "type");
            sb_t tv = {0};
            if (ty && j_str(ty, e, &tv) && is(&tv, "text")) {
                const char *tx = j_get(q, e, "text");
                if (tx) j_str(tx, e, out);
            }
            sb_free(&tv);
            if (!end) break;
            q = j_ws(end, e);
            if (q < e && *q == ',') q = j_ws(q + 1, e);
        }
        return 1;
    }
    const char *ch = j_get(p, e, "choices");
    if (!ch || *(ch = j_ws(ch, e)) != '[') return 0;
    const char *m = j_get(j_ws(ch + 1, e), e, "message");
    const char *c = m ? j_get(m, e, "content") : 0;
    return c ? j_str(c, e, out) : 0;
}

/* The service's own error message, if the body carries one. */
static void error_text(const char *p, const char *e, sb_t *out) {
    sb_clear(out);
    const char *er = j_get(p, e, "error");
    if (!er) return;
    const char *m = j_get(er, e, "message");
    if (m) j_str(m, e, out);
    else j_str(er, e, out);
}

/* Split the reply into words for the chat and proposed files. */
static void take_reply(const sb_t *reply) {
    files_free(g_pending, &g_npending);
    sb_t words = {0};
    const char *p = sb_str(reply);
    const char *e = p + reply->n;
    static const char open[] = "=== FILE: ";
    static const char close[] = "\n=== END";
    while (p < e) {
        const char *f = strstr(p, open);
        if (!f) {
            sb_putn(&words, p, (size_t)(e - p));
            break;
        }
        sb_putn(&words, p, (size_t)(f - p));
        const char *ps = f + sizeof(open) - 1;
        const char *pe = strchr(ps, '\n');
        if (!pe) break;
        const char *body = pe + 1;
        const char *be = strstr(body, close);
        if (!be) be = e;
        size_t pl = (size_t)(pe - ps);
        while (pl && (ps[pl - 1] == ' ' || ps[pl - 1] == '\r')) pl--;
        if (allowed_path(ps, pl) && g_npending < MAX_FILES) {
            file_t *pf = &g_pending[g_npending++];
            memset(pf, 0, sizeof(*pf));
            sb_putn(&pf->path, ps, pl);
            sb_putn(&pf->text, body, (size_t)(be - body));
            sb_putc(&pf->text, '\n');
        }
        p = be + (be < e ? sizeof(close) - 1 : 0);
        while (p < e && *p != '\n') p++;
    }
    /* Trim the words. */
    while (words.n && (words.p[words.n - 1] == '\n' || words.p[words.n - 1] == ' '))
        words.p[--words.n] = 0;
    if (words.n) chat("in", "AI", sb_str(&words));

    sb_t list = {0};
    for (int i = 0; i < g_npending; i++) {
        size_t lines = 0;
        for (size_t k = 0; k < g_pending[i].text.n; k++)
            if (g_pending[i].text.p[k] == '\n') lines++;
        char num[24];
        int ln = 0;
        size_t v = lines;
        char rev[24];
        do {
            rev[ln++] = (char)('0' + v % 10);
            v /= 10;
        } while (v && ln < 23);
        for (int k = 0; k < ln; k++) num[k] = rev[ln - 1 - k];
        num[ln] = 0;
        sb_puts(&list, sb_str(&g_pending[i].path));
        sb_puts(&list, " · ");
        sb_puts(&list, num);
        sb_puts(&list, " ");
        sb_puts(&list, t("msg.lines", "lines"));
        sb_putc(&list, '\n');
    }
    set_field("changes", list.n ? sb_str(&list)
                                : t("msg.no_changes", "No changes proposed."));
    sb_free(&list);
    sb_free(&words);
}


static void poll_request(void) {
    if (g_req < 0) return;
    sb_t body = {0}, text = {0};
    int status = 0;
    int32_t st = http_take(g_req, &body, &status);
    if (st == 0) return;
    g_req = -1;
    const char *p = sb_str(&body), *e = p + body.n;
    if (st < 0) {
        chat("in", "AI",
             t("msg.unreachable", "The service could not be reached."));
    } else if (status < 200 || status >= 300) {
        error_text(p, e, &text);
        sb_t m = {0};
        sb_puts(&m, t("msg.refused", "The service said no"));
        sb_puts(&m, ": ");
        sb_puts(&m, text.n ? sb_str(&text) : "HTTP error");
        chat("in", "AI", sb_str(&m));
        sb_free(&m);
    } else {
        const char *sr = j_get(p, e, "stop_reason");
        sb_t srv = {0};
        if (sr) j_str(sr, e, &srv);
        if (is(&srv, "refusal")) {
            chat("in", "AI",
                 t("msg.declined", "The AI declined this request."));
        } else if (reply_text(p, e, &text) && text.n) {
            turn_add("assistant", sb_str(&text), text.n);
            take_reply(&text);
            if (is(&srv, "max_tokens"))
                chat("in", "AI",
                     t("msg.cut", "The answer was cut off; ask for less at once."));
        } else {
            chat("in", "AI", t("msg.empty", "The AI sent nothing back."));
        }
        sb_free(&srv);
    }
    sb_free(&body);
    sb_free(&text);
}

/* ── ai.chat: the configured AI, for other wapps ─────────────────────────
 *
 * Another wapp asks through the host's functionality broker
 * (wapp-interfaces.md 5):
 *   {"type":"widget.request","widget":"ai.chat","req_id":R,"caller":W,
 *    "args":{"messages":[{"role":"user"|"assistant","content":S},...],
 *            "system":S (optional),"max_tokens":N (optional)}}
 * and is answered when the service has replied:
 *   {"type":"widget.response","req_id":R,
 *    "result":{"text":S,"stop":"end"|"max_tokens"|"refusal",
 *              "input_tokens":N,"output_tokens":N}}
 * or {"type":"widget.response","req_id":R,"error":"<code>: <words>"} with
 * code one of not_configured, bad_request, busy, unreachable, refused,
 * empty, unsupported.
 *
 * The caller sends the whole conversation each time and gets text back:
 * nothing of it is kept here, and the key, the service and the model stay
 * in this wapp. Several requests may be in flight. */

#define SVC_MAX 8
#define SVC_DEFAULT_TOKENS 4096
#define SVC_MAX_TOKENS 16000
/* Under the functionality's timeout_ms in manifest.json, so the service's
 * own failure is what the caller hears, not the broker's time limit. */
#define SVC_HTTP_TIMEOUT_MS 280000

typedef struct {
    int32_t req; /* hal http handle, -1 when free */
    sb_t id;     /* the broker's req_id */
} svc_t;
static svc_t g_svc[SVC_MAX];
static int g_svc_ready;

static void svc_setup(void) {
    if (g_svc_ready) return;
    for (int i = 0; i < SVC_MAX; i++) g_svc[i].req = -1;
    g_svc_ready = 1;
}

static void svc_error(const char *id, const char *code, const char *words) {
    sb_t b = {0}, m = {0};
    sb_puts(&m, code);
    sb_puts(&m, ": ");
    sb_puts(&m, words);
    sb_puts(&b, "{\"type\":\"widget.response\",\"req_id\":");
    sb_jsonz(&b, id);
    sb_puts(&b, ",\"error\":");
    sb_jsonz(&b, sb_str(&m));
    sb_putc(&b, '}');
    send_sb(&b);
    sb_free(&b);
    sb_free(&m);
}

static void sb_putl(sb_t *b, long v) {
    char tmp[24];
    int n = 0;
    if (v <= 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0) sb_putc(b, tmp[--n]);
}


/* Copy the caller's messages into [out] as a JSON array body (no brackets):
 * only user/assistant turns with string content. Returns how many. */
static int svc_messages(const char *arr, const char *e, sb_t *out) {
    int count = 0;
    if (!arr || *(arr = j_ws(arr, e)) != '[') return 0;
    const char *q = j_ws(arr + 1, e);
    while (q && q < e && *q == '{') {
        const char *end = j_skip(q, e);
        sb_t role = {0}, text = {0};
        const char *r = j_get(q, e, "role");
        const char *c = j_get(q, e, "content");
        if (r && c && j_str(r, e, &role) && j_str(c, e, &text) &&
            (is(&role, "user") || is(&role, "assistant"))) {
            if (count++) sb_putc(out, ',');
            sb_puts(out, "{\"role\":");
            sb_jsonz(out, sb_str(&role));
            sb_puts(out, ",\"content\":");
            sb_json(out, sb_str(&text), text.n);
            sb_putc(out, '}');
        }
        sb_free(&role);
        sb_free(&text);
        if (!end) break;
        q = j_ws(end, e);
        if (q < e && *q == ',') q = j_ws(q + 1, e);
    }
    return count;
}

static void svc_chat(const char *id, const char *args, const char *e) {
    svc_setup();
    if (!provider_is_claude() &&
        (provider_model()[0] == 0 || provider_base()[0] == 0)) {
        svc_error(id, "not_configured", "set the model and address in AI");
        return;
    }
    if (s_key.n == 0 && !is(&s_provider, "compatible")) {
        svc_error(id, "not_configured", "add the service's key in AI");
        return;
    }
    svc_t *slot = 0;
    for (int i = 0; i < SVC_MAX && !slot; i++)
        if (g_svc[i].req < 0) slot = &g_svc[i];
    if (!slot) {
        svc_error(id, "busy", "too many requests at once");
        return;
    }
    sb_t msgs = {0}, system = {0};
    int count = args ? svc_messages(j_get(args, e, "messages"), e, &msgs) : 0;
    if (count == 0) {
        svc_error(id, "bad_request", "args.messages has no user message");
        sb_free(&msgs);
        return;
    }
    const char *sy = j_get(args, e, "system");
    if (sy) j_str(sy, e, &system);
    long max = j_num(j_get(args, e, "max_tokens"), e);
    if (max <= 0) max = SVC_DEFAULT_TOKENS;
    if (max > SVC_MAX_TOKENS) max = SVC_MAX_TOKENS;

    int claude = provider_is_claude();
    sb_t url = {0}, body = {0}, hdr = {0};
    sb_puts(&url, provider_base());
    sb_puts(&url, claude ? "/messages" : "/chat/completions");
    sb_puts(&body, "{\"model\":");
    sb_jsonz(&body, provider_model());
    if (claude) {
        sb_puts(&body, ",\"max_tokens\":");
        sb_putl(&body, max);
        sb_puts(&body, ",\"fallbacks\":\"default\"");
        if (system.n) {
            sb_puts(&body, ",\"system\":");
            sb_json(&body, sb_str(&system), system.n);
        }
        sb_puts(&body, ",\"messages\":[");
    } else {
        /* OpenAI's own service names the limit max_completion_tokens;
         * the servers that copy its API still take max_tokens. */
        sb_puts(&body, is(&s_provider, "openai")
                           ? ",\"max_completion_tokens\":"
                           : ",\"max_tokens\":");
        sb_putl(&body, max);
        sb_puts(&body, ",\"messages\":[");
        if (system.n) {
            sb_puts(&body, "{\"role\":\"system\",\"content\":");
            sb_json(&body, sb_str(&system), system.n);
            sb_puts(&body, "},");
        }
    }
    sb_putn(&body, sb_str(&msgs), msgs.n);
    sb_puts(&body, "]}");
    build_headers(&hdr);
    int32_t req = hal_http_request2(1, sb_str(&url), (uint32_t)url.n,
                                    sb_str(&hdr), (uint32_t)hdr.n,
                                    sb_str(&body), (uint32_t)body.n,
                                    SVC_HTTP_TIMEOUT_MS);
    if (req < 0) {
        svc_error(id, "unreachable", "the request could not be sent");
    } else {
        slot->req = req;
        sb_clear(&slot->id);
        sb_puts(&slot->id, id);
    }
    sb_free(&url);
    sb_free(&body);
    sb_free(&hdr);
    sb_free(&msgs);
    sb_free(&system);
}

/* Answer every ai.chat request whose service reply is in. */
static void svc_poll(void) {
    svc_setup();
    for (int i = 0; i < SVC_MAX; i++) {
        svc_t *s = &g_svc[i];
        if (s->req < 0) continue;
        sb_t body = {0}, text = {0};
        int status = 0;
        int32_t st = http_take(s->req, &body, &status);
        if (st == 0) continue;
        s->req = -1;
        const char *id = sb_str(&s->id);
        const char *p = sb_str(&body), *e = p + body.n;
        if (st < 0) {
            svc_error(id, "unreachable", "the service could not be reached");
        } else if (status < 200 || status >= 300) {
            error_text(p, e, &text);
            svc_error(id, "refused", text.n ? sb_str(&text) : "HTTP error");
        } else {
            sb_t stop = {0};
            const char *sr = j_get(p, e, "stop_reason");
            if (sr) j_str(sr, e, &stop);
            if (!provider_is_claude()) {
                const char *ch = j_get(p, e, "choices");
                const char *fr = ch && *(ch = j_ws(ch, e)) == '['
                                     ? j_get(j_ws(ch + 1, e), e, "finish_reason")
                                     : 0;
                if (fr) j_str(fr, e, &stop);
            }
            int refusal = is(&stop, "refusal") || is(&stop, "content_filter");
            int cut = is(&stop, "max_tokens") || is(&stop, "length");
            reply_text(p, e, &text);
            if (!refusal && text.n == 0) {
                svc_error(id, "empty", "the AI sent nothing back");
            } else {
                const char *u = j_get(p, e, "usage");
                long in = 0, out = 0;
                if (u) {
                    const char *a = j_get(u, e, "input_tokens");
                    const char *b = j_get(u, e, "output_tokens");
                    if (!a) a = j_get(u, e, "prompt_tokens");
                    if (!b) b = j_get(u, e, "completion_tokens");
                    in = j_num(a, e);
                    out = j_num(b, e);
                }
                sb_t r = {0};
                sb_puts(&r, "{\"type\":\"widget.response\",\"req_id\":");
                sb_jsonz(&r, id);
                sb_puts(&r, ",\"result\":{\"text\":");
                sb_json(&r, sb_str(&text), text.n);
                sb_puts(&r, ",\"stop\":");
                sb_jsonz(&r, refusal ? "refusal" : cut ? "max_tokens" : "end");
                sb_puts(&r, ",\"input_tokens\":");
                sb_putl(&r, in);
                sb_puts(&r, ",\"output_tokens\":");
                sb_putl(&r, out);
                sb_puts(&r, "}}");
                send_sb(&r);
                sb_free(&r);
            }
            sb_free(&stop);
        }
        sb_free(&body);
        sb_free(&text);
    }
}

static void svc_request(const char *msg, const char *e) {
    sb_t id = {0}, what = {0};
    const char *r = j_get(msg, e, "req_id");
    const char *w = j_get(msg, e, "widget");
    if (r) j_str(r, e, &id);
    if (w) j_str(w, e, &what);
    if (is(&what, "ai.chat")) svc_chat(sb_str(&id), j_get(msg, e, "args"), e);
    else svc_error(sb_str(&id), "unsupported", sb_str(&what));
    sb_free(&id);
    sb_free(&what);
}

/* ── Apply, discard, undo ────────────────────────────────────────────── */

static void apply_changes(void) {
    if (g_npending == 0) {
        toast(t("msg.nothing", "Nothing to apply."));
        return;
    }
    files_free(g_undo, &g_nundo);
    sb_t abs = {0};
    int written = 0;
    for (int i = 0; i < g_npending; i++) {
        abs_path(sb_str(&g_pending[i].path), &abs);
        file_t *u = &g_undo[g_nundo++];
        memset(u, 0, sizeof(*u));
        sb_puts(&u->path, sb_str(&g_pending[i].path));
        if (!read_file(sb_str(&abs), &u->text)) sb_clear(&u->text);
        if (write_file(sb_str(&abs), &g_pending[i].text)) written++;
    }
    sb_free(&abs);
    files_free(g_pending, &g_npending);
    set_field("changes", t("msg.applied", "Applied. Build it in the editor."));
    chat("in", "AI", t("msg.applied", "Applied. Build it in the editor."));
    (void)written;
    load_target();
}

static void undo_changes(void) {
    if (g_nundo == 0) {
        toast(t("msg.nothing_undo", "Nothing to undo."));
        return;
    }
    sb_t abs = {0};
    for (int i = 0; i < g_nundo; i++) {
        abs_path(sb_str(&g_undo[i].path), &abs);
        write_file(sb_str(&abs), &g_undo[i].text);
    }
    sb_free(&abs);
    files_free(g_undo, &g_nundo);
    set_field("changes", t("msg.undone", "Undone."));
    load_target();
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

void module_init(void) {
    kv_load("ai_provider", &s_provider);
    kv_load("ai_model", &s_model);
    kv_load("ai_base_url", &s_base);
    kv_load("ai_key", &s_key);
    push_settings();
    set_field("changes", "");
}

/* No clock: the host hands over {"type":"http.done","id":N} when the
 * answer is in (hal_http_request2), so nothing asks whether it came. */
void module_tick(void) {}

uint32_t module_tick_interval_ms(void) { return 0; }

void module_destroy(void) {
    if (g_req >= 0) hal_http_free(g_req);
    g_req = -1;
    for (int i = 0; i < SVC_MAX; i++) {
        if (g_svc_ready && g_svc[i].req >= 0) hal_http_free(g_svc[i].req);
        g_svc[i].req = -1;
    }
}

static int cmd_is(const sb_t *cmd, const char *s) { return is(cmd, s); }

void module_handle_event(void) {
    static char *buf;
    static uint32_t cap;
    while (hal_msg_available() != 0) {
        uint32_t want = hal_msg_available();
        if (want + 1 > cap) {
            char *nb = realloc(buf, want + 1);
            if (!nb) return;
            buf = nb;
            cap = want + 1;
        }
        uint32_t n = hal_msg_recv(buf, cap - 1);
        if (n == 0) break;
        buf[n] = 0;
        const char *e = buf + n;
        sb_t v = {0};

        const char *ty = j_get(buf, e, "type");
        if (ty) j_str(ty, e, &v);
        if (is(&v, "http.done")) {
            poll_request();
            svc_poll();
            sb_free(&v);
            continue;
        }
        if (is(&v, "widget.request")) {
            svc_request(buf, e);
            sb_free(&v);
            continue;
        }
        if (is(&v, "host.edit_target")) {
            const char *d = j_get(buf, e, "dir");
            sb_clear(&g_dir);
            if (d) j_str(d, e, &g_dir);
            while (g_dir.n && g_dir.p[g_dir.n - 1] == '/') g_dir.p[--g_dir.n] = 0;
            load_target();
            sb_free(&v);
            continue;
        }

        const char *c = j_get(buf, e, "command");
        sb_t cmd = {0};
        if (c) j_str(c, e, &cmd);
        if (cmd_is(&cmd, "ai_send")) {
            if (field_str(buf, n, "ai_input", &v) && v.n) {
                chat("out", t("msg.you", "You"), sb_str(&v));
                ai_send(sb_str(&v), v.n);
            }
        } else if (cmd_is(&cmd, "apply")) {
            apply_changes();
        } else if (cmd_is(&cmd, "discard")) {
            files_free(g_pending, &g_npending);
            set_field("changes", "");
        } else if (cmd_is(&cmd, "undo")) {
            undo_changes();
        } else if (cmd_is(&cmd, "new_chat")) {
            turns_clear();
            files_free(g_pending, &g_npending);
            {
                static const char clr[] =
                    "{\"type\":\"ui.chat.clear\",\"field\":\"ai\"}";
                hal_msg_send(clr, sizeof(clr) - 1);
            }
            set_field("changes", "");
        } else if (cmd_is(&cmd, "save_settings")) {
            field_str(buf, n, "ai_provider", &s_provider);
            field_str(buf, n, "ai_model", &s_model);
            field_str(buf, n, "ai_base_url", &s_base);
            kv_save("ai_provider", &s_provider);
            kv_save("ai_model", &s_model);
            kv_save("ai_base_url", &s_base);
            /* A blank key field keeps the saved key (it is never shown). */
            if (field_str(buf, n, "ai_key", &v) && v.n) {
                sb_clear(&s_key);
                sb_putn(&s_key, sb_str(&v), v.n);
                kv_save("ai_key", &s_key);
            }
            toast(t("msg.saved", "Saved."));
        } else if (cmd_is(&cmd, "forget_key")) {
            sb_clear(&s_key);
            hal_kv_delete("ai_key", 6);
            toast(t("msg.key_forgotten", "Key removed from this device."));
        }
        sb_free(&cmd);
        sb_free(&v);
    }
}
