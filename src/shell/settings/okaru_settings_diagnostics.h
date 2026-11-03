#pragma once

// OKARU: Avanzado → Diagnóstico de Hyprland. Versión y errores de config, recargar, respaldos de la
// config (abrir carpeta y restaurar con doble confirmación) y bloques de hypr/oka/ que escribe OKARU.

#include <functional>

class Flex;

namespace settings {

  struct SettingsDiagnosticsContext {
    float scale = 1.0F;
    std::function<void()> requestRebuild;
  };

  void addSettingsDiagnostics(Flex& content, SettingsDiagnosticsContext ctx);

} // namespace settings
