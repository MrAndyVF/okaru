#pragma once

#include "compositors/compositor_detect.h"

namespace wayland_protocol_policy {

  [[nodiscard]] constexpr bool shouldBindExtForeignToplevelList(compositors::CompositorKind compositor) noexcept {
    // OKARU: solo Hyprland y Niri (y Unknown por si se ejecuta en otro compositor).
    return compositor == compositors::CompositorKind::Niri
        || compositor == compositors::CompositorKind::Hyprland
        || compositor == compositors::CompositorKind::Unknown;
  }

} // namespace wayland_protocol_policy
