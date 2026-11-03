// OKARU: lectura de las respuestas de ipinfo.io y de la geocodificación de Open-Meteo.
#include "system/okaru_geo.h"

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <print>

using okaru::geo::parseIpinfo;
using okaru::geo::parseOpenMeteoGeocode;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_geo_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  const auto ip = parseIpinfo(nlohmann::json::parse(R"({"city":"Bogotá","country":"CO","loc":"4.6097,-74.0817"})"));
  ok &= expect(ip.has_value() && ip->latitude > 4.6 && ip->longitude < -74.0 && ip->name == "Bogotá", "ipinfo");
  ok &= expect(!parseIpinfo(nlohmann::json::parse(R"({"loc":"nope"})")).has_value(), "loc inválido");
  ok &= expect(!parseIpinfo(nlohmann::json::parse(R"({"loc":"95.0,10.0"})")).has_value(), "latitud fuera de rango");
  ok &= expect(!parseIpinfo(nlohmann::json::parse(R"({"bogon":true})")).has_value(), "sin loc");

  const auto city = parseOpenMeteoGeocode(nlohmann::json::parse(
      R"({"results":[{"name":"Medellín","latitude":6.25,"longitude":-75.56,"country":"Colombia"}]})"
  ));
  ok &= expect(city.has_value() && city->name == "Medellín" && city->country == "Colombia", "open-meteo");
  ok &= expect(!parseOpenMeteoGeocode(nlohmann::json::parse(R"({"generationtime_ms":0.5})")).has_value(), "sin resultados");
  ok &= expect(!parseOpenMeteoGeocode(nlohmann::json::parse(R"({"results":[]})")).has_value(), "lista vacía");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
