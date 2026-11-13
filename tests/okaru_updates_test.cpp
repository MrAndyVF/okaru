// OKARU: parseo de checkupdates/-Qua, Flatpak, Snap y pacman -Si, el comando okaru-update y el progreso del log.
#include "system/okaru_updates.h"

#include <cstdlib>
#include <print>

using namespace okaru::updates;

namespace {
  bool ok = true;
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_updates_test: {}", message);
      ok = false;
    }
  }
} // namespace

int main() {
  {
    // pacman.log: solo las transacciones con "upgraded", con la hora (y su zona) en que terminaron.
    const auto runs = parsePacmanLog(
        "[2026-11-13T14:38:40-0500] [PACMAN] Running 'pacman -Syu'\n"
        "[2026-11-13T14:38:41-0500] [ALPM] transaction started\n"
        "[2026-11-13T14:38:46-0500] [ALPM] upgraded go (2:1.27.4-2 -> 2:1.27.4-3)\n"
        "[2026-11-13T14:38:48-0500] [ALPM] upgraded okaru (1.2.1.r0.gdc1f5f4aa-1 -> 1.2.2.r0.g55afb091d-1)\n"
        "[2026-11-13T14:38:48-0500] [ALPM] transaction completed\n"
        "[2026-11-13T15:00:00-0500] [ALPM] transaction started\n"
        "[2026-11-13T15:00:01-0500] [ALPM] installed nano (8.5-1)\n"
        "[2026-11-13T15:00:02-0500] [ALPM] transaction completed\n"
        "[2026-11-13T16:00:00-0500] [ALPM] transaction started\n"
        "[2026-11-13T16:00:01-0500] [ALPM] upgraded vim (9.1-1 -> 9.1-2)\n"
    );
    expect(runs.size() == 1, "pacman.log: una transacción con actualizaciones (la de instalar y la sin cerrar no)");
    expect(!runs.empty() && runs[0].packages.size() == 2 && runs[0].packages[0].name == "go"
               && runs[0].packages[0].from == "2:1.27.4-2" && runs[0].packages[1].to == "1.2.2.r0.g55afb091d-1",
           "pacman.log: nombres y versiones");
    expect(!runs.empty() && runs[0].at == 1794598728, "pacman.log: hora de fin con la zona horaria");
  }
  std::vector<Package> ignored;
  const auto pacman = parseVersionLines("linux 6.1-1 -> 6.2-1\nmesa 1:24-1 -> 1:25-1\n"
                                        "glibc 2.40-1 -> 2.41-1 [ignored]\nbasura\n",
                                        "pacman", {"mesa"}, ignored);
  expect(pacman.count == 1 && pacman.items.size() == 1 && pacman.items[0].name == "linux"
             && pacman.items[0].from == "6.1-1" && pacman.items[0].to == "6.2-1" && pacman.items[0].source == "pacman",
         "checkupdates: linux pendiente");
  expect(ignored.size() == 2 && ignored[0].name == "mesa" && ignored[0].source == "settings"
             && ignored[1].name == "glibc" && ignored[1].source == "system",
         "checkupdates: mesa por ajustes y glibc por pacman.conf");

  ignored.clear();
  const auto flatpak = parseTabLines("org.app.A\t1.0\t1.1\norg.app.B\tabc1234\tdef5678\n", "flatpak", {}, ignored);
  expect(flatpak.count == 2 && flatpak.items[1].from == "abc1234", "flatpak: filas con tabulador");

  const auto snap = parseSnap("Name Version Rev Tracking\ncore22 20261022 3129 x\nintellij-idea 2026.3 154 y\n",
                              "Name Version Rev Size\nintellij-idea 2026.4 160 1GB\n", {}, ignored);
  expect(snap.count == 1 && snap.items[0].name == "intellij-idea" && snap.items[0].from == "2026.3"
             && snap.items[0].to == "2026.4" && snap.items[0].source == "snap",
         "snap: versión instalada desde snap list");
  expect(parseSnap("Name Version\n", "", {}, ignored).count == 0, "snap: nada pendiente");

  const auto size = parseDownloadSizeMiB("Name            : linux\nDownload Size   : 150.00 MiB\n"
                                         "Name            : mesa\nDownload Size   : 1.00 GiB\n"
                                         "Name            : linux\nDownload Size   : 150.00 MiB\n");
  expect(size.has_value() && *size > 1173.9 && *size < 1174.1, "pacman -Si: suma sin repetir paquetes");
  expect(!parseDownloadSizeMiB("").has_value(), "pacman -Si vacío");

  UpdatesConfig config;
  config.snap = false;
  config.ignore = {"linux", "bad name;rm"};
  expect(updateCommand(config, true, true, "paru") == "okaru-update --noconfirm --aur-helper paru --no-snap --ignore 'linux'",
         "comando en segundo plano");
  expect(updateCommand({}, false, false, "") == "okaru-update --no-aur", "comando en terminal sin AUR");

  expect(cleanLogLine("descargando 10%\rdescargando 100%\x1b[0m\r") == "descargando 100%", "línea del log limpia");
  expect(countProgress("(1/3) upgrading linux\n( 2/3) upgrading mesa\x1b[K\nUpdating app/org.app.A/x86_64\nother\n") == 3,
         "progreso del log");
  expect(validName("lib32-glibc") && !validName("-rf") && !validName("a b") && validVersion("1:2.3-4"),
         "nombres y versiones válidos");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
