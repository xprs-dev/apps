/*
 * wapp_page_meta.h — what a web page says about itself: its title, a short
 * summary, its picture, read from the page's head.
 *
 * The rules are the ones every link preview follows (Open Graph first, then
 * Twitter cards, then JSON-LD, then the plain <title> and description), in
 * a scanner over the head rather than a DOM: the head is the only part
 * read, and it is a few kilobytes. Pure: no HAL, no I/O, so a test feeds it
 * fixture pages directly.
 *
 *   #include "../sdk/wapp_page_meta.h"
 *
 *   page_meta_t m = {0};
 *   page_meta_read(html, n, "https://example.org/a", &m);
 *   // m.title, m.summary, m.site, m.image (absolute URL) — cleaned text
 *   page_meta_free(&m);
 *
 *   url_is_public_http("http://192.168.1.1/")   // 0: never fetched
 *
 * Tested against the fixture pages in apps/linkcard/tests/fixtures.
 */
#ifndef WAPP_PAGE_META_H
#define WAPP_PAGE_META_H

#include "wapp_json.h"

#define PAGE_META_TITLE_MAX 80   /* characters, not bytes */
#define PAGE_META_SUMMARY_MAX 160

typedef struct {
    sb_t title, summary, site, image;
} page_meta_t;

static inline void page_meta_free(page_meta_t *m) {
    sb_free(&m->title);
    sb_free(&m->summary);
    sb_free(&m->site);
    sb_free(&m->image);
}

/* ── Small text helpers ──────────────────────────────────────────────── */

static inline int pm_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}
static inline int pm_ieq(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (pm_lower((unsigned char)a[i]) != pm_lower((unsigned char)b[i]))
            return 0;
    return 1;
}
static inline const char *pm_ifind(const char *p, const char *e,
                                   const char *needle) {
    size_t n = strlen(needle);
    for (; p + n <= e; p++) {
        size_t i = 0;
        while (i < n && pm_lower((unsigned char)p[i]) ==
                            pm_lower((unsigned char)needle[i]))
            i++;
        if (i == n) return p;
    }
    return 0;
}
static inline int pm_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* One character entity at [p] (after '&'), decoded into [out]. Returns the
 * bytes consumed after the '&', or 0 when it is not one we read. */
static inline size_t pm_entity(const char *p, const char *e, sb_t *out) {
    static const struct { const char *name; unsigned cp; } named[] = {
        {"amp", '&'},       {"lt", '<'},         {"gt", '>'},
        {"quot", '"'},      {"apos", '\''},      {"nbsp", ' '},
        {"hellip", 0x2026}, {"mdash", 0x2014},   {"ndash", 0x2013},
        {"rsquo", 0x2019},  {"lsquo", 0x2018},   {"ldquo", 0x201C},
        {"rdquo", 0x201D},  {"middot", 0x00B7},  {"copy", 0x00A9},
        {"reg", 0x00AE},    {"trade", 0x2122},   {"laquo", 0x00AB},
        {"raquo", 0x00BB},  {"eacute", 0x00E9},  {"aacute", 0x00E1},
        {"oacute", 0x00F3}, {"uacute", 0x00FA},  {"iacute", 0x00ED},
        {"ccedil", 0x00E7}, {"atilde", 0x00E3},  {"otilde", 0x00F5},
        {"uuml", 0x00FC},   {"ouml", 0x00F6},    {"auml", 0x00E4},
        {"szlig", 0x00DF},
    };
    const char *q = p;
    while (q < e && q - p < 12 && *q != ';') q++;
    if (q >= e || *q != ';' || q == p) return 0;
    size_t n = (size_t)(q - p);
    if (*p == '#') {
        unsigned cp = 0;
        const char *d = p + 1;
        int hex = d < q && (*d == 'x' || *d == 'X');
        if (hex) d++;
        if (d >= q) return 0;
        for (; d < q; d++) {
            int v = hex ? hexv(*d) : (*d >= '0' && *d <= '9' ? *d - '0' : -1);
            if (v < 0) return 0;
            cp = cp * (hex ? 16 : 10) + (unsigned)v;
            if (cp > 0x10FFFF) return 0;
        }
        if (cp == 0 || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
        utf8_put(out, cp);
        return n + 1;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strlen(named[i].name) == n && memcmp(p, named[i].name, n) == 0) {
            utf8_put(out, named[i].cp);
            return n + 1;
        }
    }
    return 0;
}

/* Length of the valid UTF-8 sequence at [p], or 0. */
static inline size_t pm_utf8_len(const unsigned char *p, const unsigned char *e) {
    unsigned c = p[0];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3
             : (c >> 3) == 30 ? 4 : 0;
    if (!n || p + n > e) return 0;
    for (size_t i = 1; i < n; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return n;
}

/* [in] as one clean line for a card: entities decoded, tags dropped, every
 * run of space (and every control or separator character, U+2028/2029
 * included, since they break a line on the wire) made one space, invalid
 * UTF-8 replaced, trimmed, and cut to [max] characters at a word with "…".
 * [out] is cleared first. */
static inline void pm_clean(const char *in, size_t n, size_t max, sb_t *out) {
    sb_t dec = {0};
    const char *p = in, *e = in + n;
    while (p < e) {
        if (*p == '&') {
            size_t used = pm_entity(p + 1, e, &dec);
            if (used) { p += 1 + used; continue; }
        }
        if (*p == '<') { /* a tag inside a title: drop it */
            const char *q = memchr(p, '>', (size_t)(e - p));
            if (q) { p = q + 1; sb_putc(&dec, ' '); continue; }
        }
        sb_putc(&dec, *p++);
    }
    sb_clear(out);
    const unsigned char *s = (const unsigned char *)sb_str(&dec);
    const unsigned char *se = s + dec.n;
    size_t chars = 0, last_space_at = 0, last_space_chars = 0;
    int pending_space = 0, cut = 0;
    while (s < se) {
        size_t l = pm_utf8_len(s, se);
        unsigned cp = 0;
        if (!l) { cp = 0xFFFD; l = 1; }
        else if (l == 1) cp = s[0];
        else if (l == 3 && s[0] == 0xE2 && s[1] == 0x80 &&
                 (s[2] == 0xA8 || s[2] == 0xA9)) cp = ' ';
        int is_space = cp == ' ' || (cp && cp < 0x20) || cp == 0x7F ||
                       (l == 2 && s[0] == 0xC2 && s[1] == 0xA0);
        if (is_space) {
            if (out->n) pending_space = 1;
            s += l;
            continue;
        }
        if (pending_space) {
            if (chars + 1 > max) { cut = 1; break; }
            last_space_at = out->n;
            last_space_chars = chars;
            sb_putc(out, ' ');
            chars++;
            pending_space = 0;
        }
        if (chars + 1 > max) { cut = 1; break; }
        if (cp == 0xFFFD) utf8_put(out, 0xFFFD);
        else sb_putn(out, (const char *)s, l);
        chars++;
        s += l;
    }
    if (cut) {
        /* Back to the last word boundary when it is not far, then "…". */
        if (last_space_at && chars - last_space_chars < 20) {
            out->n = last_space_at;
            out->p[out->n] = 0;
        } else if (chars >= max) {
            /* make room for the ellipsis: drop one character */
            size_t k = out->n;
            while (k > 0 && (((unsigned char)out->p[k - 1]) & 0xC0) == 0x80) k--;
            if (k > 0) k--;
            out->n = k;
            out->p[k] = 0;
        }
        while (out->n && (out->p[out->n - 1] == ' ' || out->p[out->n - 1] == ',' ||
                          out->p[out->n - 1] == '.' || out->p[out->n - 1] == ';' ||
                          out->p[out->n - 1] == ':'))
            out->p[--out->n] = 0;
        utf8_put(out, 0x2026);
    }
    sb_free(&dec);
}

/* ── URLs ────────────────────────────────────────────────────────────── */

/* The scheme://authority part of [url] (its length), or 0. */
static inline size_t pm_origin_len(const char *url) {
    const char *s = strstr(url, "://");
    if (!s) return 0;
    const char *a = s + 3;
    while (*a && *a != '/' && *a != '?' && *a != '#') a++;
    return (size_t)(a - url);
}

/* The host of [url], lowercased, into [out] (cleared). */
static inline void url_host(const char *url, sb_t *out) {
    sb_clear(out);
    const char *s = strstr(url, "://");
    if (!s) return;
    const char *a = s + 3, *end = url + pm_origin_len(url);
    const char *at = 0;
    for (const char *q = a; q < end; q++)
        if (*q == '@') at = q;
    if (at) a = at + 1;
    if (a < end && *a == '[') { /* IPv6 literal */
        const char *rb = memchr(a, ']', (size_t)(end - a));
        if (rb) end = rb + 1;
    } else {
        const char *colon = memchr(a, ':', (size_t)(end - a));
        if (colon) end = colon;
    }
    for (; a < end; a++) sb_putc(out, (char)pm_lower((unsigned char)*a));
}

/* 1 when [url] is http(s) to a host somebody on the internet could serve:
 * not a private, loopback or link-local address, not a name that only
 * means this machine or this network, and no user:password@ in front
 * (the classic way to make a link read as one site and go to another). */
static inline int url_is_public_http(const char *url) {
    size_t sl = 0;
    if (strncmp(url, "https://", 8) == 0) sl = 8;
    else if (strncmp(url, "http://", 7) == 0) sl = 7;
    else return 0;
    size_t ol = pm_origin_len(url);
    if (ol <= sl) return 0;
    if (memchr(url + sl, '@', ol - sl)) return 0;
    for (const char *c = url; *c; c++)
        if ((unsigned char)*c <= ' ') return 0;
    sb_t h = {0};
    url_host(url, &h);
    const char *s = sb_str(&h);
    size_t n = h.n;
    int ok = n > 0 && s[0] != '[' && strchr(s, '.') != 0;
    if (ok && (pm_ieq(s, n, "localhost") ||
               (n > 6 && pm_ieq(s + n - 6, 6, ".local")) ||
               (n > 10 && pm_ieq(s + n - 10, 10, ".localhost")) ||
               (n > 9 && pm_ieq(s + n - 9, 9, ".internal")) ||
               (n > 4 && pm_ieq(s + n - 4, 4, ".lan"))))
        ok = 0;
    if (ok) {
        unsigned o[4] = {0, 0, 0, 0};
        int parts = 0, digits = 0, numeric = 1;
        for (size_t i = 0; i <= n && numeric; i++) {
            char c = i < n ? s[i] : '.';
            if (c >= '0' && c <= '9') {
                o[parts < 4 ? parts : 3] = o[parts < 4 ? parts : 3] * 10 +
                                           (unsigned)(c - '0');
                digits++;
            } else if (c == '.' && digits) {
                parts++;
                digits = 0;
            } else {
                numeric = 0;
            }
        }
        if (numeric) {
            if (parts != 4 || o[0] > 255 || o[1] > 255 || o[2] > 255 ||
                o[3] > 255)
                ok = 0;
            else if (o[0] == 0 || o[0] == 10 || o[0] == 127 ||
                     (o[0] == 169 && o[1] == 254) ||
                     (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
                     (o[0] == 192 && o[1] == 168) ||
                     (o[0] == 100 && o[1] >= 64 && o[1] <= 127) ||
                     o[0] >= 224)
                ok = 0;
        }
    }
    sb_free(&h);
    return ok;
}

/* [ref] (an href or a content URL from the page) made absolute against
 * the page's [base], into [out] (cleared). Empty when it cannot be. */
static inline void url_resolve(const char *base, const char *ref, size_t rn,
                               sb_t *out) {
    sb_clear(out);
    while (rn && pm_space((unsigned char)*ref)) { ref++; rn--; }
    while (rn && pm_space((unsigned char)ref[rn - 1])) rn--;
    if (!rn) return;
    if ((rn > 7 && pm_ieq(ref, 7, "http://")) ||
        (rn > 8 && pm_ieq(ref, 8, "https://"))) {
        sb_putn(out, ref, rn);
        return;
    }
    size_t ol = pm_origin_len(base);
    if (!ol) return;
    if (rn > 2 && ref[0] == '/' && ref[1] == '/') {
        const char *colon = strchr(base, ':');
        sb_putn(out, base, (size_t)(colon - base) + 1);
        sb_putn(out, ref, rn);
        return;
    }
    {   /* data:, javascript:, mailto: ... never */
        size_t i = 0;
        while (i < rn && ((ref[i] >= 'a' && ref[i] <= 'z') ||
                          (ref[i] >= 'A' && ref[i] <= 'Z')))
            i++;
        if (i > 0 && i < rn && ref[i] == ':') return;
    }
    if (ref[0] == '/') {
        sb_putn(out, base, ol);
        sb_putn(out, ref, rn);
        return;
    }
    /* Relative to the base's directory (its query and fragment dropped). */
    size_t bl = strcspn(base, "?#");
    size_t dir = bl;
    while (dir > ol && base[dir - 1] != '/') dir--;
    if (dir <= ol) {
        sb_putn(out, base, ol);
        sb_putc(out, '/');
    } else {
        sb_putn(out, base, dir);
    }
    sb_putn(out, ref, rn);
}

/* ── The head scanner ────────────────────────────────────────────────── */

/* The value of attribute [name] in the tag body [p, e), or 0 (and *vn). */
static inline const char *pm_attr(const char *p, const char *e,
                                  const char *name, size_t *vn) {
    while (p < e) {
        while (p < e && (pm_space((unsigned char)*p) || *p == '/')) p++;
        const char *ns = p;
        while (p < e && !pm_space((unsigned char)*p) && *p != '=' && *p != '>' &&
               *p != '/')
            p++;
        size_t nl = (size_t)(p - ns);
        while (p < e && pm_space((unsigned char)*p)) p++;
        const char *vs = p, *ve = p;
        if (p < e && *p == '=') {
            p++;
            while (p < e && pm_space((unsigned char)*p)) p++;
            if (p < e && (*p == '"' || *p == '\'')) {
                char q = *p++;
                vs = p;
                while (p < e && *p != q) p++;
                ve = p;
                if (p < e) p++;
            } else {
                vs = p;
                while (p < e && !pm_space((unsigned char)*p) && *p != '>') p++;
                ve = p;
            }
        }
        if (!nl) {
            if (p < e) p++;
            continue;
        }
        if (pm_ieq(ns, nl, name)) {
            *vn = (size_t)(ve - vs);
            return vs;
        }
    }
    return 0;
}

/* Keep [v] in [slot] when the slot is still empty. */
static inline void pm_offer(sb_t *slot, const char *v, size_t n) {
    if (slot->n || !v || !n) return;
    sb_putn(slot, v, n);
}

/* The first string value of "key" in a JSON-LD block, raw (still escaped). */
static inline const char *pm_ld_value(const char *p, const char *e,
                                      const char *key, size_t *vn) {
    char pat[24];
    size_t kl = strlen(key);
    if (kl + 2 >= sizeof(pat)) return 0;
    pat[0] = '"';
    memcpy(pat + 1, key, kl);
    pat[kl + 1] = '"';
    pat[kl + 2] = 0;
    const char *k = pm_ifind(p, e, pat);
    if (!k) return 0;
    const char *q = k + kl + 2;
    while (q < e && (pm_space((unsigned char)*q) || *q == ':')) q++;
    /* An image may be a list, or an object with a url: take that string. */
    if (q < e && *q == '[') {
        q++;
        while (q < e && pm_space((unsigned char)*q)) q++;
    }
    if (q < e && *q == '{') {
        const char *u = pm_ifind(q, e, "\"url\"");
        if (!u) return 0;
        q = u + 5;
        while (q < e && (pm_space((unsigned char)*q) || *q == ':')) q++;
    }
    if (q >= e || *q != '"') return 0;
    const char *s = ++q;
    while (q < e && *q != '"') {
        if (*q == '\\') q++;
        q++;
    }
    if (q >= e) return 0;
    *vn = (size_t)(q - s);
    return s;
}

/* Read [html] (the start of a page fetched from [base]) into [m]. Returns
 * 1 when the page said anything usable (a title or a summary). */
static inline int page_meta_read(const char *html, size_t n, const char *base,
                                 page_meta_t *m) {
    /* Candidates, best first within each list; the first found wins. */
    sb_t og_title = {0}, tw_title = {0}, ld_title = {0}, tag_title = {0};
    sb_t og_desc = {0}, tw_desc = {0}, ld_desc = {0}, meta_desc = {0};
    sb_t og_image = {0}, tw_image = {0}, ld_image = {0}, icon = {0};
    sb_t og_site = {0}, ld_unesc = {0};
    const char *p = html, *e = html + n;
    while (p < e) {
        const char *lt = memchr(p, '<', (size_t)(e - p));
        if (!lt) break;
        p = lt + 1;
        if (p < e && *p == '!') { /* comment or doctype */
            const char *end = (p + 2 < e && p[1] == '-' && p[2] == '-')
                                  ? pm_ifind(p, e, "-->")
                                  : memchr(p, '>', (size_t)(e - p));
            p = end ? end + 1 : e;
            continue;
        }
        const char *ns = p;
        while (p < e && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                         (*p >= '0' && *p <= '9')))
            p++;
        size_t nl = (size_t)(p - ns);
        /* The tag body ends at the first '>' outside quotes. */
        const char *bs = p;
        char q = 0;
        while (p < e && (q || *p != '>')) {
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            p++;
        }
        const char *be = p;
        if (p < e) p++;
        if (pm_ieq(ns, nl, "body")) break;
        if (nl == 4 && pm_ieq(ns, nl, "meta")) {
            size_t kn = 0, vn = 0;
            const char *k = pm_attr(bs, be, "property", &kn);
            if (!k) k = pm_attr(bs, be, "name", &kn);
            if (!k) k = pm_attr(bs, be, "itemprop", &kn);
            const char *v = pm_attr(bs, be, "content", &vn);
            if (!k || !v) continue;
            if (pm_ieq(k, kn, "og:title")) pm_offer(&og_title, v, vn);
            else if (pm_ieq(k, kn, "twitter:title")) pm_offer(&tw_title, v, vn);
            else if (pm_ieq(k, kn, "og:description")) pm_offer(&og_desc, v, vn);
            else if (pm_ieq(k, kn, "twitter:description")) pm_offer(&tw_desc, v, vn);
            else if (pm_ieq(k, kn, "description")) pm_offer(&meta_desc, v, vn);
            else if (pm_ieq(k, kn, "og:image") || pm_ieq(k, kn, "og:image:url") ||
                     pm_ieq(k, kn, "og:image:secure_url"))
                pm_offer(&og_image, v, vn);
            else if (pm_ieq(k, kn, "twitter:image") ||
                     pm_ieq(k, kn, "twitter:image:src"))
                pm_offer(&tw_image, v, vn);
            else if (pm_ieq(k, kn, "og:site_name")) pm_offer(&og_site, v, vn);
        } else if (nl == 5 && pm_ieq(ns, nl, "title")) {
            const char *end = pm_ifind(p, e, "</title");
            if (end) {
                pm_offer(&tag_title, p, (size_t)(end - p));
                p = end;
            }
        } else if (nl == 4 && pm_ieq(ns, nl, "link")) {
            size_t rn = 0, hn = 0;
            const char *rel = pm_attr(bs, be, "rel", &rn);
            const char *href = pm_attr(bs, be, "href", &hn);
            if (!rel || !href || icon.n) continue;
            /* A bitmap icon only: an .ico or an .svg is no picture for a card. */
            if (pm_ifind(rel, rel + rn, "apple-touch-icon") ||
                (pm_ifind(rel, rel + rn, "icon") &&
                 (pm_ifind(href, href + hn, ".png") ||
                  pm_ifind(href, href + hn, ".jpg") ||
                  pm_ifind(href, href + hn, ".jpeg") ||
                  pm_ifind(href, href + hn, ".webp"))))
                pm_offer(&icon, href, hn);
        } else if (nl == 6 && pm_ieq(ns, nl, "script")) {
            size_t tn = 0;
            const char *t = pm_attr(bs, be, "type", &tn);
            const char *end = pm_ifind(p, e, "</script");
            if (!end) break;
            if (t && pm_ifind(t, t + tn, "ld+json")) {
                size_t vn = 0;
                const char *v = pm_ld_value(p, end, "headline", &vn);
                if (v) pm_offer(&ld_title, v, vn);
                v = pm_ld_value(p, end, "description", &vn);
                if (v) pm_offer(&ld_desc, v, vn);
                v = pm_ld_value(p, end, "image", &vn);
                if (v) pm_offer(&ld_image, v, vn);
            }
            p = end;
        } else if (nl == 5 && pm_ieq(ns, nl, "style")) {
            const char *end = pm_ifind(p, e, "</style");
            p = end ? end : e;
        }
    }

    /* JSON-LD strings are JSON-escaped: decode before cleaning. */
    sb_t *ld[] = {&ld_title, &ld_desc, &ld_image};
    for (int i = 0; i < 3; i++) {
        if (!ld[i]->n) continue;
        sb_t raw = {0};
        sb_putc(&raw, '"');
        sb_putn(&raw, ld[i]->p, ld[i]->n);
        sb_putc(&raw, '"');
        sb_clear(&ld_unesc);
        j_str(raw.p, raw.p + raw.n, &ld_unesc);
        sb_clear(ld[i]);
        sb_putn(ld[i], sb_str(&ld_unesc), ld_unesc.n);
        sb_free(&raw);
    }

    sb_t *title = og_title.n ? &og_title : tw_title.n ? &tw_title
                : ld_title.n ? &ld_title : &tag_title;
    sb_t *desc = og_desc.n ? &og_desc : tw_desc.n ? &tw_desc
               : ld_desc.n ? &ld_desc : &meta_desc;
    sb_t *image = og_image.n ? &og_image : tw_image.n ? &tw_image
                : ld_image.n ? &ld_image : &icon;

    pm_clean(sb_str(title), title->n, PAGE_META_TITLE_MAX, &m->title);
    pm_clean(sb_str(desc), desc->n, PAGE_META_SUMMARY_MAX, &m->summary);
    /* The same words twice are no summary. */
    if (m->summary.n && m->summary.n == m->title.n &&
        memcmp(m->summary.p, m->title.p, m->title.n) == 0)
        sb_clear(&m->summary);
    pm_clean(sb_str(&og_site), og_site.n, 40, &m->site);
    if (!m->site.n) url_host(base, &m->site);
    if (image->n) {
        sb_t dec = {0};
        pm_clean(sb_str(image), image->n, 2000, &dec); /* entities */
        url_resolve(base, sb_str(&dec), dec.n, &m->image);
        if (m->image.n && !url_is_public_http(sb_str(&m->image)))
            sb_clear(&m->image);
        sb_free(&dec);
    } else {
        sb_clear(&m->image);
    }

    sb_t *all[] = {&og_title, &tw_title, &ld_title, &tag_title, &og_desc,
                   &tw_desc, &ld_desc, &meta_desc, &og_image, &tw_image,
                   &ld_image, &icon, &og_site, &ld_unesc};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) sb_free(all[i]);
    return m->title.n || m->summary.n;
}

/* The picture type of [b] by its first bytes ("jpg", "png", "gif",
 * "webp"), or 0 when it is not a picture a card can show. */
static inline const char *pm_image_ext(const unsigned char *b, size_t n) {
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpg";
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "png";
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
        return "gif";
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
        return "webp";
    return 0;
}

#endif /* WAPP_PAGE_META_H */
