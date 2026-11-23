/*
 * tools.xprs.linkcard — link cards, for any wapp and for the host's chat box.
 *
 * Somebody shares a link. The people who receive it should see what it is
 * (the page's picture, its title, a few words) without their device ever
 * contacting the site: a preview fetched by each reader tells the site who
 * every reader is and where they are. So the card is made ONCE, by the
 * sender, and travels inside the message as the sender's own words plus an
 * ordinary picture (XPRS.md 7.7.7). This add-on makes it.
 *
 * It provides the functionality `link.card` (wapp-interfaces.md 5):
 *
 *   args:   {"url":"https://..."}
 *   result: {"url","title","summary","site","file"}
 *           file is the picture's media token ("file:<sha>.<ext>"), absent
 *           when the page names none or it could not be had
 *   error:  "off: ..."          link cards are switched off here
 *           "refused: ..."      not a public http(s) address
 *           "unreachable: ..."  the page did not answer, or not with 2xx
 *           "nothing: ..."      the page says nothing about itself
 *           "busy: ..."         too many cards being made at once
 *
 * How:
 *   1. The page, at most its first 1 MB: the host stops reading there
 *      (Range, wapp_engine.dart _rangeCap), and the head is at the start.
 *   2. Its head read by sdk/wapp_page_meta.h.
 *   3. Its picture, at most 1 MB, handed to the media archive with
 *      hal_media_put_bytes. The archive makes the small preview that
 *      travels (MediaPreviews, performance.md 8.14): no image code here.
 *
 * No clock: every step is woken by {"type":"http.done","id":N}
 * (performance.md 8.4). The broker opens this add-on headless for a
 * request and closes it after, so nothing runs while nobody shares a link.
 *
 * On screen (System folder): one switch, Link cards on or off.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hal/xprs_wasm_hal.h"
#include "../sdk/wapp_json.h"
#include "../sdk/wapp_page_meta.h"

#define PAGE_MAX (1024 * 1024) /* YouTube names its title ~850 kB in */
#define IMAGE_MAX (1024 * 1024)
#define SLOTS 4

/* Generic on purpose: the request should say no more about the sender than
 * any browser does. */
static const char *const HDR_PAGE =
    "User-Agent: Mozilla/5.0 (compatible; link preview)\n"
    "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.5\n"
    "Range: bytes=0-1048575";
static const char *const HDR_IMAGE =
    "User-Agent: Mozilla/5.0 (compatible; link preview)\n"
    "Accept: image/jpeg,image/png,image/webp,image/gif;q=0.9\n"
    "Range: bytes=0-1048575";

typedef struct {
    int used;
    int stage;   /* 1 = reading the page, 2 = reading its picture */
    int32_t req; /* the HTTP request in flight */
    sb_t req_id; /* the broker's */
    sb_t url;
    page_meta_t meta;
} slot_t;

static slot_t g_slots[SLOTS];

/* ── Switch ──────────────────────────────────────────────────────────── */

static int cards_on(void) {
    char b[4];
    uint32_t n = hal_kv_get("on", 2, b, sizeof(b) - 1);
    return n == 0 || b[0] != '0';
}

static void show_switch(void) {
    const char *v = cards_on() ? "true" : "false";
    sb_t b = {0};
    sb_puts(&b, "{\"type\":\"ui.field.set\",\"field\":\"cards_on\",\"value\":");
    sb_puts(&b, v);
    sb_putc(&b, '}');
    hal_msg_send(sb_str(&b), (uint32_t)b.n);
    sb_free(&b);
}

/* ── Answers ─────────────────────────────────────────────────────────── */

static void slot_free(slot_t *s) {
    if (s->req >= 0) hal_http_free(s->req);
    sb_free(&s->req_id);
    sb_free(&s->url);
    page_meta_free(&s->meta);
    memset(s, 0, sizeof(*s));
    s->req = -1;
}

static void answer_error(const char *req_id, size_t n, const char *why) {
    sb_t b = {0};
    sb_puts(&b, "{\"type\":\"widget.response\",\"req_id\":");
    sb_json(&b, req_id, n);
    sb_puts(&b, ",\"error\":");
    sb_jsonz(&b, why);
    sb_putc(&b, '}');
    hal_msg_send(sb_str(&b), (uint32_t)b.n);
    sb_free(&b);
}

static void fail(slot_t *s, const char *why) {
    answer_error(sb_str(&s->req_id), s->req_id.n, why);
    slot_free(s);
}

static void answer(slot_t *s, const char *token, size_t tn) {
    sb_t b = {0};
    sb_puts(&b, "{\"type\":\"widget.response\",\"req_id\":");
    sb_json(&b, sb_str(&s->req_id), s->req_id.n);
    sb_puts(&b, ",\"result\":{\"url\":");
    sb_json(&b, sb_str(&s->url), s->url.n);
    sb_puts(&b, ",\"title\":");
    sb_json(&b, sb_str(&s->meta.title), s->meta.title.n);
    sb_puts(&b, ",\"summary\":");
    sb_json(&b, sb_str(&s->meta.summary), s->meta.summary.n);
    sb_puts(&b, ",\"site\":");
    sb_json(&b, sb_str(&s->meta.site), s->meta.site.n);
    if (token && tn) {
        sb_puts(&b, ",\"file\":");
        sb_json(&b, token, tn);
    }
    sb_puts(&b, "}}");
    hal_msg_send(sb_str(&b), (uint32_t)b.n);
    sb_free(&b);
    slot_free(s);
}

/* ── The steps ───────────────────────────────────────────────────────── */

static int32_t get(const char *url, size_t n, const char *headers) {
    return hal_http_request2(0, url, (uint32_t)n, headers,
                             (uint32_t)strlen(headers), 0, 0, 10000);
}

static void start(const char *req_id, size_t rn, const char *url, size_t un) {
    if (!cards_on()) {
        answer_error(req_id, rn, "off: link cards are switched off");
        return;
    }
    slot_t *s = 0;
    for (int i = 0; i < SLOTS; i++)
        if (!g_slots[i].used) { s = &g_slots[i]; break; }
    if (!s) {
        answer_error(req_id, rn, "busy: too many cards being made");
        return;
    }
    memset(s, 0, sizeof(*s));
    s->req = -1;
    sb_putn(&s->url, url, un);
    if (!un || un > 2000 || !url_is_public_http(sb_str(&s->url))) {
        sb_free(&s->url);
        answer_error(req_id, rn, "refused: not a public http(s) address");
        return;
    }
    s->used = 1;
    sb_putn(&s->req_id, req_id, rn);
    s->stage = 1;
    s->req = get(sb_str(&s->url), s->url.n, HDR_PAGE);
    if (s->req < 0) fail(s, "unreachable: the request did not start");
}

static void page_done(slot_t *s, sb_t *body, int status) {
    if (status < 200 || status > 299) {
        fail(s, "unreachable: the page did not answer");
        return;
    }
    if (!page_meta_read(sb_str(body), body->n, sb_str(&s->url), &s->meta)) {
        fail(s, "nothing: the page says nothing about itself");
        return;
    }
    if (!s->meta.image.n) {
        answer(s, 0, 0);
        return;
    }
    s->stage = 2;
    s->req = get(sb_str(&s->meta.image), s->meta.image.n, HDR_IMAGE);
    if (s->req < 0) answer(s, 0, 0); /* the words still make a card */
}

static void image_done(slot_t *s, sb_t *body, int status) {
    char token[160];
    uint32_t tn = 0;
    const char *ext =
        pm_image_ext((const unsigned char *)sb_str(body), body->n);
    /* A body as long as the cap was cut there: a broken picture, so none. */
    if (status >= 200 && status <= 299 && ext && body->n < IMAGE_MAX) {
        tn = hal_media_put_bytes(sb_str(body), (uint32_t)body->n, ext,
                                 (uint32_t)strlen(ext), token,
                                 sizeof(token) - 1);
        if (tn >= sizeof(token)) tn = 0;
    }
    answer(s, token, tn);
}

static void http_done(int32_t id) {
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &g_slots[i];
        if (!s->used || s->req != id) continue;
        sb_t body = {0};
        int status = 0;
        int32_t st = http_take(id, &body, &status);
        if (st == 0) { sb_free(&body); return; } /* not finished after all */
        s->req = -1;
        if (st < 0) status = 0;
        if (s->stage == 1) page_done(s, &body, status);
        else image_done(s, &body, status);
        sb_free(&body);
        return;
    }
}

/* ── Module ──────────────────────────────────────────────────────────── */

void module_init(void) {
    for (int i = 0; i < SLOTS; i++) g_slots[i].req = -1;
    show_switch();
}

void module_tick(void) {}
int32_t module_tick_interval_ms(void) { return 0; }

static int is(const char *v, const char *e, const char *want) {
    sb_t s = {0};
    int ok = v && j_str(v, e, &s) && strcmp(sb_str(&s), want) == 0;
    sb_free(&s);
    return ok;
}

void module_handle_event(void) {
    static char buf[16384];
    while (hal_msg_available() > 0) {
        uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
        if (n == 0) return;
        buf[n] = 0;
        const char *e = buf + n;
        const char *type = j_get(buf, e, "type");
        if (is(type, e, "http.done")) {
            http_done((int32_t)j_num(j_get(buf, e, "id"), e));
            continue;
        }
        if (is(type, e, "widget.request")) {
            sb_t rid = {0}, url = {0};
            j_str(j_get(buf, e, "req_id"), e, &rid);
            const char *args = j_get(buf, e, "args");
            if (args) j_str(j_get(args, e, "url"), e, &url);
            if (!is(j_get(buf, e, "widget"), e, "link.card"))
                answer_error(sb_str(&rid), rid.n, "unsupported: not link.card");
            else
                start(sb_str(&rid), rid.n, sb_str(&url), url.n);
            sb_free(&rid);
            sb_free(&url);
            continue;
        }
        /* The switch: {"command":"cards_apply","fields":{"cards_on":bool}} */
        if (is(j_get(buf, e, "command"), e, "cards_apply")) {
            const char *f = j_get(buf, e, "fields");
            const char *v = f ? j_get(f, e, "cards_on") : 0;
            if (v) {
                v = j_ws(v, e);
                int on = !(v < e && (*v == 'f' || *v == '0'));
                hal_kv_set("on", 2, on ? "1" : "0", 1);
            }
            show_switch();
        }
    }
}

void module_destroy(void) {
    for (int i = 0; i < SLOTS; i++)
        if (g_slots[i].used) slot_free(&g_slots[i]);
}
