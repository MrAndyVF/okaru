#pragma once

#include <cstdint>

namespace okaru::pipewire {

  enum class ErrorDisposition : std::uint8_t {
    Reconnect,
    StaleObject,
    Report,
  };

  [[nodiscard]] ErrorDisposition classifyError(std::uint32_t objectId, int result) noexcept;

} // namespace okaru::pipewire
