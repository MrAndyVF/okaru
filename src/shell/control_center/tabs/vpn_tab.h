#pragma once

// OKARU: pestaña VPN (okaru-vpn), la gestión nativa que reemplaza al plugin vpn-manager. Lista de VPN con conectar,
// VPN por defecto (estrella), detalles, editar y borrar; crear e importar; y la autoconexión con redes de confianza.
// El editor tiene sus campos por protocolo y el resto de vpn.data como opciones avanzadas.

#include "dbus/network/vpn_profiles.h"
#include "shell/control_center/tab.h"

#include <map>
#include <set>
#include <string>

class ConfigService;
class Flex;
class INetworkService;
struct VpnConnectionInfo;

class VpnTab : public Tab {
public:
  VpnTab(INetworkService* network, ConfigService* config);
  ~VpnTab() override = default;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  enum class Mode : std::uint8_t { List, Edit };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildIfNeeded();
  void requestRebuild();
  [[nodiscard]] std::string stateKey() const;

  void addConnections(Flex& parent);
  void addConnectionRow(Flex& card, const VpnConnectionInfo& vpn);
  void addAutoConnect(Flex& parent);
  void addEditor(Flex& parent);

  void toggle(const VpnConnectionInfo& vpn);
  void setDefault(const std::string& uuid);
  void setTrusted(const std::string& name, bool trusted);
  void startEdit(const VpnConnectionInfo& vpn);
  void startCreate(const std::string& type);
  void saveEdit();
  // Cierra el panel (un selector encima del panel quedaría sin teclado) y abre el de okaru-files; `done` recibe la
  // ruta elegida (o nada) y vuelve a abrir esta pestaña.
  void pickFile(const std::string& title, std::function<void(const std::string&)> done);
  void importVpn();
  void setMessage(std::string message, bool error);
  [[nodiscard]] std::string currentNetwork() const;

  INetworkService* m_network = nullptr;
  ConfigService* m_config = nullptr;
  Flex* m_root = nullptr;
  Flex* m_content = nullptr;
  std::string m_key;

  Mode m_mode = Mode::List;
  std::set<std::string> m_expanded; // uuid con los detalles abiertos
  std::map<std::string, vpn::VpnDetails> m_details;
  std::string m_armed; // borrar: espera el segundo clic
  std::string m_message;
  bool m_messageError = false;
  bool m_busy = false;

  // Editor.
  vpn::VpnProfile m_edit;
  std::map<std::string, std::string> m_fields; // valores de los campos del protocolo
  std::string m_advanced;                      // el resto de vpn.data, "clave=valor" por línea
  std::uint32_t m_editGeneration = 0;          // cambia al abrir el editor: rehace los campos
};
