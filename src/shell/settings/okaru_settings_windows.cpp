#include "shell/settings/okaru_settings_windows.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_animations.h"
#include "compositors/hyprland/okaru_hypr_options.h"
#include "i18n/i18n.h"
#include "shell/settings/okaru_settings_search.h"
#include "i18n/i18n_service.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/okaru_motion_preview.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    using okaru::hypr::OptionDesc;
    using okaru::hypr::OptionKind;

    // Cómo se edita una opción. Choice = texto con valores fijos (p. ej. general:layout).
    enum class Control : std::uint8_t { Toggle, Slider, Gaps, Enum, Choice, Text };

    struct Item {
      std::string_view name;
      Control control;
      double min = 0.0;
      double max = 1.0;
      double step = 1.0;
      std::vector<std::string_view> choices = {};
    };

    struct Section {
      std::string_view id;
      std::string_view onlyForLayout = {}; // si no está vacío, solo se muestra con ese layout activo
      std::vector<Item> items;
    };

    // Las opciones salen de `hyprctl descriptions` de Hyprland 0.57; aquí solo se eligen y agrupan.
    const std::vector<Section>& layoutSections() {
      static const std::vector<Section> kSections{
          {"general",
           {},
           {
               {"general:layout", Control::Choice, 0, 0, 0, {"dwindle", "master", "scrolling", "monocle"}},
               {"general:gaps_in", Control::Gaps, 0, 40},
               {"general:gaps_out", Control::Gaps, 0, 60},
               {"general:float_gaps", Control::Gaps, 0, 60},
               {"general:gaps_workspaces", Control::Slider, 0, 200},
               {"general:border_size", Control::Slider, 0, 10},
               {"general:resize_on_border", Control::Toggle},
               {"general:extend_border_grab_area", Control::Slider, 0, 50},
               {"general:hover_icon_on_border", Control::Toggle},
               {"general:resize_corner", Control::Enum},
               {"general:no_focus_fallback", Control::Toggle},
               {"general:modal_parent_blocking", Control::Toggle},
               {"general:allow_tearing", Control::Toggle},
           }},
          {"scrolling",
           "scrolling",
           {
               {"scrolling:column_width", Control::Slider, 0.1, 1.0, 0.05},
               {"scrolling:explicit_column_widths", Control::Text},
               {"scrolling:direction", Control::Choice, 0, 0, 0, {"right", "left", "down", "up"}},
               {"scrolling:fullscreen_on_one_column", Control::Toggle},
               {"scrolling:focus_fit_method", Control::Enum},
               {"scrolling:follow_focus", Control::Slider, 0, 2},
               {"scrolling:follow_min_visible", Control::Slider, 0.0, 1.0, 0.05},
               {"scrolling:wrap_focus", Control::Toggle},
               {"scrolling:wrap_swapcol", Control::Toggle},
           }},
          {"dwindle",
           "dwindle",
           {
               {"dwindle:force_split", Control::Enum},
               {"dwindle:preserve_split", Control::Toggle},
               {"dwindle:smart_split", Control::Toggle},
               {"dwindle:smart_resizing", Control::Toggle},
               {"dwindle:use_active_for_splits", Control::Toggle},
               {"dwindle:default_split_ratio", Control::Slider, 0.1, 1.9, 0.05},
               {"dwindle:split_width_multiplier", Control::Slider, 0.1, 3.0, 0.1},
               {"dwindle:split_bias", Control::Enum},
               {"dwindle:special_scale_factor", Control::Slider, 0.1, 1.0, 0.05},
               {"dwindle:permanent_direction_override", Control::Toggle},
               {"dwindle:precise_mouse_move", Control::Toggle},
           }},
          {"master",
           "master",
           {
               {"master:mfact", Control::Slider, 0.05, 0.95, 0.05},
               {"master:orientation", Control::Choice, 0, 0, 0, {"left", "right", "top", "bottom", "center"}},
               {"master:new_status", Control::Choice, 0, 0, 0, {"slave", "master", "inherit"}},
               {"master:new_on_top", Control::Toggle},
               {"master:new_on_active", Control::Choice, 0, 0, 0, {"none", "before", "after"}},
               {"master:allow_small_split", Control::Toggle},
               {"master:slave_count_for_center_master", Control::Slider, 0, 10},
               {"master:center_master_fallback", Control::Choice, 0, 0, 0, {"left", "right", "top", "bottom"}},
               {"master:center_ignores_reserved", Control::Toggle},
               {"master:smart_resizing", Control::Toggle},
               {"master:drop_at_cursor", Control::Toggle},
               {"master:always_keep_position", Control::Toggle},
               {"master:focus_master_on_close", Control::Toggle},
               {"master:special_scale_factor", Control::Slider, 0.1, 1.0, 0.05},
           }},
          {"snap",
           {},
           {
               {"general:snap:enabled", Control::Toggle},
               {"general:snap:window_gap", Control::Slider, 0, 50},
               {"general:snap:monitor_gap", Control::Slider, 0, 50},
               {"general:snap:border_overlap", Control::Toggle},
               {"general:snap:respect_gaps", Control::Toggle},
           }},
      };
      return kSections;
    }

    const std::vector<Section>& decorationSections() {
      static const std::vector<Section> kSections{
          {"corners",
           {},
           {
               // OKARU: sin redondeo de ventanas: las esquinas rectas son el estándar del sistema.
               {"decoration:border_part_of_window", Control::Toggle},
           }},
          {"opacity",
           {},
           {
               {"decoration:active_opacity", Control::Slider, 0.1, 1.0, 0.05},
               {"decoration:inactive_opacity", Control::Slider, 0.1, 1.0, 0.05},
               {"decoration:fullscreen_opacity", Control::Slider, 0.1, 1.0, 0.05},
           }},
          {"dim",
           {},
           {
               {"decoration:dim_inactive", Control::Toggle},
               {"decoration:dim_strength", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:dim_special", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:dim_around", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:dim_modal", Control::Toggle},
           }},
          {"blur",
           {},
           {
               {"decoration:blur:enabled", Control::Toggle},
               {"decoration:blur:variant", Control::Enum},
               {"decoration:blur:size", Control::Slider, 0, 20},
               {"decoration:blur:passes", Control::Slider, 0, 10},
               {"decoration:blur:noise", Control::Slider, 0.0, 0.2, 0.005},
               {"decoration:blur:contrast", Control::Slider, 0.0, 2.0, 0.05},
               {"decoration:blur:brightness", Control::Slider, 0.0, 2.0, 0.05},
               {"decoration:blur:vibrancy", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:blur:vibrancy_darkness", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:blur:ignore_opacity", Control::Toggle},
               {"decoration:blur:new_optimizations", Control::Toggle},
               {"decoration:blur:xray", Control::Toggle},
               {"decoration:blur:special", Control::Toggle},
               {"decoration:blur:popups", Control::Toggle},
               {"decoration:blur:popups_ignorealpha", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:blur:input_methods", Control::Toggle},
           }},
          {"shadow",
           {},
           {
               {"decoration:shadow:enabled", Control::Toggle},
               {"decoration:shadow:range", Control::Slider, 0, 100},
               {"decoration:shadow:render_power", Control::Slider, 1, 4},
               {"decoration:shadow:sharp", Control::Toggle},
               {"decoration:shadow:scale", Control::Slider, 0.0, 1.0, 0.05},
           }},
          {"effects",
           {},
           {
               {"decoration:glow:enabled", Control::Toggle},
               {"decoration:glow:range", Control::Slider, 0, 50},
               {"decoration:glow:render_power", Control::Slider, 1, 4},
               {"decoration:motion_blur:enabled", Control::Toggle},
               {"decoration:motion_blur:samples", Control::Slider, 1, 20},
               {"decoration:wobble:enabled", Control::Toggle},
               {"decoration:wobble:intensity", Control::Slider, 0.0, 1.0, 0.05},
               {"decoration:wobble:stiffness", Control::Slider, 10, 1000, 10},
               {"decoration:wobble:damping", Control::Slider, 1, 50},
           }},
      };
      return kSections;
    }

    const std::vector<Section>& deviceSections() {
      static const std::vector<Section> kSections{
          {"keyboard",
           {},
           {
               {"input:kb_layout", Control::Text},
               {"input:kb_variant", Control::Text},
               {"input:kb_options", Control::Text},
               {"input:repeat_rate", Control::Slider, 5, 80},
               {"input:repeat_delay", Control::Slider, 100, 1500, 50},
               {"input:numlock_by_default", Control::Toggle},
               {"input:resolve_binds_by_sym", Control::Toggle},
           }},
          {"mouse",
           {},
           {
               {"input:sensitivity", Control::Slider, -1.0, 1.0, 0.05},
               {"input:accel_profile", Control::Choice, 0, 0, 0, {"flat", "adaptive", "custom"}},
               {"input:force_no_accel", Control::Toggle},
               {"input:natural_scroll", Control::Toggle},
               {"input:scroll_factor", Control::Slider, 0.1, 5.0, 0.1},
               {"input:left_handed", Control::Toggle},
           }},
          {"touchpad",
           {},
           {
               {"input:touchpad:tap_to_click", Control::Toggle},
               {"input:touchpad:natural_scroll", Control::Toggle},
               {"input:touchpad:disable_while_typing", Control::Toggle},
               {"input:touchpad:scroll_factor", Control::Slider, 0.1, 5.0, 0.1},
               {"input:touchpad:clickfinger_behavior", Control::Toggle},
               {"input:touchpad:tap_and_drag", Control::Toggle},
               {"input:touchpad:drag_lock", Control::Slider, 0, 2},
               {"input:touchpad:middle_button_emulation", Control::Toggle},
               {"input:touchpad:drag_3fg", Control::Enum},
           }},
          {"focus",
           {},
           {
               {"input:follow_mouse", Control::Enum},
               {"input:mouse_refocus", Control::Toggle},
               {"input:focus_on_close", Control::Enum},
               {"input:float_switch_override_focus", Control::Slider, 0, 2},
               {"input:special_fallthrough", Control::Toggle},
           }},
      };
      return kSections;
    }

    // Sistema → Inicio y entorno: comportamiento general (misc). misc:vrr vive en Pantallas.
    const std::vector<Section>& behaviorSections() {
      static const std::vector<Section> kSections{
          {"behavior",
           {},
           {
               {"misc:mouse_move_enables_dpms", Control::Toggle},
               {"misc:key_press_enables_dpms", Control::Toggle},
               {"misc:focus_on_activate", Control::Toggle},
               {"misc:middle_click_paste", Control::Toggle},
               {"misc:close_special_on_empty", Control::Toggle},
               {"misc:enable_anr_dialog", Control::Toggle},
               {"misc:allow_session_lock_restore", Control::Toggle},
           }},
      };
      return kSections;
    }

    struct WindowsState {
      compositors::hyprland::HyprlandRuntime runtime;
      std::vector<OptionDesc> options;
      okaru::hypr::OptionValues overrides;
      okaru::hypr::OptionValues bases;    // valor de cada opción antes de su override
      std::string confirmReset;           // opción que espera confirmación para restablecer
      okaru::hypr::AnimChoice animations; // estilo de animación guardado en layout.lua
      std::shared_ptr<OkaruMotionPreview::Clock> previewClock = std::make_shared<OkaruMotionPreview::Clock>();
      bool playPreview = false; // reproducir en cuanto se dibuje la página tras un cambio
      std::string error;
      std::function<void()> rebuild;
    };

    WindowsState& state() {
      static WindowsState s;
      return s;
    }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    // Ventanas guarda en layout.lua; Entrada → Teclado y ratón, en inputs.lua.
    std::string g_targetFile = "layout";
    std::filesystem::path targetFile() { return okaru::hypr::okaFilePath(g_targetFile); }

    const OptionDesc* findOption(std::string_view name) {
      const auto& options = state().options;
      const auto it = std::ranges::find(options, name, &OptionDesc::name);
      return it != options.end() ? &*it : nullptr;
    }

    // Aplica en vivo y guarda el override. El archivo cambia y Hyprland recarga solo.
    // Si el valor nuevo es el que había antes del override, el override se quita: no queda nada
    // que restablecer.
    void setOption(std::string_view name, nlohmann::json value) {
      auto& s = state();
      const std::string key(name);
      s.error.clear();
      s.confirmReset.clear();
      if (!s.overrides.contains(key)) {
        if (const auto* desc = findOption(name)) {
          s.bases[key] = desc->current;
        }
      }
      if (auto err = okaru::hypr::applyOption(s.runtime, name, value)) {
        s.error = *err;
        rebuild();
        return;
      }
      if (const auto base = s.bases.find(key);
          base != s.bases.end() && okaru::hypr::sameOptionValue(value, base->second)) {
        s.overrides.erase(key);
        s.bases.erase(key);
      } else {
        s.overrides[key] = std::move(value);
      }
      if (auto err = okaru::hypr::writeOptionOverrides(targetFile(), s.overrides, s.bases)) {
        s.error = *err;
      }
      rebuild();
    }

    void resetOption(std::string_view name) {
      auto& s = state();
      s.error.clear();
      s.confirmReset.clear();
      s.overrides.erase(std::string(name));
      s.bases.erase(std::string(name));
      if (auto err = okaru::hypr::writeOptionOverrides(targetFile(), s.overrides, s.bases)) {
        s.error = *err;
      }
      // Sin el override, el valor vuelve a ser el del resto de la config; Hyprland lo relee al recargar.
      (void)s.runtime.request("reload");
      rebuild();
    }

    // general:snap:window_gap -> settings.okaru.hypr-label.general.snap.window-gap
    std::string i18nKey(std::string_view tree, std::string_view name) {
      std::string key = "settings.okaru." + std::string(tree) + ".";
      for (const char c : name) {
        key += c == ':' ? '.' : (c == '_' ? '-' : c);
      }
      return key;
    }

    std::string humanize(std::string_view text) {
      std::string out(text);
      std::ranges::replace(out, '_', ' ');
      if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
      }
      return out;
    }

    std::string optionLabel(std::string_view name) {
      const auto key = i18nKey("hypr-label", name);
      if (!i18n::Service::instance().lookup(key).empty()) {
        return i18n::tr(key);
      }
      return humanize(name.substr(name.rfind(':') + 1));
    }

    std::string optionDescription(const OptionDesc& desc) {
      const auto key = i18nKey("hypr-desc", desc.name);
      if (!i18n::Service::instance().lookup(key).empty()) {
        return i18n::tr(key);
      }
      std::string text = desc.description;
      if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
      }
      return text;
    }

    double numberOf(const nlohmann::json& value) {
      if (value.is_number()) {
        return value.get<double>();
      }
      if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
      }
      if (value.is_string()) {
        return std::strtod(value.get<std::string>().c_str(), nullptr);
      }
      return 0.0;
    }

    std::string formatNumber(double value, double step) {
      if (step >= 1.0) {
        return std::to_string(static_cast<long long>(std::llround(value)));
      }
      const int decimals = step >= 0.1 ? 1 : (step >= 0.01 ? 2 : 3);
      return std::format("{:.{}f}", value, decimals);
    }

    std::unique_ptr<Node> makeControl(const Item& item, const OptionDesc& desc, float scale) {
      const std::string name(item.name);
      const auto& current = desc.current;
      switch (item.control) {
      case Control::Toggle:
        return ui::toggle({
            .checked = current.is_boolean() ? current.get<bool>() : numberOf(current) != 0.0,
            .scale = scale,
            .onChange = [name](bool on) { setOption(name, on); },
        });
      case Control::Slider:
      case Control::Gaps: {
        const double value =
            item.control == Control::Gaps ? okaru::hypr::uniformGap(current).value_or(0) : numberOf(current);
        const bool integer = item.step >= 1.0;
        Label* valueLabel = nullptr;
        auto valueText = makeLabel(
            formatNumber(value, item.step), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant)
        );
        valueLabel = valueText.get();
        valueText->setMinWidth(kValueSlotWidth * scale);
        valueText->setTextAlign(TextAlign::End);
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
        row->addChild(std::move(valueText));
        auto pending = std::make_shared<double>(value);
        row->addChild(
            ui::slider({
                .minValue = item.min,
                .maxValue = item.max,
                .step = item.step,
                .value = value,
                .width = 200.0F * scale,
                .onValueChanged =
                    [valueLabel, pending, step = item.step](double v) {
                      *pending = v;
                      valueLabel->setText(formatNumber(v, step));
                    },
                .onDragEnd =
                    [name, pending, integer] {
                      if (integer) {
                        setOption(name, static_cast<long long>(std::llround(*pending)));
                      } else {
                        setOption(name, std::round(*pending * 1000.0) / 1000.0);
                      }
                    },
            })
        );
        return row;
      }
      case Control::Enum: {
        std::vector<std::string> labels;
        std::size_t selected = 0;
        const int value = static_cast<int>(numberOf(current));
        for (std::size_t i = 0; i < desc.choices.size(); ++i) {
          labels.push_back(humanize(desc.choices[i].first));
          if (desc.choices[i].second == value) {
            selected = i;
          }
        }
        std::vector<int> values;
        for (const auto& choice : desc.choices) {
          values.push_back(choice.second);
        }
        return ui::select({
            .options = std::move(labels),
            .selectedIndex = selected,
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .width = 200.0F * scale,
            .onSelectionChanged = [name, values](std::size_t index, std::string_view) {
              setOption(name, values[index]);
            },
        });
      }
      case Control::Choice: {
        std::vector<std::string> labels;
        std::vector<std::string> values;
        std::size_t selected = 0;
        const std::string value = current.is_string() ? current.get<std::string>() : current.dump();
        for (std::size_t i = 0; i < item.choices.size(); ++i) {
          values.emplace_back(item.choices[i]);
          labels.push_back(humanize(item.choices[i]));
          if (item.choices[i] == value) {
            selected = i;
          }
        }
        return ui::select({
            .options = std::move(labels),
            .selectedIndex = selected,
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .width = 200.0F * scale,
            .onSelectionChanged = [name, values](std::size_t index, std::string_view) {
              setOption(name, values[index]);
            },
        });
      }
      case Control::Text: {
        Input* input = nullptr;
        auto field = ui::input({
            .out = &input,
            .value = current.is_string() ? current.get<std::string>() : current.dump(),
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .width = 200.0F * scale,
        });
        input->setOnSubmit([name](const std::string& text) { setOption(name, text); });
        return field;
      }
      }
      return ui::spacer();
    }

    void addItemRow(Flex& body, const Item& item, float scale) {
      const OptionDesc* desc = findOption(item.name);
      if (desc == nullptr) {
        return; // opción que esta versión de Hyprland no tiene
      }
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(
              optionLabel(item.name), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface),
              FontWeight::Bold
          )
      );
      auto description = makeLabel(
          optionDescription(*desc), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      description->setMaxLines(3);
      text->addChild(std::move(description));

      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      row->addChild(std::move(text));
      const std::string name(item.name);
      if (state().confirmReset == name) {
        // Segundo paso: confirmar en la misma fila, en lugar del control.
        const auto smallButton =
            [scale](std::string label, std::string glyph, ButtonVariant variant, std::function<void()> onClick) {
              return ui::button({
                  .text = std::move(label),
                  .glyph = std::move(glyph),
                  .fontSize = Style::fontSizeCaption * scale,
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = variant,
                  .minHeight = Style::controlHeightSm * scale,
                  .paddingH = Style::spaceSm * scale,
                  .radius = Style::scaledRadiusMd(scale),
                  .onClick = std::move(onClick),
              });
            };
        row->addChild(makeLabel(
            i18n::tr("settings.okaru.windows.reset-confirm"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
        ));
        row->addChild(smallButton(i18n::tr("settings.okaru.windows.cancel"), "circle-x", ButtonVariant::Ghost, [] {
          state().confirmReset.clear();
          rebuild();
        }));
        row->addChild(
            smallButton(i18n::tr("settings.okaru.windows.reset-action"), "restore", ButtonVariant::Destructive, [name] {
              resetOption(name);
            })
        );
        body.addChild(std::move(row));
        return;
      }
      if (state().overrides.contains(name)) {
        row->addChild(
            ui::button({
                .glyph = "restore",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.okaru.windows.reset"),
                .minWidth = Style::controlHeightSm * scale,
                .minHeight = Style::controlHeightSm * scale,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = [name] {
                  state().confirmReset = name;
                  rebuild();
                },
            })
        );
      }
      row->addChild(makeControl(item, *desc, scale));
      body.addChild(std::move(row));
    }

    // --- Animaciones -------------------------------------------------------------------------

    // Movimiento de la shell que corresponde a un estilo: misma duración que Hyprland, y
    // "Mínimo" activa el movimiento reducido (transiciones instantáneas).
    void syncShellMotion(const SettingsWindowsContext& ctx, const okaru::hypr::AnimChoice& choice, bool enabled) {
      if (!ctx.setShellOverrides) {
        return;
      }
      if (okaru::hypr::isCustom(choice)) {
        // Personalizado: la shell se ajusta en su propia tarjeta; aquí solo el encendido general.
        ctx.setShellOverrides({{{"shell", "animation", "enabled"}, enabled}});
        return;
      }
      const double speed = std::clamp(1.0 / choice.duration, 0.1, 4.0);
      const bool shellEnabled = enabled && choice.preset != "minimal";
      ctx.setShellOverrides({
          {{"shell", "animation", "enabled"}, shellEnabled},
          {{"shell", "animation", "speed"}, std::round(speed * 100.0) / 100.0},
          {{"shell", "animation", "style"}, choice.preset == "minimal" ? std::string{} : choice.preset},
      });
    }

    void applyAnimationChoice(const SettingsWindowsContext& ctx, okaru::hypr::AnimChoice choice, bool enabled) {
      auto& s = state();
      s.error.clear();
      if (auto err = okaru::hypr::writeAnimationChoice(choice)) {
        s.error = *err;
      } else if (auto liveErr = okaru::hypr::applyAnimationsLive(s.runtime, choice)) {
        s.error = *liveErr;
      }
      s.animations = choice;
      s.playPreview = true;
      syncShellMotion(ctx, choice, enabled);
      rebuild();
    }

    // --- Personalizado: detalle por animación, separado en compositor y shell ---------------

    struct LeafInfo {
      std::string_view leaf;
      std::string_view kind; // qué estilos acepta: windows, layers, workspaces o vacío
    };

    const std::vector<LeafInfo>& editableLeaves() {
      static const std::vector<LeafInfo> kLeaves{
          {"windowsIn", "windows"},
          {"windowsOut", "windows"},
          {"windowsMove", ""},
          {"layersIn", "layers"},
          {"layersOut", "layers"},
          {"fadeIn", ""},
          {"fadeOut", ""},
          {"fadeSwitch", ""},
          {"fadeDim", ""},
          {"fadePopups", ""},
          {"border", ""},
          {"workspaces", "workspaces"},
          {"specialWorkspace", "workspaces"},
          {"zoomFactor", ""},
          {"monitorAdded", ""},
      };
      return kLeaves;
    }

    std::vector<std::string> stylesFor(std::string_view kind) {
      if (kind == "windows") {
        return {"", "slide", "popin", "popin 80%", "popin 90%", "gnomed"};
      }
      if (kind == "layers") {
        return {"", "slide", "popin", "fade"};
      }
      if (kind == "workspaces") {
        return {"", "slide", "slidevert", "fade", "slidefade", "slidefadevert"};
      }
      return {};
    }

    void writeCustom(okaru::hypr::AnimChoice choice) {
      auto& s = state();
      s.error.clear();
      s.confirmReset.clear();
      choice.preset = "custom";
      if (auto err = okaru::hypr::writeAnimationChoice(choice)) {
        s.error = *err;
      } else {
        (void)okaru::hypr::applyAnimationsLive(s.runtime, choice);
      }
      s.animations = choice;
      rebuild();
    }

    void
    addCustomAnimationCards(Flex& content, const SettingsWindowsContext& ctx, const okaru::hypr::AnimChoice& choice) {
      auto& s = state();
      const float scale = ctx.scale;
      const auto live = okaru::hypr::queryAnimations(s.runtime);

      // Curvas que se pueden elegir: las del usuario (bezier y resortes en uso) y las de OKARU.
      std::vector<std::string> curves;
      for (const auto& name : live.beziers) {
        if (!name.starts_with("__") && !name.starts_with("scrolloverview")) {
          curves.push_back(name);
        }
      }
      for (const auto& name : live.springs) {
        curves.push_back("spring:" + name);
      }
      for (const auto& curve : okaru::hypr::okaruCurves()) {
        if (!std::ranges::contains(curves, curve.name)) {
          curves.push_back(curve.name);
        }
      }
      const auto curveLabel = [](const std::string& name) {
        return name.starts_with("spring:") ? name.substr(7) + " (" + i18n::tr("settings.okaru.animations.spring") + ")"
                                           : name;
      };

      Flex* compositor = addSettingsCard(content, i18n::tr("settings.okaru.animations.compositor"), scale);
      for (const auto& info : editableLeaves()) {
        const auto liveIt = std::ranges::find(live.leaves, info.leaf, &okaru::hypr::LiveLeaf::leaf);
        if (liveIt == live.leaves.end()) {
          continue; // animación que esta versión de Hyprland no tiene
        }
        const std::string leaf(info.leaf);
        const auto editedIt = std::ranges::find(choice.custom, leaf, &okaru::hypr::AnimLeaf::leaf);
        const bool edited = editedIt != choice.custom.end();
        // Valor actual: el editado si lo hay; si no, el efectivo en Hyprland.
        const okaru::hypr::AnimLeaf current = edited
            ? *editedIt
            : okaru::hypr::AnimLeaf{
                  .leaf = leaf,
                  .enabled = liveIt->enabled,
                  .speed = liveIt->speed > 0.0 ? liveIt->speed : 3.0,
                  .curve =
                      liveIt->spring ? "spring:" + liveIt->curve : (liveIt->curve.empty() ? "default" : liveIt->curve),
                  .style = liveIt->style,
              };
        const auto update = [choice, current](const std::function<void(okaru::hypr::AnimLeaf&)>& change) {
          okaru::hypr::AnimChoice next = choice;
          okaru::hypr::AnimLeaf value = current;
          change(value);
          std::erase_if(next.custom, [&](const okaru::hypr::AnimLeaf& l) { return l.leaf == value.leaf; });
          next.custom.push_back(value);
          writeCustom(next);
        };

        // Encabezado: nombre, descripción, ↺ (con confirmación) e interruptor.
        auto text = ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
            makeLabel(
                i18n::tr("settings.okaru.animations.leaves." + leaf), Style::fontSizeBody * scale,
                colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
            )
        );
        auto desc = makeLabel(
            i18n::tr("settings.okaru.animations.leaves-desc." + leaf), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant)
        );
        desc->setMaxLines(2);
        text->addChild(std::move(desc));
        auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
        header->addChild(std::move(text));
        const std::string confirmKey = "anim:" + leaf;
        if (s.confirmReset == confirmKey) {
          header->addChild(makeLabel(
              i18n::tr("settings.okaru.windows.reset-confirm"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          ));
          header->addChild(
              ui::button({
                  .text = i18n::tr("settings.okaru.windows.cancel"),
                  .glyph = "circle-x",
                  .fontSize = Style::fontSizeCaption * scale,
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = ButtonVariant::Ghost,
                  .minHeight = Style::controlHeightSm * scale,
                  .paddingH = Style::spaceSm * scale,
                  .radius = Style::scaledRadiusMd(scale),
                  .onClick = [] {
                    state().confirmReset.clear();
                    rebuild();
                  },
              })
          );
          header->addChild(
              ui::button({
                  .text = i18n::tr("settings.okaru.windows.reset-action"),
                  .glyph = "restore",
                  .fontSize = Style::fontSizeCaption * scale,
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = ButtonVariant::Destructive,
                  .minHeight = Style::controlHeightSm * scale,
                  .paddingH = Style::spaceSm * scale,
                  .radius = Style::scaledRadiusMd(scale),
                  .onClick = [choice, leaf] {
                    okaru::hypr::AnimChoice next = choice;
                    std::erase_if(next.custom, [&](const okaru::hypr::AnimLeaf& l) { return l.leaf == leaf; });
                    writeCustom(next);
                  },
              })
          );
          compositor->addChild(std::move(header));
          continue;
        }
        if (edited) {
          header->addChild(
              ui::button({
                  .glyph = "restore",
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = ButtonVariant::Ghost,
                  .tooltip = i18n::tr("settings.okaru.windows.reset"),
                  .minWidth = Style::controlHeightSm * scale,
                  .minHeight = Style::controlHeightSm * scale,
                  .radius = Style::scaledRadiusMd(scale),
                  .onClick = [confirmKey] {
                    state().confirmReset = confirmKey;
                    rebuild();
                  },
              })
          );
        }
        header->addChild(
            ui::toggle({
                .checked = current.enabled,
                .scale = scale,
                .onChange = [update](bool on) { update([on](okaru::hypr::AnimLeaf& l) { l.enabled = on; }); },
            })
        );
        compositor->addChild(std::move(header));
        if (!current.enabled) {
          continue;
        }

        // Detalle: velocidad, curva y estilo.
        auto details =
            ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
        const bool spring = current.curve.starts_with("spring:")
            || std::ranges::any_of(okaru::hypr::okaruCurves(), [&](const okaru::hypr::AnimCurve& c) {
                              return c.name == current.curve && c.spring;
                            });
        auto pending = std::make_shared<double>(current.speed);
        auto speedText = makeLabel(
            std::format("{:.1f}", current.speed), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant)
        );
        Label* speedLabel = speedText.get();
        speedText->setMinWidth(kValueSlotWidth * scale);
        details->addChild(makeLabel(
            i18n::tr("settings.okaru.animations.speed"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant)
        ));
        details->addChild(std::move(speedText));
        details->addChild(
            ui::slider({
                .minValue = 0.5,
                .maxValue = 20.0,
                .step = 0.5,
                .value = current.speed,
                .enabled = !spring, // los resortes se rigen por su física, no por la velocidad
                .width = Style::sliderDefaultWidth * scale,
                .onValueChanged =
                    [pending, speedLabel](double v) {
                      *pending = v;
                      speedLabel->setText(std::format("{:.1f}", v));
                    },
                .onDragEnd =
                    [pending, update] {
                      update([v = std::round(*pending * 10.0) / 10.0](okaru::hypr::AnimLeaf& l) { l.speed = v; });
                    },
            })
        );
        std::vector<std::string> curveLabels;
        std::size_t curveIndex = 0;
        for (std::size_t i = 0; i < curves.size(); ++i) {
          curveLabels.push_back(curveLabel(curves[i]));
          if (curves[i] == current.curve) {
            curveIndex = i;
          }
        }
        details->addChild(
            ui::select({
                .options = std::move(curveLabels),
                .selectedIndex = curveIndex,
                .fontSize = Style::fontSizeCaption * scale,
                .controlHeight = Style::controlHeightSm * scale,
                .width = kControlWidth * scale,
                .onSelectionChanged = [curves, update](std::size_t index, std::string_view) {
                  update([name = curves[index]](okaru::hypr::AnimLeaf& l) { l.curve = name; });
                },
            })
        );
        const auto styles = stylesFor(info.kind);
        if (!styles.empty()) {
          std::vector<std::string> styleLabels;
          std::size_t styleIndex = 0;
          for (std::size_t i = 0; i < styles.size(); ++i) {
            styleLabels.push_back(styles[i].empty() ? i18n::tr("settings.okaru.animations.style-default") : styles[i]);
            if (styles[i] == current.style) {
              styleIndex = i;
            }
          }
          details->addChild(
              ui::select({
                  .options = std::move(styleLabels),
                  .selectedIndex = styleIndex,
                  .fontSize = Style::fontSizeCaption * scale,
                  .controlHeight = Style::controlHeightSm * scale,
                  .width = kControlWidth * scale,
                  .onSelectionChanged = [styles, update](std::size_t index, std::string_view) {
                    update([style = styles[index]](okaru::hypr::AnimLeaf& l) { l.style = style; });
                  },
              })
          );
        }
        compositor->addChild(std::move(details));
      }

      // Shell: encendido, velocidad y curva del motor de animación de la interfaz.
      Flex* shell = addSettingsCard(content, i18n::tr("settings.okaru.animations.shell"), scale);
      const auto setShell = ctx.setShellOverrides;
      const auto shellRow = [scale,
                             shell](const std::string& title, const std::string& d, std::unique_ptr<Node> control) {
        auto text = ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
            makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
        );
        auto dl = makeLabel(d, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
        dl->setMaxLines(3);
        text->addChild(std::move(dl));
        auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
        line->addChild(std::move(text));
        line->addChild(std::move(control));
        shell->addChild(std::move(line));
      };
      shellRow(
          i18n::tr("settings.okaru.animations.shell-enabled"), i18n::tr("settings.okaru.animations.shell-enabled-desc"),
          ui::toggle({
              .checked = ctx.shellAnimationsEnabled,
              .scale = scale,
              .onChange = [setShell](bool on) {
                if (setShell) {
                  setShell({{{"shell", "animation", "enabled"}, on}});
                }
              },
          })
      );
      auto pendingSpeed = std::make_shared<double>(ctx.shellAnimationSpeed);
      auto speedText = makeLabel(
          std::format("{:.2f}×", ctx.shellAnimationSpeed), Style::fontSizeCaption * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      Label* speedLabel = speedText.get();
      speedText->setMinWidth(kValueSlotWidth * scale);
      auto speedControl = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      speedControl->addChild(std::move(speedText));
      speedControl->addChild(
          ui::slider({
              .minValue = 0.25,
              .maxValue = 3.0,
              .step = 0.05,
              .value = ctx.shellAnimationSpeed,
              .width = 200.0F * scale,
              .onValueChanged =
                  [pendingSpeed, speedLabel](double v) {
                    *pendingSpeed = v;
                    speedLabel->setText(std::format("{:.2f}×", v));
                  },
              .onDragEnd =
                  [pendingSpeed, setShell] {
                    if (setShell) {
                      setShell({{{"shell", "animation", "speed"}, std::round(*pendingSpeed * 100.0) / 100.0}});
                    }
                  },
          })
      );
      shellRow(
          i18n::tr("settings.okaru.animations.shell-speed"), i18n::tr("settings.okaru.animations.shell-speed-desc"),
          std::move(speedControl)
      );
      std::vector<std::string> styleIds{""};
      std::vector<std::string> styleLabels{i18n::tr("settings.okaru.animations.shell-curve-default")};
      for (const auto& p : okaru::hypr::animationPresets()) {
        if (p.id != "minimal") {
          styleIds.push_back(p.id);
          styleLabels.push_back(i18n::tr("settings.okaru.animations.presets." + p.id));
        }
      }
      const auto styleIt = std::ranges::find(styleIds, ctx.shellAnimationStyle);
      shellRow(
          i18n::tr("settings.okaru.animations.shell-curve"), i18n::tr("settings.okaru.animations.shell-curve-desc"),
          ui::select({
              .options = std::move(styleLabels),
              .selectedIndex = styleIt == styleIds.end() ? 0 : static_cast<std::size_t>(styleIt - styleIds.begin()),
              .fontSize = Style::fontSizeCaption * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .width = 200.0F * scale,
              .onSelectionChanged = [styleIds, setShell](std::size_t index, std::string_view) {
                if (setShell) {
                  setShell({{{"shell", "animation", "style"}, styleIds[index]}});
                }
              },
          })
      );
    }

    // Alcance de Super+Alt+Space / widget hypr_layout (shell.layout_per_workspace).
    void addLayoutScopeCard(Flex& content, const SettingsWindowsContext& ctx) {
      const float scale = ctx.scale;
      Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.windows.layout-switch.title"), scale);
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(
              i18n::tr("settings.okaru.windows.layout-switch.scope"), Style::fontSizeBody * scale,
              colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          )
      );
      auto desc = makeLabel(
          i18n::tr("settings.okaru.windows.layout-switch.scope-desc"), Style::fontSizeCaption * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      desc->setMaxLines(3);
      text->addChild(std::move(desc));
      auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      line->addChild(std::move(text));
      line->addChild(ui::select({
          .options = std::vector<std::string>{i18n::tr("settings.okaru.windows.layout-switch.workspace"),
                                              i18n::tr("settings.okaru.windows.layout-switch.global")},
          .selectedIndex = ctx.layoutPerWorkspace ? std::size_t{0} : std::size_t{1},
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .width = 200.0F * scale,
          .onSelectionChanged = [setShell = ctx.setShellOverrides](std::size_t index, std::string_view) {
            if (setShell) {
              setShell({{{"shell", "layout_per_workspace"}, index == 0}});
            }
          },
      }));
      body->addChild(std::move(line));
    }

    void addAnimationsPage(Flex& content, const SettingsWindowsContext& ctx) {
      auto& s = state();
      const float scale = ctx.scale;
      s.animations = okaru::hypr::readAnimationChoice();
      const okaru::hypr::AnimChoice choice = s.animations;

      bool hyprEnabled = true;
      if (const auto* desc = findOption("animations:enabled"); desc != nullptr) {
        hyprEnabled = desc->current.is_boolean() ? desc->current.get<bool>() : numberOf(desc->current) != 0.0;
      }
      const bool enabled = hyprEnabled && (ctx.shellAnimationsEnabled || choice.preset == "minimal");

      Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.animations.style"), scale);

      // Vista previa con la hoja que más se ve: el movimiento de ventanas.
      const auto preset = okaru::hypr::resolvePreset(choice);
      const auto leaf = std::ranges::find(preset.leaves, std::string("windowsMove"), &okaru::hypr::AnimLeaf::leaf);
      if (!choice.preset.empty() && leaf != preset.leaves.end() && leaf->enabled) {
        const auto curve = std::ranges::find(preset.curves, leaf->curve, &okaru::hypr::AnimCurve::name);
        const double duration = okaru::hypr::leafDurationMs(preset, *leaf);
        if (std::exchange(s.playPreview, false)) {
          // Mostrar el cambio sin tener que tocar: arranca en cuanto la vista previa entra en la escena.
          s.previewClock->towardEnd = !s.previewClock->towardEnd;
          s.previewClock->startedAt = std::chrono::steady_clock::now();
          s.previewClock->running = true;
        }
        body->addChild(
            std::make_unique<OkaruMotionPreview>(
                [c = *curve, duration](double ms) { return okaru::hypr::curveProgress(c, ms, duration); }, duration,
                i18n::tr("settings.okaru.animations.tap-to-play"), scale, s.previewClock
            )
        );
      } else {
        body->addChild(makeLabel(
            i18n::tr(
                choice.preset.empty() ? "settings.okaru.animations.preview-custom"
                                      : "settings.okaru.animations.preview-none"
            ),
            Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
        ));
      }

      // Estilos: botones que se acomodan en varias filas.
      auto presets =
          ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      std::vector<std::string> ids{"custom"};
      for (const auto& p : okaru::hypr::animationPresets()) {
        ids.push_back(p.id);
      }
      for (const auto& id : ids) {
        const bool selected = okaru::hypr::isCustom(choice) ? id == "custom" : id == choice.preset;
        presets->addChild(
            ui::button({
                .text = i18n::tr("settings.okaru.animations.presets." + id),
                .fontSize = Style::fontSizeCaption * scale,
                .variant = selected ? ButtonVariant::Primary : ButtonVariant::Outline,
                .minHeight = Style::controlHeightSm * scale,
                .paddingH = Style::spaceMd * scale,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = [id, choice, enabled, setShell = ctx.setShellOverrides] {
                  okaru::hypr::AnimChoice next = choice;
                  next.preset = id;
                  next.custom.clear(); // elegir Personalizado vuelve a tu config sin ediciones
                  applyAnimationChoice(SettingsWindowsContext{.setShellOverrides = setShell}, next, enabled);
                },
            })
        );
      }
      body->addChild(std::move(presets));
      auto description = makeLabel(
          i18n::tr(
              "settings.okaru.animations.descriptions."
              + (okaru::hypr::isCustom(choice) ? std::string("custom") : choice.preset)
          ),
          Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      description->setMaxLines(3);
      body->addChild(std::move(description));

      const auto setShell = ctx.setShellOverrides;
      const auto row = [scale](const std::string& title, const std::string& desc, std::unique_ptr<Node> control) {
        auto text = ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
            makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
        );
        if (!desc.empty()) {
          auto d = makeLabel(desc, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
          d->setMaxLines(3);
          text->addChild(std::move(d));
        }
        auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
        line->addChild(std::move(text));
        line->addChild(std::move(control));
        return line;
      };

      // Encendido general: Hyprland y la shell a la vez.
      body->addChild(
          row(i18n::tr("settings.okaru.animations.enabled"), i18n::tr("settings.okaru.animations.enabled-desc"),
              ui::toggle({
                  .checked = enabled,
                  .scale = scale,
                  .onChange = [choice, setShell](bool on) {
                    setOption("animations:enabled", on);
                    syncShellMotion(SettingsWindowsContext{.setShellOverrides = setShell}, choice, on);
                  },
              }))
      );

      const bool hasPreset = !okaru::hypr::isCustom(choice);
      if (!hasPreset) {
        addCustomAnimationCards(content, ctx, choice);
        return;
      }
      // Duración: escala Hyprland (curvas y resortes) y la shell.
      auto pending = std::make_shared<double>(choice.duration);
      Label* durationLabel = nullptr;
      auto durationText = makeLabel(
          std::format("{:.1f}×", choice.duration), Style::fontSizeCaption * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      durationLabel = durationText.get();
      durationText->setMinWidth(kValueSlotWidth * scale);
      auto durationControl = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      durationControl->addChild(std::move(durationText));
      durationControl->addChild(
          ui::slider({
              .minValue = 0.5,
              .maxValue = 2.0,
              .step = 0.1,
              .value = choice.duration,
              .enabled = hasPreset,
              .width = 200.0F * scale,
              .onValueChanged =
                  [pending, durationLabel](double v) {
                    *pending = v;
                    durationLabel->setText(std::format("{:.1f}×", v));
                  },
              .onDragEnd =
                  [pending, choice, enabled, setShell] {
                    okaru::hypr::AnimChoice next = choice;
                    next.duration = std::round(*pending * 10.0) / 10.0;
                    applyAnimationChoice(SettingsWindowsContext{.setShellOverrides = setShell}, next, enabled);
                  },
          })
      );
      body->addChild(
          row(i18n::tr("settings.okaru.animations.duration"), i18n::tr("settings.okaru.animations.duration-desc"),
              std::move(durationControl))
      );

      // Cambio de escritorio.
      static const std::vector<std::string> kWorkspaceStyles{"slidevert", "slide", "fade"};
      std::vector<std::string> wsLabels;
      for (const auto& style : kWorkspaceStyles) {
        wsLabels.push_back(i18n::tr("settings.okaru.animations.workspaces-" + style));
      }
      const auto wsIt = std::ranges::find(kWorkspaceStyles, choice.workspaces);
      body->addChild(
          row(i18n::tr("settings.okaru.animations.workspaces"), i18n::tr("settings.okaru.animations.workspaces-desc"),
              ui::select({
                  .options = std::move(wsLabels),
                  .selectedIndex =
                      wsIt == kWorkspaceStyles.end() ? 0 : static_cast<std::size_t>(wsIt - kWorkspaceStyles.begin()),
                  .fontSize = Style::fontSizeCaption * scale,
                  .controlHeight = Style::controlHeightSm * scale,
                  .enabled = hasPreset && choice.preset != "minimal",
                  .width = 200.0F * scale,
                  .onSelectionChanged = [choice, enabled, setShell](std::size_t index, std::string_view) {
                    okaru::hypr::AnimChoice next = choice;
                    next.workspaces = kWorkspaceStyles[index];
                    applyAnimationChoice(SettingsWindowsContext{.setShellOverrides = setShell}, next, enabled);
                  },
              }))
      );
    }

  } // namespace

  // OKARU: opciones de estas páginas para la búsqueda de los ajustes (okaru_settings_search).
  std::vector<OkaruSearchItem> okaruWindowsSearchItems() {
    std::vector<OkaruSearchItem> items;
    const std::pair<const char*, const std::vector<Section>*> pages[] = {
        {"window-layout", &layoutSections()},
        {"window-decoration", &decorationSections()},
        {"input-devices", &deviceSections()},
        {"system-environment", &behaviorSections()},
    };
    for (const auto& [page, sections] : pages) {
      for (const auto& section : *sections) {
        for (const auto& item : section.items) {
          std::string keywords(item.name.substr(item.name.rfind(':') + 1));
          std::ranges::replace(keywords, '_', ' ');
          items.push_back({
              .section = page,
              .label = optionLabel(item.name),
              .detail = i18n::tr("settings.okaru.windows.sections." + std::string(section.id)),
              .keywords = keywords,
          });
        }
      }
    }
    return items;
  }

  void addSettingsWindows(Flex& content, SettingsWindowsContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    if (s.runtime.requestSocketPath().empty()) {
      Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.windows.title"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.hyprland-only"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }

    g_targetFile = ctx.page == "input-devices" ? "inputs" : "layout";
    s.options = okaru::hypr::queryOptions(s.runtime);
    s.bases.clear();
    s.overrides = okaru::hypr::readOptionOverrides(targetFile(), &s.bases);

    if (!s.error.empty()) {
      content.addChild(makeLabel(
          i18n::tr("settings.okaru.displays.error", "error", s.error), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::Error), FontWeight::Bold
      ));
    }

    std::string activeLayout;
    if (const auto* layout = findOption("general:layout"); layout != nullptr && layout->current.is_string()) {
      activeLayout = layout->current.get<std::string>();
    }

    if (ctx.page == "window-animations") {
      addAnimationsPage(content, ctx);
      return;
    }

    const auto& sections = ctx.page == "input-devices"
        ? deviceSections()
        : (ctx.page == "system-behavior" ? behaviorSections()
                                         : (ctx.page == "window-decoration" ? decorationSections() : layoutSections()));
    for (const auto& section : sections) {
      if (!section.onlyForLayout.empty() && section.onlyForLayout != activeLayout) {
        continue;
      }
      Flex* body =
          addSettingsCard(content, i18n::tr("settings.okaru.windows.sections." + std::string(section.id)), scale);
      for (const auto& item : section.items) {
        addItemRow(*body, item, scale);
      }
    }
    if (ctx.page == "window-layout") {
      addLayoutScopeCard(content, ctx);
    }
    if (ctx.page == "system-behavior") {
      return; // va dentro de Inicio y entorno, que pone su propio pie
    }
    content.addChild(makeLabel(
        i18n::tr(ctx.page == "input-devices" ? "settings.okaru.windows.footer-input" : "settings.okaru.windows.footer"),
        Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }

} // namespace settings
