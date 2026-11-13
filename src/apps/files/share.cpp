#include "apps/files/share.h"

#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"

#include <chrono>
#include <gio/gio.h>
#include <sstream>
#include <thread>

namespace okaru::files {

  namespace {

    using namespace std::chrono_literals;

    std::vector<std::string> lines(const std::string& text) {
      std::vector<std::string> out;
      std::istringstream in(text);
      for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
          line.pop_back();
        }
        if (!line.empty()) {
          out.push_back(line);
        }
      }
      return out;
    }

    std::string firstCommand(std::initializer_list<const char*> names) {
      for (const char* name : names) {
        if (process::commandExists(name)) {
          return name;
        }
      }
      return {};
    }

    // Estado de una transferencia OBEX; vacío si ya no existe (obexd la quita al terminar).
    std::string transferStatus(GDBusConnection* bus, const char* path) {
      GVariant* reply = g_dbus_connection_call_sync(
          bus, "org.bluez.obex", path, "org.freedesktop.DBus.Properties", "Get",
          g_variant_new("(ss)", "org.bluez.obex.Transfer1", "Status"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
          5000, nullptr, nullptr
      );
      if (reply == nullptr) {
        return {};
      }
      GVariant* value = nullptr;
      g_variant_get(reply, "(v)", &value);
      std::string status = g_variant_get_string(value, nullptr);
      g_variant_unref(value);
      g_variant_unref(reply);
      return status;
    }

    bool sendBluetooth(const std::string& mac, const std::vector<std::string>& paths, std::string& error) {
      GError* gerror = nullptr;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &gerror);
      if (bus == nullptr) {
        error = gerror != nullptr ? gerror->message : "?";
        g_clear_error(&gerror);
        return false;
      }
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&options, "{sv}", "Target", g_variant_new_string("opp"));
      GVariant* reply = g_dbus_connection_call_sync(
          bus, "org.bluez.obex", "/org/bluez/obex", "org.bluez.obex.Client1", "CreateSession",
          g_variant_new("(sa{sv})", mac.c_str(), &options), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000,
          nullptr, &gerror
      );
      if (reply == nullptr) {
        error = gerror != nullptr ? gerror->message : "?";
        g_clear_error(&gerror);
        g_object_unref(bus);
        return false;
      }
      const char* sessionRaw = nullptr;
      g_variant_get(reply, "(&o)", &sessionRaw);
      const std::string session = sessionRaw;
      g_variant_unref(reply);

      bool ok = true;
      for (const auto& path : paths) {
        GVariant* sent = g_dbus_connection_call_sync(
            bus, "org.bluez.obex", session.c_str(), "org.bluez.obex.ObjectPush1", "SendFile",
            g_variant_new("(s)", path.c_str()), G_VARIANT_TYPE("(oa{sv})"), G_DBUS_CALL_FLAGS_NONE, 30000, nullptr,
            &gerror
        );
        if (sent == nullptr) {
          error = gerror != nullptr ? gerror->message : "?";
          g_clear_error(&gerror);
          ok = false;
          break;
        }
        const char* transferRaw = nullptr;
        g_variant_get(sent, "(&o@a{sv})", &transferRaw, nullptr);
        const std::string transfer = transferRaw;
        g_variant_unref(sent);
        // El teléfono tiene que aceptar: se espera hasta 10 minutos por archivo.
        const auto until = std::chrono::steady_clock::now() + 10min;
        for (;;) {
          const std::string status = transferStatus(bus, transfer.c_str());
          if (status.empty() || status == "complete") {
            break;
          }
          if (status == "error" || std::chrono::steady_clock::now() > until) {
            error = i18n::tr("apps.files.share.bluetooth-rejected");
            ok = false;
            break;
          }
          std::this_thread::sleep_for(300ms);
        }
        if (!ok) {
          break;
        }
      }
      GVariant* removed = g_dbus_connection_call_sync(
          bus, "org.bluez.obex", "/org/bluez/obex", "org.bluez.obex.Client1", "RemoveSession",
          g_variant_new("(o)", session.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr
      );
      if (removed != nullptr) {
        g_variant_unref(removed);
      }
      g_object_unref(bus);
      return ok;
    }

  } // namespace

  std::pair<std::string, std::string> parseBluetoothDevice(const std::string& line) {
    // bluetoothctl puede anteponer códigos de color o "[CHG]"; se busca "Device ".
    const auto at = line.find("Device ");
    if (at == std::string::npos) {
      return {};
    }
    const std::string rest = line.substr(at + 7);
    const auto space = rest.find(' ');
    const std::string mac = rest.substr(0, space);
    if (mac.size() != 17 || mac[2] != ':') {
      return {};
    }
    return {mac, space == std::string::npos ? mac : rest.substr(space + 1)};
  }

  std::vector<ShareTarget> discoverShareTargets() {
    using i18n::tr;
    std::vector<ShareTarget> out;
    if (!firstCommand({"localsend", "localsend_app"}).empty()) {
      out.push_back({.kind = ShareKind::LocalSend, .name = "LocalSend", .glyph = "send"});
    }
    if (process::commandExists("kdeconnect-cli")) {
      // Solo los teléfonos emparejados y a la vista ahora.
      const auto result = process::runSyncWithTimeout({"kdeconnect-cli", "-a", "--id-name-only"}, 3s);
      for (const auto& line : lines(result.out)) {
        const auto space = line.find(' ');
        if (space != std::string::npos) {
          out.push_back({.kind = ShareKind::KdeConnect, .id = line.substr(0, space),
                         .name = tr("apps.files.share.kdeconnect", "name", line.substr(space + 1)),
                         .glyph = "device-mobile"});
        }
      }
    }
    if (process::commandExists("bluetoothctl")) {
      const auto result = process::runSyncWithTimeout({"bluetoothctl", "devices", "Paired"}, 3s);
      for (const auto& line : lines(result.out)) {
        if (const auto [mac, name] = parseBluetoothDevice(line); !mac.empty()) {
          out.push_back({.kind = ShareKind::Bluetooth, .id = mac,
                         .name = tr("apps.files.share.bluetooth", "name", name), .glyph = "bluetooth"});
        }
      }
    }
    if (!firstCommand({"Telegram", "telegram-desktop"}).empty()) {
      out.push_back({.kind = ShareKind::Telegram, .name = "Telegram", .glyph = "brand-telegram"});
    }
    if (GAppInfo* mail = g_app_info_get_default_for_uri_scheme("mailto"); mail != nullptr) {
      out.push_back({.kind = ShareKind::Email, .name = tr("apps.files.share.email", "app", g_app_info_get_display_name(mail)),
                     .glyph = "mail"});
      g_object_unref(mail);
    }
    return out;
  }

  void shareFiles(const ShareTarget& target, std::vector<std::string> paths,
                  std::function<void(bool, std::string)> done) {
    std::thread([target, paths = std::move(paths), done = std::move(done)] {
      bool ok = true;
      std::string detail;
      const auto run = [&](const std::vector<std::string>& args) {
        const auto result = process::runSyncWithTimeout(args, 60s);
        if (!result) {
          ok = false;
          detail = result.err.empty() ? result.out : result.err;
        }
      };
      switch (target.kind) {
      case ShareKind::LocalSend: {
        // LocalSend abre su pantalla de envío con los archivos ya elegidos.
        std::vector<std::string> args{firstCommand({"localsend", "localsend_app"})};
        args.insert(args.end(), paths.begin(), paths.end());
        ok = process::runAsync(args);
        break;
      }
      case ShareKind::KdeConnect:
        for (const auto& path : paths) {
          run({"kdeconnect-cli", "-d", target.id, "--share", path});
        }
        break;
      case ShareKind::Bluetooth:
        ok = sendBluetooth(target.id, paths, detail);
        break;
      case ShareKind::Telegram: {
        std::vector<std::string> args{firstCommand({"Telegram", "telegram-desktop"})};
        for (const auto& path : paths) {
          args.insert(args.end(), {"-sendpath", path});
        }
        ok = process::runAsync(args);
        break;
      }
      case ShareKind::Email: {
        std::vector<std::string> args{"xdg-email"};
        for (const auto& path : paths) {
          args.insert(args.end(), {"--attach", path});
        }
        ok = process::runAsync(args);
        break;
      }
      }
      DeferredCall::callLater([done, ok, detail] { done(ok, detail); });
    }).detach();
  }

} // namespace okaru::files
