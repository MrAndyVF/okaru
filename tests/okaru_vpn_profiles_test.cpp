// OKARU: lo puro de los perfiles VPN (vpn.data de nmcli, salida -t, tipo para importar, configuración de WireGuard).
#include "dbus/network/vpn_profiles.h"

#include <cstdlib>
#include <print>

using namespace vpn;

namespace {
  bool ok = true;
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_vpn_profiles_test: {}", message);
      ok = false;
    }
  }
} // namespace

int main() {
  // vpn.data tal como lo imprime nmcli (con una coma escapada en un valor).
  auto data = parseVpnData("gateway = 200.47.173.6:10443, password-flags = 2, realm = a\\,b, user = EXNOKASOSSA");
  expect(
      data.size() == 4 && data["gateway"] == "200.47.173.6:10443" && data["user"] == "EXNOKASOSSA",
      "vpn.data: claves y valores"
  );
  expect(data["realm"] == "a,b", "vpn.data: coma escapada");
  // Ida y vuelta por el formato que acepta nmcli modify.
  const std::string formatted = formatVpnData(data);
  expect(formatted == "gateway=200.47.173.6:10443,password-flags=2,realm=a\\,b,user=EXNOKASOSSA", "vpn.data: formato");
  expect(parseVpnData(formatted) == data, "vpn.data: ida y vuelta");
  expect(parseVpnData("").empty(), "vpn.data: vacío");

  auto terse =
      parseTerse("GENERAL.STATE:activated\nIP4.ADDRESS[1]:10.0.0.2/32\nIP4.ADDRESS[2]:10.0.0.3/32\nIP4.GATEWAY:\n");
  expect(terse["GENERAL.STATE"] == "activated", "-t: campo simple");
  expect(terse["IP4.ADDRESS"] == "10.0.0.2/32, 10.0.0.3/32", "-t: campos repetidos juntos");
  expect(terse.contains("IP4.GATEWAY") && terse["IP4.GATEWAY"].empty(), "-t: campo vacío");

  expect(importTypeFor("/x/casa.OVPN", "") == "openvpn", "importar: .ovpn");
  expect(importTypeFor("/x/oficina.pcf", "") == "vpnc", "importar: .pcf");
  expect(importTypeFor("/x/wg0.conf", "[Interface]\nPrivateKey = abc\n") == "wireguard", "importar: WireGuard");
  expect(importTypeFor("/x/c.conf", "client\nremote vpn.x 1194\n") == "openvpn", "importar: OpenVPN en .conf");
  expect(importTypeFor("/x/nada.txt", "hola").empty(), "importar: desconocido");

  const std::string wg = wireguardConfig(
      {{"PrivateKey", "priv"}, {"Address", "10.0.0.2/32"}, {"PublicKey", "pub"}, {"Endpoint", "vpn.x:51820"}}
  );
  expect(
      wg.contains("[Interface]\nPrivateKey = priv\nAddress = 10.0.0.2/32\n") && !wg.contains("DNS ="),
      "WireGuard: interfaz sin DNS"
  );
  expect(
      wg.contains("[Peer]\nPublicKey = pub\nEndpoint = vpn.x:51820\nAllowedIPs = 0.0.0.0/0, ::/0"),
      "WireGuard: par con todo el tráfico por defecto"
  );

  expect(typeLabel("org.freedesktop.NetworkManager.fortisslvpn") == "FortiSSL", "etiqueta por servicio");
  expect(!fieldsFor("openvpn").empty() && fieldsFor("openvpn")[0].required, "campos de OpenVPN");
  expect(fieldsFor("desconocido").empty(), "sin campos para un tipo desconocido");
  const auto types = installedTypes();
  expect(!types.empty() && types.back().name != "", "WireGuard siempre entre los tipos");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
