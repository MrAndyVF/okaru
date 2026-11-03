#include "shell/okaru_auth/okaru_auth_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/okaru_auth/okaru_auth_prompt.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <utility>

using okaru::auth::AuthRequest;

OkaruAuthPanel::OkaruAuthPanel(ConfigService* config, okaru::auth::AuthPromptService* service)
    : m_config(config), m_service(service) {}

PanelPlacement OkaruAuthPanel::panelPlacement() const noexcept {
  // Misma ubicación que el diálogo de polkit: los dos piden permisos de administrador.
  return m_config != nullptr ? m_config->config().shell.panel.polkitPlacement : PanelPlacement::Floating;
}

float OkaruAuthPanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  m_rootLayout->setSize(contentWidth, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(contentWidth);
  return m_rootLayout->measure(renderer, constraints).height;
}

void OkaruAuthPanel::create() {
  const float scale = contentScale();
  const auto caption = [scale](Label** out, ColorRole role) {
    return ui::label({
        .out = out,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(role),
        .maxLines = 3,
    });
  };

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });

  root->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
          ui::glyph({
              .glyph = "lock",
              .glyphSize = 32.0F * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
              ui::label({
                  .text = i18n::tr("auth.okaru.title"),
                  .fontSize = Style::fontSizeTitle * scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::Primary),
              }),
              ui::label({
                  .out = &m_messageLabel,
                  .fontSize = Style::fontSizeBody * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 6,
              }),
              caption(&m_requesterLabel, ColorRole::OnSurfaceVariant)
          )
      )
  );

  root->addChild(caption(&m_promptLabel, ColorRole::OnSurfaceVariant));

  auto field = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  field->addChild(
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("auth.okaru.placeholder"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange =
              [this](const std::string& value) {
                if (m_submitButton != nullptr) {
                  m_submitButton->setEnabled(!value.empty());
                }
              },
          .onSubmit = [this](const std::string& /*value*/) { submit(); },
      })
  );
  field->addChild(
      ui::button({
          .out = &m_revealButton,
          .glyph = "eye",
          .variant = ButtonVariant::Ghost,
          .onClick = [this] {
            m_revealed = !m_revealed;
            if (m_input != nullptr) {
              m_input->setPasswordRevealed(m_revealed);
            }
            if (m_revealButton != nullptr) {
              m_revealButton->setGlyph(m_revealed ? "eye-off" : "eye");
            }
          },
      })
  );
  root->addChild(std::move(field));

  root->addChild(caption(&m_errorLabel, ColorRole::Error));

  root->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true},
          ui::button({
              .text = i18n::tr("common.actions.cancel"),
              .glyph = "circle-x",
              .variant = ButtonVariant::Outline,
              .onClick = [this] { cancel(); },
          }),
          ui::button({
              .out = &m_submitButton,
              .text = i18n::tr("auth.okaru.accept"),
              .glyph = "check",
              .variant = ButtonVariant::Primary,
              .onClick = [this] { submit(); },
          })
      )
  );

  setRoot(std::move(root));
}

void OkaruAuthPanel::onOpen(std::string_view /*context*/) {
  m_revealed = false;
  const AuthRequest* request = m_service != nullptr ? m_service->current() : nullptr;
  m_shownId = request != nullptr ? request->id : std::string();
  if (m_input != nullptr) {
    m_input->setValue("");
    m_input->setPasswordRevealed(false);
  }
}

void OkaruAuthPanel::onClose() {
  // Cerrar el diálogo sin responder (Esc, otro panel encima) cancela lo que mostraba.
  if (m_service != nullptr && !m_shownId.empty()) {
    m_service->cancel(m_shownId);
  }
  m_shownId.clear();
  if (m_input != nullptr) {
    m_input->setValue("");
  }
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_messageLabel = nullptr;
  m_requesterLabel = nullptr;
  m_promptLabel = nullptr;
  m_errorLabel = nullptr;
  m_input = nullptr;
  m_revealButton = nullptr;
  m_submitButton = nullptr;
}

InputArea* OkaruAuthPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

bool OkaruAuthPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool /*preedit*/) {
  if (!pressed || !KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    return false;
  }
  cancel();
  return true;
}

void OkaruAuthPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
}

void OkaruAuthPanel::doUpdate(Renderer& /*renderer*/) {
  const AuthRequest* request = m_service != nullptr ? m_service->current() : nullptr;
  if (request == nullptr || m_messageLabel == nullptr || m_input == nullptr) {
    return;
  }
  if (request->id != m_shownId) {
    // Llegó otra solicitud (p. ej. el reintento de sudo): el campo empieza vacío.
    m_shownId = request->id;
    m_input->setValue("");
  }
  const std::string program = request->requester.empty() ? i18n::tr("auth.okaru.unknown") : request->requester;
  m_messageLabel->setText(
      request->message.empty() ? i18n::tr("auth.okaru.message", "program", program) : request->message
  );
  m_requesterLabel->setText(
      i18n::tr("auth.okaru.requester", "program", program, "pid", std::to_string(request->requesterPid))
  );
  m_promptLabel->setText(request->prompt);
  m_promptLabel->setVisible(!request->prompt.empty());
  m_errorLabel->setText(
      request->attempt > 1 ? i18n::tr("auth.okaru.retry", "attempt", std::to_string(request->attempt)) : std::string()
  );
  m_errorLabel->setVisible(request->attempt > 1);
  m_submitButton->setEnabled(!m_input->value().empty());
}

void OkaruAuthPanel::submit() {
  if (m_service == nullptr || m_input == nullptr || m_shownId.empty() || m_input->value().empty()) {
    return;
  }
  const std::string id = std::exchange(m_shownId, {});
  std::string secret = m_input->value();
  m_input->setValue("");
  m_service->answer(id, std::move(secret));
}

void OkaruAuthPanel::cancel() {
  if (m_service != nullptr && !m_shownId.empty()) {
    m_service->cancel(std::exchange(m_shownId, {}));
  }
  PanelManager::instance().close();
}

void OkaruAuthPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
}
