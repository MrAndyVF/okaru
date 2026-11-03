// OKARU: alineación de los menús flotantes con su widget.
#include "shell/panel/okaru_menu_align.h"

#include <cstdlib>
#include <print>

using namespace okaru::panel;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_menu_align_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  // Widget de 1000 a 1040 en una pantalla de 1920, menú de 400, margen 8.
  ok = expect(alignedMenuStart(1000, 1040, 400, 1920, 8, MenuAlign::Start) == 1000, "izquierda") && ok;
  ok = expect(alignedMenuStart(1000, 1040, 400, 1920, 8, MenuAlign::End) == 640, "derecha") && ok;
  ok = expect(alignedMenuStart(1000, 1040, 400, 1920, 8, MenuAlign::Center) == 820, "centro") && ok;
  // Cerca de los bordes no se sale de la pantalla.
  ok = expect(alignedMenuStart(1880, 1910, 400, 1920, 8, MenuAlign::Start) == 1512, "no se sale por la derecha") && ok;
  ok = expect(alignedMenuStart(4, 30, 400, 1920, 8, MenuAlign::End) == 8, "no se sale por la izquierda") && ok;
  // Menú más grande que la pantalla: queda pegado al margen.
  ok = expect(alignedMenuStart(100, 140, 3000, 1920, 8, MenuAlign::Center) == 8, "menú enorme") && ok;
  ok = expect(menuAlignFromString("end") == MenuAlign::End && menuAlignFromString("x") == MenuAlign::Center, "texto")
      && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
