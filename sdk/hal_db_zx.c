/*
 * hal_db_* for a wapp's NATIVE tests, on the real database: zx.
 *
 * The host answers hal_db_* with a zx store (app/lib/wapp/wapp_engine.dart).
 * A native test is C and cannot link that engine, so this file starts the
 * bridge in sdk/zxdb_bridge (a few lines of Dart on package:zx that open,
 * name and convert exactly as the host does) and talks to it over two pipes,
 * one JSON line per call. A statement that works here works in the app,
 * because it ran on the same engine. There is no SQLite anywhere: link this
 * file instead of writing a hal_db mock.
 *
 * The run script sets ZXDB_BRIDGE to the command that starts the bridge,
 *
 *   ZXDB_BRIDGE="dart $SDK/zxdb_bridge/bin/zxdb_bridge.dart"
 *
 * after `dart pub get` once in sdk/zxdb_bridge. Stores are files under a
 * root, the test's stand-in for the wapp's data directory: ZXDB_ROOT, or
 * whatever the test sets with hal_db_zx_root() (a simulator gives each node
 * its own). The run script empties it first.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *g_to, *g_from;
static char *g_line;
static size_t g_line_cap;
static char g_err[512];
static char g_root[400];
static uint32_t g_cap_limit;
static const char *g_last_sql;   /* the statement in flight, for ZXDB_TRACE */
static uint32_t g_last_sl;

/* Where the next hal_db_open puts its store. */
void hal_db_zx_root(const char *root) { snprintf(g_root, sizeof g_root, "%s", root); }

/* A test can make every query answer -2 above [cap] bytes, as a small
 * buffer at the host would. 0 turns it off. */
void hal_db_zx_query_cap(uint32_t cap) { g_cap_limit = cap; }

static int bridge_start(void)
{
    if (g_to) return 0;
    const char *cmd = getenv("ZXDB_BRIDGE");
    if (!cmd || !*cmd) {
        fprintf(stderr, "hal_db_zx: ZXDB_BRIDGE is not set\n");
        return -1;
    }
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]);
        close(out[0]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    g_to = fdopen(in[1], "w");
    g_from = fdopen(out[0], "r");
    return g_to && g_from ? 0 : -1;
}

/* A JSON string of [s], [n] bytes, onto the bridge. */
static void put_str(const char *s, uint32_t n)
{
    fputc('"', g_to);
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', g_to); fputc(c, g_to); }
        else if (c == '\n') fputs("\\n", g_to);
        else if (c == '\r') fputs("\\r", g_to);
        else if (c == '\t') fputs("\\t", g_to);
        else if (c < 0x20) fprintf(g_to, "\\u%04x", c);
        else fputc(c, g_to);
    }
    fputc('"', g_to);
}

/* Send the line written so far, read the answer into g_line. 0 when ok. */
static int finish(void)
{
    fputs("}\n", g_to);
    fflush(g_to);
    ssize_t n = getline(&g_line, &g_line_cap, g_from);
    if (n <= 0) {
        snprintf(g_err, sizeof g_err, "the zx bridge went away");
        return -1;
    }
    if (g_line[n - 1] == '\n') g_line[--n] = 0;
    {   /* ZXDB_TRACE=2 prints every statement and its answer. */
        const char *t = getenv("ZXDB_TRACE");
        if (t && t[0] == '2' && g_last_sql)
            fprintf(stderr, "zx> %.*s\nzx< %.300s\n", (int)g_last_sl, g_last_sql, g_line);
    }
    if (strncmp(g_line, "{\"ok\":true", 10) == 0) return 0;
    const char *e = strstr(g_line, "\"error\":\"");
    /* Kept for hal_db_error, not printed: a wapp may expect an error (an
     * ALTER TABLE that adds a column twice) and the host stays quiet too. */
    snprintf(g_err, sizeof g_err, "%s", e ? e + 9 : g_line);
    size_t k = strlen(g_err);
    if (k >= 2 && g_err[k - 1] == '}' && g_err[k - 2] == '"') g_err[k - 2] = 0;
    /* ZXDB_TRACE=1 prints each failing statement, for finding out why. */
    if (getenv("ZXDB_TRACE") && g_last_sql)
        fprintf(stderr, "hal_db (zx): %s\n  in: %.*s\n", g_err, (int)g_last_sl, g_last_sql);
    return -1;
}

int32_t hal_db_open(const char *path, uint32_t len)
{
    if (!len || path[0] == '/' || bridge_start()) return -1;
    /* The host refuses an escape from the data directory; so does this. */
    for (uint32_t i = 0; i + 1 < len; i++)
        if (path[i] == '.' && path[i + 1] == '.' && (i == 0 || path[i - 1] == '/')) return -1;
    if (!g_root[0]) {
        const char *r = getenv("ZXDB_ROOT");
        hal_db_zx_root(r && *r ? r : "/tmp/wapp_native_db");
    }
    char full[1024];
    snprintf(full, sizeof full, "%s/%.*s", g_root, (int)len, path);
    fputs("{\"op\":\"open\",\"path\":", g_to);
    put_str(full, (uint32_t)strlen(full));
    if (finish()) return -1;
    const char *h = strstr(g_line, "\"h\":");
    return h ? (int32_t)atoi(h + 4) : -1;
}

static void head(const char *op, int32_t h, const char *sql, uint32_t sl,
                 const char *params, uint32_t pl)
{
    g_last_sql = sql;
    g_last_sl = sl;
    fprintf(g_to, "{\"op\":\"%s\",\"h\":%d,\"sql\":", op, (int)h);
    put_str(sql, sl);
    fputs(",\"params\":", g_to);
    if (params && pl) fwrite(params, 1, pl, g_to);
    else fputs("[]", g_to);
}

int32_t hal_db_exec(int32_t h, const char *sql, uint32_t sl,
                    const char *params, uint32_t pl)
{
    if (h <= 0 || !g_to) return -1;
    head("exec", h, sql, sl, params, pl);
    return finish();
}

int32_t hal_db_query(int32_t h, const char *sql, uint32_t sl,
                     const char *params, uint32_t pl, char *out, uint32_t cap)
{
    if (h <= 0 || !g_to) return -1;
    head("query", h, sql, sl, params, pl);
    if (finish()) return -1;
    /* {"ok":true,"rows":[...]}: the array, as the host hands it back. */
    const char *rows = strstr(g_line, "\"rows\":");
    if (!rows) return -1;
    rows += 7;
    size_t n = strlen(rows);
    if (n && rows[n - 1] == '}') n--;
    if (g_cap_limit && n > g_cap_limit) return -2;
    if (n > cap) return -2;
    memcpy(out, rows, n);
    return (int32_t)n;
}

uint32_t hal_db_error(int32_t h, char *out, uint32_t cap)
{
    (void)h;
    uint32_t n = (uint32_t)strlen(g_err);
    if (n > cap) n = cap;
    memcpy(out, g_err, n);
    return n;
}

void hal_db_close(int32_t h)
{
    if (h <= 0 || !g_to) return;
    fprintf(g_to, "{\"op\":\"close\",\"h\":%d", (int)h);
    finish();
}
