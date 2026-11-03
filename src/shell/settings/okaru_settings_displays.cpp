#include "shell/settings/okaru_settings_displays.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_outputs.h"
#include "core/timer_manager.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/okaru_monitor_arrangement.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace settings {
  namespace {

    using okaru::hypr::OutputInfo;
    using okaru::hypr::OutputRule;

    constexpr int kConfirmSeconds = 15;

    // Estado de la página. Vive mientras corre la shell: la ventana de settings se reconstruye entera
    // en cada cambio, y la cuenta regresiva debe seguir (y revertir) aunque se cierre la ventana.
    struct DisplaysState {
      compositors::hyprland::HyprlandRuntime runtime;
      std::vector<OutputInfo> infos;
      std::vector<OutputRule> applied; // lo que está en vivo y confirmado
      std::vector<OutputRule> pending; // lo que el usuario está editando
      std::vector<OutputRule> revertTo;
      std::string appliedPrimary; // monitor principal (cursor.default_monitor)
      std::string pendingPrimary;
      std::string revertPrimary;
      std::string selected;
      std::string error;
      bool loaded = false;
      bool confirming = false;
      int secondsLeft = 0;
      Timer countdown;
      std::function<void()> rebuild;
    };

    DisplaysState& state() {
      static DisplaysState s;
      return s;
    }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    void reload() {
      auto& s = state();
      s.infos = okaru::hypr::queryOutputs(s.runtime);
      const auto fileRules = okaru::hypr::readFileRules();
      s.applied.clear();
      for (const auto& info : s.infos) {
        s.applied.push_back(okaru::hypr::ruleFor(info, fileRules));
      }
      s.pending = s.applied;
      s.appliedPrimary = okaru::hypr::queryPrimary(s.runtime);
      if (s.appliedPrimary.empty()) {
        s.appliedPrimary = okaru::hypr::readFilePrimary();
      }
      s.pendingPrimary = s.appliedPrimary;
      s.loaded = true;
      const bool selectedExists = std::ranges::contains(s.applied, s.selected, &OutputRule::output);
      if (!selectedExists && !s.applied.empty()) {
        const auto focused = std::ranges::find_if(s.infos, &OutputInfo::focused);
        s.selected = focused != s.infos.end() ? focused->name : s.applied.front().output;
      }
    }

    OutputRule* pendingRule(const std::string& name) {
      auto& rules = state().pending;
      const auto it = std::ranges::find(rules, name, &OutputRule::output);
      return it != rules.end() ? &*it : nullptr;
    }

    const OutputInfo* infoFor(const std::string& name) {
      const auto& infos = state().infos;
      const auto it = std::ranges::find(infos, name, &OutputInfo::name);
      return it != infos.end() ? &*it : nullptr;
    }

    bool dirty() {
      const auto& s = state();
      return s.pending != s.applied || s.pendingPrimary != s.appliedPrimary;
    }

    void finishConfirm(bool keep) {
      auto& s = state();
      s.countdown.stop();
      s.confirming = false;
      if (keep) {
        if (auto err = okaru::hypr::writeFileRules(s.pending, s.pendingPrimary)) {
          s.error = *err;
        }
        s.applied = s.pending;
        s.appliedPrimary = s.pendingPrimary;
      } else {
        if (auto err = okaru::hypr::applyLive(s.runtime, s.revertTo, s.revertPrimary)) {
          s.error = *err;
        }
        s.applied = s.revertTo;
        s.pending = s.revertTo;
        s.appliedPrimary = s.revertPrimary;
        s.pendingPrimary = s.revertPrimary;
      }
      reload();
      rebuild();
    }

    void applyPending() {
      auto& s = state();
      s.error.clear();
      okaru::hypr::normalizePositions(s.pending);
      s.revertTo = s.applied;
      s.revertPrimary = s.appliedPrimary;
      if (auto err = okaru::hypr::applyLive(s.runtime, s.pending, s.pendingPrimary)) {
        s.error = *err;
        (void)okaru::hypr::applyLive(s.runtime, s.revertTo, s.revertPrimary);
        s.pending = s.applied;
        s.pendingPrimary = s.appliedPrimary;
        rebuild();
        return;
      }
      s.confirming = true;
      s.secondsLeft = kConfirmSeconds;
      s.countdown.startRepeating(std::chrono::seconds(1), [] {
        auto& st = state();
        if (--st.secondsLeft <= 0) {
          finishConfirm(/*keep=*/false);
          return;
        }
        rebuild();
      });
      rebuild();
    }

    // "1920x1080@144.00Hz" -> {"1920x1080", "144.00"}
    std::pair<std::string, std::string> splitMode(const std::string& mode) {
      const auto at = mode.find('@');
      if (at == std::string::npos) {
        return {mode, ""};
      }
      std::string refresh = mode.substr(at + 1);
      if (refresh.ends_with("Hz")) {
        refresh.resize(refresh.size() - 2);
      }
      return {mode.substr(0, at), refresh};
    }

    std::string formatScale(double scale) {
      std::string text = std::format("{:.2f}", scale);
      while (text.ends_with('0')) {
        text.pop_back();
      }
      if (text.ends_with('.')) {
        text.pop_back();
      }
      return text + "×";
    }

    std::string monitorDetail(const OutputRule& rule) {
      if (rule.disabled) {
        return i18n::tr("settings.okaru.displays.off");
      }
      if (!rule.mirror.empty()) {
        return i18n::tr("settings.okaru.displays.mirror-of", "name", rule.mirror);
      }
      const auto [size, refresh] = splitMode(rule.mode);
      const double hz = std::strtod(refresh.c_str(), nullptr);
      std::string detail = std::format("{} · {} Hz", size, static_cast<int>(std::lround(hz)));
      if (std::abs(rule.scale - 1.0) > 0.001) {
        detail += " · " + formatScale(rule.scale);
      }
      return detail;
    }

    // Una fila de ajuste: título, descripción opcional y el control a la derecha.
    std::unique_ptr<Flex>
    settingRow(float scale, const std::string& title, const std::string& description, std::unique_ptr<Node> control) {
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (!description.empty()) {
        auto desc =
            makeLabel(description, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
        desc->setMaxLines(3);
        text->addChild(std::move(desc));
      }
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      row->addChild(std::move(text));
      row->addChild(std::move(control));
      return row;
    }

    std::unique_ptr<Node> selectControl(
        float scale, std::vector<std::string> labels, std::size_t selected, std::function<void(std::size_t)> onChange,
        bool enabled = true
    ) {
      return ui::select({
          .options = std::move(labels),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .enabled = enabled,
          .width = kControlWidth * scale,
          .onSelectionChanged = [onChange = std::move(onChange)](std::size_t index, std::string_view) {
            onChange(index);
          },
      });
    }

    // Edita la regla pendiente del monitor seleccionado y redibuja.
    void edit(const std::string& name, const std::function<void(OutputRule&)>& change) {
      if (auto* rule = pendingRule(name)) {
        change(*rule);
        rebuild();
      }
    }

    void addArrangementCard(Flex& content, float scale) {
      auto& s = state();
      Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.displays.arrangement"), scale);

      std::vector<OkaruMonitorArrangement::Monitor> monitors;
      for (const auto& rule : s.pending) {
        const auto size = okaru::hypr::logicalSize(rule);
        monitors.push_back({
            .name = rule.output,
            .detail = rule.output == s.pendingPrimary && !rule.disabled
                ? monitorDetail(rule) + " · " + i18n::tr("settings.okaru.displays.primary-badge")
                : monitorDetail(rule),
            .x = static_cast<double>(rule.x),
            .y = static_cast<double>(rule.y),
            .width = size.width,
            .height = size.height,
            .disabled = rule.disabled,
            .mirrored = !rule.mirror.empty(),
        });
      }
      auto arrangement = std::make_unique<OkaruMonitorArrangement>(std::move(monitors), s.selected, scale);
      arrangement->setOnSelect([](const std::string& name) {
        state().selected = name;
        rebuild();
      });
      arrangement->setOnMoved([](const std::string& name, int x, int y) {
        edit(name, [x, y](OutputRule& rule) {
          rule.x = x;
          rule.y = y;
        });
      });
      body->addChild(std::move(arrangement));
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.arrangement-hint"), Style::fontSizeCaption * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
    }

    void addMonitorCard(Flex& content, float scale) {
      auto& s = state();
      const OutputRule* rulePtr = pendingRule(s.selected);
      const OutputInfo* info = infoFor(s.selected);
      if (rulePtr == nullptr || info == nullptr) {
        return;
      }
      const OutputRule rule = *rulePtr;
      const std::string name = rule.output;
      const std::string title = info->description.empty() ? name : std::format("{} — {}", name, info->description);
      Flex* body = addSettingsCard(content, title, scale);

      // Encendido. No se puede apagar el último monitor encendido.
      const auto enabledCount = std::ranges::count_if(s.pending, [](const OutputRule& r) { return !r.disabled; });
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.enabled"), i18n::tr("settings.okaru.displays.enabled-desc"),
          ui::toggle({
              .checked = !rule.disabled,
              .enabled = rule.disabled || enabledCount > 1,
              .scale = scale,
              .onChange = [name](bool on) { edit(name, [on](OutputRule& r) { r.disabled = !on; }); },
          })
      ));
      if (rule.disabled) {
        return;
      }

      // Monitor principal: recibe el cursor y el foco al iniciar sesión.
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.primary"), i18n::tr("settings.okaru.displays.primary-desc"),
          ui::toggle({
              .checked = s.pendingPrimary == name,
              .scale = scale,
              .onChange = [name](bool on) {
                auto& st = state();
                if (on) {
                  st.pendingPrimary = name;
                } else if (st.pendingPrimary == name) {
                  st.pendingPrimary.clear();
                }
                rebuild();
              },
          })
      ));

      // Resolución y frecuencia, a partir de los modos que reporta el monitor.
      const auto [currentSize, currentRefresh] = splitMode(rule.mode);
      std::vector<std::string> sizes;
      for (const auto& mode : info->modes) {
        const auto size = splitMode(mode).first;
        if (!std::ranges::contains(sizes, size)) {
          sizes.push_back(size);
        }
      }
      if (!std::ranges::contains(sizes, currentSize)) {
        sizes.insert(sizes.begin(), currentSize);
      }
      std::vector<std::string> refreshes;
      std::vector<std::string> refreshModes;
      for (const auto& mode : info->modes) {
        const auto [size, refresh] = splitMode(mode);
        if (size == currentSize) {
          refreshes.push_back(refresh + " Hz");
          refreshModes.push_back(mode);
        }
      }
      if (refreshModes.empty()) {
        refreshes.push_back(currentRefresh + " Hz");
        refreshModes.push_back(rule.mode);
      }
      const auto sizeIndex = static_cast<std::size_t>(std::ranges::find(sizes, currentSize) - sizes.begin());
      const auto refreshIt = std::ranges::find(refreshModes, rule.mode);
      const std::size_t refreshIndex =
          refreshIt == refreshModes.end() ? 0 : static_cast<std::size_t>(refreshIt - refreshModes.begin());

      const std::vector<std::string> modes = info->modes;
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.resolution"), {},
          selectControl(scale, sizes, sizeIndex, [name, sizes, modes](std::size_t index) {
            const std::string& size = sizes[index];
            // La frecuencia más alta disponible para esa resolución.
            std::string best;
            double bestHz = -1.0;
            for (const auto& mode : modes) {
              const auto [s2, r2] = splitMode(mode);
              const double hz = std::strtod(r2.c_str(), nullptr);
              if (s2 == size && hz > bestHz) {
                bestHz = hz;
                best = mode;
              }
            }
            if (!best.empty()) {
              edit(name, [best](OutputRule& r) { r.mode = best; });
            }
          })
      ));
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.refresh"), {},
          selectControl(scale, refreshes, refreshIndex, [name, refreshModes](std::size_t index) {
            edit(name, [mode = refreshModes[index]](OutputRule& r) { r.mode = mode; });
          })
      ));

      // Escala. Hyprland ajusta solo las que no dan un tamaño lógico entero.
      static const std::vector<double> kScales{1.0, 1.25, 1.333333, 1.5, 1.6, 1.666667, 1.75, 2.0, 2.5, 3.0};
      std::vector<std::string> scaleLabels;
      std::size_t scaleIndex = 0;
      double bestDiff = 1e9;
      for (std::size_t i = 0; i < kScales.size(); ++i) {
        scaleLabels.push_back(formatScale(kScales[i]));
        if (std::abs(kScales[i] - rule.scale) < bestDiff) {
          bestDiff = std::abs(kScales[i] - rule.scale);
          scaleIndex = i;
        }
      }
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.scale"), i18n::tr("settings.okaru.displays.scale-desc"),
          selectControl(scale, scaleLabels, scaleIndex, [name](std::size_t index) {
            edit(name, [value = kScales[index]](OutputRule& r) { r.scale = value; });
          })
      ));

      // Rotación (wl_output_transform 0-7).
      std::vector<std::string> transforms;
      for (int i = 0; i < 8; ++i) {
        transforms.push_back(i18n::tr(std::format("settings.okaru.displays.transform-{}", i)));
      }
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.rotation"), {},
          selectControl(
              scale, transforms, static_cast<std::size_t>(std::clamp(rule.transform, 0, 7)), [name](std::size_t index) {
                edit(name, [index](OutputRule& r) { r.transform = static_cast<int>(index); });
              }
          )
      ));

      // Espejo.
      std::vector<std::string> mirrorLabels{i18n::tr("settings.okaru.displays.mirror-none")};
      std::vector<std::string> mirrorTargets{""};
      for (const auto& other : s.pending) {
        if (other.output != name && !other.disabled && other.mirror.empty()) {
          mirrorLabels.push_back(other.output);
          mirrorTargets.push_back(other.output);
        }
      }
      const auto mirrorIt = std::ranges::find(mirrorTargets, rule.mirror);
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.mirror"), i18n::tr("settings.okaru.displays.mirror-desc"),
          selectControl(
              scale, mirrorLabels,
              mirrorIt == mirrorTargets.end() ? 0 : static_cast<std::size_t>(mirrorIt - mirrorTargets.begin()),
              [name, mirrorTargets](std::size_t index) {
                edit(name, [target = mirrorTargets[index]](OutputRule& r) { r.mirror = target; });
              }
          )
      ));

      // Frecuencia variable.
      std::vector<std::string> vrrLabels;
      for (int i = -1; i <= 3; ++i) {
        vrrLabels.push_back(
            i18n::tr(std::format("settings.okaru.displays.vrr-{}", i < 0 ? "inherit" : std::to_string(i)))
        );
      }
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.vrr"),
          info->vrrCapable ? i18n::tr("settings.okaru.displays.vrr-desc")
                           : i18n::tr("settings.okaru.displays.vrr-unsupported"),
          selectControl(
              scale, vrrLabels, static_cast<std::size_t>(std::clamp(rule.vrr, -1, 3) + 1),
              [name](std::size_t index) {
                edit(name, [index](OutputRule& r) { r.vrr = static_cast<int>(index) - 1; });
              },
              info->vrrCapable
          )
      ));

      // Profundidad de color.
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.bitdepth"), i18n::tr("settings.okaru.displays.bitdepth-desc"),
          selectControl(scale, {"8 bits", "10 bits"}, rule.bitdepth == 10 ? 1 : 0, [name](std::size_t index) {
            edit(name, [index](OutputRule& r) { r.bitdepth = index == 1 ? 10 : 8; });
          })
      ));

      // Gestión de color.
      static const std::vector<std::string> kCm{"srgb", "wide", "edid", "dcip3", "dp3", "adobe", "hdr", "hdredid"};
      std::vector<std::string> cmLabels;
      for (const auto& cm : kCm) {
        cmLabels.push_back(i18n::tr("settings.okaru.displays.cm-" + cm));
      }
      const auto cmIt = std::ranges::find(kCm, rule.cm);
      const bool hdr = rule.cm.starts_with("hdr");
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.cm"),
          info->hdrCapable ? i18n::tr("settings.okaru.displays.cm-desc")
                           : i18n::tr("settings.okaru.displays.cm-desc-nohdr"),
          selectControl(
              scale, cmLabels, cmIt == kCm.end() ? 0 : static_cast<std::size_t>(cmIt - kCm.begin()),
              [name](std::size_t index) { edit(name, [index](OutputRule& r) { r.cm = kCm[index]; }); }
          )
      ));
      if (hdr) {
        const auto sliderRow = [&](const std::string& key, double value, double min, double max,
                                   std::function<void(OutputRule&, double)> apply) {
          body->addChild(settingRow(
              scale, i18n::tr("settings.okaru.displays." + key), {},
              ui::slider({
                  .minValue = min,
                  .maxValue = max,
                  .step = 0.05,
                  .value = value,
                  .width = Style::sliderDefaultWidth * scale,
                  .onValueChanged =
                      [name, apply](double v) {
                        if (auto* r = pendingRule(name)) {
                          apply(*r, v);
                        }
                      },
                  .onDragEnd = [] { rebuild(); },
              })
          ));
        };
        sliderRow("sdr-brightness", rule.sdrBrightness, 0.5, 2.0, [](OutputRule& r, double v) { r.sdrBrightness = v; });
        sliderRow("sdr-saturation", rule.sdrSaturation, 0.5, 1.5, [](OutputRule& r, double v) { r.sdrSaturation = v; });
      }

      // Área reservada: margen fijo por lado, sumado al que piden barras y paneles.
      auto sides = ui::row({.align = FlexAlign::End, .gap = Style::spaceSm * scale});
      const auto addSide = [&](const char* key, int value, int OutputRule::* field) {
        auto column = ui::column(
            {.align = FlexAlign::Start, .gap = Style::spaceXs * scale},
            makeLabel(
                i18n::tr(std::string("settings.okaru.displays.reserved-") + key), Style::fontSizeCaption * scale,
                colorSpecFromRole(ColorRole::OnSurfaceVariant)
            )
        );
        column->addChild(
            ui::stepper({
                .minValue = 0,
                .maxValue = 2000,
                .step = 1,
                .value = value,
                .scale = scale,
                .onValueCommitted = [name, field](int v) { edit(name, [field, v](OutputRule& r) { r.*field = v; }); },
            })
        );
        sides->addChild(std::move(column));
      };
      addSide("top", rule.reservedTop, &OutputRule::reservedTop);
      addSide("right", rule.reservedRight, &OutputRule::reservedRight);
      addSide("bottom", rule.reservedBottom, &OutputRule::reservedBottom);
      addSide("left", rule.reservedLeft, &OutputRule::reservedLeft);
      body->addChild(settingRow(
          scale, i18n::tr("settings.okaru.displays.reserved"), i18n::tr("settings.okaru.displays.reserved-desc"),
          std::move(sides)
      ));

      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.position", "x", std::to_string(rule.x), "y", std::to_string(rule.y)),
          Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
    }

    void addActionBar(Flex& content, float scale) {
      auto& s = state();
      const bool hasChanges = dirty();
      if (!s.confirming && !hasChanges && s.error.empty()) {
        return;
      }
      auto bar = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .fillWidth = true,
          .configure = [scale](Flex& row) {
            row.setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
            row.setCardStyle(scale, kCardFillOpacity);
          },
      });
      std::string message;
      if (s.confirming) {
        message = i18n::tr("settings.okaru.displays.confirm", "seconds", std::to_string(s.secondsLeft));
      } else if (!s.error.empty()) {
        message = i18n::tr("settings.okaru.displays.error", "error", s.error);
      } else {
        message = i18n::tr("settings.okaru.displays.pending");
      }
      auto label = makeLabel(
          message, Style::fontSizeBody * scale,
          colorSpecFromRole(s.error.empty() || s.confirming ? ColorRole::OnSurface : ColorRole::Error), FontWeight::Bold
      );
      label->setFlexGrow(1.0F);
      bar->addChild(std::move(label));

      const auto button = [scale](const std::string& text, ButtonVariant variant, std::function<void()> onClick) {
        return ui::button({
            .text = text,
            .fontSize = Style::fontSizeCaption * scale,
            .variant = variant,
            .minHeight = Style::controlHeightSm * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = std::move(onClick),
        });
      };
      if (s.confirming) {
        bar->addChild(button(i18n::tr("settings.okaru.displays.revert"), ButtonVariant::Outline, [] {
          finishConfirm(false);
        }));
        bar->addChild(button(i18n::tr("settings.okaru.displays.keep"), ButtonVariant::Primary, [] {
          finishConfirm(true);
        }));
      } else {
        if (hasChanges) {
          bar->addChild(button(i18n::tr("settings.okaru.displays.discard"), ButtonVariant::Outline, [] {
            auto& st = state();
            st.pending = st.applied;
            st.pendingPrimary = st.appliedPrimary;
            st.error.clear();
            rebuild();
          }));
          bar->addChild(button(i18n::tr("settings.okaru.displays.apply"), ButtonVariant::Primary, [] {
            applyPending();
          }));
        } else {
          bar->addChild(button(i18n::tr("settings.okaru.displays.dismiss"), ButtonVariant::Ghost, [] {
            state().error.clear();
            rebuild();
          }));
        }
      }
      content.addChild(std::move(bar));
    }

  } // namespace

  void addSettingsDisplays(Flex& content, SettingsDisplaysContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    if (s.runtime.requestSocketPath().empty()) { // solo hace falta el socket de peticiones
      Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.displays.arrangement"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.hyprland-only"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }
    // Con cambios sin aplicar o esperando confirmación no se relee: se perdería lo editado.
    if (!s.loaded || (!s.confirming && !dirty())) {
      reload();
    }

    addActionBar(content, scale);
    addArrangementCard(content, scale);
    addMonitorCard(content, scale);
  }

} // namespace settings
