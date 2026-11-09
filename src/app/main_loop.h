#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

class PollSource;
class WaylandConnection;

class MainLoop {
public:
  using PollSourcesProvider = std::function<std::vector<PollSource*>()>;

  // `closeSurfaces` cierra las superficies antes del último flush al salir (la barra en la shell).
  // `stop` termina el bucle; también se activa si el compositor cierra la conexión.
  MainLoop(
      WaylandConnection& wayland, std::atomic<bool>& stop, std::function<void()> closeSurfaces,
      PollSourcesProvider sourcesProvider
  );

  void run();

private:
  WaylandConnection& m_wayland;
  std::atomic<bool>& m_stop;
  std::function<void()> m_closeSurfaces;
  PollSourcesProvider m_sourcesProvider;

  // Absolute deadline by which each source must next be dispatched, armed from
  // the timeout it advertised and retained across iterations until it fires.
  std::unordered_map<PollSource*, std::chrono::steady_clock::time_point> m_sourceDeadlines;
};
