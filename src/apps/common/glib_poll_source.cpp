#include "apps/common/glib_poll_source.h"

namespace okaru::apps {

  void GlibPollSource::doAddPollFds(std::vector<pollfd>& fds) {
    GMainContext* context = g_main_context_default();
    m_fds.clear();
    m_timeoutMs = -1;
    if (!g_main_context_acquire(context)) {
      return;
    }
    const gboolean ready = g_main_context_prepare(context, &m_priority);
    gint timeout = -1;
    const gint count = g_main_context_query(context, m_priority, &timeout, nullptr, 0);
    if (count > 0) {
      m_fds.resize(static_cast<std::size_t>(count));
      g_main_context_query(context, m_priority, &timeout, m_fds.data(), count);
    }
    m_timeoutMs = ready ? 0 : timeout;
    for (const GPollFD& fd : m_fds) {
      fds.push_back({.fd = fd.fd, .events = static_cast<short>(fd.events), .revents = 0});
    }
    g_main_context_release(context);
  }

  void GlibPollSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    GMainContext* context = g_main_context_default();
    if (!g_main_context_acquire(context)) {
      return;
    }
    for (std::size_t i = 0; i < m_fds.size(); ++i) {
      const std::size_t index = startIdx + i;
      m_fds[i].revents = index < fds.size() ? static_cast<gushort>(fds[index].revents) : 0;
    }
    if (g_main_context_check(context, m_priority, m_fds.data(), static_cast<gint>(m_fds.size()))) {
      g_main_context_dispatch(context);
    }
    g_main_context_release(context);
  }

} // namespace okaru::apps
