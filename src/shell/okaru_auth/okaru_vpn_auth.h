#pragma once

// OKARU: credenciales de VPN en el diálogo propio de la shell, sin terminal.
// Igual que GNOME Shell: el auth-dialog del plugin de NetworkManager se ejecuta en
// --external-ui-mode y solo describe qué pedir; la shell lo pide y responde a NM.

#include "dbus/network/network_secret_agent.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::auth {

  class AuthPromptService;

  struct VpnField {
    std::string key;   // nombre del secreto en vpn.secrets ("password", "otp"...)
    std::string label; // texto que muestra el plugin
    std::string value; // valor conocido (se reenvía tal cual si no hay que pedirlo)
    bool shouldAsk = false;
  };

  struct VpnDialogDescription {
    std::string title;
    std::string description;
    std::vector<VpnField> fields;
  };

  // Lee los .name de `pluginDir` y devuelve el auth-dialog del servicio si admite el modo externo.
  [[nodiscard]] std::optional<std::string> findExternalAuthDialog(std::string_view serviceType, const std::string& pluginDir);
  // Entrada que espera el auth-dialog por stdin (DATA_KEY/DATA_VAL ... DONE).
  [[nodiscard]] std::string encodeAuthDialogInput(const std::map<std::string, std::string>& data);
  // Salida del auth-dialog en modo externo (keyfile con [VPN Plugin UI] y un grupo por campo).
  [[nodiscard]] std::optional<VpnDialogDescription> parseAuthDialogOutput(std::string_view output);

  class VpnAuthFlow {
  public:
    VpnAuthFlow(NetworkSecretAgent& agent, AuthPromptService& prompts);
    ~VpnAuthFlow();

    VpnAuthFlow(const VpnAuthFlow&) = delete;
    VpnAuthFlow& operator=(const VpnAuthFlow&) = delete;

  private:
    void onRequest(const NetworkSecretAgent::VpnRequest& request);
    void askNext();
    void reset();

    NetworkSecretAgent& m_agent;
    AuthPromptService& m_prompts;
    std::string m_title;
    std::string m_description;
    std::vector<VpnField> m_pending; // campos que faltan por pedir, en orden
    std::map<std::string, std::string> m_answers;
    std::string m_promptId; // solicitud abierta en el diálogo
  };

} // namespace okaru::auth
