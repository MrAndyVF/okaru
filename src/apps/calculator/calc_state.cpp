#include "apps/calculator/calc_state.h"

#include "util/file_utils.h"

#include <algorithm>
#include <fstream>
#include <toml++/toml.hpp>

namespace okaru::calc {

  namespace {

    template <typename T> T clampedEnum(std::int64_t value, std::size_t count, T fallback) {
      return value >= 0 && static_cast<std::size_t>(value) < count ? static_cast<T>(value) : fallback;
    }

  } // namespace

  std::filesystem::path statePath() { return std::filesystem::path(FileUtils::stateDir()) / "calculator.toml"; }

  State loadState(const std::filesystem::path& path) {
    State state;
    toml::table root;
    try {
      root = toml::parse_file(path.string());
    } catch (const toml::parse_error&) {
      return state;
    }
    state.mode = clampedEnum<Mode>(root["mode"].value_or<std::int64_t>(0), kModeCount, Mode::Basic);
    auto& s = state.settings;
    s.angle = clampedEnum<AngleUnit>(root["angle"].value_or<std::int64_t>(0), 3, AngleUnit::Degrees);
    s.format = clampedEnum<NumberFormat>(root["format"].value_or<std::int64_t>(0), 4, NumberFormat::Automatic);
    s.decimals = std::clamp<int>(root["decimals"].value_or(9), 0, 30);
    s.trailingZeros = root["trailing_zeros"].value_or(false);
    s.thousandsSeparators = root["thousands_separators"].value_or(false);
    const int base = root["base"].value_or(10);
    s.base = base == 2 || base == 8 || base == 16 ? base : 10;
    const int word = root["word_size"].value_or(64);
    s.wordSize = word == 8 || word == 16 || word == 32 ? word : 64;
    state.financial = std::min<std::size_t>(root["financial"].value_or<std::int64_t>(0), kFinancialCount - 1);
    state.unitCategory = root["unit_category"].value_or("");
    state.unitFrom = root["unit_from"].value_or("");
    state.unitTo = root["unit_to"].value_or("");
    if (const auto* history = root["history"].as_array()) {
      for (const auto& node : *history) {
        if (const auto* entry = node.as_table()) {
          state.history.push_back(
              {.expression = (*entry)["expression"].value_or(""), .result = (*entry)["result"].value_or("")}
          );
        }
      }
    }
    if (state.history.size() > kHistoryLimit) {
      state.history.erase(state.history.begin(), state.history.end() - kHistoryLimit);
    }
    if (const auto* variables = root["variables"].as_table()) {
      for (const auto& [name, value] : *variables) {
        if (const auto expression = value.value<std::string>()) {
          state.variables.emplace_back(std::string(name.str()), *expression);
        }
      }
    }
    return state;
  }

  bool saveState(const State& state, const std::filesystem::path& path) {
    const auto& s = state.settings;
    toml::table root{
        {"mode", static_cast<std::int64_t>(state.mode)},
        {"angle", static_cast<std::int64_t>(s.angle)},
        {"format", static_cast<std::int64_t>(s.format)},
        {"decimals", s.decimals},
        {"trailing_zeros", s.trailingZeros},
        {"thousands_separators", s.thousandsSeparators},
        {"base", s.base},
        {"word_size", s.wordSize},
        {"financial", static_cast<std::int64_t>(state.financial)},
        {"unit_category", state.unitCategory},
        {"unit_from", state.unitFrom},
        {"unit_to", state.unitTo},
    };
    toml::array history;
    for (const auto& entry : state.history) {
      history.push_back(toml::table{{"expression", entry.expression}, {"result", entry.result}});
    }
    root.insert("history", std::move(history));
    toml::table variables;
    for (const auto& [name, expression] : state.variables) {
      variables.insert(name, expression);
    }
    root.insert("variables", std::move(variables));

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out) {
        return false;
      }
      out << root << '\n';
      if (!out) {
        return false;
      }
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
  }

  void pushHistory(State& state, HistoryEntry entry) {
    if (!state.history.empty() && state.history.back() == entry) {
      return;
    }
    state.history.push_back(std::move(entry));
    if (state.history.size() > kHistoryLimit) {
      state.history.erase(state.history.begin());
    }
  }

} // namespace okaru::calc
