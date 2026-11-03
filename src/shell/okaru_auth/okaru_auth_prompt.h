#pragma once

// OKARU: solicitudes de contraseña de la shell (sudo vía `okaru askpass`, y más adelante la VPN).
// Lógica pura: no sabe de paneles ni de IPC, así se prueba sola.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::auth {

  using Clock = std::chrono::steady_clock;

  // Un cliente que deja de preguntar por el resultado se considera muerto.
  inline constexpr auto kClientTimeout = std::chrono::seconds(10);
  // Un pedido nuevo del mismo programa dentro de este plazo es un reintento (contraseña incorrecta).
  inline constexpr auto kRetryWindow = std::chrono::seconds(30);

  struct AuthRequest {
    std::string id;        // 32 hex aleatorios; lo asigna open()
    std::string requester; // programa que pide la contraseña ("hyprpm")
    int requesterPid = 0;  // pid de ese programa (agrupa los reintentos)
    int clientPid = 0;     // pid de quien abrió la solicitud; solo él puede leer la respuesta
    std::string prompt;    // texto del pedido ("[sudo] password for andress: ")
    std::string message;   // contexto opcional que manda quien lanzó la operación
    int attempt = 1;       // > 1 si el intento anterior del mismo programa falló
  };

  enum class AuthResultKind { Pending, Answered, Cancelled, Unknown };

  struct AuthResult {
    AuthResultKind kind = AuthResultKind::Unknown;
    std::string secret;
  };

  class AuthPromptService {
  public:
    // Registra la solicitud y devuelve su id. Vacío si no se pudo generar un id seguro.
    std::string open(AuthRequest request, Clock::time_point now = Clock::now());
    // La solicitud que se muestra ahora: la más antigua sin responder.
    [[nodiscard]] const AuthRequest* current() const;
    void answer(std::string_view id, std::string secret);
    void cancel(std::string_view id);
    // Entrega el resultado a su cliente. Answered y Cancelled se entregan una sola vez.
    AuthResult take(std::string_view id, int clientPid, Clock::time_point now = Clock::now());
    // Quita las solicitudes cuyos clientes dejaron de preguntar. Devuelve si cambió algo.
    bool expire(Clock::time_point now = Clock::now());
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    void setOnChanged(std::function<void()> cb) { m_onChanged = std::move(cb); }

  private:
    struct Entry {
      AuthRequest request;
      AuthResultKind state = AuthResultKind::Pending;
      std::string secret;
      Clock::time_point touched;
    };
    struct Answered {
      int requesterPid = 0;
      int attempt = 0;
      Clock::time_point at;
    };

    Entry* find(std::string_view id);
    void erase(std::string_view id);
    void changed();

    std::vector<Entry> m_entries;
    std::vector<Answered> m_answered;
    std::function<void()> m_onChanged;
  };

  // Sube por los padres de `pid` hasta el primer programa que no sea un intermediario
  // (okaru, sudo, shells). `readStat` devuelve el contenido de /proc/<pid>/stat.
  struct Requester {
    std::string name;
    int pid = 0;
  };
  [[nodiscard]] Requester resolveRequester(int pid, const std::function<std::optional<std::string>(int)>& readStat);
  [[nodiscard]] Requester resolveRequester(int pid);

} // namespace okaru::auth
