#pragma once

// OKARU: autoconexión de la VPN (antes el servicio del plugin vpn-manager). Con [vpn].auto_connect, al entrar a una
// red que no es de confianza conecta la VPN por defecto; al volver a una de confianza la desconecta si la conectó
// ella. Solo actúa cuando cambia la red (y tras unos segundos estable): si el usuario la desconecta a mano en la misma
// red, no se la vuelve a conectar. Viene apagada.

#include "core/timer_manager.h"

#include <string>

class ConfigService;
class INetworkService;
struct NetworkState;
struct VpnConfig;

class VpnAutoConnect {
public:
  VpnAutoConnect(INetworkService* network, ConfigService* config);

  void onNetworkChanged();
  // Una vez: las redes de confianza y la VPN por defecto que guardaba el plugin pasan a [vpn].
  void importPluginData();

  // Puro (probado): ¿la red actual es de confianza? 1 sí, 0 no, -1 sin red.
  [[nodiscard]] static int trustOf(const NetworkState& state, const VpnConfig& config);

private:
  void evaluate();

  INetworkService* m_network = nullptr;
  ConfigService* m_config = nullptr;
  Timer m_settle;
  std::string m_lastNetwork; // red sobre la que ya se actuó
  std::string m_autoStarted; // uuid que conectó la autoconexión
};
