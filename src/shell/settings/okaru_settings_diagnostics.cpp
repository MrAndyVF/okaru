#include "shell/settings/okaru_settings_diagnostics.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_diagnostics.h"
#include "compositors/hyprland/okaru_hypr_options.h"
#include "core/build_info.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <memory>
#include <string>

namespace settings {
  namespace {

    struct DiagnosticsState {
      compositors::hyprland::HyprlandRuntime runtime;
      std::function<void()> rebuild;
      std::string armed;   // respaldo con la restauración en curso
      int confirmStep = 0; // 1 = primera confirmación dada, 2 = segunda
      std::string message;
      bool messageIsError = false;
    };

    DiagnosticsState& state() {
      static DiagnosticsState s;
      return s;
    }

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.diagnostics." + key); }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    void setMessage(std::string text, bool error) {
      state().message = std::move(text);
      state().messageIsError = error;
      rebuild();
    }

    std::unique_ptr<Flex> row(float scale, const std::string& title, const std::string& detail, ColorRole detailRole) {
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (!detail.empty()) {
        auto label = makeLabel(detail, Style::fontSizeCaption * scale, colorSpecFromRole(detailRole));
        label->setMaxLines(4);
        text->addChild(std::move(label));
      }
      auto out = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      out->addChild(std::move(text));
      return out;
    }

    std::unique_ptr<Button>
    button(const std::string& text, const char* glyph, ButtonVariant variant, std::function<void()> onClick) {
      return ui::button({.text = text, .glyph = std::string(glyph), .variant = variant, .onClick = std::move(onClick)});
    }

    void reloadHyprland() {
      const auto reply = state().runtime.request("reload");
      setMessage(reply.has_value() ? tr("reloaded") : tr("reload-failed"), !reply.has_value());
    }

    void addStatusCard(Flex& content, float scale) {
      auto& s = state();
      Flex* body = addSettingsCard(content, tr("status"), scale);
      const auto version = okaru::hypr::queryVersion(s.runtime);
      body->addChild(row(
          scale,
          version.has_value() ? std::format("Hyprland {} ({})", version->version, version->commit) : tr("no-hyprland"),
          std::format("OKARU {}", okaru::build_info::displayVersion()), ColorRole::OnSurfaceVariant
      ));

      const auto errors = okaru::hypr::queryConfigErrors(s.runtime);
      std::string detail;
      for (const auto& error : errors) {
        detail += (detail.empty() ? "" : "\n") + error;
      }
      auto line =
          row(scale,
              errors.empty() ? tr("no-errors")
                             : i18n::tr("settings.okaru.diagnostics.errors", "count", std::to_string(errors.size())),
              detail, ColorRole::Error);
      line->addChild(button(tr("reload"), "reload", ButtonVariant::Default, [] { reloadHyprland(); }));
      body->addChild(std::move(line));
    }

    void addBackupsCard(Flex& content, float scale) {
      auto& s = state();
      Flex* body = addSettingsCard(content, tr("backups"), scale);
      auto intro = makeLabel(
          tr("backups-intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      intro->setMaxLines(3);
      body->addChild(std::move(intro));

      const auto backups = okaru::hypr::findBackups();
      if (backups.empty()) {
        body->addChild(
            makeLabel(tr("no-backups"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
        );
      }
      for (const auto& backup : backups) {
        const bool armed = s.armed == backup.path.string();
        std::string detail = i18n::tr(
            backup.directory ? "settings.okaru.diagnostics.restores-folder"
                             : "settings.okaru.diagnostics.restores-file",
            "target", backup.target.string()
        );
        if (armed) {
          // Doble confirmación: cada paso explica qué va a pasar.
          detail = i18n::tr(
              s.confirmStep == 1 ? "settings.okaru.diagnostics.confirm-1" : "settings.okaru.diagnostics.confirm-2",
              "target", backup.target.string()
          );
        }
        auto line = row(scale, backup.label, detail, armed ? ColorRole::Error : ColorRole::OnSurfaceVariant);
        line->addChild(button(
            tr("open"), "folder-open", ButtonVariant::Ghost,
            [dir = (backup.directory ? backup.path : backup.path.parent_path()).string()] {
              (void)process::runAsync({"xdg-open", dir.c_str()});
            }
        ));
        if (armed) {
          line->addChild(button(tr("cancel"), "circle-x", ButtonVariant::Outline, [] {
            state().armed.clear();
            state().confirmStep = 0;
            rebuild();
          }));
          const bool last = s.confirmStep >= 2;
          line->addChild(button(
              last ? tr("restore-final") : tr("restore-yes"), "restore", ButtonVariant::Destructive, [backup, last] {
                auto& st = state();
                if (!last) {
                  st.confirmStep = 2;
                  rebuild();
                  return;
                }
                st.armed.clear();
                st.confirmStep = 0;
                if (const auto error = okaru::hypr::restoreBackup(backup)) {
                  setMessage(*error, true);
                  return;
                }
                (void)st.runtime.request("reload");
                setMessage(i18n::tr("settings.okaru.diagnostics.restored", "target", backup.target.string()), false);
              }
          ));
        } else {
          line->addChild(button(tr("restore"), "restore", ButtonVariant::Outline, [path = backup.path.string()] {
            state().armed = path;
            state().confirmStep = 1;
            rebuild();
          }));
        }
        body->addChild(std::move(line));
      }
    }

    void addBlocksCard(Flex& content, float scale) {
      Flex* body = addSettingsCard(content, tr("blocks"), scale);
      auto intro =
          makeLabel(tr("blocks-intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      intro->setMaxLines(3);
      body->addChild(std::move(intro));
      const auto okaDir = okaru::hypr::okaFilePath("layout").parent_path();
      for (const auto& block : okaru::hypr::findManagedBlocks(okaDir)) {
        body->addChild(
            row(scale, std::format("{}  ·  okaru:{}", block.file, block.block), tr("block-" + block.block),
                ColorRole::OnSurfaceVariant)
        );
      }
      // Archivos que OKARU edita en su lugar, sin bloque.
      body->addChild(row(scale, "windowrules.lua", tr("file-rules"), ColorRole::OnSurfaceVariant));
      body->addChild(row(scale, "binds.lua  ·  user-binds.lua", tr("file-binds"), ColorRole::OnSurfaceVariant));
    }

  } // namespace

  void addSettingsDiagnostics(Flex& content, SettingsDiagnosticsContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    if (s.runtime.requestSocketPath().empty()) {
      Flex* body = addSettingsCard(content, tr("status"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.hyprland-only"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }

    if (!s.message.empty()) {
      auto message = makeLabel(
          s.message, Style::fontSizeBody * scale,
          colorSpecFromRole(s.messageIsError ? ColorRole::Error : ColorRole::Primary), FontWeight::Bold
      );
      message->setMaxLines(4);
      content.addChild(std::move(message));
    }
    addStatusCard(content, scale);
    addBackupsCard(content, scale);
    addBlocksCard(content, scale);
  }

} // namespace settings
