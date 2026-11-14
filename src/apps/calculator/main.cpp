// OKARU: okaru-calc, la calculadora de OKARU.
#include "apps/calculator/calculator_window.h"
#include "apps/common/okaru_app.h"

#include <clocale>

int main() {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C"); // libqalculate y el UI esperan '.' internamente

  okaru::apps::OkaruApp app({
      .appId = "dev.okaru.Calculator",
      .dataName = "calculator",
      .titleKey = "apps.calc.title",
      .width = 440.0F,
      .height = 760.0F,
      .minWidth = 360.0F,
      .minHeight = 560.0F,
  });
  okaru::calc::CalculatorWindow window(app);
  return app.run();
}
