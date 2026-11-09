#pragma once

// OKARU: lo que okaru-calc recuerda entre sesiones (~/.local/state/okaru/calculator.toml).

#include "apps/calculator/calc_engine.h"

#include <filesystem>
#include <string>
#include <vector>

namespace okaru::calc {

  enum class Mode : std::uint8_t { Basic, Advanced, Financial, Programming, Conversion };
  inline constexpr std::size_t kModeCount = 5;

  struct HistoryEntry {
    std::string expression;
    std::string result;

    bool operator==(const HistoryEntry&) const = default;
  };

  inline constexpr std::size_t kHistoryLimit = 100;

  struct State {
    Mode mode = Mode::Basic;
    Settings settings;
    std::vector<HistoryEntry> history; // la más reciente al final
    std::vector<std::pair<std::string, std::string>> variables;
    std::size_t financial = 0;
    std::string unitCategory;
    std::string unitFrom;
    std::string unitTo;

    bool operator==(const State&) const = default;
  };

  [[nodiscard]] std::filesystem::path statePath();
  [[nodiscard]] State loadState(const std::filesystem::path& path = statePath());
  bool saveState(const State& state, const std::filesystem::path& path = statePath());
  // Agrega al historial respetando el límite; ignora la repetición inmediata.
  void pushHistory(State& state, HistoryEntry entry);

} // namespace okaru::calc
