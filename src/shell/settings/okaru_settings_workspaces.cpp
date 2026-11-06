#include "shell/settings/okaru_settings_workspaces.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_outputs.h"
#include "compositors/hyprland/okaru_hypr_workspaces.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
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

    std::string iconFor(const ConfigService& config, int id) {
      const auto& widgets = config.config().widgets;
      const auto it = widgets.find("workspaces");
      if (it == widgets.end()) {
        return {};
      }
      const auto map = it->second.getStringMap("icons");
      const auto icon = map.find(std::to_string(id));
      return icon == map.end() ? std::string() : icon->second;
    }

    std::unique_ptr<Button>
    button(const std::string& text, const char* glyph, ButtonVariant variant, std::function<void()> onClick) {
      return ui::button({.text = text, .glyph = std::string(glyph), .variant = variant, .onClick = std::move(onClick)});
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

    if (s.runtime.requestSocketPath().empty() || ctx.config == nullptr) {
      Flex* body = addSettingsCard(content, tr("title"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.hyprland-only"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }

    auto intro = makeLabel(tr("intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
    intro->setMaxLines(3);
    content.addChild(std::move(intro));
    if (!s.error.empty()) {
      auto error =
          makeLabel(s.error, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold);
      error->setMaxLines(4);
      content.addChild(std::move(error));
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
