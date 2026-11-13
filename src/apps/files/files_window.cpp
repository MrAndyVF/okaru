#include "apps/files/files_window.h"

#include "apps/common/glib_poll_source.h"
#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "apps/common/tab_strip.h"
#include "apps/pdf/pdf_view.h"
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
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cmath>
#include <cursor-shape-v1-client-protocol.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <sstream>
#include <thread>
#include <toml++/toml.hpp>
#include <pwd.h>
#include <unistd.h>

namespace okaru::files {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;
    using i18n::trp;

    constexpr float kSidebarWidth = 220.0F;
    constexpr float kColumnWidth = 250.0F;
    constexpr std::size_t kFolderModesLimit = 400;
    constexpr std::array<float, 7> kIconSizes{32.0F, 48.0F, 64.0F, 80.0F, 96.0F, 128.0F, 160.0F};

    fs::path statePath() { return fs::path(FileUtils::stateDir()) / "files.toml"; }

    std::string homeUri() { return pathToUri(g_get_home_dir()); }

    std::string sortId(SortKey key) {
      switch (key) {
      case SortKey::Size:
        return "size";
      case SortKey::Modified:
        return "modified";
      case SortKey::Type:
        return "type";
      case SortKey::Name:
        break;
      }
      return "name";
    }

    SortKey sortFromId(std::string_view id) {
      return id == "size" ? SortKey::Size : id == "modified" ? SortKey::Modified : id == "type" ? SortKey::Type : SortKey::Name;
    }

    std::string filesystemId(const std::string& uri) {
      GFile* file = g_file_new_for_uri(uri.c_str());
      GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
      std::string out;
      if (info != nullptr) {
        if (const char* id = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM); id != nullptr) {
          out = id;
        }
        g_object_unref(info);
      }
      g_object_unref(file);
      return out;
    }

    std::pair<std::uint64_t, std::uint64_t> filesystemSpace(const std::string& uri) {
      GFile* file = g_file_new_for_uri(uri.c_str());
      std::pair<std::uint64_t, std::uint64_t> out{0, 0};
      if (g_file_is_native(file) != FALSE) {
        GFileInfo* info = g_file_query_filesystem_info(file, "filesystem::free,filesystem::size", nullptr, nullptr);
        if (info != nullptr) {
          out = {g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE),
                 g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)};
          g_object_unref(info);
        }
      }
      g_object_unref(file);
      return out;
    }

    // Suma recursiva para Propiedades (en un hilo).
    void countRecursive(const std::string& uri, std::uint64_t& bytes, std::size_t& files, std::size_t& folders,
                        const std::weak_ptr<int>& alive, int depth = 0) {
      std::vector<FileEntry> entries;
      std::string error;
      if (depth > 64 || alive.expired() || !listDirectory(uri, entries, error)) {
        return;
      }
      for (const auto& e : entries) {
        if (e.directory && !e.symlink && !e.mountable) {
          ++folders;
          countRecursive(e.uri, bytes, files, folders, alive, depth + 1);
        } else {
          ++files;
          bytes += e.size;
        }
      }
    }

    std::string trimmed(std::string text) {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
      }
      return text;
    }

    std::vector<std::string> splitLines(const std::string& text) {
      std::vector<std::string> out;
      std::istringstream in(text);
      for (std::string line; std::getline(in, line);) {
        line = trimmed(line);
        if (!line.empty() && !line.starts_with('#')) {
          out.push_back(line);
        }
      }
      return out;
    }

  } // namespace

  FilesWindow::FilesWindow(apps::OkaruApp& app, const std::vector<std::string>& locations) : m_app(app) {
    m_glib = std::make_unique<apps::GlibPollSource>();
    m_app.addPollSource(m_glib.get());
    loadState();

    // Los nombres de los lugares se traducen al construir la escena (el idioma se carga dentro de run()).
    // Idioma, tema o paleta cambiados: lugares traducidos de nuevo e iconos (acento) otra vez.
    m_app.setReloadCallback([this] {
      m_placesReady = false;
      m_iconsDirty = true;
    });
    m_thumbs.setOnReady([this] {
      if (m_view != nullptr) {
        m_view->markLayoutDirty();
      }
      for (const auto& [view, pane] : m_columnViews) {
        view->markLayoutDirty();
      }
      updateDetails();
      m_app.invalidate();
    });
    // Con el contexto GL activo: soltar previsualizaciones viejas y subir la foto recién decodificada.
    m_app.setBeforeRender([this](Renderer& renderer) {
      for (auto& handle : m_retiredTextures) {
        renderer.textureManager().unload(handle);
      }
      m_retiredTextures.clear();
      for (auto& tab : m_retiredPdfs) {
        for (auto* slots : {&tab->pages, &tab->thumbs}) {
          for (auto& slot : *slots) {
            if (slot.handle.valid()) {
              renderer.textureManager().unload(slot.handle);
            }
          }
        }
      }
      m_retiredPdfs.clear();
      if (!m_preview.pending.rgba.empty()) {
        m_preview.texture =
            renderer.textureManager().loadFromRgba(m_preview.pending.rgba.data(), m_preview.pending.width, m_preview.pending.height);
        m_preview.width = m_preview.pending.width;
        m_preview.height = m_preview.pending.height;
        m_preview.pending = {};
        if (m_imagePreview != nullptr) {
          m_imagePreview->setTexture(m_preview.texture, m_preview.width, m_preview.height);
          updatePreviewLabels();
        }
        // Este cuadro ya se maquetó y su marca se borra al pintar: se vuelve a marcar fuera del cuadro.
        DeferredCall::callLater([this] {
          if (m_imagePreview != nullptr) {
            m_imagePreview->markLayoutDirty();
          }
          m_app.invalidate();
        });
      }
    });
    m_ops.setOnProgress([this](const OpProgress& progress) {
      m_progress = progress;
      updateStatus();
    });
    m_ops.setOnConflict([this](const Conflict& conflict, std::function<void(ConflictAnswer)> reply) {
      m_conflict = conflict;
      m_conflictReply = std::move(reply);
      m_conflictAll = false;
      m_overlay = Overlay::Conflict;
      m_app.requestRebuild();
    });
    m_ops.setOnDone([this](OpResult result) { onOperationDone(std::move(result)); });

    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setDropCallback([this](std::vector<std::string> paths) { dropExternal(paths); });
    m_app.setAfterBuild([this] {
      if (m_overlay != Overlay::None && m_dialogInput != nullptr) {
        if (m_focusDialog) {
          m_app.focus(m_dialogInput->inputArea());
          m_dialogInput->selectAll();
          m_focusDialog = false;
        }
      } else if (m_editingPath && m_pathInput != nullptr) {
        m_app.focus(m_pathInput->inputArea());
      } else if (m_focusFind && m_findInput != nullptr) {
        m_app.focus(m_findInput->inputArea());
        m_focusFind = false;
      } else if (m_view != nullptr && (m_findInput == nullptr || !m_findInput->inputArea()->focused())) {
        m_app.focus(m_view->inputArea());
      }
    });
    m_app.setOnClose([this] {
      saveState();
      if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
        m_icons.clear(*renderer);
        for (auto& [key, thumb] : m_thumbTextures) {
          renderer->textureManager().unload(thumb.handle);
        }
        m_thumbTextures.clear();
        startPreview(nullptr);
        for (auto& handle : m_retiredTextures) {
          renderer->textureManager().unload(handle);
        }
        for (auto& tab : m_retiredPdfs) {
          for (auto* slots : {&tab->pages, &tab->thumbs}) {
            for (auto& slot : *slots) {
              if (slot.handle.valid()) {
                renderer->textureManager().unload(slot.handle);
              }
            }
          }
        }
        m_retiredTextures.clear();
        m_retiredPdfs.clear();
      }
      m_tabs.clear();
    });

    // KDE Connect avisa por D-Bus al aparecer, desaparecer, montar o desmontar un teléfono.
    if (GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr); bus != nullptr) {
      m_dbus = bus;
      m_dbusSubscription = g_dbus_connection_signal_subscribe(
          bus, "org.kde.kdeconnect", nullptr, nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection*, const gchar*, const gchar*, const gchar* interface, const gchar*, GVariant*, gpointer self) {
            const std::string_view name = interface != nullptr ? interface : "";
            // Batería, notificaciones y demás no cambian la lista de teléfonos.
            if (name == "org.kde.kdeconnect.daemon" || name == "org.kde.kdeconnect.device"
                || name == "org.kde.kdeconnect.device.sftp") {
              auto* window = static_cast<FilesWindow*>(self);
              window->m_phonesTimer.start(std::chrono::milliseconds(700), [window] { window->refreshPhones(); });
            }
          },
          this, nullptr
      );
    }
    refreshPhones();

    for (const auto& location : locations) {
      openTab(location, false);
    }
    if (m_tabs.empty()) {
      openTab(homeUri(), false);
    }
    m_active = 0;
  }

  FilesWindow::~FilesWindow() {
    if (m_dbus != nullptr) {
      g_dbus_connection_signal_unsubscribe(static_cast<GDBusConnection*>(m_dbus), m_dbusSubscription);
      g_object_unref(m_dbus);
    }
    if (m_mountOp != nullptr) {
      g_mount_operation_reply(G_MOUNT_OPERATION(m_mountOp), G_MOUNT_OPERATION_ABORTED);
      g_object_unref(m_mountOp);
    }
  }

  // ── Pestañas y navegación ──────────────────────────────────────────────────────────────────────────

  FilesWindow::Mode FilesWindow::mode() const {
    const Tab& t = *m_tabs[m_active];
    if (!t.columns.empty() || t.mode == ViewMode::Compact) {
      return Mode::Columns;
    }
    return t.mode == ViewMode::Grid ? Mode::Grid : Mode::List;
  }

  void FilesWindow::setMode(Mode next) {
    Tab& t = tab();
    t.mode = next == Mode::Grid ? ViewMode::Grid : next == Mode::Columns ? ViewMode::Compact : ViewMode::List;
    m_folderModes[t.model->uri()] = next;
    if (m_folderModes.size() > kFolderModesLimit) {
      m_folderModes.erase(m_folderModes.begin());
    }
    m_defaultMode = next;
    syncColumns();
    m_app.requestRebuild();
  }

  void FilesWindow::openTab(const std::string& uri, bool activate) {
    auto t = std::make_unique<Tab>();
    t->model->setOnChanged([this] { onModelChanged(); });
    m_tabs.push_back(std::move(t));
    const std::size_t previous = m_active;
    m_active = m_tabs.size() - 1;
    navigate(uri, false);
    if (!activate) {
      m_active = std::min(previous, m_tabs.size() - 1);
    }
    m_app.requestRebuild();
  }

  void FilesWindow::closeTab(std::size_t index) {
    if (index >= m_tabs.size()) {
      return;
    }
    if (m_tabs.size() == 1) {
      m_app.quit();
      return;
    }
    m_tabs.erase(m_tabs.begin() + static_cast<long>(index));
    if (m_active >= m_tabs.size() || m_active > index) {
      m_active = m_active == 0 ? 0 : m_active - 1;
    }
    m_app.requestRebuild();
  }

  void FilesWindow::activateTab(std::size_t index) {
    if (index < m_tabs.size()) {
      m_active = index;
      m_findOpen = !tab().model->filter().empty() || tab().model->searching();
      m_app.requestRebuild();
    }
  }

  void FilesWindow::navigate(const std::string& requested, bool record, const std::string& reveal) {
    if (requested.empty()) {
      return;
    }
    Tab& t = tab();
    const std::string current = t.model->uri();
    if (record && !current.empty() && current != requested) {
      t.back.push_back(current);
      t.forward.clear();
    }
    m_editingPath = false;
    m_findOpen = false;
    m_findText.clear();
    t.view = {};
    t.view.revealUri = reveal;
    if (const auto it = m_folderModes.find(requested); it != m_folderModes.end()) {
      t.mode = it->second == Mode::Grid ? ViewMode::Grid : it->second == Mode::Columns ? ViewMode::Compact : ViewMode::List;
    } else if (current.empty()) {
      t.mode = m_defaultMode == Mode::Grid ? ViewMode::Grid
          : m_defaultMode == Mode::Columns ? ViewMode::Compact
                                           : ViewMode::List;
    }
    t.model->setOptions(optionsFor(requested));
    t.model->load(requested);
    syncColumns();
    m_app.requestRebuild();
  }

  void FilesWindow::goBack() {
    Tab& t = tab();
    if (t.back.empty()) {
      return;
    }
    const std::string target = t.back.back();
    t.back.pop_back();
    t.forward.push_back(t.model->uri());
    navigate(target, false, t.forward.back());
  }

  void FilesWindow::goForward() {
    Tab& t = tab();
    if (t.forward.empty()) {
      return;
    }
    const std::string target = t.forward.back();
    t.forward.pop_back();
    t.back.push_back(t.model->uri());
    navigate(target, false);
  }

  void FilesWindow::goUp() {
    const std::string current = tab().model->uri();
    const std::string parent = parentUri(current);
    if (!parent.empty()) {
      navigate(parent, true, current);
    }
  }

  void FilesWindow::syncColumns() {
    Tab& t = tab();
    m_columnViews.clear(); // las vistas viejas apuntan a columnas que van a cambiar
    m_syncingColumns = true;
    struct Done {
      bool& flag;
      ~Done() { flag = false; }
    } done{m_syncingColumns};
    if (t.mode != ViewMode::Compact) {
      t.columns.clear();
      return;
    }
    // Antepasados de la carpeta actual hasta la personal (o la raíz del montaje), del más alto al padre.
    std::vector<std::string> chain;
    const std::string home = homeUri();
    std::string uri = t.model->uri();
    while (chain.size() < 12 && uri != home) {
      const std::string parent = parentUri(uri);
      if (parent.empty()) {
        break;
      }
      chain.push_back(parent);
      uri = parent;
    }
    std::ranges::reverse(chain);
    std::vector<ColumnPane> next;
    for (std::size_t i = 0; i < chain.size(); ++i) {
      ColumnPane pane;
      if (const auto it = std::ranges::find_if(t.columns, [&](const ColumnPane& p) { return p.model != nullptr && p.model->uri() == chain[i]; });
          it != t.columns.end()) {
        pane = std::move(*it);
      } else {
        pane.model = std::make_unique<DirectoryModel>();
        pane.model->setOnChanged([this] { onModelChanged(); });
        pane.model->setOptions(optionsFor(chain[i]));
        pane.model->load(chain[i]);
      }
      // La carpeta del siguiente paso queda seleccionada.
      const std::string child = i + 1 < chain.size() ? chain[i + 1] : t.model->uri();
      pane.state.selected = {child};
      pane.state.revealUri = child;
      next.push_back(std::move(pane));
    }
    t.columns = std::move(next);
  }

  void FilesWindow::onModelChanged() {
    // Durante syncColumns las columnas están a medio rearmar: la escena se reconstruye al terminar.
    if (m_tabs.empty() || m_syncingColumns) {
      return;
    }
    Tab& t = tab();
    // Sin montar (smb://, sftp://…): montar el volumen y volver a listar.
    if (!t.model->loading() && t.model->errorCode() == G_IO_ERROR_NOT_MOUNTED && m_mountingUri != t.model->uri()) {
      const std::string uri = t.model->uri();
      m_mountingUri = uri;
      GFile* file = g_file_new_for_uri(uri.c_str());
      auto* operation = static_cast<GMountOperation*>(newMountOperation());
      std::weak_ptr<int> alive = m_alive;
      auto* data = new std::pair<FilesWindow*, std::weak_ptr<int>>(this, alive);
      g_file_mount_enclosing_volume(
          file, G_MOUNT_MOUNT_NONE, operation, nullptr,
          [](GObject* source, GAsyncResult* result, gpointer raw) {
            std::unique_ptr<std::pair<FilesWindow*, std::weak_ptr<int>>> ctx(
                static_cast<std::pair<FilesWindow*, std::weak_ptr<int>>*>(raw)
            );
            GError* error = nullptr;
            const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(source), result, &error) != FALSE;
            if (ctx->second.expired()) {
              g_clear_error(&error);
              return;
            }
            ctx->first->m_mountingUri.clear();
            if (ok || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
              ctx->first->tab().model->reload();
            } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
              // Cancelado en el diálogo: nada que avisar.
            } else {
              ctx->first->setMessage(error != nullptr ? error->message : "?", true);
            }
            g_clear_error(&error);
          },
          data
      );
      g_object_unref(operation);
      g_object_unref(file);
    }
    if (m_view != nullptr) {
      m_view->setEmptyText(
          t.model->loading()          ? ""
              : !t.model->error().empty() ? t.model->error()
              : t.model->searching()      ? (t.model->searchRunning() ? tr("apps.files.searching") : tr("apps.files.no-results"))
              : !t.model->filter().empty() ? tr("apps.files.no-results")
                                           : tr("apps.files.empty-folder")
      );
      m_view->setEntries(&t.model->entries(), &t.view);
    }
    for (const auto& [view, pane] : m_columnViews) {
      if (pane < t.columns.size() && t.columns[pane].model != nullptr) {
        view->setEntries(&t.columns[pane].model->entries(), &t.columns[pane].state);
      }
    }
    updateStatus();
    m_app.invalidate();
  }

  void FilesWindow::reloadAll() {
    for (auto& t : m_tabs) {
      t->model->setOptions(optionsFor(t->model->uri()));
      for (auto& pane : t->columns) {
        pane.model->setOptions(optionsFor(pane.model->uri()));
      }
    }
    m_app.requestRebuild();
  }

  // ── Selección y apertura ───────────────────────────────────────────────────────────────────────────

  std::vector<const FileEntry*> FilesWindow::selection() const {
    std::vector<const FileEntry*> out;
    const Tab& t = *m_tabs[m_active];
    for (const auto& e : t.model->entries()) {
      if (t.view.selected.contains(e.uri)) {
        out.push_back(&e);
      }
    }
    return out;
  }

  std::vector<std::string> FilesWindow::selectedUris() const {
    std::vector<std::string> out;
    for (const FileEntry* e : selection()) {
      out.push_back(e->uri);
    }
    return out;
  }

  FileView* FilesWindow::activeView() const { return m_view; }

  bool FilesWindow::inTrash() const { return m_tabs[m_active]->model->uri().starts_with("trash:"); }

  bool FilesWindow::canWriteHere() const {
    const std::string& uri = m_tabs[m_active]->model->uri();
    return !uri.starts_with("trash:") && !uri.starts_with("recent:") && !uri.starts_with("network:")
        && !m_tabs[m_active]->model->searching();
  }

  void FilesWindow::activateEntry(const FileEntry& entry, bool newTab) {
    const std::string uri = entry.target.empty() ? entry.uri : entry.target;
    if (entry.directory) {
      if (newTab) {
        openTab(uri, false);
      } else {
        navigate(uri);
      }
      return;
    }
    if (uri.starts_with("trash:")) {
      showProperties({entry.uri});
      return;
    }
    GError* error = nullptr;
    if (g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error) == FALSE) {
      g_clear_error(&error);
      tab().view.selected = {entry.uri};
      openWithDialog();
    }
  }

  void FilesWindow::openSelection() {
    const auto items = selection();
    std::size_t folders = 0;
    for (const FileEntry* e : items) {
      folders += e->directory ? 1 : 0;
    }
    // Varias carpetas: cada una en su pestaña. Una sola: entrar.
    for (const FileEntry* e : items) {
      const FileEntry copy = *e;
      activateEntry(copy, folders > 1 && copy.directory);
      if (folders == 1 && copy.directory) {
        return;
      }
    }
  }

  void FilesWindow::openWithDialog() {
    const auto items = selection();
    if (items.empty() || items.front()->directory) {
      return;
    }
    m_openWith.clear();
    m_openWithType = items.front()->contentType;
    m_openWithDefault = false;
    GList* apps = g_app_info_get_all_for_type(m_openWithType.c_str());
    if (apps == nullptr) {
      apps = g_app_info_get_all();
    }
    for (GList* l = apps; l != nullptr; l = l->next) {
      auto* info = G_APP_INFO(l->data);
      if (g_app_info_should_show(info) == FALSE || g_app_info_get_id(info) == nullptr) {
        continue;
      }
      OpenWithApp app{.id = g_app_info_get_id(info), .name = g_app_info_get_display_name(info)};
      if (GIcon* icon = g_app_info_get_icon(info); icon != nullptr && G_IS_THEMED_ICON(icon)) {
        app.icon = g_themed_icon_get_names(G_THEMED_ICON(icon))[0];
      }
      m_openWith.push_back(std::move(app));
    }
    g_list_free_full(apps, g_object_unref);
    m_dialogUris = selectedUris();
    m_overlay = Overlay::OpenWith;
    m_app.requestRebuild();
  }

  void FilesWindow::launchWith(const std::string& appId, bool makeDefault) {
    GList* apps = g_app_info_get_all();
    GAppInfo* chosen = nullptr;
    for (GList* l = apps; l != nullptr; l = l->next) {
      if (chosen == nullptr && appId == g_app_info_get_id(G_APP_INFO(l->data))) {
        chosen = G_APP_INFO(g_object_ref(l->data));
      }
    }
    g_list_free_full(apps, g_object_unref);
    if (chosen == nullptr) {
      return;
    }
    GList* uris = nullptr;
    for (const auto& uri : m_dialogUris) {
      uris = g_list_append(uris, g_strdup(uri.c_str()));
    }
    GError* error = nullptr;
    if (makeDefault && !m_openWithType.empty()) {
      (void)g_app_info_set_as_default_for_type(chosen, m_openWithType.c_str(), nullptr);
    }
    if (g_app_info_launch_uris(chosen, uris, nullptr, &error) == FALSE) {
      setMessage(error != nullptr ? error->message : "?", true);
      g_clear_error(&error);
    }
    g_list_free_full(uris, g_free);
    g_object_unref(chosen);
    m_overlay = Overlay::None;
    m_app.requestRebuild();
  }

  void FilesWindow::shareDialog() {
    m_sharePaths.clear();
    for (const FileEntry* e : selection()) {
      if (!e->path.empty()) {
        m_sharePaths.push_back(e->path);
      }
    }
    if (m_sharePaths.empty()) {
      setMessage(tr("apps.files.share.no-local"), true);
      return;
    }
    m_shareTargets.clear();
    m_shareSearching = true;
    m_overlay = Overlay::Share;
    std::weak_ptr<int> alive = m_alive;
    std::thread([this, alive] {
      auto targets = discoverShareTargets();
      DeferredCall::callLater([this, alive, targets = std::move(targets)]() mutable {
        if (alive.expired()) {
          return;
        }
        m_shareTargets = std::move(targets);
        m_shareSearching = false;
        if (m_overlay == Overlay::Share) {
          m_app.requestRebuild();
        }
      });
    }).detach();
    m_app.requestRebuild();
  }

  void FilesWindow::sendTo(const ShareTarget& target) {
    m_overlay = Overlay::None;
    m_app.requestRebuild();
    if (!target.acceptsFolders()) {
      for (const auto& path : m_sharePaths) {
        if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
          setMessage(tr("apps.files.share.no-folders", "name", target.name), true);
          return;
        }
      }
    }
    // Las apps (LocalSend, Telegram, correo) se abren con los archivos; los dispositivos reciben la transferencia.
    const bool device = target.kind == ShareKind::KdeConnect || target.kind == ShareKind::Bluetooth;
    setMessage(tr(device ? "apps.files.share.sending" : "apps.files.share.opening", "name", target.name));
    std::weak_ptr<int> alive = m_alive;
    shareFiles(target, m_sharePaths, [this, alive, device, name = target.name](bool ok, std::string detail) {
      if (alive.expired()) {
        return;
      }
      if (ok) {
        if (device) {
          setMessage(tr("apps.files.share.sent", "name", name));
        }
      } else {
        setMessage(tr("apps.files.share.failed", "name", name) + (detail.empty() ? "" : ": " + detail), true);
      }
    });
  }

  TextureHandle FilesWindow::iconFor(Renderer& renderer, const FileEntry& entry, int pixels) {
    // Icono propio > miniatura > icono del tema (con el color de la carpeta).
    if (m_thumbnails && entry.customIcon.empty() && entry.customIconName.empty() && m_thumbs.supports(entry)) {
      if (const auto path = m_thumbs.get(entry, pixels > 128)) {
        const std::string key = *path + "#" + std::to_string(entry.modified);
        auto it = m_thumbTextures.find(key);
        if (it == m_thumbTextures.end()) {
          // Tope de texturas: se sueltan las que hace más tiempo no se dibujan.
          if (m_thumbTextures.size() >= 700) {
            std::vector<std::pair<std::uint64_t, std::string>> byAge;
            for (const auto& [k, t] : m_thumbTextures) {
              byAge.emplace_back(t.used, k);
            }
            std::ranges::sort(byAge);
            for (std::size_t i = 0; i < 200; ++i) {
              auto victim = m_thumbTextures.find(byAge[i].second);
              renderer.textureManager().unload(victim->second.handle);
              m_thumbTextures.erase(victim);
            }
          }
          it = m_thumbTextures.emplace(key, ThumbTexture{renderer.textureManager().loadFromFile(*path, 0)}).first;
        }
        it->second.used = ++m_thumbTick;
        if (it->second.handle.valid()) {
          return it->second.handle;
        }
      }
    }
    return m_icons.get(renderer, entry, pixels);
  }

  void FilesWindow::setMetadata(const std::vector<std::string>& uris, const char* key, const std::string& value) {
    std::size_t failed = 0;
    for (const auto& uri : uris) {
      GFile* file = g_file_new_for_uri(uri.c_str());
      GError* error = nullptr;
      // Vacío = quitar el atributo (G_FILE_ATTRIBUTE_TYPE_INVALID lo borra de los metadatos de gvfs).
      const gboolean ok = value.empty()
          ? g_file_set_attribute(file, key, G_FILE_ATTRIBUTE_TYPE_INVALID, nullptr, G_FILE_QUERY_INFO_NONE, nullptr, &error)
          : g_file_set_attribute_string(file, key, value.c_str(), G_FILE_QUERY_INFO_NONE, nullptr, &error);
      failed += ok == FALSE ? 1 : 0;
      g_clear_error(&error);
      g_object_unref(file);
    }
    if (failed > 0) {
      setMessage(tr("apps.files.metadata.failed"), true);
    }
    tab().model->reload();
    for (auto& pane : tab().columns) {
      pane.model->reload();
    }
  }

  void FilesWindow::folderColorDialog(bool global) {
    m_colorTargets.clear();
    if (!global) {
      for (const FileEntry* e : selection()) {
        if (e->directory) {
          m_colorTargets.push_back(e->uri);
        }
      }
      if (m_colorTargets.empty()) {
        return;
      }
    }
    m_overlay = Overlay::FolderColor;
    m_app.requestRebuild();
  }

  void FilesWindow::setFolderColor(const std::string& color) {
    m_overlay = Overlay::None;
    if (m_colorTargets.empty()) {
      m_icons.setDefaultFolderColor(color);
      saveState();
    } else {
      setMetadata(m_colorTargets, "metadata::okaru-folder-color", color);
    }
    m_app.requestRebuild();
  }

  void FilesWindow::chooseCustomIcon() {
    const auto uris = selectedUris();
    SessionBus* bus = m_app.sessionBus();
    if (uris.empty() || bus == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    std::weak_ptr<int> alive = m_alive;
    m_chooser->open(
        tr("apps.files.icon.choose"), {{.name = tr("apps.files.icon.filter"), .mimeTypes = {"image/png", "image/svg+xml", "image/jpeg", "image/webp"}}},
        false,
        [this, alive, uris](std::vector<std::string> paths) {
          if (alive.expired() || paths.empty()) {
            return;
          }
          setMetadata(uris, "metadata::custom-icon", pathToUri(paths.front()));
          setMetadata(uris, "metadata::custom-icon-name", {});
        }
    );
  }

  void FilesWindow::clearCustomIcon() {
    const auto uris = selectedUris();
    setMetadata(uris, "metadata::custom-icon", {});
    setMetadata(uris, "metadata::custom-icon-name", {});
  }

  // ── Portapapeles ───────────────────────────────────────────────────────────────────────────────────

  void FilesWindow::copyToClipboard(bool cut) {
    m_clipUris = selectedUris();
    m_clipCut = cut && canWriteHere();
    if (m_clipUris.empty()) {
      return;
    }
    std::string list;
    for (const auto& uri : m_clipUris) {
      list += uri + "\r\n";
    }
    // text/uri-list: lo entienden Nautilus, Dolphin, Thunar, navegadores y terminales.
    (void)process::runAsync({"wl-copy", "--type", "text/uri-list", "--", list.c_str()});
    setMessage(trp(cut ? "apps.files.clipboard.cut" : "apps.files.clipboard.copied", static_cast<long>(m_clipUris.size())));
    if (m_view != nullptr) {
      m_view->setDimmed(m_clipCut ? std::set<std::string>(m_clipUris.begin(), m_clipUris.end()) : std::set<std::string>{});
      m_app.invalidate();
    }
  }

  void FilesWindow::copyPaths() {
    std::string text;
    for (const FileEntry* e : selection()) {
      text += (text.empty() ? "" : "\n") + (e->path.empty() ? e->uri : e->path);
    }
    if (text.empty()) {
      text = uriToPath(tab().model->uri());
    }
    m_app.clipboard().setClipboardText(text);
    setMessage(tr("apps.files.clipboard.path"));
  }

  void FilesWindow::paste(const std::string& destination) {
    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 1U << 22U;
    const auto types = process::runSync(std::vector<std::string>{"wl-paste", "--list-types"}, options);
    if (!types) {
      return;
    }
    std::vector<std::string> uris;
    bool cut = false;
    if (types.out.contains("x-special/gnome-copied-files")) {
      // Nautilus y Thunar: primera línea "copy" o "cut".
      const auto data = process::runSync(std::vector<std::string>{"wl-paste", "--no-newline", "--type", "x-special/gnome-copied-files"}, options);
      auto lines = splitLines(data.out);
      if (!lines.empty()) {
        cut = lines.front() == "cut";
        uris.assign(lines.begin() + 1, lines.end());
      }
    } else if (types.out.contains("text/uri-list")) {
      const auto data = process::runSync(std::vector<std::string>{"wl-paste", "--no-newline", "--type", "text/uri-list"}, options);
      uris = splitLines(data.out);
      cut = m_clipCut && uris == m_clipUris;
    }
    if (uris.empty()) {
      setMessage(tr("apps.files.clipboard.empty"));
      return;
    }
    submit({.kind = cut ? OpKind::Move : OpKind::Copy, .sources = uris, .destination = destination});
    if (cut) {
      m_clipUris.clear();
      m_clipCut = false;
      (void)process::runAsync({"wl-copy", "--clear"});
    }
  }

  // ── Operaciones ────────────────────────────────────────────────────────────────────────────────────

  void FilesWindow::submit(OpRequest request) {
    m_progress = OpProgress{.kind = request.kind};
    m_ops.submit(std::move(request));
    updateStatus();
  }

  void FilesWindow::onOperationDone(OpResult result) {
    m_ops.remember(result);
    m_progress.reset();
    if (!result.untrashable.empty()) {
      // Sin papelera en ese sistema de archivos (red, USB sin .Trash): ofrecer borrar para siempre.
      m_dialogUris = result.untrashable;
      m_dialogText = tr("apps.files.delete.no-trash");
      m_overlay = Overlay::ConfirmDelete;
    } else if (!result.errors.empty()) {
      m_errors = result.errors;
      m_overlay = Overlay::Errors;
    } else if (result.cancelled) {
      setMessage(tr("apps.files.op.cancelled"));
    } else if (result.kind == OpKind::Trash && result.record) {
      setMessage(trp("apps.files.op.trashed", static_cast<long>(result.undo ? result.undo->items.size() : 0)));
    }
    // Lo creado queda seleccionado (pegar, carpeta nueva, renombrar).
    if (!result.created.empty() && result.kind != OpKind::Restore) {
      Tab& t = tab();
      t.view.selected = {result.created.begin(), result.created.end()};
      t.view.revealUri = result.created.front();
      t.model->reload();
    }
    m_app.requestRebuild();
  }

  void FilesWindow::startRename() {
    const auto items = selection();
    if (items.size() != 1 || !canWriteHere()) {
      return;
    }
    m_dialogTarget = items.front()->uri;
    m_dialogText = items.front()->display;
    m_overlay = Overlay::Rename;
    m_focusDialog = true;
    m_app.requestRebuild();
  }

  void FilesWindow::startNew(bool folder) {
    if (!canWriteHere()) {
      return;
    }
    m_dialogTarget = tab().model->uri();
    m_dialogText = folder ? tr("apps.files.new.folder-default") : tr("apps.files.new.file-default");
    m_overlay = folder ? Overlay::NewFolder : Overlay::NewFile;
    m_focusDialog = true;
    m_app.requestRebuild();
  }

  void FilesWindow::trashSelection() {
    auto uris = selectedUris();
    if (uris.empty()) {
      return;
    }
    if (inTrash()) {
      deleteSelection();
      return;
    }
    if (!canWriteHere() && !tab().model->searching()) {
      return;
    }
    submit({.kind = OpKind::Trash, .sources = std::move(uris)});
  }

  void FilesWindow::deleteSelection() {
    m_dialogUris = selectedUris();
    if (m_dialogUris.empty()) {
      return;
    }
    m_dialogText.clear();
    m_overlay = Overlay::ConfirmDelete;
    m_app.requestRebuild();
  }

  void FilesWindow::restoreSelection() {
    auto uris = selectedUris();
    if (!uris.empty()) {
      submit({.kind = OpKind::Restore, .sources = std::move(uris)});
    }
  }

  void FilesWindow::emptyTrash() {
    m_overlay = Overlay::ConfirmEmptyTrash;
    m_app.requestRebuild();
  }

  void FilesWindow::createLink() {
    auto uris = selectedUris();
    if (uris.empty() || !canWriteHere()) {
      return;
    }
    // Enlace junto al original: "nombre (2)" si ya existe.
    std::vector<std::string> targets;
    for (const auto& uri : uris) {
      const std::string dir = parentUri(uri);
      targets.push_back(childUri(dir, uniqueName(dir, tr("apps.files.link-name", "name", displayNameOf(uri)))));
    }
    submit({.kind = OpKind::Link, .sources = std::move(uris), .destination = tab().model->uri(), .targets = std::move(targets)});
  }

  void FilesWindow::showProperties(std::vector<std::string> uris) {
    if (uris.empty()) {
      uris = {tab().model->uri()};
    }
    m_properties = {.uris = uris, .counting = true};
    const auto [free, total] = filesystemSpace(uris.front());
    m_properties.free = free;
    m_properties.total = total;
    m_overlay = Overlay::Properties;
    std::weak_ptr<int> alive = m_alive;
    // Carpetas: tamaño recursivo en un hilo; se actualiza al terminar.
    std::vector<std::pair<std::string, bool>> items;
    for (const auto& uri : uris) {
      GFile* file = g_file_new_for_uri(uri.c_str());
      const bool dir = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY;
      g_object_unref(file);
      items.emplace_back(uri, dir);
    }
    std::thread([this, alive, items] {
      std::uint64_t bytes = 0;
      std::size_t files = 0;
      std::size_t folders = 0;
      for (const auto& [uri, dir] : items) {
        if (dir) {
          ++folders;
          countRecursive(uri, bytes, files, folders, alive);
        } else {
          GFile* file = g_file_new_for_uri(uri.c_str());
          GFileInfo* info = g_file_query_info(file, "standard::size", G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
          if (info != nullptr) {
            bytes += static_cast<std::uint64_t>(g_file_info_get_size(info));
            g_object_unref(info);
          }
          g_object_unref(file);
          ++files;
        }
      }
      DeferredCall::callLater([this, alive, items, bytes, files, folders] {
        if (alive.expired() || m_properties.uris.size() != items.size() || m_properties.uris.front() != items.front().first) {
          return;
        }
        m_properties.bytes = bytes;
        m_properties.files = files;
        m_properties.folders = folders;
        m_properties.counting = false;
        if (m_overlay == Overlay::Properties) {
          m_app.requestRebuild();
        }
      });
    }).detach();
    m_app.requestRebuild();
  }

  void FilesWindow::openTerminal(const std::string& uri) {
    std::string path = uriToPath(uri);
    if (path.empty() || path.find("://") != std::string::npos) {
      path = g_get_home_dir();
    }
    // kitty flotante, la misma clase que el resto del sistema (Super+T); otra terminal: $TERMINAL tal cual.
    const char* terminal = g_getenv("TERMINAL");
    std::string program = terminal != nullptr && *terminal != '\0' ? terminal : "kitty";
    std::vector<std::string> args{program};
    if (fs::path(program).filename() == "kitty") {
      args.insert(args.end(), {"--class", "kitty-floating", "--directory", path});
    }
    std::vector<gchar*> argv;
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    // La shell de la cuenta (fish, zsh…), no la del proceso que abrió okaru-files.
    gchar** envp = g_get_environ();
    if (const passwd* account = getpwuid(getuid()); account != nullptr && account->pw_shell != nullptr) {
      envp = g_environ_setenv(envp, "SHELL", account->pw_shell, TRUE);
    }
    GError* error = nullptr;
    if (g_spawn_async(path.c_str(), argv.data(), envp, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error) == FALSE) {
      setMessage(error != nullptr ? error->message : "?", true);
      g_clear_error(&error);
    }
    g_strfreev(envp);
  }

  void FilesWindow::newWindow(const std::string& uri) {
    std::string self = "/proc/self/exe";
    std::error_code ec;
    if (const auto resolved = fs::read_symlink(self, ec); !ec) {
      self = resolved.string();
    }
    (void)process::runAsync({self.c_str(), uri.c_str()});
  }

  void FilesWindow::toggleBookmark(const std::string& uri) {
    if (m_places.isBookmarked(uri)) {
      m_places.removeBookmark(uri);
      setMessage(tr("apps.files.bookmark.removed"));
    } else {
      m_places.addBookmark(uri);
      setMessage(tr("apps.files.bookmark.added"));
    }
  }

  void FilesWindow::drop(const DropRequest& request) {
    std::string target = request.targetUri.empty() ? m_dropPlace : request.targetUri;
    m_dropPlace.clear();
    for (auto& row : m_placeRows) {
      row.background->setFill(row.active ? colorSpecFromRole(ColorRole::Primary, 0.16F) : clearColorSpec());
    }
    if (target.empty() || request.uris.empty()) {
      m_app.invalidate();
      return;
    }
    if (target.starts_with("trash:")) {
      submit({.kind = OpKind::Trash, .sources = request.uris});
      return;
    }
    const bool ctrl = (request.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (request.modifiers & KeyMod::Shift) != 0;
    OpKind kind = OpKind::Copy;
    if (ctrl && shift) {
      kind = OpKind::Link;
    } else if (shift) {
      kind = OpKind::Move;
    } else if (!ctrl) {
      // Sin teclas: mover dentro del mismo disco, copiar entre discos (como todos los exploradores).
      kind = filesystemId(request.uris.front()) == filesystemId(target) ? OpKind::Move : OpKind::Copy;
    }
    submit({.kind = kind, .sources = request.uris, .destination = target});
  }

  void FilesWindow::dropExternal(const std::vector<std::string>& paths) {
    if (!canWriteHere()) {
      return;
    }
    std::vector<std::string> uris;
    for (const auto& p : paths) {
      uris.push_back(p.find("://") != std::string::npos ? p : pathToUri(p));
    }
    submit({.kind = OpKind::Copy, .sources = std::move(uris), .destination = tab().model->uri()});
  }

  ViewOptions FilesWindow::optionsFor(const std::string& uri) const {
    // De fábrica: alfabético con las carpetas primero; un cambio manual se recuerda solo en esa carpeta.
    ViewOptions options;
    if (const auto it = m_folderSorts.find(uri); it != m_folderSorts.end()) {
      options = it->second;
    }
    options.showHidden = m_options.showHidden;
    return options;
  }

  void FilesWindow::changeSort(const std::function<void(ViewOptions&)>& change) {
    const std::string uri = tab().model->uri();
    ViewOptions options = optionsFor(uri);
    change(options);
    if (options.sort == SortKey::Name && !options.descending && options.foldersFirst) {
      m_folderSorts.erase(uri); // volvió al de fábrica: no hace falta guardarlo
    } else {
      m_folderSorts[uri] = options;
      if (m_folderSorts.size() > kFolderModesLimit) {
        m_folderSorts.erase(m_folderSorts.begin());
      }
    }
    reloadAll();
  }

  void FilesWindow::setSort(SortKey key) {
    changeSort([key](ViewOptions& options) {
      if (options.sort == key) {
        options.descending = !options.descending;
      } else {
        options.sort = key;
        options.descending = key == SortKey::Modified || key == SortKey::Size; // lo más nuevo/grande arriba
      }
    });
  }

  void FilesWindow::toggleHidden() {
    m_options.showHidden = !m_options.showHidden;
    setMessage(tr(m_options.showHidden ? "apps.files.hidden.shown" : "apps.files.hidden.hidden"));
    reloadAll();
  }

  void FilesWindow::zoom(int direction) {
    if (mode() != Mode::Grid) {
      setMode(Mode::Grid);
      return;
    }
    const auto it = std::ranges::lower_bound(kIconSizes, m_iconSize);
    std::size_t index = static_cast<std::size_t>(it - kIconSizes.begin());
    if (direction > 0 && index + 1 < kIconSizes.size()) {
      ++index;
    } else if (direction < 0 && index > 0) {
      --index;
    } else if (direction == 0) {
      index = 2;
    }
    m_iconSize = kIconSizes[std::min(index, kIconSizes.size() - 1)];
    m_app.requestRebuild();
  }

  void FilesWindow::activatePlace(const Place& place, bool newTab) {
    if (place.uri == "okaru:connect") {
      connectDialog();
      return;
    }
    if (place.id.starts_with("kdeconnect:") && !place.mounted) {
      // El teléfono tiene que estar desbloqueado; KDE Connect espera hasta 30 s.
      setMessage(tr("apps.files.phone.mounting", "name", place.name));
      std::weak_ptr<int> alive = m_alive;
      std::thread([this, alive, newTab, id = place.id.substr(11), name = place.name] {
        std::string root;
        std::string error;
        const bool ok = mountPhone(id, root, error);
        DeferredCall::callLater([this, alive, newTab, ok, root, error, name] {
          if (alive.expired()) {
            return;
          }
          if (ok) {
            m_message.clear();
            updateStatus();
            newTab ? openTab(pathToUri(root)) : navigate(pathToUri(root));
          } else {
            setMessage(tr("apps.files.phone.failed", "name", name) + (error.empty() ? "" : ": " + error), true);
          }
          refreshPhones();
        });
      }).detach();
      return;
    }
    if (!place.mounted) {
      const std::string name = place.name;
      std::weak_ptr<int> alive = m_alive;
      auto* operation = static_cast<GMountOperation*>(newMountOperation());
      m_places.mount(place.id, operation, [this, alive, newTab, name](bool ok, std::string result) {
        if (alive.expired()) {
          return;
        }
        if (!ok) {
          setMessage(tr("apps.files.mount.failed", "name", name) + (result.empty() ? "" : ": " + result), true);
        } else if (!result.empty()) {
          newTab ? openTab(result) : navigate(result);
        }
      });
      g_object_unref(operation);
      return;
    }
    newTab ? openTab(place.uri) : navigate(place.uri);
  }

  void FilesWindow::unmountPlace(const Place& place, bool eject) {
    // Si la pestaña está dentro de lo que se desmonta, vuelve a la carpeta personal.
    const auto leave = [this](const std::string& prefix) {
      for (auto& t : m_tabs) {
        if (!prefix.empty() && t->model->uri().starts_with(prefix)) {
          Tab& current = tab();
          if (&current == t.get()) {
            navigate(homeUri());
          }
        }
      }
    };
    std::weak_ptr<int> alive = m_alive;
    const std::string name = place.name;
    if (place.id.starts_with("kdeconnect:")) {
      const std::string id = place.id.substr(11);
      leave(pathToUri(std::string(g_get_user_runtime_dir()) + "/" + id));
      std::thread([this, alive, id, name] {
        unmountPhone(id);
        DeferredCall::callLater([this, alive, name] {
          if (!alive.expired()) {
            setMessage(tr("apps.files.unmount.done", "name", name));
            refreshPhones();
          }
        });
      }).detach();
      return;
    }
    leave(place.uri);
    auto* operation = static_cast<GMountOperation*>(newMountOperation());
    m_places.unmount(place.id, eject, operation, [this, alive, name](bool ok, std::string error) {
      if (!alive.expired()) {
        setMessage(ok ? tr("apps.files.unmount.done", "name", name)
                      : tr("apps.files.unmount.failed", "name", name) + (error.empty() ? "" : ": " + error),
                   !ok);
      }
    });
    g_object_unref(operation);
  }

  void FilesWindow::refreshPhones() {
    if (m_phonesBusy) {
      m_phonesAgain = true;
      return;
    }
    m_phonesBusy = true;
    std::weak_ptr<int> alive = m_alive;
    std::thread([this, alive] {
      auto phones = listPhones();
      DeferredCall::callLater([this, alive, phones = std::move(phones)]() mutable {
        if (alive.expired()) {
          return;
        }
        m_phonesBusy = false;
        m_places.setPhones(std::move(phones));
        if (m_phonesAgain) {
          m_phonesAgain = false;
          refreshPhones();
        }
      });
    }).detach();
  }

  // ── Credenciales y servidores ──────────────────────────────────────────────────────────────────────

  void* FilesWindow::newMountOperation() {
    GMountOperation* op = g_mount_operation_new();
    g_signal_connect(op, "ask-password",
                     G_CALLBACK(+[](GMountOperation* o, const gchar* message, const gchar* user, const gchar* domain,
                                    GAskPasswordFlags flags, gpointer self) {
                       static_cast<FilesWindow*>(self)->onAskPassword(o, message, user, domain, flags);
                     }),
                     this);
    g_signal_connect(op, "ask-question",
                     G_CALLBACK(+[](GMountOperation* o, const gchar* message, const gchar** choices, gpointer self) {
                       static_cast<FilesWindow*>(self)->onAskQuestion(o, message, choices);
                     }),
                     this);
    g_signal_connect(op, "aborted", G_CALLBACK(+[](GMountOperation* o, gpointer self) {
                       static_cast<FilesWindow*>(self)->onMountAborted(o);
                     }),
                     this);
    return op;
  }

  void FilesWindow::onAskPassword(void* operation, const char* message, const char* user, const char* domain,
                                  unsigned flags) {
    if (m_mountOp != nullptr) {
      g_mount_operation_reply(G_MOUNT_OPERATION(m_mountOp), G_MOUNT_OPERATION_ABORTED);
      g_object_unref(m_mountOp);
    }
    m_mountOp = g_object_ref(operation);
    m_authMessage = message != nullptr ? message : "";
    m_authUser = user != nullptr && *user != '\0' ? user : g_get_user_name();
    m_authDomain = domain != nullptr ? domain : "";
    m_authPassword.clear();
    m_authFlags = flags;
    m_authAnonymous = false;
    m_menu.clear();
    m_overlay = Overlay::Password;
    m_focusDialog = true;
    m_app.requestRebuild();
  }

  void FilesWindow::onAskQuestion(void* operation, const char* message, const char* const* choices) {
    if (m_mountOp != nullptr) {
      g_mount_operation_reply(G_MOUNT_OPERATION(m_mountOp), G_MOUNT_OPERATION_ABORTED);
      g_object_unref(m_mountOp);
    }
    m_mountOp = g_object_ref(operation);
    m_authMessage = message != nullptr ? message : "";
    m_questionChoices.clear();
    for (const char* const* c = choices; c != nullptr && *c != nullptr; ++c) {
      m_questionChoices.emplace_back(*c);
    }
    m_menu.clear();
    m_overlay = Overlay::Question;
    m_app.requestRebuild();
  }

  void FilesWindow::onMountAborted(void* operation) {
    if (operation == m_mountOp) {
      g_object_unref(m_mountOp);
      m_mountOp = nullptr;
      if (m_overlay == Overlay::Password || m_overlay == Overlay::Question) {
        m_overlay = Overlay::None;
        m_app.requestRebuild();
      }
    }
  }

  void FilesWindow::replyPassword(bool accept) {
    if (m_mountOp == nullptr) {
      return;
    }
    auto* op = G_MOUNT_OPERATION(m_mountOp);
    m_mountOp = nullptr;
    if (accept) {
      if (m_authAnonymous && (m_authFlags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0) {
        g_mount_operation_set_anonymous(op, TRUE);
      } else {
        g_mount_operation_set_username(op, m_authUser.c_str());
        g_mount_operation_set_domain(op, m_authDomain.c_str());
        g_mount_operation_set_password(op, m_authPassword.c_str());
      }
      // "Recordar siempre" lo guarda gvfs en el llavero (libsecret).
      g_mount_operation_set_password_save(op, m_authSave == 2   ? G_PASSWORD_SAVE_PERMANENTLY
                                                : m_authSave == 1 ? G_PASSWORD_SAVE_FOR_SESSION
                                                                  : G_PASSWORD_SAVE_NEVER);
    }
    g_mount_operation_reply(op, accept ? G_MOUNT_OPERATION_HANDLED : G_MOUNT_OPERATION_ABORTED);
    g_object_unref(op);
    std::fill(m_authPassword.begin(), m_authPassword.end(), '\0');
    m_authPassword.clear();
    m_overlay = Overlay::None;
    m_app.requestRebuild();
  }

  void FilesWindow::replyQuestion(int choice) {
    if (m_mountOp == nullptr) {
      return;
    }
    auto* op = G_MOUNT_OPERATION(m_mountOp);
    m_mountOp = nullptr;
    if (choice >= 0) {
      g_mount_operation_set_choice(op, choice);
    }
    g_mount_operation_reply(op, choice >= 0 ? G_MOUNT_OPERATION_HANDLED : G_MOUNT_OPERATION_ABORTED);
    g_object_unref(op);
    m_overlay = Overlay::None;
    m_app.requestRebuild();
  }

  void FilesWindow::connectDialog() {
    m_connectText.clear();
    m_overlay = Overlay::Connect;
    m_focusDialog = true;
    m_app.requestRebuild();
  }

  void FilesWindow::connectToServer(std::string address) {
    address = trimmed(address);
    if (address.empty()) {
      return;
    }
    const auto scheme = address.find("://");
    if (scheme == std::string::npos) {
      setMessage(tr("apps.files.connect.no-scheme"), true);
      return;
    }
    // ¿Hay backend de gvfs para ese protocolo? Si no, decir qué paquete falta.
    const std::string protocol = address.substr(0, scheme);
    bool supported = false;
    for (const gchar* const* s = g_vfs_get_supported_uri_schemes(g_vfs_get_default()); s != nullptr && *s != nullptr; ++s) {
      supported = supported || protocol == *s;
    }
    if (!supported) {
      const std::string package = protocol == "smb" ? "gvfs-smb" : protocol == "nfs" ? "gvfs-nfs"
          : protocol == "mtp" ? "gvfs-mtp" : protocol == "afc" ? "gvfs-afc" : "gvfs";
      setMessage(tr("apps.files.connect.missing", "scheme", protocol, "package", package), true);
      return;
    }
    m_overlay = Overlay::None;
    m_app.requestRebuild();
    std::erase(m_recentServers, address);
    m_recentServers.insert(m_recentServers.begin(), address);
    if (m_recentServers.size() > 10) {
      m_recentServers.resize(10);
    }
    saveState();
    setMessage(tr("apps.files.connect.connecting", "address", address));
    GFile* file = g_file_new_for_uri(address.c_str());
    auto* operation = static_cast<GMountOperation*>(newMountOperation());
    auto* ctx = new std::pair<FilesWindow*, std::weak_ptr<int>>(this, m_alive);
    g_file_mount_enclosing_volume(
        file, G_MOUNT_MOUNT_NONE, operation, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer raw) {
          std::unique_ptr<std::pair<FilesWindow*, std::weak_ptr<int>>> c(
              static_cast<std::pair<FilesWindow*, std::weak_ptr<int>>*>(raw)
          );
          GError* error = nullptr;
          const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(source), result, &error) != FALSE;
          if (!c->second.expired()) {
            FilesWindow* window = c->first;
            if (ok || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
              char* uri = g_file_get_uri(G_FILE(source));
              window->m_message.clear();
              window->navigate(uri);
              g_free(uri);
            } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
              window->setMessage(error != nullptr ? error->message : "?", true);
            } else {
              window->m_message.clear();
              window->updateStatus();
            }
          }
          g_clear_error(&error);
        },
        ctx
    );
    g_object_unref(operation);
    g_object_unref(file);
  }

  void FilesWindow::setMessage(std::string message, bool error) {
    m_message = std::move(message);
    m_messageError = error;
    m_messageTimer.start(std::chrono::seconds(error ? 6 : 3), [this] {
      m_message.clear();
      updateStatus();
    });
    updateStatus();
  }

  // ── Menús ──────────────────────────────────────────────────────────────────────────────────────────

  void FilesWindow::showMenu(std::vector<MenuEntry> entries, float x, float y) {
    m_menu = std::move(entries);
    m_menuX = x;
    m_menuY = y;
    m_app.requestRebuild();
  }

  void FilesWindow::openContextMenu(std::optional<std::size_t> index, float x, float y) {
    std::vector<MenuEntry> menu;
    const auto sep = [&] { menu.push_back({.separator = true}); };
    const auto items = selection();
    const bool writable = canWriteHere();
    const std::string here = tab().model->uri();

    if (!index || items.empty()) {
      // Fondo de la carpeta.
      if (inTrash()) {
        menu.push_back({tr("apps.files.menu.empty-trash"), [this] { emptyTrash(); }, !tab().model->entries().empty()});
      } else {
        menu.push_back({tr("apps.files.menu.new-folder"), [this] { startNew(true); }, writable});
        menu.push_back({tr("apps.files.menu.new-file"), [this] { startNew(false); }, writable});
        menu.push_back({tr("apps.files.menu.paste"), [this, here] { paste(here); }, writable});
      }
      sep();
      menu.push_back({tr("apps.files.menu.select-all"), [this] {
                        if (m_view != nullptr) {
                          m_view->selectAll();
                        }
                      }});
      menu.push_back({.label = tr("apps.files.menu.show-hidden"), .action = [this] { toggleHidden(); },
                      .checked = m_options.showHidden, .check = true});
      const ViewOptions current = optionsFor(here);
      menu.push_back({.label = tr("apps.files.menu.folders-first"), .action = [this] {
                        changeSort([](ViewOptions& options) { options.foldersFirst = !options.foldersFirst; });
                      },
                      .checked = current.foldersFirst, .check = true});
      sep();
      for (const auto& [key, label] : std::array<std::pair<SortKey, const char*>, 4>{
               {{SortKey::Name, "apps.files.sort.name"},
                {SortKey::Size, "apps.files.sort.size"},
                {SortKey::Modified, "apps.files.sort.modified"},
                {SortKey::Type, "apps.files.sort.type"}}
           }) {
        menu.push_back({.label = tr(label), .action = [this, k = key] { setSort(k); }, .checked = current.sort == key,
                        .check = true});
      }
      sep();
      menu.push_back({tr("apps.files.menu.terminal"), [this, here] { openTerminal(here); }});
      menu.push_back({tr("apps.files.menu.bookmark-toggle"), [this, here] { toggleBookmark(here); }});
      menu.push_back({tr("apps.files.menu.properties"), [this] { showProperties({}); }});
      showMenu(std::move(menu), x, y);
      return;
    }

    const bool single = items.size() == 1;
    const bool folder = single && items.front()->directory;
    const FileEntry first = *items.front();
    if (inTrash()) {
      menu.push_back({tr("apps.files.menu.restore"), [this] { restoreSelection(); }});
      menu.push_back({tr("apps.files.menu.delete"), [this] { deleteSelection(); }});
      sep();
      menu.push_back({tr("apps.files.menu.properties"), [this] { showProperties(selectedUris()); }});
      showMenu(std::move(menu), x, y);
      return;
    }
    menu.push_back({tr("apps.files.menu.open"), [this] { openSelection(); }});
    if (folder) {
      menu.push_back({tr("apps.files.menu.open-tab"), [this, first] { activateEntry(first, true); }});
      menu.push_back({tr("apps.files.menu.open-window"), [this, first] {
                        newWindow(first.target.empty() ? first.uri : first.target);
                      }});
    } else {
      menu.push_back({tr("apps.files.menu.open-with"), [this] { openWithDialog(); }, !first.directory});
    }
    if (tab().model->searching() || here.starts_with("recent:")) {
      menu.push_back({tr("apps.files.menu.open-location"), [this, first] {
                        const std::string uri = first.target.empty() ? first.uri : first.target;
                        navigate(parentUri(uri), true, uri);
                      }});
    }
    sep();
    menu.push_back({tr("apps.files.menu.cut"), [this] { copyToClipboard(true); }, writable});
    menu.push_back({tr("apps.files.menu.copy"), [this] { copyToClipboard(false); }});
    if (folder) {
      const std::string dest = first.target.empty() ? first.uri : first.target;
      menu.push_back({tr("apps.files.menu.paste-into"), [this, dest] { paste(dest); }});
    }
    menu.push_back({tr("apps.files.menu.copy-path"), [this] { copyPaths(); }});
    menu.push_back({tr("apps.files.menu.send"), [this] { shareDialog(); }, !first.path.empty()});
    sep();
    menu.push_back({tr("apps.files.menu.rename"), [this] { startRename(); }, single && writable});
    menu.push_back({tr("apps.files.menu.link"), [this] { createLink(); }, writable});
    if (folder) {
      const std::string uri = first.uri;
      menu.push_back({tr(m_places.isBookmarked(uri) ? "apps.files.menu.bookmark-remove" : "apps.files.menu.bookmark-add"),
                      [this, uri] { toggleBookmark(uri); }});
      menu.push_back({tr("apps.files.menu.terminal"), [this, uri] { openTerminal(uri); }});
    }
    bool anyFolder = false;
    bool anyCustom = false;
    for (const FileEntry* e : items) {
      anyFolder = anyFolder || e->directory;
      anyCustom = anyCustom || !e->customIcon.empty() || !e->customIconName.empty();
    }
    if (anyFolder) {
      menu.push_back({tr("apps.files.menu.folder-color"), [this] { folderColorDialog(false); }, !first.path.empty()});
    }
    menu.push_back({tr("apps.files.menu.custom-icon"), [this] { chooseCustomIcon(); }, !first.path.empty()});
    if (anyCustom) {
      menu.push_back({tr("apps.files.menu.custom-icon-clear"), [this] { clearCustomIcon(); }});
    }
    sep();
    menu.push_back({tr("apps.files.menu.trash"), [this] { trashSelection(); }, writable || tab().model->searching()});
    menu.push_back({tr("apps.files.menu.delete"), [this] { deleteSelection(); }});
    sep();
    menu.push_back({tr("apps.files.menu.properties"), [this] { showProperties(selectedUris()); }});
    showMenu(std::move(menu), x, y);
  }

  void FilesWindow::openPlaceMenu(const Place& place, float x, float y) {
    std::vector<MenuEntry> menu;
    const Place copy = place;
    menu.push_back({tr("apps.files.menu.open"), [this, copy] { activatePlace(copy); }});
    menu.push_back({tr("apps.files.menu.open-tab"), [this, copy] { activatePlace(copy, true); }, true});
    if (place.mounted && !place.uri.empty()) {
      menu.push_back({tr("apps.files.menu.open-window"), [this, uri = place.uri] { newWindow(uri); }});
    }
    if (place.section == PlaceSection::Bookmarks) {
      menu.push_back({.separator = true});
      menu.push_back({tr("apps.files.menu.bookmark-remove"), [this, uri = place.uri] { m_places.removeBookmark(uri); }});
    }
    if (place.uri == "trash:///") {
      menu.push_back({.separator = true});
      menu.push_back({tr("apps.files.menu.empty-trash"), [this] { emptyTrash(); }});
    }
    if (place.canUnmount || place.canEject) {
      menu.push_back({.separator = true});
      const auto act = [this, copy](bool eject) { unmountPlace(copy, eject); };
      if (place.canUnmount) {
        menu.push_back({tr("apps.files.menu.unmount"), [act] { act(false); }});
      }
      if (place.canEject) {
        menu.push_back({tr("apps.files.menu.eject"), [act] { act(true); }});
      }
    }
    showMenu(std::move(menu), x, y);
  }

  void FilesWindow::openMainMenu(float x, float y) {
    std::vector<MenuEntry> menu;
    const std::string here = tab().model->uri();
    menu.push_back({tr("apps.files.menu.new-tab"), [this, here] { openTab(here); }});
    menu.push_back({tr("apps.files.menu.new-window"), [this, here] { newWindow(here); }});
    menu.push_back({tr("apps.files.menu.connect"), [this] { connectDialog(); }});
    menu.push_back({.separator = true});
    menu.push_back({tr("apps.files.menu.new-folder"), [this] { startNew(true); }, canWriteHere()});
    menu.push_back({tr("apps.files.menu.new-file"), [this] { startNew(false); }, canWriteHere()});
    menu.push_back({.separator = true});
    menu.push_back({tr("apps.files.menu.undo"), [this] { m_ops.undo(); }, m_ops.canUndo()});
    menu.push_back({tr("apps.files.menu.redo"), [this] { m_ops.redo(); }, m_ops.canRedo()});
    menu.push_back({.separator = true});
    menu.push_back({.label = tr("apps.files.menu.show-hidden"), .action = [this] { toggleHidden(); },
                    .checked = m_options.showHidden, .check = true});
    menu.push_back({.label = tr("apps.files.menu.sidebar"), .action = [this] {
                      m_sidebar = !m_sidebar;
                      m_app.requestRebuild();
                    },
                    .checked = m_sidebar, .check = true});
    menu.push_back({.label = tr("apps.files.menu.thumbnails"), .action = [this] {
                      m_thumbnails = !m_thumbnails;
                      saveState();
                      m_app.requestRebuild();
                    },
                    .checked = m_thumbnails, .check = true});
    menu.push_back({tr("apps.files.menu.folder-color-all"), [this] { folderColorDialog(true); }});
    menu.push_back({.separator = true});
    menu.push_back({tr("apps.files.menu.terminal"), [this, here] { openTerminal(here); }});
    menu.push_back({tr("apps.files.menu.bookmark-toggle"), [this, here] { toggleBookmark(here); }});
    menu.push_back({tr("apps.files.menu.properties"), [this] { showProperties({}); }});
    showMenu(std::move(menu), x, y);
  }

  // ── Escena ─────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> FilesWindow::build(float width, float height, float scale) {
    m_view = nullptr;
    m_columnViews.clear();
    m_pathInput = nullptr;
    m_findInput = nullptr;
    m_dialogInput = nullptr;
    m_statusLabel = nullptr;
    m_menuControl = nullptr;
    m_previewHost = nullptr;
    m_imagePreview = nullptr;
    m_pdfPreview = nullptr;
    m_previewZoom = nullptr;
    m_previewPage = nullptr;
    m_detailsName = nullptr;
    m_detailsInfo = nullptr;
    m_progressLabel = nullptr;
    m_progressBar = nullptr;
    m_placeRows.clear();
    m_width = width;
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      m_icons.setAccent(colorForRole(ColorRole::Primary));
      if (m_iconsDirty) {
        m_iconsDirty = false;
        m_icons.clear(*renderer);
      }
    }
    if (!m_placesReady) {
      m_placesReady = true;
      m_places.setOnChanged(nullptr);
      m_places.refresh();
      m_places.setOnChanged([this] {
        if (m_view == nullptr || !m_view->dragging()) {
          m_app.requestRebuild();
        }
      });
    }
    m_height = height;

    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    auto main = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .width = width, .height = height});
    Node* header = main->addChild(buildHeader(width, scale));
    m_app.setDragNode(header);
    if (m_tabs.size() > 1) {
      apps::TabStripProps props{
          .active = m_active,
          .closeTooltip = tr("apps.files.close-tab"),
          .onActivate = [this](std::size_t i) { activateTab(i); },
          .onClose = [this](std::size_t i) { closeTab(i); },
      };
      for (const auto& t : m_tabs) {
        props.tabs.push_back({displayNameOf(t->model->uri()), uriToPath(t->model->uri())});
      }
      main->addChild(apps::tabStrip(m_app.renderer(), width, scale, std::move(props)));
    }
    if (m_findOpen) {
      main->addChild(buildFindBar(scale));
    }
    auto body = ui::row({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    if (m_sidebar && width > 560.0F * scale) {
      body->addChild(buildSidebar(height, scale));
    }
    body->addChild(buildBody(width - (m_sidebar && width > 560.0F * scale ? kSidebarWidth * scale : 0.0F),
                             height, scale));
    main->addChild(std::move(body));
    main->addChild(buildStatus(width, scale));
    root->addChild(std::move(main));
    if (m_overlay != Overlay::None) {
      root->addChild(buildOverlay(width, height, scale));
    }
    if (!m_menu.empty()) {
      root->addChild(buildMenu(width, height, scale));
    }
    onModelChanged();
    return root;
  }

  std::unique_ptr<Node> FilesWindow::buildHeader(float width, float scale) {
    const auto ghost = [&](const char* glyph, std::string tooltip, std::function<void()> onClick, bool enabled = true,
                           bool on = false) {
      return ui::button({
          .glyph = glyph,
          .enabled = enabled,
          .selected = on,
          .variant = on ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };
    Tab& t = tab();
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .height = 52.0F * scale,
    });
    if (width > 560.0F * scale) {
      header->addChild(ghost("layout-sidebar", tr("apps.files.sidebar"), [this] {
        m_sidebar = !m_sidebar;
        m_app.requestRebuild();
      }, true, m_sidebar));
    }
    header->addChild(ghost("arrow-left", tr("apps.files.back"), [this] { goBack(); }, !t.back.empty()));
    header->addChild(ghost("arrow-right", tr("apps.files.forward"), [this] { goForward(); }, !t.forward.empty()));
    header->addChild(ghost("arrow-up", tr("apps.files.up"), [this] { goUp(); }, !parentUri(t.model->uri()).empty()));
    header->addChild(buildPathBar(scale));
    header->addChild(ghost("search", tr("apps.files.find.title"), [this] {
      m_findOpen = !m_findOpen;
      m_focusFind = m_findOpen;
      if (!m_findOpen) {
        tab().model->setFilter({});
        tab().model->setSearch({});
        m_findText.clear();
      }
      m_app.requestRebuild();
    }, true, m_findOpen));
    header->addChild(ui::segmented({
        .options = std::vector<ui::SegmentedOption>{
            {.glyph = "list", .tooltip = tr("apps.files.view.list")},
            {.glyph = "layout-grid", .tooltip = tr("apps.files.view.grid")},
            {.glyph = "layout-columns", .tooltip = tr("apps.files.view.columns")},
        },
        .selectedIndex = static_cast<std::size_t>(mode()),
        .scale = scale,
        .compact = true,
        .onChange = [this](std::size_t index) {
          DeferredCall::callLater([this, index] { setMode(static_cast<Mode>(index)); });
        },
    }));
    header->addChild(ui::button({
        .glyph = "dots-vertical",
        .variant = ButtonVariant::Ghost,
        .tooltip = tr("apps.files.menu.title"),
        .onClick = [this, scale] { openMainMenu(m_width - 250.0F * scale, 48.0F * scale); },
    }));
    return header;
  }

  std::unique_ptr<Node> FilesWindow::buildPathBar(float scale) {
    const std::string current = tab().model->uri();
    auto bar = ui::row({
        .align = FlexAlign::Center,
        .gap = 0.0F,
        .paddingH = Style::spaceXs * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .radius = Style::radiusMd * scale,
        .clipChildren = true,
        .height = Style::controlHeightSm * scale,
        .flexGrow = 1.0F,
    });
    if (m_editingPath) {
      bar->addChild(ui::input({
          .out = &m_pathInput,
          .value = m_pathText,
          .fontSize = Style::fontSizeBody * scale,
          .frameVisible = false,
          .flexGrow = 1.0F,
          .onChange = [this](const std::string& value) { m_pathText = value; },
          .onSubmit = [this](const std::string& value) {
            GFile* file = g_file_parse_name(value.c_str());
            char* uri = g_file_get_uri(file);
            const std::string target = uri != nullptr ? uri : "";
            g_free(uri);
            g_object_unref(file);
            DeferredCall::callLater([this, target] { navigate(target); });
          },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t) -> bool {
            if (sym == XKB_KEY_Escape) {
              m_editingPath = false;
              m_app.requestRebuild();
              return true;
            }
            return false;
          },
          .onFocusLoss = [this] {
            DeferredCall::callLater([this] {
              if (m_editingPath && (m_pathInput == nullptr || !m_pathInput->inputArea()->focused())) {
                m_editingPath = false;
                m_app.requestRebuild();
              }
            });
          },
      }));
      return bar;
    }

    // Migas: de la carpeta personal (o la raíz) hasta la actual; cada una lleva a su carpeta.
    std::vector<std::pair<std::string, std::string>> crumbs; // URI, nombre
    const std::string home = homeUri();
    std::string uri = current;
    while (!uri.empty() && crumbs.size() < 32) {
      if (uri == home) {
        crumbs.emplace_back(uri, tr("apps.files.places.home"));
        break;
      }
      const std::string parent = parentUri(uri);
      std::string name = parent.empty() ? (uri == "file:///" ? tr("apps.files.places.computer") : current == uri ? displayNameOf(uri) : displayNameOf(uri))
                                        : displayNameOf(uri);
      if (uri == "trash:///") {
        name = tr("apps.files.places.trash");
      } else if (uri == "recent:///") {
        name = tr("apps.files.places.recent");
      } else if (uri == "network:///") {
        name = tr("apps.files.places.network");
      }
      crumbs.emplace_back(uri, name);
      uri = parent;
    }
    std::ranges::reverse(crumbs);
    // Con muchas migas solo caben las últimas.
    const std::size_t maxCrumbs = std::max<std::size_t>(2, static_cast<std::size_t>((m_width - 520.0F * scale) / (110.0F * scale)));
    const std::size_t skip = crumbs.size() > maxCrumbs ? crumbs.size() - maxCrumbs : 0;
    if (skip > 0) {
      bar->addChild(ui::label({.text = "…", .fontSize = Style::fontSizeBody * scale,
                               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    }
    for (std::size_t i = skip; i < crumbs.size(); ++i) {
      const bool last = i + 1 == crumbs.size();
      if (i > skip) {
        bar->addChild(ui::glyph({.glyph = "chevron-right", .glyphSize = 12.0F * scale,
                                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
      }
      const std::string target = crumbs[i].first;
      bar->addChild(ui::button({
          .text = crumbs[i].second,
          .glyph = i == 0 && target == home ? std::optional<std::string>("home") : std::nullopt,
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = i == 0 && target == home ? std::optional<float>(14.0F * scale) : std::nullopt,
          .controlHeight = (Style::controlHeightSm - 6.0F) * scale,
          .variant = last ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .maxWidth = 200.0F * scale,
          .paddingH = Style::spaceSm * scale,
          .onClick = [this, target] { DeferredCall::callLater([this, target] { navigate(target); }); },
      }));
    }
    // Clic en el espacio libre: escribir la ruta (Ctrl+L).
    auto rest = ui::inputArea({
        .tooltip = tr("apps.files.path.edit"),
        .height = Style::controlHeightSm * scale,
        .flexGrow = 1.0F,
        .onClick = [this, current](const InputArea::PointerData&) {
          m_pathText = uriToPath(current);
          m_editingPath = true;
          m_app.requestRebuild();
        },
    });
    rest->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    bar->addChild(std::move(rest));
    return bar;
  }

  std::unique_ptr<Node> FilesWindow::buildFindBar(float scale) {
    auto bar = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
    });
    bar->addChild(ui::input({
        .out = &m_findInput,
        .value = m_findText,
        .placeholder = tr(m_findRecursive ? "apps.files.find.search-placeholder" : "apps.files.find.filter-placeholder"),
        .fontSize = Style::fontSizeBody * scale,
        .clearButtonEnabled = true,
        .flexGrow = 1.0F,
        .onChange = [this](const std::string& value) {
          m_findText = value;
          if (m_findRecursive) {
            m_searchTimer.start(std::chrono::milliseconds(300), [this] { tab().model->setSearch(m_findText); });
          } else {
            tab().model->setFilter(value);
          }
        },
        .onSubmit = [this](const std::string&) {
          if (m_view != nullptr) {
            m_app.focus(m_view->inputArea());
            if (tab().view.selected.empty()) {
              m_view->moveCursor(1, false);
            }
          }
        },
        .onKeyEvent = [this](std::uint32_t sym, std::uint32_t) -> bool {
          if (sym == XKB_KEY_Escape) {
            DeferredCall::callLater([this] {
              m_findOpen = false;
              m_findText.clear();
              tab().model->setFilter({});
              tab().model->setSearch({});
              m_app.requestRebuild();
            });
            return true;
          }
          if (sym == XKB_KEY_Down && m_view != nullptr) {
            m_app.focus(m_view->inputArea());
            m_view->moveCursor(1, false);
            return true;
          }
          return false;
        },
    }));
    bar->addChild(ui::button({
        .text = tr("apps.files.find.recursive"),
        .glyph = "folders",
        .fontSize = Style::fontSizeCaption * scale,
        .selected = m_findRecursive,
        .variant = m_findRecursive ? ButtonVariant::Secondary : ButtonVariant::Ghost,
        .tooltip = tr("apps.files.find.recursive-hint"),
        .onClick = [this] {
          m_findRecursive = !m_findRecursive;
          if (m_findRecursive) {
            tab().model->setFilter({});
            tab().model->setSearch(m_findText);
          } else {
            tab().model->setSearch({});
            tab().model->setFilter(m_findText);
          }
          m_focusFind = true;
          m_app.requestRebuild();
        },
    }));
    return bar;
  }

  std::unique_ptr<Node> FilesWindow::buildSidebar(float height, float scale) {
    (void)height;
    auto column = ui::column({
        .align = FlexAlign::Stretch,
        .gap = 1.0F * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
    });
    const std::string current = tab().model->uri();
    std::optional<PlaceSection> section;
    const std::array<const char*, 4> sectionKeys{
        "apps.files.section.places", "apps.files.section.bookmarks", "apps.files.section.devices",
        "apps.files.section.network"
    };
    for (const Place& place : m_places.places()) {
      if (section != place.section) {
        section = place.section;
        column->addChild(ui::label({
            .text = tr(sectionKeys[static_cast<std::size_t>(place.section)]),
            .fontSize = Style::fontSizeMini * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }));
      }
      const bool active = !place.uri.empty() && place.uri == current;
      const float rowHeight = (place.total > 0 ? 40.0F : 32.0F) * scale;
      auto row = ui::node({.height = rowHeight});
      Box* background = nullptr;
      row->addChild(ui::box({
          .out = &background,
          .fill = active ? colorSpecFromRole(ColorRole::Primary, 0.16F) : clearColorSpec(),
          .radius = Style::radiusMd * scale,
          .width = (kSidebarWidth - 2.0F * Style::spaceSm) * scale,
          .height = rowHeight,
      }));
      auto content = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingH = Style::spaceSm * scale,
          .width = (kSidebarWidth - 2.0F * Style::spaceSm) * scale,
          .height = rowHeight,
      });
      content->addChild(ui::glyph({
          .glyph = place.glyph,
          .glyphSize = 16.0F * scale,
          .color = colorSpecFromRole(active ? ColorRole::Primary : ColorRole::OnSurfaceVariant),
      }));
      auto texts = ui::column({.align = FlexAlign::Stretch, .justify = FlexJustify::Center, .gap = 3.0F * scale, .flexGrow = 1.0F});
      texts->addChild(ui::label({
          .text = place.name,
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = active ? FontWeight::SemiBold : FontWeight::Normal,
          .color = colorSpecFromRole(place.mounted ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }));
      if (place.total > 0) {
        const float used = 1.0F - static_cast<float>(static_cast<double>(place.free) / static_cast<double>(place.total));
        texts->addChild(ui::progressBar({
            .fill = colorSpecFromRole(used > 0.9F ? ColorRole::Error : ColorRole::Primary),
            .track = colorSpecFromRole(ColorRole::OnSurface, 0.1F),
            .radius = 2.0F * scale,
            .progress = used,
            .height = 3.0F * scale,
        }));
      }
      content->addChild(std::move(texts));
      if (place.canEject || place.canUnmount) {
        const bool eject = place.canEject;
        const Place target = place;
        content->addChild(ui::button({
            .glyph = "player-eject",
            .glyphSize = 14.0F * scale,
            .controlHeight = 24.0F * scale,
            .variant = ButtonVariant::Ghost,
            .tooltip = tr(eject ? "apps.files.menu.eject" : "apps.files.menu.unmount"),
            .onClick = [this, target, eject] { DeferredCall::callLater([this, target, eject] { unmountPlace(target, eject); }); },
        }));
      }
      row->addChild(std::move(content));
      const Place copy = place;
      auto hit = ui::inputArea({
          .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}),
          .tooltip = place.uri.empty() ? place.name : uriToPath(place.uri),
          .width = (kSidebarWidth - 2.0F * Style::spaceSm) * scale - (place.canEject || place.canUnmount ? 34.0F * scale : 0.0F),
          .height = rowHeight,
          .onEnter = [background, active](const InputArea::PointerData&) {
            if (!active) {
              background->setFill(colorSpecFromRole(ColorRole::OnSurface, 0.06F));
            }
          },
          .onLeave = [background, active] {
            if (!active) {
              background->setFill(clearColorSpec());
            }
          },
          .onClick = [this, copy](const InputArea::PointerData& data) {
            if (data.button == BTN_RIGHT) {
              openPlaceMenu(copy, data.sceneX, data.sceneY);
            } else {
              DeferredCall::callLater([this, copy, tabbed = data.button == BTN_MIDDLE] { activatePlace(copy, tabbed); });
            }
          },
      });
      hit->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      row->addChild(std::move(hit));
      m_placeRows.push_back({.node = row.get(), .background = background, .uri = place.uri, .active = active});
      column->addChild(std::move(row));
    }

    auto scroll = ui::scrollView({
        .state = &m_sidebarScroll,
        .contentScale = scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .width = kSidebarWidth * scale,
    });
    scroll->content()->addChild(std::move(column));
    return scroll;
  }

  std::unique_ptr<FileView> FilesWindow::makeView(float scale) {
    auto view = std::make_unique<FileView>();
    view->setScale(scale);
    view->setIconSize(m_iconSize);
    const ViewOptions sorted = tab().model->options();
    view->setSort(sorted.sort, sorted.descending);
    view->setIconProvider([this](Renderer& renderer, const FileEntry& entry, int pixels) {
      return iconFor(renderer, entry, pixels);
    });
    view->setOnZoom([this](int direction) { zoom(direction); });
    view->setOnBack([this](bool forward) { forward ? goForward() : goBack(); });
    view->setOnDragMotion([this](float sceneX, float sceneY) {
      std::string hovered;
      for (const auto& row : m_placeRows) {
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(row.node, x, y);
        if (!row.uri.empty() && sceneX >= x && sceneX < x + row.node->width() && sceneY >= y
            && sceneY < y + row.node->height()) {
          hovered = row.uri;
        }
      }
      if (hovered != m_dropPlace) {
        m_dropPlace = hovered;
        for (auto& row : m_placeRows) {
          row.background->setFill(
              row.uri == hovered && !hovered.empty() ? colorSpecFromRole(ColorRole::Primary, 0.32F)
              : row.active                            ? colorSpecFromRole(ColorRole::Primary, 0.16F)
                                                      : clearColorSpec()
          );
        }
        m_app.invalidate();
      }
    });
    view->setOnDrop([this](const DropRequest& request) { drop(request); });
    view->setDimmed(m_clipCut ? std::set<std::string>(m_clipUris.begin(), m_clipUris.end()) : std::set<std::string>{});
    return view;
  }

  std::unique_ptr<Node> FilesWindow::buildBody(float width, float height, float scale) {
    Tab& t = tab();
    auto body = ui::row({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    (void)height;

    if (mode() == Mode::Columns) {
      // Columnas: antepasados a la izquierda, la carpeta actual y una columna de detalles del archivo elegido.
      const float colW = kColumnWidth * scale;
      // Caben las columnas que dejen al menos 300 px para los detalles.
      const auto fit = static_cast<std::size_t>(std::max(1.0F, (width - 300.0F * scale) / colW));
      const std::size_t total = t.columns.size() + 1;
      const std::size_t firstShown = total > fit ? total - fit : 0;
      for (std::size_t i = firstShown; i < t.columns.size(); ++i) {
        auto view = makeView(scale);
        view->setMode(ViewMode::Compact);
        view->setActive(false);
        view->setEntries(&t.columns[i].model->entries(), &t.columns[i].state);
        // Clic en una columna anterior: carpeta → entrar; archivo → ir a su carpeta con él elegido.
        const auto open = [this, i](std::size_t index) {
          Tab& current = tab();
          if (i >= current.columns.size() || index >= current.columns[i].model->entries().size()) {
            return;
          }
          const FileEntry e = current.columns[i].model->entries()[index];
          DeferredCall::callLater([this, e] {
            if (e.directory) {
              activateEntry(e);
            } else {
              navigate(parentUri(e.uri), true, e.uri);
            }
          });
        };
        view->setOnSingleClick(open);
        view->setOnActivate(open);
        m_columnViews.emplace_back(view.get(), i);
        view->setSize(colW, 1.0F);
        body->addChild(std::move(view));
        body->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));
      }
    }

    auto view = makeView(scale);
    view->setMode(t.mode);
    view->setShowLocation(t.model->searching());
    view->setFlexGrow(1.0F);
    view->setOnActivate([this](std::size_t) { openSelection(); });
    view->setOnMiddleClick([this](std::size_t index) {
      const auto& entries = tab().model->entries();
      if (index < entries.size() && entries[index].directory) {
        activateEntry(entries[index], true);
      }
    });
    view->setOnSelectionChanged([this] { updateStatus(); });
    view->setOnContextMenu([this](std::optional<std::size_t> index, float x, float y) { openContextMenu(index, x, y); });
    view->setOnSortChanged([this](SortKey key) { setSort(key); });
    if (t.mode == ViewMode::Compact) {
      view->setOnSingleClick([this](std::size_t index) {
        const auto& entries = tab().model->entries();
        if (index < entries.size() && entries[index].directory) {
          const FileEntry e = entries[index];
          DeferredCall::callLater([this, e] { activateEntry(e); });
        }
      });
    }
    m_view = view.get();
    view->setEntries(&t.model->entries(), &t.view);
    if (t.mode == ViewMode::Compact) {
      view->setFlexGrow(0.0F);
      view->setSize(kColumnWidth * scale, 1.0F);
      body->addChild(std::move(view));
      body->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));
      body->addChild(buildDetails(scale)); // detalles del elemento elegido
    } else {
      body->addChild(std::move(view));
    }
    return body;
  }

  std::unique_ptr<Node> FilesWindow::buildDetails(float scale) {
    // Previsualización arriba (cambia en el sitio, sin reconstruir la escena) y los datos del archivo abajo.
    auto pane = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceMd * scale,
        .flexGrow = 1.0F,
    });
    pane->addChild(ui::column({.out = &m_previewHost, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale,
                               .clipChildren = true, .flexGrow = 1.0F}));
    pane->addChild(ui::label({.out = &m_detailsName, .text = " ", .fontSize = Style::fontSizeTitle * scale,
                              .fontWeight = FontWeight::SemiBold, .maxLines = 2, .textAlign = TextAlign::Center,
                              .ellipsize = TextEllipsize::Middle}));
    pane->addChild(ui::label({.out = &m_detailsInfo, .text = " ", .fontSize = Style::fontSizeCaption * scale,
                              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 6,
                              .textAlign = TextAlign::Center}));
    m_detailsScale = scale;
    fillPreviewHost();
    return pane;
  }

  void FilesWindow::updateDetails() {
    if (m_detailsName == nullptr) {
      return;
    }
    const auto items = selection();
    const bool show = items.size() == 1;
    m_detailsName->setVisible(show);
    m_detailsInfo->setVisible(show);
    // Previsualizar el elegido; con una pequeña espera para no cargar cada archivo al pasar con las flechas.
    const FileEntry* chosen = show && !items.front()->directory ? items.front() : nullptr;
    const std::string wanted = chosen != nullptr ? chosen->uri + "#" + std::to_string(chosen->modified) : std::string();
    if (wanted != m_previewWanted) {
      m_previewWanted = wanted;
      if (chosen == nullptr) {
        startPreview(nullptr);
      } else {
        const FileEntry copy = *chosen;
        m_previewTimer.start(std::chrono::milliseconds(140), [this, copy] { startPreview(&copy); });
      }
    }
    if (!show) {
      return;
    }
    const FileEntry& e = *items.front();
    m_detailsName->setText(e.display);
    std::string info = e.typeName;
    if (!e.directory) {
      info += " · " + formatSize(e.size);
    }
    if (m_preview.uri == e.uri && m_preview.originalWidth > 0) {
      info += std::format(" · {}×{}", m_preview.originalWidth, m_preview.originalHeight);
    }
    info += "\n" + tr("apps.files.prop.modified") + ": " + formatDate(e.modified);
    if (e.mode != 0) {
      info += "  ·  " + formatPermissions(e.mode);
    }
    m_detailsInfo->setText(info);
    // Carpeta elegida: su icono grande en el lugar de la previsualización.
    if (m_preview.uri.empty()) {
      fillPreviewHost();
    }
  }

  void FilesWindow::startPreview(const FileEntry* entry) {
    // Lo anterior se suelta en el próximo cuadro (con el contexto GL activo).
    if (m_preview.texture.valid()) {
      m_retiredTextures.push_back(m_preview.texture);
    }
    if (m_preview.pdf != nullptr) {
      m_retiredPdfs.push_back(std::move(m_preview.pdf));
    }
    const std::uint64_t generation = m_preview.generation + 1;
    m_preview = {};
    m_preview.generation = generation;
    if (entry != nullptr && !entry->path.empty()) {
      m_preview.uri = entry->uri;
      m_preview.modified = entry->modified;
      m_preview.kind = previewKindFor(entry->contentType);
    }
    std::weak_ptr<int> alive = m_alive;
    switch (m_preview.kind) {
    case PreviewKind::Image:
      m_preview.loading = true;
      std::thread([this, alive, generation, path = entry->path] {
        auto decoded = decodeImage(path, 3072);
        DeferredCall::callLater([this, alive, generation, decoded = std::move(decoded)]() mutable {
          if (alive.expired() || generation != m_preview.generation) {
            return;
          }
          m_preview.loading = false;
          m_preview.originalWidth = decoded.originalWidth;
          m_preview.originalHeight = decoded.originalHeight;
          if (decoded.rgba.empty()) {
            m_preview.error = tr("apps.files.preview.failed");
          }
          m_preview.pending = std::move(decoded);
          fillPreviewHost();
          updateDetails();
          m_app.requestFrame();
        });
      }).detach();
      break;
    case PreviewKind::Pdf:
      openPreviewPdf(entry->path);
      break;
    case PreviewKind::Office: {
      const std::string cached = officePreviewPath(entry->uri, entry->modified);
      if (g_file_test(cached.c_str(), G_FILE_TEST_EXISTS)) {
        openPreviewPdf(cached);
        break;
      }
      m_preview.loading = true;
      std::thread([this, alive, generation, path = entry->path, cached] {
        std::string error;
        const bool ok = convertOfficeToPdf(path, cached, error);
        DeferredCall::callLater([this, alive, generation, ok, cached, error] {
          if (alive.expired() || generation != m_preview.generation) {
            return;
          }
          m_preview.loading = false;
          if (ok) {
            openPreviewPdf(cached);
          } else {
            m_preview.error = tr("apps.files.preview.office-failed");
          }
          fillPreviewHost();
        });
      }).detach();
      break;
    }
    case PreviewKind::None:
      break;
    }
    fillPreviewHost();
  }

  void FilesWindow::openPreviewPdf(const std::string& path) {
    auto opened = pdf::Document::open(path);
    if (!opened) {
      m_preview.error = tr(opened.error() == pdf::OpenError::Damaged ? "apps.files.preview.failed"
                                                                      : "apps.files.preview.locked");
      return;
    }
    auto tab = std::make_unique<pdf::PdfTab>();
    tab->path = path;
    tab->doc = std::move(*opened);
    const int pages = tab->doc->pageCount();
    for (int i = 0; i < pages; ++i) {
      tab->sizes.push_back(tab->doc->pageSize(i));
    }
    tab->pages.resize(static_cast<std::size_t>(pages));
    tab->thumbs.resize(static_cast<std::size_t>(pages));
    tab->fit = pdf::Fit::Width;
    pdf::PdfTab* raw = tab.get();
    tab->worker = std::make_unique<pdf::RenderWorker>(path, std::string(), [this, raw](const pdf::RenderJob& job, pdf::RenderedPage page) {
      if (m_preview.pdf.get() != raw) {
        return; // previsualización ya cambiada
      }
      raw->arrived.emplace_back(job, std::move(page));
      if (m_pdfPreview != nullptr) {
        m_pdfPreview->markLayoutDirty();
      }
      m_app.invalidate();
    });
    m_preview.pdf = std::move(tab);
  }

  void FilesWindow::fillPreviewHost() {
    if (m_previewHost == nullptr) {
      return;
    }
    while (!m_previewHost->children().empty()) {
      (void)m_previewHost->removeChild(m_previewHost->children().back().get());
    }
    m_imagePreview = nullptr;
    m_pdfPreview = nullptr;
    m_previewZoom = nullptr;
    m_previewPage = nullptr;
    const float scale = m_detailsScale;
    const auto message = [&](std::string text, bool spinner) {
      auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * scale,
                             .flexGrow = 1.0F});
      if (spinner) {
        box->addChild(ui::glyph({.glyph = "loader-2", .glyphSize = 28.0F * scale,
                                 .color = colorSpecFromRole(ColorRole::Primary)}));
      }
      box->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale,
                               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 4,
                               .textAlign = TextAlign::Center}));
      m_previewHost->addChild(std::move(box));
    };
    const auto ghost = [&](const char* glyph, std::string tooltip, std::function<void()> onClick) {
      return ui::button({.glyph = glyph, .glyphSize = 16.0F * scale, .controlHeight = 30.0F * scale,
                         .variant = ButtonVariant::Ghost, .tooltip = std::move(tooltip), .onClick = std::move(onClick)});
    };
    const auto toolbar = [&](bool pages) {
      auto bar = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale,
                          .paddingH = Style::spaceSm * scale, .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
                          .radius = Style::radiusLg * scale, .height = 38.0F * scale});
      if (pages) {
        bar->addChild(ghost("chevron-up", tr("apps.files.preview.previous"), [this] {
          if (m_pdfPreview != nullptr && m_preview.pdf != nullptr) {
            m_pdfPreview->goToPage(m_preview.pdf->currentPage - 1);
            m_app.invalidate();
          }
        }));
        bar->addChild(ui::label({.out = &m_previewPage, .text = " ", .fontSize = Style::fontSizeCaption * scale,
                                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .minWidth = 64.0F * scale,
                                 .textAlign = TextAlign::Center}));
        bar->addChild(ghost("chevron-down", tr("apps.files.preview.next"), [this] {
          if (m_pdfPreview != nullptr && m_preview.pdf != nullptr) {
            m_pdfPreview->goToPage(m_preview.pdf->currentPage + 1);
            m_app.invalidate();
          }
        }));
        bar->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));
      }
      const auto zoom = [this](float factor) {
        if (m_imagePreview != nullptr) {
          m_imagePreview->zoomBy(factor);
        } else if (m_pdfPreview != nullptr) {
          m_pdfPreview->setZoom(std::clamp(m_pdfPreview->effectiveZoom() * factor, 0.1, 8.0));
        }
        updatePreviewLabels();
        m_app.invalidate();
      };
      bar->addChild(ghost("zoom-out", tr("apps.files.preview.zoom-out"), [zoom] { zoom(1.0F / 1.25F); }));
      bar->addChild(ui::label({.out = &m_previewZoom, .text = " ", .fontSize = Style::fontSizeCaption * scale,
                               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .minWidth = 48.0F * scale,
                               .textAlign = TextAlign::Center}));
      bar->addChild(ghost("zoom-in", tr("apps.files.preview.zoom-in"), [zoom] { zoom(1.25F); }));
      bar->addChild(ghost("arrows-maximize", tr("apps.files.preview.fit"), [this] {
        if (m_imagePreview != nullptr) {
          m_imagePreview->fit();
        } else if (m_pdfPreview != nullptr) {
          m_pdfPreview->setFit(pdf::Fit::Width);
        }
        updatePreviewLabels();
        m_app.invalidate();
      }));
      bar->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));
      bar->addChild(ui::button({
          .text = tr("apps.files.preview.open"),
          .glyph = "external-link",
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = 14.0F * scale,
          .controlHeight = 30.0F * scale,
          .variant = ButtonVariant::Ghost,
          .onClick = [this] { openSelection(); },
      }));
      m_previewHost->addChild(std::move(bar));
    };

    if (m_preview.uri.empty()) {
      // Sin archivo (carpeta o nada): el icono grande del elegido.
      const auto items = selection();
      if (items.size() == 1) {
        auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F});
        Image* icon = nullptr;
        box->addChild(ui::image({.out = &icon, .fit = ImageFit::Contain, .width = 128.0F * scale, .height = 128.0F * scale}));
        if (Renderer* renderer = m_app.renderer(); renderer != nullptr && icon != nullptr) {
          if (const TextureHandle t = iconFor(*renderer, *items.front(), static_cast<int>(128 * scale)); t.valid()) {
            icon->setExternalTexture(*renderer, t);
          }
        }
        m_previewHost->addChild(std::move(box));
      }
    } else if (!m_preview.error.empty()) {
      message(m_preview.error, false);
    } else if (m_preview.loading) {
      message(tr(m_preview.kind == PreviewKind::Office ? "apps.files.preview.converting" : "apps.files.loading"), true);
    } else if (m_preview.kind == PreviewKind::Image) {
      auto view = std::make_unique<ImagePreview>();
      view->setFlexGrow(1.0F);
      view->setOnZoomChanged([this] { updatePreviewLabels(); });
      if (m_preview.texture.valid()) {
        view->setTexture(m_preview.texture, m_preview.width, m_preview.height);
      }
      m_imagePreview = view.get();
      m_previewHost->addChild(std::move(view));
      toolbar(false);
    } else if (m_preview.pdf != nullptr) {
      auto view = std::make_unique<pdf::PdfView>();
      view->setFlexGrow(1.0F);
      view->setBufferScale(m_app.bufferScale());
      view->setTab(m_preview.pdf.get());
      view->setOnPageChanged([this] { updatePreviewLabels(); });
      view->setOnZoomChanged([this] { updatePreviewLabels(); });
      view->setOnOpenUri([](const std::string& uri) {
        if (uri.starts_with("http://") || uri.starts_with("https://") || uri.starts_with("mailto:")) {
          (void)process::runAsync({"xdg-open", uri.c_str()});
        }
      });
      m_pdfPreview = view.get();
      m_previewHost->addChild(std::move(view));
      toolbar(true);
    } else {
      // Tipo sin previsualización: el icono (o la miniatura) en grande.
      const auto items = selection();
      if (items.size() == 1) {
        auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F});
        Image* icon = nullptr;
        box->addChild(ui::image({.out = &icon, .fit = ImageFit::Contain, .width = 192.0F * scale, .height = 192.0F * scale}));
        if (Renderer* renderer = m_app.renderer(); renderer != nullptr && icon != nullptr) {
          if (const TextureHandle t = iconFor(*renderer, *items.front(), static_cast<int>(256 * scale)); t.valid()) {
            icon->setExternalTexture(*renderer, t);
          }
        }
        m_previewHost->addChild(std::move(box));
      }
    }
    m_previewHost->markLayoutDirty();
    updatePreviewLabels();
    m_app.invalidate();
  }

  void FilesWindow::updatePreviewLabels() {
    if (m_previewZoom != nullptr) {
      const float percent = m_imagePreview != nullptr ? m_imagePreview->zoomPercent()
          : m_pdfPreview != nullptr                   ? static_cast<float>(m_pdfPreview->effectiveZoom() * 100.0)
                                                      : 0.0F;
      m_previewZoom->setText(std::format("{}%", static_cast<int>(std::lround(percent))));
    }
    if (m_previewPage != nullptr && m_preview.pdf != nullptr) {
      m_previewPage->setText(std::format("{} / {}", m_preview.pdf->currentPage + 1, m_preview.pdf->sizes.size()));
    }
  }

  std::unique_ptr<Node> FilesWindow::buildStatus(float width, float scale) {
    (void)width;
    auto bar = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .height = 30.0F * scale,
    });
    bar->addChild(ui::label({
        .out = &m_statusLabel,
        .text = " ",
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = 1,
        .flexGrow = 1.0F,
    }));
    bar->addChild(ui::label({
        .out = &m_progressLabel,
        .text = " ",
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = 360.0F * scale,
        .maxLines = 1,
        .ellipsize = TextEllipsize::Middle,
    }));
    bar->addChild(ui::progressBar({
        .out = &m_progressBar,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .track = colorSpecFromRole(ColorRole::OnSurface, 0.1F),
        .radius = 2.0F * scale,
        .width = 140.0F * scale,
        .height = 4.0F * scale,
        .visible = false,
    }));
    if (m_ops.busy() || m_progress) {
      bar->addChild(ui::button({
          .glyph = "x",
          .glyphSize = 12.0F * scale,
          .controlHeight = 22.0F * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.files.op.cancel"),
          .onClick = [this] { m_ops.cancelCurrent(); },
      }));
    }
    updateStatus();
    return bar;
  }

  void FilesWindow::updateStatus() {
    if (m_statusLabel == nullptr || m_tabs.empty()) {
      return;
    }
    const Tab& t = tab();
    const auto items = selection();
    std::string text;
    if (items.empty()) {
      text = trp("apps.files.status.items", static_cast<long>(t.model->entries().size()));
    } else {
      std::uint64_t bytes = 0;
      bool hasFiles = false;
      for (const FileEntry* e : items) {
        if (!e->directory) {
          bytes += e->size;
          hasFiles = true;
        }
      }
      text = trp("apps.files.status.selected", static_cast<long>(items.size()));
      if (hasFiles) {
        text += " (" + formatSize(bytes) + ")";
      }
    }
    if (t.model->loading()) {
      text = tr("apps.files.loading");
    }
    m_statusLabel->setText(text);
    updateDetails();

    std::string right;
    bool error = false;
    float progress = -1.0F;
    if (m_progress) {
      static constexpr std::array<const char*, 9> kOps{
          "apps.files.op.copy", "apps.files.op.move", "apps.files.op.link", "apps.files.op.trash", "apps.files.op.delete",
          "apps.files.op.restore", "apps.files.op.rename", "apps.files.op.new", "apps.files.op.new"
      };
      right = tr(kOps[static_cast<std::size_t>(m_progress->kind)], "name", m_progress->current);
      if (m_progress->bytesTotal > 0) {
        right += "  " + formatSize(m_progress->bytesDone) + " / " + formatSize(m_progress->bytesTotal);
        progress = static_cast<float>(static_cast<double>(m_progress->bytesDone) / static_cast<double>(m_progress->bytesTotal));
      } else if (m_progress->itemsTotal > 0) {
        right += std::format("  {} / {}", m_progress->itemsDone, m_progress->itemsTotal);
        progress = static_cast<float>(m_progress->itemsDone) / static_cast<float>(m_progress->itemsTotal);
      }
      if (m_progress->queued > 0) {
        right += "  " + trp("apps.files.op.queued", static_cast<long>(m_progress->queued));
      }
    } else if (!m_message.empty()) {
      right = m_message;
      error = m_messageError;
    }
    if (m_progressLabel != nullptr) {
      m_progressLabel->setText(right.empty() ? " " : right);
      m_progressLabel->setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    }
    if (m_progressBar != nullptr) {
      m_progressBar->setVisible(progress >= 0.0F);
      m_progressBar->setProgress(std::clamp(progress, 0.0F, 1.0F));
    }
    m_app.invalidate();
  }

  std::unique_ptr<Node> FilesWindow::buildMenu(float width, float height, float scale) {
    auto layer = ui::node({.width = width, .height = height, .zIndex = 20});
    layer->addChild(ui::inputArea({
        .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}),
        .width = width,
        .height = height,
        .onPress = [this](const InputArea::PointerData&) {
          DeferredCall::callLater([this] {
            m_menu.clear();
            m_app.requestRebuild();
          });
        },
    }));
    std::vector<ContextMenuControlEntry> entries;
    for (std::size_t i = 0; i < m_menu.size(); ++i) {
      const MenuEntry& e = m_menu[i];
      entries.push_back({
          .id = static_cast<std::int32_t>(i),
          .label = e.label,
          .enabled = e.enabled,
          .separator = e.separator,
          .checkmark = e.check,
          .toggleState = e.check ? (e.checked ? 1 : 0) : -1,
      });
    }
    Renderer* renderer = m_app.renderer();
    const float menuWidth = std::clamp(
        renderer != nullptr ? ContextMenuControl::preferredWidth(*renderer, entries, scale, true) : 240.0F * scale,
        200.0F * scale, 360.0F * scale
    );
    const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size(), scale);
    const float x = std::clamp(m_menuX, 4.0F, std::max(4.0F, width - menuWidth - 4.0F));
    const float y = std::clamp(m_menuY, 4.0F, std::max(4.0F, height - menuHeight - 4.0F));
    layer->addChild(ui::box({
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .border = colorSpecFromRole(ColorRole::OutlineVariant),
        .borderWidth = 1.0F,
        .radius = Style::radiusLg * scale,
        .width = menuWidth,
        .height = menuHeight,
        .configure = [x, y](Box& box) { box.setPosition(x, y); },
    }));
    auto menu = std::make_unique<ContextMenuControl>();
    menu->setContentScale(scale);
    menu->setMenuWidth(menuWidth);
    menu->setReserveToggleSpace(true);
    menu->setMaxVisible(entries.size());
    menu->setEntries(std::move(entries));
    menu->setRedrawCallback([this] { m_app.invalidate(); });
    menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
      const auto index = static_cast<std::size_t>(entry.id);
      DeferredCall::callLater([this, index] {
        if (index >= m_menu.size()) {
          return;
        }
        auto action = m_menu[index].action;
        m_menu.clear();
        m_app.requestRebuild();
        if (action) {
          action();
        }
      });
    });
    menu->setPosition(x, y);
    menu->setSize(menuWidth, menuHeight);
    m_menuControl = menu.get();
    layer->addChild(std::move(menu));
    return layer;
  }

  std::unique_ptr<Node> FilesWindow::buildOverlay(float width, float height, float scale) {
    auto overlay = ui::node({.width = width, .height = height, .zIndex = 10});
    overlay->addChild(ui::inputArea({.width = width, .height = height}));
    overlay->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Scrim, 0.45F), .width = width, .height = height}));
    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .radius = Style::radiusLg * scale,
        .width = std::min(480.0F * scale, width - 32.0F),
    });
    const auto heading = [&](std::string text) {
      card->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeTitle * scale,
                                .fontWeight = FontWeight::SemiBold, .maxLines = 2, .ellipsize = TextEllipsize::Middle}));
    };
    const auto body = [&](std::string text, ColorRole role = ColorRole::OnSurfaceVariant) {
      card->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale,
                                .color = colorSpecFromRole(role), .maxLines = 8}));
    };
    const auto close = [this] {
      DeferredCall::callLater([this] {
        m_overlay = Overlay::None;
        m_app.requestRebuild();
      });
    };
    const auto buttons = [&](auto... children) {
      card->addChild(ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
                             std::move(children)...));
    };
    const auto cancelButton = [&] {
      return ui::button({.text = tr("apps.files.cancel"), .variant = ButtonVariant::Ghost, .onClick = close});
    };
    const auto info = [&](std::string name, std::string value) {
      if (value.empty()) {
        return;
      }
      card->addChild(ui::row(
          {.align = FlexAlign::Start, .gap = Style::spaceMd * scale},
          ui::label({.text = std::move(name), .fontSize = Style::fontSizeCaption * scale,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .width = 120.0F * scale}),
          ui::label({.text = std::move(value), .fontSize = Style::fontSizeCaption * scale, .maxLines = 4, .flexGrow = 1.0F})
      ));
    };

    switch (m_overlay) {
    case Overlay::Rename:
    case Overlay::NewFolder:
    case Overlay::NewFile: {
      heading(tr(m_overlay == Overlay::Rename ? "apps.files.rename.title"
                 : m_overlay == Overlay::NewFolder ? "apps.files.new.folder-title"
                                                   : "apps.files.new.file-title"));
      const auto submit = [this] {
        std::string name = trimmed(m_dialogText);
        if (name.empty() || name.contains('/') || name == "." || name == "..") {
          return;
        }
        const Overlay kind = m_overlay;
        m_overlay = Overlay::None;
        if (kind == Overlay::Rename) {
          if (name != displayNameOf(m_dialogTarget)) {
            this->submit({.kind = OpKind::Rename, .sources = {m_dialogTarget}, .destination = name});
          }
        } else {
          this->submit({.kind = kind == Overlay::NewFolder ? OpKind::NewFolder : OpKind::NewFile,
                        .destination = m_dialogTarget, .name = name});
        }
        m_app.requestRebuild();
      };
      const bool invalid = m_dialogText.contains('/');
      card->addChild(ui::input({
          .out = &m_dialogInput,
          .value = m_dialogText,
          .fontSize = Style::fontSizeBody * scale,
          .invalid = invalid,
          .onChange = [this](const std::string& value) { m_dialogText = value; },
          .onSubmit = [submit](const std::string&) { DeferredCall::callLater(submit); },
          .onKeyEvent = [close](std::uint32_t sym, std::uint32_t) -> bool {
            if (sym == XKB_KEY_Escape) {
              close();
              return true;
            }
            return false;
          },
      }));
      buttons(cancelButton(), ui::button({
          .text = tr(m_overlay == Overlay::Rename ? "apps.files.rename.action" : "apps.files.new.action"),
          .variant = ButtonVariant::Primary,
          .onClick = [submit] { DeferredCall::callLater(submit); },
      }));
      break;
    }
    case Overlay::Conflict: {
      const Conflict& c = *m_conflict;
      const bool merge = c.sourceIsDir && c.destinationIsDir;
      heading(tr(merge ? "apps.files.conflict.folder-title" : "apps.files.conflict.title", "name", displayNameOf(c.destination)));
      body(tr("apps.files.conflict.body", "folder", displayNameOf(parentUri(c.destination))));
      info(tr("apps.files.conflict.existing"),
           (c.destinationIsDir ? std::string() : formatSize(c.destinationSize) + " · ") + formatDate(c.destinationModified));
      info(tr("apps.files.conflict.incoming"),
           (c.sourceIsDir ? std::string() : formatSize(c.sourceSize) + " · ") + formatDate(c.sourceModified));
      card->addChild(ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::checkbox({.checked = m_conflictAll, .scale = scale,
                        .onChange = [this](bool value) { m_conflictAll = value; }}),
          ui::label({.text = tr("apps.files.conflict.all"), .fontSize = Style::fontSizeBody * scale})
      ));
      const auto answer = [this](ConflictChoice choice) {
        return [this, choice] {
          DeferredCall::callLater([this, choice] {
            if (m_conflictReply) {
              m_conflictReply({choice, m_conflictAll});
              m_conflictReply = nullptr;
            }
            m_overlay = Overlay::None;
            m_app.requestRebuild();
          });
        };
      };
      buttons(
          ui::button({.text = tr("apps.files.cancel"), .variant = ButtonVariant::Ghost, .onClick = answer(ConflictChoice::Cancel)}),
          ui::button({.text = tr("apps.files.conflict.skip"), .variant = ButtonVariant::Ghost, .onClick = answer(ConflictChoice::Skip)}),
          ui::button({.text = tr("apps.files.conflict.keep-both"), .variant = ButtonVariant::Secondary,
                      .onClick = answer(ConflictChoice::KeepBoth)}),
          ui::button({.text = tr(merge ? "apps.files.conflict.merge" : "apps.files.conflict.replace"),
                      .variant = ButtonVariant::Primary, .onClick = answer(ConflictChoice::Replace)})
      );
      break;
    }
    case Overlay::ConfirmDelete: {
      heading(trp("apps.files.delete.title", static_cast<long>(m_dialogUris.size()),
                  "name", m_dialogUris.empty() ? std::string() : displayNameOf(m_dialogUris.front())));
      body(m_dialogText.empty() ? tr("apps.files.delete.body") : m_dialogText);
      buttons(cancelButton(), ui::button({
          .text = tr("apps.files.delete.action"),
          .variant = ButtonVariant::Destructive,
          .onClick = [this] {
            DeferredCall::callLater([this] {
              m_overlay = Overlay::None;
              submit({.kind = OpKind::Delete, .sources = m_dialogUris});
              m_app.requestRebuild();
            });
          },
      }));
      break;
    }
    case Overlay::ConfirmEmptyTrash: {
      heading(tr("apps.files.empty-trash.title"));
      body(tr("apps.files.empty-trash.body"));
      buttons(cancelButton(), ui::button({
          .text = tr("apps.files.empty-trash.action"),
          .variant = ButtonVariant::Destructive,
          .onClick = [this] {
            DeferredCall::callLater([this] {
              m_overlay = Overlay::None;
              std::vector<FileEntry> entries;
              std::string error;
              (void)listDirectory("trash:///", entries, error);
              std::vector<std::string> uris;
              for (const auto& e : entries) {
                uris.push_back(e.uri);
              }
              if (!uris.empty()) {
                submit({.kind = OpKind::Delete, .sources = std::move(uris)});
              }
              m_app.requestRebuild();
            });
          },
      }));
      break;
    }
    case Overlay::Properties: {
      const Properties& p = m_properties;
      const bool single = p.uris.size() == 1;
      GFile* file = g_file_new_for_uri(p.uris.front().c_str());
      GFileInfo* gi = g_file_query_info(file, kQueryAttributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
      GFile* parent = g_file_get_parent(file);
      FileEntry e = gi != nullptr && parent != nullptr ? entryFromInfo(parent, gi) : FileEntry{};
      if (gi != nullptr) {
        g_object_unref(gi);
      }
      if (parent != nullptr) {
        g_object_unref(parent);
      }
      g_object_unref(file);
      if (single) {
        auto top = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
        Image* icon = nullptr;
        top->addChild(ui::image({.out = &icon, .fit = ImageFit::Contain, .width = 48.0F * scale, .height = 48.0F * scale}));
        if (Renderer* renderer = m_app.renderer(); renderer != nullptr && icon != nullptr) {
          if (const TextureHandle t = iconFor(*renderer, e, static_cast<int>(64 * scale)); t.valid()) {
            icon->setExternalTexture(*renderer, t);
          }
        }
        top->addChild(ui::label({.text = e.display.empty() ? displayNameOf(p.uris.front()) : e.display,
                                 .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::SemiBold,
                                 .maxLines = 3, .flexGrow = 1.0F}));
        card->addChild(std::move(top));
        info(tr("apps.files.prop.type"), e.typeName);
        info(tr("apps.files.prop.location"), uriToPath(parentUri(p.uris.front())));
        if (!e.trashOrigin.empty()) {
          info(tr("apps.files.prop.origin"), e.trashOrigin);
        }
        if (!e.target.empty()) {
          info(tr("apps.files.prop.target"), uriToPath(e.target));
        }
      } else {
        heading(trp("apps.files.prop.many", static_cast<long>(p.uris.size())));
      }
      std::string size = p.counting ? tr("apps.files.prop.counting") : formatSize(p.bytes);
      if (!p.counting && (p.folders > 0 || !single)) {
        size += " — " + trp("apps.files.prop.contents", static_cast<long>(p.files), "folders", p.folders);
      }
      info(tr("apps.files.prop.size"), size);
      if (single) {
        info(tr("apps.files.prop.modified"), formatDate(e.modified));
        if (e.mode != 0) {
          info(tr("apps.files.prop.permissions"), formatPermissions(e.mode) + (e.owner.empty() ? "" : "  · " + e.owner));
        }
      }
      if (p.total > 0) {
        info(tr("apps.files.prop.free"), tr("apps.files.prop.free-value", "free", formatSize(p.free), "total", formatSize(p.total)));
      }
      buttons(ui::button({.text = tr("apps.files.close"), .variant = ButtonVariant::Primary, .onClick = close}));
      break;
    }
    case Overlay::OpenWith: {
      heading(tr("apps.files.open-with.title"));
      auto list = ui::column({.align = FlexAlign::Stretch, .gap = 2.0F * scale});
      Renderer* renderer = m_app.renderer();
      for (const auto& app : m_openWith) {
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingH = Style::spaceSm * scale,
                            .height = 36.0F * scale});
        Image* icon = nullptr;
        row->addChild(ui::image({.out = &icon, .fit = ImageFit::Contain, .width = 24.0F * scale, .height = 24.0F * scale}));
        if (renderer != nullptr && icon != nullptr && !app.icon.empty()) {
          if (const TextureHandle t = m_icons.named(*renderer, {app.icon, "application-x-executable"}, static_cast<int>(32 * scale));
              t.valid()) {
            icon->setExternalTexture(*renderer, t);
          }
        }
        row->addChild(ui::label({.text = app.name, .fontSize = Style::fontSizeBody * scale, .maxLines = 1, .flexGrow = 1.0F}));
        auto hit = ui::inputArea({
            .height = 36.0F * scale,
            .flexGrow = 1.0F,
            .onClick = [this, id = app.id](const InputArea::PointerData&) {
              DeferredCall::callLater([this, id] { launchWith(id, m_openWithDefault); });
            },
        });
        hit->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
        auto stack = ui::node({.height = 36.0F * scale});
        stack->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::OnSurface, 0.04F), .radius = Style::radiusMd * scale,
                                 .width = std::min(480.0F * scale, width - 32.0F) - 2.0F * Style::spaceLg * scale,
                                 .height = 36.0F * scale}));
        row->setSize(std::min(480.0F * scale, width - 32.0F) - 2.0F * Style::spaceLg * scale, 36.0F * scale);
        stack->addChild(std::move(row));
        hit->setSize(std::min(480.0F * scale, width - 32.0F) - 2.0F * Style::spaceLg * scale, 36.0F * scale);
        stack->addChild(std::move(hit));
        list->addChild(std::move(stack));
      }
      auto scroll = ui::scrollView({.contentScale = scale, .height = std::min(360.0F * scale, height - 220.0F * scale)});
      scroll->content()->addChild(std::move(list));
      card->addChild(std::move(scroll));
      card->addChild(ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::checkbox({.checked = m_openWithDefault, .scale = scale,
                        .onChange = [this](bool value) { m_openWithDefault = value; }}),
          ui::label({.text = tr("apps.files.open-with.default"), .fontSize = Style::fontSizeBody * scale})
      ));
      buttons(cancelButton());
      break;
    }
    case Overlay::FolderColor: {
      heading(m_colorTargets.empty() ? tr("apps.files.color.title-all")
                                     : trp("apps.files.color.title", static_cast<long>(m_colorTargets.size()), "name",
                                           displayNameOf(m_colorTargets.front())));
      if (m_colorTargets.empty()) {
        body(tr("apps.files.color.body-all"));
      }
      // Muestras: tema (sin cambio), acento de OKARU y los colores de Papirus.
      const std::string current = m_colorTargets.empty() ? m_icons.defaultFolderColor() : std::string("-");
      struct Swatch {
        std::string id;
        std::string label;
        ColorSpec fill;
        const char* glyph;
      };
      std::vector<Swatch> swatches{
          {"", tr("apps.files.color.theme"), colorSpecFromRole(ColorRole::SurfaceContainerHighest), "x"},
          {"accent", tr("apps.files.color.accent"), colorSpecFromRole(ColorRole::Primary), "palette"},
      };
      for (const auto& option : folderColorOptions()) {
        swatches.push_back({option.id, tr(option.label), fixedColorSpec(option.color), nullptr});
      }
      const float dot = 36.0F * scale;
      auto grid = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
      for (const auto& swatch : swatches) {
        auto cell = ui::node({.width = dot, .height = dot});
        Box* ring = nullptr;
        cell->addChild(ui::box({.out = &ring, .fill = swatch.fill,
                                .border = colorSpecFromRole(swatch.id == current ? ColorRole::OnSurface : ColorRole::OutlineVariant),
                                .borderWidth = swatch.id == current ? 3.0F * scale : 1.0F, .radius = dot * 0.5F,
                                .width = dot, .height = dot}));
        if (swatch.glyph != nullptr) {
          auto icon = ui::glyph({.glyph = swatch.glyph, .glyphSize = 16.0F * scale,
                                 .color = colorSpecFromRole(swatch.id == "accent" ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant)});
          icon->setPosition(dot * 0.5F - 8.0F * scale, dot * 0.5F - 8.0F * scale);
          cell->addChild(std::move(icon));
        }
        cell->addChild(ui::inputArea({
            .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
            .tooltip = swatch.label,
            .width = dot,
            .height = dot,
            .onClick = [this, id = swatch.id](const InputArea::PointerData&) {
              DeferredCall::callLater([this, id] { setFolderColor(id); });
            },
        }));
        grid->addChild(std::move(cell));
      }
      card->addChild(std::move(grid));
      buttons(cancelButton());
      break;
    }
    case Overlay::Password: {
      // Primera línea del mensaje de gvfs como título; el resto (equipo, recurso) debajo.
      const auto newline = m_authMessage.find('\n');
      heading(newline == std::string::npos ? m_authMessage : m_authMessage.substr(0, newline));
      if (newline != std::string::npos) {
        body(m_authMessage.substr(newline + 1));
      }
      const auto submit = [this] { DeferredCall::callLater([this] { replyPassword(true); }); };
      const auto escape = [this](std::uint32_t sym, std::uint32_t) -> bool {
        if (sym == XKB_KEY_Escape) {
          DeferredCall::callLater([this] { replyPassword(false); });
          return true;
        }
        return false;
      };
      const bool anonymous = m_authAnonymous && (m_authFlags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0;
      if ((m_authFlags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0) {
        card->addChild(ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::checkbox({.checked = m_authAnonymous, .scale = scale, .onChange = [this](bool value) {
                            m_authAnonymous = value;
                            m_app.requestRebuild();
                          }}),
            ui::label({.text = tr("apps.files.auth.anonymous"), .fontSize = Style::fontSizeBody * scale})
        ));
      }
      Input** first = &m_dialogInput;
      const auto field = [&](std::string placeholder, std::string& value, bool password) {
        Input* created = nullptr;
        card->addChild(ui::input({
            .out = &created,
            .value = value,
            .placeholder = std::move(placeholder),
            .fontSize = Style::fontSizeBody * scale,
            .passwordMode = password,
            .onChange = [&value](const std::string& v) { value = v; },
            .onSubmit = [submit](const std::string&) { submit(); },
            .onKeyEvent = escape,
        }));
        if (*first == nullptr) {
          *first = created;
        }
        if (password) {
          m_authPasswordInput = created;
        }
      };
      m_authPasswordInput = nullptr;
      if (!anonymous) {
        if ((m_authFlags & G_ASK_PASSWORD_NEED_USERNAME) != 0) {
          field(tr("apps.files.auth.user"), m_authUser, false);
        }
        if ((m_authFlags & G_ASK_PASSWORD_NEED_DOMAIN) != 0) {
          field(tr("apps.files.auth.domain"), m_authDomain, false);
        }
        if ((m_authFlags & G_ASK_PASSWORD_NEED_PASSWORD) != 0) {
          field(tr((m_authFlags & G_ASK_PASSWORD_TCRYPT) != 0 ? "apps.files.auth.passphrase" : "apps.files.auth.password"),
                m_authPassword, true);
        }
        // Con el usuario ya puesto, el cursor va directo a la contraseña.
        if (m_authPasswordInput != nullptr && !m_authUser.empty()) {
          m_dialogInput = m_authPasswordInput;
        }
        if ((m_authFlags & G_ASK_PASSWORD_SAVING_SUPPORTED) != 0) {
          card->addChild(ui::segmented({
              .options = std::vector<ui::SegmentedOption>{
                  {.label = tr("apps.files.auth.save-never")},
                  {.label = tr("apps.files.auth.save-session")},
                  {.label = tr("apps.files.auth.save-forever")},
              },
              .selectedIndex = m_authSave,
              .fontSize = Style::fontSizeCaption * scale,
              .scale = scale,
              .onChange = [this](std::size_t index) { m_authSave = index; },
          }));
        }
      }
      buttons(ui::button({.text = tr("apps.files.cancel"), .variant = ButtonVariant::Ghost,
                          .onClick = [this] { DeferredCall::callLater([this] { replyPassword(false); }); }}),
              ui::button({.text = tr("apps.files.auth.connect"), .variant = ButtonVariant::Primary, .onClick = submit}));
      break;
    }
    case Overlay::Question: {
      const auto newline = m_authMessage.find('\n');
      heading(newline == std::string::npos ? m_authMessage : m_authMessage.substr(0, newline));
      if (newline != std::string::npos) {
        body(m_authMessage.substr(newline + 1));
      }
      auto row = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .wrap = true, .gap = Style::spaceSm * scale});
      for (std::size_t i = 0; i < m_questionChoices.size(); ++i) {
        row->addChild(ui::button({
            .text = m_questionChoices[i],
            .variant = i + 1 == m_questionChoices.size() ? ButtonVariant::Primary : ButtonVariant::Ghost,
            .onClick = [this, i] { DeferredCall::callLater([this, i] { replyQuestion(static_cast<int>(i)); }); },
        }));
      }
      card->addChild(std::move(row));
      break;
    }
    case Overlay::Connect: {
      heading(tr("apps.files.connect.title"));
      const auto submit = [this] { DeferredCall::callLater([this] { connectToServer(m_connectText); }); };
      card->addChild(ui::input({
          .out = &m_dialogInput,
          .value = m_connectText,
          .placeholder = "smb://servidor/carpeta · sftp://usuario@equipo",
          .fontSize = Style::fontSizeBody * scale,
          .onChange = [this](const std::string& value) { m_connectText = value; },
          .onSubmit = [submit](const std::string&) { submit(); },
          .onKeyEvent = [close](std::uint32_t sym, std::uint32_t) -> bool {
            if (sym == XKB_KEY_Escape) {
              close();
              return true;
            }
            return false;
          },
      }));
      // Protocolos que hay en este equipo (backends de gvfs).
      std::string schemes;
      for (const char* wanted : {"smb", "sftp", "ftp", "davs", "dav", "nfs", "afp"}) {
        for (const gchar* const* sc = g_vfs_get_supported_uri_schemes(g_vfs_get_default()); sc != nullptr && *sc != nullptr; ++sc) {
          if (std::string_view(*sc) == wanted) {
            schemes += (schemes.empty() ? "" : ", ") + std::string(wanted) + "://";
          }
        }
      }
      body(tr("apps.files.connect.available", "schemes", schemes));
      if (!m_recentServers.empty()) {
        card->addChild(ui::label({.text = tr("apps.files.connect.recent"), .fontSize = Style::fontSizeCaption * scale,
                                  .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
        for (const auto& server : m_recentServers) {
          card->addChild(ui::button({
              .text = server,
              .glyph = "server-2",
              .fontSize = Style::fontSizeCaption * scale,
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              .onClick = [this, server] { DeferredCall::callLater([this, server] { connectToServer(server); }); },
          }));
        }
      }
      buttons(cancelButton(), ui::button({.text = tr("apps.files.auth.connect"), .variant = ButtonVariant::Primary, .onClick = submit}));
      break;
    }
    case Overlay::Share: {
      heading(trp("apps.files.share.title", static_cast<long>(m_sharePaths.size()), "name",
                  m_sharePaths.empty() ? std::string() : fs::path(m_sharePaths.front()).filename().string()));
      if (m_shareSearching) {
        body(tr("apps.files.share.searching"));
      } else if (m_shareTargets.empty()) {
        body(tr("apps.files.share.none"));
      }
      const float rowWidth = std::min(480.0F * scale, width - 32.0F) - 2.0F * Style::spaceLg * scale;
      for (const auto& target : m_shareTargets) {
        auto stack = ui::node({.height = 40.0F * scale});
        Box* background = nullptr;
        stack->addChild(ui::box({.out = &background, .fill = colorSpecFromRole(ColorRole::OnSurface, 0.04F),
                                 .radius = Style::radiusMd * scale, .width = rowWidth, .height = 40.0F * scale}));
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .paddingH = Style::spaceMd * scale,
                            .width = rowWidth, .height = 40.0F * scale});
        row->addChild(ui::glyph({.glyph = target.glyph, .glyphSize = 18.0F * scale,
                                 .color = colorSpecFromRole(ColorRole::Primary)}));
        row->addChild(ui::label({.text = std::to_string(&target - m_shareTargets.data() + 1) + "  " + target.name, .fontSize = Style::fontSizeBody * scale, .maxLines = 1,
                                 .ellipsize = TextEllipsize::End, .flexGrow = 1.0F}));
        stack->addChild(std::move(row));
        auto hit = ui::inputArea({
            .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
            .width = rowWidth,
            .height = 40.0F * scale,
            .onEnter = [background](const InputArea::PointerData&) {
              background->setFill(colorSpecFromRole(ColorRole::Primary, 0.16F));
            },
            .onLeave = [background] { background->setFill(colorSpecFromRole(ColorRole::OnSurface, 0.04F)); },
            .onClick = [this, target](const InputArea::PointerData&) {
              DeferredCall::callLater([this, target] { sendTo(target); });
            },
        });
        stack->addChild(std::move(hit));
        card->addChild(std::move(stack));
      }
      buttons(cancelButton());
      break;
    }
    case Overlay::Errors: {
      heading(tr("apps.files.errors.title"));
      std::string text;
      for (std::size_t i = 0; i < m_errors.size() && i < 8; ++i) {
        text += (i > 0 ? "\n" : "") + m_errors[i];
      }
      if (m_errors.size() > 8) {
        text += "\n" + trp("apps.files.errors.more", static_cast<long>(m_errors.size() - 8));
      }
      body(text, ColorRole::Error);
      buttons(ui::button({.text = tr("apps.files.close"), .variant = ButtonVariant::Primary, .onClick = close}));
      break;
    }
    case Overlay::None:
      break;
    }

    // Centrada.
    auto center = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .width = width, .height = height});
    center->addChild(std::move(card));
    overlay->addChild(std::move(center));
    return overlay;
  }

  // ── Teclado ────────────────────────────────────────────────────────────────────────────────────────

  bool FilesWindow::textFocused() const {
    for (const Input* input : {m_pathInput, m_findInput, m_dialogInput}) {
      if (input != nullptr && input->inputArea()->focused()) {
        return true;
      }
    }
    return false;
  }

  bool FilesWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed) {
      return false;
    }
    const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt);
    const bool ctrl = (mods & KeyMod::Ctrl) != 0;
    const bool shift = (mods & KeyMod::Shift) != 0;
    const bool alt = (mods & KeyMod::Alt) != 0;
    const std::uint32_t sym = event.sym;

    if (!m_menu.empty()) {
      if (sym == XKB_KEY_Escape) {
        m_menu.clear();
        m_app.requestRebuild();
      } else if (m_menuControl != nullptr && (sym == XKB_KEY_Down || sym == XKB_KEY_Up)) {
        (void)m_menuControl->moveHighlight(sym == XKB_KEY_Down ? 1 : -1);
        m_app.invalidate();
      } else if (m_menuControl != nullptr && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
        (void)m_menuControl->activateHighlighted();
      }
      return true;
    }
    if (m_overlay != Overlay::None) {
      if (sym == XKB_KEY_Escape) {
        if (m_overlay == Overlay::Conflict && m_conflictReply) {
          m_conflictReply({ConflictChoice::Cancel, true});
          m_conflictReply = nullptr;
        }
        if (m_overlay == Overlay::Password) {
          replyPassword(false);
          return true;
        }
        if (m_overlay == Overlay::Question) {
          replyQuestion(-1);
          return true;
        }
        m_overlay = Overlay::None;
        m_app.requestRebuild();
        return true;
      }
      if (m_overlay == Overlay::Share && sym >= XKB_KEY_1 && sym <= XKB_KEY_9
          && static_cast<std::size_t>(sym - XKB_KEY_1) < m_shareTargets.size()) {
        const ShareTarget target = m_shareTargets[sym - XKB_KEY_1];
        sendTo(target);
        return true;
      }
      if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && m_overlay == Overlay::ConfirmDelete) {
        m_overlay = Overlay::None;
        submit({.kind = OpKind::Delete, .sources = m_dialogUris});
        m_app.requestRebuild();
        return true;
      }
      return false;
    }
    if (textFocused()) {
      return false;
    }

    const auto key = [&](std::uint32_t k, std::uint32_t m) { return sym == k && mods == m; };
    const std::string here = tab().model->uri();
    if (key(XKB_KEY_l, KeyMod::Ctrl) || key(XKB_KEY_d, KeyMod::Alt)) {
      m_pathText = uriToPath(here);
      m_editingPath = true;
      m_app.requestRebuild();
      return true;
    }
    if (key(XKB_KEY_f, KeyMod::Ctrl) || key(XKB_KEY_slash, 0)) {
      m_findOpen = true;
      m_focusFind = true;
      m_app.requestRebuild();
      return true;
    }
    if (key(XKB_KEY_Escape, 0)) {
      if (m_findOpen) {
        m_findOpen = false;
        m_findText.clear();
        tab().model->setFilter({});
        tab().model->setSearch({});
        m_app.requestRebuild();
      } else if (m_view != nullptr) {
        m_view->clearSelection();
      }
      return true;
    }
    // Pestañas y ventanas.
    if (key(XKB_KEY_t, KeyMod::Ctrl)) {
      openTab(here);
      return true;
    }
    if (key(XKB_KEY_w, KeyMod::Ctrl)) {
      closeTab(m_active);
      return true;
    }
    if (key(XKB_KEY_n, KeyMod::Ctrl)) {
      newWindow(here);
      return true;
    }
    if (key(XKB_KEY_q, KeyMod::Ctrl)) {
      m_app.quit();
      return true;
    }
    if ((key(XKB_KEY_Tab, KeyMod::Ctrl) || key(XKB_KEY_Next, KeyMod::Ctrl)) && m_tabs.size() > 1) {
      activateTab((m_active + 1) % m_tabs.size());
      return true;
    }
    if ((key(XKB_KEY_ISO_Left_Tab, KeyMod::Ctrl | KeyMod::Shift) || key(XKB_KEY_Tab, KeyMod::Ctrl | KeyMod::Shift)
         || key(XKB_KEY_Prior, KeyMod::Ctrl))
        && m_tabs.size() > 1) {
      activateTab((m_active + m_tabs.size() - 1) % m_tabs.size());
      return true;
    }
    if (alt && !ctrl && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
      activateTab(std::min<std::size_t>(sym - XKB_KEY_1, m_tabs.size() - 1));
      return true;
    }
    // Navegación.
    if (key(XKB_KEY_Left, KeyMod::Alt) || key(XKB_KEY_BackSpace, 0)) {
      goBack();
      return true;
    }
    if (key(XKB_KEY_Right, KeyMod::Alt)) {
      goForward();
      return true;
    }
    if (key(XKB_KEY_Up, KeyMod::Alt)) {
      goUp();
      return true;
    }
    if (key(XKB_KEY_Home, KeyMod::Alt)) {
      navigate(homeUri());
      return true;
    }
    if (mode() == Mode::Columns && mods == 0 && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
      if (sym == XKB_KEY_Left) {
        goUp();
      } else if (const auto items = selection(); items.size() == 1 && items.front()->directory) {
        activateEntry(*items.front());
      }
      return true;
    }
    // Edición.
    if (key(XKB_KEY_c, KeyMod::Ctrl)) {
      copyToClipboard(false);
      return true;
    }
    if (key(XKB_KEY_x, KeyMod::Ctrl)) {
      copyToClipboard(true);
      return true;
    }
    if (key(XKB_KEY_v, KeyMod::Ctrl)) {
      if (canWriteHere()) {
        paste(here);
      }
      return true;
    }
    if (key(XKB_KEY_C, KeyMod::Ctrl | KeyMod::Shift) || key(XKB_KEY_c, KeyMod::Ctrl | KeyMod::Shift)) {
      copyPaths();
      return true;
    }
    if (key(XKB_KEY_a, KeyMod::Ctrl) && m_view != nullptr) {
      m_view->selectAll();
      return true;
    }
    if ((key(XKB_KEY_I, KeyMod::Ctrl | KeyMod::Shift) || key(XKB_KEY_i, KeyMod::Ctrl | KeyMod::Shift)) && m_view != nullptr) {
      m_view->invertSelection();
      return true;
    }
    if (key(XKB_KEY_F2, 0)) {
      startRename();
      return true;
    }
    if (key(XKB_KEY_Delete, 0) || key(XKB_KEY_KP_Delete, 0)) {
      trashSelection();
      return true;
    }
    if (key(XKB_KEY_Delete, KeyMod::Shift) || key(XKB_KEY_KP_Delete, KeyMod::Shift)) {
      deleteSelection();
      return true;
    }
    if (key(XKB_KEY_z, KeyMod::Ctrl)) {
      m_ops.undo();
      return true;
    }
    if (key(XKB_KEY_Z, KeyMod::Ctrl | KeyMod::Shift) || key(XKB_KEY_z, KeyMod::Ctrl | KeyMod::Shift)
        || key(XKB_KEY_y, KeyMod::Ctrl)) {
      m_ops.redo();
      return true;
    }
    if (key(XKB_KEY_N, KeyMod::Ctrl | KeyMod::Shift) || key(XKB_KEY_n, KeyMod::Ctrl | KeyMod::Shift)) {
      startNew(true);
      return true;
    }
    if (key(XKB_KEY_n, KeyMod::Ctrl | KeyMod::Alt)) {
      startNew(false);
      return true;
    }
    if (key(XKB_KEY_m, KeyMod::Ctrl)) {
      createLink();
      return true;
    }
    // Vista.
    if (key(XKB_KEY_h, KeyMod::Ctrl) || key(XKB_KEY_period, KeyMod::Alt)) {
      toggleHidden();
      return true;
    }
    if (ctrl && !shift && !alt && sym >= XKB_KEY_1 && sym <= XKB_KEY_3) {
      setMode(static_cast<Mode>(sym - XKB_KEY_1));
      return true;
    }
    if (key(XKB_KEY_plus, KeyMod::Ctrl) || key(XKB_KEY_equal, KeyMod::Ctrl) || key(XKB_KEY_KP_Add, KeyMod::Ctrl)
        || key(XKB_KEY_plus, KeyMod::Ctrl | KeyMod::Shift)) {
      zoom(1);
      return true;
    }
    if (key(XKB_KEY_minus, KeyMod::Ctrl) || key(XKB_KEY_KP_Subtract, KeyMod::Ctrl)) {
      zoom(-1);
      return true;
    }
    if (key(XKB_KEY_0, KeyMod::Ctrl)) {
      zoom(0);
      return true;
    }
    if (key(XKB_KEY_F5, 0) || key(XKB_KEY_r, KeyMod::Ctrl)) {
      tab().model->reload();
      m_places.refresh();
      return true;
    }
    if (key(XKB_KEY_F9, 0)) {
      m_sidebar = !m_sidebar;
      m_app.requestRebuild();
      return true;
    }
    if (key(XKB_KEY_d, KeyMod::Ctrl)) {
      toggleBookmark(here);
      return true;
    }
    if (key(XKB_KEY_Return, KeyMod::Alt) || key(XKB_KEY_KP_Enter, KeyMod::Alt)) {
      showProperties(selectedUris());
      return true;
    }
    if (key(XKB_KEY_F4, 0) || key(XKB_KEY_t, KeyMod::Ctrl | KeyMod::Alt)) {
      openTerminal(here);
      return true;
    }
    if (key(XKB_KEY_Menu, 0) || key(XKB_KEY_F10, KeyMod::Shift)) {
      openContextMenu(tab().view.cursor, m_width * 0.4F, m_height * 0.4F);
      return true;
    }
    return false;
  }

  // ── Estado ─────────────────────────────────────────────────────────────────────────────────────────

  void FilesWindow::loadState() {
    std::error_code ec;
    if (!fs::exists(statePath(), ec)) {
      return;
    }
    try {
      const toml::table table = toml::parse_file(statePath().string());
      const auto modeOf = [](std::string_view id) {
        return id == "grid" ? Mode::Grid : id == "columns" ? Mode::Columns : Mode::List;
      };
      if (const auto* view = table["view"].as_table(); view != nullptr) {
        m_defaultMode = modeOf((*view)["mode"].value_or(std::string("list")));
        m_iconSize = static_cast<float>((*view)["icon_size"].value_or(64.0));
        m_sidebar = (*view)["sidebar"].value_or(true);
        m_options.showHidden = (*view)["show_hidden"].value_or(false);
        m_thumbnails = (*view)["thumbnails"].value_or(true);
        m_icons.setDefaultFolderColor((*view)["folder_color"].value_or(std::string()));
      }
      if (const auto* folders = table["folders"].as_table(); folders != nullptr) {
        for (const auto& [key, value] : *folders) {
          m_folderModes[std::string(key.str())] = modeOf(value.value_or(std::string("list")));
        }
      }
      if (const auto* recent = table["servers"]["recent"].as_array(); recent != nullptr) {
        for (const auto& server : *recent) {
          if (const auto value = server.value<std::string>()) {
            m_recentServers.push_back(*value);
          }
        }
      }
      if (const auto* sorts = table["sort"].as_table(); sorts != nullptr) {
        for (const auto& [key, value] : *sorts) {
          if (const auto* entry = value.as_table(); entry != nullptr) {
            m_folderSorts[std::string(key.str())] = {
                .sort = sortFromId((*entry)["by"].value_or(std::string("name"))),
                .descending = (*entry)["descending"].value_or(false),
                .foldersFirst = (*entry)["folders_first"].value_or(true),
            };
          }
        }
      }
    } catch (const toml::parse_error&) {
      // Archivo dañado: valores de fábrica.
    }
  }

  void FilesWindow::saveState() {
    const auto modeId = [](Mode m) { return m == Mode::Grid ? "grid" : m == Mode::Columns ? "columns" : "list"; };
    toml::table view{
        {"mode", modeId(m_defaultMode)},
        {"icon_size", static_cast<double>(m_iconSize)},
        {"sidebar", m_sidebar},
        {"show_hidden", m_options.showHidden},
        {"thumbnails", m_thumbnails},
        {"folder_color", m_icons.defaultFolderColor()},
    };
    toml::table folders;
    for (const auto& [uri, m] : m_folderModes) {
      folders.insert(uri, modeId(m));
    }
    toml::table sorts;
    for (const auto& [uri, options] : m_folderSorts) {
      sorts.insert(uri, toml::table{{"by", sortId(options.sort)}, {"descending", options.descending},
                                    {"folders_first", options.foldersFirst}});
    }
    toml::array servers;
    for (const auto& server : m_recentServers) {
      servers.push_back(server);
    }
    toml::table root{{"view", view}, {"folders", folders}, {"sort", sorts}, {"servers", toml::table{{"recent", servers}}}};
    std::error_code ec;
    fs::create_directories(statePath().parent_path(), ec);
    std::ofstream out(statePath(), std::ios::trunc);
    out << root << '\n';
  }

} // namespace okaru::files
