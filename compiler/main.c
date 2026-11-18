/*
 * tools.xprs.compiler — the Compiler add-on.
 *
 * An ordinary wapp, installed and shared like any other and never part of
 * the app. It carries two things:
 *
 *   1. A toolchain the HOST runs for other wapps' builds (media/llvm.wasm,
 *      media/usr; see fetch-toolchain.sh and the app's compiler_addon.dart).
 *      This program has nothing to do with that part.
 *
 *   2. This program: an AI that edits a wapp. The editor opens the add-on
 *      "on" the wapp being edited; the host says which with
 *      {"type":"host.edit_target","dir":"<absolute package dir>"}. The AI
 *      reads the wapp's files, answers in the chat, and proposes whole-file
 *      replacements. Nothing is written until the person taps Apply, and
 *      Undo puts the previous files back. Building happens in the editor.
 *
 * Services: Claude (Anthropic Messages API), Codex / OpenAI, OpenCode Zen and
 * any OpenAI-compatible server (Ollama, LM Studio, OpenRouter, ...). The key
 * stays in this add-on's own storage on the device and goes only to the
 * chosen service.
 *
 * Wire protocol, host -> wapp:
 *   {"type":"host.edit_target","dir":...}
 *   {"command":"ai_send"|"apply"|"discard"|"undo"|"new_chat"|
 *              "save_settings"|"forget_key", "fields":{...}}
 *   {"type":"http.done","id":N}   (the answer to hal_http_request2 is in)
 * wapp -> host: ui.field.set, ui.chat.append, ui.chat.clear, ui.toast.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hal/xprs_wasm_hal.h"

/* ── Growable string ─────────────────────────────────────────────────── */

typedef struct {
    char *p;
    size_t n, cap;
} sb_t;

static void sb_reserve(sb_t *b, size_t more) {
    if (b->n + more + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + more + 1) cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np) return;
    b->p = np;
    b->cap = cap;
}
static void sb_putn(sb_t *b, const char *s, size_t n) {
    sb_reserve(b, n);
    if (b->n + n + 1 > b->cap) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_puts(sb_t *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(sb_t *b, char c) { sb_putn(b, &c, 1); }
static void sb_clear(sb_t *b) {
    b->n = 0;
    if (b->p) b->p[0] = 0;
}
static void sb_free(sb_t *b) {
    free(b->p);
    b->p = 0;
    b->n = b->cap = 0;
}
static const char *sb_str(const sb_t *b) { return b->p ? b->p : ""; }

/* A JSON string literal, quotes included. */
static void sb_json(sb_t *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    sb_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char u[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                sb_putn(b, u, 6);
            } else {
                sb_putc(b, (char)c);
            }
        }
    }
    sb_putc(b, '"');
}
static void sb_jsonz(sb_t *b, const char *s) { sb_json(b, s, strlen(s)); }

/* ── A small JSON reader: enough to walk to a value and decode strings ── */

static const char *j_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}
static const char *j_skip(const char *p, const char *e);
static const char *j_skip_str(const char *p, const char *e) {
    if (p >= e || *p != '"') return 0;
    for (p++; p < e; p++) {
        if (*p == '\\') {
            p++;
            continue;
        }
        if (*p == '"') return p + 1;
    }
    return 0;
}
static const char *j_skip(const char *p, const char *e) {
    p = j_ws(p, e);
    if (p >= e) return 0;
    if (*p == '"') return j_skip_str(p, e);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = j_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = j_skip_str(j_ws(p, e), e);
                if (!p) return 0;
                p = j_ws(p, e);
                if (p >= e || *p != ':') return 0;
                p++;
            }
            p = j_skip(p, e);
            if (!p) return 0;
            p = j_ws(p, e);
            if (p >= e) return 0;
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == close) return p + 1;
            return 0;
        }
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\n' && *p != '\r' && *p != '\t')
        p++;
    return p;
}

/* The value of [key] in the object at [p], or 0. */
static const char *j_get(const char *p, const char *e, const char *key) {
    size_t kl = strlen(key);
    p = j_ws(p, e);
    if (p >= e || *p != '{') return 0;
    p = j_ws(p + 1, e);
    while (p && p < e && *p == '"') {
        const char *ks = p + 1;
        const char *ke = j_skip_str(p, e);
        if (!ke) return 0;
        p = j_ws(ke, e);
        if (p >= e || *p != ':') return 0;
        p = j_ws(p + 1, e);
        if ((size_t)(ke - 1 - ks) == kl && memcmp(ks, key, kl) == 0) return p;
        p = j_skip(p, e);
        if (!p) return 0;
        p = j_ws(p, e);
        if (p < e && *p == ',') p = j_ws(p + 1, e);
    }
    return 0;
}

static void utf8_put(sb_t *b, unsigned cp) {
    char u[4];
    if (cp < 0x80) {
        sb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        sb_putn(b, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        sb_putn(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        sb_putn(b, u, 4);
    }
}
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static unsigned u4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p[i]);
        if (h < 0) return 0xFFFD;
        v = v * 16 + (unsigned)h;
    }
    return v;
}

/* Append the decoded string at [p] to [out]; 1 if it was a string. */
static int j_str(const char *p, const char *e, sb_t *out) {
    p = j_ws(p, e);
    if (!p || p >= e || *p != '"') return 0;
    for (p++; p < e && *p != '"'; p++) {
        if (*p != '\\') {
            sb_putc(out, *p);
            continue;
        }
        if (++p >= e) break;
        switch (*p) {
        case 'n': sb_putc(out, '\n'); break;
        case 't': sb_putc(out, '\t'); break;
        case 'r': sb_putc(out, '\r'); break;
        case 'b': sb_putc(out, '\b'); break;
        case 'f': sb_putc(out, '\f'); break;
        case 'u':
            if (p + 4 < e) {
                unsigned cp = u4(p + 1);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < e && p[1] == '\\' &&
                    p[2] == 'u') {
                    unsigned lo = u4(p + 3);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                utf8_put(out, cp);
            }
            break;
        default: sb_putc(out, *p);
        }
    }
    return 1;
}

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
        chat("in", "Compiler",
             t("msg.no_target", "Open this from the editor of a wapp."));
        return;
    }
    if (!provider_is_claude() && (provider_model()[0] == 0 ||
                                  provider_base()[0] == 0)) {
        chat("in", "Compiler",
             t("msg.need_model", "Set the model and address in Settings."));
        return;
    }
    if (s_key.n == 0 && !is(&s_provider, "compatible")) {
        chat("in", "Compiler",
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
        chat("in", "Compiler", t("msg.send_failed", "Could not send it."));
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
    int32_t st = hal_http_poll(g_req);
    if (st == 0) return;
    sb_t body = {0}, text = {0};
    int status = hal_http_status(g_req);
    if (st > 0) {
        char buf[8192];
        for (;;) {
            int32_t n = hal_http_read_response(g_req, buf, sizeof(buf));
            if (n <= 0) break;
            sb_putn(&body, buf, (size_t)n);
        }
    }
    hal_http_free(g_req);
    g_req = -1;
    const char *p = sb_str(&body), *e = p + body.n;
    if (st < 0) {
        chat("in", "Compiler",
             t("msg.unreachable", "The service could not be reached."));
    } else if (status < 200 || status >= 300) {
        error_text(p, e, &text);
        sb_t m = {0};
        sb_puts(&m, t("msg.refused", "The service said no"));
        sb_puts(&m, ": ");
        sb_puts(&m, text.n ? sb_str(&text) : "HTTP error");
        chat("in", "Compiler", sb_str(&m));
        sb_free(&m);
    } else {
        const char *sr = j_get(p, e, "stop_reason");
        sb_t srv = {0};
        if (sr) j_str(sr, e, &srv);
        if (is(&srv, "refusal")) {
            chat("in", "Compiler",
                 t("msg.declined", "The AI declined this request."));
        } else if (reply_text(p, e, &text) && text.n) {
            turn_add("assistant", sb_str(&text), text.n);
            take_reply(&text);
            if (is(&srv, "max_tokens"))
                chat("in", "Compiler",
                     t("msg.cut", "The answer was cut off; ask for less at once."));
        } else {
            chat("in", "Compiler", t("msg.empty", "The AI sent nothing back."));
        }
        sb_free(&srv);
    }
    sb_free(&body);
    sb_free(&text);
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
    chat("in", "Compiler", t("msg.applied", "Applied. Build it in the editor."));
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
