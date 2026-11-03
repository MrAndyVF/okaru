#pragma once

// OKARU: el dock se configura en un solo apartado. En vez de overrides por monitor, una opción
// "Mostrar en todas las pantallas" y, si está apagada, en qué pantalla se muestra (como en DMS).
// Por dentro usa los overrides `dock.monitor.<salida>.enabled` que el dock ya entiende.

#include <string>
#include <vector>

class ConfigService;
class Flex;

namespace settings {

  struct DockScreensState {
    bool allScreens = true;
    std::string screen; // con allScreens = false, la pantalla donde se muestra
  };

  // Lee el estado de las overrides `enabled` del dock para estas salidas.
  [[nodiscard]] DockScreensState readDockScreens(const ConfigService& config, const std::vector<std::string>& outputs);

  void addDockScreensCard(Flex& content, float scale, ConfigService& config, const std::vector<std::string>& outputs);

} // namespace settings
