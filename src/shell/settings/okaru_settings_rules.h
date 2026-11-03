#pragma once

// OKARU: Ventanas → Reglas. Lista y editor de las reglas de ventana y de capa de
// hypr/oka/windowrules.lua; cada regla se edita en su lugar.

#include <functional>

class Flex;

namespace settings {

  struct SettingsRulesContext {
    float scale = 1.0F;
    std::function<void()> requestRebuild;
  };

  void addSettingsRules(Flex& content, SettingsRulesContext ctx);

} // namespace settings
