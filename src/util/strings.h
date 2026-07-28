#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace credis {

// --- case-insensitive helpers (ASCII only, like Redis) ------------------------

char lower_ascii(char c);
bool str_ieq(std::string_view a, std::string_view b);
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);

// --- numeric conversion -------------------------------------------------------

// Strict integer parse, matching Redis's string2ll(): no whitespace, no '+' sign,
// no leading zeros ("0" itself is fine), no trailing garbage, overflow rejected.
bool string2ll(std::string_view s, int64_t* out);

// Strict double parse, matching Redis's getDoubleFromObject(): accepts "inf",
// "+inf", "-inf" and "infinity"; rejects NaN, empty strings, leading whitespace
// and trailing garbage.
bool string2d(std::string_view s, double* out);

// Like string2d but also rejects the infinities.
bool string2d_finite(std::string_view s, double* out);

// Strict long double parse, matching Redis's string2ld(). INCRBYFLOAT and
// HINCRBYFLOAT accumulate in long double: at double precision 10.5 + 0.1 would
// print as 10.59999999999999964 rather than 10.6.
bool string2ld(std::string_view s, long double* out);

std::string ll2string(int64_t value);

// Shortest round-trippable representation, integers without a fractional part,
// and the "inf"/"-inf" spellings Redis uses on the wire.
std::string d2string(double value);

// Formats a long double with `decimals` digits and strips trailing zeros, the way
// INCRBYFLOAT and HINCRBYFLOAT report their results.
std::string ld2string_human(long double value);

// --- argument splitting -------------------------------------------------------

// Splits a command line the way redis-cli and inline requests do: whitespace
// separated, with "double quoted" (supporting \n \r \t \b \a and \xHH escapes)
// and 'single quoted' (supporting \') tokens. A closing quote must be followed by
// whitespace or end of input.
//
// Returns false on unbalanced quotes, which the server reports as
// "Protocol error: unbalanced quotes in request".
bool split_args(std::string_view line, std::vector<std::string>* out);

// --- pattern matching ---------------------------------------------------------

// Glob-style matcher supporting '*', '?', '[...]' (with '^' negation and 'a-z'
// ranges) and '\' escapes. Port of Redis's stringmatchlen().
bool glob_match(std::string_view pattern, std::string_view str, bool nocase = false);

}  // namespace credis
