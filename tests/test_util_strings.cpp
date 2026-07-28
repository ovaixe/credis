#include <cmath>
#include <limits>

#include "harness.h"
#include "util/strings.h"

using namespace credis;

TEST(String2ll, AcceptsValidIntegers) {
  int64_t v = -1;
  CHECK(string2ll("0", &v));
  CHECK_EQ(v, 0);
  CHECK(string2ll("123", &v));
  CHECK_EQ(v, 123);
  CHECK(string2ll("-123", &v));
  CHECK_EQ(v, -123);
  CHECK(string2ll("9223372036854775807", &v));
  CHECK_EQ(v, std::numeric_limits<int64_t>::max());
  CHECK(string2ll("-9223372036854775808", &v));
  CHECK_EQ(v, std::numeric_limits<int64_t>::min());
}

TEST(String2ll, RejectsWhatRedisRejects) {
  int64_t v = 0;
  CHECK_FALSE(string2ll("", &v));
  CHECK_FALSE(string2ll(" 1", &v));
  CHECK_FALSE(string2ll("1 ", &v));
  CHECK_FALSE(string2ll("+1", &v));
  CHECK_FALSE(string2ll("01", &v));       // leading zero
  CHECK_FALSE(string2ll("-0", &v));       // negative zero
  CHECK_FALSE(string2ll("-", &v));
  CHECK_FALSE(string2ll("12x", &v));
  CHECK_FALSE(string2ll("1.0", &v));
  CHECK_FALSE(string2ll("9223372036854775808", &v));   // overflow by one
  CHECK_FALSE(string2ll("-9223372036854775809", &v));  // underflow by one
  CHECK_FALSE(string2ll("99999999999999999999999", &v));
}

TEST(String2d, ParsesDoublesAndInfinities) {
  double d = 0;
  CHECK(string2d("3.5", &d));
  CHECK_EQ(d, 3.5);
  CHECK(string2d("-0.25", &d));
  CHECK_EQ(d, -0.25);
  CHECK(string2d("inf", &d));
  CHECK(d > 0 && std::isinf(d));
  CHECK(string2d("-inf", &d));
  CHECK(d < 0 && std::isinf(d));
  CHECK(string2d("+inf", &d));
  CHECK(d > 0 && std::isinf(d));

  CHECK_FALSE(string2d("", &d));
  CHECK_FALSE(string2d("nan", &d));
  CHECK_FALSE(string2d(" 1", &d));
  CHECK_FALSE(string2d("1.0.0", &d));
  CHECK_FALSE(string2d("abc", &d));
  CHECK_FALSE(string2d_finite("inf", &d));
}

TEST(D2string, MatchesRedisFormatting) {
  CHECK_BYTES(d2string(3.0), "3");
  CHECK_BYTES(d2string(3.5), "3.5");
  CHECK_BYTES(d2string(0.0), "0");
  CHECK_BYTES(d2string(-0.0), "-0");
  CHECK_BYTES(d2string(1.0 / 0.0), "inf");
  CHECK_BYTES(d2string(-1.0 / 0.0), "-inf");
  CHECK_BYTES(d2string(-17.0), "-17");
}

TEST(Ld2string, StripsTrailingZeros) {
  CHECK_BYTES(ld2string_human(10.5L), "10.5");
  CHECK_BYTES(ld2string_human(3.0L), "3");
  CHECK_BYTES(ld2string_human(5.0e3L), "5000");
  CHECK_BYTES(ld2string_human(0.1L), "0.1");
}

TEST(Glob, MatchesRedisSemantics) {
  CHECK(glob_match("*", "anything"));
  CHECK(glob_match("*", ""));
  CHECK(glob_match("h?llo", "hello"));
  CHECK(glob_match("h?llo", "hallo"));
  CHECK_FALSE(glob_match("h?llo", "hllo"));
  CHECK(glob_match("h*llo", "hllo"));
  CHECK(glob_match("h*llo", "heeeello"));
  CHECK(glob_match("h[ae]llo", "hello"));
  CHECK(glob_match("h[ae]llo", "hallo"));
  CHECK_FALSE(glob_match("h[ae]llo", "hillo"));
  CHECK(glob_match("h[^e]llo", "hallo"));
  CHECK_FALSE(glob_match("h[^e]llo", "hello"));
  CHECK(glob_match("h[a-c]llo", "hbllo"));
  CHECK_FALSE(glob_match("h[a-c]llo", "hdllo"));
  CHECK(glob_match("user:*", "user:1000"));
  CHECK_FALSE(glob_match("user:*", "admin:1000"));
  CHECK(glob_match("\\*", "*"));
  CHECK_FALSE(glob_match("\\*", "x"));
  CHECK(glob_match("HELLO", "hello", /*nocase=*/true));
  CHECK_FALSE(glob_match("HELLO", "hello", /*nocase=*/false));
  // Pathological input must terminate rather than blow up.
  CHECK_FALSE(glob_match("*a*a*a*a*a*a*a*b", std::string(64, 'a')));
}

TEST(Case, InsensitiveCompare) {
  CHECK(str_ieq("GET", "get"));
  CHECK(str_ieq("", ""));
  CHECK_FALSE(str_ieq("GET", "getx"));
  CHECK_BYTES(to_lower("HeLLo"), "hello");
  CHECK_BYTES(to_upper("HeLLo"), "HELLO");
}

TEST(Glob, EmptySubject) {
  CHECK(glob_match("*", ""));
  CHECK(glob_match("**", ""));
  CHECK(glob_match("", ""));
  CHECK_FALSE(glob_match("a*", ""));
  CHECK_FALSE(glob_match("?", ""));
}
