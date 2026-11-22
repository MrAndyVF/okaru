#pragma once

#include "app/poll_source.h"

#include <memory>

class NotificationManager;

// OKARU: avisa al conectar o quitar un dispositivo extraíble (USB, disco externo, teléfono MTP).
class OkaruDeviceMonitor final : public PollSource {
public:
  explicit OkaruDeviceMonitor(NotificationManager& notifications);
  ~OkaruDeviceMonitor() override;

  OkaruDeviceMonitor(const OkaruDeviceMonitor&) = delete;
  OkaruDeviceMonitor& operator=(const OkaruDeviceMonitor&) = delete;

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
