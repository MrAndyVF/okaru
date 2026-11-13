#pragma once

// OKARU: el contexto principal de GLib dentro del bucle de una app, para las señales de GIO (montajes, vigilar
// carpetas, gvfs). Mismo esquema que el agente de polkit: prepare/query → poll → check/dispatch.

#include "app/poll_source.h"

#include <glib.h>
#include <vector>

namespace okaru::apps {

  class GlibPollSource final : public PollSource {
  public:
    [[nodiscard]] int pollTimeoutMs() const override { return m_timeoutMs; }
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    std::vector<GPollFD> m_fds;
    gint m_priority = G_PRIORITY_DEFAULT;
    int m_timeoutMs = -1;
  };

} // namespace okaru::apps
