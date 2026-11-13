#include "shell/okaru_updates/okaru_updates_tab.h"

#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/okaru_updates.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <ranges>

using namespace control_center;
using okaru::updates::Engine;
using okaru::updates::Package;
using okaru::updates::Phase;
using okaru::updates::Source;

namespace {

  constexpr float kActivityHeight = 44.0F;

  std::unique_ptr<Flex> card(float scale, float opacity) {
    return ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure = [scale, opacity](Flex& c) { applySectionCardStyle(c, scale, opacity); },
    });
  }

  std::unique_ptr<Button>
  iconButton(float scale, const char* glyph, std::string tooltip, std::function<void()> onClick, bool enabled = true) {
    return ui::button(
        {.glyph = glyph,
         .glyphSize = Style::fontSizeBody * scale,
         .enabled = enabled,
         .variant = ButtonVariant::Ghost,
         .tooltip = std::move(tooltip),
         .onClick = std::move(onClick)}
    );
  }

  std::unique_ptr<Label> caption(float scale, const std::string& text, ColorRole role = ColorRole::OnSurfaceVariant) {
    return ui::label({.text = text, .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(role)});
  }

  std::string formatDate(std::int64_t at) {
    const auto t = static_cast<std::time_t>(at);
    std::tm local{};
    localtime_r(&t, &local);
    return std::format("{:02}/{:02} {:02}:{:02}", local.tm_mday, local.tm_mon + 1, local.tm_hour, local.tm_min);
  }

  std::string ago(std::int64_t at) {
    const auto seconds = static_cast<long>(std::time(nullptr) - at);
    if (seconds < 3600) {
      return i18n::trp("okaru.updates.minutes-ago", std::max(1L, seconds / 60));
    }
    if (seconds < 86400) {
      return i18n::trp("okaru.updates.hours-ago", seconds / 3600);
    }
    return i18n::trp("okaru.updates.days-ago", seconds / 86400);
  }

  const char* sourceGlyph(const std::string& source) {
    if (source == "aur") {
      return "cloud";
    }
    if (source == "flatpak") {
      return "app-window";
    }
    if (source == "snap") {
      return "box";
    }
    return "package";
  }

  // Búsqueda general de Arch: sirve igual en distros derivadas.
  void openPackagePage(const Package& p) {
    std::string url;
    if (p.source == "aur") {
      url = "https://aur.archlinux.org/packages/" + p.name;
    } else if (p.source == "flatpak") {
      url = "https://flathub.org/apps/" + p.name;
    } else if (p.source == "snap") {
      url = "https://snapcraft.io/" + p.name;
    } else {
      url = "https://archlinux.org/packages/?q=" + p.name;
    }
    (void)process::runAsync({"xdg-open", url}); // los nombres ya pasaron validName: no hace falta codificarlos
    PanelManager::instance().closePanel();
  }

  std::string versions(const Package& p) {
    if (p.to.empty()) {
      return {};
    }
    return p.from.empty() ? "→ " + p.to : p.from + " → " + p.to;
  }

} // namespace

OkaruUpdatesTab::~OkaruUpdatesTab() {
  if (m_listener != 0) {
    Engine::instance().removeListener(m_listener);
  }
}

std::unique_ptr<Flex> OkaruUpdatesTab::create() {
  const float scale = contentScale();
  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch});
  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
      },
  });
  m_content = scroll->content();
  m_content->setDirection(FlexDirection::Vertical);
  m_content->setAlign(FlexAlign::Stretch);
  m_content->setGap(Style::spaceMd * scale);
  root->addChild(std::move(scroll));
  m_key.clear();
  if (m_listener == 0) {
    m_listener = Engine::instance().addListener([] { PanelManager::instance().refresh(); });
  }
  return root;
}

std::unique_ptr<Flex> OkaruUpdatesTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "file-text",
          .tooltip = i18n::tr("okaru.updates.open-log"),
          .onClick = [] { Engine::instance().openLog(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("okaru.updates.check"),
          .onClick = [] { Engine::instance().check(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void OkaruUpdatesTab::setActive(bool active) {
  if (!active) {
    m_armed.clear();
    return;
  }
  // Al abrir sin revisar nunca, se revisa: el panel vacío no dice nada.
  if (Engine::instance().phase() == Phase::Idle) {
    Engine::instance().check();
  }
  requestRebuild();
}

void OkaruUpdatesTab::onClose() {
  if (m_listener != 0) {
    Engine::instance().removeListener(m_listener);
    m_listener = 0;
  }
  m_root = nullptr;
  m_content = nullptr;
  m_armed.clear();
  m_openRun = 0;
}

void OkaruUpdatesTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  rebuildIfNeeded();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void OkaruUpdatesTab::doUpdate(Renderer& /*renderer*/) { rebuildIfNeeded(); }

void OkaruUpdatesTab::requestRebuild() {
  m_key.clear();
  PanelManager::instance().refresh();
}

std::string OkaruUpdatesTab::stateKey() const {
  const auto& e = Engine::instance();
  std::string key = std::format(
      "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", static_cast<int>(e.phase()), e.step(), e.error(), e.total(),
      e.checkedAt(), e.rebootRecommended(), e.progressDone(), e.runs().size(), e.activity().size(),
      e.polkitRuleInstalled(), m_openRun, m_armed, e.ignoredPending().size(), e.config().ignore.size()
  );
  for (const auto& source : m_expanded) {
    key += "|" + source;
  }
  for (const auto& line : e.logTail()) {
    key += "\n" + line;
  }
  return key;
}

void OkaruUpdatesTab::rebuildIfNeeded() {
  if (m_content == nullptr) {
    return;
  }
  std::string key = stateKey();
  if (key == m_key) {
    return;
  }
  m_key = std::move(key);
  while (!m_content->children().empty()) {
    m_content->removeChild(m_content->children().back().get());
  }
  if (m_openRun != 0) {
    addRunView(*m_content);
    return;
  }
  addStatus(*m_content);
  if (Engine::instance().phase() == Phase::Running || !Engine::instance().logTail().empty()) {
    addRun(*m_content);
  }
  addSources(*m_content);
  addIgnored(*m_content);
  addActivity(*m_content);
  addHistory(*m_content);
}

void OkaruUpdatesTab::addStatus(Flex& parent) {
  const float scale = contentScale();
  const auto& e = Engine::instance();
  const Phase phase = e.phase();
  auto c = card(scale, panelCardOpacity());

  const ColorRole role = phase == Phase::Error || phase == Phase::Missing ? ColorRole::Error
      : phase == Phase::Ready                                             ? ColorRole::Primary
      : phase == Phase::Clean                                             ? ColorRole::Tertiary
                                                                          : ColorRole::Secondary;
  const char* glyph = phase == Phase::Clean              ? "circle-check"
      : phase == Phase::Error || phase == Phase::Missing ? "alert-triangle"
                                                         : "package";
  auto top = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  top->addChild(ui::glyph({.glyph = glyph, .glyphSize = 32.0F * scale, .color = colorSpecFromRole(role)}));
  auto text = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
  text->addChild(
      ui::label(
          {.text = okaru::updates::statusText(e),
           .fontSize = Style::fontSizeTitle * scale,
           .fontWeight = FontWeight::Bold,
           .maxLines = 3}
      )
  );
  std::string detail;
  if (!e.checkedAt().empty()) {
    detail = i18n::tr("okaru.updates.checked-at", "time", e.checkedAt());
  }
  if (const auto size = e.downloadSizeMiB(); size && phase == Phase::Ready) {
    detail += (detail.empty() ? "" : " · ")
        + (*size >= 1024.0 ? i18n::tr("okaru.updates.size-gib", "value", std::format("{:.2f}", *size / 1024.0))
                           : i18n::tr("okaru.updates.size-mib", "value", std::format("{:.1f}", *size)));
  }
  if (!detail.empty()) {
    text->addChild(caption(scale, detail));
  }
  top->addChild(std::move(text));
  c->addChild(std::move(top));

  if (e.rebootRecommended()) {
    c->addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .gap = Style::spaceSm * scale,
             .paddingV = Style::spaceXs * scale,
             .paddingH = Style::spaceSm * scale,
             .fill = colorSpecFromRole(ColorRole::Tertiary, 0.12F),
             .radius = Style::radiusSm * scale},
            ui::glyph(
                {.glyph = "refresh-alert",
                 .glyphSize = Style::fontSizeBody * scale,
                 .color = colorSpecFromRole(ColorRole::Tertiary)}
            ),
            ui::label(
                {.text = i18n::tr("okaru.updates.reboot"),
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::Tertiary),
                 .maxLines = 2,
                 .flexGrow = 1.0F}
            )
        )
    );
  }

  const bool background = e.config().mode == UpdatesMode::Background;
  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (phase == Phase::Ready) {
    actions->addChild(
        ui::button({
            .text = i18n::tr("okaru.updates.update"),
            .glyph = background ? "download" : "terminal-2",
            .variant = ButtonVariant::Primary,
            .tooltip =
                i18n::tr(background ? "okaru.updates.update-background-tip" : "okaru.updates.update-terminal-tip"),
            .flexGrow = 1.0F,
            .onClick = [] { Engine::instance().update(); },
        })
    );
    actions->addChild(iconButton(scale, "x", i18n::tr("okaru.updates.dismiss"), [] { Engine::instance().dismiss(); }));
  } else if (phase == Phase::Error && e.lastExit().has_value() && !e.runIsRollback()) {
    actions->addChild(
        ui::button({
            .text = i18n::tr("okaru.updates.retry-terminal"),
            .glyph = "terminal-2",
            .variant = ButtonVariant::Primary,
            .flexGrow = 1.0F,
            .onClick = [] { Engine::instance().update(true); },
        })
    );
  } else if (phase != Phase::Checking && phase != Phase::Running) {
    actions->addChild(
        ui::button({
            .text = i18n::tr("okaru.updates.check"),
            .glyph = "refresh",
            .variant = ButtonVariant::Secondary,
            .flexGrow = 1.0F,
            .onClick = [] { Engine::instance().check(); },
        })
    );
  }
  if (!actions->children().empty()) {
    c->addChild(std::move(actions));
  }

  // En segundo plano, pkexec pide la contraseña en cada transacción hasta que se instala la regla de polkit.
  if (background && !e.polkitRuleInstalled()) {
    auto hint = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    hint->addChild(
        ui::glyph(
            {.glyph = "key",
             .glyphSize = Style::fontSizeBody * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    hint->addChild(
        ui::label(
            {.text = i18n::tr("okaru.updates.polkit-hint"),
             .fontSize = Style::fontSizeCaption * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 2,
             .flexGrow = 1.0F}
        )
    );
    hint->addChild(
        ui::button(
            {.text = i18n::tr("okaru.updates.polkit-install"),
             .variant = ButtonVariant::Outline,
             .tooltip = i18n::tr("okaru.updates.polkit-install-tip"),
             .onClick = [] { Engine::instance().installPolkitRule(); }}
        )
    );
    c->addChild(std::move(hint));
  }
  parent.addChild(std::move(c));
}

void OkaruUpdatesTab::addRun(Flex& parent) {
  const float scale = contentScale();
  const auto& e = Engine::instance();
  auto c = card(scale, panelCardOpacity());
  auto header = makeCardHeaderRow(i18n::tr("okaru.updates.log-title"), scale);
  header->addChild(iconButton(scale, "external-link", i18n::tr("okaru.updates.open-log"), [] {
    Engine::instance().openLog();
  }));
  c->addChild(std::move(header));
  if (e.phase() == Phase::Running && e.progressTotal() > 0) {
    const float progress = std::min(1.0F, static_cast<float>(e.progressDone()) / static_cast<float>(e.progressTotal()));
    c->addChild(ui::progressBar({.progress = progress, .height = 6.0F * scale}));
  }
  if (e.logTail().empty()) {
    c->addChild(
        caption(scale, i18n::tr(e.runInTerminal() ? "okaru.updates.log-terminal" : "okaru.updates.log-waiting"))
    );
  }
  for (const auto& line : e.logTail()) {
    c->addChild(
        ui::label(
            {.text = line,
             .fontSize = Style::fontSizeCaption * scale,
             .fontFamily = "monospace",
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 1}
        )
    );
  }
  parent.addChild(std::move(c));
}

void OkaruUpdatesTab::addSources(Flex& parent) {
  const float scale = contentScale();
  const auto& e = Engine::instance();
  struct Entry {
    const char* key;
    std::string label;
    const Source* source;
  };
  std::vector<Entry> entries{
      {"pacman", i18n::tr("okaru.updates.source.pacman"), &e.pacman()},
      {"aur", e.aurHelper().empty() ? i18n::tr("okaru.updates.source.aur") : std::format("AUR ({})", e.aurHelper()),
       &e.aur()},
      {"flatpak", i18n::tr("okaru.updates.source.flatpak"), &e.flatpak()},
      {"snap", i18n::tr("okaru.updates.source.snap"), &e.snap()},
  };
  std::erase_if(entries, [](const Entry& entry) { return entry.source->count == 0; });
  if (entries.empty()) {
    return;
  }
  std::ranges::stable_sort(entries, [](const Entry& a, const Entry& b) { return a.source->count > b.source->count; });
  auto c = card(scale, panelCardOpacity());
  for (const auto& entry : entries) {
    const bool open = m_expanded.contains(entry.key);
    auto header = ui::button({
        .variant = ButtonVariant::Ghost,
        .onClick = [this, key = std::string(entry.key), open] {
          if (open) {
            m_expanded.erase(key);
          } else {
            m_expanded.insert(key);
          }
          requestRebuild();
        },
    });
    header->setDirection(FlexDirection::Horizontal);
    header->setAlign(FlexAlign::Center);
    header->setGap(Style::spaceSm * scale);
    header->addChild(
        ui::glyph(
            {.glyph = sourceGlyph(entry.key),
             .glyphSize = Style::fontSizeBody * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    header->addChild(ui::label({.text = entry.label, .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F}));
    header->addChild(
        ui::label(
            {.text = std::to_string(entry.source->count),
             .fontSize = Style::fontSizeBody * scale,
             .fontWeight = FontWeight::Bold,
             .color = colorSpecFromRole(ColorRole::Primary)}
        )
    );
    header->addChild(
        ui::glyph(
            {.glyph = open ? "chevron-up" : "chevron-down",
             .glyphSize = Style::fontSizeBody * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    c->addChild(std::move(header));
    if (!open) {
      continue;
    }
    for (const auto& pkg : entry.source->items) {
      auto row =
          ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingH = Style::spaceMd * scale});
      auto names = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
      names->addChild(ui::label({.text = pkg.name, .fontSize = Style::fontSizeBody * scale, .maxLines = 1}));
      if (const std::string v = versions(pkg); !v.empty()) {
        names->addChild(
            ui::label(
                {.text = v,
                 .fontSize = Style::fontSizeCaption * scale,
                 .fontFamily = "monospace",
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .maxLines = 1}
            )
        );
      }
      row->addChild(std::move(names));
      row->addChild(iconButton(scale, "external-link", i18n::tr("okaru.updates.open-page"), [pkg] {
        openPackagePage(pkg);
      }));
      row->addChild(iconButton(scale, "eye-off", i18n::tr("okaru.updates.ignore-tip"), [name = pkg.name] {
        Engine::instance().ignore(name);
      }));
      c->addChild(std::move(row));
    }
    if (entry.source->count > static_cast<int>(entry.source->items.size())) {
      c->addChild(caption(
          scale,
          i18n::tr("okaru.updates.more", "count", entry.source->count - static_cast<int>(entry.source->items.size()))
      ));
    }
  }
  parent.addChild(std::move(c));
}

void OkaruUpdatesTab::addIgnored(Flex& parent) {
  const float scale = contentScale();
  const auto& e = Engine::instance();
  // Los de los ajustes (aunque no tengan nada pendiente) y los que pacman.conf retuvo en esta revisión.
  std::vector<std::pair<std::string, bool>> rows; // nombre, editable
  for (const auto& name : e.config().ignore) {
    rows.emplace_back(name, true);
  }
  for (const auto& p : e.ignoredPending()) {
    if (p.source == "system") {
      rows.emplace_back(p.name, false);
    }
  }
  if (rows.empty()) {
    return;
  }
  auto c = card(scale, panelCardOpacity());
  c->addChild(makeCardHeaderRow(i18n::tr("okaru.updates.ignored"), scale));
  for (const auto& [name, editable] : rows) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    row->addChild(ui::label({.text = name, .fontSize = Style::fontSizeBody * scale, .maxLines = 1, .flexGrow = 1.0F}));
    if (editable) {
      row->addChild(iconButton(scale, "eye", i18n::tr("okaru.updates.unignore-tip"), [name] {
        Engine::instance().unignore(name);
      }));
    } else {
      row->addChild(caption(scale, "pacman.conf"));
    }
    c->addChild(std::move(row));
  }
  parent.addChild(std::move(c));
}

void OkaruUpdatesTab::addActivity(Flex& parent) {
  const float scale = contentScale();
  const auto& e = Engine::instance();
  if (!e.config().activityGraph || e.activity().empty()) {
    return;
  }
  auto c = card(scale, panelCardOpacity());
  auto header = makeCardHeaderRow(i18n::tr("okaru.updates.activity"), scale);
  header->addChild(caption(
      scale,
      e.lastUpdateAt() > 0 ? i18n::tr("okaru.updates.updated-ago", "when", ago(e.lastUpdateAt()))
                           : i18n::tr("okaru.updates.never-updated")
  ));
  c->addChild(std::move(header));
  int peak = 1;
  for (const auto& point : e.activity()) {
    peak = std::max(peak, point.count);
  }
  // Una barra por revisión; las que siguieron a una actualización van en otro color.
  auto bars = ui::row({.align = FlexAlign::End, .gap = Style::spaceXs * scale, .height = kActivityHeight * scale});
  for (const auto& point : e.activity()) {
    const float h =
        std::max(2.0F, kActivityHeight * scale * static_cast<float>(point.count) / static_cast<float>(peak));
    auto bar = ui::box(
        {.fill = colorSpecFromRole(
             point.afterUpdate ? ColorRole::Tertiary : ColorRole::Primary, point.count == 0 ? 0.35F : 1.0F
         ),
         .radius = 2.0F * scale,
         .height = h,
         .flexGrow = 1.0F}
    );
    auto holder = ui::column(
        {.align = FlexAlign::Stretch, .justify = FlexJustify::End, .height = kActivityHeight * scale, .flexGrow = 1.0F}
    );
    holder->addChild(std::move(bar));
    auto area = ui::inputArea(
        {.tooltip = std::format("{} · {}", formatDate(point.at), i18n::trp("okaru.updates.pending", point.count)),
         .flexGrow = 1.0F}
    );
    area->addChild(std::move(holder));
    bars->addChild(std::move(area));
  }
  c->addChild(std::move(bars));
  parent.addChild(std::move(c));
}

void OkaruUpdatesTab::addHistory(Flex& parent) {
  const float scale = contentScale();
  const auto& runs = Engine::instance().runs();
  if (runs.empty()) {
    return;
  }
  auto c = card(scale, panelCardOpacity());
  c->addChild(makeCardHeaderRow(i18n::tr("okaru.updates.history"), scale));
  for (const auto& run : std::views::reverse(runs)) {
    auto row = ui::button(
        {.variant = ButtonVariant::Ghost,
         .tooltip = i18n::tr("okaru.updates.history-tip"),
         .onClick = [this, at = run.at] {
           m_openRun = at;
           m_armed.clear();
           requestRebuild();
         }}
    );
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setGap(Style::spaceSm * scale);
    row->addChild(
        ui::glyph(
            {.glyph = run.rollback ? "arrow-back-up" : "package",
             .glyphSize = Style::fontSizeBody * scale,
             .color = colorSpecFromRole(run.rollback ? ColorRole::Tertiary : ColorRole::OnSurfaceVariant)}
        )
    );
    row->addChild(ui::label({.text = formatDate(run.at), .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F}));
    row->addChild(caption(scale, i18n::trp("okaru.updates.packages", static_cast<long>(run.packages.size()))));
    row->addChild(
        ui::glyph(
            {.glyph = "chevron-right",
             .glyphSize = Style::fontSizeBody * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    c->addChild(std::move(row));
  }
  parent.addChild(std::move(c));
}

void OkaruUpdatesTab::addRunView(Flex& parent) {
  const float scale = contentScale();
  auto& e = Engine::instance();
  const auto& runs = e.runs();
  const auto run = std::ranges::find(runs, m_openRun, &okaru::updates::Run::at);
  if (run == runs.end()) {
    m_openRun = 0;
    addStatus(parent);
    return;
  }
  const bool busy = e.phase() == Phase::Running || e.phase() == Phase::Checking;
  auto c = card(scale, panelCardOpacity());
  auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  header->addChild(iconButton(scale, "chevron-left", i18n::tr("okaru.updates.back"), [this] {
    m_openRun = 0;
    m_armed.clear();
    requestRebuild();
  }));
  header->addChild(
      ui::label(
          {.text =
               formatDate(run->at) + (run->rollback ? " · " + i18n::tr("okaru.updates.rollback-tag") : std::string{}),
           .fontSize = Style::fontSizeBody * scale,
           .fontWeight = FontWeight::Bold,
           .flexGrow = 1.0F}
      )
  );
  const bool anyRollbackable =
      std::ranges::any_of(run->packages, [](const Package& p) { return p.source != "flatpak" && p.source != "snap"; });
  if (anyRollbackable && !run->rollback) {
    const std::string armKey = std::format("all-{}", run->at);
    const bool armed = m_armed == armKey;
    header->addChild(
        ui::button({
            .text = i18n::tr(armed ? "okaru.updates.confirm" : "okaru.updates.rollback-all"),
            .glyph = "arrow-back-up",
            .enabled = !busy,
            .variant = armed ? ButtonVariant::Destructive : ButtonVariant::Outline,
            .tooltip = i18n::tr("okaru.updates.rollback-all-tip"),
            .onClick = [this, armKey, armed, at = run->at] {
              if (armed) {
                m_armed.clear();
                m_openRun = 0;
                Engine::instance().rollbackRun(at);
              } else {
                m_armed = armKey;
              }
              requestRebuild();
            },
        })
    );
  }
  c->addChild(std::move(header));

  for (const auto& pkg : run->packages) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    if (pkg.source == "flatpak" || pkg.source == "snap") {
      // Flatpak y Snap no pasan por la caché de pacman: no se revierten desde aquí.
      row->addChild(
          ui::glyph(
              {.glyph = sourceGlyph(pkg.source),
               .glyphSize = Style::fontSizeBody * scale,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          )
      );
    } else if (!e.cached(pkg)) {
      row->addChild(iconButton(scale, "arrow-back-up", i18n::tr("okaru.updates.rollback-missing"), [] {}, false));
    } else {
      const std::string armKey = std::format("{}-{}", run->at, pkg.name);
      const bool armed = m_armed == armKey;
      row->addChild(
          ui::button({
              .glyph = armed ? "check" : "arrow-back-up",
              .glyphSize = Style::fontSizeBody * scale,
              .enabled = !busy,
              .variant = armed ? ButtonVariant::Destructive : ButtonVariant::Ghost,
              .tooltip = armed ? i18n::tr("okaru.updates.confirm")
                               : i18n::tr("okaru.updates.rollback-tip", "version", pkg.from),
              .onClick = [this, armKey, armed, at = run->at, name = pkg.name] {
                if (armed) {
                  m_armed.clear();
                  m_openRun = 0;
                  Engine::instance().rollbackPackage(at, name);
                } else {
                  m_armed = armKey;
                }
                requestRebuild();
              },
          })
      );
    }
    auto names = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
    names->addChild(ui::label({.text = pkg.name, .fontSize = Style::fontSizeBody * scale, .maxLines = 1}));
    if (const std::string v = versions(pkg); !v.empty()) {
      names->addChild(
          ui::label(
              {.text = v,
               .fontSize = Style::fontSizeCaption * scale,
               .fontFamily = "monospace",
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 1}
          )
      );
    }
    row->addChild(std::move(names));
    c->addChild(std::move(row));
  }
  parent.addChild(std::move(c));
}
