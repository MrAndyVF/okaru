#include "shell/settings/okaru_settings_startup.h"

#include "i18n/i18n.h"
#include "shell/settings/okaru_settings_windows.h"
#include "shell/settings/settings_content_common.h"
#include "system/desktop_entry.h"
#include "system/okaru_autostart.h"
#include "system/okaru_uwsm_env.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <regex>
#include <string>

namespace settings {
  namespace {

    enum class AddMode { None, App, Command };

    struct StartupState {
      std::function<void()> rebuild;
      std::string error;
      std::string confirmRemove; // id de autostart o "env:CLAVE" con el primer clic dado
      AddMode addMode = AddMode::None;
      std::string appFilter;
      std::string draftName;
      std::string draftExec;
      std::map<std::string, std::string> envDrafts; // CLAVE -> valor en edición
      std::string newKey;
      std::string newValue;
    };

    StartupState& state() {
      static StartupState s;
      return s;
    }

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.startup." + key); }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    void report(const std::optional<std::string>& error) {
      state().error = error.value_or(std::string());
      rebuild();
    }

    // Los códigos %u, %F… de Exec no tienen sentido al iniciar sesión.
    std::string stripFieldCodes(std::string exec) {
      static const std::regex kCodes(R"(\s*%[fFuUdDnNickvm])");
      return std::regex_replace(exec, kCodes, "");
    }

    std::unique_ptr<Flex> row(float scale, const std::string& title, const std::string& detail) {
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (!detail.empty()) {
        auto label = makeLabel(detail, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
        label->setMaxLines(2);
        text->addChild(std::move(label));
      }
      auto out = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      out->addChild(std::move(text));
      return out;
    }

    std::unique_ptr<Button>
    button(const std::string& text, const char* glyph, ButtonVariant variant, std::function<void()> onClick) {
      return ui::button({.text = text, .glyph = std::string(glyph), .variant = variant, .onClick = std::move(onClick)});
    }

    std::unique_ptr<Node> input(
        float scale, const std::string& value, const std::string& placeholder, float width,
        std::function<void(const std::string&)> onChange, std::function<void(const std::string&)> onSubmit = nullptr
    ) {
      return ui::input({
          .value = value,
          .placeholder = placeholder,
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .width = width > 0 ? std::optional(width * scale) : std::nullopt,
          .flexGrow = width > 0 ? std::nullopt : std::optional(1.0F),
          .onChange = std::move(onChange),
          .onSubmit = std::move(onSubmit),
      });
    }

    // Botón "Quitar" con doble confirmación: el primer clic lo arma, el segundo ejecuta.
    std::unique_ptr<Button> removeButton(const std::string& token, std::function<void()> onConfirm) {
      const bool armed = state().confirmRemove == token;
      return button(
          armed ? tr("remove-confirm") : tr("remove"), "trash",
          armed ? ButtonVariant::Destructive : ButtonVariant::Outline,
          [token, armed, onConfirm = std::move(onConfirm)] {
            if (!armed) {
              state().confirmRemove = token;
              rebuild();
              return;
            }
            state().confirmRemove.clear();
            onConfirm();
          }
      );
    }

    void addAppPicker(Flex& body, float scale) {
      auto& s = state();
      body.addChild(input(
          scale, s.appFilter, tr("search-apps"), 0, [](const std::string& v) { state().appFilter = v; },
          [](const std::string&) { rebuild(); }
      ));
      std::string filter = s.appFilter;
      std::ranges::transform(filter, filter.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      int shown = 0;
      for (const auto& app : desktopEntries()) {
        if (app.noDisplay
            || app.hidden
            || app.exec.empty()
            || (!filter.empty() && app.nameLower.find(filter) == std::string::npos)) {
          continue;
        }
        if (++shown > 8) {
          break;
        }
        auto line = row(scale, app.name, stripFieldCodes(app.exec));
        line->addChild(button(
            tr("add-app"), "plus", ButtonVariant::Outline,
            [name = app.name, exec = stripFieldCodes(app.exec), icon = app.icon] {
              state().addMode = AddMode::None;
              report(okaru::autostart::create(name, exec, icon));
            }
        ));
        body.addChild(std::move(line));
      }
    }

    void addAppsCard(Flex& content, float scale) {
      auto& s = state();
      Flex* body = addSettingsCard(content, tr("apps"), scale);
      auto intro =
          makeLabel(tr("apps-intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      intro->setMaxLines(3);
      body->addChild(std::move(intro));

      const auto units = okaru::autostart::queryUnitStates();
      for (const auto& entry : okaru::autostart::listEntries()) {
        const auto unit = units.find(okaru::autostart::unitName(entry.id));
        const bool running = unit != units.end() && unit->second;
        const std::string origin = entry.user && !entry.system ? tr("user") : tr("system");
        auto line =
            row(scale, entry.name,
                std::format(
                    "{} · {} · {}", entry.exec.empty() ? entry.id : entry.exec, origin,
                    running ? tr("running") : tr("stopped")
                ));
        if (entry.ownedByOkaru) {
          line->addChild(removeButton(entry.id, [entry] { report(okaru::autostart::remove(entry)); }));
        }
        line->addChild(
            ui::toggle({
                .checked = entry.enabled,
                .scale = scale,
                .onChange = [entry](bool on) { report(okaru::autostart::setEnabled(entry, on)); },
            })
        );
        body->addChild(std::move(line));
      }

      if (s.addMode == AddMode::App) {
        body->addChild(
            makeLabel(tr("pick-app"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
        );
        addAppPicker(*body, scale);
      } else if (s.addMode == AddMode::Command) {
        auto form = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
        form->addChild(input(scale, s.draftName, tr("name"), 180, [](const std::string& v) { state().draftName = v; }));
        form->addChild(input(scale, s.draftExec, tr("command"), 0, [](const std::string& v) {
          state().draftExec = v;
        }));
        form->addChild(button(tr("save"), "device-floppy", ButtonVariant::Primary, [] {
          auto& st = state();
          const auto error = okaru::autostart::create(st.draftName, st.draftExec, "");
          if (!error) {
            st.addMode = AddMode::None;
            st.draftName.clear();
            st.draftExec.clear();
          }
          report(error);
        }));
        body->addChild(std::move(form));
      }

      auto actions = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true}
      );
      if (s.addMode != AddMode::None) {
        actions->addChild(button(tr("cancel"), "circle-x", ButtonVariant::Outline, [] {
          state().addMode = AddMode::None;
          rebuild();
        }));
      } else {
        actions->addChild(button(tr("add-app"), "apps", ButtonVariant::Default, [] {
          state().addMode = AddMode::App;
          rebuild();
        }));
        actions->addChild(button(tr("add-command"), "terminal-2", ButtonVariant::Default, [] {
          state().addMode = AddMode::Command;
          rebuild();
        }));
      }
      body->addChild(std::move(actions));
    }

    void saveVar(const std::string& key, const std::string& value) {
      state().envDrafts.erase(key);
      report(okaru::uwsm::writeEnvText(okaru::uwsm::setVar(okaru::uwsm::readEnvText(), key, value)));
    }

    void addEnvCard(Flex& content, float scale) {
      auto& s = state();
      Flex* body = addSettingsCard(content, tr("env"), scale);
      auto intro =
          makeLabel(tr("env-intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      intro->setMaxLines(3);
      body->addChild(std::move(intro));

      for (const auto& var : okaru::uwsm::parseEnv(okaru::uwsm::readEnvText())) {
        auto line = row(scale, var.key, var.managed ? tr("env-managed") : var.comment);
        if (var.managed) {
          line->addChild(
              makeLabel(var.value, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
          );
          body->addChild(std::move(line));
          continue;
        }
        const auto draft = s.envDrafts.find(var.key);
        const std::string shown = draft != s.envDrafts.end() ? draft->second : var.value;
        line->addChild(input(
            scale, shown, tr("env-value"), 260, [key = var.key](const std::string& v) { state().envDrafts[key] = v; },
            [key = var.key](const std::string& v) { saveVar(key, v); }
        ));
        if (draft != s.envDrafts.end() && draft->second != var.value) {
          line->addChild(
              button(tr("save"), "device-floppy", ButtonVariant::Primary, [key = var.key, value = draft->second] {
                saveVar(key, value);
              })
          );
        }
        line->addChild(removeButton("env:" + var.key, [key = var.key] {
          report(okaru::uwsm::writeEnvText(okaru::uwsm::removeVar(okaru::uwsm::readEnvText(), key)));
        }));
        body->addChild(std::move(line));
      }

      auto add = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      add->addChild(input(scale, s.newKey, tr("env-key"), 220, [](const std::string& v) { state().newKey = v; }));
      add->addChild(input(scale, s.newValue, tr("env-value"), 0, [](const std::string& v) { state().newValue = v; }));
      add->addChild(button(tr("env-add"), "plus", ButtonVariant::Default, [] {
        auto& st = state();
        if (!okaru::uwsm::isValidKey(st.newKey)) {
          st.error = tr("env-invalid");
          rebuild();
          return;
        }
        const std::string key = std::exchange(st.newKey, {});
        saveVar(key, std::exchange(st.newValue, {}));
      }));
      body->addChild(std::move(add));
    }

    void addHyprlandStartCard(Flex& content, float scale) {
      const auto commands = okaru::autostart::hyprlandStartCommands();
      if (commands.empty()) {
        return;
      }
      Flex* body = addSettingsCard(content, tr("hyprland-start"), scale);
      body->addChild(makeLabel(
          tr("hyprland-start-intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      for (const auto& cmd : commands) {
        body->addChild(row(scale, cmd.command, i18n::tr("settings.okaru.startup.from", "file", cmd.file)));
      }
    }

  } // namespace

  void addSettingsStartup(Flex& content, SettingsStartupContext ctx) {
    auto& s = state();
    s.rebuild = ctx.requestRebuild;
    const float scale = ctx.scale;

    if (!s.error.empty()) {
      auto error =
          makeLabel(s.error, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold);
      error->setMaxLines(4);
      content.addChild(std::move(error));
    }

    if (ctx.page != "system-environment") {
      addAppsCard(content, scale);
      addHyprlandStartCard(content, scale);
      return;
    }
    addEnvCard(content, scale);
    addSettingsWindows(
        content, SettingsWindowsContext{.scale = scale, .page = "system-behavior", .requestRebuild = ctx.requestRebuild}
    );
    content.addChild(
        makeLabel(tr("footer"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
    );
  }

} // namespace settings
