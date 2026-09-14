/*
 * Reading what the core hands over: a wire's fields (XPRS.md 4) and the
 * little JSON the host speaks. Run via the App Creator "Run tests" action,
 * or: make tests
 */
#include "wapp_test.h"
#include "../wire.c"

WAPP_TEST(a_field_is_matched_whole_never_by_its_tail) {
  const char *w = "t:observation f:X1HOME intemp:21.5C temp:14.2C lifeload:12400Wh load:65W";
  char v[24];
  WAPP_EXPECT_TRUE(th_field(w, "temp", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "14.2C");
  WAPP_EXPECT_TRUE(th_field(w, "load", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "65W");
  WAPP_EXPECT_FALSE(th_field(w, "hum", v, sizeof v));
}

WAPP_TEST(m_runs_to_the_end_and_hides_what_follows) {
  const char *w = "t:observation f:X4PL3M state:on m:pump restarted state:off";
  char v[40];
  WAPP_EXPECT_TRUE(th_field(w, "state", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "on");
  WAPP_EXPECT_TRUE(th_field(w, "m", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "pump restarted state:off");
}

WAPP_TEST(a_kept_list_reads_like_a_wire) {
  char list[64] = "";
  char v[16];
  th_put(list, "state", "off", sizeof list);
  th_put(list, "volt", "23.8V", sizeof list);
  WAPP_EXPECT_STR_EQ(list, "state:off volt:23.8V");
  WAPP_EXPECT_TRUE(th_field(list, "volt", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "23.8V");
}

WAPP_TEST(a_top_level_array_of_strings) {
  char c[16];
  const char *p = th_json_top("[\"X4PL3M\",\"X4DOOR\"]");
  p = th_json_next_str(p, c, sizeof c);
  WAPP_EXPECT_STR_EQ(c, "X4PL3M");
  p = th_json_next_str(p, c, sizeof c);
  WAPP_EXPECT_STR_EQ(c, "X4DOOR");
  WAPP_EXPECT_TRUE(th_json_next_str(p, c, sizeof c) == 0);
}

WAPP_TEST(a_top_level_array_of_objects) {
  char o[64], v[16];
  const char *p = th_json_top("[{\"from\":\"X4A\",\"m\":\"a}b\"},{\"from\":\"X4B\"}]");
  p = th_json_next(p, o, sizeof o);
  WAPP_EXPECT_TRUE(th_json(o, "from", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "X4A");
  p = th_json_next(p, o, sizeof o);
  WAPP_EXPECT_TRUE(th_json(o, "from", v, sizeof v));
  WAPP_EXPECT_STR_EQ(v, "X4B");
  WAPP_EXPECT_TRUE(th_json_next(p, o, sizeof o) == 0);
}
