// OKARU: credenciales de VPN (auth-dialog en modo externo) y solicitudes internas del diálogo.
#include "shell/okaru_auth/okaru_auth_prompt.h"
#include "shell/okaru_auth/okaru_vpn_auth.h"

#include <filesystem>
#include <fstream>
#include <print>

using namespace okaru::auth;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_vpn_auth_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  bool ok = true;

  // Salida real de nm-fortisslvpn-auth-dialog --external-ui-mode.
  const auto desc = parseAuthDialogOutput("[VPN Plugin UI]\nVersion=2\nDescription=Autentícate en “VPN X”.\n"
                                          "Title=Authenticate VPN\n\n[password]\nValue=\nLabel=Password\n"
                                          "IsSecret=true\nShouldAsk=true\n\n[otp]\nValue=123\nLabel=OTP\n"
                                          "IsSecret=true\nShouldAsk=false\n");
  ok = expect(desc.has_value(), "keyfile válido") && ok;
  ok = expect(desc->title == "Authenticate VPN" && desc->description == "Autentícate en “VPN X”.", "título") && ok;
  ok = expect(desc->fields.size() == 2, "dos campos") && ok;
  ok = expect(desc->fields[0].key == "password" && desc->fields[0].shouldAsk, "password se pide") && ok;
  ok = expect(desc->fields[1].key == "otp" && !desc->fields[1].shouldAsk && desc->fields[1].value == "123", "otp") && ok;
  ok = expect(!parseAuthDialogOutput("basura").has_value(), "sin [VPN Plugin UI] no vale") && ok;

  ok = expect(
           encodeAuthDialogInput({{"gateway", "1.2.3.4"}, {"user", "yo"}}) ==
               "DATA_KEY=gateway\nDATA_VAL=1.2.3.4\n\nDATA_KEY=user\nDATA_VAL=yo\n\nDONE\n\n",
           "entrada del auth-dialog"
       ) &&
       ok;

  const auto dir = std::filesystem::temp_directory_path() / "okaru_vpn_auth_test";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "a.name") << "[VPN Connection]\nservice=org.x.forti\n[GNOME]\nauth-dialog=/x/forti-auth\n"
                                   "supports-external-ui-mode=true\n";
  std::ofstream(dir / "b.name") << "[VPN Connection]\nservice=org.x.oc\n[GNOME]\nauth-dialog=/x/oc-auth\n";
  ok = expect(findExternalAuthDialog("org.x.forti", dir.string()) == "/x/forti-auth", "auth-dialog encontrado") && ok;
  ok = expect(!findExternalAuthDialog("org.x.oc", dir.string()).has_value(), "sin modo externo no sirve") && ok;
  ok = expect(!findExternalAuthDialog("org.x.nada", dir.string()).has_value(), "servicio desconocido") && ok;
  std::filesystem::remove_all(dir);

  // Solicitud interna: el resultado llega por onDone, no expira y se quita al responder.
  AuthPromptService service;
  AuthResult got;
  const auto t0 = Clock::now();
  const auto id = service.open(
      {.id = {}, .requester = {}, .requesterPid = 0, .clientPid = 0, .prompt = "Password", .message = {},
       .title = "VPN", .attempt = 1, .onDone = [&](AuthResult r) { got = std::move(r); }},
      t0
  );
  ok = expect(!service.expire(t0 + kClientTimeout * 3), "la solicitud interna no expira") && ok;
  service.answer(id, "clave");
  ok = expect(got.kind == AuthResultKind::Answered && got.secret == "clave", "onDone con la clave") && ok;
  ok = expect(service.empty(), "respondida se quita") && ok;

  const auto id2 = service.open(
      {.id = {}, .requester = {}, .requesterPid = 0, .clientPid = 0, .prompt = "p", .message = {}, .title = {},
       .attempt = 1, .onDone = [&](AuthResult r) { got = std::move(r); }}
  );
  service.cancel(id2);
  ok = expect(got.kind == AuthResultKind::Cancelled && service.empty(), "cancelada avisa y se quita") && ok;

  return ok ? 0 : 1;
}
