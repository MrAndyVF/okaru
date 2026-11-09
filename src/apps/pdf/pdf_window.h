#pragma once

// OKARU: ventana de okaru-pdf, al estilo de Papers: pestañas, barra lateral con miniaturas o índice, buscar con los
// resultados resaltados, zoom (ajustar al ancho o a la página), girar, doble página, modo nocturno, copiar texto,
// enlaces, contraseña, propiedades, imprimir con CUPS, presentación y la última página de cada archivo recordada.

#include "apps/pdf/pdf_view.h"
#include "core/timer_manager.h"
#include "ui/controls/scroll_view.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Image;
class Input;
class Label;
class Node;
class Renderer;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class PortalFileChooser;
} // namespace okaru::apps

namespace okaru::pdf {

  struct Remembered {
    int page = 0;
    Fit fit = Fit::Width;
    double zoom = 1.0;
    bool dual = false;
  };

  class PdfWindow {
  public:
    PdfWindow(apps::OkaruApp& app, const std::vector<std::filesystem::path>& files);
    ~PdfWindow();

  private:
    enum class Overlay : std::uint8_t { None, Password, Properties, Print };
    enum class SidebarMode : std::uint8_t { Thumbnails, Outline };

    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildSidebar(float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildFindBar(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildGotoBar(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildBanner(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildEmpty(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildOverlay(float width, float height, float scale);
    [[nodiscard]] bool onKey(const KeyboardEvent& event);

    [[nodiscard]] PdfTab* current();
    [[nodiscard]] std::string title(const PdfTab& tab) const;
    void openFile(const std::filesystem::path& path, const std::string& password = {});
    void openDialog();
    void closeTab(std::size_t index);
    void activate(std::size_t index);
    void rememberTab(const PdfTab& tab);
    void onRendered(PdfTab* tab, const RenderJob& job, RenderedPage page);
    void updatePageLabel();
    void updateThumbs(Renderer& renderer);

    void startSearch();
    void searchStep(int direction);
    void showHit();
    void updateMatchesLabel();
    void copySelection();
    void rotate(int degrees);
    void zoomStep(int direction);
    void togglePresentation();
    void openUri(const std::string& uri);
    void openPrint();
    void print();
    void setMessage(std::string message, bool error);

    void loadState();
    void saveState();

    apps::OkaruApp& m_app;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::vector<std::unique_ptr<PdfTab>> m_tabs;
    std::size_t m_active = 0;
    std::vector<std::filesystem::path> m_recent;
    std::map<std::string, Remembered> m_remembered;

    bool m_sidebar = true;
    bool m_night = false;
    SidebarMode m_sidebarMode = SidebarMode::Thumbnails;
    ScrollViewState m_sidebarScroll;
    bool m_presentation = false;
    bool m_findOpen = false;
    bool m_focusFind = false;
    bool m_gotoOpen = false;
    bool m_caseSensitive = false;
    bool m_wholeWords = false;

    Overlay m_overlay = Overlay::None;
    std::filesystem::path m_passwordPath;
    bool m_passwordWrong = false;
    std::string m_passwordText;

    // Imprimir.
    std::vector<std::string> m_printers;
    std::size_t m_printer = 0;
    int m_copies = 1;
    std::string m_printRange;
    std::size_t m_duplex = 0;
    bool m_printRangeInvalid = false;

    std::string m_message;
    bool m_messageError = false;
    Timer m_messageTimer;
    Timer m_searchTimer;

    PdfView* m_view = nullptr;
    Input* m_findInput = nullptr;
    Input* m_gotoInput = nullptr;
    Input* m_passwordInput = nullptr;
    Label* m_pageLabel = nullptr;
    Label* m_matchesLabel = nullptr;
    Label* m_zoomLabel = nullptr;
    std::vector<Image*> m_thumbImages;
    std::vector<Node*> m_thumbFrames;
    int m_thumbHighlighted = -1;
  };

} // namespace okaru::pdf
