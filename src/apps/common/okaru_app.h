#pragma once

// OKARU: base de las apps propias (okaru-calc, okaru-view, okaru-play). Cada app es un binario aparte que
// enlaza okaru_core: abre una ventana normal (xdg_toplevel) con el UI, la fuente y la paleta de la shell, y la
// mantiene al día si cambia el tema (palette.json) o la config (fuente, idioma, escala).

#include "apps/common/file_drop.h"
#include "config/config_service.h"
#include "core/files/file_watcher.h"
#include "core/text_clipboard.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/select_dropdown_popup.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PollSource;
class Renderer;
class SessionBus;
class SessionBusPollSource;

namespace okaru::apps {

  // Copiar y pegar con wl-clipboard.
  // shortcut: un proceso por operación; basta para texto corto. Usar wl_data_device si se copia en ráfaga.
  class WlClipboard final : public TextClipboard {
  public:
    [[nodiscard]] std::optional<std::string> clipboardText() override;
    void setClipboardText(std::string text) override;
  };

  // Modificadores del teclado ahora mismo (KeyMod), para clics con Shift o Ctrl.
  [[nodiscard]] std::uint32_t keyboardModifiers() noexcept;

  struct AppOptions {
    const char* appId = "dev.okaru.App";
    std::string titleKey; // clave i18n del título; se traduce al abrir, con el idioma de la config
    std::string title;    // título literal (gana sobre titleKey), p. ej. el que pide una app al selector
    float width = 400.0F; // tamaño lógico, antes de accessibility.ui_scale
    float height = 600.0F;
    float minWidth = 320.0F;
    float minHeight = 400.0F;
  };

  class OkaruApp {
  public:
    // Construye toda la escena; se llama al abrir y con requestRebuild(). `scale` = accessibility.ui_scale.
    using BuildFn = std::function<std::unique_ptr<Node>(float width, float height, float scale)>;
    // Atajos de la app antes de pasarle la tecla al control con foco; true = consumida.
    using KeyFn = std::function<bool(const KeyboardEvent&)>;

    explicit OkaruApp(AppOptions options);
    ~OkaruApp();

    OkaruApp(const OkaruApp&) = delete;
    OkaruApp& operator=(const OkaruApp&) = delete;

    void setBuild(BuildFn build) { m_build = std::move(build); }
    void setKeyHandler(KeyFn handler) { m_keyHandler = std::move(handler); }
    // Llamado tras recargar config o paleta, antes de reconstruir.
    void setReloadCallback(std::function<void()> callback) { m_onReload = std::move(callback); }
    // En cada cuadro, con el contexto GL activo y antes de pintar (video: mpv dibuja aquí en su framebuffer).
    void setBeforeRender(std::function<void(Renderer&)> callback) { m_beforeRender = std::move(callback); }
    // Al cerrar, con el contexto GL todavía activo (liberar recursos de GPU propios).
    void setOnClose(std::function<void()> callback) { m_onClose = std::move(callback); }
    // El puntero entra (true) o sale (false) de la ventana.
    void setPointerPresenceCallback(std::function<void(bool inside)> callback) {
      m_pointerPresence = std::move(callback);
    }
    // Pide un cuadro aunque la escena no haya cambiado (llegó un cuadro de video).
    void requestFrame();
    [[nodiscard]] struct wl_display* wlDisplay() const noexcept;
    // Tras cada construcción, con la escena ya enganchada a la entrada (para dar foco).
    void setAfterBuild(std::function<void()> callback) { m_afterBuild = std::move(callback); }

    // Abre la ventana y corre el bucle hasta cerrarla. Devuelve el código de salida.
    int run();
    void quit();

    void requestRebuild();
    // Tras cambiar textos o estados de nodos ya construidos.
    void invalidate();
    void focus(InputArea* area);
    // Arrastrar desde este nodo mueve la ventana (la cabecera).
    void setDragNode(Node* node) { m_dragNode = node; }
    // Archivos soltados sobre la ventana (rutas locales o URL).
    void setDropCallback(FileDrop::DropFn callback) { m_drop.setOnDrop(std::move(callback)); }
    [[nodiscard]] FileDrop& fileDrop() noexcept { return m_drop; }
    // Arrastrar archivos hacia afuera (u otra parte de la misma ventana); `serial` del clic que lo empezó.
    bool startDrag(std::vector<std::string> uris, unsigned serial);

    void setFullscreen(bool fullscreen);
    // Al cerrar la ventana desde el compositor: false la deja abierta (p. ej. para preguntar por cambios sin guardar).
    void setCloseRequestCallback(std::function<bool()> callback) { m_closeRequest = std::move(callback); }
    [[nodiscard]] FileWatcher& fileWatcher() noexcept { return m_fileWatcher; }
    // Fuente extra del bucle (no se adueña; debe vivir hasta que run() termine), p. ej. el contexto de GLib.
    void addPollSource(PollSource* source) { m_extraSources.push_back(source); }
    [[nodiscard]] bool fullscreen() const noexcept { return m_fullscreen; }
    // Hay un desplegable de un Select abierto (es otra superficie: el puntero "sale" de la ventana al usarlo).
    [[nodiscard]] bool popupOpen() const { return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen(); }

    [[nodiscard]] ConfigService& config() noexcept { return m_config; }
    // Bus de sesión bajo demanda (portales); nullptr si no hay bus.
    [[nodiscard]] SessionBus* sessionBus();
    // Válido dentro de la construcción de la escena (contexto GL activo).
    [[nodiscard]] Renderer* renderer();
    // Píxeles físicos por píxel lógico de la ventana (escala fraccional incluida).
    [[nodiscard]] float bufferScale() const;
    [[nodiscard]] WlClipboard& clipboard() noexcept { return m_clipboard; }
    [[nodiscard]] float scale() const;
    [[nodiscard]] InputDispatcher& input() noexcept { return m_input; }

  private:
    void applyConfig();
    void applyPalette();
    void prepareFrame(bool needsUpdate, bool needsLayout);
    void onPointer(const PointerEvent& event);
    void onKey(const KeyboardEvent& event);
    [[nodiscard]] bool dragRegionContains(float x, float y) const;

    AppOptions m_options;
    ConfigService m_config;
    WaylandConnection m_wayland;
    FileDrop m_drop; // después de m_wayland: se destruye antes de cerrar la conexión
    GlSharedContext m_glShared;
    RenderContext m_render;
    AnimationManager m_animations;
    InputDispatcher m_input;
    FileWatcher m_fileWatcher;
    WlClipboard m_clipboard;
    std::unique_ptr<ToplevelSurface> m_surface;
    std::unique_ptr<SelectDropdownPopup> m_selectPopup; // desplegables de los Select
    std::unique_ptr<SessionBus> m_sessionBus;
    std::unique_ptr<SessionBusPollSource> m_sessionBusSource;
    std::unique_ptr<Node> m_root;
    std::vector<PollSource*> m_extraSources;
    Node* m_dragNode = nullptr;
    BuildFn m_build;
    KeyFn m_keyHandler;
    std::function<void()> m_onReload;
    std::function<void()> m_afterBuild;
    std::function<void(Renderer&)> m_beforeRender;
    std::function<void()> m_onClose;
    std::function<void(bool)> m_pointerPresence;
    std::function<bool()> m_closeRequest;
    std::atomic<bool> m_stop{false};
    bool m_rebuild = true;
    bool m_pointerInside = false;
    bool m_fullscreen = false;
  };

} // namespace okaru::apps
