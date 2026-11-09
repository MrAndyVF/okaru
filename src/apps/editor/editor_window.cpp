#include "apps/editor/editor_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <glib.h>
#include <sstream>
#include <toml++/toml.hpp>

namespace okaru::editor {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;

    constexpr std::size_t kRecentLimit = 12;
    constexpr std::size_t kMatchLimit = 10000;
    constexpr std::size_t kCsvRowLimit = 2000;
    constexpr std::array<int, 3> kIndentWidths{2, 4, 8};

    fs::path statePath() { return fs::path(FileUtils::stateDir()) / "editor.toml"; }

    std::string readFile(const fs::path& path, bool& ok) {
      std::ifstream in(path, std::ios::binary);
      ok = static_cast<bool>(in);
      std::ostringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

    // Sangría del archivo: tabuladores si la mayoría de las líneas sangradas los usan; si no, el menor salto de
    // espacios.
    void detectIndent(const TextBuffer& buffer, int& width, bool& spaces) {
      std::size_t tabs = 0;
      std::size_t spaced = 0;
      std::size_t smallest = 0;
      for (std::size_t i = 0; i < std::min<std::size_t>(buffer.lineCount(), 2000); ++i) {
        const std::string& line = buffer.line(i);
        if (line.starts_with('\t')) {
          ++tabs;
        } else if (line.starts_with(' ')) {
          std::size_t n = 0;
          while (n < line.size() && line[n] == ' ') {
            ++n;
          }
          if (n < line.size() && n >= 2) {
            ++spaced;
            smallest = smallest == 0 ? n : std::min(smallest, n);
          }
        }
      }
      if (tabs > spaced) {
        spaces = false;
      } else if (spaced > 0) {
        spaces = true;
        width = smallest >= 8 ? 8 : smallest >= 4 ? 4 : 2;
      }
    }

  } // namespace

  EditorWindow::EditorWindow(apps::OkaruApp& app, const std::vector<fs::path>& files) : m_app(app) {
    loadState();
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setCloseRequestCallback([this] {
      requestQuit();
      return false; // requestQuit sale solo cuando no queda nada por guardar
    });
    m_app.setAfterBuild([this] {
      if (m_focusFind && m_findInput != nullptr) {
        m_app.focus(m_findInput->inputArea());
        m_focusFind = false;
      } else if (m_gotoOpen && m_gotoInput != nullptr) {
        m_app.focus(m_gotoInput->inputArea());
      } else if (m_view != nullptr && !m_findOpen && m_closing.empty()) {
        m_app.focus(m_view->inputArea());
      }
    });
    for (const auto& file : files) {
      (void)openFile(file);
    }
    if (m_docs.empty() && !files.empty()) {
      newDocument(); // no se pudo abrir: al menos una hoja vacía
    }
  }

  EditorWindow::~EditorWindow() { saveState(); }

  // ── Documentos ─────────────────────────────────────────────────────────────────────────────────────

  Document* EditorWindow::current() { return m_active < m_docs.size() ? m_docs[m_active].get() : nullptr; }

  std::string EditorWindow::title(const Document& doc) const {
    return doc.path.empty() ? tr("apps.edit.untitled", "number", doc.untitled) : doc.path.filename().string();
  }

  ViewOptions EditorWindow::viewOptions(const Document& doc) const {
    return {
        .fontSize = m_prefs.fontSize,
        .tabWidth = doc.tabWidth,
        .insertSpaces = doc.insertSpaces,
        .wrap = m_prefs.wrap,
        .lineNumbers = m_prefs.lineNumbers,
        .highlightLine = m_prefs.highlightLine,
    };
  }

  void EditorWindow::newDocument() {
    captureView();
    auto doc = std::make_unique<Document>();
    doc->untitled = m_nextUntitled++;
    doc->tabWidth = m_prefs.tabWidth;
    doc->insertSpaces = m_prefs.insertSpaces;
    m_docs.push_back(std::move(doc));
    m_active = m_docs.size() - 1;
    m_showPreferences = false;
    m_app.requestRebuild();
  }

  bool EditorWindow::loadInto(Document& doc) {
    bool ok = false;
    std::string text = readFile(doc.path, ok);
    if (!ok) {
      return false;
    }
    // No era UTF-8: se interpreta como Latin-1 (archivos viejos de Windows) y se guardará en UTF-8.
    doc.latin1 = g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) == FALSE;
    if (doc.latin1) {
      gchar* converted =
          g_convert(text.data(), static_cast<gssize>(text.size()), "UTF-8", "ISO-8859-1", nullptr, nullptr, nullptr);
      if (converted != nullptr) {
        text = converted;
        g_free(converted);
      }
    }
    doc.buffer.setText(text);
    doc.highlight.setLanguage(detectLanguage(doc.path, doc.buffer.line(0)));
    doc.tabWidth = m_prefs.tabWidth;
    doc.insertSpaces = m_prefs.insertSpaces;
    detectIndent(doc.buffer, doc.tabWidth, doc.insertSpaces);
    std::error_code ec;
    doc.savedTime = fs::last_write_time(doc.path, ec);
    doc.changedOnDisk = false;
    doc.problem.reset();
    doc.validatedVersion = static_cast<std::uint64_t>(-1);
    return true;
  }

  bool EditorWindow::openFile(const fs::path& requested, std::optional<TextPos> cursor, float scrollY) {
    std::error_code ec;
    const fs::path path = fs::weakly_canonical(fs::absolute(requested, ec), ec);
    // Ya abierto: se va a su pestaña.
    for (std::size_t i = 0; i < m_docs.size(); ++i) {
      if (m_docs[i]->path == path) {
        activate(i);
        return true;
      }
    }
    auto doc = std::make_unique<Document>();
    doc->path = path;
    if (!fs::exists(path, ec)) {
      // Un archivo que todavía no existe se abre vacío y se crea al guardar (como `gnome-text-editor nuevo.txt`).
      doc->highlight.setLanguage(detectLanguage(path));
      doc->tabWidth = m_prefs.tabWidth;
      doc->insertSpaces = m_prefs.insertSpaces;
    } else if (!loadInto(*doc)) {
      setMessage(tr("apps.edit.error.open", "file", path.filename().string()), true);
      return false;
    }
    captureView();
    // Una hoja sin título y sin tocar se reemplaza.
    if (m_docs.size() == 1 && m_docs[0]->path.empty() && !m_docs[0]->buffer.modified()) {
      m_docs.clear();
    }
    if (cursor.has_value()) {
      doc->cursor = doc->anchor = doc->buffer.clamp(*cursor);
      doc->scrollY = scrollY;
    }
    watch(*doc);
    m_docs.push_back(std::move(doc));
    m_active = m_docs.size() - 1;
    rememberRecent(path);
    m_showPreferences = false;
    validateNow();
    m_app.requestRebuild();
    return true;
  }

  void EditorWindow::watch(Document& doc) {
    if (doc.path.empty()) {
      return;
    }
    if (doc.watch != 0) {
      m_app.fileWatcher().unwatch(doc.watch);
    }
    doc.watch = m_app.fileWatcher().watch(
        doc.path, [this, path = doc.path] { onDiskChange(path); }, FileWatcher::WatchTrigger::WriteCompleted
    );
  }

  void EditorWindow::onDiskChange(const fs::path& path) {
    for (auto& doc : m_docs) {
      if (doc->path != path) {
        continue;
      }
      std::error_code ec;
      const auto time = fs::last_write_time(path, ec);
      if (ec || time == doc->savedTime) {
        return; // lo guardamos nosotros, o ya no existe (se avisa al guardar)
      }
      if (!doc->buffer.modified()) {
        // Sin cambios propios: se recarga en silencio y el cursor se queda donde estaba.
        const TextPos cursor = doc->cursor;
        if (doc.get() == current()) {
          captureView();
        }
        if (loadInto(*doc)) {
          doc->cursor = doc->anchor = doc->buffer.clamp(cursor);
          validateNow();
        }
      } else {
        doc->changedOnDisk = true;
      }
      m_app.requestRebuild();
      return;
    }
  }

  bool EditorWindow::saveDocument(Document& doc) {
    if (doc.path.empty()) {
      return false;
    }
    std::error_code ec;
    fs::path target = doc.path;
    if (fs::is_symlink(target, ec)) {
      target = fs::canonical(target, ec); // se escribe en el destino: el enlace (dotfiles) se conserva
    }
    // Escritura atómica junto al original, con sus mismos permisos.
    const fs::path tmp = target.parent_path() / ("." + target.filename().string() + ".okaru-tmp");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        setMessage(tr("apps.edit.error.save", "file", title(doc)), true);
        return false;
      }
      out << doc.buffer.text();
      if (!out) {
        fs::remove(tmp, ec);
        setMessage(tr("apps.edit.error.save", "file", title(doc)), true);
        return false;
      }
    }
    if (fs::exists(target, ec)) {
      fs::permissions(tmp, fs::status(target, ec).permissions(), ec);
    }
    fs::rename(tmp, target, ec);
    if (ec) {
      fs::remove(tmp, ec);
      setMessage(tr("apps.edit.error.save", "file", title(doc)), true);
      return false;
    }
    doc.buffer.markSaved();
    doc.savedTime = fs::last_write_time(target, ec);
    doc.changedOnDisk = false;
    doc.latin1 = false;
    watch(doc);
    rememberRecent(doc.path);
    setMessage(tr("apps.edit.saved", "file", title(doc)), false);
    m_app.requestRebuild();
    return true;
  }

  void EditorWindow::saveAs(std::size_t index, std::function<void()> then) {
    SessionBus* bus = m_app.sessionBus();
    if (bus == nullptr || index >= m_docs.size()) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    Document* doc = m_docs[index].get();
    const std::string name = doc->path.empty() ? title(*doc) + ".txt" : doc->path.filename().string();
    const std::string folder =
        doc->path.empty() ? FileUtils::expandUserPath("~").string() : doc->path.parent_path().string();
    m_chooser->save(tr("apps.edit.save-as"), name, folder, [this, doc, then](std::vector<std::string> paths) {
      // El documento puede haberse cerrado mientras el diálogo estaba abierto.
      const auto it = std::ranges::find_if(m_docs, [doc](const auto& d) { return d.get() == doc; });
      if (paths.empty() || it == m_docs.end()) {
        m_closing.clear();
        m_quitAfterClosing = false;
        m_app.requestRebuild();
        return;
      }
      doc->path = paths.front();
      doc->highlight.setLanguage(detectLanguage(doc->path, doc->buffer.line(0)));
      if (saveDocument(*doc) && then) {
        then();
      }
    });
  }

  void EditorWindow::activate(std::size_t index) {
    if (index >= m_docs.size()) {
      return;
    }
    captureView();
    m_active = index;
    m_showPreferences = false;
    updateMatches();
    m_app.requestRebuild();
  }

  void EditorWindow::closeDocument(std::size_t index) {
    if (index >= m_docs.size()) {
      return;
    }
    captureView();
    if (m_docs[index]->watch != 0) {
      m_app.fileWatcher().unwatch(m_docs[index]->watch);
    }
    m_docs.erase(m_docs.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_active >= m_docs.size()) {
      m_active = m_docs.empty() ? 0 : m_docs.size() - 1;
    } else if (m_active > index) {
      --m_active;
    }
    m_view = nullptr;
    m_app.requestRebuild();
  }

  void EditorWindow::requestClose(std::size_t index) {
    if (index < m_docs.size() && m_docs[index]->buffer.modified()) {
      m_closing = {index};
      m_quitAfterClosing = false;
      m_app.requestRebuild();
      return;
    }
    closeDocument(index);
  }

  void EditorWindow::requestQuit() {
    captureView();
    m_closing.clear();
    for (std::size_t i = 0; i < m_docs.size(); ++i) {
      if (m_docs[i]->buffer.modified()) {
        m_closing.push_back(i);
      }
    }
    if (m_closing.empty()) {
      saveState();
      m_app.quit();
      return;
    }
    m_quitAfterClosing = true;
    m_app.requestRebuild();
  }

  void EditorWindow::openDialog() {
    SessionBus* bus = m_app.sessionBus();
    if (bus == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    m_chooser->open(
        tr("apps.edit.open"),
        {{.name = tr("apps.edit.filter"),
          .mimeTypes =
              {"text/*", "application/json", "application/xml", "application/toml", "application/x-desktop",
               "application/xhtml+xml"}}},
        true, [this](std::vector<std::string> paths) {
          for (const auto& path : paths) {
            (void)openFile(path);
          }
        }
    );
  }

  void EditorWindow::rememberRecent(const fs::path& path) {
    std::erase(m_recent, path);
    m_recent.insert(m_recent.begin(), path);
    if (m_recent.size() > kRecentLimit) {
      m_recent.resize(kRecentLimit);
    }
  }

  // ── Estado del documento ───────────────────────────────────────────────────────────────────────────

  void EditorWindow::captureView() {
    if (Document* doc = current(); doc != nullptr && m_view != nullptr) {
      doc->cursor = m_view->cursor();
      doc->anchor = m_view->selection().from;
      doc->scrollY = m_view->scrollY();
    }
  }

  void EditorWindow::onTextChanged() {
    Document* doc = current();
    if (doc == nullptr) {
      return;
    }
    // El título (•) cambia solo al pasar de guardado a modificado y viceversa.
    const bool wasMarked = m_titleLabel != nullptr && m_titleLabel->text().ends_with(" •");
    if (wasMarked != doc->buffer.modified()) {
      m_app.requestRebuild();
    }
    if (m_findOpen) {
      updateMatches();
    }
    if (canValidate(doc->language())) {
      m_validateTimer.start(std::chrono::milliseconds(400), [this] { validateNow(); });
    }
  }

  void EditorWindow::validateNow() {
    Document* doc = current();
    if (doc == nullptr || !canValidate(doc->language()) || doc->validatedVersion == doc->buffer.version()) {
      return;
    }
    const bool hadProblem = doc->problem.has_value();
    doc->problem = validate(doc->language(), doc->buffer.text());
    doc->validatedVersion = doc->buffer.version();
    if (m_view != nullptr) {
      m_view->setProblem(
          doc->problem ? std::optional<TextPos>(TextPos{doc->problem->line, doc->problem->column}) : std::nullopt
      );
    }
    if (hadProblem != doc->problem.has_value() || doc->problem.has_value()) {
      updateStatus();
    }
  }

  void EditorWindow::updateStatus() {
    Document* doc = current();
    if (doc == nullptr || m_view == nullptr) {
      return;
    }
    if (m_positionLabel != nullptr) {
      const TextPos cursor = m_view->cursor();
      m_positionLabel->setText(
          tr("apps.edit.status.position", "line", cursor.line + 1, "column",
             g_utf8_strlen(doc->buffer.line(cursor.line).c_str(), static_cast<gssize>(cursor.col)) + 1)
      );
    }
    if (m_problemLabel != nullptr && canValidate(doc->language())) {
      m_problemLabel->setText(
          doc->problem ? tr("apps.edit.status.invalid", "line", doc->problem->line + 1)
                       : tr("apps.edit.status.valid", "format",
                            tr(std::format("apps.edit.language.{}", languageId(doc->language()))))
      );
      m_problemLabel->setColor(colorSpecFromRole(doc->problem ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    }
    m_app.invalidate();
  }

  void EditorWindow::formatDocument() {
    Document* doc = current();
    if (doc == nullptr || m_view == nullptr || !canFormat(doc->language())) {
      return;
    }
    auto formatted = format(doc->language(), doc->buffer.text(), doc->tabWidth, !doc->insertSpaces);
    if (!formatted) {
      setMessage(
          formatted.error().message == "comments" ? tr("apps.edit.format.comments")
                                                  : tr("apps.edit.format.invalid", "line", formatted.error().line + 1),
          true
      );
      return;
    }
    // Todo el texto en un paso de deshacer; el cursor se queda en su línea.
    const TextPos cursor = m_view->cursor();
    m_view->selectAll();
    m_view->insertText(*formatted);
    m_view->setCursor(doc->buffer.clamp({cursor.line, 0}));
    setMessage(tr("apps.edit.format.done"), false);
  }

  void EditorWindow::setMessage(std::string message, bool error) {
    m_message = std::move(message);
    m_messageError = error;
    m_messageTimer.start(std::chrono::seconds(error ? 6 : 2), [this] {
      m_message.clear();
      m_app.requestRebuild();
    });
    m_app.requestRebuild();
  }

  void EditorWindow::zoom(int steps) {
    m_prefs.fontSize = steps == 0 ? 14.0F : std::clamp(m_prefs.fontSize + static_cast<float>(steps), 8.0F, 40.0F);
    m_app.requestRebuild();
  }

  // ── Buscar y reemplazar ────────────────────────────────────────────────────────────────────────────

  void EditorWindow::updateMatches() {
    Document* doc = current();
    m_matches.clear();
    if (doc != nullptr && m_findOpen && !m_findText.empty()) {
      m_matches = doc->buffer.findAll(m_findText, m_search);
      if (m_matches.size() > kMatchLimit) {
        m_matches.resize(kMatchLimit);
      }
    }
    if (m_view != nullptr) {
      m_view->setSearchMatches(m_matches);
    }
    if (m_matchesLabel != nullptr) {
      std::size_t index = 0;
      if (m_view != nullptr) {
        const TextRange sel = m_view->selection().ordered();
        for (std::size_t i = 0; i < m_matches.size(); ++i) {
          if (m_matches[i] == sel) {
            index = i + 1;
          }
        }
      }
      m_matchesLabel->setText(
          m_findText.empty()      ? std::string()
              : m_matches.empty() ? tr("apps.edit.find.none")
                                  : tr("apps.edit.find.count", "index", index, "total", m_matches.size())
      );
    }
    m_app.invalidate();
  }

  void EditorWindow::findStep(bool forward) {
    Document* doc = current();
    if (doc == nullptr || m_view == nullptr || m_findText.empty()) {
      return;
    }
    const TextRange sel = m_view->selection().ordered();
    const TextPos from = forward ? sel.to : sel.from;
    if (const auto match = doc->buffer.find(m_findText, from, forward, m_search)) {
      m_view->select(*match);
    }
    updateMatches();
  }

  void EditorWindow::replaceOne() {
    Document* doc = current();
    if (doc == nullptr || m_view == nullptr || m_findText.empty()) {
      return;
    }
    // Si la selección es una coincidencia se reemplaza y se pasa a la siguiente.
    const TextRange sel = m_view->selection().ordered();
    if (std::ranges::contains(m_matches, sel)) {
      m_view->insertText(m_replaceText);
    }
    findStep(true);
  }

  void EditorWindow::replaceEverything() {
    Document* doc = current();
    if (doc == nullptr || m_findText.empty()) {
      return;
    }
    const std::size_t count = doc->buffer.replaceAll(m_findText, m_replaceText, m_search);
    if (m_view != nullptr) {
      m_view->setCursor(doc->buffer.clamp(m_view->cursor()));
    }
    onTextChanged();
    setMessage(tr("apps.edit.find.replaced", "count", count), false);
  }

  // ── Escena ─────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> EditorWindow::build(float width, float height, float scale) {
    // La vista anterior sigue viva mientras se arma la nueva: su cursor y desplazamiento pasan al documento.
    captureView();
    m_view = nullptr;
    m_findInput = nullptr;
    m_gotoInput = nullptr;
    m_positionLabel = nullptr;
    m_matchesLabel = nullptr;
    m_problemLabel = nullptr;
    m_titleLabel = nullptr;

    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    auto main = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .width = width, .height = height});
    Node* header = main->addChild(buildHeader(scale));
    m_app.setDragNode(header);

    if (m_showPreferences) {
      main->addChild(buildPreferences(scale));
    } else if (m_docs.empty()) {
      main->addChild(buildEmpty(scale));
    } else {
      if (m_docs.size() > 1) {
        main->addChild(buildTabs(width, scale));
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
      Document* doc = current();
      if (doc->csvTable && doc->language() == Language::Csv) {
        main->addChild(buildCsvTable(scale));
      } else {
        auto view = std::make_unique<EditorView>();
        m_view = view.get();
        view->setBuffer(&doc->buffer, &doc->highlight);
        view->setOptions(viewOptions(*doc));
        view->setCursor(doc->anchor);
        view->setCursor(doc->cursor, true);
        view->setScrollY(doc->scrollY);
        view->setSearchMatches(m_matches);
        view->setProblem(
            doc->problem ? std::optional<TextPos>(TextPos{doc->problem->line, doc->problem->column}) : std::nullopt
        );
        view->setOnChanged([this] { onTextChanged(); });
        view->setOnCursorMoved([this] {
          updateStatus();
          if (m_findOpen) {
            updateMatches();
          }
        });
        view->setOnZoom([this](int steps) { zoom(steps); });
        view->setFlexGrow(1.0F);
        main->addChild(std::move(view));
      }
      main->addChild(buildStatus(scale));
    }
    root->addChild(std::move(main));
    if (!m_closing.empty()) {
      root->addChild(buildConfirm(width, height, scale));
    }
    updateStatus();
    updateMatches();
    return root;
  }

  std::unique_ptr<Node> EditorWindow::buildHeader(float scale) {
    const auto ghost = [&](const char* glyph, std::string tooltip, std::function<void()> onClick, bool enabled = true) {
      return ui::button({
          .glyph = glyph,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .height = 52.0F * scale,
    });
    header->addChild(ghost("folder-open", tr("apps.edit.open"), [this] { openDialog(); }));
    header->addChild(ghost("file-plus", tr("apps.edit.new"), [this] { newDocument(); }));
    auto titles = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
    Document* doc = m_showPreferences ? nullptr : current();
    titles->addChild(
        ui::label({
            .out = &m_titleLabel,
            .text = doc == nullptr ? (m_showPreferences ? tr("apps.edit.preferences.title") : tr("apps.edit.title"))
                                   : title(*doc) + (doc->buffer.modified() ? " •" : ""),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 1,
            .ellipsize = TextEllipsize::Middle,
        })
    );
    if (doc != nullptr && !doc->path.empty()) {
      std::string folder = doc->path.parent_path().string();
      if (const std::string home = FileUtils::expandUserPath("~").string(); folder.starts_with(home)) {
        folder = "~" + folder.substr(home.size());
      }
      titles->addChild(
          ui::label({
              .text = folder,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::Start,
          })
      );
    }
    header->addChild(std::move(titles));
    if (doc != nullptr) {
      header->addChild(ghost("search", tr("apps.edit.find.title"), [this] {
        m_findOpen = !m_findOpen;
        m_focusFind = m_findOpen;
        m_app.requestRebuild();
      }));
      header->addChild(
          ui::button({
              .text = tr("apps.edit.save"),
              .enabled = doc->buffer.modified() || doc->path.empty(),
              .variant = ButtonVariant::Primary,
              .onClick = [this] {
                if (Document* d = current(); d != nullptr) {
                  if (d->path.empty()) {
                    saveAs(m_active);
                  } else {
                    (void)saveDocument(*d);
                  }
                }
              },
          })
      );
      header->addChild(ghost("device-floppy", tr("apps.edit.save-as"), [this] { saveAs(m_active); }));
    }
    header->addChild(ghost(m_showPreferences ? "arrow-left" : "settings", tr("apps.edit.preferences.title"), [this] {
      captureView();
      m_showPreferences = !m_showPreferences;
      m_app.requestRebuild();
    }));
    header->addChild(ghost("x", tr("apps.edit.close"), [this] { requestQuit(); }));
    return header;
  }

  std::unique_ptr<Node> EditorWindow::buildTabs(float width, float scale) {
    const float padding = Style::spaceMd * scale;
    const float gap = Style::spaceXs * scale;
    const float height = Style::controlHeightSm * scale;
    const auto count = static_cast<float>(m_docs.size());
    // Todas del mismo ancho, como en GNOME, hasta 240 px.
    const float tabWidth = std::min(240.0F * scale, (width - 2.0F * padding - gap * (count - 1.0F)) / count);
    const float side = 22.0F * scale;
    const float radius = Style::radiusMd * scale;
    Renderer* renderer = m_app.renderer();

    auto tabs = ui::row({
        .align = FlexAlign::Center,
        .gap = gap,
        .paddingV = 3.0F * scale,
        .paddingH = padding,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
    });
    for (std::size_t i = 0; i < m_docs.size(); ++i) {
      const Document& doc = *m_docs[i];
      const bool active = i == m_active;
      auto tab = ui::node({.width = tabWidth, .height = height});

      // Fondo: la activa en el color primario; las demás se marcan apenas al pasar el puntero.
      Box* background = nullptr;
      const ColorSpec restFill = active ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec();
      const ColorSpec hoverFill = active ? restFill : colorSpecFromRole(ColorRole::OnSurface, 0.06F);
      tab->addChild(
          ui::box({.out = &background, .fill = restFill, .radius = radius, .width = tabWidth, .height = height})
      );
      auto area = ui::inputArea({
          .tooltip = doc.path.empty() ? std::string() : doc.path.string(),
          .width = tabWidth,
          .height = height,
          .onClick = [this, i](const InputArea::PointerData&) { activate(i); },
      });
      area->setOnEnter([background, hoverFill](const InputArea::PointerData&) { background->setFill(hoverFill); });
      area->setOnLeave([background, restFill] { background->setFill(restFill); });
      tab->addChild(std::move(area));

      // Nombre centrado; se acorta si no cabe sin tapar la X.
      const ColorRole text = active ? ColorRole::OnPrimary : ColorRole::OnSurface;
      auto label = ui::label({
          .text = title(doc) + (doc.buffer.modified() ? " •" : ""),
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = active ? FontWeight::SemiBold : FontWeight::Normal,
          .color = colorSpecFromRole(text),
          .maxWidth = tabWidth - 2.0F * (side + 8.0F * scale),
          .maxLines = 1,
          .ellipsize = TextEllipsize::Middle,
      });
      label->setHitTestVisible(false);
      if (renderer != nullptr) {
        label->layout(*renderer);
      }
      label->setPosition(std::round((tabWidth - label->width()) * 0.5F), std::round((height - label->height()) * 0.5F));
      tab->addChild(std::move(label));

      // X a la derecha: su recuadro se oscurece al pasar el puntero (en la activa, un tono más oscuro del primario).
      const float closeX = tabWidth - side - 6.0F * scale;
      const float closeY = (height - side) * 0.5F;
      Box* closeBox = nullptr;
      const ColorSpec closeHover =
          active ? colorSpecFromRole(ColorRole::Shadow, 0.22F) : colorSpecFromRole(ColorRole::OnSurface, 0.12F);
      auto closeBackground =
          ui::box({.out = &closeBox, .fill = clearColorSpec(), .radius = radius, .width = side, .height = side});
      closeBackground->setPosition(closeX, closeY);
      tab->addChild(std::move(closeBackground));
      const float glyphSize = 14.0F * scale;
      auto icon = ui::glyph({
          .glyph = "x",
          .glyphSize = glyphSize,
          .color = colorSpecFromRole(text, 0.85F),
          .width = glyphSize,
          .height = glyphSize,
      });
      icon->setHitTestVisible(false);
      icon->setPosition(std::round(closeX + (side - glyphSize) * 0.5F), std::round(closeY + (side - glyphSize) * 0.5F));
      tab->addChild(std::move(icon));
      auto close = ui::inputArea({
          .tooltip = tr("apps.edit.close-tab"),
          .width = side,
          .height = side,
          .onClick = [this, i](const InputArea::PointerData&) { requestClose(i); },
      });
      close->setPosition(closeX, closeY);
      close->setZIndex(1);
      close->setOnEnter([closeBox, closeHover](const InputArea::PointerData&) { closeBox->setFill(closeHover); });
      close->setOnLeave([closeBox] { closeBox->setFill(clearColorSpec()); });
      tab->addChild(std::move(close));
      tabs->addChild(std::move(tab));
    }
    return tabs;
  }

  std::unique_ptr<Node> EditorWindow::buildBanner(float scale) {
    Document* doc = current();
    const bool disk = doc != nullptr && doc->changedOnDisk;
    if (!disk && m_message.empty()) {
      return nullptr;
    }
    auto banner = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(disk || m_messageError ? ColorRole::ErrorContainer : ColorRole::SecondaryContainer),
    });
    banner->addChild(
        ui::label({
            .text = disk ? tr("apps.edit.disk.changed") : m_message,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(
                disk || m_messageError ? ColorRole::OnErrorContainer : ColorRole::OnSecondaryContainer
            ),
            .flexGrow = 1.0F,
        })
    );
    if (disk) {
      banner->addChild(
          ui::button({
              .text = tr("apps.edit.disk.reload"),
              .variant = ButtonVariant::Secondary,
              .onClick = [this] {
                if (Document* d = current(); d != nullptr && loadInto(*d)) {
                  m_view = nullptr;
                  validateNow();
                  m_app.requestRebuild();
                }
              },
          })
      );
      banner->addChild(
          ui::button({
              .text = tr("apps.edit.disk.keep"),
              .variant = ButtonVariant::Ghost,
              .onClick = [this] {
                if (Document* d = current(); d != nullptr) {
                  d->changedOnDisk = false;
                  std::error_code ec;
                  d->savedTime = fs::last_write_time(d->path, ec);
                  m_app.requestRebuild();
                }
              },
          })
      );
    }
    return banner;
  }

  std::unique_ptr<Node> EditorWindow::buildFindBar(float scale) {
    auto bar = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
    });
    const auto toggle = [&](const char* glyph, std::string tooltip, bool on, std::function<void()> onClick) {
      return ui::button({
          .glyph = glyph,
          .selected = on,
          .variant = on ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };
    auto findRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    findRow->addChild(
        ui::input({
            .out = &m_findInput,
            .value = m_findText,
            .placeholder = tr("apps.edit.find.placeholder"),
            .flexGrow = 1.0F,
            .onChange =
                [this](const std::string& value) {
                  m_findText = value;
                  updateMatches();
                  // Mientras se escribe, salta a la primera coincidencia desde el cursor.
                  if (Document* doc = current(); doc != nullptr && m_view != nullptr && !value.empty()) {
                    if (const auto match =
                            doc->buffer.find(value, m_view->selection().ordered().from, true, m_search)) {
                      m_view->select(*match);
                    }
                  }
                },
            .onSubmit = [this](const std::string&) { findStep(true); },
            .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) -> bool {
              if (sym == XKB_KEY_Escape) {
                m_findOpen = false;
                m_replaceOpen = false;
                m_app.requestRebuild();
                return true;
              }
              if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && (modifiers & KeyMod::Shift) != 0) {
                findStep(false);
                return true;
              }
              return false;
            },
        })
    );
    findRow->addChild(
        ui::label({
            .out = &m_matchesLabel,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = 90.0F * scale,
            .textAlign = TextAlign::Center,
        })
    );
    findRow->addChild(
        ui::button(
            {.glyph = "chevron-up",
             .variant = ButtonVariant::Ghost,
             .tooltip = tr("apps.edit.find.previous"),
             .onClick = [this] { findStep(false); }}
        )
    );
    findRow->addChild(
        ui::button(
            {.glyph = "chevron-down",
             .variant = ButtonVariant::Ghost,
             .tooltip = tr("apps.edit.find.next"),
             .onClick = [this] { findStep(true); }}
        )
    );
    findRow->addChild(toggle("letter-case", tr("apps.edit.find.case"), m_search.caseSensitive, [this] {
      m_search.caseSensitive = !m_search.caseSensitive;
      updateMatches();
      m_app.requestRebuild();
    }));
    findRow->addChild(toggle("text-size", tr("apps.edit.find.word"), m_search.wholeWord, [this] {
      m_search.wholeWord = !m_search.wholeWord;
      updateMatches();
      m_app.requestRebuild();
    }));
    findRow->addChild(toggle("replace", tr("apps.edit.find.replace"), m_replaceOpen, [this] {
      m_replaceOpen = !m_replaceOpen;
      m_app.requestRebuild();
    }));
    findRow->addChild(
        ui::button({.glyph = "x", .variant = ButtonVariant::Ghost, .tooltip = tr("apps.edit.close"), .onClick = [this] {
                      m_findOpen = false;
                      m_replaceOpen = false;
                      m_app.requestRebuild();
                    }})
    );
    bar->addChild(std::move(findRow));
    if (m_replaceOpen) {
      bar->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
              ui::input({
                  .value = m_replaceText,
                  .placeholder = tr("apps.edit.find.replace-placeholder"),
                  .flexGrow = 1.0F,
                  .onChange = [this](const std::string& value) { m_replaceText = value; },
                  .onSubmit = [this](const std::string&) { replaceOne(); },
              }),
              ui::button(
                  {.text = tr("apps.edit.find.replace-one"),
                   .variant = ButtonVariant::Secondary,
                   .onClick = [this] { replaceOne(); }}
              ),
              ui::button(
                  {.text = tr("apps.edit.find.replace-all"), .variant = ButtonVariant::Secondary, .onClick = [this] {
                     replaceEverything();
                   }}
              )
          )
      );
    }
    return bar;
  }

  std::unique_ptr<Node> EditorWindow::buildGotoBar(float scale) {
    Document* doc = current();
    return ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingV = Style::spaceXs * scale,
         .paddingH = Style::spaceMd * scale,
         .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow)},
        ui::label({.text = tr("apps.edit.goto.title"), .fontSize = Style::fontSizeCaption * scale}),
        ui::input({
            .out = &m_gotoInput,
            .placeholder = tr("apps.edit.goto.placeholder", "count", doc != nullptr ? doc->buffer.lineCount() : 0),
            .width = 200.0F * scale,
            .onSubmit =
                [this](const std::string& value) {
                  // "12" o "12:5" (línea:columna).
                  std::size_t line = 0;
                  std::size_t column = 0;
                  if (std::sscanf(value.c_str(), "%zu:%zu", &line, &column) >= 1 && line > 0 && m_view != nullptr) {
                    m_gotoOpen = false;
                    const TextPos pos{line - 1, column > 0 ? column - 1 : 0};
                    m_view->select({pos, pos});
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

  std::unique_ptr<Node> EditorWindow::buildStatus(float scale) {
    Document* doc = current();
    auto status = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .paddingV = 2.0F * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
    });
    // Formato del archivo.
    std::vector<std::string> languages;
    for (std::size_t i = 0; i < kLanguageCount; ++i) {
      languages.push_back(tr(std::format("apps.edit.language.{}", languageId(static_cast<Language>(i)))));
    }
    status->addChild(
        ui::select({
            .options = std::move(languages),
            .selectedIndex = static_cast<std::size_t>(doc->language()),
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .width = 130.0F * scale,
            .onSelectionChanged = [this](std::size_t index, std::string_view) {
              if (Document* d = current(); d != nullptr) {
                captureView();
                d->highlight.setLanguage(static_cast<Language>(index));
                d->problem.reset();
                d->validatedVersion = static_cast<std::uint64_t>(-1);
                validateNow();
                m_app.requestRebuild();
              }
            },
        })
    );
    // Validación: estado, y clic para ir a la línea del error.
    if (canValidate(doc->language())) {
      status->addChild(
          ui::button({
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = doc->problem ? doc->problem->message : std::string(),
              .onClick =
                  [this] {
                    if (Document* d = current(); d != nullptr && d->problem && m_view != nullptr) {
                      const TextPos pos{d->problem->line, d->problem->column};
                      m_view->select({pos, pos});
                    }
                  },
              .configure = [this](Button& button) { m_problemLabel = button.label(); },
          })
      );
    }
    if (canFormat(doc->language())) {
      status->addChild(
          ui::button({
              .text = tr("apps.edit.format.title"),
              .glyph = "wand",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = tr("apps.edit.format.tooltip"),
              .onClick = [this] { formatDocument(); },
          })
      );
    }
    if (doc->language() == Language::Csv) {
      status->addChild(
          ui::button({
              .text = doc->csvTable ? tr("apps.edit.csv.text") : tr("apps.edit.csv.table"),
              .glyph = doc->csvTable ? "file-text" : "table",
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .onClick = [this] {
                if (Document* d = current(); d != nullptr) {
                  captureView();
                  d->csvTable = !d->csvTable;
                  m_app.requestRebuild();
                }
              },
          })
      );
    }
    status->addChild(ui::node({.flexGrow = 1.0F}));
    status->addChild(
        ui::label({
            .out = &m_positionLabel,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    // Sangría: espacios 2/4/8 o tabuladores.
    std::vector<std::string> indents;
    std::size_t selected = 3;
    for (std::size_t i = 0; i < kIndentWidths.size(); ++i) {
      indents.push_back(tr("apps.edit.indent.spaces", "count", kIndentWidths[i]));
      if (doc->insertSpaces && doc->tabWidth == kIndentWidths[i]) {
        selected = i;
      }
    }
    indents.push_back(tr("apps.edit.indent.tabs"));
    status->addChild(
        ui::select({
            .options = std::move(indents),
            .selectedIndex = selected,
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .width = 140.0F * scale,
            .onSelectionChanged = [this](std::size_t index, std::string_view) {
              if (Document* d = current(); d != nullptr) {
                captureView();
                d->insertSpaces = index < kIndentWidths.size();
                if (d->insertSpaces) {
                  d->tabWidth = kIndentWidths[index];
                }
                m_app.requestRebuild();
              }
            },
        })
    );
    status->addChild(
        ui::select({
            .options = std::vector<std::string>{"LF", "CRLF"},
            .selectedIndex = doc->buffer.crlf() ? 1U : 0U,
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .width = 90.0F * scale,
            .onSelectionChanged = [this](std::size_t index, std::string_view) {
              if (Document* d = current(); d != nullptr) {
                captureView();
                d->buffer.setCrlf(index == 1);
                m_app.requestRebuild();
              }
            },
        })
    );
    status->addChild(
        ui::label({
            .text = doc->latin1 ? "ISO-8859-1 → UTF-8" : "UTF-8",
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(doc->latin1 ? ColorRole::Error : ColorRole::OnSurfaceVariant),
        })
    );
    return status;
  }

  std::unique_ptr<Node> EditorWindow::buildEmpty(float scale) {
    auto page = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .flexGrow = 1.0F,
    });
    page->addChild(
        ui::glyph(
            {.glyph = "file-text", .glyphSize = 64.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    page->addChild(
        ui::label(
            {.text = tr("apps.edit.empty.title"),
             .fontSize = Style::fontSizeTitle * scale,
             .fontWeight = FontWeight::SemiBold}
        )
    );
    page->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::button(
                {.text = tr("apps.edit.new"),
                 .glyph = "file-plus",
                 .variant = ButtonVariant::Primary,
                 .onClick = [this] { newDocument(); }}
            ),
            ui::button(
                {.text = tr("apps.edit.open"),
                 .glyph = "folder-open",
                 .variant = ButtonVariant::Secondary,
                 .onClick = [this] { openDialog(); }}
            )
        )
    );
    if (!m_recent.empty()) {
      page->addChild(
          ui::label({
              .text = tr("apps.edit.recent"),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      for (const auto& path : m_recent) {
        page->addChild(
            ui::button({
                .text = path.filename().string(),
                .contentAlign = ButtonContentAlign::Start,
                .variant = ButtonVariant::Ghost,
                .tooltip = path.string(),
                .minWidth = 320.0F * scale,
                .onClick = [this, path] { (void)openFile(path); },
            })
        );
      }
    }
    return page;
  }

  std::unique_ptr<Node> EditorWindow::buildCsvTable(float scale) {
    Document* doc = current();
    const auto rows = parseCsv(doc->buffer.text());
    std::size_t columns = 0;
    for (const auto& row : rows) {
      columns = std::max(columns, row.size());
    }
    // Ancho por columna: el texto más largo (hasta 40 caracteres).
    std::vector<std::size_t> widths(columns, 4);
    for (std::size_t r = 0; r < std::min(rows.size(), kCsvRowLimit); ++r) {
      for (std::size_t c = 0; c < rows[r].size(); ++c) {
        widths[c] = std::clamp<std::size_t>(g_utf8_strlen(rows[r][c].c_str(), -1), widths[c], 40);
      }
    }
    const float charWidth = m_prefs.fontSize * 0.62F * scale;
    ScrollView* view = nullptr;
    auto scroll = ui::scrollView({.out = &view, .flexGrow = 1.0F});
    Flex* content = view->content();
    content->setGap(0.0F);
    for (std::size_t r = 0; r < std::min(rows.size(), kCsvRowLimit); ++r) {
      auto line = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingV = 3.0F * scale,
          .paddingH = Style::spaceMd * scale,
          .fill = r == 0 ? colorSpecFromRole(ColorRole::SurfaceContainer)
                         : colorSpecFromRole(r % 2 == 0 ? ColorRole::Surface : ColorRole::SurfaceContainerLow),
      });
      for (std::size_t c = 0; c < columns; ++c) {
        line->addChild(
            ui::label({
                .text = c < rows[r].size() ? rows[r][c] : std::string(),
                .fontSize = m_prefs.fontSize * scale,
                .fontWeight = r == 0 ? FontWeight::SemiBold : FontWeight::Normal,
                .fontFamily = "monospace",
                .minWidth = static_cast<float>(widths[c] + 2) * charWidth,
                .maxWidth = static_cast<float>(widths[c] + 2) * charWidth,
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
            })
        );
      }
      content->addChild(std::move(line));
    }
    if (rows.size() > kCsvRowLimit) {
      content->addChild(
          ui::label({
              .text = tr("apps.edit.csv.limit", "shown", kCsvRowLimit, "total", rows.size()),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
    return scroll;
  }

  std::unique_ptr<Node> EditorWindow::buildPreferences(float scale) {
    ScrollView* view = nullptr;
    auto scroll = ui::scrollView(
        {.out = &view,
         .viewportPaddingH = Style::spaceLg * scale,
         .viewportPaddingV = Style::spaceMd * scale,
         .flexGrow = 1.0F}
    );
    Flex* content = view->content();
    content->setGap(Style::spaceMd * scale);
    content->setAlign(FlexAlign::Stretch);
    const auto row = [&](std::string title, std::unique_ptr<Node> control) {
      content->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
              ui::label({.text = std::move(title), .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F}),
              std::move(control)
          )
      );
    };
    const auto toggle = [&](bool value, std::function<void(bool)> onChange) {
      return ui::toggle({.checked = value, .scale = scale, .onChange = std::move(onChange)});
    };
    row(tr("apps.edit.preferences.font-size"),
        ui::stepper({
            .minValue = 8,
            .maxValue = 40,
            .value = static_cast<int>(m_prefs.fontSize),
            .scale = scale,
            .onValueChanged = [this](int value) { m_prefs.fontSize = static_cast<float>(value); },
        }));
    row(tr("apps.edit.preferences.wrap"), toggle(m_prefs.wrap, [this](bool v) { m_prefs.wrap = v; }));
    row(tr("apps.edit.preferences.line-numbers"),
        toggle(m_prefs.lineNumbers, [this](bool v) { m_prefs.lineNumbers = v; }));
    row(tr("apps.edit.preferences.highlight-line"),
        toggle(m_prefs.highlightLine, [this](bool v) { m_prefs.highlightLine = v; }));
    std::vector<std::string> indents;
    for (const int width : kIndentWidths) {
      indents.push_back(tr("apps.edit.indent.spaces", "count", width));
    }
    indents.push_back(tr("apps.edit.indent.tabs"));
    const auto selected = m_prefs.insertSpaces
        ? static_cast<std::size_t>(std::ranges::find(kIndentWidths, m_prefs.tabWidth) - kIndentWidths.begin())
        : kIndentWidths.size();
    row(tr("apps.edit.preferences.indent"),
        ui::select({
            .options = std::move(indents),
            .selectedIndex = std::min(selected, kIndentWidths.size()),
            .width = 170.0F * scale,
            .onSelectionChanged = [this](std::size_t index, std::string_view) {
              m_prefs.insertSpaces = index < kIndentWidths.size();
              if (m_prefs.insertSpaces) {
                m_prefs.tabWidth = kIndentWidths[index];
              }
            },
        }));
    row(tr("apps.edit.preferences.restore"),
        toggle(m_prefs.restoreSession, [this](bool v) { m_prefs.restoreSession = v; }));
    content->addChild(
        ui::label({
            .text = tr("apps.edit.preferences.shortcuts"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 12,
        })
    );
    return scroll;
  }

  std::unique_ptr<Node> EditorWindow::buildConfirm(float width, float height, float scale) {
    const Document& doc = *m_docs[m_closing.front()];
    auto overlay = ui::node({.width = width, .height = height, .zIndex = 10});
    // El velo capta los clics: nada de atrás responde mientras se decide.
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
    card->addChild(
        ui::label({
            .text = tr("apps.edit.unsaved.title", "file", title(doc)),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 2,
        })
    );
    card->addChild(
        ui::label({
            .text = tr("apps.edit.unsaved.body"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 3,
        })
    );
    // Después de decidir sobre este, el siguiente pendiente; al final se cierra la pestaña o la app.
    const auto next = [this] {
      const std::size_t index = m_closing.front();
      m_closing.erase(m_closing.begin());
      if (!m_quitAfterClosing) {
        closeDocument(index);
        return;
      }
      if (m_closing.empty()) {
        saveState();
        m_app.quit();
      }
      m_app.requestRebuild();
    };
    card->addChild(
        ui::row(
            {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
            ui::button(
                {.text = tr("apps.edit.unsaved.cancel"),
                 .variant = ButtonVariant::Ghost,
                 .onClick =
                     [this] {
                       m_closing.clear();
                       m_quitAfterClosing = false;
                       m_app.requestRebuild();
                     }}
            ),
            ui::button(
                {.text = tr("apps.edit.unsaved.discard"),
                 .variant = ButtonVariant::Destructive,
                 .onClick =
                     [this, next] {
                       m_docs[m_closing.front()]->buffer.markSaved(); // descartados: ya no cuentan como cambios
                       next();
                     }}
            ),
            ui::button({.text = tr("apps.edit.save"), .variant = ButtonVariant::Primary, .onClick = [this, next] {
                          Document& d = *m_docs[m_closing.front()];
                          if (d.path.empty()) {
                            saveAs(m_closing.front(), next);
                          } else if (saveDocument(d)) {
                            next();
                          }
                        }})
        )
    );
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      card->layout(*renderer);
    }
    card->setPosition((width - card->width()) * 0.5F, (height - card->height()) * 0.4F);
    overlay->addChild(std::move(card));
    return overlay;
  }

  // ── Teclado ─────────────────────────────────────────────────────────────────────────────────────────

  bool EditorWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit || !m_closing.empty()) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    Document* doc = current();
    if (event.sym == XKB_KEY_F3) {
      findStep(!shift);
      return true;
    }
    if (!ctrl) {
      if (event.sym == XKB_KEY_Escape && (m_findOpen || m_gotoOpen || m_showPreferences)) {
        m_findOpen = m_replaceOpen = m_gotoOpen = m_showPreferences = false;
        m_app.requestRebuild();
        return true;
      }
      return false;
    }
    switch (event.sym) {
    case XKB_KEY_n:
      newDocument();
      return true;
    case XKB_KEY_o:
      openDialog();
      return true;
    case XKB_KEY_s:
      if (doc != nullptr) {
        if (doc->path.empty()) {
          saveAs(m_active);
        } else {
          (void)saveDocument(*doc);
        }
      }
      return true;
    case XKB_KEY_S:
      saveAs(m_active);
      return true;
    case XKB_KEY_w:
      requestClose(m_active);
      return true;
    case XKB_KEY_q:
      requestQuit();
      return true;
    case XKB_KEY_f:
    case XKB_KEY_h:
      if (doc != nullptr) {
        captureView();
        m_findOpen = true;
        m_replaceOpen = event.sym == XKB_KEY_h || m_replaceOpen;
        m_focusFind = true;
        // La selección (de una línea) pasa a ser lo que se busca.
        if (m_view != nullptr) {
          const std::string selected = m_view->selectedText();
          if (!selected.empty() && selected.find('\n') == std::string::npos) {
            m_findText = selected;
          }
        }
        m_app.requestRebuild();
      }
      return true;
    case XKB_KEY_g:
      findStep(true);
      return true;
    case XKB_KEY_G:
      findStep(false);
      return true;
    case XKB_KEY_l:
    case XKB_KEY_i:
      if (doc != nullptr) {
        captureView();
        m_gotoOpen = true;
        m_app.requestRebuild();
      }
      return true;
    case XKB_KEY_F:
      formatDocument();
      return true;
    case XKB_KEY_Page_Down:
    case XKB_KEY_Tab:
      if (!m_docs.empty()) {
        activate((m_active + 1) % m_docs.size());
      }
      return true;
    case XKB_KEY_Page_Up:
    case XKB_KEY_ISO_Left_Tab:
      if (!m_docs.empty()) {
        activate((m_active + m_docs.size() - 1) % m_docs.size());
      }
      return true;
    case XKB_KEY_plus:
    case XKB_KEY_equal:
      zoom(1);
      return true;
    case XKB_KEY_minus:
      zoom(-1);
      return true;
    case XKB_KEY_0:
      zoom(0);
      return true;
    default:
      return false;
    }
  }

  // ── Estado guardado ────────────────────────────────────────────────────────────────────────────────

  void EditorWindow::loadState() {
    toml::table root;
    try {
      root = toml::parse_file(statePath().string());
    } catch (const toml::parse_error&) {
      return;
    }
    m_prefs.fontSize = static_cast<float>(std::clamp(root["font_size"].value_or(14.0), 8.0, 40.0));
    m_prefs.wrap = root["wrap"].value_or(true);
    m_prefs.lineNumbers = root["line_numbers"].value_or(true);
    m_prefs.highlightLine = root["highlight_line"].value_or(true);
    const int width = root["tab_width"].value_or(4);
    m_prefs.tabWidth = std::ranges::contains(kIndentWidths, width) ? width : 4;
    m_prefs.insertSpaces = root["insert_spaces"].value_or(true);
    m_prefs.restoreSession = root["restore_session"].value_or(true);
    if (const auto* recent = root["recent"].as_array()) {
      for (const auto& node : *recent) {
        if (const auto path = node.value<std::string>(); path && fs::exists(*path)) {
          m_recent.emplace_back(*path);
        }
      }
    }
    // Los archivos que quedaron abiertos, con su cursor (solo si se abre sin archivos; ver main).
    if (const auto* files = root["files"].as_array(); files != nullptr && m_prefs.restoreSession) {
      for (const auto& node : *files) {
        const auto* entry = node.as_table();
        if (entry == nullptr) {
          continue;
        }
        const auto path = (*entry)["path"].value<std::string>();
        if (path && fs::exists(*path)) {
          const TextPos cursor{
              static_cast<std::size_t>((*entry)["line"].value_or(0)),
              static_cast<std::size_t>((*entry)["column"].value_or(0))
          };
          (void)openFile(*path, cursor, static_cast<float>((*entry)["scroll"].value_or(0.0)));
        }
      }
      const auto active = static_cast<std::size_t>(root["active"].value_or(0));
      m_active = std::min(active, m_docs.empty() ? 0 : m_docs.size() - 1);
    }
  }

  void EditorWindow::saveState() const {
    toml::table root{
        {"font_size", static_cast<double>(m_prefs.fontSize)},
        {"wrap", m_prefs.wrap},
        {"line_numbers", m_prefs.lineNumbers},
        {"highlight_line", m_prefs.highlightLine},
        {"tab_width", m_prefs.tabWidth},
        {"insert_spaces", m_prefs.insertSpaces},
        {"restore_session", m_prefs.restoreSession},
        {"active", static_cast<std::int64_t>(m_active)},
    };
    toml::array recent;
    for (const auto& path : m_recent) {
      recent.push_back(path.string());
    }
    root.insert("recent", std::move(recent));
    toml::array files;
    for (const auto& doc : m_docs) {
      if (!doc->path.empty()) {
        files.push_back(
            toml::table{
                {"path", doc->path.string()},
                {"line", static_cast<std::int64_t>(doc->cursor.line)},
                {"column", static_cast<std::int64_t>(doc->cursor.col)},
                {"scroll", static_cast<double>(doc->scrollY)},
            }
        );
      }
    }
    root.insert("files", std::move(files));
    std::error_code ec;
    fs::create_directories(statePath().parent_path(), ec);
    std::ofstream(statePath(), std::ios::trunc) << root << '\n';
  }

} // namespace okaru::editor
