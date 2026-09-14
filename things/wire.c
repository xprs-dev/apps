/*
 * wire.c -- see wire.h. No libc: the wapps build without one, and the
 * native tests compile this same file.
 */
#include "wire.h"

unsigned th_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

int th_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int th_starts(const char *s, const char *p)
{
    if (!s || !p) return 0;
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

void th_cpy(char *d, const char *s, unsigned cap)
{
    unsigned i = 0;
    if (!cap) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void th_cat(char *d, const char *s, unsigned cap)
{
    unsigned l = th_len(d), i = 0;
    while (s && s[i] && l + i < cap - 1) { d[l + i] = s[i]; i++; }
    if (l + i < cap) d[l + i] = 0;
}

void th_cat_u(char *d, unsigned long long v, unsigned cap)
{
    char b[24];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    char r[24];
    for (int i = 0; i < n; i++) r[i] = b[n - 1 - i];
    r[n] = 0;
    th_cat(d, r, cap);
}

char th_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

unsigned long long th_num(const char *s)
{
    unsigned long long n = 0;
    for (; s && *s >= '0' && *s <= '9'; s++) n = n * 10 + (unsigned)(*s - '0');
    return n;
}

/* ── A wire's fields ──────────────────────────────────────────────────── */
int th_field(const char *wire, const char *key, char *out, unsigned cap)
{
    unsigned kl = th_len(key);
    if (cap) out[0] = 0;
    for (const char *p = wire; p && *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (th_starts(p, key) && p[kl] == ':') {
            const char *v = p + kl + 1;
            unsigned n = 0;
            int last = th_eq(key, "m");
            while (v[n] && (last || v[n] != ' ')) n++;
            unsigned c = n < cap - 1 ? n : cap - 1;
            for (unsigned i = 0; i < c; i++) out[i] = v[i];
            out[c] = 0;
            return 1;
        }
        if (th_starts(p, "m:")) return 0;          /* m: runs to the end */
        while (*p && *p != ' ') p++;
    }
    return 0;
}

void th_put(char *list, const char *key, const char *value, unsigned cap)
{
    if (list[0]) th_cat(list, " ", cap);
    th_cat(list, key, cap);
    th_cat(list, ":", cap);
    th_cat(list, value, cap);
}

/* ── JSON ─────────────────────────────────────────────────────────────── */
void th_jesc(char *d, const char *s, unsigned cap)
{
    unsigned l = th_len(d);
    static const char hx[] = "0123456789abcdef";
    for (; s && *s && l + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { d[l++] = '\\'; d[l++] = (char)c; }
        else if (c == '\n') { d[l++] = '\\'; d[l++] = 'n'; }
        else if (c < 0x20) {
            d[l++] = '\\'; d[l++] = 'u'; d[l++] = '0'; d[l++] = '0';
            d[l++] = hx[c >> 4]; d[l++] = hx[c & 15];
        } else d[l++] = (char)c;
    }
    d[l] = 0;
}

int th_json(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    th_cat(pat, key, sizeof pat);
    th_cat(pat, "\":", sizeof pat);
    unsigned pl = th_len(pat), o = 0;
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!th_starts(p, pat)) continue;
        p += pl;
        while (*p == ' ') p++;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && o < cap - 1) {
                if (*p == '\\' && p[1]) {
                    p++;
                    out[o++] = *p == 'n' ? '\n' : *p;
                    p++;
                    continue;
                }
                out[o++] = *p++;
            }
        } else {
            while (*p && *p != ',' && *p != '}' && *p != ']' && o < cap - 1) out[o++] = *p++;
        }
        out[o] = 0;
        return 1;
    }
    return 0;
}

/* The text of one balanced {..} starting at p, strings skipped. Returns the
 * length consumed, 0 when p is not an object or it does not fit. */
static unsigned json_object(const char *p, char *out, unsigned cap)
{
    if (*p != '{') return 0;
    unsigned depth = 0, o = 0;
    int instr = 0;
    for (const char *s = p; *s; s++) {
        if (o + 2 >= cap) { out[0] = 0; return 0; }
        out[o++] = *s;
        if (instr) {
            if (*s == '\\' && s[1]) { out[o++] = *++s; continue; }
            if (*s == '"') instr = 0;
            continue;
        }
        if (*s == '"') instr = 1;
        else if (*s == '{') depth++;
        else if (*s == '}' && --depth == 0) { out[o] = 0; return (unsigned)(s - p) + 1; }
    }
    out[0] = 0;
    return 0;
}

int th_json_obj(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    th_cat(pat, key, sizeof pat);
    th_cat(pat, "\":", sizeof pat);
    unsigned pl = th_len(pat);
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!th_starts(p, pat)) continue;
        p += pl;
        while (*p == ' ') p++;
        return json_object(p, out, cap) > 0;
    }
    return 0;
}

const char *th_json_arr(const char *json, const char *key)
{
    char pat[40] = "\"";
    th_cat(pat, key, sizeof pat);
    th_cat(pat, "\":[", sizeof pat);
    unsigned pl = th_len(pat);
    for (const char *p = json; p && *p; p++)
        if (th_starts(p, pat)) return p + pl;
    return 0;
}

const char *th_json_top(const char *json)
{
    const char *p = json;
    while (p && (*p == ' ' || *p == '\n')) p++;
    return (p && *p == '[') ? p + 1 : 0;
}

const char *th_json_next(const char *p, char *out, unsigned cap)
{
    if (cap) out[0] = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == ',' || *p == '\n') p++;
    if (*p != '{') return 0;
    unsigned n = json_object(p, out, cap);
    if (!n) return 0;
    return p + n;
}

const char *th_json_next_str(const char *p, char *out, unsigned cap)
{
    if (cap) out[0] = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == ',' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    unsigned o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        if (o + 1 < cap) out[o++] = *p;
        p++;
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return *p ? p + 1 : 0;
}
