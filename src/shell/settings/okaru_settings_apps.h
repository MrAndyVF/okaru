#pragma once

// OKARU: módulo Aplicaciones de los ajustes: apps por defecto, tipos de archivo por app y apps instaladas
// (abrir, fijar en el dock, ocultar del lanzador, paquete y desinstalar).

#include <functional>
#include <string_view>

class AsyncTextureCache;
class ConfigService;
class Flex;

namespace settings {

  struct SettingsAppsContext {
    float scale = 1.0F;
    std::string_view page; // "apps-defaults", "apps-file-types" o "apps-installed"
    ConfigService* config = nullptr;
    AsyncTextureCache* textures = nullptr; // iconos de Instaladas
    std::function<void()> requestRebuild;
  };

  void addSettingsApps(Flex& content, const SettingsAppsContext& ctx);

} // namespace settings
