#pragma once

#include <cstdlib>
#include <print>
#include <source_location>

#ifdef NDEBUG
#error "OKARU test targets must be compiled without NDEBUG"
#endif

namespace okaru::test {

  [[noreturn]] inline void
  failCheck(const char* expression, std::source_location location = std::source_location::current()) {
    std::println(
        stderr, "{}:{}: check failed: {}", location.file_name(), static_cast<unsigned>(location.line()), expression
    );
    std::exit(EXIT_FAILURE);
  }

} // namespace okaru::test

#define TEST_CHECK(expression)                                                                                         \
  do {                                                                                                                 \
    if (!(expression)) {                                                                                               \
      ::okaru::test::failCheck(#expression);                                                                        \
    }                                                                                                                  \
  } while (false)
