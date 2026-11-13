#pragma once

// OKARU: perfiles VPN de NetworkManager hechos desde OKARU (lo que antes hacía el plugin vpn-manager): protocolos
// instalados, leer y guardar los ajustes de un perfil, crear, importar, borrar y los detalles de una conexión activa.
// Todo va por nmcli (los resultados llegan en el hilo principal). Las contraseñas no se guardan aquí: el perfil las
// deja en manos del agente de secretos de OKARU (password-flags = 1), que las pide al conectar.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vpn {

  // Un protocolo instalado (un plugin de NetworkManager, o WireGuard que viene con NetworkManager).
  struct VpnType {
    std::string name;    // "fortisslvpn", "openvpn", "wireguard"…
    std::string service; // "org.freedesktop.NetworkManager.fortisslvpn"; vacío para WireGuard
    std::string label;   // "FortiSSL", "OpenVPN"…
  };

  // Un campo del formulario de un protocolo (una clave de vpn.data).
  struct VpnField {
    enum class Kind : std::uint8_t { Text, Choice, File, Bool };
    std::string key;      // clave en vpn.data
    std::string labelKey; // clave i18n
    Kind kind = Kind::Text;
    std::vector<std::string> choices;
    bool required = false;
    std::string placeholder;
  };

  struct VpnProfile {
    std::string uuid; // vacío = perfil nuevo
    std::string name;
    std::string type;                        // VpnType::name
    std::map<std::string, std::string> data; // vpn.data
  };

  struct VpnDetails {
    std::string state;
    std::string device;
    std::string ipv4;
    std::string gateway;
    std::string ipv6;
    std::string dns;
  };

  [[nodiscard]] std::vector<VpnType> installedTypes();
  [[nodiscard]] std::string typeLabel(std::string_view nameOrService);
  [[nodiscard]] std::vector<VpnField> fieldsFor(std::string_view type);

  // vpn.data de nmcli ("clave = valor, otra = valor", con \, y \= escapados) ↔ mapa; format* es lo que acepta
  // `nmcli connection modify … vpn.data`.
  [[nodiscard]] std::map<std::string, std::string> parseVpnData(std::string_view text);
  [[nodiscard]] std::string formatVpnData(const std::map<std::string, std::string>& data);
  // Salida de `nmcli -t -f …` (una línea "CAMPO:valor" por campo) → mapa campo → valor.
  [[nodiscard]] std::map<std::string, std::string> parseTerse(std::string_view text);
  // Protocolo para `nmcli connection import` según la extensión y el contenido; vacío si no se reconoce.
  [[nodiscard]] std::string importTypeFor(std::string_view path, std::string_view contents);
  // Configuración de wg-quick con los campos del formulario de WireGuard (para crear importándola).
  [[nodiscard]] std::string wireguardConfig(const std::map<std::string, std::string>& fields);

  using Done = std::function<void(bool ok, std::string error)>;
  void load(const std::string& uuid, std::function<void(bool ok, VpnProfile profile)> done);
  void save(const VpnProfile& profile, Done done);
  void importFile(const std::string& path, Done done);
  void remove(const std::string& uuid, Done done);
  void details(const std::string& uuid, std::function<void(VpnDetails)> done);

} // namespace vpn
