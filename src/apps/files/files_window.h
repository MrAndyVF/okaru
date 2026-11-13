#pragma once

// OKARU: ventana de okaru-files. Pestañas, panel lateral (lugares, marcadores, dispositivos, red), barra de ruta
// editable, vistas de lista, cuadrícula y columnas, filtro y búsqueda recursiva, operaciones con progreso y
// conflictos, deshacer/rehacer, portapapeles compatible con otros exploradores, menú contextual y propiedades.

#include "apps/files/file_model.h"
#include "apps/files/file_ops.h"
#include "apps/files/file_view.h"
#include "apps/files/icon_cache.h"
#include "apps/files/places.h"
#include "apps/files/share.h"
#include "core/timer_manager.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Box;
class Image;
class Input;
class Label;
class Node;
class ProgressBar;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class GlibPollSource;
} // namespace okaru::apps

namespace okaru::files {

  // Columna de la vista en columnas: una carpeta antepasada con su selección.
  struct ColumnPane {
    std::unique_ptr<DirectoryModel> model;
    ViewState state;
  };

  struct Tab {
    std::unique_ptr<DirectoryModel> model = std::make_unique<DirectoryModel>();
    ViewState view;
    ViewMode mode = ViewMode::List;
    std::vector<std::string> back;
    std::vector<std::string> forward;
    std::vector<ColumnPane> columns; // modo columnas: de la raíz hasta la carpeta padre de la actual
  };

  class FilesWindow {
  public:
    FilesWindow(apps::OkaruApp& app, const std::vector<std::string>& locations);
    ~FilesWindow();

  private:
    enum class Overlay : std::uint8_t {
      None,
      Rename,
      NewFolder,
      NewFile,
      Conflict,
      ConfirmDelete,
      ConfirmEmptyTrash,
      Properties,
      OpenWith,
      Share,
      Errors,
    };
    enum class Mode : std::uint8_t { List, Grid, Columns };

    struct MenuEntry {
      std::string label;
      std::function<void()> action;
      bool enabled = true;
      bool separator = false;
      bool checked = false;
      bool check = false;
    };

    struct Properties {
      std::vector<std::string> uris;
      std::uint64_t bytes = 0;
      std::size_t files = 0;
      std::size_t folders = 0;
      bool counting = false;
      std::uint64_t free = 0;
      std::uint64_t total = 0;
    };

    struct OpenWithApp {
      std::string id;
      std::string name;
      std::string icon;
    };

    // Escena.
    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildPathBar(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildFindBar(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildSidebar(float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildBody(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<FileView> makeView(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildStatus(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildDetails(float scale);
    void updateDetails();
    [[nodiscard]] std::unique_ptr<Node> buildOverlay(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildMenu(float width, float height, float scale);
    [[nodiscard]] bool onKey(const KeyboardEvent& event);
    [[nodiscard]] bool textFocused() const;
    void updateStatus();

    // Navegación.
    [[nodiscard]] Tab& tab() { return *m_tabs[m_active]; }
    [[nodiscard]] Mode mode() const;
    void setMode(Mode mode);
    void openTab(const std::string& uri, bool activate = true);
    void closeTab(std::size_t index);
    void activateTab(std::size_t index);
    void navigate(const std::string& uri, bool record = true, const std::string& reveal = {});
    void goBack();
    void goForward();
    void goUp();
    void syncColumns();
    void onModelChanged();
    void reloadAll();

    // Acciones.
    [[nodiscard]] std::vector<const FileEntry*> selection() const;
    [[nodiscard]] std::vector<std::string> selectedUris() const;
    [[nodiscard]] FileView* activeView() const;
    [[nodiscard]] bool inTrash() const;
    [[nodiscard]] bool canWriteHere() const;
    void activateEntry(const FileEntry& entry, bool newTab = false);
    void openSelection();
    void openWithDialog();
    void shareDialog();
    void sendTo(const ShareTarget& target);
    void launchWith(const std::string& appId, bool makeDefault);
    void copyToClipboard(bool cut);
    void copyPaths();
    void paste(const std::string& destination);
    void startRename();
    void startNew(bool folder);
    void trashSelection();
    void deleteSelection();
    void restoreSelection();
    void emptyTrash();
    void createLink();
    void showProperties(std::vector<std::string> uris);
    void openTerminal(const std::string& uri);
    void newWindow(const std::string& uri);
    void toggleBookmark(const std::string& uri);
    void drop(const DropRequest& request);
    void dropExternal(const std::vector<std::string>& paths);
    [[nodiscard]] ViewOptions optionsFor(const std::string& uri) const;
    void changeSort(const std::function<void(ViewOptions&)>& change);
    void setSort(SortKey key);
    void toggleHidden();
    void zoom(int direction);
    void openContextMenu(std::optional<std::size_t> index, float x, float y);
    void openPlaceMenu(const Place& place, float x, float y);
    void openMainMenu(float x, float y);
    void showMenu(std::vector<MenuEntry> entries, float x, float y);
    void activatePlace(const Place& place, bool newTab = false);
    void setMessage(std::string message, bool error = false);
    void onOperationDone(OpResult result);
    void submit(OpRequest request);

    void loadState();
    void saveState();

    apps::OkaruApp& m_app;
    std::unique_ptr<apps::GlibPollSource> m_glib;
    Places m_places;
    OperationQueue m_ops;
    IconCache m_icons;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::size_t m_active = 0;

    // Preferencias (files.toml). De m_options solo cuenta showHidden (global); el orden va por carpeta.
    ViewOptions m_options;
    Mode m_defaultMode = Mode::List;
    float m_iconSize = 64.0F;
    bool m_sidebar = true;
    std::map<std::string, Mode> m_folderModes;
    std::map<std::string, ViewOptions> m_folderSorts; // orden elegido a mano por carpeta

    // Barra de ruta y búsqueda.
    bool m_editingPath = false;
    std::string m_pathText;
    bool m_findOpen = false;
    bool m_findRecursive = false;
    std::string m_findText;
    Timer m_searchTimer;

    // Portapapeles propio (para saber si lo copiado fue "cortar").
    std::vector<std::string> m_clipUris;
    bool m_clipCut = false;

    // Diálogos.
    Overlay m_overlay = Overlay::None;
    std::string m_dialogText;
    std::string m_dialogTarget; // URI a renombrar / carpeta destino
    std::vector<std::string> m_dialogUris;
    std::optional<Conflict> m_conflict;
    std::function<void(ConflictAnswer)> m_conflictReply;
    bool m_conflictAll = false;
    Properties m_properties;
    std::vector<OpenWithApp> m_openWith;
    bool m_openWithDefault = false;
    std::string m_openWithType;
    std::vector<std::string> m_errors;
    std::vector<ShareTarget> m_shareTargets;
    std::vector<std::string> m_sharePaths;
    bool m_shareSearching = false;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);

    // Menú contextual (capa dentro de la ventana).
    std::vector<MenuEntry> m_menu;
    float m_menuX = 0.0F;
    float m_menuY = 0.0F;

    // Progreso de operaciones y mensajes.
    std::optional<OpProgress> m_progress;
    std::string m_message;
    bool m_messageError = false;
    Timer m_messageTimer;

    // Nodos de la escena actual (válidos hasta la siguiente reconstrucción).
    FileView* m_view = nullptr;
    std::vector<std::pair<FileView*, std::size_t>> m_columnViews; // vista y su índice en Tab::columns
    bool m_syncingColumns = false;
    Input* m_pathInput = nullptr;
    Input* m_findInput = nullptr;
    Input* m_dialogInput = nullptr;
    Label* m_statusLabel = nullptr;
    ContextMenuControl* m_menuControl = nullptr;
    Image* m_detailsIcon = nullptr;
    Label* m_detailsName = nullptr;
    Label* m_detailsInfo = nullptr;
    float m_detailsScale = 1.0F;
    Label* m_progressLabel = nullptr;
    ProgressBar* m_progressBar = nullptr;
    ScrollViewState m_sidebarScroll;
    struct PlaceRow {
      Node* node = nullptr;
      Box* background = nullptr;
      std::string uri;
      bool active = false;
    };
    std::vector<PlaceRow> m_placeRows; // filas del panel lateral (soltar encima mientras se arrastra)
    std::string m_dropPlace;           // URI de la fila resaltada
    bool m_focusFind = false;
    bool m_placesReady = false;
    bool m_focusDialog = false;
    float m_width = 0.0F;
    float m_height = 0.0F;
  };

} // namespace okaru::files
