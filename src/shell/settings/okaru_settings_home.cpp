#include "shell/settings/okaru_settings_home.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_diagnostics.h"
#include "core/build_info.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "system/okaru_system_info.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <memory>

namespace settings {
  namespace {

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.home." + key); }

    std::unique_ptr<Flex> infoRow(float scale, const std::string& label, const std::string& value) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      row->addChild(
          ui::column(
              {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
              makeLabel(label, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
          )
      );
      row->addChild(
          makeLabel(value, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      return row;
    }

    // Uso con barra: "usado / total" y la barra de progreso debajo.
    void addUsage(Flex& body, float scale, const std::string& label, std::uint64_t used, std::uint64_t total) {
      body.addChild(infoRow(
          scale, label, std::format("{} / {}", okaru::sysinfo::formatBytes(used), okaru::sysinfo::formatBytes(total))
      ));
      const float progress =
          total > 0 ? static_cast<float>(static_cast<double>(used) / static_cast<double>(total)) : 0.0F;
      body.addChild(
          ui::progressBar({
              .fill = colorSpecFromRole(progress > 0.9F ? ColorRole::Error : ColorRole::Primary),
              .progress = progress,
              .height = 6.0F * scale,
          })
      );
    }

    std::string uptimeText(std::uint64_t seconds) {
      const auto days = seconds / 86400;
      const auto hours = (seconds % 86400) / 3600;
      const auto minutes = (seconds % 3600) / 60;
      return days > 0 ? std::format("{} d {} h {} min", days, hours, minutes)
                      : std::format("{} h {} min", hours, minutes);
    }

  } // namespace

  void addSettingsHome(Flex& content, float scale) {
    const auto info = okaru::sysinfo::collect();
    compositors::hyprland::HyprlandRuntime runtime;
    const auto hypr = runtime.requestSocketPath().empty() ? std::nullopt : okaru::hypr::queryVersion(runtime);

    Flex* shell = addSettingsCard(content, tr("about"), scale);
    shell->addChild(
        makeLabel("OKARU (郁)", Style::fontSizeTitle * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
    );
    shell->addChild(infoRow(scale, tr("shell-version"), okaru::build_info::displayVersion()));
    shell->addChild(infoRow(
        scale, tr("compositor"),
        hypr.has_value() ? std::format("Hyprland {} ({})", hypr->version, hypr->commit) : tr("compositor-unknown")
    ));

    Flex* system = addSettingsCard(content, tr("system"), scale);
    system->addChild(infoRow(scale, tr("os"), info.os));
    system->addChild(infoRow(scale, tr("kernel"), info.kernel));
    system->addChild(infoRow(scale, tr("host"), info.host));
    system->addChild(infoRow(scale, tr("cpu"), info.cpu));
    system->addChild(infoRow(scale, tr("uptime"), uptimeText(info.uptimeSeconds)));
    const auto memUsedKb =
        info.memory.totalKb > info.memory.availableKb ? info.memory.totalKb - info.memory.availableKb : 0;
    addUsage(*system, scale, tr("memory"), memUsedKb * 1024, info.memory.totalKb * 1024);

    Flex* disks = addSettingsCard(content, tr("disks"), scale);
    for (const auto& disk : info.disks) {
      addUsage(
          *disks, scale, std::format("{}  ·  {}", disk.mount, disk.fs), disk.totalBytes - disk.freeBytes,
          disk.totalBytes
      );
    }

    // Novedades: cuando OKARU tenga changelog, aquí se mostrarán las de la versión instalada.
    Flex* news = addSettingsCard(content, tr("changelog"), scale);
    news->addChild(
        makeLabel(tr("changelog-soon"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
    );
  }

} // namespace settings
