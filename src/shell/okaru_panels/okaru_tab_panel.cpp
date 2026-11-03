#include "shell/okaru_panels/okaru_tab_panel.h"

#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

OkaruTabPanel::OkaruTabPanel(
    std::string titleKey, std::string glyph, std::unique_ptr<Tab> tab, float width, float height
)
    : m_titleKey(std::move(titleKey)), m_glyph(std::move(glyph)), m_tab(std::move(tab)), m_width(width),
      m_height(height) {}

OkaruTabPanel::~OkaruTabPanel() = default;

void OkaruTabPanel::onPanelCardOpacityChanged(float opacity) { m_tab->setPanelCardOpacity(opacity); }

void OkaruTabPanel::create() {
  const float scale = contentScale();
  m_tab->setContentScale(scale);
  m_tab->setPanelCardOpacity(panelCardOpacity());
  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });

  auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  header->addChild(
      ui::glyph({
          .glyph = m_glyph,
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
  header->addChild(
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .text = i18n::tr(m_titleKey),
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      )
  );
  if (auto actions = m_tab->createHeaderActions(); actions != nullptr) {
    header->addChild(std::move(actions));
  }
  root->addChild(std::move(header));

  auto body = ui::column({.out = &m_body, .align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F});
  auto container = m_tab->create();
  container->setParticipatesInLayout(false); // se ubica a mano en doLayout, como en el centro de control
  m_container = container.get();
  body->addChild(std::move(container));
  root->addChild(std::move(body));
  setRoot(std::move(root));
}

void OkaruTabPanel::onOpen(std::string_view /*context*/) { m_tab->setActive(true); }

void OkaruTabPanel::onClose() {
  m_tab->setActive(false);
  m_tab->onClose();
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_body = nullptr;
  m_container = nullptr;
}

void OkaruTabPanel::onFrameTick(float deltaMs) { m_tab->onFrameTick(deltaMs); }

bool OkaruTabPanel::dismissTransientUi() { return m_tab->dismissTransientUi(); }

bool OkaruTabPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (m_tab->handleKey(sym, modifiers, pressed, preedit)) {
    return true;
  }
  if (pressed && KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    PanelManager::instance().close();
    return true;
  }
  return false;
}

void OkaruTabPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_body == nullptr || m_container == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  const float bodyWidth = m_body->width();
  const float bodyHeight = m_body->height();
  m_container->setPosition(0.0F, 0.0F);
  m_container->setSize(bodyWidth, bodyHeight);
  m_tab->layout(renderer, bodyWidth, bodyHeight);
}

void OkaruTabPanel::doUpdate(Renderer& renderer) { m_tab->update(renderer); }
