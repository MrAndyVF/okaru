#pragma once

// OKARU: puertos en escucha (antes el plugin mahmoudomiesh/network-monitor, MIT). Un escáner compartido
// ejecuta `ss -tulnpH` cada pocos segundos mientras alguien lo escucha (widget "ports", panel okaru-ports).

#include "core/timer_manager.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::ports {

  struct Listener {
    std::string key; // proto:puerto:pid
    int port = 0;
    std::string proto; // "tcp" | "udp"
    int pid = 0;       // 0 cuando el dueño no es visible (otro usuario o root)
    std::string process;
    std::vector<std::string> addresses;
    bool localOnly = true; // todas las direcciones son loopback
  };

  struct Filters {
    bool includeUdp = false;
    bool hideSystemPorts = true; // < 1024
    bool onlyOwnProcesses = false;
    bool operator==(const Filters&) const = default;
  };

  // Salida de `ss -tulnpH`. Las filas con mismo proto+puerto+pid (IPv4 e IPv6) se unen; primero los de dueño
  // conocido y luego por puerto.
  [[nodiscard]] std::vector<Listener> parse(std::string_view ssOutput, const Filters& filters);
  // Sin pid el dueño no es visible: se detiene por puerto con privilegios (pkexec fuser).
  [[nodiscard]] std::vector<std::string> stopCommand(const Listener& listener);
  // "12K", "1.5M" / "12 KB/s".
  [[nodiscard]] std::string compactRate(double bytesPerSecond);
  [[nodiscard]] std::string fullRate(double bytesPerSecond);

  class Scanner {
  public:
    static Scanner& instance();

    int addListener(std::function<void()> callback);
    void removeListener(int id);
    // shortcut: los filtros e intervalo los fija el widget de la barra; sin widget se usan los valores por
    // defecto. Pasarlos a [system] si alguien usa el panel sin el widget.
    void configure(const Filters& filters, int intervalSeconds);
    void rescan(int delayMs = 0);

    [[nodiscard]] const std::vector<Listener>& listeners() const noexcept { return m_listeners; }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

  private:
    void scan();
    void schedule();

    std::vector<std::pair<int, std::function<void()>>> m_callbacks;
    int m_nextId = 1;
    Filters m_filters;
    int m_intervalSeconds = 5;
    std::string m_raw;
    std::vector<Listener> m_listeners;
    bool m_failed = false;
    bool m_pending = false;
    Timer m_timer;
  };

} // namespace okaru::ports
