#include "shell/okaru_workspaces/okaru_workspace_panel.h"

#include "compositors/hyprland/okaru_hypr_workspaces.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/glyph_picker.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstdlib>
#include <format>

namespace {
  constexpr const char* kPanelId = "okaru-workspace";

  std::string tr(const std::string& key) { return i18n::tr("settings.okaru.workspaces." + key); }

  std::vector<std::string> iconPath(int id) { return {"widget", "workspaces", "icons", std::to_string(id)}; }
} // namespace

OkaruWorkspacePanel::OkaruWorkspacePanel(ConfigService* config) : m_config(config) {}

// El tamaño del panel se fija al abrir: el ancho es el del selector de glifos y la altura máxima también;
// la altura real sigue al contenido.
float OkaruWorkspacePanel::preferredWidth() const { return GlyphPicker::preferredDialogWidth(contentScale()); }

float OkaruWorkspacePanel::preferredHeight() const { return GlyphPicker::preferredDialogHeight(contentScale()); }

float OkaruWorkspacePanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  if (m_dirty) {
    rebuild();
  }
  m_rootLayout->setSize(contentWidth, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(contentWidth);
  return m_rootLayout->measure(renderer, constraints).height;
}

void OkaruWorkspacePanel::create() {
  const float scale = contentScale();
  setRoot(
      ui::column({
          .out = &m_rootLayout,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
          .padding = Style::spaceLg * scale,
      })
  );
  m_dirty = true;
}

void OkaruWorkspacePanel::onOpen(std::string_view context) {
  m_context = std::string(context);
  m_id = std::atoi(m_context.c_str());
  m_picking = false;
  m_error.clear();
  m_dirty = true;
}

void OkaruWorkspacePanel::onClose() {
  clearReleasedRoot();
  m_rootLayout = nullptr;
}

void OkaruWorkspacePanel::markDirty() {
  m_dirty = true;
  if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel(kPanelId)) {
    manager->refresh();
  }
}

void OkaruWorkspacePanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
}

void OkaruWorkspacePanel::doUpdate(Renderer& /*renderer*/) {
  if (m_dirty) {
    rebuild();
  }
}

std::string OkaruWorkspacePanel::currentIcon() const {
  if (m_config == nullptr) {
    return {};
  }
  const auto& widgets = m_config->config().widgets;
  const auto it = widgets.find("workspaces");
  if (it == widgets.end()) {
    return {};
  }
  const auto map = it->second.getStringMap("icons");
  const auto icon = map.find(std::to_string(m_id));
  return icon == map.end() ? std::string() : icon->second;
}

void OkaruWorkspacePanel::rebuild() {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_dirty = false;
  while (!m_rootLayout->children().empty()) {
    (void)m_rootLayout->removeChild(m_rootLayout->children().back().get());
  }
  const float scale = contentScale();
  const std::string icon = currentIcon();

  if (m_picking) {
    auto picker = std::make_unique<GlyphPicker>(scale);
    picker->setTitle(tr("icon"));
    picker->setInitialGlyph(icon.empty() ? std::nullopt : std::optional(icon));
    picker->setOnCancel([this] {
      m_picking = false;
      markDirty();
    });
    picker->setOnApply([this](const GlyphPickerResult& result) {
      if (m_config != nullptr) {
        (void)m_config->setOverride(iconPath(m_id), result.name);
      }
      m_picking = false;
      markDirty();
    });
    m_rootLayout->addChild(std::move(picker));
    return;
  }

  const auto defs = okaru::hypr::readWorkspaces();
  const auto def = std::ranges::find(defs, m_id, &okaru::hypr::WorkspaceDef::id);
  const std::string name = def != defs.end() ? def->name : std::string();

  m_rootLayout->addChild(
      settings::makeLabel(
          std::format("Workspace {}", m_id), Style::fontSizeTitle * scale, colorSpecFromRole(ColorRole::Primary),
          FontWeight::Bold
      )
  );
  if (def == defs.end()) {
    m_rootLayout->addChild(
        settings::makeLabel(
            tr("not-managed"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
        )
    );
  }

  m_rootLayout->addChild(
      ui::input({
          .value = name,
          .placeholder = tr("name"),
          .fontSize = Style::fontSizeBody * scale,
          .enabled = def != defs.end(),
          .onSubmit = [this](const std::string& value) {
            if (auto err = okaru::hypr::renameWorkspace(m_runtime, m_id, value)) {
              m_error = *err;
              markDirty();
              return;
            }
            PanelManager::instance().close();
          },
      })
  );
  m_rootLayout->addChild(
      settings::makeLabel(
          tr("rename-hint"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      )
  );

  auto iconRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  iconRow->addChild(
      ui::button({
          .text = tr("icon"),
          .glyph = icon.empty() ? std::string("photo-plus") : icon,
          .variant = ButtonVariant::Default,
          .flexGrow = 1.0F,
          .onClick = [this] {
            m_picking = true;
            markDirty();
          },
      })
  );
  if (!icon.empty()) {
    iconRow->addChild(
        ui::button({
            .text = tr("icon-remove"),
            .glyph = "x",
            .variant = ButtonVariant::Outline,
            .onClick = [this] {
              if (m_config != nullptr) {
                (void)m_config->clearOverride(iconPath(m_id));
              }
              markDirty();
            },
        })
    );
  }
  m_rootLayout->addChild(std::move(iconRow));

  if (!m_error.empty()) {
    m_rootLayout->addChild(
        settings::makeLabel(m_error, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Error))
    );
  }
}
