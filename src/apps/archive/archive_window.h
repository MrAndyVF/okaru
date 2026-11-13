#pragma once

// OKARU: ventana de okaru-archive, el gestor de comprimidos. Muestra el contenido en árbol con la vista de lista de
// okaru-files, extrae (todo, la selección o lo que se arrastra hacia afuera), abre un archivo de dentro con su app,
// edita (agregar soltando, carpetas, renombrar, borrar), prueba la integridad, pide contraseñas (con el llavero) y
// crea comprimidos nuevos con nivel, contraseña y volúmenes.

#include "apps/archive/archive_backend.h"
#include "apps/files/file_model.h"
#include "apps/files/file_view.h"
#include "apps/files/icon_cache.h"
#include "core/timer_manager.h"
#include "ui/controls/context_menu.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Input;
class Label;
class Node;
class ProgressBar;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class PortalFileChooser;
} // namespace okaru::apps

namespace okaru::archive {

  class ArchiveWindow {
  public:
    // `archive`: el comprimido a abrir (puede ser vacío); `create`: archivos para un comprimido nuevo (--create).
    ArchiveWindow(apps::OkaruApp& app, const std::string& archive, const std::vector<std::string>& create);
    ~ArchiveWindow();

    ArchiveWindow(const ArchiveWindow&) = delete;
    ArchiveWindow& operator=(const ArchiveWindow&) = delete;

  private:
    enum class Overlay : std::uint8_t {
      None,
      Password,
      Create,
      Extract,
      ConfirmReplace,
      NewFolder,
      Rename,
      ConfirmDelete,
      Comment,
      Properties,
    };

    struct MenuEntry {
      std::string label;
      std::function<void()> action;
      bool enabled = true;
      bool separator = false;
    };

    using Work = std::function<Result(const std::string& password, const Progress& progress)>;
    using Done = std::function<void(const Result& result)>;

    // Escena.
    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildWelcome(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildStatus(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildOverlay(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildMenu(float width, float height, float scale);
    bool onKey(const KeyboardEvent& event);
    void showMenu(std::vector<MenuEntry> entries, float x, float y);
    void openContextMenu(std::optional<std::size_t> index, float x, float y);
    void openMainMenu(float x, float y);
    void setMessage(std::string message, bool error = false);
    void updateStatus();

    // Trabajo en un hilo con progreso; si falta la contraseña (o es mala) la pide y lo repite.
    void run(std::string label, const Work& work, const Done& done);
    void finish(const Result& result);

    // Comprimido abierto.
    void openArchive(const std::string& path);
    void chooseArchive();
    void reload();
    void refreshEntries();
    void navigate(std::string folder);
    void goUp();
    void activate(std::size_t index);
    [[nodiscard]] std::vector<std::string> selectedPaths() const;
    [[nodiscard]] bool writable() const;
    void openItem(const std::string& path);

    // Acciones.
    void extractDialog(bool selectionOnly);
    void chooseExtractFolder();
    void startExtract(bool confirmed);
    void addFiles(const std::vector<std::string>& files);
    void chooseFilesToAdd();
    void applyEdit(const Edit& changes, const std::string& doneMessage);
    void testArchive();
    void createDialog(std::vector<std::string> sources);
    void chooseFilesToCreate();
    void startCreate();
    void showProperties();
    bool startDrag(std::uint32_t serial);
    void dropAt(const std::vector<std::string>& paths, bool own);

    [[nodiscard]] std::string tempDir();
    void rememberPassword();
    [[nodiscard]] std::string keyringPassword() const;

    apps::OkaruApp& m_app;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);

    // Comprimido.
    std::string m_path;
    std::string m_password;
    std::vector<Item> m_items;
    std::string m_folder; // carpeta actual dentro del comprimido ("" = raíz)
    std::vector<files::FileEntry> m_entries;
    files::ViewState m_state;
    files::ViewOptions m_options;
    files::IconCache m_icons;
    files::FileView* m_view = nullptr;
    bool m_loaded = false;
    bool m_encrypted = false;
    std::string m_temp; // ~/.cache/okaru/archive/<pid>: archivos abiertos y arrastrados
    unsigned m_tempCounter = 0;

    // Trabajo en curso.
    bool m_busy = false;
    std::string m_busyLabel;
    std::atomic<double> m_fraction{0.0};
    std::atomic<bool> m_progressQueued{false};
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::function<void()> m_retry; // repetir el último trabajo (tras pedir la contraseña)

    // Escena y diálogos.
    Overlay m_overlay = Overlay::None;
    std::vector<MenuEntry> m_menu;
    float m_menuX = 0.0F;
    float m_menuY = 0.0F;
    ContextMenuControl* m_menuControl = nullptr;
    Input* m_dialogInput = nullptr;
    Label* m_statusLabel = nullptr;
    ProgressBar* m_progressBar = nullptr;
    std::string m_message;
    bool m_messageError = false;
    Timer m_messageTimer;
    float m_width = 0.0F;

    std::string m_dialogText;
    bool m_passwordWrong = false;
    bool m_rememberPassword = false;
    std::string m_renameFrom;

    // Extraer.
    std::string m_extractTo;
    bool m_extractOwnFolder = true;
    bool m_extractSelection = false;
    std::vector<std::string> m_extractPaths;
    std::vector<std::string> m_collisions;

    // Crear.
    std::vector<std::string> m_createFrom;
    std::size_t m_createFormat = 0;
    std::size_t m_createLevel = 2;
    std::size_t m_createVolume = 0;
    std::string m_createPassword;

    std::string m_comment;
  };

} // namespace okaru::archive
