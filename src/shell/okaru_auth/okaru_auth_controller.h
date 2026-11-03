#pragma once

// OKARU: une AuthPromptService con la IPC (auth-prompt-open/result) y con el diálogo "okaru-auth".

#include "core/timer_manager.h"
#include "shell/okaru_auth/okaru_auth_prompt.h"

class CompositorPlatform;
class IpcService;
class PanelManager;

namespace okaru::auth {

  inline constexpr const char* kPanelId = "okaru-auth";

  class AuthController {
  public:
    AuthController(IpcService& ipc, PanelManager& panels, CompositorPlatform& platform);

    [[nodiscard]] AuthPromptService& service() noexcept { return m_service; }

  private:
    void sync();

    IpcService& m_ipc;
    PanelManager& m_panels;
    CompositorPlatform& m_platform;
    AuthPromptService m_service;
    Timer m_expiryTimer;
  };

} // namespace okaru::auth
