// OKARU: parser de `ss -tulnpH`, comando para detener y formato de velocidades.
#include "system/okaru_ports.h"

#include <cstdlib>
#include <print>

using namespace okaru::ports;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_ports_test: {}", message);
    }
    return condition;
  }

  constexpr const char* kSample =
      "tcp   LISTEN 0 4096   127.0.0.1:631    0.0.0.0:*  users:((\"cupsd\",pid=900,fd=7))\n"
      "tcp   LISTEN 0 511    0.0.0.0:5173     0.0.0.0:*  users:((\"node\",pid=4242,fd=20))\n"
      "tcp   LISTEN 0 511    [::]:5173        [::]:*     users:((\"node\",pid=4242,fd=21))\n"
      "tcp   LISTEN 0 128    127.0.0.1:8080   0.0.0.0:*  users:((\"app\",pid=10,fd=3),(\"app\",pid=11,fd=3))\n"
      "tcp   LISTEN 0 128    [::1]:9000       [::]:*\n"
      "tcp   LISTEN 0 128    [fe80::1%eth0]:7000 [::]:*\n"
      "udp   UNCONN 0 0      0.0.0.0:5353     0.0.0.0:*  users:((\"avahi\",pid=500,fd=12))\n"
      "basura\n";
} // namespace

int main() {
  bool ok = true;
  const auto list = parse(kSample, {});
  ok &= expect(list.size() == 5, "5 puertos (sin 631 ni UDP; 8080 con dos dueños)");
  ok &= expect(list.size() == 5 && list[0].port == 5173 && list[0].addresses.size() == 2 && !list[0].localOnly,
               "5173 une IPv4 e IPv6 y está expuesto");
  ok &= expect(list.size() == 5 && list[1].port == 8080 && list[1].pid == 10 && list[2].pid == 11 && list[1].localOnly,
               "8080 una fila por dueño, local");
  ok &= expect(list.size() == 5 && list[3].port == 7000 && list[3].addresses[0] == "fe80::1" && list[3].pid == 0,
               "sin dueño al final; se quita %iface y corchetes");
  ok &= expect(list.size() == 5 && list[4].port == 9000 && list[4].localOnly, "::1 es loopback");

  const auto all = parse(kSample, {.includeUdp = true, .hideSystemPorts = false, .onlyOwnProcesses = true});
  ok &= expect(all.size() == 5, "631 y 5353 aparecen; los sin dueño no");

  ok &= expect(stopCommand(list[0]) == std::vector<std::string>{"kill", "4242"}, "con pid: kill");
  ok &= expect(stopCommand(list[3]) == std::vector<std::string>{"pkexec", "fuser", "-k", "-TERM", "7000/tcp"},
               "sin pid: pkexec fuser");

  ok &= expect(compactRate(0) == "0" && compactRate(999) == "999" && compactRate(999.6) == "1.0K", "redondeo de unidad");
  ok &= expect(compactRate(12345) == "12K" && fullRate(1500000) == "1.5 MB/s", "unidades");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
