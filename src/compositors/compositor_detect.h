#pragma once

#include <cstdint>
#include <string_view>

namespace compositors {

  // OKARU: solo Hyprland y Niri (el resto de compositores se quitó).
  enum class CompositorKind : std::uint8_t {
    Unknown = 0,
    Niri = 1,
    Hyprland = 2,
  };

  // Detected once per process from env vars. Cached after the first call.
  [[nodiscard]] CompositorKind detect();

  // Human-readable name (e.g. for diagnostics / hardware info). "Unknown" for unrecognized.
  [[nodiscard]] std::string_view name(CompositorKind kind);

  // Concatenated XDG_CURRENT_DESKTOP:XDG_SESSION_DESKTOP:DESKTOP_SESSION env hint for diagnostics.
  [[nodiscard]] std::string_view envHint();

  [[nodiscard]] inline bool isNiri() { return detect() == CompositorKind::Niri; }
  [[nodiscard]] inline bool isHyprland() { return detect() == CompositorKind::Hyprland; }

} // namespace compositors
