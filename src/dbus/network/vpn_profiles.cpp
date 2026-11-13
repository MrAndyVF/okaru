#include "dbus/network/vpn_profiles.h"

#include "core/deferred_call.h"
#include "core/process/process.h"
#include "dbus/network/inetwork_service.h"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace vpn {

  namespace {

    constexpr const char* kPluginDir = "/usr/lib/NetworkManager/VPN";

    std::string trim(std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
      }
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
      }
      return std::string(text);
    }

    std::string lower(std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }

    // Corre nmcli y entrega el resultado en el hilo principal.
    void nmcli(std::vector<std::string> args, std::function<void(const process::RunResult&)> done) {
      args.insert(args.begin(), "nmcli");
      auto callback = std::make_shared<std::function<void(const process::RunResult&)>>(std::move(done));
      const bool started = process::runAsync(
          args,
          {.onExit = [callback](
                         process::RunResult result
                     ) { DeferredCall::callLater([callback, result = std::move(result)] { (*callback)(result); }); }},
          {.timeout = std::chrono::seconds(30)}
      );
      if (!started) {
        (*callback)(process::RunResult{.exitCode = -1, .err = "nmcli"});
      }
    }

    std::string errorOf(const process::RunResult& result) {
      std::string text = trim(result.err.empty() ? result.out : result.err);
      if (text.starts_with("Error: ")) {
        text.erase(0, 7);
      }
      return text.empty() ? "nmcli" : text;
    }

  } // namespace

  std::vector<VpnType> installedTypes() {
    std::vector<VpnType> types;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kPluginDir, ec)) {
      if (entry.path().extension() != ".name") {
        continue;
      }
      std::ifstream in(entry.path());
      std::string line;
      VpnType type;
      bool section = false;
      while (std::getline(in, line)) {
        if (line.starts_with('[')) {
          section = line.starts_with("[VPN Connection]");
        } else if (section && line.starts_with("name=")) {
          type.name = trim(line.substr(5));
        } else if (section && line.starts_with("service=")) {
          type.service = trim(line.substr(8));
        }
      }
      if (!type.name.empty() && !type.service.empty()) {
        type.label = typeLabel(type.name);
        types.push_back(std::move(type));
      }
    }
    types.push_back({.name = "wireguard", .service = {}, .label = "WireGuard"});
    std::ranges::sort(types, {}, &VpnType::label);
    return types;
  }

  std::string typeLabel(std::string_view nameOrService) {
    std::string name(nameOrService);
    if (const auto dot = name.rfind('.'); dot != std::string::npos) {
      name = name.substr(dot + 1);
    }
    static const std::map<std::string, std::string> kLabels{
        {"fortisslvpn", "FortiSSL"},        {"openvpn", "OpenVPN"}, {"openconnect", "OpenConnect"},
        {"vpnc", "Cisco (vpnc)"},           {"l2tp", "L2TP/IPsec"}, {"pptp", "PPTP"},
        {"strongswan", "IPsec/IKEv2"},      {"sstp", "SSTP"},       {"wireguard", "WireGuard"},
        {"libreswan", "IPsec (Libreswan)"},
    };
    const auto it = kLabels.find(name);
    return it != kLabels.end() ? it->second : name;
  }

  std::vector<VpnField> fieldsFor(std::string_view type) {
    using K = VpnField::Kind;
    const auto text = [](const char* key, const char* label, bool required = false, const char* hint = "") {
      return VpnField{.key = key, .labelKey = label, .kind = K::Text, .required = required, .placeholder = hint};
    };
    const auto file = [](const char* key, const char* label) {
      return VpnField{.key = key, .labelKey = label, .kind = K::File};
    };
    if (type == "fortisslvpn") {
      return {
          text("gateway", "okaru.vpn.field.gateway", true, "vpn.empresa.com:443"), text("user", "okaru.vpn.field.user"),
          text("realm", "okaru.vpn.field.realm"), text("trusted-cert", "okaru.vpn.field.trusted-cert")
      };
    }
    if (type == "openvpn") {
      return {
          text("remote", "okaru.vpn.field.gateway", true, "vpn.empresa.com:1194"),
          VpnField{
              .key = "connection-type",
              .labelKey = "okaru.vpn.field.auth",
              .kind = K::Choice,
              .choices = {"tls", "password", "password-tls", "static-key"}
          },
          text("username", "okaru.vpn.field.user"),
          file("ca", "okaru.vpn.field.ca"),
          file("cert", "okaru.vpn.field.cert"),
          file("key", "okaru.vpn.field.key")
      };
    }
    if (type == "openconnect") {
      return {
          text("gateway", "okaru.vpn.field.gateway", true, "vpn.empresa.com"),
          VpnField{
              .key = "protocol",
              .labelKey = "okaru.vpn.field.protocol",
              .kind = K::Choice,
              .choices = {"anyconnect", "nc", "gp", "pulse", "fortinet", "f5", "array"}
          },
          file("usercert", "okaru.vpn.field.cert"), file("userkey", "okaru.vpn.field.key")
      };
    }
    if (type == "vpnc") {
      return {
          text("IPSec gateway", "okaru.vpn.field.gateway", true), text("IPSec ID", "okaru.vpn.field.group"),
          text("Xauth username", "okaru.vpn.field.user")
      };
    }
    if (type == "l2tp" || type == "pptp" || type == "sstp") {
      return {text("gateway", "okaru.vpn.field.gateway", true), text("user", "okaru.vpn.field.user")};
    }
    if (type == "strongswan") {
      return {
          text("address", "okaru.vpn.field.gateway", true),
          VpnField{
              .key = "method",
              .labelKey = "okaru.vpn.field.auth",
              .kind = K::Choice,
              .choices = {"eap", "key", "psk", "agent", "smartcard"}
          },
          text("user", "okaru.vpn.field.user"), file("certificate", "okaru.vpn.field.ca")
      };
    }
    if (type == "wireguard") {
      // No son claves de vpn.data: arman la configuración de wg-quick que se importa.
      return {
          text("PrivateKey", "okaru.vpn.field.private-key", true),
          text("Address", "okaru.vpn.field.address", true, "10.0.0.2/32"),
          text("DNS", "okaru.vpn.field.dns"),
          text("PublicKey", "okaru.vpn.field.peer-key", true),
          text("Endpoint", "okaru.vpn.field.gateway", true, "vpn.empresa.com:51820"),
          text("AllowedIPs", "okaru.vpn.field.allowed-ips", false, "0.0.0.0/0, ::/0")
      };
    }
    return {};
  }

  std::map<std::string, std::string> parseVpnData(std::string_view text) {
    std::map<std::string, std::string> data;
    std::string item;
    const auto flush = [&] {
      // "clave = valor": el primer " = " sin escapar separa.
      std::string key;
      std::string value;
      bool inValue = false;
      for (std::size_t i = 0; i < item.size(); ++i) {
        if (item[i] == '\\' && i + 1 < item.size()) {
          (inValue ? value : key) += item[++i];
        } else if (!inValue && item[i] == '=') {
          inValue = true;
        } else {
          (inValue ? value : key) += item[i];
        }
      }
      key = trim(key);
      if (!key.empty()) {
        data[key] = trim(value);
      }
      item.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size()) {
        item += text[i];
        item += text[++i];
      } else if (text[i] == ',') {
        flush();
      } else {
        item += text[i];
      }
    }
    flush();
    return data;
  }

  std::string formatVpnData(const std::map<std::string, std::string>& data) {
    const auto escape = [](const std::string& text) {
      std::string out;
      for (const char c : text) {
        if (c == ',' || c == '=' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      return out;
    };
    std::string out;
    for (const auto& [key, value] : data) {
      if (key.empty()) {
        continue;
      }
      out += (out.empty() ? "" : ",") + escape(key) + "=" + escape(value);
    }
    return out;
  }

  std::map<std::string, std::string> parseTerse(std::string_view text) {
    std::map<std::string, std::string> fields;
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t end = text.find('\n', start);
      if (end == std::string_view::npos) {
        end = text.size();
      }
      const std::string_view line = text.substr(start, end - start);
      start = end + 1;
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      std::string key(line.substr(0, colon));
      // IP4.ADDRESS[1], IP4.DNS[2]…: se juntan con ", ".
      if (const auto bracket = key.find('['); bracket != std::string::npos) {
        key.resize(bracket);
      }
      std::string value = trim(line.substr(colon + 1));
      auto& slot = fields[key];
      slot += (slot.empty() || value.empty() ? "" : ", ") + value;
    }
    return fields;
  }

  std::string_view importTypeFor(std::string_view path, std::string_view contents) {
    const std::string name = lower(std::string(path));
    if (name.ends_with(".ovpn")) {
      return "openvpn";
    }
    if (name.ends_with(".pcf")) {
      return "vpnc";
    }
    if (contents.contains("[Interface]") && contents.contains("PrivateKey")) {
      return "wireguard";
    }
    if (contents.contains("\nremote ") || contents.starts_with("remote ") || contents.contains("\nclient")) {
      return "openvpn";
    }
    return {};
  }

  std::string wireguardConfig(const std::map<std::string, std::string>& fields) {
    const auto get = [&](const char* key) {
      const auto it = fields.find(key);
      return it != fields.end() ? trim(it->second) : std::string{};
    };
    std::string out = "[Interface]\nPrivateKey = " + get("PrivateKey") + "\nAddress = " + get("Address") + "\n";
    if (const std::string dns = get("DNS"); !dns.empty()) {
      out += "DNS = " + dns + "\n";
    }
    const std::string allowed = get("AllowedIPs");
    out += "\n[Peer]\nPublicKey = "
        + get("PublicKey")
        + "\nEndpoint = "
        + get("Endpoint")
        + "\nAllowedIPs = "
        + (allowed.empty() ? "0.0.0.0/0, ::/0" : allowed)
        + "\nPersistentKeepalive = 25\n";
    return out;
  }

  bool toggleDefault(INetworkService& network, const std::string& preferredUuid) {
    const auto vpns = network.vpnConnections();
    bool any = false;
    for (const auto& v : vpns) {
      if (v.active) {
        any = true;
        (void)network.deactivateVpnConnection(v);
      }
    }
    if (any || vpns.empty()) {
      return true;
    }
    for (const auto& v : vpns) {
      if (v.uuid == preferredUuid || (preferredUuid.empty() && vpns.size() == 1)) {
        (void)network.activateVpnConnection(v);
        return true;
      }
    }
    return false;
  }

  void load(const std::string& uuid, std::function<void(bool ok, VpnProfile profile)> done) {
    nmcli(
        {"-t", "-f", "connection.id,connection.uuid,connection.type,vpn.service-type,vpn.data", "connection", "show",
         uuid},
        [uuid, done = std::move(done)](const process::RunResult& result) {
          VpnProfile profile;
          if (!result) {
            done(false, profile);
            return;
          }
          const auto fields = parseTerse(result.out);
          const auto get = [&](const char* key) {
            const auto it = fields.find(key);
            return it != fields.end() ? it->second : std::string{};
          };
          profile.uuid = uuid;
          profile.name = get("connection.id");
          const std::string service = get("vpn.service-type");
          profile.type = get("connection.type") == "wireguard" ? "wireguard"
              : service.empty()                                ? std::string{}
                                                               : service.substr(service.rfind('.') + 1);
          profile.data = parseVpnData(get("vpn.data"));
          done(true, std::move(profile));
        }
    );
  }

  void save(const VpnProfile& profile, Done done) {
    if (profile.type == "wireguard") {
      // WireGuard no es un plugin con vpn.data: se crea importando su configuración de wg-quick con ese nombre.
      // shortcut: editar uno existente lo reemplaza (borrar + importar); leer sus pares con nmcli no se puede.
      std::string name = profile.name;
      std::ranges::replace_if(
          name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-' && c != '_'; }, '-'
      );
      // El nombre de la interfaz sale del archivo (15 caracteres como mucho). Va con la clave privada: se crea ya con
      // permisos 600 en el directorio de ejecución del usuario (privado), y se borra al importar.
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const fs::path conf = fs::path(runtime != nullptr && runtime[0] != '\0' ? runtime : "/tmp")
          / ((name.empty() ? std::string("wg") : name.substr(0, 15)) + ".conf");
      const int fd = ::open(conf.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd < 0) {
        done(false, "open");
        return;
      }
      const std::string text = wireguardConfig(profile.data);
      const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
      ::close(fd);
      if (!written) {
        std::error_code ec;
        fs::remove(conf, ec);
        done(false, "write");
        return;
      }
      const auto importIt = [conf, profile, done](bool) {
        nmcli(
            {"connection", "import", "type", "wireguard", "file", conf.string()},
            [conf, profile, done](const process::RunResult& result) {
              std::error_code ec;
              fs::remove(conf, ec);
              if (!result) {
                done(false, errorOf(result));
                return;
              }
              // El perfil importado se llama como el archivo: se le pone el nombre elegido.
              const std::string out = result.out;
              const auto open = out.find('(');
              const auto close = out.find(')', open);
              if (open == std::string::npos || close == std::string::npos) {
                done(true, {});
                return;
              }
              nmcli(
                  {"connection", "modify", out.substr(open + 1, close - open - 1), "connection.id", profile.name},
                  [done](const process::RunResult& renamed) { done(static_cast<bool>(renamed), errorOf(renamed)); }
              );
            }
        );
      };
      if (profile.uuid.empty()) {
        importIt(true);
      } else {
        nmcli({"connection", "delete", "uuid", profile.uuid}, [importIt](const process::RunResult&) {
          importIt(true);
        });
      }
      return;
    }
    auto data = profile.data;
    // Sin guardar contraseñas: las pide el agente de OKARU al conectar (si el perfil no decía otra cosa).
    if (!data.contains("password-flags")) {
      data["password-flags"] = "1";
    }
    std::vector<std::string> args;
    if (profile.uuid.empty()) {
      args = {"connection", "add",      "type",       "vpn",      "con-name",
              profile.name, "vpn-type", profile.type, "vpn.data", formatVpnData(data)};
    } else {
      args = {"connection",    "modify",     "uuid",     profile.uuid,
              "connection.id", profile.name, "vpn.data", formatVpnData(data)};
    }
    nmcli(std::move(args), [done = std::move(done)](const process::RunResult& result) {
      done(static_cast<bool>(result), result ? std::string{} : errorOf(result));
    });
  }

  void importFile(const std::string& path, Done done) {
    std::ifstream in(path);
    std::stringstream contents;
    contents << in.rdbuf();
    const std::string type(importTypeFor(path, contents.str()));
    if (type.empty()) {
      done(false, "unknown-format");
      return;
    }
    nmcli(
        {"connection", "import", "type", type, "file", path},
        [done = std::move(done)](const process::RunResult& result) {
          done(static_cast<bool>(result), result ? std::string{} : errorOf(result));
        }
    );
  }

  void remove(const std::string& uuid, Done done) {
    nmcli({"connection", "delete", "uuid", uuid}, [done = std::move(done)](const process::RunResult& result) {
      done(static_cast<bool>(result), result ? std::string{} : errorOf(result));
    });
  }

  void details(const std::string& uuid, std::function<void(VpnDetails)> done) {
    nmcli(
        {"-t", "-f", "GENERAL.STATE,GENERAL.DEVICES,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,IP6.ADDRESS", "connection", "show",
         uuid},
        [done = std::move(done)](const process::RunResult& result) {
          VpnDetails details;
          if (result) {
            auto fields = parseTerse(result.out);
            details.state = fields["GENERAL.STATE"];
            details.device = fields["GENERAL.DEVICES"];
            details.ipv4 = fields["IP4.ADDRESS"];
            details.gateway = fields["IP4.GATEWAY"];
            details.dns = fields["IP4.DNS"];
            details.ipv6 = fields["IP6.ADDRESS"];
          }
          done(std::move(details));
        }
    );
  }

} // namespace vpn
