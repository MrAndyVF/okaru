#pragma once

// OKARU: ubicación sin noctalia.dev. Por IP con ipinfo.io y por nombre de ciudad con la geocodificación de
// Open-Meteo (el mismo proveedor del clima). Gratis, HTTPS y sin clave.

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace okaru::geo {

  inline constexpr std::string_view kIpLocateUrl = "https://ipinfo.io/json";

  struct Place {
    double latitude = 0.0;
    double longitude = 0.0;
    std::string name;
    std::string country;
  };

  [[nodiscard]] std::string geocodeUrl(std::string_view city);
  // {"city": "...", "country": "CO", "loc": "4.61,-74.08"}
  [[nodiscard]] std::optional<Place> parseIpinfo(const nlohmann::json& json);
  // {"results": [{"name": "...", "latitude": 4.6, "longitude": -74.1, "country": "..."}]}
  [[nodiscard]] std::optional<Place> parseOpenMeteoGeocode(const nlohmann::json& json);

} // namespace okaru::geo
