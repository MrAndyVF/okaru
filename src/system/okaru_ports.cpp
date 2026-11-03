#include "system/okaru_ports.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <regex>
#include <sstream>

namespace okaru::ports {

  namespace {
    constexpr Logger kLog("ports");

    bool isLoopback(std::string_view address) { return address.starts_with("127.") || address == "::1"; }

    std::pair<std::string, const char*> scale(double bytesPerSecond) {
      static constexpr const char* kUnits[] = {"", "K", "M", "G", "T"};
      double value = std::max(bytesPerSecond, 0.0);
      std::size_t unit = 0;
      while (value >= 999.5 && unit + 1 < std::size(kUnits)) { // pasar de unidad antes de redondear a "1000K"
        value /= 1000.0;
        ++unit;
      }
      return {unit > 0 && value < 9.95 ? std::format("{:.1f}", value) : std::format("{:.0f}", value), kUnits[unit]};
    }
  } // namespace

  std::vector<Listener> parse(std::string_view ssOutput, const Filters& filters) {
    static const std::regex kOwner(R"re(\("([^"]*)",pid=(\d+))re");
    std::vector<Listener> result;
    std::map<std::string, std::size_t> byKey;
    std::istringstream lines{std::string(ssOutput)};
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream fields(line);
      std::vector<std::string> parts;
      for (std::string part; fields >> part && parts.size() < 6;) {
        parts.push_back(part);
      }
      if (parts.size() < 6 || (parts[0] != "tcp" && parts[0] != "udp")) {
        continue;
      }
      const std::string& proto = parts[0];
      const auto colon = parts[4].rfind(':');
      int port = 0;
      if (colon == std::string::npos
          || std::from_chars(parts[4].data() + colon + 1, parts[4].data() + parts[4].size(), port).ec != std::errc{}) {
        continue;
      }
      if ((proto == "udp" && !filters.includeUdp) || (filters.hideSystemPorts && port < 1024)) {
        continue;
      }
      std::string address = parts[4].substr(0, colon);
      if (const auto pct = address.find('%'); pct != std::string::npos) {
        address.erase(pct);
      }
      if (address.starts_with('[')) {
        address.erase(0, 1);
      }
      if (address.ends_with(']')) {
        address.pop_back();
      }

      // Un socket compartido tras fork lista a todos sus dueños; cada uno es su propia fila.
      std::vector<std::pair<std::string, int>> owners;
      for (auto it = std::sregex_iterator(line.begin(), line.end(), kOwner); it != std::sregex_iterator(); ++it) {
        owners.emplace_back((*it)[1].str(), std::stoi((*it)[2].str()));
      }
      if (owners.empty()) {
        owners.emplace_back("", 0);
      }
      for (const auto& [process, pid] : owners) {
        if (filters.onlyOwnProcesses && pid == 0) {
          continue;
        }
        const std::string key = std::format("{}:{}:{}", proto, port, pid);
        auto [it, inserted] = byKey.try_emplace(key, result.size());
        if (inserted) {
          result.push_back({.key = key, .port = port, .proto = proto, .pid = pid, .process = process});
        }
        Listener& listener = result[it->second];
        if (std::ranges::find(listener.addresses, address) == listener.addresses.end()) {
          listener.addresses.push_back(address);
          listener.localOnly = listener.localOnly && isLoopback(address);
        }
      }
    }
    std::ranges::stable_sort(result, [](const Listener& a, const Listener& b) {
      if ((a.pid == 0) != (b.pid == 0)) {
        return a.pid != 0;
      }
      return a.port < b.port;
    });
    return result;
  }

  std::vector<std::string> stopCommand(const Listener& listener) {
    if (listener.pid > 0) {
      return {"kill", std::to_string(listener.pid)};
    }
    return {"pkexec", "fuser", "-k", "-TERM", std::format("{}/{}", listener.port, listener.proto)};
  }

  std::string compactRate(double bytesPerSecond) {
    const auto [value, unit] = scale(bytesPerSecond);
    return value + unit;
  }

  std::string fullRate(double bytesPerSecond) {
    const auto [value, unit] = scale(bytesPerSecond);
    return std::format("{} {}B/s", value, unit);
  }

  Scanner& Scanner::instance() {
    static Scanner scanner;
    return scanner;
  }

  int Scanner::addListener(std::function<void()> callback) {
    const int id = m_nextId++;
    m_callbacks.emplace_back(id, std::move(callback));
    if (m_callbacks.size() == 1) {
      scan();
    }
    return id;
  }

  void Scanner::removeListener(int id) {
    std::erase_if(m_callbacks, [id](const auto& entry) { return entry.first == id; });
    if (m_callbacks.empty()) {
      m_timer.stop();
    }
  }

  void Scanner::configure(const Filters& filters, int intervalSeconds) {
    intervalSeconds = std::clamp(intervalSeconds, 1, 30);
    if (filters == m_filters && intervalSeconds == m_intervalSeconds) {
      return;
    }
    m_filters = filters;
    m_intervalSeconds = intervalSeconds;
    m_listeners = parse(m_raw, m_filters);
    for (const auto& [id, callback] : m_callbacks) {
      callback();
    }
    schedule();
  }

  void Scanner::rescan(int delayMs) {
    if (delayMs <= 0) {
      scan();
      return;
    }
    m_timer.start(std::chrono::milliseconds(delayMs), [this] { scan(); });
  }

  void Scanner::schedule() {
    if (m_callbacks.empty()) {
      return;
    }
    m_timer.start(std::chrono::seconds(m_intervalSeconds), [this] { scan(); });
  }

  void Scanner::scan() {
    if (m_pending || m_callbacks.empty()) {
      return;
    }
    process::RunCallbacks callbacks;
    callbacks.onExit = [this](process::RunResult result) {
      DeferredCall::callLater([this, result = std::move(result)] {
        m_pending = false;
        m_failed = !result;
        if (m_failed) {
          kLog.warn("ss falló ({}): {}", result.exitCode, result.err);
        } else {
          m_raw = result.out;
        }
        auto next = parse(m_raw, m_filters);
        const bool changed = next.size() != m_listeners.size()
            || !std::ranges::equal(next, m_listeners, [](const Listener& a, const Listener& b) {
                 return a.key == b.key && a.addresses == b.addresses;
               });
        m_listeners = std::move(next);
        if (changed || m_failed) {
          for (const auto& [id, callback] : m_callbacks) {
            callback();
          }
        }
        schedule();
      });
    };
    m_pending = process::runAsync({"ss", "-tulnpH"}, std::move(callbacks), {.timeout = std::chrono::seconds(5)});
    if (!m_pending) {
      schedule();
    }
  }

} // namespace okaru::ports
