#pragma once

// OKARU: ventana de okaru-edit, al estilo de GNOME Text Editor: pestañas, buscar y reemplazar, ir a línea, barra de
// estado (formato, posición, sangría, fin de línea), validación con el error marcado, dar formato, CSV como tabla,
// aviso de cambios sin guardar, archivos recientes, sesión recordada y archivos cambiados desde fuera.

#include "apps/editor/editor_view.h"
#include "apps/editor/formats.h"
#include "core/files/file_watcher.h"
#include "core/timer_manager.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Input;
class Label;
class Node;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class PortalFileChooser;
} // namespace okaru::apps

namespace okaru::editor {

  struct Document {
    std::filesystem::path path; // vacío = sin título
    int untitled = 0;           // número de "Sin título N"
    TextBuffer buffer;
    HighlightCache highlight;
    TextPos cursor;
    TextPos anchor;
    float scrollY = 0.0F;
    int tabWidth = 4;
    bool insertSpaces = true;
    bool latin1 = false; // se abrió como ISO-8859-1 (no era UTF-8 válido); se guarda en UTF-8
    std::optional<Problem> problem;
    std::uint64_t validatedVersion = static_cast<std::uint64_t>(-1);
    std::filesystem::file_time_type savedTime{};
    bool changedOnDisk = false;
    bool csvTable = false;
    FileWatcher::WatchId watch = 0;

    [[nodiscard]] Language language() const { return highlight.language(); }
  };

  struct EditorPrefs {
    float fontSize = 14.0F;
    bool wrap = true;
    bool lineNumbers = true;
    bool highlightLine = true;
    int tabWidth = 4;
    bool insertSpaces = true;
    bool restoreSession = true;
  };

  class EditorWindow {
  public:
    EditorWindow(apps::OkaruApp& app, const std::vector<std::filesystem::path>& files);
    ~EditorWindow();

  private:
    // Escena.
    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildTabs(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildBanner(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildFindBar(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildGotoBar(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildStatus(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildEmpty(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildCsvTable(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildPreferences(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildConfirm(float width, float height, float scale);
    [[nodiscard]] bool onKey(const KeyboardEvent& event);

    // Documentos.
    [[nodiscard]] Document* current();
    [[nodiscard]] std::string title(const Document& doc) const;
    void newDocument();
    bool
    openFile(const std::filesystem::path& path, std::optional<TextPos> cursor = std::nullopt, float scrollY = 0.0F);
    bool loadInto(Document& doc);
    bool saveDocument(Document& doc);
    void saveAs(std::size_t index, std::function<void()> then = {});
    void requestClose(std::size_t index);
    void closeDocument(std::size_t index);
    void requestQuit();
    void activate(std::size_t index);
    void watch(Document& doc);
    void onDiskChange(const std::filesystem::path& path);
    void openDialog();
    void rememberRecent(const std::filesystem::path& path);

    // Edición y estado.
    void captureView();
    void onTextChanged();
    void updateStatus();
    void validateNow();
    void formatDocument();
    void updateMatches();
    void findStep(bool forward);
    void replaceOne();
    void replaceEverything();
    void zoom(int steps);
    void setMessage(std::string message, bool error);
    [[nodiscard]] ViewOptions viewOptions(const Document& doc) const;

    void loadState();
    void saveState() const;

    apps::OkaruApp& m_app;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::vector<std::unique_ptr<Document>> m_docs;
    std::size_t m_active = 0;
    int m_nextUntitled = 1;
    EditorPrefs m_prefs;
    std::vector<std::filesystem::path> m_recent;

    // Barras y páginas.
    bool m_findOpen = false;
    bool m_replaceOpen = false;
    bool m_gotoOpen = false;
    bool m_showPreferences = false;
    std::string m_findText;
    std::string m_replaceText;
    SearchOptions m_search;
    std::vector<TextRange> m_matches;
    bool m_focusFind = false;
    std::string m_message;
    bool m_messageError = false;
    Timer m_messageTimer;

    // Cerrar con cambios sin guardar: documentos pendientes y si al final se sale de la app.
    std::vector<std::size_t> m_closing;
    bool m_quitAfterClosing = false;

    Timer m_validateTimer;
    EditorView* m_view = nullptr;
    Input* m_findInput = nullptr;
    Input* m_gotoInput = nullptr;
    Label* m_positionLabel = nullptr;
    Label* m_matchesLabel = nullptr;
    Label* m_problemLabel = nullptr;
    Label* m_titleLabel = nullptr;
  };

} // namespace okaru::editor
