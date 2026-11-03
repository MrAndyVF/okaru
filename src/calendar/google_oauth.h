#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class HttpClient;

namespace calendar {

  struct OAuthTokens {
    std::string refreshToken;
    std::string accessToken;
    std::chrono::system_clock::time_point expiry;
  };

  // OKARU: OAuth de Google con el cliente propio del usuario ([calendar] google_client_id /
  // google_client_secret), sin el intermediario de noctalia.dev. Flujo de aplicación de escritorio: el
  // navegador abre la página de Google con PKCE y un receptor en 127.0.0.1 recoge el código; el canje y el
  // refresco van directo a oauth2.googleapis.com.
  class GoogleOAuthBroker {
  public:
    struct StartResult {
      std::string sessionId;
      std::string pollToken;
      std::string authUrl;
      int expiresIn = 0;
      long httpStatus = 0;
    };
    enum class PollStatus { Pending, Complete, Expired, Error };
    struct ClientCredentials {
      std::string clientId;
      std::string clientSecret;
    };

    explicit GoogleOAuthBroker(HttpClient& http);
    ~GoogleOAuthBroker();

    void setCredentialsProvider(std::function<ClientCredentials()> provider) { m_credentials = std::move(provider); }

    // Begin a Connect flow. On success the caller opens authUrl in a browser and polls.
    void start(std::function<void(bool ok, StartResult)> cb);

    // One poll: Pending until the browser comes back to the loopback receiver; then the code is exchanged.
    void poll(const std::string& pollToken, std::function<void(PollStatus, OAuthTokens)> cb);

    // Exchange a refresh token for a fresh access token. invalidGrant=true means the refresh token
    // is revoked/expired and the account must reconnect.
    void refresh(const std::string& refreshToken, std::function<void(bool ok, bool invalidGrant, OAuthTokens)> cb);

    struct Session;

  private:
    HttpClient& m_http;
    std::function<ClientCredentials()> m_credentials;
    std::map<std::string, std::shared_ptr<Session>> m_sessions;
  };

  // Partes puras del flujo (con prueba).
  namespace google_oauth {
    inline constexpr std::string_view kScope = "https://www.googleapis.com/auth/calendar.readonly";
    [[nodiscard]] std::string authUrl(
        std::string_view clientId, std::string_view redirectUri, std::string_view challenge, std::string_view state
    );
    // Primera línea de la petición que el navegador hace al receptor: "GET /?state=..&code=.. HTTP/1.1".
    // Devuelve el código si el state coincide; nullopt si falta, no coincide o Google devolvió error.
    [[nodiscard]] std::optional<std::string> codeFromRequestLine(std::string_view requestLine, std::string_view state);
    [[nodiscard]] std::string formEncode(const std::map<std::string, std::string>& fields);
  } // namespace google_oauth

} // namespace calendar
