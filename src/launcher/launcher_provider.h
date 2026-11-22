#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct LauncherResult {
  std::string id;
  std::string providerId; // Set by LauncherPanel after query; used for activation dispatch and usage tracking
  std::string title;
  // Optional source label for application-origin indicators and tooltips.
  std::string origin;
  std::string originGlyph;
  std::string subtitle;
  std::string glyphName;
  std::string iconName;
  std::string iconPath;
  // A short string drawn in the leading icon slot in place of an icon (e.g. an
  // emoji or a single symbol). When set, it replaces glyphName/iconName/iconPath.
  std::string badge;
  // AppProvider source desktop file path; empty for other providers.
  std::string desktopEntryPath;
  // When launching an application via AppProvider, matches DesktopAction::id (primary Exec leaves this empty).
  std::string desktopActionId;
  std::string category;
  std::string presentation;
  std::optional<std::string> query;
  double score = 0.0;
  int recentlyUsedIndex = 0; // Higher is more recent. <=0 means no record or too old.
  bool pinned = false;       // Set by LauncherPanel for launcher-owned pinned applications.
  bool newInstance = false;  // OKARU: Shift+Enter; sin él, AppProvider enfoca la ventana abierta de la app
};

// OKARU: lo que okaru-launcher necesita de un proveedor (apps, comandos y las listas de `okaru dmenu`).
class LauncherProvider {
public:
  virtual ~LauncherProvider() = default;

  // Identidad estable: clave del registro de uso y de la activación. No se traduce ni cambia.
  [[nodiscard]] virtual std::string_view id() const = 0;

  // Registrar el uso para ordenar por frecuencia y llenar «Sugerencias».
  [[nodiscard]] virtual bool trackUsage() const { return false; }

  // activate() copia al portapapeles y el lanzador debe pegar luego (shell.launcher.auto_paste).
  [[nodiscard]] virtual bool supportsAutoPaste() const { return false; }

  virtual void initialize() {}

  // Al abrir el lanzador, antes de la primera búsqueda.
  virtual void onLauncherOpened() {}

  // Al cerrar el lanzador. dmenu lo usa para avisar al script que se canceló.
  virtual void reset() {}

  [[nodiscard]] virtual std::vector<LauncherResult> query(std::string_view text) const = 0;

  // false si no se cerró el lanzador (por ejemplo, el comando que pasa a la vista de atajos).
  virtual bool activate(const LauncherResult& result) = 0;
};
