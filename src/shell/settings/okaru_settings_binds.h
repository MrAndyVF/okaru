#pragma once

// OKARU: Entrada → Atajos. Atajos de hypr/oka/binds.lua y user-binds.lua agrupados en Sistema,
// Multimedia, Entrada, User y Otros, con un selector horizontal de categoría. Los de User (apps
// que instala el usuario) se guardan en user-binds.lua.

#include <functional>

class Flex;

namespace settings {

  struct SettingsBindsContext {
    float scale = 1.0F;
    std::function<void()> requestRebuild;
  };

  void addSettingsBinds(Flex& content, SettingsBindsContext ctx);

} // namespace settings
