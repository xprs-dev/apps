/*
 * The packets this wapp builds, against XPRS.md's own examples.
 *
 * 11.9 and 11.10 write out the claim, a setting and a new key, and the
 * corpus tool (app/tool/xprs_corpus.py) derived their identifiers: this
 * wapp's builders must produce those wires byte for byte and name them the
 * same way, because a result's r: is the only thing that ties an answer to
 * the command it answers.
 *
 * Run via the App Creator "Run tests" action, or: make tests
 */
#include "wapp_test.h"
#include "../wire.c"

#define OWNER_NPUB "npub1qz3n7fu9j9uenmyva7ha6x9eqwymytv2847ccv4vxdmn45y50q7h7k5f"

WAPP_TEST(the_claim_is_11_9s_example) {
  char w[FW_WIRE_MAX + 1], id[7];
  int n = dv_claim(w, sizeof w, "X1QZ3N", "X3RLY7", "2026-08-08_14:26:50", OWNER_NPUB);
  WAPP_EXPECT_STR_EQ(w, "t:command f:X1QZ3N d:X3RLY7 ts:2026-08-08_14:26:50 "
                        "cmd:set owner:X1QZ3N k:" OWNER_NPUB);
  WAPP_EXPECT_INT_EQ(n + FW_SIG_ROOM, 200);   /* the document's count */
  dv_id(w, (unsigned)n, id);
  WAPP_EXPECT_STR_EQ(id, "06092b");
}

WAPP_TEST(a_setting_and_a_new_key_are_11_10s) {
  char w[FW_WIRE_MAX + 1], id[7];
  int n = dv_set(w, sizeof w, "X1QZ3N", "X3RLY7", "2026-08-08_14:32:00",
                 "nick:roof-north zone:+01:00 ap:off");
  WAPP_EXPECT_INT_EQ(n + FW_SIG_ROOM, 158);
  dv_id(w, (unsigned)n, id);
  WAPP_EXPECT_STR_EQ(id, "92d70e");
  n = dv_set(w, sizeof w, "X1QZ3N", "X3RLY7", "2026-08-08_14:33:00", "key:new");
  WAPP_EXPECT_INT_EQ(n + FW_SIG_ROOM, 131);
  dv_id(w, (unsigned)n, id);
  WAPP_EXPECT_STR_EQ(id, "6e945a");
}

WAPP_TEST(the_sealed_body_is_lines) {
  char b[128];
  const char *kv[] = { "ssid", "Casa do Mar", "pass", "sardinha na brasa 2026", 0 };
  int n = dv_body(b, sizeof b, kv);
  WAPP_EXPECT_STR_EQ(b, "cmd:set\nssid:Casa do Mar\npass:sardinha na brasa 2026");
  WAPP_EXPECT_INT_EQ(n, 52);                  /* 11.10: 52 bytes, 107 sealed */
  const char *bad[] = { "ssid", "two\nlines", 0 };
  WAPP_EXPECT_INT_EQ(dv_body(b, sizeof b, bad), -1);
}

WAPP_TEST(a_sealed_command_fits_at_128_characters_of_x) {
  char x[129], w[FW_WIRE_MAX + 1];
  for (int i = 0; i < 128; i++) x[i] = 'C';
  x[128] = 0;
  int n = dv_sealed(w, sizeof w, "X1QZ3N", "X3RLY7", "2026-08-08_14:34:00", x);
  WAPP_EXPECT_INT_EQ(n + FW_SIG_ROOM, 246);   /* 11.10's nsec example */
  char more[140];
  for (int i = 0; i < 139; i++) more[i] = 'C';
  more[139] = 0;
  WAPP_EXPECT_INT_EQ(dv_sealed(w, sizeof w, "X1QZ3N", "X3RLY7", "2026-08-08_14:34:00", more), -1);
}

WAPP_TEST(a_callsign_derives_from_its_key) {
  char c[12];
  dv_call_of(OWNER_NPUB, "X3", c, sizeof c);
  WAPP_EXPECT_STR_EQ(c, "X3QZ3N");
  WAPP_EXPECT_TRUE(dv_call_matches("X1QZ3N", OWNER_NPUB));
  WAPP_EXPECT_TRUE(dv_call_matches("X3QZ3N", OWNER_NPUB));
  WAPP_EXPECT_FALSE(dv_call_matches("X3QZ3X", OWNER_NPUB));
  WAPP_EXPECT_FALSE(dv_call_matches("X3QZ3NA", OWNER_NPUB));
}

WAPP_TEST(fields_read_like_section_4) {
  char v[64];
  const char *w = "t:result f:X3RLY7 d:X1QZ3N r:b4a13e code:500 wifi:failed sig:KKKK m:wrong password";
  WAPP_EXPECT_TRUE(dv_field(w, "code", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "500");
  WAPP_EXPECT_TRUE(dv_field(w, "m", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "wrong password");
  WAPP_EXPECT_FALSE(dv_field(w, "ip", v, sizeof v));
}

WAPP_TEST(the_stamp_is_the_documented_shape) {
  char s[24];
  dv_stamp(s, sizeof s, 1786199200ULL);
  WAPP_EXPECT_STR_EQ(s, "2026-08-08_14:26:40");
}

/* ── Reading what the core hands over (the equipment half) ───────────── */

WAPP_TEST(a_field_is_matched_whole_never_by_its_tail) {
  const char *w = "t:observation f:X1HOME intemp:21.5C temp:14.2C lifeload:12400Wh load:65W";
  char v[24];
  WAPP_EXPECT_TRUE(dv_field(w, "temp", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "14.2C");
  WAPP_EXPECT_TRUE(dv_field(w, "load", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "65W");
  WAPP_EXPECT_FALSE(dv_field(w, "hum", v, sizeof v));
}

WAPP_TEST(m_runs_to_the_end_and_hides_what_follows) {
  const char *w = "t:observation f:X4PL3M state:on m:pump restarted state:off";
  char v[40];
  WAPP_EXPECT_TRUE(dv_field(w, "state", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "on");
  WAPP_EXPECT_TRUE(dv_field(w, "m", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "pump restarted state:off");
}

WAPP_TEST(a_kept_list_reads_like_a_wire) {
  char list[64] = "";
  char v[16];
  dv_put(list, "state", "off", sizeof list);
  dv_put(list, "volt", "23.8V", sizeof list);
  WAPP_EXPECT_STR_EQ(list, "state:off volt:23.8V");
  WAPP_EXPECT_TRUE(dv_field(list, "volt", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "23.8V");
}

WAPP_TEST(a_top_level_array_of_strings) {
  char c[16];
  const char *p = dv_json_top("[\"X4PL3M\",\"X4DOOR\"]");
  p = dv_json_next_str(p, c, sizeof c);
  WAPP_EXPECT_STR_EQ(c, "X4PL3M");
  p = dv_json_next_str(p, c, sizeof c);
  WAPP_EXPECT_STR_EQ(c, "X4DOOR");
  WAPP_EXPECT_TRUE(dv_json_next_str(p, c, sizeof c) == 0);
}

WAPP_TEST(a_top_level_array_of_objects) {
  char o[64], v[16];
  const char *p = dv_json_top("[{\"from\":\"X4A\",\"m\":\"a}b\"},{\"from\":\"X4B\"}]");
  p = dv_json_next(p, o, sizeof o);
  WAPP_EXPECT_TRUE(dv_json(o, "from", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "X4A");
  p = dv_json_next(p, o, sizeof o);
  WAPP_EXPECT_TRUE(dv_json(o, "from", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "X4B");
  WAPP_EXPECT_TRUE(dv_json_next(p, o, sizeof o) == 0);
}
