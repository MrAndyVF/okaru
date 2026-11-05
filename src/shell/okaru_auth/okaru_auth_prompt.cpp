#include "shell/okaru_auth/okaru_auth_prompt.h"

#include "security/secure_buffer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sodium.h>
#include <sstream>

namespace okaru::auth {

  namespace {

    void wipe(std::string& secret) {
      if (!secret.empty()) {
        sodium_memzero(secret.data(), secret.size());
      }
      secret.clear();
    }

  } // namespace

  std::string AuthPromptService::open(AuthRequest request, Clock::time_point now) {
    if (!security::initializeSecurityPrimitives()) {
      return {};
    }
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, 33> hex{};
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    request.id = hex.data();

    std::erase_if(m_answered, [now](const Answered& a) { return now - a.at > kRetryWindow; });
    const auto previous = std::ranges::find(m_answered, request.requesterPid, &Answered::requesterPid);
    request.attempt = previous != m_answered.end() && request.requesterPid > 0 ? previous->attempt + 1 : 1;

    std::string id = request.id;
    m_entries.push_back(
        {.request = std::move(request), .state = AuthResultKind::Pending, .secret = {}, .touched = now}
    );
    changed();
    return id;
  }

  const AuthRequest* AuthPromptService::current() const {
    const auto it = std::ranges::find(m_entries, AuthResultKind::Pending, &Entry::state);
    return it == m_entries.end() ? nullptr : &it->request;
  }

  AuthPromptService::Entry* AuthPromptService::find(std::string_view id) {
    const auto it = std::ranges::find_if(m_entries, [id](const Entry& e) { return e.request.id == id; });
    return it == m_entries.end() ? nullptr : &*it;
  }

  void AuthPromptService::erase(std::string_view id) {
    for (auto& entry : m_entries) {
      if (entry.request.id == id) {
        wipe(entry.secret);
      }
    }
    std::erase_if(m_entries, [id](const Entry& e) { return e.request.id == id; });
  }

  void AuthPromptService::answer(std::string_view id, std::string secret) {
    Entry* entry = find(id);
    if (entry == nullptr || entry->state != AuthResultKind::Pending) {
      wipe(secret);
      return;
    }
    if (entry->request.onDone) {
      finish(*entry, {.kind = AuthResultKind::Answered, .secret = std::move(secret)});
      return;
    }
    entry->state = AuthResultKind::Answered;
    entry->secret = std::move(secret);
    changed();
  }

  void AuthPromptService::finish(Entry& entry, AuthResult result) {
    // Se quita antes de avisar: el aviso puede abrir la siguiente solicitud.
    auto onDone = std::move(entry.request.onDone);
    erase(std::string(entry.request.id));
    changed();
    onDone(std::move(result));
    wipe(result.secret);
  }

  void AuthPromptService::cancel(std::string_view id) {
    Entry* entry = find(id);
    if (entry == nullptr || entry->state != AuthResultKind::Pending) {
      return;
    }
    if (entry->request.onDone) {
      finish(*entry, {.kind = AuthResultKind::Cancelled, .secret = {}});
      return;
    }
    entry->state = AuthResultKind::Cancelled;
    changed();
  }

  AuthResult AuthPromptService::take(std::string_view id, int clientPid, Clock::time_point now) {
    Entry* entry = find(id);
    if (entry == nullptr || entry->request.clientPid != clientPid) {
      return {.kind = AuthResultKind::Unknown, .secret = {}};
    }
    entry->touched = now;
    if (entry->state == AuthResultKind::Pending) {
      return {.kind = AuthResultKind::Pending, .secret = {}};
    }
    AuthResult result{.kind = entry->state, .secret = {}};
    if (entry->state == AuthResultKind::Answered) {
      result.secret = std::move(entry->secret);
      std::erase_if(m_answered, [&](const Answered& a) { return a.requesterPid == entry->request.requesterPid; });
      m_answered.push_back({.requesterPid = entry->request.requesterPid, .attempt = entry->request.attempt, .at = now});
    }
    erase(id);
    return result;
  }

  bool AuthPromptService::expire(Clock::time_point now) {
    const auto before = m_entries.size();
    std::vector<std::string> dead;
    for (const auto& entry : m_entries) {
      if (!entry.request.onDone && now - entry.touched > kClientTimeout) {
        dead.push_back(entry.request.id);
      }
    }
    for (const auto& id : dead) {
      erase(id);
    }
    if (m_entries.size() != before) {
      changed();
      return true;
    }
    return false;
  }

  void AuthPromptService::changed() {
    if (m_onChanged) {
      m_onChanged();
    }
  }

  Requester resolveRequester(int pid, const std::function<std::optional<std::string>(int)>& readStat) {
    static constexpr std::array<std::string_view, 9> kIntermediaries{
        "okaru", "okaru-askpass", "sudo", "doas", "run0", "sh", "bash", "dash", "zsh",
    };
    Requester last{};
    for (int depth = 0; depth < 8 && pid > 1; ++depth) {
      const auto stat = readStat(pid);
      if (!stat.has_value()) {
        break;
      }
      // Formato: "<pid> (<comm>) <state> <ppid> ..."; comm puede tener espacios o paréntesis.
      const auto open = stat->find('(');
      const auto close = stat->rfind(')');
      if (open == std::string::npos || close == std::string::npos || close < open) {
        break;
      }
      const std::string name = stat->substr(open + 1, close - open - 1);
      last = {.name = name, .pid = pid};
      if (!std::ranges::contains(kIntermediaries, std::string_view(name))) {
        return last;
      }
      char state = 0;
      int ppid = 0;
      if (std::sscanf(stat->c_str() + close + 1, " %c %d", &state, &ppid) != 2) {
        break;
      }
      pid = ppid;
    }
    return last;
  }

  Requester resolveRequester(int pid) {
    return resolveRequester(pid, [](int p) -> std::optional<std::string> {
      std::ifstream file("/proc/" + std::to_string(p) + "/stat");
      if (!file) {
        return std::nullopt;
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      return buffer.str();
    });
  }

} // namespace okaru::auth
