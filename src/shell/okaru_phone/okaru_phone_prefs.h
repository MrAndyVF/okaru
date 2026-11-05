#pragma once

// OKARU: preferencias del teléfono (alias, foto y dispositivo elegido) y utilidades compartidas por el
// widget de la barra, el panel okaru-phone y la tarjeta del centro de control.
// Se guardan en <state>/phone.json; la primera vez se importan del state.json del módulo Luau phone-connect.

#include <map>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct PhoneDevice;
class KdeConnectService;

namespace okaru::phone {

  struct Prefs {
    std::string selected;
    std::map<std::string, std::string> alias;  // id -> nombre propio
    std::map<std::string, std::string> avatar; // id -> ruta de imagen
    bool operator==(const Prefs&) const = default;
  };

  // phone.json propio ({selected, alias, avatar}) o el state.json viejo ({selected, aliasMap, imageMap}).
  [[nodiscard]] Prefs parsePrefs(const nlohmann::json& json);
  [[nodiscard]] nlohmann::json toJson(const Prefs& prefs);

  // Cargadas al primer uso; save() escribe phone.json.
  Prefs& prefs();
  void save();

  [[nodiscard]] std::string displayName(const PhoneDevice& device);
  [[nodiscard]] std::string avatarPath(const PhoneDevice& device);
  // El elegido si sigue existiendo; si no, el primero conectado, luego el primero emparejado, luego el primero.
  [[nodiscard]] const PhoneDevice* pickDevice(const std::vector<PhoneDevice>& devices, const std::string& selected);
  [[nodiscard]] const PhoneDevice* selectedDevice(const KdeConnectService& service);

  [[nodiscard]] bool usable(const PhoneDevice& device); // emparejado y alcanzable
  [[nodiscard]] const char* deviceGlyph(const PhoneDevice& device);
  [[nodiscard]] const char* batteryGlyph(int charge, bool charging);
  [[nodiscard]] const char* signalGlyph(int strength);

} // namespace okaru::phone
