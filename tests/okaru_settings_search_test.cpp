// OKARU: coincidencia de la búsqueda en las páginas propias.
#include "shell/settings/okaru_settings_search.h"

#include <cstdlib>
#include <print>

using settings::OkaruSearchItem;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_settings_search_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  const OkaruSearchItem primary{
      .section = "displays", .label = "Monitor principal", .detail = "Recibe el cursor", .keywords = "primary"
  };
  ok = expect(settings::okaruSearchMatches(primary, "monitor principal"), "en el idioma de la interfaz") && ok;
  ok = expect(settings::okaruSearchMatches(primary, "Primary Monitor"), "en inglés con la palabra clave") && ok;
  ok = expect(settings::okaruSearchMatches(primary, "cursor"), "en el detalle") && ok;
  ok = expect(!settings::okaruSearchMatches(primary, "monitor dock"), "todas las palabras deben aparecer") && ok;
  ok = expect(!settings::okaruSearchMatches(primary, "   "), "consulta vacía no coincide") && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
