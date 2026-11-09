// OKARU: motor y estado de okaru-calc.
#include "apps/calculator/calc_engine.h"
#include "apps/calculator/calc_state.h"

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <unistd.h>

using namespace okaru::calc;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_calc_engine_test: {}", message);
    }
    return condition;
  }

  bool expectText(const Result& result, std::string_view expected, const char* message) {
    if (!result.ok || result.text != expected) {
      std::println(stderr, "okaru_calc_engine_test: {} (got ok={} '{}')", message, result.ok, result.text);
      return false;
    }
    return true;
  }
} // namespace

int main() {
  std::setlocale(LC_ALL, "C");
  // libqalculate lee definiciones del usuario: un HOME aislado evita depender de la máquina.
  const auto home = std::filesystem::temp_directory_path() / ("okaru-calc-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(home);
  ::setenv("HOME", home.c_str(), 1);
  ::setenv("XDG_CONFIG_HOME", (home / "config").c_str(), 1);
  ::setenv("XDG_DATA_HOME", (home / "data").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (home / "state").c_str(), 1);

  bool ok = true;
  Engine engine;
  Settings settings;

  ok = expectText(engine.evaluate("2+3*4", settings), "14", "precedencia") && ok;
  ok = expectText(engine.evaluate("ans/7", settings), "2", "ans guarda el último resultado") && ok;
  ok = expectText(engine.evaluate("1/3", settings), "0.333333333", "nueve decimales por defecto") && ok;
  ok = expectText(engine.evaluate("sin(90)", settings), "1", "grados por defecto") && ok;
  Settings radians = settings;
  radians.angle = AngleUnit::Radians;
  ok = expectText(engine.evaluate("cos(0)", radians), "1", "radianes") && ok;
  ok = expect(!engine.evaluate("sin(", settings).ok, "error: faltan argumentos") && ok;
  const auto lenient = engine.evaluate("2+*", settings);
  ok = expect(lenient.ok && lenient.text == "2" && !lenient.warning.empty(), "aviso junto al resultado") && ok;
  ok = expect(!engine.preview("42", settings).has_value(), "un número suelto no tiene vista previa") && ok;
  ok = expect(engine.preview("6*7", settings) == "42", "vista previa de un cálculo") && ok;

  // Variables al estilo GNOME.
  ok = expectText(engine.evaluate("x = 6", settings), "6", "asignación") && ok;
  ok = expectText(engine.evaluate("x*7", settings), "42", "usa la variable") && ok;
  ok = expect(engine.evaluate("pi = 3", settings).badVariable, "no pisa constantes") && ok;
  ok = expect(engine.variables().size() == 1 && engine.variables()[0].first == "x", "lista de variables") && ok;
  engine.removeVariable("x");
  ok = expect(engine.variables().empty() && engine.evaluate("x*7", settings).text != "42", "variable borrada") && ok;

  // Programación.
  Settings hex = settings;
  hex.base = 16;
  hex.wordSize = 8;
  ok = expectText(engine.evaluate("FF+1", hex), "100", "entrada y salida en hexadecimal") && ok;
  (void)engine.evaluate("10", settings);
  const auto bases = engine.lastBases(hex);
  ok = expect(bases.has_value() && bases->bits == 10 && bases->hex == "A", "cuatro bases y bits") && ok;
  ok = expect(engine.formatInteger(255, hex) == "FF", "entero en la base de entrada") && ok;
  (void)engine.evaluate("0.5", settings);
  ok = expect(!engine.lastBases(hex).has_value(), "un decimal no tiene bases") && ok;

  // Conversión y finanzas.
  const auto length = engine.defaultUnitCategory();
  ok =
      expectText(engine.convert("2", length, "Kilometer", "Meter", settings), "2000 m", "conversión de unidades") && ok;
  ok = expectText(engine.convert("1", length, "Kilometer", "Centimeter", settings), "100000 cm", "unidad con prefijo")
      && ok;
  std::string currencies;
  for (const auto& category : engine.unitCategories()) {
    if (engine.isCurrencyCategory(category)) {
      currencies = category;
    }
  }
  ok = expect(
           std::ranges::contains(engine.units(currencies), std::string("Colombian Peso"), &UnitInfo::title),
           "el peso colombiano está entre las divisas"
       )
      && ok;
  const auto pesos = engine.convert("1", currencies, "U.S. Dollar", "Colombian Peso", settings);
  ok = expect(pesos.ok && pesos.text.starts_with("COP "), "dólares a pesos colombianos") && ok;
  ok = expectText(engine.financial(Financial::Sln, {"1000", "100", "9"}, settings), "100", "sln") && ok;
  ok = expectText(engine.financial(Financial::Gpm, {"100", "0.2"}, settings), "125", "gpm") && ok;
  ok = expect(!engine.financial(Financial::Pmt, {"1000", "", "12"}, settings).ok, "campo vacío") && ok;
  ok = expect(financialFields(Financial::Syd).size() == 4, "syd tiene cuatro campos") && ok;

  // Estado.
  State state;
  state.mode = Mode::Programming;
  state.settings = hex;
  state.variables = {{"tasa", "0.05"}};
  for (int i = 0; i < 105; ++i) {
    pushHistory(state, {.expression = std::to_string(i), .result = std::to_string(i)});
  }
  pushHistory(state, state.history.back());
  ok = expect(state.history.size() == kHistoryLimit && state.history.front().expression == "5", "límite del historial")
      && ok;
  const auto path = home / "calculator.toml";
  ok = expect(saveState(state, path), "guarda el estado") && ok;
  ok = expect(loadState(path) == state, "el estado vuelve igual") && ok;
  ok = expect(loadState(home / "no-existe.toml") == State{}, "sin archivo, valores de fábrica") && ok;

  std::filesystem::remove_all(home);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
