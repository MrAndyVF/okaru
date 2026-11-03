#pragma once

// OKARU: Pantallas → Workspaces. Crear, editar y quitar workspaces por monitor (nombre, icono,
// persistente, por defecto). Se guardan en el bloque okaru:workspaces de hypr/oka/outputs.lua y se
// aplican en vivo; los iconos van a [widget.workspaces] icons.

#include <functional>
#include <string>
#include <vector>

class ConfigService;
class Flex;

namespace settings {

  struct SettingsWorkspacesContext {
    float scale = 1.0F;
    ConfigService* config = nullptr;
    std::vector<std::string> outputs; // salidas conectadas
    std::function<void()> requestRebuild;
  };

  void addSettingsWorkspaces(Flex& content, SettingsWorkspacesContext ctx);

} // namespace settings
