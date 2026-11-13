#include "apps/files/phones.h"

#include <algorithm>
#include <gio/gio.h>

namespace okaru::files {

  namespace {

    constexpr const char* kService = "org.kde.kdeconnect";

    GDBusConnection* sessionBus() { return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr); }

    // Sin arrancar el demonio si no corre, y con tope de tiempo: nunca deja colgado al explorador.
    GVariant* call(GDBusConnection* bus, const std::string& path, const char* interface, const char* method,
                   GVariant* args, const char* reply, int timeoutMs, std::string* error = nullptr) {
      GError* gerror = nullptr;
      GVariant* result = g_dbus_connection_call_sync(
          bus, kService, path.c_str(), interface, method, args, reply != nullptr ? G_VARIANT_TYPE(reply) : nullptr,
          G_DBUS_CALL_FLAGS_NO_AUTO_START, timeoutMs, nullptr, &gerror
      );
      if (gerror != nullptr) {
        if (error != nullptr) {
          *error = gerror->message;
        }
        g_error_free(gerror);
      }
      return result;
    }

    std::string devicePath(const std::string& id) { return "/modules/kdeconnect/devices/" + id; }

    std::vector<std::pair<std::string, std::string>> storages(GDBusConnection* bus, const std::string& id) {
      std::vector<std::pair<std::string, std::string>> out;
      GVariant* reply = call(bus, devicePath(id) + "/sftp", "org.kde.kdeconnect.device.sftp", "getDirectories",
                             nullptr, "(a{sv})", 3000);
      if (reply == nullptr) {
        return out;
      }
      GVariantIter* iter = nullptr;
      g_variant_get(reply, "(a{sv})", &iter);
      const char* path = nullptr;
      GVariant* value = nullptr;
      while (g_variant_iter_next(iter, "{&sv}", &path, &value)) {
        const char* name = g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ? g_variant_get_string(value, nullptr) : "";
        out.emplace_back(path, name);
        g_variant_unref(value);
      }
      g_variant_iter_free(iter);
      g_variant_unref(reply);
      // El almacenamiento interno primero (las tarjetas SD después).
      std::ranges::stable_sort(out, [](const auto& a, const auto& b) {
        return a.first.contains("/emulated/") && !b.first.contains("/emulated/");
      });
      return out;
    }

  } // namespace

  std::vector<Phone> listPhones() {
    std::vector<Phone> out;
    GDBusConnection* bus = sessionBus();
    if (bus == nullptr) {
      return out;
    }
    GVariant* reply = call(bus, "/modules/kdeconnect", "org.kde.kdeconnect.daemon", "devices",
                           g_variant_new("(bb)", TRUE, TRUE), "(as)", 1500);
    if (reply != nullptr) {
      GVariantIter* iter = nullptr;
      g_variant_get(reply, "(as)", &iter);
      const char* id = nullptr;
      while (g_variant_iter_next(iter, "&s", &id)) {
        Phone phone{.id = id};
        // Solo los que tienen el plugin sftp activo.
        GVariant* loaded = call(bus, devicePath(id), "org.kde.kdeconnect.device", "hasPlugin",
                                g_variant_new("(s)", "kdeconnect_sftp"), "(b)", 1500);
        bool hasSftp = false;
        if (loaded != nullptr) {
          gboolean value = FALSE;
          g_variant_get(loaded, "(b)", &value);
          hasSftp = value != FALSE;
          g_variant_unref(loaded);
        }
        if (!hasSftp) {
          continue;
        }
        GVariant* name = call(bus, devicePath(id), "org.freedesktop.DBus.Properties", "Get",
                              g_variant_new("(ss)", "org.kde.kdeconnect.device", "name"), "(v)", 1500);
        if (name != nullptr) {
          GVariant* inner = nullptr;
          g_variant_get(name, "(v)", &inner);
          phone.name = g_variant_get_string(inner, nullptr);
          g_variant_unref(inner);
          g_variant_unref(name);
        }
        GVariant* mounted = call(bus, devicePath(id) + "/sftp", "org.kde.kdeconnect.device.sftp", "isMounted",
                                 nullptr, "(b)", 1500);
        if (mounted != nullptr) {
          gboolean value = FALSE;
          g_variant_get(mounted, "(b)", &value);
          phone.mounted = value != FALSE;
          g_variant_unref(mounted);
        }
        if (phone.mounted) {
          phone.storages = storages(bus, id);
        }
        out.push_back(std::move(phone));
      }
      g_variant_iter_free(iter);
      g_variant_unref(reply);
    }
    g_object_unref(bus);
    return out;
  }

  bool mountPhone(const std::string& id, std::string& root, std::string& error) {
    GDBusConnection* bus = sessionBus();
    if (bus == nullptr) {
      error = "D-Bus";
      return false;
    }
    bool ok = false;
    GVariant* reply = call(bus, devicePath(id) + "/sftp", "org.kde.kdeconnect.device.sftp", "mountAndWait", nullptr,
                           "(b)", 30000, &error);
    if (reply != nullptr) {
      gboolean value = FALSE;
      g_variant_get(reply, "(b)", &value);
      ok = value != FALSE;
      g_variant_unref(reply);
    }
    if (!ok && error.empty()) {
      // El motivo que da KDE Connect (teléfono bloqueado, sshfs ausente…).
      if (GVariant* why = call(bus, devicePath(id) + "/sftp", "org.kde.kdeconnect.device.sftp", "getMountError",
                               nullptr, "(s)", 3000);
          why != nullptr) {
        const char* text = nullptr;
        g_variant_get(why, "(&s)", &text);
        error = text != nullptr ? text : "";
        g_variant_unref(why);
      }
    }
    if (ok) {
      const auto found = storages(bus, id);
      if (!found.empty()) {
        root = found.front().first;
      } else if (GVariant* point = call(bus, devicePath(id) + "/sftp", "org.kde.kdeconnect.device.sftp", "mountPoint",
                                        nullptr, "(s)", 3000);
                 point != nullptr) {
        const char* text = nullptr;
        g_variant_get(point, "(&s)", &text);
        root = text != nullptr ? text : "";
        g_variant_unref(point);
      }
    }
    g_object_unref(bus);
    return ok && !root.empty();
  }

  void unmountPhone(const std::string& id) {
    GDBusConnection* bus = sessionBus();
    if (bus == nullptr) {
      return;
    }
    if (GVariant* reply = call(bus, devicePath(id) + "/sftp", "org.kde.kdeconnect.device.sftp", "unmount", nullptr,
                               nullptr, 10000);
        reply != nullptr) {
      g_variant_unref(reply);
    }
    g_object_unref(bus);
  }

} // namespace okaru::files
