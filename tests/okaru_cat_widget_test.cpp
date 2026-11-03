// OKARU: paso del gato según la CPU (dormido / camina / corre y velocidad).
#include "shell/bar/widgets/okaru_cat_widget.h"

#include <cstdlib>
#include <print>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_cat_widget_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  const auto pace = [](double cpu) { return OkaruCatWidget::paceFor(cpu, 15, 60); };
  ok &= expect(pace(5).mode == 0, "bajo el umbral duerme");
  ok &= expect(pace(15).mode == 1 && pace(15).frameMs == 380.0F, "al umbral camina lento");
  ok &= expect(pace(59).mode == 1 && pace(59).frameMs < 190.0F, "cerca de correr camina rápido");
  ok &= expect(pace(60).mode == 2 && pace(60).frameMs == 160.0F, "al umbral corre");
  ok &= expect(pace(100).frameMs == 55.0F, "al 100 % corre al máximo");
  ok &= expect(OkaruCatWidget::paceFor(40.5, 40, 30).mode == 1 && OkaruCatWidget::paceFor(41, 40, 30).mode == 2,
               "run menor que walk se corrige a walk+1");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
