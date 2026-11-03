// OKARU: solicitudes de contraseña (reintentos, expiración, dueño del resultado, quién pide).
#include "shell/okaru_auth/okaru_auth_prompt.h"

#include <cstdlib>
#include <map>
#include <print>

using namespace okaru::auth;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_auth_prompt_test: {}", message);
    }
    return condition;
  }

  AuthRequest request(int requesterPid, int clientPid) {
    return {.id = {}, .requester = "hyprpm", .requesterPid = requesterPid, .clientPid = clientPid, .prompt = "pw:"};
  }

} // namespace

int main() {
  bool ok = true;
  const auto t0 = Clock::now();
  AuthPromptService service;
  int changes = 0;
  service.setOnChanged([&] { ++changes; });

  const std::string id = service.open(request(100, 200), t0);
  ok = expect(id.size() == 32, "id de 32 hex") && ok;
  ok = expect(service.current() != nullptr && service.current()->attempt == 1, "primer intento visible") && ok;
  ok = expect(service.take(id, 999, t0).kind == AuthResultKind::Unknown, "otro pid no puede leer") && ok;
  ok = expect(service.take(id, 200, t0).kind == AuthResultKind::Pending, "pendiente") && ok;

  service.answer(id, "secreto");
  ok = expect(service.current() == nullptr, "respondida ya no se muestra") && ok;
  const auto answered = service.take(id, 200, t0);
  ok = expect(answered.kind == AuthResultKind::Answered && answered.secret == "secreto", "entrega el secreto") && ok;
  ok = expect(service.take(id, 200, t0).kind == AuthResultKind::Unknown, "se entrega una sola vez") && ok;

  // sudo vuelve a preguntar: mismo programa dentro del plazo = reintento.
  const std::string retry = service.open(request(100, 201), t0 + std::chrono::seconds(2));
  ok = expect(service.current() != nullptr && service.current()->attempt == 2, "segundo intento") && ok;
  service.cancel(retry);
  ok = expect(service.take(retry, 201, t0).kind == AuthResultKind::Cancelled, "cancelada") && ok;
  const std::string other = service.open(request(300, 202), t0 + std::chrono::seconds(3));
  ok = expect(service.current()->attempt == 1, "otro programa empieza en 1") && ok;

  // Cliente muerto: deja de preguntar y la solicitud desaparece.
  ok = expect(!service.expire(t0 + std::chrono::seconds(5)), "no expira antes de tiempo") && ok;
  ok = expect(service.expire(t0 + std::chrono::seconds(20)) && service.empty(), "expira sin cliente") && ok;
  ok = expect(service.take(other, 202, t0).kind == AuthResultKind::Unknown, "expirada es desconocida") && ok;
  ok = expect(changes > 0, "avisa los cambios") && ok;

  // Quién pide: okaru ← sudo ← hyprpm.
  const std::map<int, std::string> procs{
      {10, "10 (okaru) S 9 10"},
      {9, "9 (sudo) S 8 9"},
      {8, "8 (hyprpm) S 1 8"},
      {20, "20 (my (odd) prog) S 1 20"},
  };
  const auto read = [&](int pid) -> std::optional<std::string> {
    const auto it = procs.find(pid);
    return it == procs.end() ? std::nullopt : std::optional(it->second);
  };
  const auto who = resolveRequester(10, read);
  ok = expect(who.name == "hyprpm" && who.pid == 8, "sube hasta hyprpm") && ok;
  ok = expect(resolveRequester(20, read).name == "my (odd) prog", "comm con paréntesis") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
