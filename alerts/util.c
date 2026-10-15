/*
 * util.c -- see util.h. Strings, the section 5 identifier, time and the
 * little JSON the host speaks, with no libc: the wapps build without one and
 * the native tests compile this same file. Copied from the Firmwares wapp's
 * wire.c, because a wapp ships its own sources.
 */
#include "util.h"

unsigned al_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

int al_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int al_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int al_starts(const char *s, const char *p)
{
    if (!s || !p) return 0;
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

void al_cpy(char *d, const char *s, unsigned cap)
{
    unsigned i = 0;
    if (!cap) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void al_cat(char *d, const char *s, unsigned cap)
{
    unsigned l = al_len(d), i = 0;
    while (s && s[i] && l + i < cap - 1) { d[l + i] = s[i]; i++; }
    if (l + i < cap) d[l + i] = 0;
}

void al_cat_u(char *d, unsigned long long v, unsigned cap)
{
    char b[24];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    char r[24];
    for (int i = 0; i < n; i++) r[i] = b[n - 1 - i];
    r[n] = 0;
    al_cat(d, r, cap);
}

char al_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* ── SHA-256, for the section 5 identifier ────────────────────────────── */
static const unsigned int k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(unsigned int h[8], const unsigned char b[64])
{
    unsigned int w[64];
    for (int t = 0; t < 16; t++)
        w[t] = ((unsigned int)b[t*4] << 24) | ((unsigned int)b[t*4+1] << 16) |
               ((unsigned int)b[t*4+2] << 8) | b[t*4+3];
    for (int t = 16; t < 64; t++) {
        unsigned int s0 = ROR(w[t-15], 7) ^ ROR(w[t-15], 18) ^ (w[t-15] >> 3);
        unsigned int s1 = ROR(w[t-2], 17) ^ ROR(w[t-2], 19) ^ (w[t-2] >> 10);
        w[t] = w[t-16] + s0 + w[t-7] + s1;
    }
    unsigned int a=h[0], bb=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
    for (int t = 0; t < 64; t++) {
        unsigned int t1 = hh + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) +
                          ((e & f) ^ (~e & g)) + k256[t] + w[t];
        unsigned int t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) +
                          ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void al_id(const char *wire, unsigned len, char out[7])
{
    unsigned int h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                          0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    const unsigned char *m = (const unsigned char *)wire;
    unsigned i = 0;
    while (len - i >= 64) { sha_block(h, m + i); i += 64; }
    unsigned char blk[64];
    unsigned n = 0;
    while (i < len) blk[n++] = m[i++];
    blk[n++] = 0x80;
    if (n > 56) { while (n < 64) blk[n++] = 0; sha_block(h, blk); n = 0; }
    while (n < 56) blk[n++] = 0;
    unsigned long long bits = (unsigned long long)len * 8;
    for (int t = 7; t >= 0; t--) blk[n++] = (unsigned char)(bits >> (t * 8));
    sha_block(h, blk);
    static const char hx[] = "0123456789abcdef";
    for (int t = 0; t < 3; t++) {
        unsigned char byte = (unsigned char)(h[0] >> (24 - 8 * t));
        out[t*2] = hx[byte >> 4];
        out[t*2+1] = hx[byte & 15];
    }
    out[6] = 0;
}

/* ── Time ─────────────────────────────────────────────────────────────── */
static void two(char *d, int v) { d[0] = (char)('0' + (v / 10) % 10); d[1] = (char)('0' + v % 10); }

void al_stamp(char *out, unsigned cap, unsigned long long epoch)
{
    if (cap < 20) { if (cap) out[0] = 0; return; }
    long z = (long)(epoch / 86400ULL) + 719468;
    int secs = (int)(epoch % 86400ULL);
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;
    out[0] = (char)('0' + (y / 1000) % 10); out[1] = (char)('0' + (y / 100) % 10);
    out[2] = (char)('0' + (y / 10) % 10);   out[3] = (char)('0' + y % 10);
    out[4] = '-'; two(out + 5, m); out[7] = '-'; two(out + 8, d);
    out[10] = '_'; two(out + 11, secs / 3600); out[13] = ':';
    two(out + 14, (secs / 60) % 60); out[16] = ':'; two(out + 17, secs % 60);
    out[19] = 0;
}

/* The epoch of a YYYY-MM-DD_HH:MM:SS stamp (section 4.8), 0 when it is
 * not one. The inverse of al_stamp. */
unsigned long long al_epoch(const char *ts)
{
    if (!ts || al_len(ts) < 19 || ts[4] != '-' || ts[7] != '-' || ts[10] != '_' ||
        ts[13] != ':' || ts[16] != ':') return 0;
    int v[6], at[6] = { 0, 5, 8, 11, 14, 17 }, wd[6] = { 4, 2, 2, 2, 2, 2 };
    for (int i = 0; i < 6; i++) {
        v[i] = 0;
        for (int k = 0; k < wd[i]; k++) {
            char c = ts[at[i] + k];
            if (c < '0' || c > '9') return 0;
            v[i] = v[i] * 10 + (c - '0');
        }
    }
    long y = v[0], m = v[1], d = v[2];
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned long yoe = (unsigned long)(y - era * 400);
    unsigned long doy = (unsigned long)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    if (days < 0) return 0;
    return (unsigned long long)days * 86400ULL + (unsigned long long)(v[3] * 3600 + v[4] * 60 + v[5]);
}

/* ── Reading ──────────────────────────────────────────────────────────── */
int al_field(const char *wire, const char *key, char *out, unsigned cap)
{
    unsigned kl = al_len(key);
    if (cap) out[0] = 0;
    for (const char *p = wire; p && *p; ) {
        if (al_starts(p, key) && p[kl] == ':') {
            const char *v = p + kl + 1;
            unsigned n = 0;
            int last = al_eq(key, "m");
            while (v[n] && (last || v[n] != ' ')) n++;
            unsigned c = n < cap - 1 ? n : cap - 1;
            for (unsigned i = 0; i < c; i++) out[i] = v[i];
            out[c] = 0;
            return 1;
        }
        if (al_starts(p, "m:")) return 0;          /* m: runs to the end */
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    return 0;
}

/* ── JSON ─────────────────────────────────────────────────────────────── */
void al_jesc(char *d, const char *s, unsigned cap)
{
    unsigned l = al_len(d);
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

int al_json(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    al_cat(pat, key, sizeof pat);
    al_cat(pat, "\":", sizeof pat);
    unsigned pl = al_len(pat), o = 0;
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!al_starts(p, pat)) continue;
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

int al_json_list(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    al_cat(pat, key, sizeof pat);
    al_cat(pat, "\":[", sizeof pat);
    unsigned pl = al_len(pat), o = 0;
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!al_starts(p, pat)) continue;
        p += pl;
        while (*p && *p != ']' && o < cap - 1) {
            if (*p == '"') { p++; continue; }
            if (*p == ',') {
                if (o + 2 < cap - 1) { out[o++] = ','; out[o++] = ' '; }
                p++;
                continue;
            }
            out[o++] = *p++;
        }
        out[o] = 0;
        return 1;
    }
    return 0;
}

/* The text of one balanced {..} starting at p, strings skipped. Returns the
 * length copied, 0 when p is not an object or it does not fit. */
static unsigned json_object(const char *p, char *out, unsigned cap)
{
    if (*p != '{') return 0;
    unsigned depth = 0, o = 0;
    int instr = 0;
    for (; *p; p++) {
        if (o + 1 >= cap) { out[0] = 0; return 0; }
        out[o++] = *p;
        if (instr) {
            if (*p == '\\' && p[1]) { out[o++] = *++p; continue; }
            if (*p == '"') instr = 0;
            continue;
        }
        if (*p == '"') instr = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) { out[o] = 0; return o; }
    }
    out[0] = 0;
    return 0;
}

int al_json_obj(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    al_cat(pat, key, sizeof pat);
    al_cat(pat, "\":", sizeof pat);
    unsigned pl = al_len(pat);
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!al_starts(p, pat)) continue;
        p += pl;
        while (*p == ' ') p++;
        return json_object(p, out, cap) > 0;
    }
    return 0;
}

int al_json_nth(const char *json, const char *key, int n, char *out, unsigned cap)
{
    char pat[40] = "\"";
    al_cat(pat, key, sizeof pat);
    al_cat(pat, "\":[", sizeof pat);
    unsigned pl = al_len(pat);
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!al_starts(p, pat)) continue;
        p += pl;
        for (int i = 0; ; i++) {
            while (*p == ' ' || *p == ',') p++;
            if (*p != '{') return 0;
            if (i == n) return json_object(p, out, cap) > 0;
            /* Skip this object the same way, into nowhere. */
            unsigned depth = 0;
            int instr = 0;
            for (; *p; p++) {
                if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
                if (*p == '"') instr = 1;
                else if (*p == '{') depth++;
                else if (*p == '}' && --depth == 0) { p++; break; }
            }
        }
    }
    return 0;
}

const char *al_json_arr(const char *json, const char *key)
{
    char pat[40] = "\"";
    al_cat(pat, key, sizeof pat);
    al_cat(pat, "\":[", sizeof pat);
    unsigned pl = al_len(pat);
    for (const char *p = json; p && *p; p++)
        if (al_starts(p, pat)) return p + pl;
    return 0;
}

const char *al_json_next(const char *p, char *out, unsigned cap)
{
    if (cap) out[0] = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == ',') p++;
    if (*p != '{') return 0;
    unsigned n = json_object(p, out, cap);
    if (!n) return 0;
    return p + n;
}
