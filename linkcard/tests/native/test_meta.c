/*
 * Native tests for the link-card reader (sdk/wapp_page_meta.h): every page
 * in ../fixtures read as index.txt says, compared with its .expect file,
 * plus the address rules. Run: sh tests/native/run.sh
 *
 * A .expect file is five lines:  ok=1  title=...  summary=...  site=...
 * image=...
 */
#include <stdio.h>

#include "../../../sdk/wapp_page_meta.h"

static int g_pass, g_fail;

static void check(int cond, const char *what) {
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("FAIL: %s\n", what);
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void fixtures(const char *dir, int update) {
    char path[1024];
    snprintf(path, sizeof path, "%s/index.txt", dir);
    size_t n = 0;
    char *idx = slurp(path, &n);
    check(idx != 0, "index.txt readable");
    if (!idx) return;
    for (char *line = strtok(idx, "\n"); line; line = strtok(0, "\n")) {
        if (line[0] == '#' || !line[0]) continue;
        char name[64], base[512];
        if (sscanf(line, "%63s %511s", name, base) != 2) continue;
        snprintf(path, sizeof path, "%s/%s.html", dir, name);
        size_t hn = 0;
        char *html = slurp(path, &hn);
        check(html != 0, name);
        if (!html) continue;
        page_meta_t m = {0};
        int ok = page_meta_read(html, hn, base, &m);
        sb_t got = {0};
        sb_puts(&got, ok ? "ok=1\n" : "ok=0\n");
        sb_puts(&got, "title="); sb_puts(&got, sb_str(&m.title)); sb_putc(&got, '\n');
        sb_puts(&got, "summary="); sb_puts(&got, sb_str(&m.summary)); sb_putc(&got, '\n');
        sb_puts(&got, "site="); sb_puts(&got, sb_str(&m.site)); sb_putc(&got, '\n');
        sb_puts(&got, "image="); sb_puts(&got, sb_str(&m.image)); sb_putc(&got, '\n');
        snprintf(path, sizeof path, "%s/%s.expect", dir, name);
        if (update) {
            FILE *f = fopen(path, "wb");
            fwrite(got.p, 1, got.n, f);
            fclose(f);
            printf("--- %s\n%s", name, sb_str(&got));
        } else {
            size_t en = 0;
            char *want = slurp(path, &en);
            int same = want && en == got.n && memcmp(want, got.p, en) == 0;
            if (!same) printf("--- %s: got\n%s--- want\n%s", name, sb_str(&got),
                              want ? want : "(no .expect)\n");
            check(same, name);
            free(want);
        }
        sb_free(&got);
        page_meta_free(&m);
        free(html);
    }
    free(idx);
}

static void addresses(void) {
    static const struct { const char *url; int ok; } cases[] = {
        {"https://example.com/a", 1},
        {"http://news.example.co.uk/x?y=1", 1},
        {"https://8.8.8.8/", 1},
        {"ftp://example.com/", 0},
        {"javascript:alert(1)", 0},
        {"https://localhost/", 0},
        {"http://printer.local/", 0},
        {"http://router.lan/", 0},
        {"http://intranet/", 0},           /* no dot: a name only this network knows */
        {"http://127.0.0.1:8080/", 0},
        {"http://10.1.2.3/", 0},
        {"http://172.20.0.1/", 0},
        {"http://172.32.0.1/", 1},
        {"http://192.168.1.1/", 0},
        {"http://169.254.169.254/latest/", 0},
        {"http://100.100.1.1/", 0},
        {"http://0.0.0.0/", 0},
        {"http://[::1]/", 0},
        {"https://bank.example.com@evil.example.net/", 0},
        {"https://exa mple.com/", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char what[160];
        snprintf(what, sizeof what, "url_is_public_http(%s) == %d", cases[i].url,
                 cases[i].ok);
        check(url_is_public_http(cases[i].url) == cases[i].ok, what);
    }
    static const struct { const char *base, *ref, *want; } res[] = {
        {"https://a.example/x/y.html?q=1", "img.png", "https://a.example/x/img.png"},
        {"https://a.example/x/y.html", "/img.png", "https://a.example/img.png"},
        {"https://a.example", "img.png", "https://a.example/img.png"},
        {"https://a.example/x/", "//cdn.example/i.jpg", "https://cdn.example/i.jpg"},
        {"http://a.example/", "HTTPS://b.example/i.jpg", "HTTPS://b.example/i.jpg"},
        {"https://a.example/", "data:image/png;base64,AAAA", ""},
        {"https://a.example/", "  ", ""},
    };
    for (size_t i = 0; i < sizeof res / sizeof res[0]; i++) {
        sb_t out = {0};
        url_resolve(res[i].base, res[i].ref, strlen(res[i].ref), &out);
        char what[256];
        snprintf(what, sizeof what, "url_resolve(%s, %s) = '%s', want '%s'",
                 res[i].base, res[i].ref, sb_str(&out), res[i].want);
        check(strcmp(sb_str(&out), res[i].want) == 0, what);
        sb_free(&out);
    }
    static const unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    check(strcmp(pm_image_ext(jpg, 4), "jpg") == 0, "jpeg sniffed");
    check(pm_image_ext((const unsigned char *)"<html>", 6) == 0, "html is no picture");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures";
    int update = getenv("UPDATE") != 0;
    fixtures(dir, update);
    addresses();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
