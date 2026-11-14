#include "core/build_info.h"

#include "okaru_git_revision.h"
#include "okaru_build_config.h"

namespace okaru::build_info {

  std::string_view version() noexcept { return OKARU_VERSION; }

  std::string_view revision() noexcept { return OKARU_GIT_REVISION; }

  std::string displayVersion() {
    std::string label = "v";
    label += version();

    constexpr std::string_view unknownRevision = "unknown";
    const std::string_view rev = revision();
    if (!rev.empty() && rev != unknownRevision) {
      label += " (";
      label += rev;
      label += ')';
    }
    return label;
  }

} // namespace okaru::build_info
