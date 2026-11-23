/*
 * wapp_json.h — a growable string, a small JSON reader and the end of an
 * HTTP request, for any wapp written in C.
 *
 * Lifted out of the AI add-on (tools.xprs.ai) so the link-card add-on and
 * every later wapp include it instead of carrying a copy. Everything is
 * `static inline`: a wapp pays only for what it calls, and -Werror's
 * unused-function check stays quiet for the rest.
 *
 *   #include "../sdk/wapp_json.h"
 *
 *   sb_t b = {0};                     // growable, NUL-terminated
 *   sb_puts(&b, "{\"k\":"); sb_jsonz(&b, value); sb_putc(&b, '}');
 *   const char *v = j_get(msg, msg + n, "args");   // walk to a value
 *   j_str(j_get(v, msg + n, "url"), msg + n, &out); // decode a string
 *   http_take(req, &body, &status);   // on {"type":"http.done","id":req}
 *
 * Needs libc (malloc/realloc/memcpy) and the HAL header for http_take.
 */
#ifndef WAPP_JSON_H
#define WAPP_JSON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hal/xprs_wasm_hal.h"

/* ── Growable string ─────────────────────────────────────────────────── */

typedef struct {
    char *p;
    size_t n, cap;
} sb_t;

static inline void sb_reserve(sb_t *b, size_t more) {
    if (b->n + more + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + more + 1) cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np) return;
    b->p = np;
    b->cap = cap;
}
static inline void sb_putn(sb_t *b, const char *s, size_t n) {
    sb_reserve(b, n);
    if (b->n + n + 1 > b->cap) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static inline void sb_puts(sb_t *b, const char *s) { sb_putn(b, s, strlen(s)); }
static inline void sb_putc(sb_t *b, char c) { sb_putn(b, &c, 1); }
static inline void sb_clear(sb_t *b) {
    b->n = 0;
    if (b->p) b->p[0] = 0;
}
static inline void sb_free(sb_t *b) {
    free(b->p);
    b->p = 0;
    b->n = b->cap = 0;
}
static inline const char *sb_str(const sb_t *b) { return b->p ? b->p : ""; }

/* A JSON string literal, quotes included. */
static inline void sb_json(sb_t *b, const char *s, size_t n) {
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
static inline void sb_jsonz(sb_t *b, const char *s) { sb_json(b, s, strlen(s)); }

/* ── A small JSON reader: enough to walk to a value and decode strings ── */

static inline const char *j_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}
static inline const char *j_skip(const char *p, const char *e);
static inline const char *j_skip_str(const char *p, const char *e) {
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
static inline const char *j_skip(const char *p, const char *e) {
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
static inline const char *j_get(const char *p, const char *e, const char *key) {
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

static inline void utf8_put(sb_t *b, unsigned cp) {
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
static inline int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static inline unsigned u4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p[i]);
        if (h < 0) return 0xFFFD;
        v = v * 16 + (unsigned)h;
    }
    return v;
}

/* Append the decoded string at [p] to [out]; 1 if it was a string. */
static inline int j_str(const char *p, const char *e, sb_t *out) {
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

/* A number at [p] (digits only; 0 when none). */
static inline long j_num(const char *p, const char *e) {
    long v = 0;
    if (!p) return 0;
    p = j_ws(p, e);
    while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

/* A finished request's status and body, and the handle freed. Returns
 * hal_http_poll's answer: 0 while still running (nothing taken). */
static inline int32_t http_take(int32_t req, sb_t *body, int *status) {
    int32_t st = hal_http_poll(req);
    if (st == 0) return 0;
    *status = hal_http_status(req);
    if (st > 0) {
        char buf[8192];
        for (;;) {
            int32_t n = hal_http_read_response(req, buf, sizeof(buf));
            if (n <= 0) break;
            sb_putn(body, buf, (size_t)n);
        }
    }
    hal_http_free(req);
    return st;
}

#endif /* WAPP_JSON_H */
