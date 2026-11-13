#include "system/okaru_updates.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "system/terminal_launch.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <sys/utsname.h>

namespace okaru::updates {

  namespace {

    constexpr Logger kLog("updates");
    constexpr auto kCheckTimeout = std::chrono::seconds(45); // cada revisión puede sincronizar un espejo
    constexpr auto kFastTimeout = std::chrono::seconds(20);
    constexpr auto kPollInterval = std::chrono::seconds(2);
    constexpr int kStaleLimitSeconds = 1800; // un run en segundo plano sin avance en 30 min está colgado
    constexpr int kPidGraceSeconds = 20;
    constexpr std::int64_t kResumeMaxAgeSeconds = 6 * 3600;
    constexpr std::size_t kMaxRuns = 15;
    constexpr std::size_t kMaxRunPackages = 100;
    constexpr std::size_t kActivityLength = 10;
    constexpr std::size_t kLogLineMax = 400;
    constexpr int kTerminalClosed = -2;
    constexpr const char* kPolkitPath = "/etc/polkit-1/rules.d/49-okaru-updater-pacman.rules";
    // La del módulo Luau (solo pacman) también cuenta como instalada; instalar la nueva la reemplaza.
    constexpr const char* kPolkitOldPath = "/etc/polkit-1/rules.d/49-arch-updater-pacman.rules";

    constexpr const char* kPolkitRule = R"(/* Installed by OKARU (one password per update run instead of one per
 * pacman transaction). Authentication is kept for ~5 minutes, like sudo's
 * timestamp. Scope: only pkexec launching /usr/bin/pacman or /usr/bin/snap,
 * only for an active local session of a wheel member.
 * Remove: sudo rm /etc/polkit-1/rules.d/49-okaru-updater-pacman.rules
 */
polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.policykit.exec" &&
        (action.lookup("program") == "/usr/bin/pacman" ||
         action.lookup("program") == "/usr/bin/snap") &&
        subject.active && subject.local &&
        subject.isInGroup("wheel")) {
        return polkit.Result.AUTH_ADMIN_KEEP;
    }
});
)";

    std::string statePath(std::string_view name) {
      const std::string dir = FileUtils::stateDir();
      return (dir.empty() ? std::string{"."} : dir) + "/" + std::string(name);
    }
    std::string logPath() { return statePath("updates.log"); }
    std::string pidPath() { return statePath("updates-run.pid"); }
    std::string metaPath() { return statePath("updates-run.json"); }
    std::string dataPath() { return statePath("updates.json"); }
    std::string polkitMarkerPath() { return statePath("updates-polkit"); }

    std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

    std::string_view trim(std::string_view s) {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
      }
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
      }
      return s;
    }

    std::vector<std::string_view> splitLines(std::string_view text) {
      std::vector<std::string_view> lines;
      std::size_t start = 0;
      while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!trim(line).empty()) {
          lines.push_back(line);
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      return lines;
    }

    std::vector<std::string_view> fields(std::string_view line, char sep) {
      std::vector<std::string_view> out;
      std::size_t start = 0;
      while (true) {
        const auto end = line.find(sep, start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
          return out;
        }
        start = end + 1;
      }
    }

    std::vector<std::string_view> words(std::string_view line) {
      std::vector<std::string_view> out;
      std::size_t i = 0;
      while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) != 0) {
          ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) == 0) {
          ++i;
        }
        if (i > start) {
          out.push_back(line.substr(start, i - start));
        }
      }
      return out;
    }

    void addPending(
        Source& source, std::string_view name, std::string_view from, std::string_view to, std::string_view key,
        const std::set<std::string>& ignored, std::vector<Package>& ignoredOut
    ) {
      Package pkg{std::string(name), std::string(from), std::string(to), std::string(key)};
      if (ignored.contains(pkg.name)) {
        pkg.source = "settings";
        ignoredOut.push_back(std::move(pkg));
        return;
      }
      ++source.count;
      if (source.items.size() < kMaxListed) {
        source.items.push_back(std::move(pkg));
      }
    }

    // Comando de shell en un hilo; el resultado vuelve al hilo principal.
    void runShell(
        const std::string& command, std::chrono::milliseconds timeout,
        std::function<void(const process::RunResult&)> done
    ) {
      auto shared = std::make_shared<std::function<void(const process::RunResult&)>>(std::move(done));
      process::RunCallbacks callbacks;
      callbacks.onExit = [shared](process::RunResult result) {
        DeferredCall::callLater([shared, result = std::move(result)] { (*shared)(result); });
      };
      if (!process::runAsync(command, std::move(callbacks), {.timeout = timeout})) {
        DeferredCall::callLater([shared] { (*shared)(process::RunResult{}); });
      }
    }

    std::optional<std::string> readText(const std::string& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::ostringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

    std::optional<nlohmann::json> readJson(const std::string& path) {
      const auto text = readText(path);
      if (!text) {
        return std::nullopt;
      }
      try {
        return nlohmann::json::parse(*text);
      } catch (const nlohmann::json::exception& e) {
        kLog.warn("{}: {}", path, e.what());
        return std::nullopt;
      }
    }

    std::string jsonString(const nlohmann::json& json, const char* key) {
      const auto it = json.find(key);
      return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
    }

    Package packageFromJson(const nlohmann::json& json) {
      return {jsonString(json, "name"), jsonString(json, "from"), jsonString(json, "to"), jsonString(json, "source")};
    }

    nlohmann::json packageToJson(const Package& p) {
      return {{"name", p.name}, {"from", p.from}, {"to", p.to}, {"source", p.source}};
    }

    std::vector<Package> packagesFromJson(const nlohmann::json& json) {
      std::vector<Package> out;
      if (json.is_array()) {
        for (const auto& item : json) {
          if (item.is_object()) {
            out.push_back(packageFromJson(item));
          }
        }
      }
      return out;
    }

    bool rollbackable(const Package& p) {
      return p.source != "flatpak" && p.source != "snap" && validName(p.name) && validVersion(p.from);
    }

    // Archivo del paquete en la caché de pacman o en la de compilación de paru/yay.
    std::optional<std::string> findCached(const std::string& name, const std::string& version) {
      const std::string prefix = name + "-" + version + "-";
      const auto matches = [&prefix](const std::filesystem::path& file) {
        const std::string fn = file.filename().string();
        return fn.starts_with(prefix)
            && (fn.ends_with(".pkg.tar.zst") || fn.ends_with(".pkg.tar.xz"))
            // "foo-1.0-1-x86_64.pkg.tar.zst": tras el prefijo solo quedan pkgrel y arquitectura.
            && std::ranges::count(fn.substr(prefix.size()), '-') == 1;
      };
      std::vector<std::filesystem::path> dirs{"/var/cache/pacman/pkg"};
      const std::string home = FileUtils::expandUserPath("~");
      std::error_code ec;
      for (const char* base : {"/.cache/paru/clone", "/.cache/yay"}) {
        for (const auto& entry : std::filesystem::directory_iterator(home + base, ec)) {
          if (entry.is_directory(ec)) {
            dirs.push_back(entry.path());
          }
        }
      }
      for (const auto& dir : dirs) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
          if (matches(entry.path())) {
            return entry.path().string();
          }
        }
      }
      return std::nullopt;
    }

    std::string uname() {
      struct utsname info{};
      return ::uname(&info) == 0 ? std::string(info.release) : std::string{};
    }

  } // namespace

  // ── Funciones puras ──

  bool validName(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '+' || c == '-';
    }) && name.front() != '-';
  }

  bool validVersion(std::string_view version) {
    return !version.empty() && std::ranges::all_of(version, [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) != 0
          || c == ':'
          || c == '.'
          || c == '_'
          || c == '+'
          || c == '~'
          || c == '-';
    });
  }

  Source parseVersionLines(
      std::string_view output, std::string_view source, const std::set<std::string>& ignored,
      std::vector<Package>& ignoredOut
  ) {
    Source out;
    for (const auto line : splitLines(output)) {
      const auto w = words(line);
      if (w.size() < 4 || w[2] != "->") {
        continue;
      }
      if (w.size() >= 5 && w[4] == "[ignored]") {
        ignoredOut.push_back({std::string(w[0]), std::string(w[1]), std::string(w[3]), "system"});
        continue;
      }
      addPending(out, w[0], w[1], w[3], source, ignored, ignoredOut);
    }
    return out;
  }

  Source parseTabLines(
      std::string_view output, std::string_view source, const std::set<std::string>& ignored,
      std::vector<Package>& ignoredOut
  ) {
    Source out;
    for (const auto line : splitLines(output)) {
      const auto f = fields(line, '\t');
      const auto name = trim(f[0]);
      if (name.empty()) {
        continue;
      }
      addPending(
          out, name, f.size() > 1 ? trim(f[1]) : "", f.size() > 2 ? trim(f[2]) : "", source, ignored, ignoredOut
      );
    }
    return out;
  }

  Source parseSnap(
      std::string_view listOutput, std::string_view refreshOutput, const std::set<std::string>& ignored,
      std::vector<Package>& ignoredOut
  ) {
    std::map<std::string, std::string, std::less<>> installed;
    const auto listLines = splitLines(listOutput);
    for (std::size_t i = 1; i < listLines.size(); ++i) {
      const auto w = words(listLines[i]);
      if (w.size() >= 2) {
        installed.emplace(std::string(w[0]), std::string(w[1]));
      }
    }
    Source out;
    const auto refreshLines = splitLines(refreshOutput);
    for (std::size_t i = 1; i < refreshLines.size(); ++i) {
      const auto w = words(refreshLines[i]);
      if (w.size() < 2) {
        continue;
      }
      const auto it = installed.find(w[0]);
      addPending(
          out, w[0], it != installed.end() ? std::string_view(it->second) : "", w[1], "snap", ignored, ignoredOut
      );
    }
    return out;
  }

  std::optional<double> parseDownloadSizeMiB(std::string_view pacmanSi) {
    std::set<std::string> seen;
    std::string name;
    double sum = 0.0;
    bool any = false;
    for (const auto line : splitLines(pacmanSi)) {
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      const auto key = trim(line.substr(0, colon));
      const auto value = trim(line.substr(colon + 1));
      if (key == "Name") {
        name = std::string(value);
      } else if (key == "Download Size" && seen.insert(name).second) {
        const auto w = words(value);
        if (w.size() < 2) {
          continue;
        }
        std::string number(w[0]);
        std::ranges::replace(number, ',', '.');
        double v = 0.0;
        if (std::from_chars(number.data(), number.data() + number.size(), v).ec != std::errc{}) {
          continue;
        }
        if (w[1] == "GiB") {
          v *= 1024.0;
        } else if (w[1] == "KiB") {
          v /= 1024.0;
        } else if (w[1] == "B") {
          v /= 1024.0 * 1024.0;
        }
        sum += v;
        any = true;
      }
    }
    return any ? std::optional<double>(sum) : std::nullopt;
  }

  std::string shellQuote(std::string_view value) {
    std::string out = "'";
    for (const char c : value) {
      if (c == '\'') {
        out += "'\\''";
      } else {
        out += c;
      }
    }
    return out + "'";
  }

  std::string
  updateCommand(const UpdatesConfig& config, bool background, bool aurHelperFound, std::string_view aurHelper) {
    std::string cmd = "okaru-update";
    if (background) {
      cmd += " --noconfirm"; // pkexec pide la contraseña; el resto se responde solo
    }
    cmd += aurHelperFound ? std::format(" --aur-helper {}", aurHelper) : std::string(" --no-aur");
    if (!config.flatpak) {
      cmd += " --no-flatpak";
    }
    if (!config.snap) {
      cmd += " --no-snap";
    }
    std::string ignore;
    for (const auto& name : config.ignore) {
      if (validName(name)) {
        ignore += (ignore.empty() ? "" : ",") + name;
      }
    }
    if (!ignore.empty()) {
      cmd += " --ignore " + shellQuote(ignore);
    }
    return cmd;
  }

  std::string cleanLogLine(std::string_view line) {
    while (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (const auto cr = line.rfind('\r'); cr != std::string_view::npos) {
      line = line.substr(cr + 1);
    }
    std::string out;
    for (std::size_t i = 0; i < line.size() && out.size() < kLogLineMax; ++i) {
      if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
        i += 2;
        while (i < line.size() && (std::isalpha(static_cast<unsigned char>(line[i])) == 0)) {
          ++i;
        }
        continue;
      }
      out += line[i];
    }
    return out;
  }

  int countProgress(std::string_view log) {
    static const std::regex kLine(
        R"(^(\([ 0-9]+/[ 0-9]+\) )?(upgrading|installing|reinstalling|downgrading) |^(Updating|Installing) (app|runtime)/)"
    );
    int count = 0;
    for (const auto line : splitLines(log)) {
      if (std::regex_search(cleanLogLine(line), kLine)) {
        ++count;
      }
    }
    return count;
  }

  // ── Textos compartidos por el widget, el panel y el lanzador ──

  // "Pacman 12 · AUR (yay) 3 · Flatpak 1 · Snap 2": solo los canales con pendientes.
  std::string breakdown(const Engine& engine) {
    std::string out;
    const auto add = [&out](const std::string& label, int count) {
      if (count > 0) {
        out += (out.empty() ? "" : " · ") + std::format("{} {}", label, count);
      }
    };
    add(i18n::tr("okaru.updates.source.pacman"), engine.pacman().count);
    add(engine.aurHelper().empty() ? i18n::tr("okaru.updates.source.aur") : std::format("AUR ({})", engine.aurHelper()),
        engine.aur().count);
    add(i18n::tr("okaru.updates.source.flatpak"), engine.flatpak().count);
    add(i18n::tr("okaru.updates.source.snap"), engine.snap().count);
    return out;
  }

  std::string statusText(const Engine& engine) {
    switch (engine.phase()) {
    case Phase::Idle:
      return i18n::tr("okaru.updates.status-idle");
    case Phase::Checking:
      return engine.step().empty()
          ? i18n::tr("okaru.updates.status-checking")
          : i18n::tr("okaru.updates.status-checking-step", "step", i18n::tr("okaru.updates.source." + engine.step()));
    case Phase::Clean:
      return i18n::tr("okaru.updates.status-clean");
    case Phase::Ready:
      return i18n::trp("okaru.updates.pending", engine.total());
    case Phase::Running:
      return i18n::tr(
          engine.runIsRollback()       ? "okaru.updates.status-rolling-back"
              : engine.runInTerminal() ? "okaru.updates.status-running-terminal"
                                       : "okaru.updates.status-running"
      );
    case Phase::Error:
    case Phase::Missing:
      return engine.error().empty() ? i18n::tr("okaru.updates.status-error") : engine.error();
    }
    return {};
  }

  // ── Motor ──

  Engine& Engine::instance() {
    static Engine engine;
    return engine;
  }

  int Engine::addListener(std::function<void()> callback) {
    const int id = m_nextId++;
    m_callbacks.emplace_back(id, std::move(callback));
    return id;
  }

  void Engine::removeListener(int id) {
    std::erase_if(m_callbacks, [id](const auto& entry) { return entry.first == id; });
  }

  void Engine::notifyListeners() const {
    const auto callbacks = m_callbacks; // un callback puede quitar su propio listener
    for (const auto& [id, callback] : callbacks) {
      callback();
    }
  }

  void Engine::configure(const UpdatesConfig& config) {
    const bool first = !m_configured;
    const bool scheduleChanged = first || config.autoCheckHours != m_settings.autoCheckHours;
    m_settings = config;
    m_configured = true;
    if (first) {
      load();
      mergePacmanLog();
      checkPolkitRule();
      resumeRun();
    }
    if (scheduleChanged) {
      scheduleAutoCheck();
    }
    notifyListeners();
  }

  void Engine::scheduleAutoCheck() {
    m_autoTimer.stop();
    if (m_settings.autoCheckHours <= 0) {
      return;
    }
    // La primera revisión, poco después de iniciar; luego cada N horas.
    const auto delay = m_checkedAt.empty() ? std::chrono::milliseconds(15000)
                                           : std::chrono::milliseconds(std::chrono::hours(m_settings.autoCheckHours));
    m_autoTimer.start(delay, [this] {
      check();
      m_autoTimer.startRepeating(std::chrono::hours(m_settings.autoCheckHours), [this] { check(); });
    });
  }

  int Engine::total() const noexcept { return m_pacman.count + m_aur.count + m_flatpak.count + m_snap.count; }

  std::set<std::string> Engine::ignoredSet() const { return {m_settings.ignore.begin(), m_settings.ignore.end()}; }

  std::vector<Package> Engine::pendingPackages() const {
    std::vector<Package> out;
    for (const Source* source : {&m_pacman, &m_aur, &m_flatpak, &m_snap}) {
      for (const auto& item : source->items) {
        if (out.size() < kMaxRunPackages) {
          out.push_back(item);
        }
      }
    }
    return out;
  }

  namespace {
    // Auto: yay y luego paru. Una elección explícita que no está instalada no cae en otro ayudante.
    std::string_view resolveAurHelper(UpdatesAurHelper choice) {
      switch (choice) {
      case UpdatesAurHelper::Off:
        return {};
      case UpdatesAurHelper::Yay:
        return process::commandExists("yay") ? "yay" : "";
      case UpdatesAurHelper::Paru:
        return process::commandExists("paru") ? "paru" : "";
      case UpdatesAurHelper::Auto:
        break;
      }
      if (process::commandExists("yay")) {
        return "yay";
      }
      return process::commandExists("paru") ? "paru" : "";
    }
  } // namespace

  // ── Revisión: pacman → AUR → Flatpak → Snap → tamaño → reinicio ──

  void Engine::check() {
    if (m_phase == Phase::Checking || m_phase == Phase::Running) {
      return;
    }
    if (!process::commandExists("checkupdates")) {
      m_phase = Phase::Missing;
      m_error = i18n::tr("okaru.updates.err-no-checkupdates");
      notifyListeners();
      return;
    }
    m_phase = Phase::Checking;
    m_error.clear();
    m_pacman = m_aur = m_flatpak = m_snap = {};
    m_ignoredPending.clear();
    m_sizeMiB.reset();
    m_step = "pacman";
    notifyListeners();
    // checkupdates sale con 2 cuando no hay nada (no es un error). LC_ALL=C: pacman traduce la marca "[ignored]".
    runShell(
        R"(LC_ALL=C checkupdates 2>/dev/null; c=$?; [ "$c" -eq 2 ] && exit 0; exit "$c")", kCheckTimeout,
        [this](const process::RunResult& r) {
          if (m_phase != Phase::Checking) {
            return;
          }
          if (r.timedOut) {
            failCheck(i18n::tr("okaru.updates.err-timeout"));
            return;
          }
          if (r.exitCode != 0) {
            kLog.warn("checkupdates falló ({}): {}", r.exitCode, r.err);
            failCheck(i18n::tr("okaru.updates.err-check"));
            return;
          }
          m_pacman = parseVersionLines(r.out, "pacman", ignoredSet(), m_ignoredPending);
          checkAur();
        }
    );
  }

  void Engine::checkAur() {
    m_aurHelperUsed = resolveAurHelper(m_settings.aurHelper);
    if (m_aurHelperUsed.empty()) {
      checkFlatpak();
      return;
    }
    m_step = "aur";
    notifyListeners();
    runShell("LC_ALL=C " + m_aurHelperUsed + " -Qua", kCheckTimeout, [this](const process::RunResult& r) {
      if (m_phase != Phase::Checking) {
        return;
      }
      // -Qua sale distinto de 0 también cuando no hay nada; un fallo real escribe en stderr.
      if (!r.timedOut && r.exitCode != 0 && !trim(r.err).empty()) {
        kLog.warn("{} -Qua falló: {}", m_aurHelperUsed, r.err);
        failCheck(i18n::tr("okaru.updates.err-aur"));
        return;
      }
      if (!r.timedOut) {
        m_aur = parseVersionLines(r.out, "aur", ignoredSet(), m_ignoredPending);
      }
      checkFlatpak();
    });
  }

  void Engine::checkFlatpak() {
    if (!m_settings.flatpak || !process::commandExists("flatpak")) {
      checkSnap();
      return;
    }
    m_step = "flatpak";
    notifyListeners();
    // Flatpak sigue commits: si la versión no cambia, se muestran los commits cortos. Cada salida se captura antes
    // del awk para que un fallo real (sin remoto, sin red) no pase como lista vacía.
    runShell(
        R"(
listOut=$(flatpak list --columns=application,version,active 2>/dev/null) || exit 1
updOut=$(flatpak remote-ls --updates --columns=application,version,commit 2>/dev/null) || exit 1
{ printf '%s\n' "$listOut" | sed 's/^/L /'
  printf '%s\n' "$updOut" | sed 's/^/R /'
} | awk -F'\t' '
{ tag=substr($1,1,1); app=substr($1,3) }
tag=="L" { v[app]=$2; c[app]=$3 }
tag=="R" && app!="" { from=v[app]; to=$2
           if (from=="" || to=="" || from==to) { from=substr(c[app],1,7); to=substr($3,1,7) }
           print app"\t"from"\t"to }')",
        kCheckTimeout, [this](const process::RunResult& r) {
          if (m_phase != Phase::Checking) {
            return;
          }
          if (!r.timedOut && r.exitCode != 0) {
            failCheck(i18n::tr("okaru.updates.err-flatpak"));
            return;
          }
          if (!r.timedOut) {
            m_flatpak = parseTabLines(r.out, "flatpak", ignoredSet(), m_ignoredPending);
          }
          checkSnap();
        }
    );
  }

  void Engine::checkSnap() {
    if (!m_settings.snap || !process::commandExists("snap")) {
      checkSize();
      return;
    }
    m_step = "snap";
    notifyListeners();
    runShell(
        R"(l=$(LC_ALL=C snap list 2>/dev/null) || exit 1
u=$(LC_ALL=C snap refresh --list 2>/dev/null) || exit 1
printf '%s\n::REFRESH::\n%s\n' "$l" "$u")",
        kCheckTimeout, [this](const process::RunResult& r) {
          if (m_phase != Phase::Checking) {
            return;
          }
          if (!r.timedOut && r.exitCode != 0) {
            failCheck(i18n::tr("okaru.updates.err-snap"));
            return;
          }
          if (!r.timedOut) {
            const std::string_view out = r.out;
            const auto split = out.find("::REFRESH::");
            if (split != std::string_view::npos) {
              m_snap = parseSnap(out.substr(0, split), out.substr(split + 11), ignoredSet(), m_ignoredPending);
            }
          }
          checkSize();
        }
    );
  }

  void Engine::checkSize() {
    if (!m_settings.showDownloadSize
        || m_pacman.items.empty()
        || m_pacman.items.size() < static_cast<std::size_t>(m_pacman.count)) {
      finishCheck(); // con más pendientes que nombres guardados el tamaño saldría corto: mejor no mostrarlo
      return;
    }
    std::string cmd = "LC_ALL=C pacman -Si";
    for (const auto& item : m_pacman.items) {
      cmd += " " + shellQuote(item.name);
    }
    runShell(cmd + " 2>/dev/null", kFastTimeout, [this](const process::RunResult& r) {
      if (m_phase != Phase::Checking) {
        return;
      }
      m_sizeMiB = r.timedOut ? std::nullopt : parseDownloadSizeMiB(r.out);
      finishCheck();
    });
  }

  void Engine::finishCheck() {
    std::error_code ec;
    // Al actualizar el kernel desaparece /usr/lib/modules/<versión en uso>: hace falta reiniciar.
    const std::string release = uname();
    m_reboot =
        m_settings.checkReboot && !release.empty() && !std::filesystem::exists("/usr/lib/modules/" + release, ec);
    m_step.clear();
    mergePacmanLog(); // lo actualizado fuera del panel desde la última revisión
    m_phase = total() > 0 ? Phase::Ready : Phase::Clean;
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    m_checkedAt = std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    if (m_settings.activityGraph) {
      m_activity.push_back({total(), now(), m_checkAfterRun});
      if (m_activity.size() > kActivityLength) {
        m_activity.erase(m_activity.begin(), m_activity.end() - static_cast<std::ptrdiff_t>(kActivityLength));
      }
      save();
    }
    m_checkAfterRun = false;
    notifyListeners();
    if (total() > 0 && m_settings.notify) {
      notify::info("OKARU", i18n::tr("okaru.updates.title"), i18n::trp("okaru.updates.pending", total()));
    }
  }

  void Engine::failCheck(const std::string& message) {
    m_phase = Phase::Error;
    m_error = message;
    m_step.clear();
    notifyListeners();
  }

  // ── Actualizar y revertir ──

  void Engine::update(bool forceTerminal) {
    if (m_phase == Phase::Checking || m_phase == Phase::Running) {
      return;
    }
    const bool terminal = forceTerminal || m_settings.mode == UpdatesMode::Terminal;
    if (!process::commandExists("okaru-update")) {
      failCheck(i18n::tr("okaru.updates.err-no-okaru-update"));
      return;
    }
    if (!terminal && !process::commandExists("pkexec")) {
      failCheck(i18n::tr("okaru.updates.err-no-pkexec"));
      return;
    }
    const std::string helper(resolveAurHelper(m_settings.aurHelper));
    startRun(updateCommand(m_settings, !terminal, !helper.empty(), helper), false, terminal, pendingPackages());
  }

  void Engine::startRun(const std::string& body, bool rollback, bool terminal, std::vector<Package> packages) {
    const std::string log = shellQuote(logPath());
    // El pid primero: así se nota una terminal cerrada o un run muerto, que nunca escriben ::EXIT.
    const std::string start =
        "echo $$ > " + shellQuote(pidPath()) + "; printf '::START %s\\n' \"$(date +%s)\" > " + log;
    bool launched = false;
    if (terminal) {
      // `script` da una pty real para que pacman dibuje su barra de progreso aunque la salida pase por tee.
      const std::string inner = process::commandExists("script")
          ? "SHELL=/bin/sh script -q -f -e -c " + shellQuote(body) + " /dev/null"
          : body;
      const std::string wrapped = start
          + "; { "
          + inner
          + R"(; printf '\n::EXIT %s\n' "$?"; } 2>&1 | tee -a )"
          + log
          + "; echo; printf '%s' "
          + shellQuote(i18n::tr("okaru.updates.press-enter"))
          + "; read -r _";
      launched = terminal_launch::launch(wrapped);
      if (!launched) {
        failCheck(i18n::tr("okaru.updates.err-no-terminal"));
        return;
      }
    } else {
      // LC_ALL=C: la barra de progreso cuenta las líneas en inglés de pacman y Flatpak.
      launched = process::runAsync(
          start + "; { export LC_ALL=C; " + body + " ; } >> " + log + R"( 2>&1; printf '::EXIT %s\n' "$?" >> )" + log
      );
      if (!launched) {
        failCheck(i18n::tr("okaru.updates.err-spawn"));
        return;
      }
    }
    const int expected = rollback ? static_cast<int>(packages.size()) : total();
    nlohmann::json meta = {
        {"rollback", rollback}, {"terminal", terminal}, {"total", expected}, {"packages", nlohmann::json::array()}
    };
    for (const auto& p : packages) {
      meta["packages"].push_back(packageToJson(p));
    }
    std::ofstream(metaPath()) << meta.dump();
    beginRun(rollback, terminal, expected, std::move(packages));
  }

  void Engine::beginRun(bool rollback, bool terminal, int expected, std::vector<Package> packages) {
    m_phase = Phase::Running;
    m_error.clear();
    m_step.clear();
    m_runRollback = rollback;
    m_runTerminal = terminal;
    m_runExit.reset();
    m_runDone = 0;
    m_runTotal = expected;
    m_runPackages = std::move(packages);
    m_logTail.clear();
    m_runStartedAt = now();
    m_lastLogSize = -1;
    m_runStaleSeconds = 0;
    m_pidSeenAlive = false;
    m_pollTimer.startRepeating(kPollInterval, [this] { pollRun(); });
    notifyListeners();
  }

  void Engine::pollRun() {
    if (m_phase != Phase::Running) {
      m_pollTimer.stop();
      return;
    }
    // Estado del proceso: sin pidfile todavía (arrancando), vivo o terminado.
    std::optional<bool> alive;
    if (const auto pidText = readText(pidPath())) {
      int pid = 0;
      const auto t = trim(*pidText);
      if (std::from_chars(t.data(), t.data() + t.size(), pid).ec == std::errc{} && pid > 0) {
        alive = ::kill(pid, 0) == 0 || errno == EPERM;
        m_pidSeenAlive = m_pidSeenAlive || *alive;
      }
    }
    const auto text = readText(logPath()).value_or(std::string{});
    const auto size = static_cast<std::int64_t>(text.size());
    if (size == m_lastLogSize) {
      if (alive.has_value() && !*alive) {
        finishRun(m_runTerminal ? kTerminalClosed : -1);
      } else if (!m_pidSeenAlive && now() - m_runStartedAt > kPidGraceSeconds) {
        finishRun(m_runTerminal ? kTerminalClosed : -1); // nunca llegó a arrancar
      } else if (!m_runTerminal && (m_runStaleSeconds += 2) >= kStaleLimitSeconds) {
        finishRun(-1); // una terminal puede quedarse en una pregunta todo lo que haga falta; el fondo no
      }
      return;
    }
    m_lastLogSize = size;
    m_runStaleSeconds = 0;
    std::optional<int> exitCode;
    std::vector<std::string> lines;
    for (const auto line : splitLines(text)) {
      if (line.starts_with("::EXIT ")) {
        int code = -1;
        const auto v = trim(line.substr(7));
        (void)std::from_chars(v.data(), v.data() + v.size(), code);
        exitCode = code;
      } else if (!line.starts_with("::START ")) {
        std::string clean = cleanLogLine(line);
        if (!trim(clean).empty()) {
          lines.push_back(std::move(clean));
        }
      }
    }
    m_runDone = countProgress(text);
    const std::size_t keep = std::min(lines.size(), kLogTailLines);
    m_logTail.assign(lines.end() - static_cast<std::ptrdiff_t>(keep), lines.end());
    if (exitCode) {
      finishRun(*exitCode);
    } else if (alive.has_value() && !*alive) {
      finishRun(m_runTerminal ? kTerminalClosed : -1);
    } else {
      notifyListeners();
    }
  }

  void Engine::finishRun(int code) {
    m_pollTimer.stop();
    m_runExit = code;
    std::error_code ec;
    std::filesystem::remove(pidPath(), ec);
    std::filesystem::remove(metaPath(), ec);
    const bool rollback = m_runRollback;
    auto packages = std::move(m_runPackages);
    m_runPackages.clear();
    if (code != 0) {
      m_phase = Phase::Error;
      m_error = code == kTerminalClosed ? i18n::tr("okaru.updates.err-terminal-closed")
                                        : i18n::tr("okaru.updates.err-run", "code", code);
      notifyListeners();
      notify::error("OKARU", i18n::tr("okaru.updates.title"), m_error);
      return;
    }
    if (!rollback) {
      m_lastUpdateAt = now();
    }
    notify::info(
        "OKARU", i18n::tr("okaru.updates.title"),
        i18n::tr(rollback ? "okaru.updates.rollback-ok" : "okaru.updates.run-ok")
    );
    if (rollback && m_settings.rollbackAutoIgnore) {
      for (const auto& p : packages) {
        ignore(p.name);
      }
    }
    // En la terminal se pudo rechazar algún paquete: solo se guarda lo que de verdad cambió de versión.
    std::string names;
    for (const auto& p : packages) {
      if (rollbackable(p)) {
        names += " " + shellQuote(p.name);
      }
    }
    m_phase = Phase::Idle;
    m_checkAfterRun = true;
    if (names.empty()) {
      recordRun(rollback, std::move(packages));
      check();
      return;
    }
    runShell(
        "LC_ALL=C pacman -Q" + names + " 2>/dev/null", kFastTimeout,
        [this, rollback, packages = std::move(packages)](const process::RunResult& r) mutable {
          if (!r.timedOut) {
            std::map<std::string, std::string, std::less<>> installed;
            for (const auto line : splitLines(r.out)) {
              const auto w = words(line);
              if (w.size() == 2) {
                installed.emplace(std::string(w[0]), std::string(w[1]));
              }
            }
            std::erase_if(packages, [&installed](const Package& p) {
              if (!rollbackable(p)) {
                return false;
              }
              const auto it = installed.find(p.name);
              return it == installed.end() || it->second == p.from;
            });
          }
          recordRun(rollback, std::move(packages));
          check();
        }
    );
  }

  std::vector<Run> parsePacmanLog(std::string_view log) {
    std::vector<Run> runs;
    Run current;
    bool open = false;
    // "[2026-11-13T14:38:48-0500] [ALPM] ..." → segundos desde la época (con su zona horaria).
    const auto stamp = [](std::string_view line) -> std::int64_t {
      if (line.size() < 26 || line[0] != '[') {
        return 0;
      }
      std::tm tm{};
      const std::string text(line.substr(1, 19));
      if (::strptime(text.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) {
        return 0;
      }
      int offset = 0;
      const std::string_view zone = line.substr(20, 5);
      if ((zone[0] == '+' || zone[0] == '-') && std::isdigit(static_cast<unsigned char>(zone[1])) != 0) {
        const int hh = (zone[1] - '0') * 10 + (zone[2] - '0');
        const int mm = (zone[3] - '0') * 10 + (zone[4] - '0');
        offset = (zone[0] == '-' ? -1 : 1) * (hh * 3600 + mm * 60);
      }
      return static_cast<std::int64_t>(::timegm(&tm)) - offset;
    };
    std::size_t start = 0;
    while (start < log.size()) {
      std::size_t end = log.find('\n', start);
      if (end == std::string_view::npos) {
        end = log.size();
      }
      const std::string_view line = log.substr(start, end - start);
      start = end + 1;
      if (line.contains("[ALPM] transaction started")) {
        current = {};
        open = true;
      } else if (open && line.contains("[ALPM] transaction completed")) {
        open = false;
        if (!current.packages.empty()) {
          current.at = stamp(line);
          runs.push_back(std::move(current));
          current = {};
        }
      } else if (open && line.contains("[ALPM] upgraded ")) {
        // "upgraded nombre (vieja -> nueva)"
        const auto at = line.find("[ALPM] upgraded ") + 16;
        const auto paren = line.find(" (", at);
        const auto arrow = line.find(" -> ", paren);
        const auto close = line.rfind(')');
        if (paren == std::string_view::npos
            || arrow == std::string_view::npos
            || close == std::string_view::npos
            || close < arrow) {
          continue;
        }
        Package package{
            .name = std::string(line.substr(at, paren - at)),
            .from = std::string(line.substr(paren + 2, arrow - paren - 2)),
            .to = std::string(line.substr(arrow + 4, close - arrow - 4)),
            .source = "pacman",
        };
        if (validName(package.name) && validVersion(package.from) && validVersion(package.to)) {
          current.packages.push_back(std::move(package));
        }
      }
    }
    return runs;
  }

  void Engine::mergePacmanLog() {
    // Solo la cola del log: basta para las últimas transacciones y el archivo crece sin límite.
    std::ifstream in("/var/log/pacman.log", std::ios::binary | std::ios::ate);
    if (!in) {
      return;
    }
    constexpr std::streamoff kTail = 2 * 1024 * 1024;
    const std::streamoff size = in.tellg();
    in.seekg(std::max<std::streamoff>(0, size - kTail));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bool changed = false;
    for (Run& run : parsePacmanLog(text)) {
      if (run.at <= m_historyClearedAt || run.at <= 0) {
        continue;
      }
      // Ya está si el panel grabó esa actualización (unos minutos antes, con alguno de sus paquetes) o si es la misma.
      const bool known = std::ranges::any_of(m_runs, [&](const Run& r) {
        if (r.at == run.at) {
          return true;
        }
        if (std::llabs(r.at - run.at) > 15 * 60) {
          return false;
        }
        return std::ranges::any_of(r.packages, [&](const Package& p) {
          return std::ranges::any_of(run.packages, [&](const Package& q) { return p.name == q.name && p.to == q.to; });
        });
      });
      m_lastUpdateAt = std::max(m_lastUpdateAt, run.at);
      if (known) {
        continue;
      }
      if (run.packages.size() > kMaxRunPackages) {
        run.packages.resize(kMaxRunPackages);
      }
      m_runs.push_back(std::move(run));
      changed = true;
    }
    if (!changed) {
      return;
    }
    std::ranges::sort(m_runs, {}, &Run::at);
    if (m_runs.size() > kMaxRuns) {
      m_runs.erase(m_runs.begin(), m_runs.end() - static_cast<std::ptrdiff_t>(kMaxRuns));
    }
    save();
  }

  void Engine::clearHistory() {
    m_runs.clear();
    m_historyClearedAt = now();
    save();
    notifyListeners();
  }

  void Engine::recordRun(bool rollback, std::vector<Package> packages) {
    if (packages.empty()) {
      return;
    }
    if (packages.size() > kMaxRunPackages) {
      packages.resize(kMaxRunPackages);
    }
    m_runs.push_back({now(), rollback, std::move(packages)});
    if (m_runs.size() > kMaxRuns) {
      m_runs.erase(m_runs.begin(), m_runs.end() - static_cast<std::ptrdiff_t>(kMaxRuns));
    }
    save();
    notifyListeners();
  }

  // Un run que sobrevivió al reinicio de la shell (se lanza separado): log con ::START reciente y sin ::EXIT.
  void Engine::resumeRun() {
    const auto text = readText(logPath());
    if (!text || !text->starts_with("::START ") || text->contains("\n::EXIT ")) {
      return;
    }
    std::int64_t started = 0;
    const std::string_view head(*text);
    const auto first = head.substr(8, head.find('\n') - 8);
    if (std::from_chars(first.data(), first.data() + first.size(), started).ec != std::errc{}
        || now() - started > kResumeMaxAgeSeconds) {
      return;
    }
    const auto meta = readJson(metaPath());
    if (!meta || !meta->is_object()) {
      return;
    }
    const bool terminal = meta->value("terminal", false);
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(logPath(), ec);
    if (!terminal && !ec) {
      const auto age =
          std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - mtime);
      if (age.count() > kStaleLimitSeconds) {
        std::filesystem::remove(metaPath(), ec);
        return;
      }
    }
    beginRun(
        meta->value("rollback", false), terminal, meta->value("total", 0),
        packagesFromJson(meta->value("packages", nlohmann::json::array()))
    );
    m_runStartedAt = started;
  }

  bool Engine::cached(const Package& package) const { return findCached(package.name, package.from).has_value(); }

  void Engine::runRollback(const std::vector<Package>& targets) {
    if (m_phase == Phase::Checking || m_phase == Phase::Running || targets.empty()) {
      return;
    }
    if (!process::commandExists("pkexec")) {
      failCheck(i18n::tr("okaru.updates.err-no-pkexec"));
      return;
    }
    // pacman -U instala la versión elegida directamente. Sin --nodeps: si rompería otro paquete, pacman rechaza
    // toda la transacción antes de tocar nada.
    std::string files;
    std::vector<Package> reversed;
    for (const auto& p : targets) {
      const auto file = findCached(p.name, p.from);
      if (!file) {
        failCheck(i18n::tr("okaru.updates.rollback-missing-named", "name", p.name));
        return;
      }
      files += " " + shellQuote(*file);
      reversed.push_back({p.name, p.to, p.from, p.source});
    }
    startRun("pkexec pacman -U --noconfirm --noprogressbar --color never" + files, true, false, std::move(reversed));
  }

  void Engine::rollbackRun(std::int64_t at) {
    const auto it = std::ranges::find(m_runs, at, &Run::at);
    if (it == m_runs.end()) {
      return;
    }
    std::vector<Package> targets;
    std::ranges::copy_if(it->packages, std::back_inserter(targets), rollbackable);
    runRollback(targets);
  }

  // Un paquete y sus dependencias que cambiaron en el mismo run vuelven juntos (pactree ∩ paquetes del run).
  void Engine::rollbackPackage(std::int64_t at, const std::string& name) {
    const auto it = std::ranges::find(m_runs, at, &Run::at);
    if (it == m_runs.end() || !validName(name)) {
      return;
    }
    const auto target = std::ranges::find(it->packages, name, &Package::name);
    if (target == it->packages.end() || !rollbackable(*target)) {
      return;
    }
    runShell(
        "pactree -l " + shellQuote(name) + " 2>/dev/null", kFastTimeout,
        [this, run = *it, pkg = *target](const process::RunResult& r) {
          std::vector<Package> targets{pkg};
          if (!r.timedOut && r.exitCode == 0) {
            for (const auto line : splitLines(r.out)) {
              // pactree -l escribe dependencias con versión ("libelf=0.196"): solo cuenta el nombre.
              const auto dep = trim(line.substr(0, line.find_first_of("<>=")));
              const auto mate = std::ranges::find(run.packages, dep, &Package::name);
              if (mate != run.packages.end()
                  && mate->name != pkg.name
                  && rollbackable(*mate)
                  && std::ranges::find(targets, mate->name, &Package::name) == targets.end()) {
                targets.push_back(*mate);
              }
            }
          }
          runRollback(targets);
        }
    );
  }

  // ── Ignorados, varios ──

  void Engine::ignore(const std::string& name) {
    if (!validName(name) || m_config == nullptr || std::ranges::contains(m_settings.ignore, name)) {
      return;
    }
    auto list = m_settings.ignore;
    list.push_back(name);
    // Se quita ya de la lista visible; la recarga de la config trae el resto.
    for (Source* source : {&m_pacman, &m_aur, &m_flatpak, &m_snap}) {
      if (const auto it = std::ranges::find(source->items, name, &Package::name); it != source->items.end()) {
        m_ignoredPending.push_back({it->name, it->from, it->to, "settings"});
        source->items.erase(it);
        --source->count;
      }
    }
    if (m_phase == Phase::Ready && total() == 0) {
      m_phase = Phase::Clean;
    }
    (void)m_config->setOverride({"updates", "ignore"}, list);
    notifyListeners();
  }

  void Engine::unignore(const std::string& name) {
    if (m_config == nullptr || !std::ranges::contains(m_settings.ignore, name)) {
      return;
    }
    auto list = m_settings.ignore;
    std::erase(list, name);
    (void)m_config->setOverride({"updates", "ignore"}, list);
    std::erase_if(m_ignoredPending, [&name](const Package& p) { return p.name == name && p.source == "settings"; });
    notifyListeners();
  }

  void Engine::dismiss() {
    m_pacman = m_aur = m_flatpak = m_snap = {};
    m_sizeMiB.reset();
    m_runExit.reset();
    m_logTail.clear();
    m_error.clear();
    m_phase = Phase::Clean;
    notifyListeners();
  }

  void Engine::openLog() const {
    // xdg-open suele abrir texto en un editor de terminal que muere sin tty: mejor un paginador en la terminal.
    if (!terminal_launch::launch("less +G -- " + shellQuote(logPath()))) {
      notify::error("OKARU", i18n::tr("okaru.updates.title"), i18n::tr("okaru.updates.err-no-terminal"));
    }
  }

  void Engine::checkPolkitRule() {
    std::error_code ec;
    std::error_code ecOld;
    if (std::filesystem::exists(kPolkitPath, ec) || std::filesystem::exists(kPolkitOldPath, ecOld)) {
      m_polkitInstalled = true;
    } else if (ec || ecOld) {
      // rules.d suele no ser legible para el usuario: se recuerda la instalación con una marca propia.
      m_polkitInstalled = std::filesystem::exists(polkitMarkerPath(), ec);
    } else {
      m_polkitInstalled = false;
      std::filesystem::remove(polkitMarkerPath(), ec);
    }
  }

  void Engine::installPolkitRule() {
    const std::string staged = statePath("49-okaru-updater-pacman.rules");
    std::ofstream(staged) << kPolkitRule;
    runShell(
        "pkexec sh -c "
            + shellQuote(
                "install -Dm644 -o root -g root "
                + shellQuote(staged)
                + " "
                + kPolkitPath
                + " && rm -f "
                + kPolkitOldPath
            ),
        std::chrono::minutes(2), [this](const process::RunResult& r) {
          if (r.exitCode == 0) {
            std::ofstream(polkitMarkerPath()) << "1";
            m_polkitInstalled = true;
            notifyListeners();
            notify::info("OKARU", i18n::tr("okaru.updates.title"), i18n::tr("okaru.updates.polkit-ok"));
          } else if (r.exitCode != 126) { // 126: se cerró el diálogo de polkit
            notify::error("OKARU", i18n::tr("okaru.updates.title"), i18n::tr("okaru.updates.err-polkit"));
          }
        }
    );
  }

  // ── Persistencia: <estado>/updates.json; la primera vez se importa lo del módulo Luau ──

  void Engine::load() {
    if (m_loaded) {
      return;
    }
    m_loaded = true;
    auto json = readJson(dataPath());
    if (!json || !json->is_object()) {
      const std::string dir = FileUtils::stateDir();
      for (const char* legacy : {"/plugins/data/okaru/updater", "/plugins/data/okaru/arch-updater"}) {
        const auto runs = readJson(dir + legacy + "/runs.json");
        const auto history = readJson(dir + legacy + "/history_state.json");
        if (!runs && !history) {
          continue;
        }
        nlohmann::json imported = nlohmann::json::object();
        if (runs && runs->is_object()) {
          imported["runs"] = runs->value("runs", nlohmann::json::array());
        }
        if (history && history->is_object()) {
          imported["activity"] = history->value("history", nlohmann::json::array());
          imported["lastUpdateAt"] = history->value("lastUpdateAt", 0);
        }
        // La lista de ignorados del panel pasa a [updates].ignore.
        if (const auto ignore = readJson(dir + legacy + "/ignore.json");
            ignore && ignore->is_object() && m_config != nullptr) {
          auto list = m_settings.ignore;
          for (const auto& name : ignore->value("packages", nlohmann::json::array())) {
            if (name.is_string()
                && validName(name.get<std::string>())
                && !std::ranges::contains(list, name.get<std::string>())) {
              list.push_back(name.get<std::string>());
            }
          }
          if (list != m_settings.ignore) {
            (void)m_config->setOverride({"updates", "ignore"}, list);
          }
        }
        kLog.info("historial de actualizaciones importado de {}", legacy);
        json = std::move(imported);
        break;
      }
    }
    if (!json || !json->is_object()) {
      return;
    }
    for (const auto& run : json->value("runs", nlohmann::json::array())) {
      if (run.is_object()) {
        m_runs.push_back(
            {run.value("at", std::int64_t{0}), run.value("rollback", false),
             packagesFromJson(run.value("packages", nlohmann::json::array()))}
        );
      }
    }
    for (const auto& point : json->value("activity", nlohmann::json::array())) {
      if (point.is_object()) {
        m_activity.push_back(
            {point.value("n", 0), point.value("at", std::int64_t{0}), point.value("afterUpdate", false)}
        );
      }
    }
    m_lastUpdateAt = json->value("lastUpdateAt", std::int64_t{0});
    m_historyClearedAt = json->value("historyClearedAt", std::int64_t{0});
    if (!readText(dataPath())) {
      save();
    }
  }

  void Engine::save() const {
    nlohmann::json json = {
        {"runs", nlohmann::json::array()},
        {"activity", nlohmann::json::array()},
        {"lastUpdateAt", m_lastUpdateAt},
        {"historyClearedAt", m_historyClearedAt},
    };
    for (const auto& run : m_runs) {
      nlohmann::json packages = nlohmann::json::array();
      for (const auto& p : run.packages) {
        packages.push_back(packageToJson(p));
      }
      json["runs"].push_back({{"at", run.at}, {"rollback", run.rollback}, {"packages", std::move(packages)}});
    }
    for (const auto& point : m_activity) {
      json["activity"].push_back({{"n", point.count}, {"at", point.at}, {"afterUpdate", point.afterUpdate}});
    }
    std::ofstream(dataPath()) << json.dump(2);
  }

} // namespace okaru::updates
