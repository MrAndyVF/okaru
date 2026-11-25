#pragma once

// OKARU: Ajustes → Aplicaciones. Apps por defecto (mimeapps.list), extensiones por tipo MIME (globs2 de
// shared-mime-info), ocultar apps del lanzador y el paquete que instaló cada app (pacman, Flatpak, Snap).

#include "system/desktop_entry.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::apps {

  // Una categoría de "apps por defecto": tipos fijos y, si `prefix` no está vacío, todos los tipos que la app
  // elegida declara con ese prefijo ("image/").
  struct DefaultCategory {
    std::string_view key; // traducción settings.okaru.apps.default-<key>
    std::string_view glyph;
    std::vector<std::string_view> mimes;
    std::string_view prefix;
  };

  [[nodiscard]] std::span<const DefaultCategory> defaultCategories();

  // Apps visibles que declaran algún tipo de la categoría, por nombre.
  [[nodiscard]] std::vector<const DesktopEntry*> candidatesFor(const DefaultCategory& category);
  [[nodiscard]] bool handles(const DefaultCategory& category, const DesktopEntry& app);
  // Tipos a escribir al elegir `app` para la categoría.
  [[nodiscard]] std::vector<std::string> mimesToSet(const DefaultCategory& category, const DesktopEntry& app);

  // Los mimeapps.list de XDG leídos una vez, en orden de prioridad.
  struct DefaultsSnapshot {
    std::vector<std::string> texts;
    // Id (.desktop) por defecto para un tipo; vacío si no hay.
    [[nodiscard]] std::string get(std::string_view mime) const;
  };
  [[nodiscard]] DefaultsSnapshot loadDefaults();
  // Escribe [Default Applications] en ~/.config/mimeapps.list. Devuelve el error, si lo hubo.
  [[nodiscard]] std::optional<std::string> setDefault(const std::vector<std::string>& mimes, std::string_view desktopId);
  // Puro, para las pruebas: `text` con cada tipo apuntando a `desktopId` (o sin él si `desktopId` está vacío).
  [[nodiscard]] std::string setDefaultsInText(
      std::string_view text, const std::vector<std::string>& mimes, std::string_view desktopId
  );
  // Puro: valor de `mime` en [Default Applications] (primer id de la lista), vacío si no está.
  [[nodiscard]] std::string defaultInText(std::string_view text, std::string_view mime);

  // Extensiones (".png") de un tipo según globs2; vacío si no tiene.
  [[nodiscard]] std::vector<std::string> extensionsFor(std::string_view mime);
  // Puro: parsea un globs2 y añade a `out` las extensiones de `mime`.
  void extensionsFromGlobs(std::string_view globs, std::string_view mime, std::vector<std::string>& out);

  // Ocultar del lanzador: copia del .desktop en ~/.local/share/applications con NoDisplay=true.
  [[nodiscard]] bool hiddenByOkaru(const DesktopEntry& app);
  [[nodiscard]] std::optional<std::string> setHidden(const DesktopEntry& app, bool hidden);
  // Puro: inserta (hidden) o quita las líneas de ocultar en el grupo [Desktop Entry].
  [[nodiscard]] std::string hideInText(std::string_view text, bool hidden);

  enum class PackageManager : std::uint8_t { Pacman, Aur, Flatpak, Snap };

  struct PackageInfo {
    PackageManager manager = PackageManager::Pacman;
    std::string name;
    std::string version;
    std::string size;
  };

  // Consulta (síncrona, unos ms) el paquete dueño de la app; nullopt si no hay (AppImage, a mano…).
  [[nodiscard]] std::optional<PackageInfo> packageFor(const DesktopEntry& app);
  // Desinstala en segundo plano (pkexec para pacman y Snap); `done` corre en el hilo principal con el error.
  void uninstall(const PackageInfo& package, std::function<void(std::optional<std::string>)> done);

} // namespace okaru::apps
