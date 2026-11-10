#pragma once

// OKARU: okaru-play publica MPRIS (org.mpris.MediaPlayer2) en el bus de sesión para que la barra de OKARU,
// playerctl y las teclas multimedia lo vean y lo controlen. Todo corre en el hilo principal (el bus se despacha
// en el bucle de la app).

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SessionBus;

namespace sdbus {
  class IObject;
}

namespace okaru::player {

  class MpvPlayer;

  class MprisServer {
  public:
    // Lo que el reproductor sabe y MPRIS no: la lista y la ventana.
    struct Hooks {
      std::function<std::size_t()> index;           // archivo actual en la lista
      std::function<std::string()> currentPath;     // ruta o URL del actual
      std::function<bool(int delta)> canStep;       // hay anterior/siguiente
      std::function<void(int delta)> step;          // ir al anterior/siguiente
      std::function<void(const std::string&)> open; // OpenUri
      std::function<void()> quit;
    };

    MprisServer(SessionBus& bus, MpvPlayer& player, Hooks hooks);
    ~MprisServer();

    MprisServer(const MprisServer&) = delete;
    MprisServer& operator=(const MprisServer&) = delete;

    // Tras cada cambio del reproductor: avisa de las propiedades que cambiaron y de los saltos de posición.
    void update();

  private:
    struct Snapshot {
      std::string status;
      std::string trackId;
      std::string title;
      double duration = 0.0;
      double volume = 0.0;
      double rate = 1.0;
      bool canPrevious = false;
      bool canNext = false;
      bool loaded = false;

      bool operator==(const Snapshot&) const = default;
    };

    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] std::int64_t positionUs() const;

    SessionBus& m_bus;
    MpvPlayer& m_player;
    Hooks m_hooks;
    std::unique_ptr<sdbus::IObject> m_object;
    Snapshot m_last;
    // Para detectar saltos (Seeked): dónde debería estar la posición si solo avanzó reproduciendo.
    double m_lastPosition = 0.0;
    std::chrono::steady_clock::time_point m_lastPositionAt;
  };

} // namespace okaru::player
