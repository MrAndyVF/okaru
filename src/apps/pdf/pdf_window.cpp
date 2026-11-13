#include "apps/pdf/pdf_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "apps/common/tab_strip.h"
#include "apps/pdf/pdf_sidebar.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cmath>
#include <cursor-shape-v1-client-protocol.h>
#include <format>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>

namespace okaru::pdf {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;

    constexpr std::size_t kRecentLimit = 12;
    constexpr std::size_t kRememberLimit = 200;
    constexpr float kSidebarWidth = 200.0F;
    constexpr float kThumbWidth = 110.0F;
    constexpr std::array<int, 9> kZoomSteps{25, 50, 75, 100, 125, 150, 200, 300, 400};

    fs::path statePath() { return fs::path(FileUtils::stateDir()) / "pdf.toml"; }

    std::string fitId(Fit fit) { return fit == Fit::Width ? "width" : fit == Fit::Page ? "page" : "free"; }
    Fit fitFromId(std::string_view id) { return id == "page" ? Fit::Page : id == "free" ? Fit::Free : Fit::Width; }

  } // namespace

  PdfWindow::PdfWindow(apps::OkaruApp& app, const std::vector<fs::path>& files) : m_app(app) {
    loadState();
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setDropCallback([this](std::vector<std::string> paths) {
      for (const auto& path : paths) {
        openFile(path);
      }
    });
    m_app.setAfterBuild([this] {
      if (m_overlay == Overlay::Password && m_passwordInput != nullptr) {
        m_app.focus(m_passwordInput->inputArea());
      } else if (m_focusFind && m_findInput != nullptr) {
        m_app.focus(m_findInput->inputArea());
        m_focusFind = false;
      } else if (m_gotoOpen && m_gotoInput != nullptr) {
        m_app.focus(m_gotoInput->inputArea());
      } else if (m_view != nullptr && !m_findOpen && m_overlay == Overlay::None) {
        m_app.focus(m_view->inputArea());
      }
    });
    m_app.setBeforeRender([this](Renderer&) { updateThumbs(); });
    m_app.setOnClose([this] {
      // Las texturas se liberan con el contexto GL todavía activo.
      saveState();
      if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
        for (auto& tab : m_tabs) {
          for (auto* slots : {&tab->pages, &tab->thumbs}) {
            for (auto& slot : *slots) {
              if (slot.handle.valid()) {
                renderer->textureManager().unload(slot.handle);
              }
            }
          }
        }
      }
      m_tabs.clear();
    });
    // Dentro del bucle: el idioma se carga en run() y abrir puede mostrar mensajes traducidos.
    DeferredCall::callLater([this, files] {
      for (const auto& file : files) {
        openFile(file);
      }
    });
  }

  PdfWindow::~PdfWindow() = default;

  // ── Documentos ─────────────────────────────────────────────────────────────────────────────────────

  PdfTab* PdfWindow::current() { return m_active < m_tabs.size() ? m_tabs[m_active].get() : nullptr; }

  std::string PdfWindow::title(const PdfTab& tab) const {
    return tab.meta.title.empty() ? tab.path.filename().string() : tab.meta.title;
  }

  void PdfWindow::openFile(const fs::path& requested, const std::string& password) {
    std::error_code ec;
    const fs::path path = fs::weakly_canonical(fs::absolute(requested, ec), ec);
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
      if (m_tabs[i]->path == path) {
        activate(i);
        return;
      }
    }
    auto opened = Document::open(path, password);
    if (!opened) {
      if (opened.error() == OpenError::Damaged) {
        setMessage(tr("apps.pdf.error.open", "file", path.filename().string()), true);
        return;
      }
      // Protegido: se pide la contraseña y se vuelve a intentar con ella.
      m_passwordPath = path;
      m_passwordWrong = opened.error() == OpenError::WrongPassword;
      m_passwordText.clear();
      m_overlay = Overlay::Password;
      m_app.requestRebuild();
      return;
    }
    auto tab = std::make_unique<PdfTab>();
    tab->path = path;
    tab->password = password;
    tab->doc = std::move(*opened);
    const int pages = tab->doc->pageCount();
    for (int i = 0; i < pages; ++i) {
      tab->sizes.push_back(tab->doc->pageSize(i));
    }
    tab->pages.resize(static_cast<std::size_t>(pages));
    tab->thumbs.resize(static_cast<std::size_t>(pages));
    tab->outline = tab->doc->outline();
    tab->meta = tab->doc->metadata();
    tab->night = m_night;
    if (const auto it = m_remembered.find(path.string()); it != m_remembered.end()) {
      tab->fit = it->second.fit;
      tab->zoom = it->second.zoom;
      tab->dual = it->second.dual;
      tab->pendingPage = std::clamp(it->second.page, 0, std::max(0, pages - 1));
      tab->currentPage = *tab->pendingPage;
    }
    PdfTab* raw = tab.get();
    tab->worker = std::make_unique<RenderWorker>(path, password, [this, raw](const RenderJob& job, RenderedPage page) {
      onRendered(raw, job, std::move(page));
    });
    m_tabs.push_back(std::move(tab));
    m_active = m_tabs.size() - 1;
    m_sidebarScroll = {};
    // El índice de un libro se ve más útil que las miniaturas.
    m_sidebarMode = raw->outline.empty() ? SidebarMode::Thumbnails : m_sidebarMode;
    std::erase(m_recent, path);
    m_recent.insert(m_recent.begin(), path);
    if (m_recent.size() > kRecentLimit) {
      m_recent.resize(kRecentLimit);
    }
    m_app.requestRebuild();
  }

  void PdfWindow::onRendered(PdfTab* tab, const RenderJob& job, RenderedPage page) {
    // La pestaña pudo cerrarse mientras se dibujaba (su worker ya no existe, pero por si acaso).
    if (std::ranges::none_of(m_tabs, [tab](const auto& t) { return t.get() == tab; })) {
      return;
    }
    tab->arrived.emplace_back(job, std::move(page));
    if (tab == current() && m_view != nullptr) {
      m_view->markLayoutDirty();
      m_app.invalidate();
    }
  }

  void PdfWindow::openDialog() {
    SessionBus* bus = m_app.sessionBus();
    if (bus == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    m_chooser->open(
        tr("apps.pdf.open"), {{.name = tr("apps.pdf.filter"), .mimeTypes = {"application/pdf"}}}, true,
        [this](std::vector<std::string> paths) {
          for (const auto& path : paths) {
            openFile(path);
          }
        }
    );
  }

  void PdfWindow::rememberTab(const PdfTab& tab) {
    m_remembered[tab.path.string()] = {tab.currentPage, tab.fit, tab.zoom, tab.dual};
  }

  void PdfWindow::closeTab(std::size_t index) {
    if (index >= m_tabs.size()) {
      return;
    }
    PdfTab& tab = *m_tabs[index];
    rememberTab(tab);
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      for (auto* slots : {&tab.pages, &tab.thumbs}) {
        for (auto& slot : *slots) {
          if (slot.handle.valid()) {
            renderer->textureManager().unload(slot.handle);
          }
        }
      }
    }
    m_view = nullptr;
    m_sidebarList = nullptr;
    m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_active >= m_tabs.size()) {
      m_active = m_tabs.empty() ? 0 : m_tabs.size() - 1;
    } else if (m_active > index) {
      --m_active;
    }
    if (m_tabs.empty() && m_presentation) {
      togglePresentation();
    }
    m_app.requestRebuild();
  }

  void PdfWindow::activate(std::size_t index) {
    if (index >= m_tabs.size()) {
      return;
    }
    m_active = index;
    m_sidebarScroll = {};
    m_app.requestRebuild();
  }

  void PdfWindow::setMessage(std::string message, bool error) {
    m_message = std::move(message);
    m_messageError = error;
    m_messageTimer.start(std::chrono::seconds(error ? 6 : 3), [this] {
      m_message.clear();
      m_app.requestRebuild();
    });
    m_app.requestRebuild();
  }

  void PdfWindow::updatePageLabel() {
    PdfTab* tab = current();
    if (tab == nullptr) {
      return;
    }
    if (m_pageLabel != nullptr) {
      const std::string label = tab->doc->pageLabel(tab->currentPage);
      m_pageLabel->setText(
          label.empty()
              ? tr("apps.pdf.page", "page", tab->currentPage + 1, "total", tab->sizes.size())
              : tr("apps.pdf.page-label", "label", label, "page", tab->currentPage + 1, "total", tab->sizes.size())
      );
    }
    if (m_zoomLabel != nullptr && m_view != nullptr) {
      m_zoomLabel->setText(std::format("{}%", static_cast<int>(std::round(m_view->effectiveZoom() * 100.0))));
    }
    // La miniatura de la página actual se marca (la lista vuelve a dibujar esas dos filas).
    if (m_thumbHighlighted != tab->currentPage
        && m_sidebarList != nullptr
        && m_sidebarMode == SidebarMode::Thumbnails) {
      if (m_thumbHighlighted >= 0) {
        m_sidebarList->notifyItemChanged(static_cast<std::size_t>(m_thumbHighlighted));
      }
      m_sidebarList->notifyItemChanged(static_cast<std::size_t>(tab->currentPage));
    }
    m_thumbHighlighted = tab->currentPage;
    m_app.invalidate();
  }

  void PdfWindow::updateThumbs() {
    PdfTab* tab = current();
    if (tab == nullptr || m_sidebarList == nullptr || m_sidebarMode != SidebarMode::Thumbnails) {
      return;
    }
    // Miniaturas que llegaron (o se reemplazaron) desde la última vez.
    bool changed = false;
    for (std::size_t i = 0; i < m_thumbIds.size() && i < tab->thumbs.size(); ++i) {
      if (const std::uint32_t id = tab->thumbs[i].handle.id.value(); id != m_thumbIds[i]) {
        m_thumbIds[i] = id;
        m_sidebarList->notifyItemChanged(i);
        changed = true;
      }
    }
    if (changed) {
      m_app.invalidate();
    }
  }

  // ── Buscar ─────────────────────────────────────────────────────────────────────────────────────────

  void PdfWindow::startSearch() {
    PdfTab* tab = current();
    if (tab == nullptr) {
      return;
    }
    tab->hits.clear();
    tab->hitIndex = -1;
    tab->searching = !tab->query.empty();
    if (tab->query.empty()) {
      tab->worker->cancelSearch();
      updateMatchesLabel();
      if (m_view != nullptr) {
        m_view->markLayoutDirty();
      }
      m_app.invalidate();
      return;
    }
    // Desde la página actual: el primer resultado es el más cercano hacia adelante.
    const int first = tab->currentPage;
    tab->worker->search(
        tab->query, m_caseSensitive, m_wholeWords, first, [this, tab, first](std::vector<SearchHit> hits, bool done) {
          if (tab != current()) {
            return;
          }
          // Llegan desde `first` dando la vuelta; se guardan en orden de páginas.
          const bool firstHits = tab->hits.empty() && !hits.empty();
          tab->hits.insert(tab->hits.end(), hits.begin(), hits.end());
          std::ranges::stable_sort(tab->hits, {}, &SearchHit::page);
          if (firstHits) {
            const auto it = std::ranges::find_if(tab->hits, [first](const SearchHit& h) { return h.page >= first; });
            tab->hitIndex = static_cast<int>(it == tab->hits.end() ? 0 : it - tab->hits.begin());
            showHit();
          } else if (
              tab->hitIndex >= 0
              && !hits.empty()
              && hits.front().page < tab->hits[static_cast<std::size_t>(tab->hitIndex)].page
          ) {
            // Resultados de antes de la página actual (al dar la vuelta): el índice se corre.
            tab->hitIndex += static_cast<int>(hits.size());
          }
          tab->searching = !done;
          updateMatchesLabel();
          if (m_view != nullptr) {
            m_view->markLayoutDirty();
          }
          m_app.invalidate();
        }
    );
    updateMatchesLabel();
  }

  void PdfWindow::searchStep(int direction) {
    PdfTab* tab = current();
    if (tab == nullptr || tab->hits.empty()) {
      return;
    }
    const auto count = static_cast<int>(tab->hits.size());
    tab->hitIndex = ((tab->hitIndex + direction) % count + count) % count;
    showHit();
    updateMatchesLabel();
  }

  void PdfWindow::showHit() {
    PdfTab* tab = current();
    if (tab == nullptr || tab->hitIndex < 0 || m_view == nullptr) {
      return;
    }
    const SearchHit& hit = tab->hits[static_cast<std::size_t>(tab->hitIndex)];
    m_view->goToPage(hit.page, hit.rect);
    m_app.invalidate();
  }

  void PdfWindow::updateMatchesLabel() {
    PdfTab* tab = current();
    if (m_matchesLabel == nullptr || tab == nullptr) {
      return;
    }
    std::string text;
    if (!tab->query.empty()) {
      if (tab->hits.empty()) {
        text = tab->searching ? tr("apps.pdf.find.searching") : tr("apps.pdf.find.none");
      } else {
        text = tr("apps.pdf.find.count", "index", tab->hitIndex + 1, "total", tab->hits.size());
      }
    }
    m_matchesLabel->setText(text);
    m_app.invalidate();
  }

  // ── Acciones ───────────────────────────────────────────────────────────────────────────────────────

  void PdfWindow::copySelection() {
    PdfTab* tab = current();
    if (tab == nullptr) {
      return;
    }
    if (const std::string text = tab->selectedText(); !text.empty()) {
      m_app.clipboard().setClipboardText(text);
      setMessage(tr("apps.pdf.copied"), false);
    }
  }

  void PdfWindow::rotate(int degrees) {
    PdfTab* tab = current();
    if (tab == nullptr) {
      return;
    }
    tab->rotation = ((tab->rotation + degrees) % 360 + 360) % 360;
    tab->pendingPage = tab->currentPage;
    m_app.requestRebuild();
  }

  void PdfWindow::zoomStep(int direction) {
    PdfTab* tab = current();
    if (tab == nullptr || m_view == nullptr) {
      return;
    }
    const int now = static_cast<int>(std::round(m_view->effectiveZoom() * 100.0));
    int target = direction > 0 ? kZoomSteps.back() : kZoomSteps.front();
    if (direction > 0) {
      for (const int step : kZoomSteps) {
        if (step > now) {
          target = step;
          break;
        }
      }
    } else {
      for (auto it = kZoomSteps.rbegin(); it != kZoomSteps.rend(); ++it) {
        if (*it < now) {
          target = *it;
          break;
        }
      }
    }
    m_view->setZoom(target / 100.0);
    m_app.invalidate();
  }

  void PdfWindow::togglePresentation() {
    m_presentation = !m_presentation && current() != nullptr;
    m_app.setFullscreen(m_presentation);
    if (PdfTab* tab = current(); tab != nullptr) {
      tab->pendingPage = tab->currentPage;
    }
    m_app.requestRebuild();
  }

  void PdfWindow::openUri(const std::string& uri) {
    // Solo direcciones web y de correo: un PDF no debería poder lanzar cualquier cosa.
    if (uri.starts_with("http://") || uri.starts_with("https://") || uri.starts_with("mailto:")) {
      (void)process::runAsync({"xdg-open", uri.c_str()});
    } else {
      setMessage(tr("apps.pdf.link-blocked", "uri", uri), true);
    }
  }

  void PdfWindow::openPrint() {
    // Impresoras de CUPS: la predeterminada primero.
    m_printers.clear();
    const auto list = process::runSyncWithTimeout({"lpstat", "-e"}, std::chrono::seconds(3));
    std::istringstream lines(list.out);
    for (std::string line; std::getline(lines, line);) {
      if (!line.empty()) {
        m_printers.push_back(line);
      }
    }
    const auto def = process::runSyncWithTimeout({"lpstat", "-d"}, std::chrono::seconds(3));
    if (const auto colon = def.out.find(": "); colon != std::string::npos) {
      std::string name = def.out.substr(colon + 2);
      while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) {
        name.pop_back();
      }
      if (const auto it = std::ranges::find(m_printers, name); it != m_printers.end()) {
        std::rotate(m_printers.begin(), it, it + 1);
      }
    }
    m_printer = 0;
    m_printRangeInvalid = false;
    m_overlay = Overlay::Print;
    m_app.requestRebuild();
  }

  void PdfWindow::print() {
    PdfTab* tab = current();
    if (tab == nullptr || m_printers.empty()) {
      return;
    }
    const auto pages = static_cast<int>(tab->sizes.size());
    if (!m_printRange.empty() && parsePageRange(m_printRange, pages).empty()) {
      m_printRangeInvalid = true;
      m_app.requestRebuild();
      return;
    }
    static constexpr std::array<const char*, 3> kSides{"one-sided", "two-sided-long-edge", "two-sided-short-edge"};
    std::vector<std::string> args{
        "lp",
        "-d",
        m_printers[m_printer],
        "-n",
        std::to_string(m_copies),
        "-o",
        std::string("sides=") + kSides[std::min<std::size_t>(m_duplex, 2)]
    };
    if (!m_printRange.empty()) {
      args.insert(args.end(), {"-P", m_printRange});
    }
    args.emplace_back("--");
    args.push_back(tab->path.string());
    const std::string name = title(*tab);
    const bool started = process::runAsync(
        args, {.onExit = [this, name](process::RunResult result) {
          const bool ok = static_cast<bool>(result);
          DeferredCall::callLater([this, ok, name] {
            setMessage(ok ? tr("apps.pdf.print.sent", "file", name) : tr("apps.pdf.print.failed"), !ok);
          });
        }}
    );
    if (!started) {
      setMessage(tr("apps.pdf.print.failed"), true);
    }
    m_overlay = Overlay::None;
    m_app.requestRebuild();
  }

  // ── Escena ─────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> PdfWindow::build(float width, float height, float scale) {
    m_view = nullptr;
    m_findInput = nullptr;
    m_gotoInput = nullptr;
    m_passwordInput = nullptr;
    m_pageLabel = nullptr;
    m_matchesLabel = nullptr;
    m_zoomLabel = nullptr;
    m_sidebarList = nullptr;
    m_thumbHighlighted = -1;

    PdfTab* tab = current();
    auto root = ui::node({.width = width, .height = height});

    if (m_presentation && tab != nullptr) {
      // Presentación: negro, una página centrada; clic, flechas o espacio para avanzar, Esc para salir.
      root->addChild(ui::box({.fill = fixedColorSpec(rgba(0.0F, 0.0F, 0.0F, 1.0F)), .width = width, .height = height}));
      auto view = std::make_unique<PdfView>();
      m_view = view.get();
      view->setBufferScale(m_app.bufferScale());
      view->setTab(tab);
      view->setPresentation(true);
      view->setOnAdvance([this](int direction) {
        if (PdfTab* t = current(); t != nullptr && m_view != nullptr) {
          m_view->goToPage(t->currentPage + direction);
          m_app.invalidate();
        }
      });
      view->setSize(width, height);
      root->addChild(std::move(view));
      return root;
    }

    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    auto main = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .width = width, .height = height});
    Node* header = main->addChild(buildHeader(scale));
    m_app.setDragNode(header);

    if (tab == nullptr) {
      main->addChild(buildEmpty(scale));
    } else {
      if (m_tabs.size() > 1) {
        apps::TabStripProps props{
            .active = m_active,
            .closeTooltip = tr("apps.pdf.close-tab"),
            .onActivate = [this](std::size_t i) { activate(i); },
            .onClose = [this](std::size_t i) { closeTab(i); },
        };
        for (const auto& t : m_tabs) {
          props.tabs.push_back({title(*t), t->path.string()});
        }
        main->addChild(apps::tabStrip(m_app.renderer(), width, scale, std::move(props)));
      }
      if (auto banner = buildBanner(scale)) {
        main->addChild(std::move(banner));
      }
      if (m_findOpen) {
        main->addChild(buildFindBar(scale));
      }
      if (m_gotoOpen) {
        main->addChild(buildGotoBar(scale));
      }
      auto body = ui::row({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
      if (m_sidebar) {
        body->addChild(buildSidebar(height, scale));
      }
      auto view = std::make_unique<PdfView>();
      m_view = view.get();
      view->setBufferScale(m_app.bufferScale());
      view->setTab(tab);
      view->setOnPageChanged([this] { updatePageLabel(); });
      view->setOnZoomChanged([this] { updatePageLabel(); });
      view->setOnOpenUri([this](const std::string& uri) { openUri(uri); });
      view->setFlexGrow(1.0F);
      body->addChild(std::move(view));
      main->addChild(std::move(body));
    }
    root->addChild(std::move(main));
    if (m_overlay != Overlay::None) {
      root->addChild(buildOverlay(width, height, scale));
    }
    updatePageLabel();
    updateMatchesLabel();
    return root;
  }

  std::unique_ptr<Node> PdfWindow::buildHeader(float scale) {
    const auto ghost = [&](const char* glyph, std::string tooltip, std::function<void()> onClick, bool on = false) {
      return ui::button({
          .glyph = glyph,
          .selected = on,
          .variant = on ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };
    PdfTab* tab = current();
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .height = 52.0F * scale,
    });
    if (tab != nullptr) {
      header->addChild(ghost(
          "layout-sidebar", tr("apps.pdf.sidebar"),
          [this] {
            m_sidebar = !m_sidebar;
            m_app.requestRebuild();
          },
          m_sidebar
      ));
    }
    header->addChild(ghost("folder-open", tr("apps.pdf.open"), [this] { openDialog(); }));

    auto titles =
        ui::column({.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = tab == nullptr ? tr("apps.pdf.title") : title(*tab),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 1,
            .ellipsize = TextEllipsize::Middle,
        })
    );
    if (tab != nullptr) {
      // Página actual; clic para ir a otra (Ctrl+L).
      auto page = ui::label({
          .out = &m_pageLabel,
          .text = " ",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      });
      auto pageArea = ui::inputArea({
          .tooltip = tr("apps.pdf.goto.title"),
          .width = 240.0F * scale,
          .height = Style::fontSizeCaption * 1.5F * scale,
          .onClick = [this](const InputArea::PointerData&) {
            m_gotoOpen = !m_gotoOpen;
            m_app.requestRebuild();
          },
      });
      pageArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      page->addChild(std::move(pageArea));
      titles->addChild(std::move(page));
    }
    header->addChild(std::move(titles));

    if (tab != nullptr) {
      header->addChild(ghost(
          "search", tr("apps.pdf.find.title"),
          [this] {
            m_findOpen = !m_findOpen;
            m_focusFind = m_findOpen;
            m_app.requestRebuild();
          },
          m_findOpen
      ));
      header->addChild(ghost("zoom-out", tr("apps.pdf.zoom.out"), [this] { zoomStep(-1); }));
      // Porcentaje; clic alterna entre ajustar al ancho y a la página.
      header->addChild(
          ui::button({
              .text = "100%",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = tab->fit == Fit::Width ? tr("apps.pdf.zoom.fit-page") : tr("apps.pdf.zoom.fit-width"),
              .minWidth = 64.0F * scale,
              .onClick =
                  [this] {
                    if (PdfTab* t = current(); t != nullptr && m_view != nullptr) {
                      m_view->setFit(t->fit == Fit::Width ? Fit::Page : Fit::Width);
                      m_app.requestRebuild();
                    }
                  },
              .configure = [this](Button& button) { m_zoomLabel = button.label(); },
          })
      );
      header->addChild(ghost("zoom-in", tr("apps.pdf.zoom.in"), [this] { zoomStep(1); }));
      header->addChild(ghost("rotate-clockwise", tr("apps.pdf.rotate"), [this] { rotate(90); }));
      header->addChild(ghost(
          "book", tr("apps.pdf.dual"),
          [this] {
            if (PdfTab* t = current(); t != nullptr) {
              t->dual = !t->dual;
              t->pendingPage = t->currentPage;
              m_app.requestRebuild();
            }
          },
          tab->dual
      ));
      header->addChild(ghost(
          "moon", tr("apps.pdf.night"),
          [this] {
            m_night = !m_night;
            for (auto& t : m_tabs) {
              t->night = m_night;
            }
            m_app.requestRebuild();
          },
          m_night
      ));
      header->addChild(ghost("presentation", tr("apps.pdf.presentation"), [this] { togglePresentation(); }));
      header->addChild(ghost("printer", tr("apps.pdf.print.tooltip"), [this] { openPrint(); }));
      header->addChild(ghost("info-circle", tr("apps.pdf.properties.tooltip"), [this] {
        m_overlay = Overlay::Properties;
        m_app.requestRebuild();
      }));
    }
    header->addChild(ghost("x", tr("apps.pdf.close"), [this] { m_app.quit(); }));
    return header;
  }

  std::unique_ptr<Node> PdfWindow::buildSidebar(float height, float scale) {
    PdfTab* tab = current();
    auto sidebar = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .paddingV = Style::spaceXs * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .width = kSidebarWidth * scale,
    });
    (void)height;
    if (!tab->outline.empty()) {
      sidebar->addChild(
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale},
              ui::segmented({
                  .options =
                      std::vector<ui::SegmentedOption>{
                          {.glyph = "layout-grid", .tooltip = tr("apps.pdf.thumbnails")},
                          {.glyph = "list", .tooltip = tr("apps.pdf.outline")},
                      },
                  .selectedIndex = m_sidebarMode == SidebarMode::Thumbnails ? 0U : 1U,
                  .scale = scale,
                  .onChange = [this](std::size_t index) {
                    m_sidebarMode = index == 0 ? SidebarMode::Thumbnails : SidebarMode::Outline;
                    m_sidebarScroll = {};
                    m_app.requestRebuild();
                  },
              })
          )
      );
    }
    // Lista virtual: solo existen las filas visibles. El adaptador anterior sigue vivo hasta la próxima
    // construcción (la escena vieja se destruye después de armar la nueva).
    m_retiredAdapter = std::move(m_adapter);
    const auto goTo = [this](int page) {
      if (page >= 0 && m_view != nullptr) {
        m_view->goToPage(page);
        m_app.invalidate();
      }
    };
    if (m_sidebarMode == SidebarMode::Outline && !tab->outline.empty()) {
      m_adapter = std::make_unique<OutlineAdapter>(*tab, scale, goTo);
    } else {
      m_adapter = std::make_unique<ThumbnailAdapter>(*tab, scale, goTo);
    }
    auto list = ui::virtualListView({
        .out = &m_sidebarList,
        .state = &m_sidebarScroll,
        .contentScale = scale,
        .itemGap = m_sidebarMode == SidebarMode::Outline ? 0.0F : Style::spaceXs * scale,
        .adapter = m_adapter.get(),
        // Ancho fijo: la lista virtual mide poco y la vista de páginas se desbordaba por la derecha.
        .width = kSidebarWidth * scale,
        .flexGrow = 1.0F,
    });
    m_thumbIds.assign(tab->thumbs.size(), 0);
    for (std::size_t i = 0; i < tab->thumbs.size(); ++i) {
      m_thumbIds[i] = tab->thumbs[i].handle.id.value();
    }
    m_thumbHighlighted = tab->currentPage;
    // Al abrir, la miniatura de la página actual a la vista.
    if (m_sidebarMode == SidebarMode::Thumbnails && m_sidebarScroll.offset == 0.0F && tab->currentPage > 0) {
      m_sidebarList->scrollToIndex(static_cast<std::size_t>(tab->currentPage));
    }
    sidebar->addChild(std::move(list));
    return sidebar;
  }

  std::unique_ptr<Node> PdfWindow::buildFindBar(float scale) {
    PdfTab* tab = current();
    const auto toggle = [&](const char* glyph, std::string tooltip, bool on, std::function<void()> onClick) {
      return ui::button({
          .glyph = glyph,
          .selected = on,
          .variant = on ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };
    auto bar = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
    });
    bar->addChild(
        ui::input({
            .out = &m_findInput,
            .value = tab->query,
            .placeholder = tr("apps.pdf.find.placeholder"),
            .flexGrow = 1.0F,
            .onChange =
                [this](const std::string& value) {
                  if (PdfTab* t = current(); t != nullptr) {
                    t->query = value;
                    // Se espera a que se deje de escribir: cada búsqueda recorre todo el documento.
                    m_searchTimer.start(std::chrono::milliseconds(250), [this] { startSearch(); });
                  }
                },
            .onSubmit = [this](const std::string&) { searchStep(1); },
            .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) -> bool {
              if (sym == XKB_KEY_Escape) {
                m_findOpen = false;
                m_app.requestRebuild();
                return true;
              }
              if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && (modifiers & KeyMod::Shift) != 0) {
                searchStep(-1);
                return true;
              }
              return false;
            },
        })
    );
    bar->addChild(
        ui::label({
            .out = &m_matchesLabel,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = 90.0F * scale,
            .textAlign = TextAlign::Center,
        })
    );
    bar->addChild(
        ui::button(
            {.glyph = "chevron-up",
             .variant = ButtonVariant::Ghost,
             .tooltip = tr("apps.pdf.find.previous"),
             .onClick = [this] { searchStep(-1); }}
        )
    );
    bar->addChild(
        ui::button(
            {.glyph = "chevron-down",
             .variant = ButtonVariant::Ghost,
             .tooltip = tr("apps.pdf.find.next"),
             .onClick = [this] { searchStep(1); }}
        )
    );
    bar->addChild(toggle("letter-case", tr("apps.pdf.find.case"), m_caseSensitive, [this] {
      m_caseSensitive = !m_caseSensitive;
      startSearch();
      m_app.requestRebuild();
    }));
    bar->addChild(toggle("text-size", tr("apps.pdf.find.word"), m_wholeWords, [this] {
      m_wholeWords = !m_wholeWords;
      startSearch();
      m_app.requestRebuild();
    }));
    bar->addChild(
        ui::button({.glyph = "x", .variant = ButtonVariant::Ghost, .tooltip = tr("apps.pdf.close"), .onClick = [this] {
                      m_findOpen = false;
                      m_app.requestRebuild();
                    }})
    );
    return bar;
  }

  std::unique_ptr<Node> PdfWindow::buildGotoBar(float scale) {
    PdfTab* tab = current();
    return ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingV = Style::spaceXs * scale,
         .paddingH = Style::spaceMd * scale,
         .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow)},
        ui::label({.text = tr("apps.pdf.goto.title"), .fontSize = Style::fontSizeCaption * scale}),
        ui::input({
            .out = &m_gotoInput,
            .placeholder = tr("apps.pdf.goto.placeholder", "count", tab->sizes.size()),
            .width = 160.0F * scale,
            .onSubmit =
                [this](const std::string& value) {
                  PdfTab* t = current();
                  if (t == nullptr || m_view == nullptr) {
                    return;
                  }
                  // Número de página, o la etiqueta impresa ("iv").
                  int page = -1;
                  if (const auto pages = parsePageRange(value, static_cast<int>(t->sizes.size())); pages.size() == 1) {
                    page = pages.front();
                  } else {
                    for (int i = 0; i < static_cast<int>(t->sizes.size()); ++i) {
                      if (t->doc->pageLabel(i) == value) {
                        page = i;
                        break;
                      }
                    }
                  }
                  if (page >= 0) {
                    m_gotoOpen = false;
                    t->pendingPage = page;
                    m_app.requestRebuild();
                  }
                },
            .onKeyEvent = [this](std::uint32_t sym, std::uint32_t) -> bool {
              if (sym == XKB_KEY_Escape) {
                m_gotoOpen = false;
                m_app.requestRebuild();
                return true;
              }
              return false;
            },
        }),
        ui::node({.flexGrow = 1.0F})
    );
  }

  std::unique_ptr<Node> PdfWindow::buildBanner(float scale) {
    if (m_message.empty()) {
      return nullptr;
    }
    return ui::row(
        {.align = FlexAlign::Center,
         .paddingV = Style::spaceXs * scale,
         .paddingH = Style::spaceMd * scale,
         .fill = colorSpecFromRole(m_messageError ? ColorRole::ErrorContainer : ColorRole::SecondaryContainer)},
        ui::label({
            .text = m_message,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(m_messageError ? ColorRole::OnErrorContainer : ColorRole::OnSecondaryContainer),
            .flexGrow = 1.0F,
        })
    );
  }

  std::unique_ptr<Node> PdfWindow::buildEmpty(float scale) {
    auto page = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .flexGrow = 1.0F,
    });
    if (auto banner = buildBanner(scale)) {
      page->addChild(std::move(banner));
    }
    page->addChild(
        ui::glyph(
            {.glyph = "file-type-pdf",
             .glyphSize = 64.0F * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    page->addChild(
        ui::label(
            {.text = tr("apps.pdf.empty.title"),
             .fontSize = Style::fontSizeTitle * scale,
             .fontWeight = FontWeight::SemiBold}
        )
    );
    page->addChild(
        ui::button(
            {.text = tr("apps.pdf.open"), .glyph = "folder-open", .variant = ButtonVariant::Primary, .onClick = [this] {
               openDialog();
             }}
        )
    );
    std::vector<fs::path> recent;
    for (const auto& path : m_recent) {
      if (std::error_code ec; fs::exists(path, ec)) {
        recent.push_back(path);
      }
    }
    if (!recent.empty()) {
      page->addChild(
          ui::label({
              .text = tr("apps.pdf.recent"),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      for (const auto& path : recent) {
        page->addChild(
            ui::button({
                .text = path.filename().string(),
                .contentAlign = ButtonContentAlign::Start,
                .variant = ButtonVariant::Ghost,
                .tooltip = path.string(),
                .minWidth = 320.0F * scale,
                .onClick = [this, path] { openFile(path); },
            })
        );
      }
      page->addChild(
          ui::button({
              .text = tr("apps.pdf.recent-clear"),
              .glyph = "trash",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .onClick = [this] {
                DeferredCall::callLater([this] {
                  m_recent.clear();
                  saveState();
                  m_app.requestRebuild();
                });
              },
          })
      );
    }
    return page;
  }

  std::unique_ptr<Node> PdfWindow::buildOverlay(float width, float height, float scale) {
    auto overlay = ui::node({.width = width, .height = height, .zIndex = 10});
    overlay->addChild(ui::inputArea({.width = width, .height = height}));
    overlay->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Scrim, 0.45F), .width = width, .height = height}));
    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .radius = Style::radiusLg * scale,
        .width = std::min(460.0F * scale, width - 32.0F),
    });
    const auto heading = [&](std::string text) {
      card->addChild(
          ui::label({
              .text = std::move(text),
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::SemiBold,
              .maxLines = 2,
          })
      );
    };
    const auto close = [this] {
      m_overlay = Overlay::None;
      m_app.requestRebuild();
    };
    const auto field = [&](std::string name, std::unique_ptr<Node> control) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
              ui::label({
                  .text = std::move(name),
                  .fontSize = Style::fontSizeBody * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .minWidth = 120.0F * scale,
              }),
              std::move(control)
          )
      );
    };

    if (m_overlay == Overlay::Password) {
      heading(tr("apps.pdf.password.title"));
      card->addChild(
          ui::label({
              .text = m_passwordWrong ? tr("apps.pdf.password.wrong")
                                      : tr("apps.pdf.password.body", "file", m_passwordPath.filename().string()),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(m_passwordWrong ? ColorRole::Error : ColorRole::OnSurfaceVariant),
              .maxLines = 3,
          })
      );
      const auto submit = [this] {
        m_overlay = Overlay::None;
        openFile(m_passwordPath, m_passwordText);
        m_passwordText.clear();
        m_app.requestRebuild();
      };
      card->addChild(
          ui::input({
              .out = &m_passwordInput,
              .placeholder = tr("apps.pdf.password.placeholder"),
              .passwordMode = true,
              .onChange = [this](const std::string& value) { m_passwordText = value; },
              .onSubmit = [submit](const std::string&) { submit(); },
              .onKeyEvent = [close](std::uint32_t sym, std::uint32_t) -> bool {
                if (sym == XKB_KEY_Escape) {
                  close();
                  return true;
                }
                return false;
              },
          })
      );
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
              ui::button({.text = tr("apps.pdf.cancel"), .variant = ButtonVariant::Ghost, .onClick = close}),
              ui::button({.text = tr("apps.pdf.password.unlock"), .variant = ButtonVariant::Primary, .onClick = submit})
          )
      );
    } else if (m_overlay == Overlay::Properties) {
      PdfTab* tab = current();
      heading(tr("apps.pdf.properties.title"));
      const Metadata& meta = tab->meta;
      std::error_code ec;
      const auto bytes = fs::file_size(tab->path, ec);
      const PageSize size = tab->sizes.empty() ? PageSize{} : tab->sizes.front();
      const std::vector<std::pair<std::string, std::string>> rows{
          {tr("apps.pdf.properties.file"), tab->path.string()},
          {tr("apps.pdf.properties.doc-title"), meta.title},
          {tr("apps.pdf.properties.author"), meta.author},
          {tr("apps.pdf.properties.subject"), meta.subject},
          {tr("apps.pdf.properties.keywords"), meta.keywords},
          {tr("apps.pdf.properties.creator"), meta.creator},
          {tr("apps.pdf.properties.producer"), meta.producer},
          {tr("apps.pdf.properties.created"), meta.created},
          {tr("apps.pdf.properties.modified"), meta.modified},
          {tr("apps.pdf.properties.pages"), std::to_string(tab->sizes.size())},
          // Tamaño de la primera página en milímetros (1 pt = 25.4/72 mm).
          {tr("apps.pdf.properties.paper"),
           std::format("{:.0f} × {:.0f} mm", size.width * 25.4 / 72.0, size.height * 25.4 / 72.0)},
          {tr("apps.pdf.properties.version"), meta.version},
          {tr("apps.pdf.properties.size"),
           ec ? std::string() : std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6)},
      };
      for (const auto& [name, value] : rows) {
        if (value.empty()) {
          continue;
        }
        card->addChild(
            ui::row(
                {.align = FlexAlign::Start, .gap = Style::spaceMd * scale},
                ui::label({
                    .text = name,
                    .fontSize = Style::fontSizeCaption * scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .minWidth = 120.0F * scale,
                }),
                ui::label({
                    .text = value,
                    .fontSize = Style::fontSizeCaption * scale,
                    .maxLines = 3,
                    .ellipsize = TextEllipsize::Middle,
                    .flexGrow = 1.0F,
                })
            )
        );
      }
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::End},
              ui::button({.text = tr("apps.pdf.close"), .variant = ButtonVariant::Primary, .onClick = close})
          )
      );
    } else if (m_overlay == Overlay::Print) {
      heading(tr("apps.pdf.print.title"));
      if (m_printers.empty()) {
        card->addChild(
            ui::label({
                .text = tr("apps.pdf.print.no-printers"),
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 3,
            })
        );
        card->addChild(
            ui::row(
                {.align = FlexAlign::Center, .justify = FlexJustify::End},
                ui::button({.text = tr("apps.pdf.close"), .variant = ButtonVariant::Primary, .onClick = close})
            )
        );
      } else {
        field(
            tr("apps.pdf.print.printer"),
            ui::select({
                .options = m_printers,
                .selectedIndex = m_printer,
                .flexGrow = 1.0F,
                .onSelectionChanged = [this](std::size_t index, std::string_view) { m_printer = index; },
            })
        );
        field(
            tr("apps.pdf.print.copies"),
            ui::stepper({
                .minValue = 1,
                .maxValue = 99,
                .value = m_copies,
                .scale = scale,
                .onValueChanged = [this](int value) { m_copies = value; },
            })
        );
        field(
            tr("apps.pdf.print.pages"),
            ui::input({
                .value = m_printRange,
                .placeholder = tr("apps.pdf.print.all"),
                .invalid = m_printRangeInvalid,
                .flexGrow = 1.0F,
                .onChange = [this](const std::string& value) { m_printRange = value; },
            })
        );
        field(
            tr("apps.pdf.print.sides"),
            ui::select({
                .options =
                    std::vector<std::string>{
                        tr("apps.pdf.print.one-sided"),
                        tr("apps.pdf.print.long-edge"),
                        tr("apps.pdf.print.short-edge"),
                    },
                .selectedIndex = m_duplex,
                .flexGrow = 1.0F,
                .onSelectionChanged = [this](std::size_t index, std::string_view) { m_duplex = index; },
            })
        );
        card->addChild(
            ui::row(
                {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
                ui::button({.text = tr("apps.pdf.cancel"), .variant = ButtonVariant::Ghost, .onClick = close}),
                ui::button(
                    {.text = tr("apps.pdf.print.button"),
                     .glyph = "printer",
                     .variant = ButtonVariant::Primary,
                     .onClick = [this] { print(); }}
                )
            )
        );
      }
    }
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      card->layout(*renderer);
    }
    card->setPosition((width - card->width()) * 0.5F, (height - card->height()) * 0.4F);
    overlay->addChild(std::move(card));
    return overlay;
  }

  // ── Teclado ─────────────────────────────────────────────────────────────────────────────────────────

  bool PdfWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    const bool alt = (event.modifiers & KeyMod::Alt) != 0;
    if (m_overlay != Overlay::None) {
      if (event.sym == XKB_KEY_Escape) {
        m_overlay = Overlay::None;
        m_app.requestRebuild();
        return true;
      }
      return false;
    }
    PdfTab* tab = current();
    if (m_presentation) {
      // Todo el teclado es de la presentación.
      int step = 0;
      switch (event.sym) {
      case XKB_KEY_Escape:
      case XKB_KEY_F5:
        togglePresentation();
        return true;
      case XKB_KEY_Right:
      case XKB_KEY_Down:
      case XKB_KEY_Page_Down:
      case XKB_KEY_space:
      case XKB_KEY_Return:
        step = 1;
        break;
      case XKB_KEY_Left:
      case XKB_KEY_Up:
      case XKB_KEY_Page_Up:
      case XKB_KEY_BackSpace:
        step = -1;
        break;
      case XKB_KEY_Home:
        step = -tab->currentPage;
        break;
      case XKB_KEY_End:
        step = static_cast<int>(tab->sizes.size()) - 1 - tab->currentPage;
        break;
      default:
        return true;
      }
      if (m_view != nullptr) {
        m_view->goToPage(tab->currentPage + step);
        m_app.invalidate();
      }
      return true;
    }
    if (event.sym == XKB_KEY_F3 || (ctrl && (event.sym == XKB_KEY_g || event.sym == XKB_KEY_G))) {
      searchStep(shift ? -1 : 1);
      return true;
    }
    if (event.sym == XKB_KEY_F5 && tab != nullptr) {
      togglePresentation();
      return true;
    }
    if (event.sym == XKB_KEY_F9 && tab != nullptr) {
      m_sidebar = !m_sidebar;
      m_app.requestRebuild();
      return true;
    }
    if (event.sym == XKB_KEY_F11) {
      m_app.setFullscreen(!m_app.fullscreen());
      return true;
    }
    if (alt && (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) && tab != nullptr) {
      m_overlay = Overlay::Properties;
      m_app.requestRebuild();
      return true;
    }
    if (event.sym == XKB_KEY_Escape && (m_findOpen || m_gotoOpen)) {
      m_findOpen = m_gotoOpen = false;
      m_app.requestRebuild();
      return true;
    }
    if (!ctrl) {
      return false;
    }
    switch (event.sym) {
    case XKB_KEY_o:
      openDialog();
      return true;
    case XKB_KEY_w:
      closeTab(m_active);
      return true;
    case XKB_KEY_q:
      m_app.quit();
      return true;
    case XKB_KEY_f:
      if (tab != nullptr) {
        m_findOpen = true;
        m_focusFind = true;
        m_app.requestRebuild();
      }
      return true;
    case XKB_KEY_l:
      if (tab != nullptr) {
        m_gotoOpen = true;
        m_app.requestRebuild();
      }
      return true;
    case XKB_KEY_c:
      copySelection();
      return true;
    case XKB_KEY_a:
      // Seleccionar todo el texto de la página actual.
      if (tab != nullptr) {
        tab->selectionPage = tab->currentPage;
        tab->selectionFrom = 0;
        tab->selectionTo = tab->text(tab->currentPage).chars.size();
        if (m_view != nullptr) {
          m_view->markLayoutDirty();
          m_app.invalidate();
        }
      }
      return true;
    case XKB_KEY_p:
      if (tab != nullptr) {
        openPrint();
      }
      return true;
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
      zoomStep(1);
      return true;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
      zoomStep(-1);
      return true;
    case XKB_KEY_0:
      if (m_view != nullptr) {
        m_view->setFit(Fit::Width);
        m_app.requestRebuild();
      }
      return true;
    case XKB_KEY_Left:
      rotate(-90);
      return true;
    case XKB_KEY_Right:
      rotate(90);
      return true;
    case XKB_KEY_Page_Down:
    case XKB_KEY_Tab:
      if (!m_tabs.empty()) {
        activate((m_active + 1) % m_tabs.size());
      }
      return true;
    case XKB_KEY_Page_Up:
    case XKB_KEY_ISO_Left_Tab:
      if (!m_tabs.empty()) {
        activate((m_active + m_tabs.size() - 1) % m_tabs.size());
      }
      return true;
    case XKB_KEY_Home:
      if (m_view != nullptr) {
        m_view->goToPage(0);
        m_app.invalidate();
      }
      return true;
    case XKB_KEY_End:
      if (m_view != nullptr && tab != nullptr) {
        m_view->goToPage(static_cast<int>(tab->sizes.size()) - 1);
        m_app.invalidate();
      }
      return true;
    default:
      return false;
    }
  }

  // ── Estado guardado ────────────────────────────────────────────────────────────────────────────────

  void PdfWindow::loadState() {
    toml::table root;
    try {
      root = toml::parse_file(statePath().string());
    } catch (const toml::parse_error&) {
      return;
    }
    m_sidebar = root["sidebar"].value_or(true);
    m_night = root["night"].value_or(false);
    if (const auto* recent = root["recent"].as_array()) {
      for (const auto& node : *recent) {
        if (const auto path = node.value<std::string>()) {
          m_recent.emplace_back(*path);
        }
      }
    }
    if (const auto* files = root["files"].as_array()) {
      for (const auto& node : *files) {
        const auto* entry = node.as_table();
        if (entry == nullptr) {
          continue;
        }
        if (const auto path = (*entry)["path"].value<std::string>()) {
          m_remembered[*path] = {
              .page = (*entry)["page"].value_or(0),
              .fit = fitFromId((*entry)["fit"].value_or(std::string("width"))),
              .zoom = std::clamp((*entry)["zoom"].value_or(1.0), 0.1, 8.0),
              .dual = (*entry)["dual"].value_or(false),
          };
        }
      }
    }
  }

  void PdfWindow::saveState() {
    for (const auto& tab : m_tabs) {
      rememberTab(*tab);
    }
    toml::table root{{"sidebar", m_sidebar}, {"night", m_night}};
    toml::array recent;
    for (const auto& path : m_recent) {
      recent.push_back(path.string());
    }
    root.insert("recent", std::move(recent));
    // Los recientes primero; el resto hasta el límite (un archivo que ya no existe se olvida).
    toml::array files;
    std::size_t kept = 0;
    for (const auto& [path, state] : m_remembered) {
      if (std::error_code ec; kept >= kRememberLimit || !fs::exists(path, ec)) {
        continue;
      }
      files.push_back(
          toml::table{
              {"path", path},
              {"page", state.page},
              {"fit", fitId(state.fit)},
              {"zoom", state.zoom},
              {"dual", state.dual},
          }
      );
      ++kept;
    }
    root.insert("files", std::move(files));
    std::error_code ec;
    fs::create_directories(statePath().parent_path(), ec);
    std::ofstream(statePath(), std::ios::trunc) << root << '\n';
  }

} // namespace okaru::pdf
