#include "apps/calculator/calc_engine.h"

#include <algorithm>
#include <array>
#include <format>
#include <libqalculate/BuiltinFunctions.h>
#include <libqalculate/Calculator.h>
#include <libqalculate/Function.h>
#include <libqalculate/MathStructure.h>
#include <libqalculate/Number.h>
#include <libqalculate/Unit.h>
#include <libqalculate/Variable.h>
#include <regex>
#include <set>

namespace okaru::calc {

  namespace {

    std::string trimmed(std::string_view text) {
      const auto first = text.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos) {
        return {};
      }
      const auto last = text.find_last_not_of(" \t\r\n");
      return std::string(text.substr(first, last - first + 1));
    }

    struct FinancialSpec {
      Financial function;
      std::string_view id;
      std::array<std::string_view, 4> fields;
      std::size_t fieldCount;
      // Fórmula de GNOME Calculator; $1..$4 son los campos en orden.
      std::string_view formula;
    };

    constexpr std::array<FinancialSpec, kFinancialCount> kFinancial{{
        {Financial::Ctrm, "ctrm", {"rate", "future-value", "present-value"}, 3, "ln($2/$3)/ln(1+$1)"},
        {Financial::Ddb, "ddb", {"cost", "life", "period"}, 3, "$1*(2/$2)*(1-2/$2)^($3-1)"},
        {Financial::Fv, "fv", {"payment", "rate", "periods"}, 3, "$1*((1+$2)^$3-1)/$2"},
        {Financial::Gpm, "gpm", {"cost", "margin"}, 2, "$1/(1-$2)"},
        {Financial::Pmt, "pmt", {"principal", "rate", "periods"}, 3, "$1*$2/(1-(1+$2)^(-$3))"},
        {Financial::Pv, "pv", {"payment", "rate", "periods"}, 3, "$1*(1-(1+$2)^(-$3))/$2"},
        {Financial::Rate, "rate", {"future-value", "present-value", "periods"}, 3, "($1/$2)^(1/$3)-1"},
        {Financial::Sln, "sln", {"cost", "salvage", "life"}, 3, "($1-$2)/$3"},
        {Financial::Syd, "syd", {"cost", "salvage", "life", "period"}, 4, "($1-$2)*($3-$4+1)/($3*($3+1)/2)"},
        {Financial::Term, "term", {"payment", "future-value", "rate"}, 3, "ln(1+$2*$3/$1)/ln(1+$3)"},
    }};

    const FinancialSpec& spec(Financial function) { return kFinancial[static_cast<std::size_t>(function)]; }

    EvaluationOptions evaluationOptions(const Settings& settings, int base) {
      EvaluationOptions eo = default_user_evaluation_options;
      eo.approximation = APPROXIMATION_APPROXIMATE;
      eo.parse_options.base = base;
      switch (settings.angle) {
      case AngleUnit::Degrees:
        eo.parse_options.angle_unit = ANGLE_UNIT_DEGREES;
        break;
      case AngleUnit::Radians:
        eo.parse_options.angle_unit = ANGLE_UNIT_RADIANS;
        break;
      case AngleUnit::Gradians:
        eo.parse_options.angle_unit = ANGLE_UNIT_GRADIANS;
        break;
      }
      return eo;
    }

    std::uint64_t wordMask(int wordSize) {
      return wordSize >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << static_cast<unsigned>(wordSize)) - 1U;
    }

  } // namespace

  std::vector<std::string_view> financialFields(Financial function) {
    const auto& s = spec(function);
    return {s.fields.begin(), s.fields.begin() + static_cast<std::ptrdiff_t>(s.fieldCount)};
  }

  std::string_view financialId(Financial function) { return spec(function).id; }

  Engine::Engine() : m_last(std::make_unique<MathStructure>()) {
    m_calc = std::make_unique<Calculator>();
    m_calc->loadExchangeRates();
    m_calc->loadGlobalDefinitions();
    m_calc->loadLocalDefinitions();
    // Ejecutan programas o tocan archivos: una expresión pegada no debe poder hacerlo.
    for (const int id : {FUNCTION_ID_COMMAND, FUNCTION_ID_PLOT, FUNCTION_ID_LOAD, FUNCTION_ID_EXPORT}) {
      if (MathFunction* function = m_calc->getFunctionById(id); function != nullptr) {
        function->setActive(false);
      }
    }
    m_calc->setPrecision(20);
    m_ans = new KnownVariable("", "ans", MathStructure(0));
    m_calc->addVariable(m_ans);
  }

  // ~Calculator libera las variables que se le agregaron.
  Engine::~Engine() = default;

  std::string Engine::print(const MathStructure& value, const Settings& settings, int base) const {
    PrintOptions po = default_print_options;
    po.use_unicode_signs = true;
    po.interval_display = INTERVAL_DISPLAY_SIGNIFICANT_DIGITS;
    po.base = base != 0 ? base : settings.base;
    po.max_decimals = settings.decimals;
    po.use_max_decimals = true;
    po.show_ending_zeroes = settings.trailingZeros;
    po.digit_grouping = settings.thousandsSeparators ? DIGIT_GROUPING_LOCALE : DIGIT_GROUPING_NONE;
    switch (settings.format) {
    case NumberFormat::Automatic:
      po.min_exp = EXP_PRECISION;
      break;
    case NumberFormat::Fixed:
      po.min_exp = EXP_NONE;
      break;
    case NumberFormat::Scientific:
      po.min_exp = EXP_SCIENTIFIC;
      break;
    case NumberFormat::Engineering:
      po.min_exp = EXP_BASE_3;
      break;
    }
    if (po.base != 10) {
      po.twos_complement = true;
      po.hexadecimal_twos_complement = true;
      po.binary_bits = static_cast<unsigned int>(settings.wordSize);
    }
    return m_calc->print(value, 2000, po);
  }

  Result Engine::calculate(std::string_view expression, const Settings& settings, MathStructure& out) {
    const std::string text = trimmed(expression);
    if (text.empty()) {
      return {};
    }
    m_calc->clearMessages();
    const EvaluationOptions eo = evaluationOptions(settings, settings.base);
    const std::string input = m_calc->unlocalizeExpression(text, eo.parse_options);
    if (!m_calc->calculate(&out, input, 2000, eo)) {
      return {.ok = false, .text = "timeout"};
    }
    // Como qalc: un error anula el resultado; un aviso lo acompaña.
    std::string warning;
    for (CalculatorMessage* m = m_calc->message(); m != nullptr; m = m_calc->nextMessage()) {
      if (m->type() == MESSAGE_ERROR) {
        return {.ok = false, .text = m->message()};
      }
      if (m->type() == MESSAGE_WARNING && warning.empty()) {
        warning = m->message();
      }
    }
    if (out.isAborted() || out.isUndefined()) {
      return {.ok = false, .text = "undefined"};
    }
    return {.ok = true, .text = print(out, settings), .input = input, .warning = std::move(warning)};
  }

  Result Engine::evaluate(std::string_view expression, const Settings& settings) {
    // GNOME Calculator: "x = 5" guarda una variable; "==" sigue siendo comparación.
    static const std::regex kAssignment(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=)\s*(.+)$)");
    std::smatch match;
    const std::string text(expression);
    if (std::regex_match(text, match, kAssignment)) {
      const std::string name = match[1];
      if (!setVariable(name, match[2].str(), settings)) {
        return {.ok = false, .text = name, .badVariable = true};
      }
      return evaluate(name, settings);
    }

    MathStructure value;
    Result result = calculate(expression, settings, value);
    if (result.ok) {
      m_ans->set(value);
      *m_last = value;
    }
    return result;
  }

  std::optional<std::string> Engine::preview(std::string_view expression, const Settings& settings) {
    const std::string text = trimmed(expression);
    if (text.empty() || text.find('=') != std::string::npos) {
      return std::nullopt;
    }
    MathStructure value;
    Result result = calculate(text, settings, value);
    // Un número suelto no es un cálculo: no se repite debajo; con un aviso tampoco se adelanta.
    if (!result.ok
        || !result.warning.empty()
        || result.text.empty()
        || result.input == result.text
        || text == result.text) {
      return std::nullopt;
    }
    return result.text;
  }

  std::optional<BaseValues> Engine::lastBases(const Settings& settings) const {
    if (!m_last->isNumber() || !m_last->number().isInteger()) {
      return std::nullopt;
    }
    BaseValues values;
    values.bin = print(*m_last, settings, 2);
    values.oct = print(*m_last, settings, 8);
    values.dec = print(*m_last, settings, 10);
    values.hex = print(*m_last, settings, 16);
    const Number& n = m_last->number();
    bool overflow = false;
    const auto value = n.isNegative() ? static_cast<std::uint64_t>(n.lintValue(&overflow)) : n.ulintValue(&overflow);
    values.bits = value & wordMask(settings.wordSize);
    return values;
  }

  std::string Engine::formatInteger(std::uint64_t value, const Settings& settings) const {
    const MathStructure number(Number(std::to_string(value & wordMask(settings.wordSize))));
    PrintOptions po = default_print_options;
    po.base = settings.base;
    po.base_display = BASE_DISPLAY_NONE; // solo los dígitos: se escriben en la base de entrada actual
    return m_calc->print(number, 2000, po);
  }

  bool Engine::setVariable(const std::string& name, std::string_view expression, const Settings& settings) {
    if (!m_calc->variableNameIsValid(name) || name == "ans") {
      return false;
    }
    const bool ours = std::ranges::contains(m_variables, name, &std::pair<std::string, std::string>::first);
    // Las incógnitas x, y, z se pueden redefinir, como en GNOME Calculator; constantes, funciones y unidades no.
    const Variable* existing = m_calc->getActiveVariable(name);
    if (!ours
        && ((existing != nullptr && existing->isKnown())
            || m_calc->getActiveFunction(name) != nullptr
            || m_calc->getActiveUnit(name) != nullptr)) {
      return false;
    }
    MathStructure value;
    if (!calculate(expression, settings, value).ok) {
      return false;
    }
    if (ours) {
      if (auto* mine = dynamic_cast<KnownVariable*>(m_calc->getActiveVariable(name)); mine != nullptr) {
        mine->set(value);
      }
      std::ranges::find(m_variables, name, &std::pair<std::string, std::string>::first)->second = trimmed(expression);
    } else {
      m_calc->addVariable(new KnownVariable("", name, value));
      m_variables.emplace_back(name, trimmed(expression));
    }
    return true;
  }

  void Engine::removeVariable(const std::string& name) {
    if (Variable* variable = m_calc->getActiveVariable(name); variable != nullptr) {
      variable->destroy(); // también avisa a Calculator
    }
    std::erase_if(m_variables, [&](const auto& entry) { return entry.first == name; });
  }

  std::vector<std::string> Engine::unitCategories() const {
    std::set<std::string> categories;
    for (const Unit* unit : m_calc->units) {
      if (unit->isActive() && !unit->isHidden() && !unit->category().empty()) {
        categories.insert(unit->category());
      }
    }
    return {categories.begin(), categories.end()};
  }

  std::string Engine::defaultUnitCategory() const {
    const Unit* meter = m_calc->getActiveUnit("m");
    return meter != nullptr ? meter->category() : std::string();
  }

  std::vector<UnitInfo> Engine::units(std::string_view category) const {
    std::vector<UnitInfo> out;
    for (const Unit* unit : m_calc->units) {
      if (unit->isActive() && !unit->isHidden() && unit->category() == category) {
        out.push_back({.title = unit->title(true, true)});
      }
    }
    std::ranges::sort(out, {}, &UnitInfo::title);
    out.erase(std::ranges::unique(out, {}, &UnitInfo::title).begin(), out.end());
    return out;
  }

  std::string Engine::baseUnit(std::string_view category) const {
    for (const Unit* unit : m_calc->units) {
      if (unit->isActive()
          && !unit->isHidden()
          && unit->category() == category
          && unit->subtype() == SUBTYPE_BASE_UNIT) {
        return unit->title(true, true);
      }
    }
    return {};
  }

  Result Engine::convert(
      std::string_view value, std::string_view category, std::string_view from, std::string_view to,
      const Settings& settings
  ) {
    const auto find = [&](std::string_view title) -> Unit* {
      for (Unit* unit : m_calc->units) {
        if (unit->isActive() && !unit->isHidden() && unit->category() == category && unit->title(true, true) == title) {
          return unit;
        }
      }
      return nullptr;
    };
    Unit* fromUnit = find(from);
    Unit* toUnit = find(to);
    if (fromUnit == nullptr || toUnit == nullptr) {
      return {};
    }
    Settings decimal = settings;
    decimal.base = 10;
    MathStructure amount;
    Result result = calculate(value, decimal, amount);
    if (!result.ok) {
      return result;
    }
    const MathStructure converted =
        m_calc->convert(result.input, fromUnit, toUnit, 2000, evaluationOptions(decimal, 10));
    result.text = std::format("{} {}", print(converted, decimal), toUnit->preferredDisplayName(true, true).name);
    return result;
  }

  bool Engine::isCurrencyCategory(std::string_view category) const {
    const Unit* euro = m_calc->getUnitById(UNIT_ID_EURO);
    return euro != nullptr && euro->category() == category;
  }

  std::vector<std::pair<std::string, std::string>> Engine::exchangeRateSources() const {
    std::vector<std::pair<std::string, std::string>> sources;
    for (int i = 1;; ++i) {
      std::string url = m_calc->getExchangeRatesUrl(i);
      std::string file = m_calc->getExchangeRatesFileName(i);
      if (url.empty() || file.empty()) {
        break;
      }
      sources.emplace_back(std::move(url), std::move(file));
    }
    return sources;
  }

  void Engine::reloadExchangeRates() { m_calc->loadExchangeRates(); }

  Result Engine::financial(Financial function, const std::vector<std::string>& args, const Settings& settings) {
    const auto& s = spec(function);
    if (args.size() < s.fieldCount) {
      return {};
    }
    std::string expression(s.formula);
    for (std::size_t i = s.fieldCount; i-- > 0;) {
      if (trimmed(args[i]).empty()) {
        return {};
      }
      const std::string token = std::format("${}", i + 1);
      for (auto at = expression.find(token); at != std::string::npos; at = expression.find(token, at)) {
        const std::string arg = std::format("({})", args[i]);
        expression.replace(at, token.size(), arg);
        at += arg.size();
      }
    }
    Settings decimal = settings;
    decimal.base = 10;
    return evaluate(expression, decimal);
  }

} // namespace okaru::calc
