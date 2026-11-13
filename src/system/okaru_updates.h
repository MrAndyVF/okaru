#pragma once

// OKARU: actualizaciones del sistema (antes el módulo Luau okaru/arch-updater, de yuuto, MIT). Un motor compartido
// revisa pacman (checkupdates), el ayudante de AUR, Flatpak y Snap; actualiza con okaru-update en una terminal o en
// segundo plano (log en vivo), guarda el historial para revertir desde la caché de pacman y el gráfico de actividad.
// Lo usan el widget "updates", el panel okaru-updates y el lanzador (/arch).

#include "config/config_types.h"
#include "core/timer_manager.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;

namespace okaru::updates {

  struct Package {
    std::string name;
    std::string from;
    std::string to;
    std::string source; // pacman | aur | flatpak | snap (en ignorados: settings | system)
    bool operator==(const Package&) const = default;
  };

  struct Source {
    int count = 0;
    std::vector<Package> items; // hasta kMaxListed
  };

  struct Run {
    std::int64_t at = 0;
    bool rollback = false;
    std::vector<Package> packages;
  };

  struct ActivityPoint {
    int count = 0;
    std::int64_t at = 0;
    bool afterUpdate = false;
  };

  enum class Phase : std::uint8_t { Idle, Checking, Clean, Ready, Running, Error, Missing };

  inline constexpr std::size_t kMaxListed = 300;
  inline constexpr std::size_t kLogTailLines = 14;

  // ── Funciones puras (probadas en okaru_updates_test) ──

  // Nombres que llegan a la línea de comandos: solo la gramática de pacman (también vale para Flatpak y Snap).
  [[nodiscard]] bool validName(std::string_view name);
  [[nodiscard]] bool validVersion(std::string_view version);
  // Formato de checkupdates / `yay -Qua`: "nombre vieja -> nueva", con " [ignored]" si lo ignora pacman.conf.
  // Los ignorados (por la lista o por pacman.conf) van a `ignoredOut` en lugar de contarse.
  [[nodiscard]] Source parseVersionLines(std::string_view output, std::string_view source,
                                         const std::set<std::string>& ignored, std::vector<Package>& ignoredOut);
  // Filas "nombre\tvieja\tnueva" (Flatpak).
  [[nodiscard]] Source parseTabLines(std::string_view output, std::string_view source,
                                     const std::set<std::string>& ignored, std::vector<Package>& ignoredOut);
  // `snap list` y `snap refresh --list`, ambos con encabezado; la versión instalada sale de la primera.
  [[nodiscard]] Source parseSnap(std::string_view listOutput, std::string_view refreshOutput,
                                 const std::set<std::string>& ignored, std::vector<Package>& ignoredOut);
  // Suma "Download Size" de `LC_ALL=C pacman -Si` (una vez por paquete). nullopt si no hay ninguno.
  [[nodiscard]] std::optional<double> parseDownloadSizeMiB(std::string_view pacmanSi);
  // Comando okaru-update según los ajustes; `background` = sin preguntas (pkexec).
  [[nodiscard]] std::string updateCommand(const UpdatesConfig& config, bool background, bool aurHelperFound,
                                          std::string_view aurHelper);
  // Lo que la terminal mostró al final de cada línea (texto tras el último \r, sin escapes ANSI).
  [[nodiscard]] std::string cleanLogLine(std::string_view line);
  // Paquetes ya procesados en el log (pacman/AUR y Flatpak) para la barra de progreso.
  [[nodiscard]] int countProgress(std::string_view log);
  [[nodiscard]] std::string shellQuote(std::string_view value);

  class Engine;
  // "Pacman 12 · AUR (yay) 3 · Flatpak 1": solo los canales con pendientes.
  [[nodiscard]] std::string breakdown(const Engine& engine);
  [[nodiscard]] std::string statusText(const Engine& engine);

  class Engine {
  public:
    static Engine& instance();

    void setConfigService(ConfigService* config) { m_config = config; }
    void configure(const UpdatesConfig& config);

    int addListener(std::function<void()> callback);
    void removeListener(int id);

    void check();
    // Según el modo de los ajustes; `forceTerminal` para el «Reintentar en terminal».
    void update(bool forceTerminal = false);
    void rollbackRun(std::int64_t at);
    void rollbackPackage(std::int64_t at, const std::string& name);
    void ignore(const std::string& name);
    void unignore(const std::string& name);
    void dismiss();
    void openLog() const;
    void installPolkitRule();

    [[nodiscard]] const UpdatesConfig& config() const noexcept { return m_settings; }
    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] const std::string& step() const noexcept { return m_step; }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] int total() const noexcept;
    [[nodiscard]] const Source& pacman() const noexcept { return m_pacman; }
    [[nodiscard]] const Source& aur() const noexcept { return m_aur; }
    [[nodiscard]] const Source& flatpak() const noexcept { return m_flatpak; }
    [[nodiscard]] const Source& snap() const noexcept { return m_snap; }
    [[nodiscard]] const std::string& aurHelper() const noexcept { return m_aurHelperUsed; }
    [[nodiscard]] const std::vector<Package>& ignoredPending() const noexcept { return m_ignoredPending; }
    [[nodiscard]] std::optional<double> downloadSizeMiB() const noexcept { return m_sizeMiB; }
    [[nodiscard]] bool rebootRecommended() const noexcept { return m_reboot; }
    [[nodiscard]] const std::string& checkedAt() const noexcept { return m_checkedAt; }
    [[nodiscard]] const std::vector<std::string>& logTail() const noexcept { return m_logTail; }
    [[nodiscard]] int progressDone() const noexcept { return m_runDone; }
    [[nodiscard]] int progressTotal() const noexcept { return m_runTotal; }
    [[nodiscard]] bool runIsRollback() const noexcept { return m_runRollback; }
    [[nodiscard]] bool runInTerminal() const noexcept { return m_runTerminal; }
    [[nodiscard]] std::optional<int> lastExit() const noexcept { return m_runExit; }
    [[nodiscard]] const std::vector<Run>& runs() const noexcept { return m_runs; }
    [[nodiscard]] const std::vector<ActivityPoint>& activity() const noexcept { return m_activity; }
    [[nodiscard]] std::int64_t lastUpdateAt() const noexcept { return m_lastUpdateAt; }
    [[nodiscard]] bool polkitRuleInstalled() const noexcept { return m_polkitInstalled; }
    // Versión vieja de un paquete de un run en la caché (pacman, paru o yay) para revertir.
    [[nodiscard]] bool cached(const Package& package) const;

  private:
    Engine() = default;
    void load();
    void save() const;
    void notifyListeners() const;
    void scheduleAutoCheck();
    void failCheck(const std::string& message);
    void checkAur();
    void checkFlatpak();
    void checkSnap();
    void checkSize();
    void finishCheck();
    void beginRun(bool rollback, bool terminal, int expected, std::vector<Package> packages);
    void startRun(const std::string& body, bool rollback, bool terminal, std::vector<Package> packages);
    void pollRun();
    void finishRun(int code);
    void recordRun(bool rollback, std::vector<Package> packages);
    void resumeRun();
    void checkPolkitRule();
    [[nodiscard]] std::set<std::string> ignoredSet() const;
    [[nodiscard]] std::vector<Package> pendingPackages() const;
    void runRollback(const std::vector<Package>& targets);

    ConfigService* m_config = nullptr;
    UpdatesConfig m_settings;
    bool m_configured = false;
    std::vector<std::pair<int, std::function<void()>>> m_callbacks;
    int m_nextId = 1;

    Phase m_phase = Phase::Idle;
    std::string m_step;
    std::string m_error;
    Source m_pacman;
    Source m_aur;
    Source m_flatpak;
    Source m_snap;
    std::string m_aurHelperUsed;
    std::vector<Package> m_ignoredPending;
    std::optional<double> m_sizeMiB;
    bool m_reboot = false;
    std::string m_checkedAt;
    bool m_checkAfterRun = false;

    bool m_runRollback = false;
    bool m_runTerminal = false;
    int m_runDone = 0;
    int m_runTotal = 0;
    std::optional<int> m_runExit;
    std::vector<Package> m_runPackages;
    std::vector<std::string> m_logTail;
    std::int64_t m_runStartedAt = 0;
    std::int64_t m_lastLogSize = -1;
    int m_runStaleSeconds = 0;
    bool m_pidSeenAlive = false;

    std::vector<Run> m_runs;
    std::vector<ActivityPoint> m_activity;
    std::int64_t m_lastUpdateAt = 0;
    bool m_polkitInstalled = false;
    bool m_loaded = false;

    Timer m_autoTimer;
    Timer m_pollTimer;
  };

} // namespace okaru::updates
