#pragma once

// OKARU: widget de VPN (antes okaru/vpn-manager:bar). Escudo con el estado y, si se pide, el nombre de la VPN
// conectada. Clic: pestaña okaru-vpn. Clic derecho: conectar o desconectar la VPN por defecto.

#include "shell/bar/widget.h"

#include <string>

class ConfigService;
class Glyph;
class INetworkService;
class Label;
struct wl_output;

class VpnWidget : public Widget {
public:
  struct Options {
    bool showName = false;
  };

  VpnWidget(INetworkService* network, ConfigService* config, wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync();
  void toggleDefault();

  INetworkService* m_network = nullptr;
  ConfigService* m_config = nullptr;
  Options m_options;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  std::string m_lastKey;
};
