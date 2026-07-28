#include "util/strings.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace credis {

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

static char upper_ascii(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool str_ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  }
  return true;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lower_ascii(c);
  return out;
}

std::string to_upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = upper_ascii(c);
  return out;
}

// Port of Redis's string2ll(). Deliberately stricter than strtoll: "  1", "+1",
// "01" and "1x" are all rejected so that INCR and friends behave identically.
bool string2ll(std::string_view s, int64_t* out) {
  const char* p = s.data();
  size_t plen = 0;
  const size_t slen = s.size();
  bool negative = false;

  if (slen == 0) return false;

  // Special case: the only string that may start with '0' is "0" itself.
  if (slen == 1 && p[0] == '0') {
    if (out) *out = 0;
    return true;
  }

  if (p[0] == '-') {
    negative = true;
    ++p;
    ++plen;
    if (plen == slen) return false;  // just a sign
  }

  uint64_t v = 0;
  if (p[0] >= '1' && p[0] <= '9') {
    v = static_cast<uint64_t>(p[0] - '0');
    ++p;
    ++plen;
  } else {
    return false;
  }

  constexpr uint64_t kUMax = std::numeric_limits<uint64_t>::max();
  while (plen < slen && p[0] >= '0' && p[0] <= '9') {
    if (v > kUMax / 10) return false;
    v *= 10;
    const uint64_t digit = static_cast<uint64_t>(p[0] - '0');
    if (v > kUMax - digit) return false;
    v += digit;
    ++p;
    ++plen;
  }

  if (plen < slen) return false;  // trailing garbage

  if (negative) {
    // -(LLONG_MIN) does not fit in int64_t, so compute the bound unsigned.
    constexpr uint64_t kNegBound =
        static_cast<uint64_t>(-(std::numeric_limits<int64_t>::min() + 1)) + 1;
    if (v > kNegBound) return false;
    if (out) *out = static_cast<int64_t>(-v);
  } else {
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    if (out) *out = static_cast<int64_t>(v);
  }
  return true;
}

bool string2d(std::string_view s, double* out) {
  if (s.empty() || s.size() > 512) return false;
  if (isspace(static_cast<unsigned char>(s.front()))) return false;

  // strtod needs a NUL-terminated buffer; keys/values are bounded here so a
  // stack copy is always enough.
  char buf[513];
  memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';

  errno = 0;
  char* eptr = nullptr;
  const double value = strtod(buf, &eptr);

  if (static_cast<size_t>(eptr - buf) != s.size()) return false;
  if (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL || value == 0)) return false;
  if (std::isnan(value)) return false;

  if (out) *out = value;
  return true;
}

bool string2ld(std::string_view s, long double* out) {
  // Redis allows very long inputs here (MAX_LONG_DOUBLE_CHARS), since a long
  // double can carry many significant digits.
  constexpr size_t kMaxChars = 5 * 1024;
  if (s.empty() || s.size() >= kMaxChars) return false;
  if (isspace(static_cast<unsigned char>(s.front()))) return false;

  std::string buf(s);
  errno = 0;
  char* eptr = nullptr;
  const long double value = strtold(buf.c_str(), &eptr);

  if (static_cast<size_t>(eptr - buf.c_str()) != s.size()) return false;
  if (errno == ERANGE &&
      (value == HUGE_VALL || value == -HUGE_VALL || std::fpclassify(value) == FP_ZERO)) {
    return false;
  }
  if (errno == EINVAL) return false;
  if (std::isnan(value)) return false;

  if (out) *out = value;
  return true;
}

bool string2d_finite(std::string_view s, double* out) {
  double value = 0;
  if (!string2d(s, &value)) return false;
  if (std::isinf(value)) return false;
  if (out) *out = value;
  return true;
}

std::string ll2string(int64_t value) {
  char buf[32];
  const auto res = std::to_chars(buf, buf + sizeof(buf), value);
  return std::string(buf, static_cast<size_t>(res.ptr - buf));
}

std::string d2string(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  if (value == 0) {
    // Signed zero: Redis distinguishes -0 from 0 here.
    return std::signbit(value) ? "-0" : "0";
  }
  // An integral value is printed without a fractional part ("3", not "3.0").
  if (value == static_cast<double>(static_cast<int64_t>(value)) &&
      std::fabs(value) < 1e17) {
    return ll2string(static_cast<int64_t>(value));
  }
  // Shortest representation that round-trips, matching modern Redis.
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof(buf), value);
  return std::string(buf, static_cast<size_t>(res.ptr - buf));
}

std::string ld2string_human(long double value) {
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";

  char buf[5 * 1024];
  int len = snprintf(buf, sizeof(buf), "%.17Lf", value);
  if (len < 0) return "0";

  std::string out(buf, static_cast<size_t>(len));
  // Strip trailing zeros (and a trailing '.') the way Redis's ld2string does.
  if (out.find('.') != std::string::npos) {
    size_t last = out.size();
    while (last > 0 && out[last - 1] == '0') --last;
    if (last > 0 && out[last - 1] == '.') --last;
    out.resize(last);
  }
  if (out == "-0") out = "0";
  return out;
}

namespace {

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_digit_to_int(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  return lower_ascii(c) - 'a' + 10;
}

bool is_arg_space(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f';
}

}  // namespace

// Port of Redis's sdssplitargs(). Indices are bounds-checked rather than relying
// on a NUL terminator, so this is safe on arbitrary socket bytes.
bool split_args(std::string_view line, std::vector<std::string>* out) {
  out->clear();
  size_t i = 0;
  const size_t n = line.size();

  while (true) {
    while (i < n && is_arg_space(line[i])) ++i;
    if (i >= n) return true;

    std::string current;
    bool in_double = false;
    bool in_single = false;
    bool done = false;

    while (!done) {
      const bool at_end = i >= n;
      const char c = at_end ? '\0' : line[i];

      if (in_double) {
        if (!at_end && c == '\\' && i + 3 < n && line[i + 1] == 'x' && is_hex_digit(line[i + 2]) &&
            is_hex_digit(line[i + 3])) {
          const auto byte = static_cast<unsigned char>(hex_digit_to_int(line[i + 2]) * 16 +
                                                       hex_digit_to_int(line[i + 3]));
          current.push_back(static_cast<char>(byte));
          i += 3;
        } else if (!at_end && c == '\\' && i + 1 < n) {
          ++i;
          switch (line[i]) {
            case 'n': current.push_back('\n'); break;
            case 'r': current.push_back('\r'); break;
            case 't': current.push_back('\t'); break;
            case 'b': current.push_back('\b'); break;
            case 'a': current.push_back('\a'); break;
            default: current.push_back(line[i]); break;
          }
        } else if (!at_end && c == '"') {
          // A closing quote must end the token.
          if (i + 1 < n && !is_arg_space(line[i + 1])) return false;
          done = true;
        } else if (at_end) {
          return false;  // unterminated quotes
        } else {
          current.push_back(c);
        }
      } else if (in_single) {
        if (!at_end && c == '\\' && i + 1 < n && line[i + 1] == '\'') {
          ++i;
          current.push_back('\'');
        } else if (!at_end && c == '\'') {
          if (i + 1 < n && !is_arg_space(line[i + 1])) return false;
          done = true;
        } else if (at_end) {
          return false;  // unterminated quotes
        } else {
          current.push_back(c);
        }
      } else {
        if (at_end || is_arg_space(c)) {
          done = true;
        } else if (c == '"') {
          in_double = true;
        } else if (c == '\'') {
          in_single = true;
        } else {
          current.push_back(c);
        }
      }
      if (i < n) ++i;
    }
    out->push_back(std::move(current));
  }
}

namespace {

// Recursive core of the glob matcher. `skip_longer_matches` is the pruning flag
// from Redis's implementation: once we know the tail of the pattern cannot match
// anywhere in the remaining string, earlier '*'s need not try longer expansions.
bool glob_match_impl(const char* pattern, size_t patternLen, const char* str, size_t strLen,
                     bool nocase, bool* skip_longer_matches, int depth) {
  if (depth > 1000) return false;  // guard against pathological patterns

  while (patternLen && strLen) {
    switch (pattern[0]) {
      case '*': {
        while (patternLen > 1 && pattern[1] == '*') {
          ++pattern;
          --patternLen;
        }
        if (patternLen == 1) return true;  // trailing '*' matches everything
        while (strLen) {
          if (glob_match_impl(pattern + 1, patternLen - 1, str, strLen, nocase,
                              skip_longer_matches, depth + 1)) {
            return true;
          }
          if (*skip_longer_matches) return false;
          ++str;
          --strLen;
        }
        *skip_longer_matches = true;
        return false;
      }
      case '?':
        ++str;
        --strLen;
        break;
      case '[': {
        ++pattern;
        --patternLen;
        const bool negate = patternLen && pattern[0] == '^';
        if (negate) {
          ++pattern;
          --patternLen;
        }
        bool match = false;
        while (true) {
          if (patternLen == 0) {
            // Unterminated class: back up so the outer decrement stays balanced.
            --pattern;
            ++patternLen;
            break;
          } else if (pattern[0] == '\\' && patternLen >= 2) {
            ++pattern;
            --patternLen;
            if (pattern[0] == str[0]) match = true;
          } else if (pattern[0] == ']') {
            break;
          } else if (patternLen >= 3 && pattern[1] == '-') {
            int start = static_cast<unsigned char>(pattern[0]);
            int end = static_cast<unsigned char>(pattern[2]);
            int c = static_cast<unsigned char>(str[0]);
            if (start > end) std::swap(start, end);
            if (nocase) {
              start = lower_ascii(static_cast<char>(start));
              end = lower_ascii(static_cast<char>(end));
              c = lower_ascii(static_cast<char>(c));
            }
            pattern += 2;
            patternLen -= 2;
            if (c >= start && c <= end) match = true;
          } else {
            if (!nocase) {
              if (pattern[0] == str[0]) match = true;
            } else {
              if (lower_ascii(pattern[0]) == lower_ascii(str[0])) match = true;
            }
          }
          ++pattern;
          --patternLen;
        }
        if (negate) match = !match;
        if (!match) return false;
        ++str;
        --strLen;
        break;
      }
      case '\\':
        if (patternLen >= 2) {
          ++pattern;
          --patternLen;
        }
        [[fallthrough]];
      default:
        if (!nocase) {
          if (pattern[0] != str[0]) return false;
        } else {
          if (lower_ascii(pattern[0]) != lower_ascii(str[0])) return false;
        }
        ++str;
        --strLen;
        break;
    }
    ++pattern;
    --patternLen;
    if (strLen == 0) {
      while (patternLen && pattern[0] == '*') {
        ++pattern;
        --patternLen;
      }
      break;
    }
  }
  return patternLen == 0 && strLen == 0;
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view str, bool nocase) {
  // Redis's stringmatchlen() loop requires a non-empty subject, so it reports no
  // match for ("*", ""); it hides that with an "allkeys" shortcut in KEYS/SCAN
  // instead. Handle the empty subject here so the matcher is correct on its own:
  // only an all-'*' pattern (including the empty pattern) matches "".
  if (str.empty()) {
    return pattern.find_first_not_of('*') == std::string_view::npos;
  }
  bool skip_longer_matches = false;
  return glob_match_impl(pattern.data(), pattern.size(), str.data(), str.size(), nocase,
                         &skip_longer_matches, 0);
}

}  // namespace credis
