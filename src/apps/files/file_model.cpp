#include "apps/files/file_model.h"

#include "core/deferred_call.h"

#include <algorithm>
#include <ctime>
#include <deque>
#include <gio/gio.h>
#include <thread>

namespace okaru::files {

  const char* const kQueryAttributes = "standard::name,standard::display-name,standard::type,standard::size,"
                                       "standard::is-hidden,standard::is-backup,standard::is-symlink,"
                                       "standard::content-type,standard::fast-content-type,standard::icon,"
                                       "standard::target-uri,time::modified,unix::mode,owner::user,"
                                       "access::can-write,access::can-execute,trash::orig-path,"
                                       "trash::deletion-date";

  namespace {

    std::string takeString(char* raw) {
      std::string out = raw != nullptr ? raw : "";
      g_free(raw);
      return out;
    }

    std::int64_t parseIsoDate(const char* text) {
      if (text == nullptr) {
        return 0;
      }
      GDateTime* date = g_date_time_new_from_iso8601(text, nullptr);
      if (date == nullptr) {
        return 0;
      }
      const std::int64_t out = g_date_time_to_unix(date);
      g_date_time_unref(date);
      return out;
    }

    int compareEntries(const FileEntry& a, const FileEntry& b, SortKey key) {
      switch (key) {
      case SortKey::Size:
        if (a.directory && b.directory && a.childCount != b.childCount) {
          return a.childCount < b.childCount ? -1 : 1;
        }
        if (a.size != b.size) {
          return a.size < b.size ? -1 : 1;
        }
        break;
      case SortKey::Modified:
        if (a.modified != b.modified) {
          return a.modified < b.modified ? -1 : 1;
        }
        break;
      case SortKey::Type:
        if (const int c = a.typeName.compare(b.typeName); c != 0) {
          return c;
        }
        break;
      case SortKey::Name:
        break;
      }
      return a.sortKey.compare(b.sortKey);
    }

  } // namespace

  FileEntry entryFromInfo(void* parentGFile, void* gfileInfo) {
    auto* parent = static_cast<GFile*>(parentGFile);
    auto* info = static_cast<GFileInfo*>(gfileInfo);
    FileEntry e;
    e.name = g_file_info_get_name(info) != nullptr ? g_file_info_get_name(info) : "";
    const char* display = g_file_info_get_display_name(info);
    e.display = display != nullptr ? display : e.name;
    GFile* child = g_file_get_child(parent, e.name.c_str());
    e.uri = takeString(g_file_get_uri(child));
    e.path = takeString(g_file_get_path(child));
    g_object_unref(child);

    const GFileType type = g_file_info_get_file_type(info);
    e.mountable = type == G_FILE_TYPE_MOUNTABLE || type == G_FILE_TYPE_SHORTCUT;
    e.directory = type == G_FILE_TYPE_DIRECTORY || e.mountable;
    e.symlink = g_file_info_get_is_symlink(info) != FALSE;
    e.hidden = g_file_info_get_is_hidden(info) != FALSE || g_file_info_get_is_backup(info) != FALSE
        || e.name.starts_with('.');
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
      e.size = static_cast<std::uint64_t>(g_file_info_get_size(info));
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
      e.modified = static_cast<std::int64_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
    }
    e.mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    if (const char* owner = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER); owner != nullptr) {
      e.owner = owner;
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)) {
      e.canWrite = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) != FALSE;
    }
    e.canExecute = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) != FALSE;
    if (const char* target = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
        target != nullptr) {
      e.target = target;
    }
    if (const char* orig = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
        orig != nullptr) {
      e.trashOrigin = orig;
    }
    e.deleted = parseIsoDate(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE));

    const char* contentType = g_file_info_get_content_type(info);
    if (contentType == nullptr) {
      contentType = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    }
    e.contentType = contentType != nullptr ? contentType : (e.directory ? "inode/directory" : "");
    if (!e.contentType.empty()) {
      e.typeName = takeString(g_content_type_get_description(e.contentType.c_str()));
    }
    // El icono que da gvfs (montajes, unidades, teléfonos) vale más que el del tipo.
    GIcon* icon = g_file_info_get_icon(info);
    if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
      for (const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon)); *names != nullptr; ++names) {
        e.icons.emplace_back(*names);
      }
    }
    if (e.icons.empty() && !e.contentType.empty()) {
      GIcon* typeIcon = g_content_type_get_icon(e.contentType.c_str());
      if (typeIcon != nullptr && G_IS_THEMED_ICON(typeIcon)) {
        for (const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(typeIcon)); *names != nullptr;
             ++names) {
          e.icons.emplace_back(*names);
        }
      }
      if (typeIcon != nullptr) {
        g_object_unref(typeIcon);
      }
    }
    e.icons.emplace_back(e.directory ? "folder" : "text-x-generic");
    e.sortKey = takeString(g_utf8_collate_key_for_filename(e.display.c_str(), -1));
    return e;
  }

  bool listDirectory(const std::string& uri, std::vector<FileEntry>& out, std::string& error,
                     const std::atomic<bool>* cancel, int* code) {
    GFile* dir = g_file_new_for_uri(uri.c_str());
    GError* gerror = nullptr;
    GFileEnumerator* it = g_file_enumerate_children(dir, kQueryAttributes, G_FILE_QUERY_INFO_NONE, nullptr, &gerror);
    if (it == nullptr) {
      error = gerror != nullptr ? gerror->message : "?";
      if (code != nullptr && gerror != nullptr && gerror->domain == G_IO_ERROR) {
        *code = gerror->code;
      }
      g_clear_error(&gerror);
      g_object_unref(dir);
      return false;
    }
    while (cancel == nullptr || !cancel->load()) {
      GFileInfo* info = g_file_enumerator_next_file(it, nullptr, &gerror);
      if (info == nullptr) {
        break;
      }
      out.push_back(entryFromInfo(dir, info));
      g_object_unref(info);
    }
    if (gerror != nullptr) {
      error = gerror->message;
      g_clear_error(&gerror);
    }
    g_file_enumerator_close(it, nullptr, nullptr);
    g_object_unref(it);
    g_object_unref(dir);
    return error.empty();
  }

  void sortEntries(std::vector<FileEntry>& entries, const ViewOptions& options) {
    std::ranges::stable_sort(entries, [&options](const FileEntry& a, const FileEntry& b) {
      if (options.foldersFirst && a.directory != b.directory) {
        return a.directory;
      }
      const int c = compareEntries(a, b, options.sort);
      return options.descending ? c > 0 : c < 0;
    });
  }

  std::string normalizeForMatch(std::string_view text) {
    // Sin tildes (NFD y fuera las marcas) y en minúsculas.
    char* folded = g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
    char* nfd = g_utf8_normalize(folded, -1, G_NORMALIZE_NFD);
    g_free(folded);
    std::string out;
    if (nfd == nullptr) {
      return std::string(text);
    }
    for (const char* p = nfd; *p != '\0'; p = g_utf8_next_char(p)) {
      const gunichar c = g_utf8_get_char(p);
      if (g_unichar_type(c) == G_UNICODE_NON_SPACING_MARK) {
        continue;
      }
      out.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
    }
    g_free(nfd);
    return out;
  }

  bool matchesFilter(const FileEntry& entry, std::string_view needle) {
    return needle.empty() || normalizeForMatch(entry.display).contains(needle);
  }

  std::string formatSize(std::uint64_t bytes) { return takeString(g_format_size(bytes)); }

  std::string formatDate(std::int64_t seconds) {
    if (seconds <= 0) {
      return "—";
    }
    const std::time_t now = std::time(nullptr);
    std::tm then{};
    std::tm today{};
    const std::time_t t = seconds;
    localtime_r(&t, &then);
    localtime_r(&now, &today);
    char buffer[64];
    const char* format = then.tm_year == today.tm_year && then.tm_yday == today.tm_yday ? "%H:%M"
        : then.tm_year == today.tm_year                                                 ? "%d %b %H:%M"
                                                                                        : "%d %b %Y";
    std::strftime(buffer, sizeof(buffer), format, &then);
    return buffer;
  }

  std::string formatPermissions(std::uint32_t mode) {
    std::string out(9, '-');
    const char* letters = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
      if ((mode & (1U << (8 - i))) != 0) {
        out[static_cast<std::size_t>(i)] = letters[i];
      }
    }
    return out;
  }

  std::string uriToPath(const std::string& uri) {
    GFile* file = g_file_new_for_uri(uri.c_str());
    std::string path = takeString(g_file_get_path(file));
    g_object_unref(file);
    return path.empty() ? uri : path;
  }

  std::string pathToUri(const std::string& path) {
    GFile* file = g_file_new_for_commandline_arg(path.c_str());
    std::string uri = takeString(g_file_get_uri(file));
    g_object_unref(file);
    return uri;
  }

  std::string parentUri(const std::string& uri) {
    GFile* file = g_file_new_for_uri(uri.c_str());
    GFile* parent = g_file_get_parent(file);
    std::string out = parent != nullptr ? takeString(g_file_get_uri(parent)) : "";
    if (parent != nullptr) {
      g_object_unref(parent);
    }
    g_object_unref(file);
    return out;
  }

  std::string childUri(const std::string& parent, const std::string& name) {
    GFile* dir = g_file_new_for_uri(parent.c_str());
    GFile* child = g_file_get_child(dir, name.c_str());
    std::string out = takeString(g_file_get_uri(child));
    g_object_unref(child);
    g_object_unref(dir);
    return out;
  }

  std::string displayNameOf(const std::string& uri) {
    GFile* file = g_file_new_for_uri(uri.c_str());
    std::string out = takeString(g_file_get_parse_name(file));
    char* base = g_file_get_basename(file);
    if (base != nullptr && std::string_view(base) != "/") {
      char* utf8 = g_filename_display_name(base);
      out = takeString(utf8);
    }
    g_free(base);
    g_object_unref(file);
    return out;
  }

  std::string numberedName(const std::string& name, int n, bool directory) {
    // La extensión se conserva (también la doble de .tar.gz); las carpetas no tienen extensión.
    std::size_t dot = directory ? std::string::npos : name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
      const std::size_t prev = name.rfind('.', dot - 1);
      if (prev != std::string::npos && prev > 0 && name.substr(prev, dot - prev) == ".tar") {
        dot = prev;
      }
    }
    if (dot == 0) {
      dot = std::string::npos;
    }
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    return stem + " (" + std::to_string(n) + ")" + ext;
  }

  std::string uniqueName(const std::string& dirUri, const std::string& name) {
    GFile* dir = g_file_new_for_uri(dirUri.c_str());
    GFile* candidate = g_file_get_child(dir, name.c_str());
    const bool directory = g_file_query_file_type(candidate, G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY;
    std::string out = name;
    for (int n = 2; g_file_query_exists(candidate, nullptr) != FALSE && n < 10000; ++n) {
      g_object_unref(candidate);
      out = numberedName(name, n, directory);
      candidate = g_file_get_child(dir, out.c_str());
    }
    g_object_unref(candidate);
    g_object_unref(dir);
    return out;
  }

  // ── DirectoryModel ─────────────────────────────────────────────────────────────────────────────────

  DirectoryModel::DirectoryModel() = default;

  DirectoryModel::~DirectoryModel() {
    if (m_loadCancel) {
      m_loadCancel->store(true);
    }
    cancelSearch();
    stopMonitor();
  }

  void DirectoryModel::load(const std::string& uri) {
    if (uri != m_uri) {
      m_search.clear();
      cancelSearch();
      m_found.clear();
      m_filter.clear();
      m_all.clear();
      m_visible.clear();
    }
    m_uri = uri;
    reload();
    startMonitor();
  }

  void DirectoryModel::reload() {
    if (m_loadCancel) {
      m_loadCancel->store(true);
    }
    m_loadCancel = std::make_shared<std::atomic<bool>>(false);
    m_loading = true;
    const std::uint64_t generation = ++m_generation;
    std::weak_ptr<int> alive = m_alive;
    std::thread([this, alive, generation, uri = m_uri, cancel = m_loadCancel] {
      std::vector<FileEntry> entries;
      std::string error;
      int code = -1;
      (void)listDirectory(uri, entries, error, cancel.get(), &code);
      if (cancel->load()) {
        return;
      }
      DeferredCall::callLater([this, alive, generation, code, entries = std::move(entries),
                               error = std::move(error)]() mutable {
        if (alive.expired() || generation != m_generation) {
          return;
        }
        m_all = std::move(entries);
        m_error = std::move(error);
        m_errorCode = code;
        m_loading = false;
        rebuildVisible();
      });
    }).detach();
    if (m_onChanged) {
      m_onChanged();
    }
  }

  void DirectoryModel::setOptions(const ViewOptions& options) {
    m_options = options;
    rebuildVisible();
  }

  void DirectoryModel::setFilter(std::string filter) {
    m_filter = std::move(filter);
    rebuildVisible();
  }

  void DirectoryModel::cancelSearch() {
    if (m_searchCancel) {
      m_searchCancel->store(true);
      m_searchCancel.reset();
    }
    m_searchRunning = false;
  }

  void DirectoryModel::setSearch(std::string query) {
    cancelSearch();
    m_search = std::move(query);
    m_found.clear();
    if (m_search.empty()) {
      rebuildVisible();
      return;
    }
    // Recorrido en anchura desde la carpeta actual; los resultados llegan por tandas.
    // shortcut: recorre el disco en cada búsqueda (sin índice); pasar a localsearch/plocate si se queda corto.
    m_searchCancel = std::make_shared<std::atomic<bool>>(false);
    m_searchRunning = true;
    std::weak_ptr<int> alive = m_alive;
    const std::string needle = normalizeForMatch(m_search);
    std::thread([this, alive, root = m_uri, needle, cancel = m_searchCancel, showHidden = m_options.showHidden] {
      constexpr std::size_t kLimit = 2000;
      std::deque<std::pair<std::string, std::string>> pending{{root, ""}}; // URI y ruta relativa
      std::vector<FileEntry> batch;
      std::size_t found = 0;
      const auto flush = [&](bool done) {
        DeferredCall::callLater([this, alive, cancel, batch = std::move(batch), done]() mutable {
          if (alive.expired() || cancel->load()) {
            return;
          }
          for (auto& e : batch) {
            m_found.push_back(std::move(e));
          }
          m_searchRunning = !done;
          rebuildVisible();
        });
        batch.clear();
      };
      auto last = std::chrono::steady_clock::now();
      while (!pending.empty() && !cancel->load() && found < kLimit) {
        auto [uri, relative] = std::move(pending.front());
        pending.pop_front();
        std::vector<FileEntry> children;
        std::string error;
        (void)listDirectory(uri, children, error, cancel.get());
        for (auto& child : children) {
          if (child.hidden && !showHidden) {
            continue;
          }
          if (child.directory && !child.symlink && !child.mountable) {
            pending.emplace_back(child.uri, relative.empty() ? child.display : relative + "/" + child.display);
          }
          if (normalizeForMatch(child.display).contains(needle)) {
            child.location = relative;
            batch.push_back(std::move(child));
            ++found;
          }
        }
        if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(120) && !batch.empty()) {
          flush(false);
          last = std::chrono::steady_clock::now();
        }
      }
      if (!cancel->load()) {
        flush(true);
      }
    }).detach();
    rebuildVisible();
  }

  void DirectoryModel::rebuildVisible() {
    const std::vector<FileEntry>& source = m_search.empty() ? m_all : m_found;
    const std::string needle = normalizeForMatch(m_filter);
    m_visible.clear();
    m_visible.reserve(source.size());
    for (const auto& e : source) {
      if ((m_options.showHidden || !e.hidden) && matchesFilter(e, needle)) {
        m_visible.push_back(e);
      }
    }
    sortEntries(m_visible, m_options);
    if (m_onChanged) {
      m_onChanged();
    }
  }

  void DirectoryModel::startMonitor() {
    stopMonitor();
    GFile* dir = g_file_new_for_uri(m_uri.c_str());
    m_monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(dir);
    if (m_monitor != nullptr) {
      g_signal_connect(m_monitor, "changed", G_CALLBACK(&DirectoryModel::onMonitorEvent), this);
    }
  }

  void DirectoryModel::stopMonitor() {
    if (m_monitor != nullptr) {
      g_signal_handlers_disconnect_by_data(m_monitor, this);
      g_file_monitor_cancel(m_monitor);
      g_object_unref(m_monitor);
      m_monitor = nullptr;
    }
  }

  void DirectoryModel::onMonitorEvent(GFileMonitor* /*monitor*/, void* /*file*/, void* /*other*/, int event,
                                      void* self) {
    if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
      return;
    }
    // Ráfagas (copiar 1000 archivos) → una sola recarga.
    auto* model = static_cast<DirectoryModel*>(self);
    if (!model->m_reloadTimer.active()) {
      model->m_reloadTimer.start(std::chrono::milliseconds(250), [model] { model->reload(); });
    }
  }

} // namespace okaru::files
