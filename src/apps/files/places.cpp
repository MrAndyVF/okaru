#include "apps/files/places.h"

#include "apps/files/file_model.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <sstream>

namespace okaru::files {

  namespace {

    namespace fs = std::filesystem;

    fs::path bookmarksPath() { return fs::path(g_get_user_config_dir()) / "gtk-3.0" / "bookmarks"; }

    std::string takeString(char* raw) {
      std::string out = raw != nullptr ? raw : "";
      g_free(raw);
      return out;
    }

    std::vector<std::string> iconNames(GIcon* icon) {
      std::vector<std::string> out;
      if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
        for (const char* const* n = g_themed_icon_get_names(G_THEMED_ICON(icon)); *n != nullptr; ++n) {
          out.emplace_back(*n);
        }
      }
      return out;
    }

    std::string glyphFor(const std::vector<std::string>& icons, bool remote) {
      for (const auto& name : icons) {
        if (name.contains("phone") || name.contains("multimedia-player") || name.contains("pda")) {
          return "device-mobile";
        }
        if (name.contains("camera")) {
          return "camera";
        }
        if (name.contains("optical") || name.contains("cdrom") || name.contains("dvd")) {
          return "disc";
        }
        if (name.contains("flash") || name.contains("sd")) {
          return "device-sd-card";
        }
        if (name.contains("removable") || name.contains("usb")) {
          return "usb";
        }
      }
      return remote ? "server-2" : "database";
    }

    // XDG_PROJECTS_DIR (xdg-user-dirs 0.19+): GLib aún no la expone, se lee de user-dirs.dirs. Sin definir, ~/Projects.
    std::string projectsDir() {
      const std::string home = g_get_home_dir();
      std::ifstream in(fs::path(g_get_user_config_dir()) / "user-dirs.dirs");
      for (std::string line; std::getline(in, line);) {
        if (!line.starts_with("XDG_PROJECTS_DIR=")) {
          continue;
        }
        std::string value = line.substr(17);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }
        if (value.starts_with("$HOME")) {
          value = home + value.substr(5);
        }
        return value == home ? std::string() : value; // apuntar a la personal = desactivada
      }
      return home + "/Projects";
    }

    void fillSpace(Place& place, GFile* root) {
      GFileInfo* info = g_file_query_filesystem_info(root, "filesystem::size,filesystem::free", nullptr, nullptr);
      if (info != nullptr) {
        place.total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        place.free = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        g_object_unref(info);
      }
    }

    GMount* findMount(GVolumeMonitor* monitor, const std::string& id) {
      GList* mounts = g_volume_monitor_get_mounts(monitor);
      GMount* found = nullptr;
      for (GList* l = mounts; l != nullptr; l = l->next) {
        auto* mount = G_MOUNT(l->data);
        GFile* root = g_mount_get_root(mount);
        if (found == nullptr && "mount:" + takeString(g_file_get_uri(root)) == id) {
          found = G_MOUNT(g_object_ref(mount));
        }
        g_object_unref(root);
      }
      g_list_free_full(mounts, g_object_unref);
      return found;
    }

    std::string volumeId(GVolume* volume) {
      std::string id = takeString(g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UUID));
      if (id.empty()) {
        id = takeString(g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
      }
      if (id.empty()) {
        id = takeString(g_volume_get_name(volume));
      }
      return "volume:" + id;
    }

    struct MountCall {
      Places::DoneFn done;
    };

  } // namespace

  Places::Places() {
    m_monitor = g_volume_monitor_get();
    for (const char* signal : {"mount-added", "mount-removed", "mount-changed", "volume-added", "volume-removed",
                               "volume-changed", "drive-connected", "drive-disconnected"}) {
      g_signal_connect(m_monitor, signal, G_CALLBACK(&Places::onMonitorChanged), this);
    }
    refresh();
  }

  Places::~Places() {
    if (m_monitor != nullptr) {
      g_signal_handlers_disconnect_by_data(m_monitor, this);
      g_object_unref(m_monitor);
    }
  }

  void Places::onMonitorChanged(void* /*monitor*/, void* /*object*/, void* self) {
    auto* places = static_cast<Places*>(self);
    places->refresh();
  }

  void Places::refresh() {
    using i18n::tr;
    m_places.clear();
    const auto add = [this](PlaceSection section, std::string name, std::string uri, std::string glyph) {
      m_places.push_back({.section = section, .name = std::move(name), .uri = std::move(uri), .glyph = std::move(glyph)});
    };

    // Lugares: carpeta personal, recientes, carpetas XDG que existan y no sean la personal, papelera.
    const std::string home = g_get_home_dir();
    add(PlaceSection::Places, tr("apps.files.places.home"), pathToUri(home), "home");
    add(PlaceSection::Places, tr("apps.files.places.recent"), "recent:///", "history");
    const std::pair<GUserDirectory, const char*> xdg[] = {
        {G_USER_DIRECTORY_DESKTOP, "app-window"}, {G_USER_DIRECTORY_DOCUMENTS, "file-text"},
        {G_USER_DIRECTORY_DOWNLOAD, "download"},  {G_USER_DIRECTORY_MUSIC, "music"},
        {G_USER_DIRECTORY_PICTURES, "photo"},     {G_USER_DIRECTORY_VIDEOS, "movie"},
    };
    for (const auto& [dir, glyph] : xdg) {
      const char* path = g_get_user_special_dir(dir);
      if (path != nullptr && home != path && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        add(PlaceSection::Places, displayNameOf(pathToUri(path)), pathToUri(path), glyph);
      }
      // Proyectos va justo después de Imágenes.
      if (dir == G_USER_DIRECTORY_PICTURES) {
        if (const std::string projects = projectsDir();
            !projects.empty() && g_file_test(projects.c_str(), G_FILE_TEST_IS_DIR)) {
          add(PlaceSection::Places, displayNameOf(pathToUri(projects)), pathToUri(projects), "code");
        }
      }
    }
    add(PlaceSection::Places, tr("apps.files.places.trash"), "trash:///", "trash");

    for (const auto& bookmark : readBookmarks()) {
      add(PlaceSection::Bookmarks, bookmark.name.empty() ? displayNameOf(bookmark.uri) : bookmark.name, bookmark.uri,
          bookmark.uri.starts_with("file:") ? "folder" : "folder-share");
    }

    // Raíz del sistema con su espacio libre.
    {
      Place root{.section = PlaceSection::Devices, .name = tr("apps.files.places.computer"), .uri = "file:///",
                 .glyph = "device-laptop"};
      GFile* file = g_file_new_for_path("/");
      fillSpace(root, file);
      g_object_unref(file);
      m_places.push_back(std::move(root));
    }

    // Montajes (USB, discos, teléfonos, red) y volúmenes sin montar.
    GList* mounts = g_volume_monitor_get_mounts(m_monitor);
    for (GList* l = mounts; l != nullptr; l = l->next) {
      auto* mount = G_MOUNT(l->data);
      if (g_mount_is_shadowed(mount) != FALSE) {
        continue;
      }
      GFile* root = g_mount_get_root(mount);
      const std::string uri = takeString(g_file_get_uri(root));
      const bool native = g_file_is_native(root) != FALSE;
      GIcon* icon = g_mount_get_icon(mount);
      const auto icons = iconNames(icon);
      if (icon != nullptr) {
        g_object_unref(icon);
      }
      const bool device = native || uri.starts_with("mtp:") || uri.starts_with("gphoto2:") || uri.starts_with("afc:");
      Place place{
          .section = device ? PlaceSection::Devices : PlaceSection::Network,
          .name = takeString(g_mount_get_name(mount)),
          .uri = uri,
          .glyph = glyphFor(icons, !device),
          .id = "mount:" + uri,
          .canUnmount = g_mount_can_unmount(mount) != FALSE,
          .canEject = g_mount_can_eject(mount) != FALSE,
      };
      if (native) {
        fillSpace(place, root);
      }
      g_object_unref(root);
      m_places.push_back(std::move(place));
    }
    g_list_free_full(mounts, g_object_unref);

    GList* volumes = g_volume_monitor_get_volumes(m_monitor);
    for (GList* l = volumes; l != nullptr; l = l->next) {
      auto* volume = G_VOLUME(l->data);
      GMount* mount = g_volume_get_mount(volume);
      if (mount != nullptr) {
        g_object_unref(mount);
        continue;
      }
      if (g_volume_can_mount(volume) == FALSE) {
        continue;
      }
      GIcon* icon = g_volume_get_icon(volume);
      const auto icons = iconNames(icon);
      if (icon != nullptr) {
        g_object_unref(icon);
      }
      m_places.push_back({
          .section = PlaceSection::Devices,
          .name = takeString(g_volume_get_name(volume)),
          .glyph = glyphFor(icons, false),
          .id = volumeId(volume),
          .mounted = false,
          .canEject = g_volume_can_eject(volume) != FALSE,
      });
    }
    g_list_free_full(volumes, g_object_unref);

    add(PlaceSection::Network, tr("apps.files.places.network"), "network:///", "network");

    if (m_onChanged) {
      m_onChanged();
    }
  }

  // ── Marcadores ─────────────────────────────────────────────────────────────────────────────────────

  std::vector<Places::Bookmark> Places::readBookmarks() const {
    std::vector<Bookmark> out;
    std::ifstream in(bookmarksPath());
    for (std::string line; std::getline(in, line);) {
      if (line.empty()) {
        continue;
      }
      const auto space = line.find(' ');
      out.push_back({line.substr(0, space), space == std::string::npos ? "" : line.substr(space + 1)});
    }
    return out;
  }

  void Places::writeBookmarks(const std::vector<Bookmark>& bookmarks) const {
    std::error_code ec;
    fs::create_directories(bookmarksPath().parent_path(), ec);
    const fs::path temp = bookmarksPath().string() + ".okaru-tmp";
    {
      std::ofstream out(temp, std::ios::trunc);
      for (const auto& b : bookmarks) {
        out << b.uri << (b.name.empty() ? "" : " " + b.name) << '\n';
      }
    }
    fs::rename(temp, bookmarksPath(), ec);
  }

  bool Places::isBookmarked(const std::string& uri) const {
    return std::ranges::any_of(readBookmarks(), [&](const Bookmark& b) { return b.uri == uri; });
  }

  void Places::addBookmark(const std::string& uri) {
    auto list = readBookmarks();
    if (std::ranges::none_of(list, [&](const Bookmark& b) { return b.uri == uri; })) {
      list.push_back({uri, {}});
      writeBookmarks(list);
    }
    refresh();
  }

  void Places::removeBookmark(const std::string& uri) {
    auto list = readBookmarks();
    std::erase_if(list, [&](const Bookmark& b) { return b.uri == uri; });
    writeBookmarks(list);
    refresh();
  }

  void Places::renameBookmark(const std::string& uri, const std::string& name) {
    auto list = readBookmarks();
    for (auto& b : list) {
      if (b.uri == uri) {
        b.name = name;
      }
    }
    writeBookmarks(list);
    refresh();
  }

  // ── Montar y desmontar ─────────────────────────────────────────────────────────────────────────────

  void Places::mount(const std::string& id, DoneFn done) {
    GList* volumes = g_volume_monitor_get_volumes(m_monitor);
    GVolume* target = nullptr;
    for (GList* l = volumes; l != nullptr; l = l->next) {
      if (target == nullptr && volumeId(G_VOLUME(l->data)) == id) {
        target = G_VOLUME(g_object_ref(l->data));
      }
    }
    g_list_free_full(volumes, g_object_unref);
    if (target == nullptr) {
      done(false, {});
      return;
    }
    // shortcut: sin GMountOperation con diálogo propio; los volúmenes cifrados se piden en la fase de red.
    GMountOperation* operation = g_mount_operation_new();
    g_volume_mount(
        target, G_MOUNT_MOUNT_NONE, operation, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<MountCall> call(static_cast<MountCall*>(data));
          GError* error = nullptr;
          auto* volume = G_VOLUME(source);
          if (g_volume_mount_finish(volume, result, &error) == FALSE) {
            call->done(false, error != nullptr ? error->message : "?");
            g_clear_error(&error);
            return;
          }
          std::string uri;
          if (GMount* mount = g_volume_get_mount(volume); mount != nullptr) {
            GFile* root = g_mount_get_root(mount);
            uri = takeString(g_file_get_uri(root));
            g_object_unref(root);
            g_object_unref(mount);
          }
          call->done(true, uri);
        },
        new MountCall{std::move(done)}
    );
    g_object_unref(operation);
    g_object_unref(target);
  }

  void Places::unmount(const std::string& id, bool eject, DoneFn done) {
    GMount* mount = findMount(m_monitor, id);
    if (mount == nullptr) {
      done(false, {});
      return;
    }
    GMountOperation* operation = g_mount_operation_new();
    const auto finishUnmount = [](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<MountCall> call(static_cast<MountCall*>(data));
      GError* error = nullptr;
      const bool ok = g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error) != FALSE;
      call->done(ok, ok || error == nullptr ? "" : error->message);
      g_clear_error(&error);
    };
    const auto finishEject = [](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<MountCall> call(static_cast<MountCall*>(data));
      GError* error = nullptr;
      const bool ok = g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error) != FALSE;
      call->done(ok, ok || error == nullptr ? "" : error->message);
      g_clear_error(&error);
    };
    if (eject && g_mount_can_eject(mount) != FALSE) {
      g_mount_eject_with_operation(
          mount, G_MOUNT_UNMOUNT_NONE, operation, nullptr, finishEject, new MountCall{std::move(done)}
      );
    } else {
      g_mount_unmount_with_operation(
          mount, G_MOUNT_UNMOUNT_NONE, operation, nullptr, finishUnmount, new MountCall{std::move(done)}
      );
    }
    g_object_unref(operation);
    g_object_unref(mount);
  }

} // namespace okaru::files
