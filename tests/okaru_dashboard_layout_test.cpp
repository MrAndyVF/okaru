// OKARU: tamaños y reparto en filas del centro de control.
#include "shell/okaru_dashboard/okaru_dashboard_layout.h"

#include <cstdlib>
#include <print>
#include <vector>

using namespace okaru::dashboard;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_dashboard_layout_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  using enum CardSize;
  // L | M M | S S S S | S M  (el M que no cabe tras 3 S salta de fila)
  const std::vector<CardSize> sizes{Large, Medium, Medium, Small, Small, Small, Small, Small, Small, Small, Medium};
  const auto rows = packRows(sizes);
  ok &= expect(rows.size() == 5, "número de filas");
  ok &= expect(rows[0] == std::vector<std::size_t>{0}, "la grande ocupa la fila entera");
  ok &= expect(rows[1] == std::vector<std::size_t>{1, 2}, "dos medianas por fila");
  ok &= expect(rows[3] == std::vector<std::size_t>{7, 8, 9}, "tres pequeñas y la mediana no cabe");
  ok &= expect(rows[4] == std::vector<std::size_t>{10}, "la mediana empieza fila");
  ok &= expect(packRows(std::vector<CardSize>{}).empty(), "sin tarjetas no hay filas");

  ok &= expect(normalizeSize("profile", "small") == Medium, "perfil no admite pequeña");
  ok &= expect(normalizeSize("clock", "large") == Large, "reloj admite grande");
  ok &= expect(normalizeSize("shortcut:wifi", "large") == Small, "acceso directo sin grande");
  ok &= expect(normalizeSize("nope", "large") == Small, "tipo desconocido");
  ok &= expect(nextSize("clock", Large) == Small, "ciclo vuelve al inicio");
  ok &= expect(nextSize("volume", Medium) == Large, "ciclo avanza");
  ok &= expect(defaultSize("network") == Medium && defaultSize("shortcut:x") == Medium, "mediano por defecto");
  ok &= expect(findKind("shortcut:") == nullptr, "prefijo vacío no es acceso directo");
  ok &= expect(shortcutId("shortcut:icefish/phone-connect:tile") == "icefish/phone-connect:tile", "id con ':'");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
