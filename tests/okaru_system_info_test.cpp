// OKARU: parsers de la información del sistema del módulo Home.
#include "system/okaru_system_info.h"

#include <cstdlib>
#include <print>

using namespace okaru::sysinfo;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_system_info_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  const auto mem = parseMeminfo("MemTotal:       32768000 kB\nMemFree:  100 kB\nMemAvailable:   20000000 kB\n");
  ok = expect(mem.totalKb == 32768000 && mem.availableKb == 20000000, "meminfo") && ok;
  ok = expect(parseOsRelease("NAME=x\nPRETTY_NAME=\"CachyOS Linux\"\n") == "CachyOS Linux", "os-release") && ok;
  ok = expect(
           parseCpuModel("processor\t: 0\nmodel name\t: AMD Ryzen 7 5800X 8-Core Processor\n")
               == "AMD Ryzen 7 5800X 8-Core Processor",
           "cpu"
       )
      && ok;

  const auto disks = parseMounts(
      "proc /proc proc rw 0 0\n"
      "/dev/nvme0n1p2 / btrfs rw,subvol=/@ 0 0\n"
      "/dev/nvme0n1p2 /home btrfs rw,subvol=/@home 0 0\n"
      "/dev/sda1 /mnt/Files ntfs3 rw 0 0\n"
      "/dev/sdb1 /mnt/Mis\\040Fotos exfat rw 0 0\n"
      "tmpfs /tmp tmpfs rw 0 0\n"
  );
  ok = expect(disks.size() == 3, "tres discos reales") && ok;
  ok = expect(disks.size() == 3 && disks[0].mount == "/", "btrfs: el montaje más corto") && ok;
  ok = expect(disks.size() == 3 && disks[2].mount == "/mnt/Mis Fotos", "espacios en la ruta") && ok;
  ok = expect(formatBytes(512) == "512 B" && formatBytes(1536) == "1.5 KB", "formato") && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
