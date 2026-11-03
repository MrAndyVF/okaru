#pragma once

// OKARU: Sonido → Dispositivos. Salidas y entradas con predeterminado, volumen, silencio y nombre
// personalizado ([audio.device_names], clave = node.name de PipeWire). Los sonidos de la shell están en la
// sección "sound" del registro (antes Servicios → Audio).

#include <functional>
#include <string>

class ConfigService;
class Flex;
class PipeWireService;

namespace settings {

  struct SettingsSoundContext {
    float scale = 1.0F;
    PipeWireService* audio = nullptr;
    ConfigService* config = nullptr;
    std::function<void()> requestRebuild;
  };

  void addSettingsSound(Flex& content, SettingsSoundContext ctx);

  // Normaliza un nombre personalizado: sin espacios en los extremos, a lo sumo 64 bytes sin cortar UTF-8.
  [[nodiscard]] std::string normalizeAudioDeviceAlias(std::string value);

} // namespace settings
