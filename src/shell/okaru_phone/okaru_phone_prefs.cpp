#include "shell/okaru_phone/okaru_phone_prefs.h"

#include "core/log.h"
#include "dbus/kdeconnect/kdeconnect_service.h"
#include "util/file_utils.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

namespace okaru::phone {

  namespace {

    constexpr Logger kLog("phone");

    std::map<std::string, std::string> stringMap(const nlohmann::json& json, const char* key, const char* legacyKey) {
      std::map<std::string, std::string> out;
      const auto it = json.contains(key) ? json.find(key) : json.find(legacyKey);
      if (it == json.end() || !it->is_object()) {
        return out;
      }
      for (const auto& [id, value] : it->items()) {
        if (value.is_string() && !value.get<std::string>().empty()) {
          out[id] = value.get<std::string>();
        }
      }
      return out;
    }

    std::string prefsPath() {
      const std::string dir = FileUtils::stateDir();
      return (dir.empty() ? std::string{"."} : dir) + "/phone.json";
    }

    std::optional<nlohmann::json> readJson(const std::string& path) {
      std::ifstream in(path);
      if (!in) {
        return std::nullopt;
      }
      try {
        return nlohmann::json::parse(in);
      } catch (const nlohmann::json::exception& e) {
        kLog.warn("{}: {}", path, e.what());
        return std::nullopt;
      }
    }

    Prefs load() {
      const std::string path = prefsPath();
      if (auto json = readJson(path); json && json->is_object()) {
        return parsePrefs(*json);
      }
      // Primera vez: se importa lo que el usuario configuró en el módulo Luau.
      const std::string dir = FileUtils::stateDir();
      for (const char* legacy : {"/plugins/data/okaru/phone-connect/state.json",
                                 "/plugins/data/icefish/phone-connect/state.json"}) {
        if (auto json = readJson(dir + legacy); json && json->is_object()) {
          kLog.info("imported phone preferences from {}", legacy);
          Prefs prefs = parsePrefs(*json);
          std::ofstream(path) << toJson(prefs).dump(2);
          return prefs;
        }
      }
      return {};
    }

  } // namespace

  Prefs parsePrefs(const nlohmann::json& json) {
    Prefs prefs;
    if (const auto it = json.find("selected"); it != json.end() && it->is_string()) {
      prefs.selected = it->get<std::string>();
    }
    prefs.alias = stringMap(json, "alias", "aliasMap");
    prefs.avatar = stringMap(json, "avatar", "imageMap");
    return prefs;
  }

  nlohmann::json toJson(const Prefs& prefs) {
    return {{"selected", prefs.selected}, {"alias", prefs.alias}, {"avatar", prefs.avatar}};
  }

  Prefs& prefs() {
    static Prefs instance = load();
    return instance;
  }

  void save() {
    const std::string path = prefsPath();
    const std::string tmp = path + ".tmp";
    {
      std::ofstream out(tmp);
      if (!out) {
        kLog.warn("cannot write {}", tmp);
        return;
      }
      out << toJson(prefs()).dump(2);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      kLog.warn("cannot save {}: {}", path, ec.message());
    }
  }

  std::string displayName(const PhoneDevice& device) {
    const auto& alias = prefs().alias;
    if (const auto it = alias.find(device.id); it != alias.end()) {
      return it->second;
    }
    return device.name.empty() ? device.id : device.name;
  }

  std::string avatarPath(const PhoneDevice& device) {
    const auto& avatar = prefs().avatar;
    const auto it = avatar.find(device.id);
    return it != avatar.end() ? it->second : std::string{};
  }

  bool usable(const PhoneDevice& device) { return device.paired && device.reachable; }

  const PhoneDevice* pickDevice(const std::vector<PhoneDevice>& devices, const std::string& selected) {
    const PhoneDevice* firstPaired = nullptr;
    const PhoneDevice* firstUsable = nullptr;
    for (const auto& d : devices) {
      if (d.id == selected) {
        return &d;
      }
      if (firstUsable == nullptr && usable(d)) {
        firstUsable = &d;
      }
      if (firstPaired == nullptr && d.paired) {
        firstPaired = &d;
      }
    }
    if (firstUsable != nullptr) {
      return firstUsable;
    }
    if (firstPaired != nullptr) {
      return firstPaired;
    }
    return devices.empty() ? nullptr : &devices.front();
  }

  const PhoneDevice* selectedDevice(const KdeConnectService& service) {
    return pickDevice(service.devices(), prefs().selected);
  }

  const char* deviceGlyph(const PhoneDevice& device) {
    if (!usable(device)) {
      return "device-mobile-off";
    }
    if (device.type == "tablet") {
      return "device-tablet";
    }
    if (device.type == "laptop") {
      return "device-laptop";
    }
    if (device.type == "desktop") {
      return "device-desktop";
    }
    if (device.type == "tv") {
      return "device-tv";
    }
    return "device-mobile";
  }

  const char* batteryGlyph(int charge, bool charging) {
    if (charging) {
      return "battery-charging";
    }
    if (charge >= 90) {
      return "battery-4";
    }
    if (charge >= 60) {
      return "battery-3";
    }
    if (charge >= 30) {
      return "battery-2";
    }
    if (charge > 0) {
      return "battery-1";
    }
    return "battery";
  }

  const char* signalGlyph(int strength) {
    if (strength >= 3) {
      return "antenna-bars-5";
    }
    if (strength >= 1) {
      return "antenna-bars-3";
    }
    return "antenna-bars-1";
  }

} // namespace okaru::phone
