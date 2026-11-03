#include "cli/help.h"
#include "tests/test_check.h"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

  constexpr std::array kAlphaPositionals{
      okaru::cli::Positional{"path", {}, {}, false, false, false},
  };
  constexpr std::array kChildren{
      okaru::cli::Command{"zeta", "Run zeta", {}, {}, {}, {}, {}, false},
      okaru::cli::Command{"alpha", "Run alpha", {}, {}, {}, kAlphaPositionals, {}, false},
  };
  constexpr std::array kGroupFlags{
      okaru::cli::Flag{"--verbose", {}, {}, "Enable verbose output", {}, {}, false, false},
  };
  constexpr okaru::cli::Command kGroup{
      "tools", "Tool commands", "Manage offline tools.", {}, kGroupFlags, {}, kChildren, false,
  };

  constexpr std::array<std::string_view, 2> kKinds{"full", "merged"};
  constexpr std::array kLeafPositionals{
      okaru::cli::Positional{"input", "Input file", {}, true, false, false},
      okaru::cli::Positional{"kind", {}, kKinds, false, false, false},
  };
  constexpr std::array kLeafFlags{
      okaru::cli::Flag{{}, "-o", "<file>", "Write output", {}, {}, false, false},
      okaru::cli::Flag{"--mode", "-m", "<mode>", "Select mode", kKinds, "full", false, false},
      okaru::cli::Flag{"--force", {}, {}, "Overwrite output", {}, {}, false, false},
  };
  constexpr okaru::cli::Command kLeaf{
      "convert",
      "Convert input",
      "Convert a source file.\n\nThe output format is selected independently.",
      "See https://example.test for details.",
      kLeafFlags,
      kLeafPositionals,
      {},
      false,
  };

  void checkGroupHelp() {
    const std::string expected = "Usage: okaru tools <command> [options]\n"
                                 "\n"
                                 "Manage offline tools.\n"
                                 "\n"
                                 "Commands:\n"
                                 "  alpha [path]  Run alpha\n"
                                 "  zeta          Run zeta\n"
                                 "\n"
                                 "Options:\n"
                                 "      --verbose  Enable verbose output\n"
                                 "  -h, --help     Show this help message\n";
    TEST_CHECK(okaru::cli::renderHelp(kGroup, "okaru tools") == expected);
  }

  void checkLeafHelp() {
    const std::string expected = "Usage: okaru convert <input> [kind] [options]\n"
                                 "\n"
                                 "Convert a source file.\n"
                                 "\n"
                                 "The output format is selected independently.\n"
                                 "\n"
                                 "Arguments:\n"
                                 "  <input>  Input file\n"
                                 "  [kind]   one of: full, merged\n"
                                 "\n"
                                 "Options:\n"
                                 "  -o <file>          Write output\n"
                                 "  -m, --mode <mode>  Select mode (default: full)\n"
                                 "      --force        Overwrite output\n"
                                 "  -h, --help         Show this help message\n"
                                 "\n"
                                 "See https://example.test for details.\n";
    TEST_CHECK(okaru::cli::renderHelp(kLeaf, "okaru convert") == expected);
    TEST_CHECK(okaru::cli::renderArgsSpec(kLeaf) == "<input> [kind] [options]");
    TEST_CHECK(okaru::cli::renderArgsSpec(kGroup) == "<{zeta|alpha}> ...");
  }

} // namespace

int main() {
  static_assert(okaru::cli::validateCommand(kGroup));
  static_assert(okaru::cli::validateCommand(kLeaf));
  checkGroupHelp();
  checkLeafHelp();
  return EXIT_SUCCESS;
}
