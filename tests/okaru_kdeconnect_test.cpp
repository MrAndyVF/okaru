// OKARU: partes puras del servicio nativo de KDE Connect.
#include "dbus/kdeconnect/kdeconnect_service.h"

#include <cstdlib>
#include <print>
#include <sdbus-c++/Types.h>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_kdeconnect_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  using namespace kdeconnect;
  bool ok = true;

  PhoneDevice d{.id = "abc"};
  applyDeviceProps(d, {
      {"name", sdbus::Variant{std::string{"Galaxy"}}},
      {"type", sdbus::Variant{std::string{"phone"}}},
      {"isReachable", sdbus::Variant{true}},
      {"isPaired", sdbus::Variant{true}},
      {"reachableAddresses", sdbus::Variant{std::vector<std::string>{"fe80::1", "192.168.1.5"}}},
      {"supportedPlugins", sdbus::Variant{std::vector<std::string>{"kdeconnect_battery", "kdeconnect_ping"}}},
      {"pairState", sdbus::Variant{3}}, // no se usa: no debe romper nada
  });
  ok &= expect(d.name == "Galaxy" && d.type == "phone" && d.reachable && d.paired, "propiedades del dispositivo");
  ok &= expect(d.hasPlugin("kdeconnect_ping") && !d.hasPlugin("kdeconnect_sms"), "complementos");
  ok &= expect(firstAddress(d.addresses, false) == "192.168.1.5", "IPv4");
  ok &= expect(firstAddress(d.addresses, true) == "fe80::1", "IPv6");
  ok &= expect(firstAddress({}, false).empty(), "sin direcciones");

  // Un tipo inesperado se ignora y conserva el valor anterior.
  applyDeviceProps(d, {{"name", sdbus::Variant{42}}});
  ok &= expect(d.name == "Galaxy", "tipo equivocado ignorado");

  applyBatteryProps(d, {{"charge", sdbus::Variant{91}}, {"isCharging", sdbus::Variant{true}}});
  ok &= expect(d.battery == 91 && d.charging, "batería");
  applyConnectivityProps(d, {{"cellularNetworkType", sdbus::Variant{std::string{"5G"}}},
                             {"cellularNetworkStrength", sdbus::Variant{3}}});
  ok &= expect(d.networkType == "5G" && d.networkStrength == 3, "conectividad");

  ok &= expect(fileUrl("/home/a/Fotos/mi foto#1.jpg") == "file:///home/a/Fotos/mi%20foto%231.jpg", "URL con reservados");
  ok &= expect(fileUrl("/tmp/canción.mp3") == "file:///tmp/canci%C3%B3n.mp3", "URL con UTF-8");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
