#include "dbus/network/vpn_auto_connect.h"

#include "config/config_service.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
  constexpr auto kSettle = std::chrono::seconds(6); // la red cambia varias veces al conectar: se espera a que quede
} // namespace

VpnAutoConnect::VpnAutoConnect(INetworkService* network, ConfigService* config)
    : m_network(network), m_config(config) {}

int VpnAutoConnect::trustOf(const NetworkState& state, const VpnConfig& config) {
  if (!state.connected) {
    return -1;
  }
  const bool wireless = state.kind == NetworkConnectivity::Wireless;
  if (!wireless && config.trustAllEthernet) {
    return 1;
  }
  const std::string& name = wireless ? state.ssid : state.interfaceName;
  return std::ranges::contains(config.trusted, name) ? 1 : 0;
}

void VpnAutoConnect::onNetworkChanged() {
  if (m_network == nullptr || m_config == nullptr || !m_config->config().vpn.autoConnect) {
    m_lastNetwork.clear();
    return;
  }
  m_settle.start(kSettle, [this] { evaluate(); });
}

void VpnAutoConnect::evaluate() {
  const VpnConfig& cfg = m_config->config().vpn;
  const NetworkState& state = m_network->state();
  const int trust = trustOf(state, cfg);
  if (trust < 0) {
    return;
  }
  const std::string current =
      (state.kind == NetworkConnectivity::Wireless ? "w:" + state.ssid : "e:" + state.interfaceName)
      + (trust == 1 ? "|1" : "|0");
  if (current == m_lastNetwork) {
    return; // misma red: lo que haya hecho el usuario a mano se respeta
  }
  m_lastNetwork = current;
  const auto& vpns = m_network->vpnConnections();
  const bool anyActive = std::ranges::any_of(vpns, [](const VpnConnectionInfo& v) { return v.active; });
  if (trust == 0 && !anyActive && !cfg.defaultConnection.empty()) {
    const auto it = std::ranges::find(vpns, cfg.defaultConnection, &VpnConnectionInfo::uuid);
    if (it != vpns.end() && m_network->activateVpnConnection(*it)) {
      m_autoStarted = it->uuid;
      if (cfg.notify) {
        notify::info("OKARU", i18n::tr("okaru.vpn.title"), i18n::tr("okaru.vpn.auto-connected", "name", it->name));
      }
    }
  } else if (trust == 1 && cfg.autoDisconnectOnTrusted && !m_autoStarted.empty()) {
    const auto it = std::ranges::find(vpns, m_autoStarted, &VpnConnectionInfo::uuid);
    if (it != vpns.end() && it->active && m_network->deactivateVpnConnection(*it) && cfg.notify) {
      notify::info("OKARU", i18n::tr("okaru.vpn.title"), i18n::tr("okaru.vpn.auto-disconnected", "name", it->name));
    }
    m_autoStarted.clear();
  }
}

void VpnAutoConnect::importPluginData() {
  if (m_config == nullptr) {
    return;
  }
  namespace fs = std::filesystem;
  const VpnConfig& cfg = m_config->config().vpn;
  for (const char* id :
       {"/plugins/data/okaru/vpn-manager/data.json", "/plugins/data/andrewdems/vpn-manager/data.json"}) {
    const fs::path path = FileUtils::stateDir() + id;
    std::ifstream in(path);
    if (!in) {
      continue;
    }
    const auto json = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (json.is_object()) {
      if (cfg.trusted.empty() && json.contains("trustedNetworks") && json["trustedNetworks"].is_array()) {
        std::vector<std::string> trusted;
        for (const auto& name : json["trustedNetworks"]) {
          if (name.is_string() && !name.get<std::string>().empty()) {
            trusted.push_back(name.get<std::string>());
          }
        }
        if (!trusted.empty()) {
          (void)m_config->setOverride({"vpn", "trusted"}, trusted);
        }
      }
      if (cfg.defaultConnection.empty() && !json.value("defaultVpnUuid", std::string{}).empty()) {
        (void)m_config->setOverride({"vpn", "default"}, json.value("defaultVpnUuid", std::string{}));
      }
    }
    std::error_code ec;
    fs::rename(path, fs::path(path.string() + ".importado"), ec); // una sola vez
  }
}
