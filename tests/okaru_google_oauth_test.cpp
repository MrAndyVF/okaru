// OKARU: partes puras del OAuth propio de Google (URL de autorización, código del receptor, formulario).
#include "calendar/google_oauth.h"

#include <cstdlib>
#include <print>

using namespace calendar::google_oauth;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_google_oauth_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  const std::string url = authUrl("id.apps.googleusercontent.com", "http://127.0.0.1:5000", "CH", "ST");
  ok &= expect(url.starts_with("https://accounts.google.com/o/oauth2/v2/auth?"), "host de Google");
  ok &= expect(url.contains("redirect_uri=http%3A%2F%2F127.0.0.1%3A5000"), "redirect codificado");
  ok &= expect(url.contains("code_challenge=CH") && url.contains("state=ST") && url.contains("access_type=offline"),
               "PKCE, state y offline");
  ok &= expect(url.contains("calendar.readonly"), "solo lectura");

  ok &= expect(codeFromRequestLine("GET /?state=ST&code=4%2F0Ab HTTP/1.1", "ST") == "4/0Ab", "código decodificado");
  ok &= expect(!codeFromRequestLine("GET /?state=OTRO&code=x HTTP/1.1", "ST").has_value(), "state distinto");
  ok &= expect(!codeFromRequestLine("GET /?error=access_denied&state=ST HTTP/1.1", "ST").has_value(), "error");
  ok &= expect(!codeFromRequestLine("GET /favicon.ico HTTP/1.1", "ST").has_value(), "sin query");
  ok &= expect(!codeFromRequestLine("POST /?state=ST&code=x HTTP/1.1", "ST").has_value(), "solo GET");

  ok &= expect(formEncode({{"a", "1 2"}, {"b", "x&y"}}) == "a=1%202&b=x%26y", "formulario");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
