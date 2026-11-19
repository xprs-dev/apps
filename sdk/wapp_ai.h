/*
 * wapp_ai.h — ask the AI the person has set up, from any wapp.
 *
 * The AI belongs to whichever installed wapp provides `ai.chat` (the AI
 * wapp, tools.xprs.ai, or another the person prefers). Your wapp never sees
 * its key, service or model: it sends a conversation and gets text back,
 * through the host's functionality broker (wapp-interfaces.md 5). The
 * answer comes later, as a message; nothing blocks and nothing polls.
 *
 *   #include "../sdk/wapp_ai.h"
 *
 *   // ask (one question; for a conversation build the request yourself,
 *   // the shape is in wapp-interfaces.md 5.2)
 *   wapp_ai_ask("sum1", "Answer in one sentence.", note_text);
 *
 *   // in module_handle_event, for each message `buf` of `n` bytes:
 *   char id[32], text[4096];
 *   int r = wapp_ai_reply(buf, n, id, sizeof(id), text, sizeof(text));
 *   if (r == 1) show(text);          // the AI's reply to request `id`
 *   if (r == -1) show_error(text);   // "<code>: <words>", e.g.
 *                                    // "not_configured: add the service's
 *                                    // key in AI", "no provider ..."
 *   // r == 0: not an answer to an AI request
 *
 * Request ids you pass must start with nothing special; the helper prefixes
 * them with "ai:" on the wire so wapp_ai_reply recognises its own answers
 * among other widget.response messages, and hands your id back.
 *
 * No libc, no allocation. Declare `"requires": {"functionalities":
 * ["ai.chat"]}` in manifest.json if the wapp is useless without an AI.
 */
#ifndef WAPP_AI_H
#define WAPP_AI_H

#include <stdint.h>

#include "../hal/xprs_wasm_hal.h"

#ifndef WAPP_AI_REQ_CAP
#define WAPP_AI_REQ_CAP 8192 /* the largest request wapp_ai_ask can send */
#endif

/* Append [s] to [b] as the inside of a JSON string. */
static inline unsigned wapp_ai__esc(char *b, unsigned o, unsigned cap,
                                    const char *s) {
    static const char hex[] = "0123456789abcdef";
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (o + 7 >= cap) break;
        if (c == '"' || c == '\\') {
            b[o++] = '\\';
            b[o++] = (char)c;
        } else if (c == '\n') {
            b[o++] = '\\';
            b[o++] = 'n';
        } else if (c < 0x20) {
            b[o++] = '\\';
            b[o++] = 'u';
            b[o++] = '0';
            b[o++] = '0';
            b[o++] = hex[c >> 4];
            b[o++] = hex[c & 15];
        } else {
            b[o++] = (char)c;
        }
    }
    return o;
}

static inline unsigned wapp_ai__lit(char *b, unsigned o, unsigned cap,
                                    const char *s) {
    for (; *s && o + 1 < cap; s++) b[o++] = *s;
    return o;
}

/* Ask one question. [system] may be 0. Returns 0 when sent, -1 when the
 * question does not fit WAPP_AI_REQ_CAP. */
static inline int wapp_ai_ask(const char *req_id, const char *system,
                              const char *prompt) {
    static char b[WAPP_AI_REQ_CAP];
    unsigned cap = sizeof(b), o = 0;
    o = wapp_ai__lit(b, o, cap,
                     "{\"type\":\"widget.request\",\"widget\":\"ai.chat\","
                     "\"req_id\":\"ai:");
    o = wapp_ai__esc(b, o, cap, req_id);
    o = wapp_ai__lit(b, o, cap, "\",\"args\":{");
    if (system && *system) {
        o = wapp_ai__lit(b, o, cap, "\"system\":\"");
        o = wapp_ai__esc(b, o, cap, system);
        o = wapp_ai__lit(b, o, cap, "\",");
    }
    o = wapp_ai__lit(b, o, cap,
                     "\"messages\":[{\"role\":\"user\",\"content\":\"");
    unsigned before = o;
    o = wapp_ai__esc(b, o, cap, prompt);
    /* Did the whole prompt go in? (esc stops when the buffer is full.) */
    unsigned need = 0;
    for (const char *s = prompt; s && *s; s++) need++;
    if (o - before < need || o + 6 >= cap) return -1;
    o = wapp_ai__lit(b, o, cap, "\"}]}}");
    hal_msg_send(b, o);
    return 0;
}

/* Find `"key":` at the top level of the object at [p,e); returns the value's
 * first byte or 0. Skips strings and nested values properly. */
static inline const char *wapp_ai__get(const char *p, const char *e,
                                       const char *key) {
    int depth = 0;
    while (p < e) {
        char c = *p;
        if (c == '"') {
            const char *s = ++p;
            while (p < e && *p != '"') p += (*p == '\\' && p + 1 < e) ? 2 : 1;
            const char *se = p++;
            if (depth == 1) {
                const char *q = p;
                while (q < e && (*q == ' ' || *q == '\n' || *q == '\t')) q++;
                if (q < e && *q == ':') {
                    const char *k = key;
                    const char *t = s;
                    while (t < se && *k && *t == *k) t++, k++;
                    q++;
                    while (q < e && (*q == ' ' || *q == '\n' || *q == '\t')) q++;
                    if (t == se && !*k) return q;
                }
            }
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        p++;
    }
    return 0;
}

/* Decode the JSON string at [p] into [out] (UTF-8, NUL-terminated,
 * truncated to fit). Returns 1 if [p] was a string. */
static inline int wapp_ai__str(const char *p, const char *e, char *out,
                               unsigned cap) {
    unsigned o = 0;
    if (!p || p >= e || *p != '"' || cap == 0) return 0;
    for (p++; p < e && *p != '"'; p++) {
        unsigned cp = (unsigned char)*p;
        if (*p == '\\' && p + 1 < e) {
            char c = *++p;
            if (c == 'n') cp = '\n';
            else if (c == 't') cp = '\t';
            else if (c == 'r') cp = '\r';
            else if (c == 'b') cp = '\b';
            else if (c == 'f') cp = '\f';
            else if (c == 'u' && p + 4 < e) {
                cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = p[i];
                    cp = cp * 16 + (unsigned)(h <= '9'   ? h - '0'
                                              : h <= 'F' ? h - 'A' + 10
                                                         : h - 'a' + 10);
                }
                p += 4;
                /* A surrogate pair is one character. */
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < e && p[1] == '\\' &&
                    p[2] == 'u') {
                    unsigned lo = 0;
                    for (int i = 3; i <= 6; i++) {
                        char h = p[i];
                        lo = lo * 16 + (unsigned)(h <= '9'   ? h - '0'
                                                  : h <= 'F' ? h - 'A' + 10
                                                             : h - 'a' + 10);
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            } else cp = (unsigned char)c;
            if (o + 5 >= cap) break;
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) {
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | (cp >> 18));
                out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            continue;
        }
        if (o + 2 >= cap) break;
        out[o++] = (char)cp;
    }
    out[o] = 0;
    return 1;
}

/* Is [msg] the answer to a wapp_ai_ask? 1: [text] is the AI's reply;
 * -1: [text] is the error; 0: some other message. [id] gets your req_id. */
static inline int wapp_ai_reply(const char *msg, uint32_t n, char *id,
                                unsigned id_cap, char *text,
                                unsigned text_cap) {
    const char *e = msg + n;
    char ty[20];
    static char full[80];
    if (!wapp_ai__str(wapp_ai__get(msg, e, "type"), e, ty, sizeof(ty)))
        return 0;
    const char *want = "widget.response";
    for (unsigned i = 0;; i++) {
        if (ty[i] != want[i]) return 0;
        if (!ty[i]) break;
    }
    if (!wapp_ai__str(wapp_ai__get(msg, e, "req_id"), e, full, sizeof(full)))
        return 0;
    if (full[0] != 'a' || full[1] != 'i' || full[2] != ':') return 0;
    unsigned o = 0;
    for (const char *s = full + 3; *s && o + 1 < id_cap; s++) id[o++] = *s;
    if (id_cap) id[o] = 0;
    const char *err = wapp_ai__get(msg, e, "error");
    if (err && wapp_ai__str(err, e, text, text_cap)) return -1;
    const char *res = wapp_ai__get(msg, e, "result");
    if (res && *res == '{' &&
        wapp_ai__str(wapp_ai__get(res, e, "text"), e, text, text_cap))
        return 1;
    if (text_cap) text[0] = 0;
    return -1;
}

#endif /* WAPP_AI_H */
