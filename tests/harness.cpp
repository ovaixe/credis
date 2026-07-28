#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>

namespace credis::testing {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

std::string escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (c < 0x20 || c >= 0x7f) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\x%02x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

int run_all(int argc, char** argv) {
  // Optional substring filter: ./credis-tests Dict
  const char* filter = argc > 1 ? argv[1] : nullptr;

  auto& tests = registry();
  std::stable_sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
    return a.suite < b.suite;
  });

  int passed = 0;
  int failed = 0;
  int skipped = 0;
  std::string current_suite;

  for (const TestCase& test : tests) {
    const std::string full = test.suite + "." + test.name;
    if (filter && full.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    if (test.suite != current_suite) {
      current_suite = test.suite;
      printf("\n\033[1m%s\033[0m\n", current_suite.c_str());
    }

    std::string failure;
    try {
      test.fn();
    } catch (const CheckFailure& e) {
      failure = e.message;
    } catch (const std::exception& e) {
      failure = std::string("unexpected exception: ") + e.what();
    } catch (...) {
      failure = "unexpected non-standard exception";
    }

    if (failure.empty()) {
      printf("  \033[32m✓\033[0m %s\n", test.name.c_str());
      ++passed;
    } else {
      printf("  \033[31m✗\033[0m %s\n    %s\n", test.name.c_str(), failure.c_str());
      ++failed;
    }
  }

  printf("\n%d passed, %d failed", passed, failed);
  if (skipped) printf(", %d filtered out", skipped);
  printf("\n");
  return failed == 0 ? 0 : 1;
}

}  // namespace credis::testing

int main(int argc, char** argv) { return credis::testing::run_all(argc, argv); }
