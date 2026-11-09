#pragma once

// OKARU: motor de okaru-calc sobre libqalculate. Sin UI: evalúa, guarda `ans` y variables del usuario, muestra el
// resultado en las cuatro bases, convierte unidades/divisas y calcula las funciones financieras de GNOME Calculator.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Calculator;
class KnownVariable;
class MathStructure;

namespace okaru::calc {

  enum class AngleUnit : std::uint8_t { Degrees, Radians, Gradians };
  enum class NumberFormat : std::uint8_t { Automatic, Fixed, Scientific, Engineering };

  struct Settings {
    AngleUnit angle = AngleUnit::Degrees;
    NumberFormat format = NumberFormat::Automatic;
    int decimals = 9;
    bool trailingZeros = false;
    bool thousandsSeparators = false;
    int base = 10;     // modo programación: base de entrada y de salida (2, 8, 10, 16)
    int wordSize = 64; // modo programación: 8, 16, 32 o 64 bits

    bool operator==(const Settings&) const = default;
  };

  struct Result {
    bool ok = false;
    std::string text;         // resultado listo para mostrar, o el mensaje de error
    std::string input;        // expresión tal como la entendió libqalculate (vacía si falló)
    std::string warning;      // aviso de libqalculate con un resultado válido ("operador ignorado", …)
    bool badVariable = false; // "nombre = …" con un nombre que no se puede usar (text = el nombre)
  };

  struct BaseValues {
    std::string bin;
    std::string oct;
    std::string dec;
    std::string hex;
    std::uint64_t bits = 0; // valor entero recortado a wordSize, para el panel de bits
  };

  struct UnitInfo {
    std::string title; // nombre para mostrar; identifica la unidad dentro de su categoría
  };

  // Funciones financieras de GNOME Calculator, con sus mismas fórmulas.
  enum class Financial : std::uint8_t { Ctrm, Ddb, Fv, Gpm, Pmt, Pv, Rate, Sln, Syd, Term };
  inline constexpr std::size_t kFinancialCount = 10;
  // Claves i18n de los campos de cada función (apps.calc.financial.field.*), en orden.
  [[nodiscard]] std::vector<std::string_view> financialFields(Financial function);
  [[nodiscard]] std::string_view financialId(Financial function);

  class Engine {
  public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Evalúa y, si sale bien, actualiza `ans`. "nombre = expresión" guarda una variable del usuario.
    [[nodiscard]] Result evaluate(std::string_view expression, const Settings& settings);
    // Vista previa mientras se escribe: no toca `ans` ni variables y descarta lo que no es un cálculo.
    [[nodiscard]] std::optional<std::string> preview(std::string_view expression, const Settings& settings);
    // El último resultado (`ans`) en las cuatro bases; nullopt si no es entero.
    [[nodiscard]] std::optional<BaseValues> lastBases(const Settings& settings) const;
    // Texto de `value` (sin signo, recortado a wordSize) en la base de entrada actual.
    [[nodiscard]] std::string formatInteger(std::uint64_t value, const Settings& settings) const;

    // Variables del usuario, por nombre.
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& variables() const { return m_variables; }
    bool setVariable(const std::string& name, std::string_view expression, const Settings& settings);
    void removeVariable(const std::string& name);

    // Conversión.
    [[nodiscard]] std::vector<std::string> unitCategories() const;
    // La de longitud, como GNOME Calculator.
    [[nodiscard]] std::string defaultUnitCategory() const;
    [[nodiscard]] std::vector<UnitInfo> units(std::string_view category) const;
    // Unidad de partida al abrir una categoría: su unidad base (metro, segundo…) si la tiene.
    [[nodiscard]] std::string baseUnit(std::string_view category) const;
    // Convierte con los objetos Unit: las compuestas no siempre tienen un nombre que se pueda escribir.
    [[nodiscard]] Result convert(
        std::string_view value, std::string_view category, std::string_view from, std::string_view to,
        const Settings& settings
    );
    [[nodiscard]] bool isCurrencyCategory(std::string_view category) const;
    // (url, archivo) de cada fuente de tasas de cambio; la app las baja con curl y luego llama reloadExchangeRates().
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> exchangeRateSources() const;
    void reloadExchangeRates();

    [[nodiscard]] Result financial(Financial function, const std::vector<std::string>& args, const Settings& settings);

  private:
    [[nodiscard]] std::string print(const MathStructure& value, const Settings& settings, int base = 0) const;
    [[nodiscard]] Result calculate(std::string_view expression, const Settings& settings, MathStructure& out);

    std::unique_ptr<Calculator> m_calc;
    KnownVariable* m_ans = nullptr; // propiedad de m_calc
    std::unique_ptr<MathStructure> m_last;
    std::vector<std::pair<std::string, std::string>> m_variables; // nombre → expresión guardada
  };

} // namespace okaru::calc
