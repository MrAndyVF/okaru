#include "shell/settings/okaru_settings_workspaces.h"

#include "compositors/compositor_detect.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_outputs.h"
#include "compositors/hyprland/okaru_hypr_workspaces.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/okaru_niri_config.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/settings/okaru_settings_displays.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <memory>
#include <set>

namespace settings {
  namespace {

    using okaru::hypr::WorkspaceDef;

    struct WorkspacesState {
      compositors::hyprland::HyprlandRuntime runtime;
      compositors::niri::NiriRuntime niri;
      std::function<void()> rebuild;
      std::string confirmRemove; // "<id>" con el primer clic de Quitar dado
      std::string error;
    };

    WorkspacesState& state() {
      static WorkspacesState s;
      return s;
    }

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.workspaces." + key); }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    std::string primaryMonitor(const std::vector<WorkspaceDef>& defs, const std::vector<std::string>& outputs) {
      std::string primary = okaru::hypr::readFilePrimary();
      if (primary.empty()) {
        primary = !defs.empty() ? defs.front().monitor : (!outputs.empty() ? outputs.front() : std::string());
      }
      return primary;
    }

    // Guarda en outputs.lua y aplica en vivo. Deja un solo "por defecto" por monitor.
    void save(std::vector<WorkspaceDef> defs, const std::string& primary) {
      std::set<std::string> withDefault;
      for (auto& def : defs) {
        if (def.isDefault && !withDefault.insert(def.monitor).second) {
          def.isDefault = false;
        }
      }
      auto& s = state();
      s.error.clear();
      if (auto err = okaru::hypr::writeWorkspaces(defs, primary)) {
        s.error = *err;
      } else if (auto live = okaru::hypr::applyWorkspacesLive(s.runtime, defs)) {
        s.error = *live;
      }
      rebuild();
    }

    std::vector<std::string> iconPath(int id) { return {"widget", "workspaces", "icons", std::to_string(id)}; }

    // Hyprland guarda el icono por id; Niri por nombre (sus workspaces con nombre no tienen id estable).
    std::vector<std::string> iconPath(const std::string& key) { return {"widget", "workspaces", "icons", key}; }

    std::string iconFor(const ConfigService& config, const std::string& key) {
      const auto& widgets = config.config().widgets;
      const auto it = widgets.find("workspaces");
      if (it == widgets.end()) {
        return {};
      }
      const auto map = it->second.getStringMap("icons");
      const auto icon = map.find(key);
      return icon == map.end() ? std::string() : icon->second;
    }

    std::string iconFor(const ConfigService& config, int id) { return iconFor(config, std::to_string(id)); }

    std::unique_ptr<Button>
    button(const std::string& text, const char* glyph, ButtonVariant variant, std::function<void()> onClick) {
      return ui::button({.text = text, .glyph = std::string(glyph), .variant = variant, .onClick = std::move(onClick)});
    }

    // ── Niri: una lista ordenada de workspaces con nombre (okaru/workspaces.kdl) ──────────────────────────

    using okaru::niri::NamedWorkspace;

    void saveNiri(
        const std::vector<NamedWorkspace>& before, const std::vector<NamedWorkspace>& after,
        const std::string& renamedFrom = {}, const std::string& renamedTo = {}
    ) {
      auto& s = state();
      s.error = okaru::niri::saveWorkspaces(s.niri, before, after, renamedFrom, renamedTo).value_or("");
      rebuild();
    }

    // Icono por nombre: abre el selector de glifos; el botón de al lado lo quita.
    void addIconButtons(Flex& row, ConfigService& config, const std::string& key) {
      const std::string icon = iconFor(config, key);
      row.addChild(
          ui::button({
              .glyph = icon.empty() ? std::string("photo-plus") : icon,
              .variant = ButtonVariant::Outline,
              .tooltip = tr("icon"),
              .onClick = [&config, key, icon] {
                GlyphPickerDialogOptions options;
                options.title = i18n::tr("settings.okaru.workspaces.icon");
                if (!icon.empty()) {
                  options.initialGlyph = icon;
                }
                (void)GlyphPickerDialog::open(
                    std::move(options), [&config, key](std::optional<GlyphPickerResult> result) {
                      if (result.has_value()) {
                        (void)config.setOverride(iconPath(key), result->name);
                        rebuild();
                      }
                    }
                );
              },
          })
      );
      if (!icon.empty()) {
        row.addChild(
            ui::button({
                .glyph = "x",
                .variant = ButtonVariant::Ghost,
                .tooltip = tr("icon-remove"),
                .onClick = [&config, key] {
                  (void)config.clearOverride(iconPath(key));
                  rebuild();
                },
            })
        );
      }
    }

    void addNiriWorkspaces(Flex& content, float scale, ConfigService& config, const std::vector<std::string>& outputs) {
      auto& s = state();
      const std::vector<NamedWorkspace> all = okaru::niri::readWorkspaces();
      Flex* body = addSettingsCard(content, tr("title"), scale);

      // Monitor de cada workspace: "Cualquiera" (sin open-on-output) o una salida conectada.
      std::vector<std::string> outputChoices = outputs;
      for (const auto& ws : all) {
        if (!ws.output.empty() && !std::ranges::contains(outputChoices, ws.output)) {
          outputChoices.push_back(ws.output);
        }
      }
      std::vector<std::string> outputLabels{tr("any-output")};
      outputLabels.insert(outputLabels.end(), outputChoices.begin(), outputChoices.end());

      for (std::size_t i = 0; i < all.size(); ++i) {
        const NamedWorkspace ws = all[i];
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
        row->addChild(makeLabel(
            std::to_string(i + 1), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
            FontWeight::Bold
        ));
        addIconButtons(*row, config, ws.name);

        // Nombre: Enter guarda; el icono pasa al nombre nuevo.
        row->addChild(
            ui::input({
                .value = ws.name,
                .placeholder = tr("name"),
                .fontSize = Style::fontSizeCaption * scale,
                .controlHeight = Style::controlHeightSm * scale,
                .flexGrow = 1.0F,
                .onSubmit = [&config, all, i, old = ws.name](const std::string& value) {
                  if (value == old
                      || !okaru::niri::isValidWorkspaceName(value)
                      || std::ranges::contains(all, value, &NamedWorkspace::name)) {
                    return;
                  }
                  auto after = all;
                  after[i].name = value;
                  if (const std::string icon = iconFor(config, old); !icon.empty()) {
                    (void)config.setOverride(iconPath(value), icon);
                    (void)config.clearOverride(iconPath(old));
                  }
                  saveNiri(all, after, old, value);
                },
            })
        );

        const auto selected = std::ranges::find(outputChoices, ws.output);
        row->addChild(
            ui::select({
                .options = outputLabels,
                .selectedIndex = ws.output.empty() || selected == outputChoices.end()
                    ? 0
                    : static_cast<std::size_t>(selected - outputChoices.begin()) + 1,
                .fontSize = Style::fontSizeCaption * scale,
                .controlHeight = Style::controlHeightSm * scale,
                .width = 160.0F * scale,
                .onSelectionChanged = [all, i, outputChoices](std::size_t index, std::string_view) {
                  auto after = all;
                  after[i].output = index == 0 ? std::string() : outputChoices[index - 1];
                  saveNiri(all, after);
                },
            })
        );

        // Orden: Niri crea los workspaces con nombre en el orden del archivo.
        row->addChild(
            ui::button({
                .glyph = "chevron-up",
                .enabled = i > 0,
                .variant = ButtonVariant::Ghost,
                .tooltip = tr("move-up"),
                .onClick = [all, i] {
                  auto after = all;
                  std::swap(after[i - 1], after[i]);
                  saveNiri(all, after);
                },
            })
        );
        row->addChild(
            ui::button({
                .glyph = "chevron-down",
                .enabled = i + 1 < all.size(),
                .variant = ButtonVariant::Ghost,
                .tooltip = tr("move-down"),
                .onClick = [all, i] {
                  auto after = all;
                  std::swap(after[i], after[i + 1]);
                  saveNiri(all, after);
                },
            })
        );

        const std::string token = "niri:" + ws.name;
        const bool armed = s.confirmRemove == token;
        row->addChild(button(
            armed ? tr("remove-confirm") : std::string(), "trash",
            armed ? ButtonVariant::Destructive : ButtonVariant::Ghost, [&config, all, i, token, armed, name = ws.name] {
              if (!armed) {
                state().confirmRemove = token;
                rebuild();
                return;
              }
              state().confirmRemove.clear();
              auto after = all;
              after.erase(after.begin() + static_cast<std::ptrdiff_t>(i));
              (void)config.clearOverride(iconPath(name));
              saveNiri(all, after);
            }
        ));
        body->addChild(std::move(row));
      }

      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .fillWidth = true});
      actions->addChild(button(tr("add"), "plus", ButtonVariant::Default, [all] {
        auto after = all;
        int n = static_cast<int>(all.size()) + 1;
        std::string name;
        do {
          name = std::format("{} {}", i18n::tr("settings.okaru.workspaces.new-name"), n++);
        } while (std::ranges::contains(all, name, &NamedWorkspace::name));
        after.push_back({.name = name});
        saveNiri(all, after);
      }));
      body->addChild(std::move(actions));
    }

    void addMonitorCard(
        Flex& content, float scale, ConfigService& config, const std::string& monitor,
        const std::vector<WorkspaceDef>& all, const std::string& primary
    ) {
      auto& s = state();
      std::vector<WorkspaceDef> mine;
      for (const auto& def : all) {
        if (def.monitor == monitor) {
          mine.push_back(def);
        }
      }
      std::ranges::sort(mine, {}, &WorkspaceDef::id);
      const std::string title =
          monitor == primary ? i18n::tr("settings.okaru.workspaces.monitor-primary", "monitor", monitor) : monitor;
      Flex* body = addSettingsCard(content, title, scale);

      for (const auto& def : mine) {
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
        row->addChild(makeLabel(
            std::to_string(def.id), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
            FontWeight::Bold
        ));

        // Icono: abre el selector de glifos; el botón de al lado lo quita.
        const std::string icon = iconFor(config, def.id);
        row->addChild(
            ui::button({
                .glyph = icon.empty() ? std::string("photo-plus") : icon,
                .variant = ButtonVariant::Outline,
                .tooltip = tr("icon"),
                .onClick = [&config, id = def.id, icon] {
                  GlyphPickerDialogOptions options;
                  options.title = i18n::tr("settings.okaru.workspaces.icon");
                  if (!icon.empty()) {
                    options.initialGlyph = icon;
                  }
                  (void)GlyphPickerDialog::open(
                      std::move(options), [&config, id](std::optional<GlyphPickerResult> result) {
                        if (result.has_value()) {
                          (void)config.setOverride(iconPath(id), result->name);
                          rebuild();
                        }
                      }
                  );
                },
            })
        );
        if (!icon.empty()) {
          row->addChild(
              ui::button({
                  .glyph = "x",
                  .variant = ButtonVariant::Ghost,
                  .tooltip = tr("icon-remove"),
                  .onClick = [&config, id = def.id] {
                    (void)config.clearOverride(iconPath(id));
                    rebuild();
                  },
              })
          );
        }

        // Nombre: Enter guarda.
        row->addChild(
            ui::input({
                .value = def.name,
                .placeholder = tr("name"),
                .fontSize = Style::fontSizeCaption * scale,
                .controlHeight = Style::controlHeightSm * scale,
                .flexGrow = 1.0F,
                .onSubmit = [all, id = def.id, primary](const std::string& value) {
                  if (!okaru::hypr::isValidWorkspaceName(value)) {
                    return;
                  }
                  auto defs = all;
                  for (auto& d : defs) {
                    if (d.id == id) {
                      d.name = value;
                    }
                  }
                  save(defs, primary);
                },
            })
        );

        row->addChild(
            makeLabel(tr("persistent"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
        );
        row->addChild(
            ui::toggle({
                .checked = def.persistent,
                .scale = scale,
                .onChange = [all, id = def.id, primary](bool on) {
                  auto defs = all;
                  for (auto& d : defs) {
                    if (d.id == id) {
                      d.persistent = on;
                    }
                  }
                  save(defs, primary);
                },
            })
        );
        row->addChild(
            makeLabel(tr("default"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
        );
        row->addChild(
            ui::toggle({
                .checked = def.isDefault,
                .scale = scale,
                .onChange = [all, id = def.id, monitor, primary](bool on) {
                  auto defs = all;
                  for (auto& d : defs) {
                    if (d.monitor == monitor) {
                      d.isDefault = on && d.id == id;
                    }
                  }
                  save(defs, primary);
                },
            })
        );

        const std::string token = std::to_string(def.id);
        const bool armed = s.confirmRemove == token;
        row->addChild(button(
            armed ? tr("remove-confirm") : std::string(), "trash",
            armed ? ButtonVariant::Destructive : ButtonVariant::Ghost,
            [&config, all, id = def.id, token, armed, primary] {
              if (!armed) {
                state().confirmRemove = token;
                rebuild();
                return;
              }
              state().confirmRemove.clear();
              auto defs = all;
              std::erase_if(defs, [id](const WorkspaceDef& d) { return d.id == id; });
              (void)config.clearOverride(iconPath(id));
              save(defs, primary);
            }
        ));
        body->addChild(std::move(row));
      }

      auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .fillWidth = true});
      actions->addChild(button(tr("add"), "plus", ButtonVariant::Default, [all, monitor, primary] {
        auto defs = all;
        const int id = okaru::hypr::nextWorkspaceId(defs, monitor);
        const bool first = std::ranges::none_of(defs, [&](const WorkspaceDef& d) { return d.monitor == monitor; });
        defs.push_back(
            {.id = id,
             .monitor = monitor,
             .name = std::format("{} {}", i18n::tr("settings.okaru.workspaces.new-name"), id),
             .persistent = true,
             .isDefault = first}
        );
        save(defs, primary);
      }));
      body->addChild(std::move(actions));
    }

  } // namespace

  void addSettingsWorkspaces(Flex& content, SettingsWorkspacesContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    const bool niri = compositors::isNiri() && s.niri.available();
    if (ctx.config == nullptr || (!niri && s.runtime.requestSocketPath().empty())) {
      Flex* body = addSettingsCard(content, tr("title"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.unsupported"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }
    if (niri && addNiriIncludeNotice(content, scale, s.rebuild)) {
      return;
    }

    auto intro = makeLabel(
        tr(niri ? "intro-niri" : "intro"), Style::fontSizeCaption * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    );
    intro->setMaxLines(3);
    content.addChild(std::move(intro));
    if (!s.error.empty()) {
      auto error =
          makeLabel(s.error, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold);
      error->setMaxLines(4);
      content.addChild(std::move(error));
    }
    if (niri) {
      addNiriWorkspaces(content, scale, *ctx.config, ctx.outputs);
      return;
    }

    auto defs = okaru::hypr::readWorkspaces();
    const std::string primary = primaryMonitor(defs, ctx.outputs);
    // Instalación limpia: Hyprland ya tiene el workspace 1 sin regla; se muestra para poder configurarlo y se
    // guarda en outputs.lua con el primer cambio.
    if (defs.empty() && !primary.empty()) {
      defs.push_back(
          {.id = 1,
           .monitor = primary,
           .name = std::format("{} 1", i18n::tr("settings.okaru.workspaces.new-name")),
           .persistent = true,
           .isDefault = true}
      );
    }
    // Monitores: los conectados y los que ya tienen workspaces; el principal primero.
    std::vector<std::string> monitors = ctx.outputs;
    for (const auto& def : defs) {
      if (std::ranges::find(monitors, def.monitor) == monitors.end()) {
        monitors.push_back(def.monitor);
      }
    }
    std::ranges::stable_sort(monitors, [&](const std::string& a, const std::string& b) {
      return a == primary && b != primary;
    });
    for (const auto& monitor : monitors) {
      addMonitorCard(content, scale, *ctx.config, monitor, defs, primary);
    }
  }

} // namespace settings
