#include "apps/calculator/calculator_window.h"

#include "apps/common/okaru_app.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>

namespace okaru::calc {

  namespace {

    using i18n::tr;

    enum class KeyAction : std::uint8_t { Insert, Backspace, Clear, Equals };

    struct Key {
      std::string_view label;
      std::string_view insert;
      KeyAction action = KeyAction::Insert;
      ButtonVariant variant = ButtonVariant::Default;
      int span = 1;
    };

    using KeyRow = std::array<Key, 6>;

    // Teclado básico de GNOME Calculator.
    constexpr std::array<KeyRow, 4> kBasicKeys{{
        {{{"7", "7"},
          {"8", "8"},
          {"9", "9"},
          {"÷", "÷", KeyAction::Insert, ButtonVariant::Outline},
          {"⌫", "", KeyAction::Backspace, ButtonVariant::Outline},
          {"C", "", KeyAction::Clear, ButtonVariant::Destructive}}},
        {{{"4", "4"},
          {"5", "5"},
          {"6", "6"},
          {"×", "×", KeyAction::Insert, ButtonVariant::Outline},
          {"(", "(", KeyAction::Insert, ButtonVariant::Outline},
          {")", ")", KeyAction::Insert, ButtonVariant::Outline}}},
        {{{"1", "1"},
          {"2", "2"},
          {"3", "3"},
          {"−", "−", KeyAction::Insert, ButtonVariant::Outline},
          {"x²", "^2", KeyAction::Insert, ButtonVariant::Outline},
          {"√", "√", KeyAction::Insert, ButtonVariant::Outline}}},
        {{{"0", "0"},
          {".", "."},
          {"%", "%", KeyAction::Insert, ButtonVariant::Outline},
          {"+", "+", KeyAction::Insert, ButtonVariant::Outline},
          {"=", "", KeyAction::Equals, ButtonVariant::Primary, 2},
          {}}},
    }};

    constexpr std::array<KeyRow, 5> kAdvancedKeys{{
        {{{"sin", "sin("}, {"cos", "cos("}, {"tan", "tan("}, {"sinh", "sinh("}, {"cosh", "cosh("}, {"tanh", "tanh("}}},
        {{{"asin", "asin("},
          {"acos", "acos("},
          {"atan", "atan("},
          {"ln", "ln("},
          {"log", "log10("},
          {"log₂", "log2("}}},
        {{{"xʸ", "^"}, {"eˣ", "exp("}, {"10ˣ", "10^"}, {"n!", "!"}, {"|x|", "abs("}, {"mod", " mod "}}},
        {{{"π", "π"}, {"e", "e"}, {"i", "i"}, {"x⁻¹", "^(−1)"}, {"⌊x⌋", "floor("}, {"⌈x⌉", "ceil("}}},
        {{{"∛", "∛"}, {"ⁿ√", "root("}, {"ans", "ans"}, {"×10ⁿ", "E"}, {"°", "°"}, {";", ";"}}},
    }};

    constexpr std::array<KeyRow, 2> kProgrammerKeys{{
        {{{"A", "A"}, {"B", "B"}, {"C", "C"}, {"D", "D"}, {"E", "E"}, {"F", "F"}}},
        {{{"AND", "&"}, {"OR", "|"}, {"XOR", "xor("}, {"NOT", "~"}, {"<<", "<<"}, {">>", ">>"}}},
    }};

    constexpr std::array<int, 4> kBases{2, 8, 10, 16};
    constexpr std::array<int, 4> kWordSizes{8, 16, 32, 64};
    constexpr std::array<std::string_view, kModeCount> kModeIds{
        "basic", "advanced", "financial", "programming", "conversion"
    };

    // ¿Se puede escribir este dígito en la base actual? Las letras A-F solo existen en hexadecimal.
    bool digitAllowed(std::string_view insert, int base) {
      if (insert.size() != 1) {
        return true;
      }
      const char c = insert[0];
      if (c >= '0' && c <= '9') {
        return c - '0' < base;
      }
      if (c >= 'A' && c <= 'F') {
        return base == 16;
      }
      return c != '.' || base == 10;
    }

    // Las teclas del teclado en pantalla son UTF-8 válido escrito en este archivo.
    std::vector<char32_t> codepoints(std::string_view text) {
      std::vector<char32_t> out;
      for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
        char32_t cp = length == 1 ? lead : lead & (0xFFU >> (length + 1));
        for (std::size_t j = 1; j < length && i + j < text.size(); ++j) {
          cp = (cp << 6U) | (static_cast<unsigned char>(text[i + j]) & 0x3FU);
        }
        out.push_back(cp);
        i += length;
      }
      return out;
    }

    std::unique_ptr<Node> caption(std::string text, float scale) {
      return ui::label({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      });
    }

    std::unique_ptr<Node> settingRow(std::string title, std::unique_ptr<Node> control, float scale) {
      return ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true},
          ui::label({.text = std::move(title), .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F}),
          std::move(control)
      );
    }

  } // namespace

  CalculatorWindow::CalculatorWindow(apps::OkaruApp& app) : m_app(app), m_state(loadState()) {
    for (const auto& [name, expression] : m_state.variables) {
      (void)m_engine.setVariable(name, expression, m_state.settings);
    }
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setAfterBuild([this] {
      if (m_input != nullptr && !m_showPreferences) {
        m_app.focus(m_input->inputArea());
      }
    });
  }

  CalculatorWindow::~CalculatorWindow() {
    m_state.variables = m_engine.variables();
    (void)saveState(m_state);
  }

  Settings CalculatorWindow::effectiveSettings() const {
    Settings settings = m_state.settings;
    if (m_state.mode != Mode::Programming) {
      settings.base = 10;
    }
    return settings;
  }

  void CalculatorWindow::changed(bool rebuild) {
    m_state.variables = m_engine.variables();
    (void)saveState(m_state);
    if (rebuild) {
      m_app.requestRebuild();
    }
  }

  // ── Escena ──────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> CalculatorWindow::build(float width, float height, float scale) {
    m_input = nullptr;
    m_messageLabel = nullptr;
    m_conversionLabel = nullptr;

    auto root = ui::node({.width = width, .height = height});
    root->addChild(
        ui::box({
            .fill = colorSpecFromRole(ColorRole::Surface),
            .width = width,
            .height = height,
        })
    );

    auto main = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .width = width,
        .height = height,
    });
    Node* header = main->addChild(buildHeader(scale));
    m_app.setDragNode(header);

    if (m_showPreferences) {
      main->addChild(buildPreferences(scale));
    } else {
      switch (m_state.mode) {
      case Mode::Basic:
      case Mode::Advanced:
      case Mode::Programming:
        main->addChild(buildHistory(scale));
        main->addChild(buildDisplay(scale));
        if (m_state.mode == Mode::Programming) {
          main->addChild(buildProgrammer(scale));
        }
        main->addChild(buildKeypad(scale));
        break;
      case Mode::Financial:
        main->addChild(buildHistory(scale));
        main->addChild(buildFinancial(scale));
        break;
      case Mode::Conversion:
        main->addChild(buildConversion(scale));
        break;
      }
    }
    root->addChild(std::move(main));
    return root;
  }

  std::unique_ptr<Node> CalculatorWindow::buildHeader(float scale) {
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    if (m_showPreferences) {
      header->addChild(
          ui::button({
              .glyph = "arrow-left",
              .variant = ButtonVariant::Ghost,
              .tooltip = tr("apps.calc.back"),
              .onClick = [this] {
                m_showPreferences = false;
                m_app.requestRebuild();
              },
          })
      );
    }
    header->addChild(
        ui::label({
            .text = m_showPreferences ? tr("apps.calc.preferences.title") : tr("apps.calc.title"),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::SemiBold,
            .flexGrow = 1.0F,
        })
    );
    if (!m_showPreferences) {
      std::vector<std::string> modes;
      for (const auto id : kModeIds) {
        modes.push_back(tr(std::format("apps.calc.mode.{}", id)));
      }
      header->addChild(
          ui::select({
              .options = std::move(modes),
              .selectedIndex = static_cast<std::size_t>(m_state.mode),
              .width = 150.0F * scale,
              .onSelectionChanged = [this](std::size_t index, std::string_view) {
                m_state.mode = static_cast<Mode>(index);
                m_message.clear();
                changed();
              },
          })
      );
      header->addChild(
          ui::button({
              .glyph = "settings",
              .variant = ButtonVariant::Ghost,
              .tooltip = tr("apps.calc.preferences.title"),
              .onClick = [this] {
                m_showPreferences = true;
                m_app.requestRebuild();
              },
          })
      );
    }
    header->addChild(
        ui::button({
            .glyph = "x",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.calc.close"),
            .onClick = [this] { m_app.quit(); },
        })
    );
    return header;
  }

  std::unique_ptr<Node> CalculatorWindow::buildHistory(float scale) {
    ScrollView* view = nullptr;
    auto scroll = ui::scrollView({.out = &view, .flexGrow = 1.0F});
    view->setStickToBottom(true);
    Flex* content = view->content();
    content->setGap(Style::spaceXs * scale);
    content->setAlign(FlexAlign::Stretch);
    if (m_state.history.empty()) {
      content->addChild(caption(tr("apps.calc.history.empty"), scale));
    }
    for (const auto& entry : m_state.history) {
      // Un clic en una entrada escribe su resultado en la expresión, como GNOME Calculator.
      auto item = ui::button({
          .contentAlign = ButtonContentAlign::End,
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.calc.history.insert"),
          .paddingV = Style::spaceXs * scale,
          .gap = 2.0F * scale,
          .onClick = [this, result = entry.result] { typeText(result); },
      });
      item->setDirection(FlexDirection::Vertical);
      item->setAlign(FlexAlign::End);
      item->addChild(
          ui::label({
              .text = entry.expression,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .textAlign = TextAlign::End,
          })
      );
      item->addChild(
          ui::label({
              .text = entry.result,
              .fontSize = Style::fontSizeTitle * scale,
              .textAlign = TextAlign::End,
          })
      );
      content->addChild(std::move(item));
    }
    return scroll;
  }

  std::unique_ptr<Node> CalculatorWindow::buildDisplay(float scale) {
    auto display = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .fillWidth = true});
    display->addChild(
        ui::input({
            .out = &m_input,
            .value = m_expression,
            .placeholder = tr("apps.calc.placeholder"),
            .fontSize = Style::fontSizeHeader * 1.2F * scale,
            .controlHeight = Style::controlHeightLg * 1.4F * scale,
            .textAlign = TextAlign::End,
            .onChange =
                [this](const std::string& value) {
                  m_expression = value;
                  m_historyCursor = -1;
                  m_justEvaluated = false;
                  updatePreview();
                },
            .onSubmit = [this](const std::string&) { submit(); },
            .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) -> bool {
              if (modifiers != 0) {
                return false;
              }
              if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
                recallHistory(sym == XKB_KEY_Up ? -1 : 1);
                return true;
              }
              return false;
            },
        })
    );
    display->addChild(
        ui::label({
            .out = &m_messageLabel,
            .text = m_message,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(m_messageIsError ? ColorRole::Error : ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .textAlign = TextAlign::End,
        })
    );
    return display;
  }

  std::unique_ptr<Node> CalculatorWindow::buildKeypad(float scale) {
    const float gap = Style::spaceXs * scale;
    const float keyHeight = Style::controlHeightLg * scale;
    // Solo en programación se filtran los dígitos por base; si no, "E" (×10ⁿ) parecería un dígito hexadecimal.
    const int base = m_state.mode == Mode::Programming ? effectiveSettings().base : 16;
    auto pad = ui::column({.align = FlexAlign::Stretch, .gap = gap, .fillWidth = true});
    const auto addRows = [&](const auto& rows) {
      for (const KeyRow& keys : rows) {
        auto row = ui::row({.align = FlexAlign::Stretch, .gap = gap, .fillWidth = true});
        for (const Key& key : keys) {
          if (key.label.empty()) {
            continue;
          }
          row->addChild(
              ui::button({
                  .text = std::string(key.label),
                  .fontSize = Style::fontSizeTitle * scale,
                  .enabled = key.action != KeyAction::Insert || digitAllowed(key.insert, base),
                  .variant = key.variant,
                  .minHeight = keyHeight,
                  .flexGrow = static_cast<float>(key.span),
                  .onClick = [this, key] {
                    switch (key.action) {
                    case KeyAction::Insert:
                      typeText(key.insert);
                      break;
                    case KeyAction::Backspace:
                      backspace();
                      break;
                    case KeyAction::Clear:
                      clear();
                      break;
                    case KeyAction::Equals:
                      submit();
                      break;
                    }
                  },
              })
          );
        }
        pad->addChild(std::move(row));
      }
    };
    if (m_state.mode == Mode::Advanced) {
      addRows(kAdvancedKeys);
    } else if (m_state.mode == Mode::Programming) {
      addRows(kProgrammerKeys);
    }
    addRows(kBasicKeys);
    return pad;
  }

  std::unique_ptr<Node> CalculatorWindow::buildProgrammer(float scale) {
    const Settings settings = effectiveSettings();
    auto panel = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});

    std::vector<std::string> baseNames;
    for (const char* id : {"binary", "octal", "decimal", "hexadecimal"}) {
      baseNames.push_back(tr(std::format("apps.calc.programming.base.{}", id)));
    }
    std::vector<std::string> wordNames;
    for (const int bits : kWordSizes) {
      wordNames.push_back(tr("apps.calc.programming.bits", "count", bits));
    }
    panel->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
            ui::select({
                .options = std::move(baseNames),
                .selectedIndex = static_cast<std::size_t>(std::ranges::find(kBases, settings.base) - kBases.begin()),
                .flexGrow = 1.0F,
                .onSelectionChanged =
                    [this](std::size_t index, std::string_view) {
                      m_state.settings.base = kBases[index];
                      m_expression.clear();
                      m_message.clear();
                      changed();
                    },
            }),
            ui::select({
                .options = std::move(wordNames),
                .selectedIndex =
                    static_cast<std::size_t>(std::ranges::find(kWordSizes, settings.wordSize) - kWordSizes.begin()),
                .flexGrow = 1.0F,
                .onSelectionChanged = [this](std::size_t index, std::string_view) {
                  m_state.settings.wordSize = kWordSizes[index];
                  changed();
                },
            })
        )
    );

    const auto bases = m_engine.lastBases(settings);
    if (!bases.has_value()) {
      return panel;
    }
    for (const auto& [name, value] : std::array<std::pair<std::string_view, std::string>, 4>{
             {{"HEX", bases->hex}, {"DEC", bases->dec}, {"OCT", bases->oct}, {"BIN", bases->bin}}
         }) {
      auto item = ui::button({
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.calc.copy"),
          .minHeight = Style::controlHeightSm * 0.75F * scale,
          .paddingV = 2.0F * scale,
          .gap = Style::spaceMd * scale,
          .onClick = [this, value] { m_app.clipboard().setClipboardText(value); },
      });
      item->addChild(
          ui::label({
              .text = std::string(name),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::Primary),
              .width = 36.0F * scale,
          })
      );
      item->addChild(
          ui::label({
              .text = value,
              .fontSize = Style::fontSizeCaption * scale,
              .textAlign = TextAlign::End,
              .ellipsize = TextEllipsize::Start,
              .flexGrow = 1.0F,
          })
      );
      panel->addChild(std::move(item));
    }

    // Panel de bits como el de GNOME: un clic invierte el bit y recalcula.
    const int words = settings.wordSize;
    for (int rowStart = words - 1; rowStart >= 0; rowStart -= 16) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = 2.0F * scale, .fillWidth = true});
      row->addChild(
          ui::label({
              .text = std::to_string(rowStart),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .width = 22.0F * scale,
          })
      );
      for (int bit = rowStart; bit > rowStart - 16 && bit >= 0; --bit) {
        const bool set = ((bases->bits >> static_cast<unsigned>(bit)) & 1U) != 0;
        row->addChild(
            ui::button({
                .text = set ? "1" : "0",
                .fontSize = Style::fontSizeCaption * scale,
                .variant = set ? ButtonVariant::Primary : ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * 0.75F * scale,
                .padding = 0.0F,
                .flexGrow = 1.0F,
                .onClick = [this, bit, bits = bases->bits] {
                  const Settings current = effectiveSettings();
                  m_expression =
                      m_engine.formatInteger(bits ^ (std::uint64_t{1} << static_cast<unsigned>(bit)), current);
                  const Result result = m_engine.evaluate(m_expression, current);
                  m_lastResult = result.ok ? result.text : m_lastResult;
                  m_message.clear();
                  m_app.requestRebuild();
                },
            })
        );
        if (bit % 4 == 0 && bit > rowStart - 15) {
          row->addChild(ui::node({.width = Style::spaceXs * scale}));
        }
      }
      panel->addChild(std::move(row));
    }
    return panel;
  }

  std::unique_ptr<Node> CalculatorWindow::buildFinancial(float scale) {
    const auto function = static_cast<Financial>(m_state.financial);
    const auto id = financialId(function);
    const auto fields = financialFields(function);
    m_financialArgs.resize(fields.size());

    auto panel = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kFinancialCount; ++i) {
      names.push_back(tr(std::format("apps.calc.financial.{}.name", financialId(static_cast<Financial>(i)))));
    }
    panel->addChild(
        ui::select({
            .options = std::move(names),
            .selectedIndex = m_state.financial,
            .onSelectionChanged = [this](std::size_t index, std::string_view) {
              m_state.financial = index;
              m_financialArgs.clear();
              m_message.clear();
              changed();
            },
        })
    );
    panel->addChild(caption(tr(std::format("apps.calc.financial.{}.description", id)), scale));
    for (std::size_t i = 0; i < fields.size(); ++i) {
      panel->addChild(settingRow(
          tr(std::format("apps.calc.financial.field.{}", fields[i])),
          ui::input({
              .value = m_financialArgs[i],
              .width = 160.0F * scale,
              .onChange = [this, i](const std::string& value) { m_financialArgs[i] = value; },
              .onSubmit = [this](const std::string&) { submit(); },
          }),
          scale
      ));
    }
    panel->addChild(
        ui::label({
            .out = &m_messageLabel,
            .text = m_message,
            .fontSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(m_messageIsError ? ColorRole::Error : ColorRole::OnSurface),
            .textAlign = TextAlign::End,
        })
    );
    panel->addChild(
        ui::button({
            .text = tr("apps.calc.financial.calculate"),
            .variant = ButtonVariant::Primary,
            .minHeight = Style::controlHeightLg * scale,
            .onClick = [this] { submit(); },
        })
    );
    return panel;
  }

  std::unique_ptr<Node> CalculatorWindow::buildConversion(float scale) {
    const auto categories = m_engine.unitCategories();
    if (categories.empty()) {
      return caption(tr("apps.calc.conversion.unavailable"), scale);
    }
    if (!std::ranges::contains(categories, m_state.unitCategory)) {
      const auto length = m_engine.defaultUnitCategory();
      m_state.unitCategory = std::ranges::contains(categories, length) ? length : categories.front();
    }
    const auto units = m_engine.units(m_state.unitCategory);
    std::vector<std::string> unitNames;
    for (const auto& unit : units) {
      unitNames.push_back(unit.title);
    }
    const auto indexOf = [&](const std::string& name, std::size_t fallback) {
      const auto it = std::ranges::find(units, name, &UnitInfo::title);
      return it != units.end() ? static_cast<std::size_t>(it - units.begin()) : std::min(fallback, units.size() - 1);
    };
    if (!units.empty()) {
      if (m_state.unitFrom.empty()) {
        m_state.unitFrom = m_engine.baseUnit(m_state.unitCategory);
      }
      m_state.unitFrom = units[indexOf(m_state.unitFrom, 0)].title;
      if (m_state.unitTo.empty() || m_state.unitTo == m_state.unitFrom) {
        const auto other = std::ranges::find_if(units, [&](const UnitInfo& u) { return u.title != m_state.unitFrom; });
        m_state.unitTo = other != units.end() ? other->title : m_state.unitFrom;
      }
      m_state.unitTo = units[indexOf(m_state.unitTo, 1)].title;
    }
    const bool currency = m_engine.isCurrencyCategory(m_state.unitCategory);
    if (currency && !m_ratesFetched) {
      refreshExchangeRates();
    }
    convertNow();

    auto panel = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .fillWidth = true});
    panel->addChild(
        ui::select({
            .options = categories,
            .selectedIndex =
                static_cast<std::size_t>(std::ranges::find(categories, m_state.unitCategory) - categories.begin()),
            .onSelectionChanged = [this, categories](std::size_t index, std::string_view) {
              m_state.unitCategory = categories[index];
              m_state.unitFrom.clear();
              m_state.unitTo.clear();
              changed();
            },
        })
    );
    panel->addChild(
        ui::input({
            .value = m_conversionValue,
            .placeholder = tr("apps.calc.conversion.value"),
            .fontSize = Style::fontSizeHeader * scale,
            .controlHeight = Style::controlHeightLg * 1.2F * scale,
            .textAlign = TextAlign::End,
            .onChange = [this](const std::string& value) {
              m_conversionValue = value;
              convertNow();
              if (m_conversionLabel != nullptr) {
                m_conversionLabel->setText(m_conversionResult);
                m_app.invalidate();
              }
            },
        })
    );
    if (!units.empty()) {
      panel->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
              ui::select({
                  .options = unitNames,
                  .selectedIndex = indexOf(m_state.unitFrom, 0),
                  .flexGrow = 1.0F,
                  .onSelectionChanged =
                      [this, units](std::size_t index, std::string_view) {
                        m_state.unitFrom = units[index].title;
                        changed();
                      },
              }),
              ui::button({
                  .glyph = "arrows-left-right",
                  .variant = ButtonVariant::Ghost,
                  .tooltip = tr("apps.calc.conversion.swap"),
                  .onClick =
                      [this] {
                        std::swap(m_state.unitFrom, m_state.unitTo);
                        changed();
                      },
              }),
              ui::select({
                  .options = unitNames,
                  .selectedIndex = indexOf(m_state.unitTo, 1),
                  .flexGrow = 1.0F,
                  .onSelectionChanged = [this, units](std::size_t index, std::string_view) {
                    m_state.unitTo = units[index].title;
                    changed();
                  },
              })
          )
      );
    }
    panel->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
            ui::label({
                .out = &m_conversionLabel,
                .text = m_conversionResult,
                .fontSize = Style::fontSizeHeader * scale,
                .fontWeight = FontWeight::SemiBold,
                .maxLines = 2,
                .textAlign = TextAlign::End,
                .flexGrow = 1.0F,
            }),
            ui::button({
                .glyph = "copy",
                .variant = ButtonVariant::Ghost,
                .tooltip = tr("apps.calc.copy"),
                .onClick = [this] { m_app.clipboard().setClipboardText(m_conversionResult); },
            })
        )
    );
    if (currency) {
      const bool offline = m_app.config().config().shell.offlineMode;
      std::string status = offline ? tr("apps.calc.conversion.rates.offline")
          : m_fetchingRates        ? tr("apps.calc.conversion.rates.updating")
                                   : tr("apps.calc.conversion.rates.hint");
      panel->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
              ui::label({
                  .text = std::move(status),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .flexGrow = 1.0F,
              }),
              ui::button({
                  .text = tr("apps.calc.conversion.rates.refresh"),
                  .glyph = "refresh",
                  .enabled = !offline && !m_fetchingRates,
                  .variant = ButtonVariant::Secondary,
                  .onClick = [this] {
                    m_ratesFetched = false;
                    refreshExchangeRates();
                  },
              })
          )
      );
    }
    panel->addChild(ui::node({.flexGrow = 1.0F}));
    return panel;
  }

  std::unique_ptr<Node> CalculatorWindow::buildPreferences(float scale) {
    ScrollView* view = nullptr;
    auto scroll = ui::scrollView({.out = &view, .flexGrow = 1.0F});
    Flex* content = view->content();
    content->setGap(Style::spaceMd * scale);
    auto& s = m_state.settings;

    std::vector<std::string> angles{
        tr("apps.calc.preferences.angle.degrees"), tr("apps.calc.preferences.angle.radians"),
        tr("apps.calc.preferences.angle.gradians")
    };
    content->addChild(settingRow(
        tr("apps.calc.preferences.angle.label"),
        ui::select({
            .options = std::move(angles),
            .selectedIndex = static_cast<std::size_t>(s.angle),
            .width = 170.0F * scale,
            .onSelectionChanged =
                [this](std::size_t index, std::string_view) {
                  m_state.settings.angle = static_cast<AngleUnit>(index);
                  changed();
                },
        }),
        scale
    ));
    std::vector<std::string> formats{
        tr("apps.calc.preferences.format.automatic"), tr("apps.calc.preferences.format.fixed"),
        tr("apps.calc.preferences.format.scientific"), tr("apps.calc.preferences.format.engineering")
    };
    content->addChild(settingRow(
        tr("apps.calc.preferences.format.label"),
        ui::select({
            .options = std::move(formats),
            .selectedIndex = static_cast<std::size_t>(s.format),
            .width = 170.0F * scale,
            .onSelectionChanged =
                [this](std::size_t index, std::string_view) {
                  m_state.settings.format = static_cast<NumberFormat>(index);
                  changed();
                },
        }),
        scale
    ));
    content->addChild(settingRow(
        tr("apps.calc.preferences.decimals"),
        ui::stepper({
            .minValue = 0,
            .maxValue = 30,
            .value = s.decimals,
            .scale = scale,
            .onValueChanged =
                [this](int value) {
                  m_state.settings.decimals = value;
                  changed(false);
                },
        }),
        scale
    ));
    content->addChild(settingRow(
        tr("apps.calc.preferences.trailing-zeros"),
        ui::toggle({
            .checked = s.trailingZeros,
            .scale = scale,
            .onChange =
                [this](bool value) {
                  m_state.settings.trailingZeros = value;
                  changed(false);
                },
        }),
        scale
    ));
    content->addChild(settingRow(
        tr("apps.calc.preferences.thousands"),
        ui::toggle({
            .checked = s.thousandsSeparators,
            .scale = scale,
            .onChange =
                [this](bool value) {
                  m_state.settings.thousandsSeparators = value;
                  changed(false);
                },
        }),
        scale
    ));

    content->addChild(
        ui::label({
            .text = tr("apps.calc.preferences.variables.label"),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
        })
    );
    if (m_engine.variables().empty()) {
      content->addChild(caption(tr("apps.calc.preferences.variables.empty"), scale));
    }
    for (const auto& [name, expression] : m_engine.variables()) {
      content->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
              ui::label({
                  .text = std::format("{} = {}", name, expression),
                  .fontSize = Style::fontSizeBody * scale,
                  .flexGrow = 1.0F,
              }),
              ui::button({
                  .glyph = "trash",
                  .variant = ButtonVariant::Ghost,
                  .tooltip = tr("apps.calc.preferences.variables.remove"),
                  .onClick = [this, name] {
                    m_engine.removeVariable(name);
                    changed();
                  },
              })
          )
      );
    }
    content->addChild(
        ui::button({
            .text = tr("apps.calc.preferences.clear-history"),
            .glyph = "trash",
            .enabled = !m_state.history.empty(),
            .variant = ButtonVariant::Destructive,
            .onClick = [this] {
              m_state.history.clear();
              changed();
            },
        })
    );
    return scroll;
  }

  // ── Acciones ────────────────────────────────────────────────────────────────────────────────────────

  bool CalculatorWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    if (ctrl && (event.sym == XKB_KEY_q || event.sym == XKB_KEY_w)) {
      m_app.quit();
      return true;
    }
    if (ctrl && shift && (event.sym == XKB_KEY_C || event.sym == XKB_KEY_c)) {
      copyResult();
      return true;
    }
    if (event.utf32 >= 0x20U && !ctrl) {
      startFreshIfNeeded(event.utf32);
    }
    if (event.sym == XKB_KEY_Escape) {
      if (m_showPreferences) {
        m_showPreferences = false;
        m_app.requestRebuild();
      } else if (m_state.mode != Mode::Conversion && m_state.mode != Mode::Financial) {
        clear();
      }
      return true;
    }
    return false;
  }

  void CalculatorWindow::typeText(std::string_view text) {
    if (m_input == nullptr) {
      return;
    }
    // Teclas sintéticas: el Input inserta en el cursor y reemplaza la selección como al teclear.
    m_app.focus(m_input->inputArea());
    const auto chars = codepoints(text);
    if (!chars.empty()) {
      startFreshIfNeeded(chars.front());
    }
    for (const char32_t codepoint : chars) {
      m_app.input().keyEvent(0, codepoint, 0, true);
      m_app.input().keyEvent(0, codepoint, 0, false);
    }
    m_app.invalidate();
  }

  void CalculatorWindow::startFreshIfNeeded(char32_t first) {
    if (!m_justEvaluated || m_state.mode == Mode::Financial || m_state.mode == Mode::Conversion) {
      return;
    }
    m_justEvaluated = false;
    static constexpr std::u32string_view kContinues = U"+-*/^%!)×÷−·&|<>² ";
    if (kContinues.find(first) == std::u32string_view::npos) {
      clear();
    }
  }

  void CalculatorWindow::backspace() {
    if (m_input == nullptr) {
      return;
    }
    m_app.focus(m_input->inputArea());
    m_app.input().keyEvent(XKB_KEY_BackSpace, 0, 0, true);
    m_app.input().keyEvent(XKB_KEY_BackSpace, 0, 0, false);
    m_app.invalidate();
  }

  void CalculatorWindow::clear() {
    m_expression.clear();
    m_message.clear();
    m_historyCursor = -1;
    if (m_input != nullptr) {
      m_input->setValue("");
    }
    updatePreview();
  }

  void CalculatorWindow::updatePreview() {
    const auto preview = m_engine.preview(m_expression, effectiveSettings());
    m_message = preview.has_value() ? "= " + *preview : std::string();
    m_messageIsError = false;
    if (m_messageLabel != nullptr) {
      m_messageLabel->setText(m_message);
      m_messageLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_app.invalidate();
    }
  }

  void CalculatorWindow::submit() {
    const Settings settings = effectiveSettings();
    Result result;
    std::string expression;
    if (m_state.mode == Mode::Financial) {
      const auto function = static_cast<Financial>(m_state.financial);
      result = m_engine.financial(function, m_financialArgs, settings);
      std::string args;
      for (const auto& arg : m_financialArgs) {
        args += (args.empty() ? "" : "; ") + arg;
      }
      expression = std::format("{}({})", financialId(function), args);
    } else {
      expression = m_expression;
      result = m_engine.evaluate(m_expression, settings);
    }
    if (!result.ok) {
      if (expression.empty() || result.text.empty()) {
        return;
      }
      m_message = result.badVariable ? tr("apps.calc.error.variable", "name", result.text) : result.text;
      m_messageIsError = true;
      m_app.requestRebuild();
      return;
    }
    pushHistory(m_state, {.expression = expression, .result = result.text});
    m_lastResult = result.text;
    m_historyCursor = -1;
    if (m_state.mode == Mode::Financial) {
      m_message = "= " + result.text;
    } else {
      // Como GNOME Calculator: el resultado queda en la expresión para seguir calculando.
      m_expression = result.text;
      m_message = result.warning;
      m_justEvaluated = true;
    }
    m_messageIsError = false;
    changed();
  }

  void CalculatorWindow::recallHistory(int direction) {
    const int count = static_cast<int>(m_state.history.size());
    if (count == 0) {
      return;
    }
    int next = m_historyCursor < 0 ? (direction < 0 ? count - 1 : -1) : m_historyCursor + direction;
    if (next >= count) {
      next = -1;
    }
    if (next < -1 || (next < 0 && m_historyCursor < 0)) {
      return;
    }
    m_historyCursor = std::max(next, -1);
    m_expression = m_historyCursor >= 0 ? m_state.history[static_cast<std::size_t>(m_historyCursor)].expression : "";
    if (m_input != nullptr) {
      m_input->setValue(m_expression);
    }
    const int cursor = m_historyCursor;
    updatePreview();
    m_historyCursor = cursor;
  }

  void CalculatorWindow::copyResult() {
    const std::string text = m_state.mode == Mode::Conversion ? m_conversionResult
        : !m_lastResult.empty()                               ? m_lastResult
                                                              : m_expression;
    if (!text.empty()) {
      m_app.clipboard().setClipboardText(text);
    }
  }

  void CalculatorWindow::convertNow() {
    const Result result = m_engine.convert(
        m_conversionValue, m_state.unitCategory, m_state.unitFrom, m_state.unitTo, effectiveSettings()
    );
    m_conversionResult = result.ok ? result.text : std::string();
  }

  void CalculatorWindow::refreshExchangeRates() {
    if (m_fetchingRates || m_app.config().config().shell.offlineMode) {
      return;
    }
    const auto sources = m_engine.exchangeRateSources();
    if (sources.empty()) {
      return;
    }
    m_fetchingRates = true;
    m_ratesFetched = true;
    auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(sources.size()));
    for (const auto& [url, file] : sources) {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
      process::RunOptions options;
      options.timeout = std::chrono::seconds(20);
      // El aviso de fin llega en otro hilo; el motor solo se toca desde el principal.
      const bool started = process::runAsync(
          {"curl", "-fsSL", "--max-time", "15", "-o", file, url},
          {.onExit =
               [this, remaining](const process::RunResult&) {
                 if (--*remaining == 0) {
                   DeferredCall::callLater([this] {
                     m_engine.reloadExchangeRates();
                     m_fetchingRates = false;
                     m_app.requestRebuild();
                   });
                 }
               }},
          options
      );
      if (!started && --*remaining == 0) {
        m_fetchingRates = false;
      }
    }
  }

} // namespace okaru::calc
