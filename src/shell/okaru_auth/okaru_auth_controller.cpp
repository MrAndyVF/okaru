#include "shell/okaru_auth/okaru_auth_controller.h"

#include "cli/schema_msg.h"
#include "compositors/compositor_platform.h"
#include "ipc/ipc_service.h"
#include "shell/panel/panel_manager.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

namespace okaru::auth {

  AuthController::AuthController(IpcService& ipc, PanelManager& panels, CompositorPlatform& platform)
      : m_ipc(ipc), m_panels(panels), m_platform(platform) {
    m_service.setOnChanged([this] { sync(); });

    m_ipc.bind(okaru::cli::msg::authPromptOpen, [this](const std::string& args) -> std::string {
      const int clientPid = m_ipc.callerPid();
      if (clientPid <= 0) {
        return "error: auth-prompt-open only works over the IPC socket\n";
      }
      const auto json = nlohmann::json::parse(args, nullptr, false);
      if (!json.is_object()) {
        return "error: usage: auth-prompt-open {\"prompt\": ..., \"message\": ...}\n";
      }
      const Requester requester = resolveRequester(clientPid);
      const std::string id = m_service.open({
          .id = {},
          .requester = requester.name,
          .requesterPid = requester.pid,
          .clientPid = clientPid,
          .prompt = json.value("prompt", std::string()),
          .message = json.value("message", std::string()),
      });
      return id.empty() ? "error: could not create request\n" : "ok " + id + "\n";
    });

    m_ipc.bind(okaru::cli::msg::authPromptResult, [this](const std::string& args) -> std::string {
      auto result = m_service.take(args, m_ipc.callerPid());
      switch (result.kind) {
      case AuthResultKind::Pending:
        return "pending\n";
      case AuthResultKind::Answered: {
        std::string reply = "ok " + result.secret + "\n";
        sodium_memzero(result.secret.data(), result.secret.size());
        return reply;
      }
      case AuthResultKind::Cancelled:
        return "cancelled\n";
      case AuthResultKind::Unknown:
        break;
      }
      return "error: unknown request\n";
    });
  }

  void AuthController::sync() {
    const bool open = m_panels.isOpenPanel(kPanelId);
    if (m_service.current() != nullptr) {
      if (!open) {
        m_panels.openPanel(kPanelId, PanelOpenRequest{.output = m_platform.preferredInteractiveOutput()});
      } else {
        m_panels.refresh();
      }
    } else if (open) {
      m_panels.closePanelById(kPanelId);
    }
    if (m_service.empty()) {
      m_expiryTimer.stop();
    } else if (!m_expiryTimer.active()) {
      m_expiryTimer.startRepeating(std::chrono::seconds(1), [this] { (void)m_service.expire(); });
    }
  }

} // namespace okaru::auth
