/*
 * The archiver wapp's screens, driven by a mock HAL.
 *
 * What these are about: the Archive tab must send the operator's choice to the
 * core and decide nothing itself, and it must never show a state the station
 * is not in. Before the tiers, "Enable" on the first tab governed the FILE
 * store while the station's archiver role lived in three other preferences —
 * so a device announcing serve:archive showed the role as off, and no screen
 * anywhere mentioned the callsigns you follow.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* hal_mock.c */
void cap_clear(void);
int  cap_count(void);
int  cap_contains(const char *s);
void inbox_set(const char *json);
void prefs_clear(void);
int  pref_written(const char *kv);
int  prefs_count(void);
void archive_set_public(int on);
void archive_set_always(int on);
void event_push(const char *topic);

/* main.c */
int32_t module_init(void);
int32_t module_tick(void);
int32_t module_handle_event(void);

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *what) {
    if (ok) { g_pass++; printf("  [PASS] %s\n", what); }
    else    { g_fail++; printf("  [FAIL] %s\n", what); }
}

int main(void) {
    printf("archiver native test\n");

    /* ── a private phone: the tiers are drawn, the promise is greyed ── */
    archive_set_public(0);
    archive_set_always(0);
    module_init();
    check(cap_contains("\"field\":\"archive_dashboard\""),
          "boot draws the packet archive");
    check(cap_contains("\"label\":\"Mine\""), "the tier that is always kept");
    check(cap_contains("\"label\":\"People I follow\""),
          "the tier a pocket phone exists for");
    check(cap_contains("\"label\":\"Strangers\""), "and the one you opt into");
    check(cap_contains("not kept"),
          "a private station says so rather than showing a count");
    check(cap_contains("\"field\":\"always_on__readonly\""),
          "the always-on switch reports whether it can be used");
    check(cap_contains("\"value\":true"),
          "and it cannot, while this station is private");
    check(cap_contains("\"field\":\"dashboard\""), "the files tab still draws");
    check(cap_contains("\"field\":\"dir_dashboard\""), "and the directory");
    check(cap_contains("\"field\":\"myarch\""), "and my archivers");
    check(!cap_contains("super"),
          "the word 'super' appears nowhere: section 13's serve: vocabulary "
          "has archive and nothing above it");
    /* A tile half a phone screen wide renders 154654 as "15…", which is not a
     * number. Anything past four digits is said the way a person would. */
    check(cap_contains("\"value\":\"154k\""),
          "a six-figure count is said in thousands, not truncated");
    check(cap_contains("\"value\":\"7\""),
          "a small one is left exactly as it is");

    /* ── the operator opts in ── */
    cap_clear(); prefs_clear();
    inbox_set("{\"command\":\"public_changed\",\"public\":true}");
    module_handle_event();
    check(pref_written("public=1"), "becoming a public archiver reaches the core");
    check(prefs_count() == 1, "and nothing else is written");

    /* ── always-on is refused until then, and accepted after ── */
    cap_clear(); prefs_clear();
    inbox_set("{\"command\":\"always_on_changed\",\"always_on\":true,"
              "\"public\":false}");
    module_handle_event();
    check(prefs_count() == 0,
          "always-on while private sends nothing: it is a promise only a "
          "public archiver can make");
    check(cap_contains("\"field\":\"always_on\""), "and the switch is put back");

    cap_clear(); prefs_clear();
    archive_set_public(1);
    inbox_set("{\"command\":\"always_on_changed\",\"always_on\":true,"
              "\"public\":true}");
    module_handle_event();
    check(pref_written("alwaysOn=1"), "with public on, it is accepted");
    check(cap_contains("\"field\":\"always_on__readonly\""),
          "and the redraw reports the switch usable");
    check(cap_contains("\"value\":\"1274\""),
          "a four-figure count keeps every digit: shortening it would buy no "
          "room and lose precision");

    /* ── the other tiers and limits ── */
    cap_clear(); prefs_clear();
    inbox_set("{\"command\":\"keep_followed_changed\",\"keep_followed\":false}");
    module_handle_event();
    check(pref_written("keepFollowed=0"), "the follow tier can be turned off");

    cap_clear(); prefs_clear();
    inbox_set("{\"command\":\"quota_mb_changed\",\"quota_mb\":\"2000\"}");
    module_handle_event();
    check(pref_written("archiveMaxMb=2000"), "the strangers' limit is set");

    cap_clear(); prefs_clear();
    inbox_set("{\"command\":\"max_days_changed\",\"max_days\":\"30\"}");
    module_handle_event();
    check(pref_written("archiveMaxDays=30"), "and how long they are kept");

    /* ── the files tab is a different question, and still works ── */
    cap_clear(); prefs_clear();
    inbox_set("{\"command\":\"enabled_changed\",\"enabled\":true,"
              "\"quota\":\"20\"}");
    module_handle_event();
    check(pref_written("quotaGb=20"),
          "hosting files for others is its own switch, unchanged");

    /* ── the screen is event-driven, with no clock ── */
    cap_clear();
    module_tick();
    check(cap_count() == 0, "a tick draws nothing");
    cap_clear();
    event_push("core.archive");
    module_handle_event();
    check(cap_contains("\"field\":\"archive_dashboard\""),
          "the archive changing redraws it");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
