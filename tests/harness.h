#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// A ~100 line test framework. Tests self-register via a static initializer, so a
// new test file only needs to be dropped into tests/ for CMake to pick it up.
//
//   TEST(Dict, InsertAndLookup) {
//     Dict<int> d;
//     CHECK(d.insert("a", 1));
//     CHECK_EQ(*d.find("a"), 1);
//   }

namespace credis::testing {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> fn;
};

// Thrown by the CHECK_* macros; caught by the runner so one failure does not
// abort the rest of the suite.
struct CheckFailure {
  std::string message;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(std::string suite, std::string name, std::function<void()> fn) {
    registry().push_back({std::move(suite), std::move(name), std::move(fn)});
  }
};

// Renders a value for a failure message, falling back to a placeholder for types
// that have no operator<<.
template <typename T>
std::string describe(const T& value) {
  if constexpr (requires(std::ostringstream& os) { os << value; }) {
    std::ostringstream os;
    os << value;
    return os.str();
  } else {
    return "<value>";
  }
}

std::string escape(std::string_view s);

int run_all(int argc, char** argv);

}  // namespace credis::testing

#define CREDIS_CONCAT_INNER(a, b) a##b
#define CREDIS_CONCAT(a, b) CREDIS_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                             \
  static void CREDIS_CONCAT(credis_test_, __LINE__)();                                \
  static const ::credis::testing::Registrar CREDIS_CONCAT(credis_registrar_, __LINE__)( \
      #suite, #name, &CREDIS_CONCAT(credis_test_, __LINE__));                         \
  static void CREDIS_CONCAT(credis_test_, __LINE__)()

#define CREDIS_FAIL(msg)                                                     \
  do {                                                                       \
    throw ::credis::testing::CheckFailure{                                   \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg)}; \
  } while (0)

#define CHECK(expr) \
  do {              \
    if (!(expr)) CREDIS_FAIL("CHECK(" #expr ") is false"); \
  } while (0)

#define CHECK_FALSE(expr) \
  do {                    \
    if (expr) CREDIS_FAIL("CHECK_FALSE(" #expr ") is true"); \
  } while (0)

#define CREDIS_CHECK_OP(a, b, op, label)                                            \
  do {                                                                              \
    const auto& credis_a = (a);                                                     \
    const auto& credis_b = (b);                                                     \
    if (!(credis_a op credis_b)) {                                                  \
      CREDIS_FAIL(std::string(label) + "(" #a ", " #b ")\n    left:  " +            \
                  ::credis::testing::describe(credis_a) + "\n    right: " +         \
                  ::credis::testing::describe(credis_b));                           \
    }                                                                               \
  } while (0)

#define CHECK_EQ(a, b) CREDIS_CHECK_OP(a, b, ==, "CHECK_EQ")
#define CHECK_NE(a, b) CREDIS_CHECK_OP(a, b, !=, "CHECK_NE")
#define CHECK_LT(a, b) CREDIS_CHECK_OP(a, b, <, "CHECK_LT")
#define CHECK_LE(a, b) CREDIS_CHECK_OP(a, b, <=, "CHECK_LE")
#define CHECK_GT(a, b) CREDIS_CHECK_OP(a, b, >, "CHECK_GT")
#define CHECK_GE(a, b) CREDIS_CHECK_OP(a, b, >=, "CHECK_GE")

// Compares byte strings and reports them with escapes, so a RESP mismatch prints
// as "+OK\r\n" rather than wrapping the terminal.
#define CHECK_BYTES(a, b)                                                           \
  do {                                                                              \
    const std::string credis_a{a};                                                  \
    const std::string credis_b{b};                                                  \
    if (credis_a != credis_b) {                                                     \
      CREDIS_FAIL(std::string("CHECK_BYTES(" #a ", " #b ")\n    actual:   \"") +    \
                  ::credis::testing::escape(credis_a) + "\"\n    expected: \"" +    \
                  ::credis::testing::escape(credis_b) + "\"");                      \
    }                                                                               \
  } while (0)
