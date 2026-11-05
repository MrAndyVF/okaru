#include "shell/okaru_dashboard/okaru_dashboard_layout.h"

#include <algorithm>

namespace okaru::dashboard {

  namespace {
    using enum CardSize;
  } // namespace

  int span(CardSize size) noexcept {
    switch (size) {
    case Small:
      return 1;
    case Medium:
      return 2;
    case Large:
      return kColumns;
    }
    return 1;
  }

  std::optional<CardSize> parseSize(std::string_view name) noexcept {
    if (name == "small") {
      return Small;
    }
    if (name == "medium") {
      return Medium;
    }
    if (name == "large") {
      return Large;
    }
    return std::nullopt;
  }

  std::string_view sizeName(CardSize size) noexcept {
    switch (size) {
    case Small:
      return "small";
    case Medium:
      return "medium";
    case Large:
      return "large";
    }
    return "small";
  }

  std::string_view sizeLetter(CardSize size) noexcept {
    switch (size) {
    case Small:
      return "S";
    case Medium:
      return "M";
    case Large:
      return "L";
    }
    return "S";
  }

  const std::vector<CardKind>& builtinCardKinds() {
    static const std::vector<CardKind> kKinds{
        {"profile", "user", {Medium, Large}, "wallpaper"},
        {"clock", "clock", {Small, Medium, Large}, "okaru-calendar"},
        {"calendar", "calendar", {Medium, Large}, "okaru-calendar"},
        {"weather", "cloud", {Small, Medium}, "okaru-weather"},
        {"media", "music", {Medium, Large}, "okaru-media"},
        {"volume", "volume", {Medium, Large}, "okaru-audio"},
        {"microphone", "microphone", {Medium, Large}, "okaru-microphone"},
        {"brightness", "brightness-half", {Medium, Large}, "okaru-monitor"},
        {"network", "wifi", {Small, Medium}, "okaru-network"},
        {"bluetooth", "bluetooth", {Small, Medium}, "okaru-bluetooth"},
        {"phone", "device-mobile", {Small, Medium}, "okaru-phone"}, // OKARU
        {"power", "bolt", {Small, Medium}, "okaru-power"},
        {"system", "activity", {Small, Medium}, "okaru-system"},
        {"notifications", "bell", {Small, Medium}, "okaru-notifications"},
        {"screen-time", "hourglass", {Small, Medium}, "okaru-screen-time"},
        {"session", "power", {Small, Medium, Large}, "session"},
    };
    return kKinds;
  }

  bool isShortcutType(std::string_view type) noexcept {
    return type.starts_with(kShortcutPrefix) && type.size() > kShortcutPrefix.size();
  }

  std::string_view shortcutId(std::string_view type) noexcept {
    return isShortcutType(type) ? type.substr(kShortcutPrefix.size()) : std::string_view{};
  }

  const CardKind* findKind(std::string_view type) {
    static const CardKind kShortcut{"shortcut", "click", {Small, Medium}, ""};
    if (isShortcutType(type)) {
      return &kShortcut;
    }
    const auto& kinds = builtinCardKinds();
    const auto it = std::ranges::find(kinds, type, &CardKind::type);
    return it == kinds.end() ? nullptr : &*it;
  }

  CardSize normalizeSize(std::string_view type, std::string_view size) {
    const CardKind* kind = findKind(type);
    if (kind == nullptr || kind->sizes.empty()) {
      return Small;
    }
    if (const auto parsed = parseSize(size); parsed.has_value() && std::ranges::contains(kind->sizes, *parsed)) {
      return *parsed;
    }
    return kind->sizes.front();
  }

  CardSize nextSize(std::string_view type, CardSize current) {
    const CardKind* kind = findKind(type);
    if (kind == nullptr || kind->sizes.empty()) {
      return current;
    }
    const auto it = std::ranges::find(kind->sizes, current);
    if (it == kind->sizes.end() || std::next(it) == kind->sizes.end()) {
      return kind->sizes.front();
    }
    return *std::next(it);
  }

  CardSize defaultSize(std::string_view type) {
    const CardKind* kind = findKind(type);
    if (kind == nullptr || kind->sizes.empty()) {
      return Small;
    }
    return std::ranges::contains(kind->sizes, Medium) ? Medium : kind->sizes.front();
  }

  std::vector<std::vector<std::size_t>> packRows(std::span<const CardSize> sizes) {
    std::vector<std::vector<std::size_t>> rows;
    int used = kColumns; // fuerza una fila nueva con la primera tarjeta
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      const int width = span(sizes[i]);
      if (used + width > kColumns) {
        rows.emplace_back();
        used = 0;
      }
      rows.back().push_back(i);
      used += width;
    }
    return rows;
  }

  namespace {
    std::function<bool(std::string_view)>& detailOpener() {
      static std::function<bool(std::string_view)> opener;
      return opener;
    }
  } // namespace

  bool openHostedDetail(std::string_view panelId) { return detailOpener() && detailOpener()(panelId); }

  void setHostedDetailOpener(std::function<bool(std::string_view)> opener) { detailOpener() = std::move(opener); }

} // namespace okaru::dashboard
