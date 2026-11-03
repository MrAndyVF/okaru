#pragma once

#include <string>
#include <string_view>

namespace okaru::build_info {

  [[nodiscard]] std::string_view version() noexcept;

  [[nodiscard]] std::string_view revision() noexcept;

  [[nodiscard]] std::string displayVersion();

} // namespace okaru::build_info
