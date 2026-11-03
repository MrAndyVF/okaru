#include "calendar/google_oauth.h"

#include "core/log.h"
#include "net/http_client.h"
#include "util/string_utils.h"

#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sodium.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-oauth");
    constexpr const char* kTokenUrl = "https://oauth2.googleapis.com/token";
    constexpr int kSessionSeconds = 600;

    std::string randomUrlSafe(std::size_t bytes) {
      std::string raw(bytes, '\0');
      randombytes_buf(raw.data(), raw.size());
      std::string out(sodium_base64_ENCODED_LEN(bytes, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
      sodium_bin2base64(
          out.data(), out.size(), reinterpret_cast<const unsigned char*>(raw.data()), raw.size(),
          sodium_base64_VARIANT_URLSAFE_NO_PADDING
      );
      out.resize(std::strlen(out.c_str()));
      return out;
    }

    // PKCE S256: base64url(sha256(verifier)) sin relleno.
    std::string pkceChallenge(const std::string& verifier) {
      std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
      crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size());
      std::string out(sodium_base64_ENCODED_LEN(hash.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
      sodium_bin2base64(out.data(), out.size(), hash.data(), hash.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
      out.resize(std::strlen(out.c_str()));
      return out;
    }

    OAuthTokens tokensFrom(const nlohmann::json& j) {
      OAuthTokens tokens;
      tokens.accessToken = j.at("access_token").get<std::string>();
      tokens.refreshToken = j.value("refresh_token", std::string{});
      tokens.expiry = std::chrono::system_clock::now() + std::chrono::seconds{j.value("expires_in", 3600)};
      return tokens;
    }
  } // namespace

  // Estado compartido entre el hilo receptor y el bucle principal.
  struct GoogleOAuthBroker::Session {
    std::mutex mutex;
    std::optional<std::string> code;
    bool failed = false;
    bool expired = false;
    bool exchanging = false;
    std::string verifier;
    std::string redirectUri;
  };

  namespace google_oauth {

    std::string authUrl(
        std::string_view clientId, std::string_view redirectUri, std::string_view challenge, std::string_view state
    ) {
      return "https://accounts.google.com/o/oauth2/v2/auth?response_type=code&access_type=offline&prompt=consent"
             "&code_challenge_method=S256&client_id="
          + StringUtils::urlEncode(clientId) + "&redirect_uri=" + StringUtils::urlEncode(redirectUri)
          + "&scope=" + StringUtils::urlEncode(kScope) + "&code_challenge=" + std::string(challenge)
          + "&state=" + std::string(state);
    }

    std::optional<std::string> codeFromRequestLine(std::string_view line, std::string_view state) {
      const auto q = line.find('?');
      const auto end = line.find(' ', q == std::string_view::npos ? 0 : q);
      if (!line.starts_with("GET ") || q == std::string_view::npos || end == std::string_view::npos) {
        return std::nullopt;
      }
      std::string_view query = line.substr(q + 1, end - q - 1);
      std::optional<std::string> code;
      bool stateOk = false;
      while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        const std::string_view key = pair.substr(0, eq);
        const std::string value = eq == std::string_view::npos ? std::string{} : StringUtils::urlDecode(pair.substr(eq + 1));
        if (key == "code" && !value.empty()) {
          code = value;
        } else if (key == "state") {
          stateOk = value == state;
        } else if (key == "error") {
          return std::nullopt;
        }
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
      }
      return stateOk ? code : std::nullopt;
    }

    std::string formEncode(const std::map<std::string, std::string>& fields) {
      std::string out;
      for (const auto& [key, value] : fields) {
        if (!out.empty()) {
          out += '&';
        }
        out += StringUtils::urlEncode(key) + "=" + StringUtils::urlEncode(value);
      }
      return out;
    }

  } // namespace google_oauth

  GoogleOAuthBroker::GoogleOAuthBroker(HttpClient& http) : m_http(http) {}

  GoogleOAuthBroker::~GoogleOAuthBroker() = default;

  void GoogleOAuthBroker::start(std::function<void(bool, StartResult)> cb) {
    const ClientCredentials creds = m_credentials ? m_credentials() : ClientCredentials{};
    if (creds.clientId.empty() || creds.clientSecret.empty()) {
      kLog.warn("google oauth: falta [calendar] google_client_id / google_client_secret");
      cb(false, StartResult{});
      return;
    }
    // Receptor de un solo uso en un puerto libre de 127.0.0.1.
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (listener < 0
        || ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || ::listen(listener, 1) != 0
        || ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      kLog.warn("google oauth: no se pudo abrir el receptor local: {}", std::strerror(errno));
      if (listener >= 0) {
        ::close(listener);
      }
      cb(false, StartResult{});
      return;
    }
    auto session = std::make_shared<Session>();
    session->verifier = randomUrlSafe(48);
    session->redirectUri = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    const std::string state = randomUrlSafe(16);
    const std::string token = randomUrlSafe(16);
    m_sessions[token] = session;

    std::thread([listener, session, state] {
      pollfd pfd{listener, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, kSessionSeconds * 1000);
      if (ready <= 0) {
        const std::lock_guard lock(session->mutex);
        session->expired = true;
        ::close(listener);
        return;
      }
      const int client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      ::close(listener);
      std::string request(4096, '\0');
      const ssize_t n = client >= 0 ? ::recv(client, request.data(), request.size(), 0) : -1;
      request.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
      const auto code = google_oauth::codeFromRequestLine(request.substr(0, request.find("\r\n")), state);
      const std::string body = code.has_value() ? "<h2>OKARU: cuenta conectada. Ya puedes cerrar esta pesta&ntilde;a.</h2>"
                                                : "<h2>OKARU: no se pudo conectar la cuenta.</h2>";
      const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                                "Content-Length: "
          + std::to_string(body.size()) + "\r\n\r\n" + body;
      if (client >= 0) {
        (void)::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        ::close(client);
      }
      const std::lock_guard lock(session->mutex);
      session->code = code;
      session->failed = !code.has_value();
    }).detach();

    StartResult result;
    result.sessionId = token;
    result.pollToken = token;
    result.authUrl = google_oauth::authUrl(creds.clientId, session->redirectUri, pkceChallenge(session->verifier), state);
    result.expiresIn = kSessionSeconds;
    result.httpStatus = 200;
    cb(true, std::move(result));
  }

  void GoogleOAuthBroker::poll(const std::string& pollToken, std::function<void(PollStatus, OAuthTokens)> cb) {
    const auto it = m_sessions.find(pollToken);
    if (it == m_sessions.end()) {
      cb(PollStatus::Expired, {});
      return;
    }
    const auto session = it->second;
    std::string code;
    {
      const std::lock_guard lock(session->mutex);
      if (session->expired || session->failed) {
        const bool expired = session->expired;
        m_sessions.erase(it);
        cb(expired ? PollStatus::Expired : PollStatus::Error, {});
        return;
      }
      if (!session->code.has_value() || session->exchanging) {
        cb(PollStatus::Pending, {});
        return;
      }
      session->exchanging = true;
      code = *session->code;
    }
    m_sessions.erase(it);
    const ClientCredentials creds = m_credentials ? m_credentials() : ClientCredentials{};
    HttpRequest req;
    req.method = "POST";
    req.url = kTokenUrl;
    req.headers = {"Content-Type: application/x-www-form-urlencoded"};
    req.body = google_oauth::formEncode({
        {"grant_type", "authorization_code"},
        {"code", code},
        {"code_verifier", session->verifier},
        {"redirect_uri", session->redirectUri},
        {"client_id", creds.clientId},
        {"client_secret", creds.clientSecret},
    });
    m_http.request(std::move(req), [cb = std::move(cb)](const HttpResponse& resp) {
      if (!resp.transportOk || resp.status != 200) {
        kLog.warn("google oauth: canje del código falló http={}", resp.status);
        cb(PollStatus::Error, {});
        return;
      }
      try {
        cb(PollStatus::Complete, tokensFrom(nlohmann::json::parse(resp.body)));
      } catch (const std::exception& e) {
        kLog.warn("google oauth: respuesta del canje inválida: {}", e.what());
        cb(PollStatus::Error, {});
      }
    });
  }

  void GoogleOAuthBroker::refresh(const std::string& refreshToken, std::function<void(bool, bool, OAuthTokens)> cb) {
    const ClientCredentials creds = m_credentials ? m_credentials() : ClientCredentials{};
    if (creds.clientId.empty() || creds.clientSecret.empty()) {
      cb(false, false, {});
      return;
    }
    HttpRequest req;
    req.method = "POST";
    req.url = kTokenUrl;
    req.headers = {"Content-Type: application/x-www-form-urlencoded"};
    req.body = google_oauth::formEncode({
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshToken},
        {"client_id", creds.clientId},
        {"client_secret", creds.clientSecret},
    });
    m_http.request(std::move(req), [cb = std::move(cb)](const HttpResponse& resp) {
      if (resp.transportOk && resp.status == 400) {
        // Solo invalid_grant obliga a reconectar; un cliente mal configurado no borra la cuenta.
        bool invalidGrant = false;
        try {
          invalidGrant = nlohmann::json::parse(resp.body).value("error", std::string{}) == "invalid_grant";
        } catch (const std::exception&) {
        }
        cb(false, invalidGrant, {});
        return;
      }
      if (!resp.transportOk || resp.status != 200) {
        kLog.warn("google oauth: refresco falló http={}", resp.status);
        cb(false, false, {});
        return;
      }
      try {
        cb(true, false, tokensFrom(nlohmann::json::parse(resp.body)));
      } catch (const std::exception& e) {
        kLog.warn("google oauth: respuesta del refresco inválida: {}", e.what());
        cb(false, false, {});
      }
    });
  }

} // namespace calendar
