#include "system/okaru_geo.h"

#include "util/string_utils.h"

#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>

namespace okaru::geo {

  namespace {
    std::string str(const nlohmann::json& json, const char* key) {
      const auto it = json.find(key);
      return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
    }

    bool validCoordinates(double lat, double lng) {
      return std::isfinite(lat) && std::isfinite(lng) && std::abs(lat) <= 90.0 && std::abs(lng) <= 180.0;
    }
  } // namespace

  std::string geocodeUrl(std::string_view city) {
    return "https://geocoding-api.open-meteo.com/v1/search?count=1&format=json&name="
        + StringUtils::urlEncode(std::string(city));
  }

  std::optional<Place> parseIpinfo(const nlohmann::json& json) {
    if (!json.is_object()) {
      return std::nullopt;
    }
    const std::string loc = str(json, "loc");
    const auto comma = loc.find(',');
    if (comma == std::string::npos) {
      return std::nullopt;
    }
    Place place;
    const char* begin = loc.data();
    if (std::from_chars(begin, begin + comma, place.latitude).ec != std::errc{}
        || std::from_chars(begin + comma + 1, begin + loc.size(), place.longitude).ec != std::errc{}
        || !validCoordinates(place.latitude, place.longitude)) {
      return std::nullopt;
    }
    place.name = str(json, "city");
    place.country = str(json, "country");
    return place;
  }

  std::optional<Place> parseOpenMeteoGeocode(const nlohmann::json& json) {
    if (!json.is_object()) {
      return std::nullopt;
    }
    const auto results = json.find("results");
    if (results == json.end() || !results->is_array() || results->empty()) {
      return std::nullopt;
    }
    const auto& first = results->front();
    const auto lat = first.find("latitude");
    const auto lng = first.find("longitude");
    if (lat == first.end() || lng == first.end() || !lat->is_number() || !lng->is_number()) {
      return std::nullopt;
    }
    Place place{lat->get<double>(), lng->get<double>(), str(first, "name"), str(first, "country")};
    if (!validCoordinates(place.latitude, place.longitude)) {
      return std::nullopt;
    }
    return place;
  }

} // namespace okaru::geo
