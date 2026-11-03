#include "shell/settings/okaru_settings_rules.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_rules.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace settings {
  namespace {

    using okaru::hypr::Rule;
    using okaru::hypr::RuleKind;
    using Json = nlohmann::ordered_json;

    enum class Field : std::uint8_t { Bool, Pair, Text, Number };

    struct Effect {
      std::string_view key;
      Field field;
    };

    // Efectos con control propio; el resto se edita en el campo Lua.
    const std::vector<Effect>& windowEffects() {
      static const std::vector<Effect> kEffects{
          {"float", Field::Bool},
          {"tile", Field::Bool},
          {"center", Field::Bool},
          {"pin", Field::Bool},
          {"fullscreen", Field::Bool},
          {"maximize", Field::Bool},
          {"no_initial_focus", Field::Bool},
          {"no_anim", Field::Bool},
          {"no_blur", Field::Bool},
          {"no_shadow", Field::Bool},
          {"no_dim", Field::Bool},
          {"keep_aspect_ratio", Field::Bool},
          {"immediate", Field::Bool},
          {"size", Field::Pair},
          {"min_size", Field::Pair},
          {"max_size", Field::Pair},
          {"move", Field::Pair},
          {"workspace", Field::Text},
          {"monitor", Field::Text},
          {"opacity", Field::Number},
          {"scrolling_width", Field::Number},
          {"rounding", Field::Number},
          {"border_size", Field::Number},
      };
      return kEffects;
    }

    const std::vector<Effect>& layerEffects() {
      static const std::vector<Effect> kEffects{
          {"no_anim", Field::Bool},        {"blur", Field::Bool},       {"blur_popups", Field::Bool},
          {"xray", Field::Bool},           {"dim_around", Field::Bool}, {"no_screen_share", Field::Bool},
          {"ignore_alpha", Field::Number}, {"order", Field::Number},    {"animation", Field::Text},
      };
      return kEffects;
    }

    const std::vector<std::string_view>& matchKeys(RuleKind kind) {
      static const std::vector<std::string_view> kWindow{"class", "title", "initial_class", "initial_title"};
      static const std::vector<std::string_view> kLayer{"namespace"};
      return kind == RuleKind::Window ? kWindow : kLayer;
    }

    enum class Category : std::uint8_t { Shell, Apps, Other };

    // Categoría deducida del contenido, sin guardar nada extra en el archivo: capas y superficies de
    // la shell -> Shell; reglas por clase de ventana -> Apps; el resto (título, estado...) -> Otros.
    Category categoryOf(const Rule& rule) {
      if (rule.kind == RuleKind::Layer) {
        return Category::Shell;
      }
      const auto match = rule.table.find("match");
      if (match == rule.table.end() || !match->is_object()) {
        return Category::Other;
      }
      for (const auto key : {"class", "initial_class"}) {
        if (const auto it = match->find(key); it != match->end() && it->is_string()) {
          const std::string value = it->get<std::string>();
          if (value.find("okaru") != std::string::npos || value.find("noctalia") != std::string::npos) {
            return Category::Shell;
          }
          return Category::Apps;
        }
      }
      return Category::Other;
    }

    struct RulesState {
      compositors::hyprland::HyprlandRuntime runtime;
      std::function<void()> rebuild;
      std::string filter;
      std::string error;
      // Editor: índice de la regla (o -1 para una nueva) y el borrador.
      int editing = -2; // -2 = ninguno
      RuleKind draftKind = RuleKind::Window;
      Json draft = Json::object();
      std::string draftTitle;
      std::string draftDescription;
      std::string draftLua;
      int confirmDelete = -1;
      Category newCategory = Category::Apps; // dónde se muestra el editor de una regla nueva
    };

    RulesState& state() {
      static RulesState s;
      return s;
    }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    std::string tr(std::string_view key) { return i18n::tr("settings.okaru.rules." + std::string(key)); }

    std::string valueText(const Json& v) {
      if (v.is_string()) {
        return v.get<std::string>();
      }
      if (v.is_array() && v.size() == 2) {
        return valueText(v[0]) + "×" + valueText(v[1]);
      }
      if (v.is_number_float()) {
        std::string text = std::format("{:.3f}", v.get<double>());
        while (text.ends_with('0')) {
          text.pop_back();
        }
        if (text.ends_with('.')) {
          text.pop_back();
        }
        return text;
      }
      return v.dump();
    }

    std::string matchSummary(const Json& table) {
      std::string out;
      if (const auto it = table.find("match"); it != table.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
          out += (out.empty() ? "" : " · ") + key + " " + valueText(value);
        }
      }
      return out;
    }

    std::string effectSummary(const Json& table) {
      std::string out;
      for (const auto& [key, value] : table.items()) {
        if (key == "match" || key == "name") {
          continue;
        }
        out += out.empty() ? "" : " · ";
        out += value.is_boolean() && value.get<bool>() ? key : key + " " + valueText(value);
      }
      return out;
    }

    // Número si se puede (enteros como enteros), si no el texto (p. ej. "50%").
    Json parseScalar(const std::string& text) {
      if (text.empty()) {
        return Json();
      }
      char* end = nullptr;
      const double number = std::strtod(text.c_str(), &end);
      if (end != text.c_str() && *end == '\0') {
        if (std::floor(number) == number && text.find('.') == std::string::npos) {
          return static_cast<long long>(number);
        }
        return number;
      }
      return text;
    }

    void startEditing(int index, RuleKind kind, Json table, std::string title, std::string description = {}) {
      auto& s = state();
      s.editing = index;
      s.draftKind = kind;
      s.draft = std::move(table);
      if (!s.draft.contains("match")) {
        Json withMatch = Json::object();
        withMatch["match"] = Json::object();
        for (const auto& [k, v] : s.draft.items()) {
          withMatch[k] = v;
        }
        s.draft = std::move(withMatch);
      }
      s.draftTitle = std::move(title);
      s.draftDescription = std::move(description);
      s.draftLua = okaru::hypr::serializeRuleCall(kind, s.draft);
      s.confirmDelete = -1;
      s.error.clear();
    }

    // Quita condiciones vacías y efectos apagados o vacíos antes de guardar.
    Json cleaned(const Json& draft) {
      Json out = Json::object();
      for (const auto& [key, value] : draft.items()) {
        if (key == "match") {
          Json match = Json::object();
          for (const auto& [mk, mv] : value.items()) {
            if (!(mv.is_string() && mv.get<std::string>().empty()) && !mv.is_null()) {
              match[mk] = mv;
            }
          }
          out["match"] = match;
        } else if (
            !value.is_null()
            && !(value.is_boolean() && !value.get<bool>())
            && !(value.is_string() && value.get<std::string>().empty())
        ) {
          out[key] = value;
        }
      }
      return out;
    }

    void save() {
      auto& s = state();
      const Json table = cleaned(s.draft);
      if (!table.contains("match") || table["match"].empty()) {
        s.error = tr("need-match");
        rebuild();
        return;
      }
      const std::string previous = okaru::hypr::readRulesText();
      const std::string call = okaru::hypr::serializeRuleCall(s.draftKind, table);
      std::string next;
      if (s.editing >= 0) {
        const auto rules = okaru::hypr::parseRulesFile(previous);
        if (s.editing >= static_cast<int>(rules.size())) {
          s.error = tr("changed-outside");
          rebuild();
          return;
        }
        next = okaru::hypr::replaceRuleWithComment(
            previous, rules[static_cast<std::size_t>(s.editing)], s.draftTitle, s.draftDescription, call
        );
      } else {
        next = okaru::hypr::appendRule(previous, s.draftTitle, s.draftDescription, call);
      }
      if (auto err = okaru::hypr::writeRulesText(s.runtime, previous, next)) {
        s.error = *err;
      } else {
        s.editing = -2;
        s.error.clear();
      }
      rebuild();
    }

    void removeAt(int index) {
      auto& s = state();
      const std::string previous = okaru::hypr::readRulesText();
      const auto rules = okaru::hypr::parseRulesFile(previous);
      s.confirmDelete = -1;
      if (index >= 0 && index < static_cast<int>(rules.size())) {
        if (auto err = okaru::hypr::writeRulesText(
                s.runtime, previous, okaru::hypr::removeRule(previous, rules[static_cast<std::size_t>(index)])
            )) {
          s.error = *err;
        }
      }
      rebuild();
    }

    std::unique_ptr<Button> smallButton(
        float scale, std::string text, std::string glyph, ButtonVariant variant, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = glyph.empty() ? std::nullopt : std::optional<std::string>(glyph), // sin calavera
          .fontSize = Style::fontSizeCaption * scale,
          // Sin icono tampoco se pasa su tamaño: el botón le reservaría espacio y quedaría un hueco.
          .glyphSize = glyph.empty() ? std::nullopt : std::optional<float>(Style::fontSizeBody * scale),
          .variant = variant,
          .minHeight = Style::controlHeightSm * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Flex>
    labeled(float scale, const std::string& label, std::unique_ptr<Node> control, float width = 0.0F) {
      auto text = makeLabel(label, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      if (width > 0.0F) {
        text->setMaxWidth(width * scale);
        text->setEllipsize(TextEllipsize::End);
      }
      auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale}, std::move(text));
      column->addChild(std::move(control));
      return column;
    }

    std::unique_ptr<Node>
    textInput(float scale, std::string value, float width, std::function<void(std::string)> onChange) {
      Input* raw = nullptr;
      auto input = ui::input({
          .out = &raw,
          .value = std::move(value),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .width = width * scale,
      });
      raw->setOnChange([onChange = std::move(onChange)](const std::string& text) { onChange(text); });
      return input;
    }

    void addEditor(Flex& parent, float scale) {
      auto& s = state();
      auto editor = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
          .configure = [scale](Flex& box) {
            box.setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
            box.setCardStyle(scale, kCardFillOpacity);
          },
      });

      // Título y descripción: se guardan como comentarios sobre la regla.
      auto naming =
          ui::row({.align = FlexAlign::Start, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      naming->addChild(labeled(scale, tr("rule-title"), textInput(scale, s.draftTitle, 280, [](std::string v) {
                                 state().draftTitle = std::move(v);
                               })));
      naming->addChild(
          labeled(scale, tr("rule-description"), textInput(scale, s.draftDescription, 520, [](std::string v) {
                    state().draftDescription = std::move(v);
                  }))
      );
      editor->addChild(std::move(naming));

      // Condiciones.
      editor->addChild(
          makeLabel(tr("match"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      auto matchRow =
          ui::row({.align = FlexAlign::Start, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      for (const auto key : matchKeys(s.draftKind)) {
        const std::string k(key);
        const auto& match = s.draft["match"];
        const std::string value =
            match.contains(k) && match[k].is_string() ? match[k].get<std::string>() : std::string{};
        matchRow->addChild(labeled(scale, tr("match-" + k), textInput(scale, value, 200, [k](std::string v) {
                                     state().draft["match"][k] = std::move(v);
                                   })));
      }
      editor->addChild(std::move(matchRow));

      // Efectos con control propio.
      editor->addChild(makeLabel(
          tr("effects"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
      ));
      auto toggles =
          ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      auto fields =
          ui::row({.align = FlexAlign::Start, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      for (const auto& effect : s.draftKind == RuleKind::Window ? windowEffects() : layerEffects()) {
        const std::string k(effect.key);
        const Json current = s.draft.contains(k) ? s.draft[k] : Json();
        switch (effect.field) {
        case Field::Bool: {
          const bool on = current.is_boolean() && current.get<bool>();
          toggles->addChild(smallButton(
              scale, tr("effect-" + k), on ? "check" : "", on ? ButtonVariant::Primary : ButtonVariant::Outline,
              [k, on] {
                state().draft[k] = !on;
                rebuild();
              }
          ));
          break;
        }
        case Field::Pair: {
          const std::string a = current.is_array() && current.size() == 2 ? valueText(current[0]) : std::string{};
          const std::string b = current.is_array() && current.size() == 2 ? valueText(current[1]) : std::string{};
          auto pair = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
          const auto setPart = [k](std::size_t index, std::string v) {
            auto& d = state().draft;
            Json value = d.contains(k) && d[k].is_array() && d[k].size() == 2 ? d[k] : Json::array({Json(), Json()});
            value[index] = parseScalar(v);
            d[k] = value[0].is_null() && value[1].is_null() ? Json() : value;
          };
          pair->addChild(textInput(scale, a, 70, [setPart](std::string v) { setPart(0, std::move(v)); }));
          pair->addChild(
              makeLabel("×", Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
          );
          pair->addChild(textInput(scale, b, 70, [setPart](std::string v) { setPart(1, std::move(v)); }));
          fields->addChild(labeled(scale, tr("effect-" + k), std::move(pair), 160));
          break;
        }
        case Field::Text:
        case Field::Number:
          fields->addChild(labeled(
              scale, tr("effect-" + k),
              textInput(
                  scale, current.is_null() ? std::string{} : valueText(current), 120,
                  [k, number = effect.field == Field::Number](std::string v) {
                    state().draft[k] = number ? parseScalar(v) : Json(v);
                  }
              ),
              120
          ));
          break;
        }
      }
      editor->addChild(std::move(toggles));
      editor->addChild(std::move(fields));

      // Lua completo: para cualquier efecto sin control propio. Enter aplica el texto al borrador.
      Input* luaInput = nullptr;
      auto lua = ui::input({
          .out = &luaInput,
          .value = s.draftLua,
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
      });
      luaInput->setOnSubmit([](const std::string& text) {
        auto& st = state();
        const auto open = text.find('(');
        const auto close = text.rfind(')');
        const bool layer = text.starts_with("hl.layer_rule(");
        if ((text.starts_with("hl.window_rule(") || layer)
            && open != std::string::npos
            && close != std::string::npos
            && close > open) {
          if (auto table = okaru::hypr::parseLuaTable(std::string_view(text).substr(open + 1, close - open - 1))) {
            st.draftKind = layer ? RuleKind::Layer : RuleKind::Window;
            st.draft = std::move(*table);
            if (!st.draft.contains("match")) {
              st.draft["match"] = Json::object();
            }
            st.draftLua = text;
            st.error.clear();
            rebuild();
            return;
          }
        }
        st.error = tr("invalid-lua");
        rebuild();
      });
      editor->addChild(labeled(scale, tr("lua"), std::move(lua)));

      auto actions = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true}
      );
      actions->addChild(smallButton(scale, tr("cancel"), "circle-x", ButtonVariant::Ghost, [] {
        state().editing = -2;
        state().error.clear();
        rebuild();
      }));
      actions->addChild(smallButton(scale, tr("save"), "device-floppy", ButtonVariant::Primary, [] { save(); }));
      editor->addChild(std::move(actions));
      parent.addChild(std::move(editor));
    }

  } // namespace

  void addSettingsRules(Flex& content, SettingsRulesContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    if (s.runtime.requestSocketPath().empty()) {
      Flex* body = addSettingsCard(content, tr("title"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.hyprland-only"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }

    const auto rules = okaru::hypr::parseRulesFile(okaru::hypr::readRulesText());

    // Buscar en todas las categorías.
    Input* search = nullptr;
    auto searchInput = ui::input({
        .out = &search,
        .value = s.filter,
        .placeholder = tr("search"),
        .fontSize = Style::fontSizeCaption * scale,
        .controlHeight = Style::controlHeightSm * scale,
        .width = kControlWidthWide * scale,
    });
    search->setOnSubmit([](const std::string& text) {
      state().filter = text;
      rebuild();
    });
    content.addChild(std::move(searchInput));

    if (!s.error.empty()) {
      auto error =
          makeLabel(s.error, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold);
      error->setMaxLines(4);
      content.addChild(std::move(error));
    }

    std::string filter = s.filter;
    std::ranges::transform(filter, filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto windows = okaru::hypr::queryOpenWindows(s.runtime);

    for (const auto category : {Category::Shell, Category::Apps, Category::Other}) {
      std::vector<std::size_t> members;
      for (std::size_t i = 0; i < rules.size(); ++i) {
        if (categoryOf(rules[i]) == category) {
          members.push_back(i);
        }
      }
      const std::string id = category == Category::Shell ? "shell" : (category == Category::Apps ? "apps" : "other");
      Flex* card = addSettingsCard(content, std::format("{} ({})", tr("category-" + id), members.size()), scale);
      auto description = makeLabel(
          tr("category-" + id + "-desc"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      description->setMaxLines(2);
      card->addChild(std::move(description));

      // Crear: cada categoría con su punto de partida.
      auto actions =
          ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      if (category == Category::Shell) {
        actions->addChild(smallButton(scale, tr("new-shell"), "plus", ButtonVariant::Outline, [] {
          Json table = Json::object();
          table["match"] = {{"namespace", "^okaru-"}};
          startEditing(-1, RuleKind::Layer, std::move(table), {});
          state().newCategory = Category::Shell;
          rebuild();
        }));
      } else if (category == Category::Apps) {
        actions->addChild(smallButton(scale, tr("new-app"), "plus", ButtonVariant::Outline, [] {
          startEditing(-1, RuleKind::Window, Json::object(), {});
          state().newCategory = Category::Apps;
          rebuild();
        }));
        if (!windows.empty()) {
          std::vector<std::string> labels;
          labels.reserve(windows.size());
          for (const auto& w : windows) {
            labels.push_back(w.cls);
          }
          actions->addChild(
              ui::select({
                  .options = std::move(labels),
                  .clearSelection = true, // muestra el texto de ayuda: cualquier ventana se puede elegir
                  .placeholder = tr("from-window"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .controlHeight = Style::controlHeightSm * scale,
                  .width = kControlWidthWide * scale,
                  .onSelectionChanged = [windows](std::size_t index, std::string_view) {
                    Json table = Json::object();
                    table["match"] = {{"class", okaru::hypr::exactRegex(windows[index].cls)}};
                    table["float"] = true;
                    startEditing(-1, RuleKind::Window, std::move(table), windows[index].cls);
                    state().newCategory = Category::Apps;
                    rebuild();
                  },
              })
          );
        }
      } else {
        actions->addChild(smallButton(scale, tr("new-other"), "plus", ButtonVariant::Outline, [] {
          Json table = Json::object();
          table["match"] = {{"title", ""}};
          startEditing(-1, RuleKind::Window, std::move(table), {});
          state().newCategory = Category::Other;
          rebuild();
        }));
      }
      card->addChild(std::move(actions));
      if (s.editing == -1 && s.newCategory == category) {
        addEditor(*card, scale);
      }

      for (const std::size_t i : members) {
        const auto& rule = rules[i];
        const int index = static_cast<int>(i);
        const std::string match = matchSummary(rule.table);
        const std::string title = rule.title.empty() ? match : rule.title;
        if (!filter.empty()) {
          std::string haystack = title + " " + rule.description + " " + match + " " + effectSummary(rule.table);
          std::ranges::transform(haystack, haystack.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
          if (haystack.find(filter) == std::string::npos) {
            continue;
          }
        }

        auto text = ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
            makeLabel(
                (rule.kind == RuleKind::Layer ? tr("layer-prefix") : std::string{}) + title,
                Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
            )
        );
        if (!rule.description.empty()) {
          auto about =
              makeLabel(rule.description, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface));
          about->setMaxLines(3);
          text->addChild(std::move(about));
        }
        auto detail = makeLabel(
            (rule.title.empty() ? std::string{} : match + "  →  ") + effectSummary(rule.table),
            Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
        );
        detail->setMaxLines(3);
        text->addChild(std::move(detail));
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
        row->addChild(std::move(text));
        if (s.confirmDelete == index) {
          row->addChild(makeLabel(
              tr("delete-confirm"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface),
              FontWeight::Bold
          ));
          row->addChild(smallButton(scale, tr("cancel"), "circle-x", ButtonVariant::Ghost, [] {
            state().confirmDelete = -1;
            rebuild();
          }));
          row->addChild(smallButton(scale, tr("delete"), "trash", ButtonVariant::Destructive, [index] {
            removeAt(index);
          }));
        } else {
          row->addChild(smallButton(scale, {}, "edit", ButtonVariant::Ghost, [index, rule] {
            startEditing(index, rule.kind, rule.table, rule.title, rule.description);
            rebuild();
          }));
          row->addChild(smallButton(scale, {}, "trash", ButtonVariant::Ghost, [index] {
            state().confirmDelete = index;
            state().editing = -2;
            rebuild();
          }));
        }
        card->addChild(std::move(row));
        if (s.editing == index) {
          addEditor(*card, scale);
        }
      }
    }
  }

} // namespace settings
