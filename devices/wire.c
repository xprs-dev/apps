/*
 * wire.c -- see wire.h. No libc: the wapps build without one, and the
 * native tests compile this same file.
 */
#include "wire.h"

unsigned dv_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

int dv_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int dv_starts(const char *s, const char *p)
{
    if (!s || !p) return 0;
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

void dv_cpy(char *d, const char *s, unsigned cap)
{
    unsigned i = 0;
    if (!cap) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void dv_cat(char *d, const char *s, unsigned cap)
{
    unsigned l = dv_len(d), i = 0;
    while (s && s[i] && l + i < cap - 1) { d[l + i] = s[i]; i++; }
    if (l + i < cap) d[l + i] = 0;
}

void dv_cat_u(char *d, unsigned long long v, unsigned cap)
{
    char b[24];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    char r[24];
    for (int i = 0; i < n; i++) r[i] = b[n - 1 - i];
    r[n] = 0;
    dv_cat(d, r, cap);
}

/* The next number in a version at [*p]: what is not a digit is skipped up
 * to the next '.' or '-', and so is anything after the digits. */
static unsigned long long ver_num(const char **p)
{
    unsigned long long n = 0;
    while (**p && **p != '.' && **p != '-' && (**p < '0' || **p > '9')) (*p)++;
    while (**p >= '0' && **p <= '9') { n = n * 10 + (unsigned)(**p - '0'); (*p)++; }
    while (**p && **p != '.' && **p != '-') (*p)++;
    return n;
}

/* The last number in [s]: "beta.9" is 9, "rc10" is 10. */
static unsigned long long ver_last_num(const char *s)
{
    const char *q = s + dv_len(s);
    while (q > s && (q[-1] < '0' || q[-1] > '9')) q--;
    const char *e = q;
    while (q > s && q[-1] >= '0' && q[-1] <= '9') q--;
    unsigned long long n = 0;
    for (; q < e; q++) n = n * 10 + (unsigned)(*q - '0');
    return n;
}

int dv_ver_cmp(const char *a, const char *b)
{
    if (!a[0] || !b[0]) return a[0] ? 1 : b[0] ? -1 : 0;
    const char *p = a, *q = b;
    if (*p == 'v') p++;
    if (*q == 'v') q++;
    /* The release part: numbers until a '-' (or the end) on either side. */
    while ((*p && *p != '-') || (*q && *q != '-')) {
        unsigned long long x = (*p && *p != '-') ? ver_num(&p) : 0;
        unsigned long long y = (*q && *q != '-') ? ver_num(&q) : 0;
        if (x != y) return x < y ? -1 : 1;
        if (*p == '.') p++;
        if (*q == '.') q++;
    }
    int pa = *p == '-', pb = *q == '-';
    if (pa != pb) return pa ? -1 : 1;          /* a pre-release is older */
    if (!pa) return 0;
    unsigned long long x = ver_last_num(p), y = ver_last_num(q);
    return x < y ? -1 : x > y ? 1 : 0;
}

char dv_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

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

void dv_id(const char *wire, unsigned len, char out[7])
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

void dv_stamp(char *out, unsigned cap, unsigned long long epoch)
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

/* ── A wire's fields ──────────────────────────────────────────────────── */
int dv_field(const char *wire, const char *key, char *out, unsigned cap)
{
    unsigned kl = dv_len(key);
    if (cap) out[0] = 0;
    for (const char *p = wire; p && *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (dv_starts(p, key) && p[kl] == ':') {
            const char *v = p + kl + 1;
            unsigned n = 0;
            int last = dv_eq(key, "m");
            while (v[n] && (last || v[n] != ' ')) n++;
            unsigned c = n < cap - 1 ? n : cap - 1;
            for (unsigned i = 0; i < c; i++) out[i] = v[i];
            out[c] = 0;
            return 1;
        }
        if (dv_starts(p, "m:")) return 0;          /* m: runs to the end */
        while (*p && *p != ' ') p++;
    }
    return 0;
}

void dv_call_of(const char *npub, const char *prefix, char *out, unsigned cap)
{
    dv_cpy(out, prefix, cap);
    if (!dv_starts(npub, "npub1") || dv_len(npub) < 9) { out[0] = 0; return; }
    char four[5];
    for (int i = 0; i < 4; i++) four[i] = dv_up(npub[5 + i]);
    four[4] = 0;
    dv_cat(out, four, cap);
}

int dv_call_matches(const char *call, const char *npub)
{
    if (!call || dv_len(call) < 4 || call[0] != 'X') return 0;
    if (!dv_starts(npub, "npub1") || dv_len(npub) < 10) return 0;
    unsigned n = 0;
    while (call[2 + n] && call[2 + n] != '-') n++;
    if (n < 2 || n > 5) return 0;
    for (unsigned i = 0; i < n; i++)
        if (!npub[5 + i] || dv_up(call[2 + i]) != dv_up(npub[5 + i])) return 0;
    return 1;
}

/* ── Building ─────────────────────────────────────────────────────────── */
static int head(char *out, unsigned cap, const char *me, const char *station,
                const char *ts)
{
    dv_cpy(out, "t:command f:", cap);
    dv_cat(out, me, cap);
    dv_cat(out, " d:", cap);
    dv_cat(out, station, cap);
    dv_cat(out, " ts:", cap);
    dv_cat(out, ts, cap);
    return (int)dv_len(out);
}

static int done(char *out, unsigned cap)
{
    unsigned n = dv_len(out);
    if (n + 1 >= cap || n > FW_WIRE_UNSIGNED) { if (cap) out[0] = 0; return -1; }
    return (int)n;
}

int dv_claim(char *out, unsigned cap, const char *me, const char *station,
             const char *ts, const char *my_npub)
{
    head(out, cap, me, station, ts);
    dv_cat(out, " cmd:set owner:", cap);
    dv_cat(out, me, cap);
    if (my_npub && my_npub[0]) { dv_cat(out, " k:", cap); dv_cat(out, my_npub, cap); }
    return done(out, cap);
}

int dv_set(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *fields)
{
    head(out, cap, me, station, ts);
    dv_cat(out, " cmd:set ", cap);
    dv_cat(out, fields, cap);
    return done(out, cap);
}

int dv_sealed(char *out, unsigned cap, const char *me, const char *station,
              const char *ts, const char *x)
{
    head(out, cap, me, station, ts);
    dv_cat(out, " x:", cap);
    dv_cat(out, x, cap);
    return done(out, cap);
}

int dv_cmd(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *cmd)
{
    head(out, cap, me, station, ts);
    dv_cat(out, " ", cap);
    dv_cat(out, cmd, cap);
    return done(out, cap);
}

int dv_zdiag(char *out, unsigned cap, const char *me, const char *station,
             const char *ts)
{
    return dv_cmd(out, cap, me, station, ts, "cmd:zdiag");
}

int dv_ask(char *out, unsigned cap, const char *me, const char *station,
           const char *ts, const char *what)
{
    dv_cpy(out, "t:request f:", cap);
    dv_cat(out, me, cap);
    dv_cat(out, " d:", cap);
    dv_cat(out, station, cap);
    dv_cat(out, " ts:", cap);
    dv_cat(out, ts, cap);
    dv_cat(out, " q:", cap);
    dv_cat(out, what, cap);
    return done(out, cap);
}

int dv_body(char *out, unsigned cap, const char *const *kv)
{
    dv_cpy(out, "cmd:set", cap);
    for (int i = 0; kv[i]; i += 2) {
        for (const char *v = kv[i + 1]; v && *v; v++)
            if (*v == '\n' || *v == '\r') { out[0] = 0; return -1; }
        dv_cat(out, "\n", cap);
        dv_cat(out, kv[i], cap);
        dv_cat(out, ":", cap);
        dv_cat(out, kv[i + 1], cap);
    }
    unsigned n = dv_len(out);
    if (n + 1 >= cap) { out[0] = 0; return -1; }
    return (int)n;
}

/* ── JSON ─────────────────────────────────────────────────────────────── */
void dv_jesc(char *d, const char *s, unsigned cap)
{
    unsigned l = dv_len(d);
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

int dv_json(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    dv_cat(pat, key, sizeof pat);
    dv_cat(pat, "\":", sizeof pat);
    unsigned pl = dv_len(pat), o = 0;
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!dv_starts(p, pat)) continue;
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

int dv_json_list(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    dv_cat(pat, key, sizeof pat);
    dv_cat(pat, "\":[", sizeof pat);
    unsigned pl = dv_len(pat), o = 0;
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!dv_starts(p, pat)) continue;
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

int dv_json_obj(const char *json, const char *key, char *out, unsigned cap)
{
    char pat[40] = "\"";
    dv_cat(pat, key, sizeof pat);
    dv_cat(pat, "\":", sizeof pat);
    unsigned pl = dv_len(pat);
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!dv_starts(p, pat)) continue;
        p += pl;
        while (*p == ' ') p++;
        return json_object(p, out, cap) > 0;
    }
    return 0;
}

int dv_json_nth(const char *json, const char *key, int n, char *out, unsigned cap)
{
    char pat[40] = "\"";
    dv_cat(pat, key, sizeof pat);
    dv_cat(pat, "\":[", sizeof pat);
    unsigned pl = dv_len(pat);
    if (cap) out[0] = 0;
    for (const char *p = json; p && *p; p++) {
        if (!dv_starts(p, pat)) continue;
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

const char *dv_json_arr(const char *json, const char *key)
{
    char pat[40] = "\"";
    dv_cat(pat, key, sizeof pat);
    dv_cat(pat, "\":[", sizeof pat);
    unsigned pl = dv_len(pat);
    for (const char *p = json; p && *p; p++)
        if (dv_starts(p, pat)) return p + pl;
    return 0;
}

const char *dv_json_next(const char *p, char *out, unsigned cap)
{
    if (cap) out[0] = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == ',' || *p == '\n') p++;
    if (*p != '{') return 0;
    unsigned n = json_object(p, out, cap);
    if (!n) return 0;
    return p + n;
}

/* ── The reading half Things brought: numbers, lists, base64 ─────────── */
unsigned long long dv_num(const char *s)
{
    unsigned long long n = 0;
    for (; s && *s >= '0' && *s <= '9'; s++) n = n * 10 + (unsigned)(*s - '0');
    return n;
}

void dv_put(char *list, const char *key, const char *value, unsigned cap)
{
    if (list[0]) dv_cat(list, " ", cap);
    dv_cat(list, key, cap);
    dv_cat(list, ":", cap);
    dv_cat(list, value, cap);
}

const char *dv_json_top(const char *json)
{
    const char *p = json;
    while (p && (*p == ' ' || *p == '\n')) p++;
    return (p && *p == '[') ? p + 1 : 0;
}

const char *dv_json_next_str(const char *p, char *out, unsigned cap)
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

/* ── Base64, for a picture that travels as a data: URI ────────────────── */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

unsigned dv_b64(const unsigned char *in, unsigned n, char *out, unsigned cap)
{
    /* Standard alphabet with padding: what a data: URI takes. Writes
     * nothing and answers 0 when the whole thing would not fit, because
     * half a picture is not a picture. */
    unsigned need = ((n + 2) / 3) * 4;
    if (!out || cap <= need) return 0;
    unsigned o = 0;
    for (unsigned i = 0; i < n; i += 3) {
        unsigned b = (unsigned)in[i] << 16;
        if (i + 1 < n) b |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) b |= (unsigned)in[i + 2];
        out[o++] = B64[(b >> 18) & 63];
        out[o++] = B64[(b >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(b >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[b & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* Base64 the other way, for what a camera puts in its SDP: the parameter
 * sets a decoder needs before it can make sense of the first picture. */
unsigned dv_b64_dec(const char *in, unsigned char *out, unsigned cap)
{
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned bits = 0, have = 0, n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;
        int v = -1;
        for (int i = 0; i < 64; i++) if (A[i] == *p) { v = i; break; }
        if (v < 0) break;                 /* whitespace, a comma, the end */
        bits = (bits << 6) | (unsigned)v;
        have += 6;
        if (have >= 8) {
            have -= 8;
            if (n >= cap) return 0;
            out[n++] = (unsigned char)((bits >> have) & 0xFF);
        }
    }
    return n;
}
