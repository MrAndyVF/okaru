#pragma once

// OKARU: okaru-launcher, el lanzador de OKARU (estilo oma-cast). Una tarjeta con buscador, secciones («Sugerencias»,
// «Comandos», «Aplicaciones») y un pie con las teclas. Solo busca apps y unos pocos comandos de la shell; Enter enfoca
// la ventana abierta de la app (Shift+Enter abre otra). También muestra los atajos de teclado y las listas de
// `okaru dmenu` (proveedor acotado).

#include "launcher/launcher_provider.h"
#include "launcher/usage_tracker.h"
#include "shell/panel/panel.h"
#include "core/timer_manager.h"
#include "system/icon_resolver.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AsyncTextureCache;
class ConfigService;
class Flex;
class Input;
class Label;
class Renderer;
class VirtualListView;

namespace okaru::launcher {
  class RowsAdapter;
}

class OkaruLauncherPanel : public Panel {
public:
  OkaruLauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures);
  ~OkaruLauncherPanel() override;

  // Proveedores: AppProvider siempre; los de `okaru dmenu` se añaden y quitan en caliente.
  void addProvider(std::unique_ptr<LauncherProvider> provider);
  void clearProvidersWithIdPrefix(std::string_view prefix);
  // Limita la búsqueda a un proveedor (sesión de dmenu); vacío vuelve a la normal.
  void setScopedProvider(std::string_view providerId, std::string_view placeholder = {});
  // Tras activar algo que copió al portapapeles (dmenu en modo copiar): pegado automático.
  void setCopiedActivationCallback(std::function<void()> callback) { m_onCopiedActivation = std::move(callback); }
  // Tras lanzar o enfocar una app (cerrar el overview si estaba abierto).
  void setOnAppLaunchCallback(std::function<void()> callback) { m_onAppLaunch = std::move(callback); }
  void clearUsage();
  // Lee las apps y resuelve sus iconos poco después de arrancar la shell, para que la primera apertura sea tan rápida
  // como las siguientes.
  void schedulePrewarm();

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onIconThemeChanged() override;
  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  // Spotlight: la tarjeta sigue al contenido (solo la barra hasta escribir) y crece hacia abajo.
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return spotlight(); }
  [[nodiscard]] bool contentFitGrowsDown() const noexcept override { return spotlight(); }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  // Spotlight: casi sin relleno, para que la barra sola sea baja.
  [[nodiscard]] std::optional<float> contentPadding() const override {
    return spotlight() ? std::optional<float>(4.0F) : std::nullopt;
  }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  // Fila de la lista: título de sección o resultado (índice en m_results).
  struct Row {
    enum class Kind : std::uint8_t { Header, Result, Keybind } kind = Kind::Result;
    std::string label; // título de la sección, o descripción del atajo
    std::size_t result = 0;
    std::vector<std::string> keys; // teclas del atajo, ya con nombre legible
  };

  // Vista de atajos de teclado (comando «Atajos de teclado» o contexto ":keybinds" por IPC).
  void showKeybinds(bool direct);

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void onQueryChanged(const std::string& text);
  void rebuildRows();
  void rebuildKeybindRows();
  void showSearch();
  void syncChrome();
  void select(std::optional<std::size_t> row, bool scroll = true);
  void moveSelection(int delta);
  void activate(std::size_t row, bool newInstance);
  void finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied, bool appLaunch);
  [[nodiscard]] LauncherProvider* providerById(std::string_view id) const;
  [[nodiscard]] bool trackUsage() const;
  [[nodiscard]] bool spotlight() const noexcept;
  // Spotlight con el buscador vacío: solo la barra.
  [[nodiscard]] bool collapsed() const;
  void syncCollapsed();
  [[nodiscard]] std::unique_ptr<Node> buildFooter(float scale);

  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  IconResolver m_iconResolver;
  Timer m_prewarmTimer;
  UsageTracker m_usage;
  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  std::function<void()> m_onCopiedActivation;
  std::function<void()> m_onAppLaunch;

  std::string m_query;
  std::vector<LauncherResult> m_results;
  std::vector<Row> m_rows;
  std::optional<std::size_t> m_selected; // fila seleccionada (Result o Keybind)

  enum class Mode : std::uint8_t { Search, Keybinds } m_mode = Mode::Search;
  bool m_keybindsDirect = false; // abierta directo en atajos: Esc cierra en vez de volver
  struct KeybindEntry {
    std::string section;
    std::string description;
    std::vector<std::string> keys;
    std::string search; // descripción, sección y teclas en minúsculas
  };
  std::vector<KeybindEntry> m_keybinds;

  std::unique_ptr<okaru::launcher::RowsAdapter> m_adapter;
  Flex* m_container = nullptr;
  Input* m_input = nullptr;
  VirtualListView* m_list = nullptr;
  Label* m_empty = nullptr;
  Label* m_footerTitle = nullptr;
  Node* m_header = nullptr;
  std::vector<Node*> m_expandable; // lista, separadores y pie: se esconden en Spotlight sin escribir
  Flex* m_searchHints = nullptr;
  Flex* m_keybindHints = nullptr;
};
