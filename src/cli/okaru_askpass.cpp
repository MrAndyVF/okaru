#include "cli/okaru_askpass.h"

#include "ipc/ipc_client.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <print>
#include <sodium.h>
#include <string>
#include <thread>

namespace okaru::cli {

  namespace {
    constexpr auto kPollInterval = std::chrono::milliseconds(100);
    constexpr auto kMaxWait = std::chrono::minutes(5);

    void wipe(std::string& text) {
      if (!text.empty()) {
        sodium_memzero(text.data(), text.size());
      }
      text.clear();
    }
  } // namespace

  int runAskpass(int argc, char* argv[]) {
    std::string prompt;
    for (int i = 2; i < argc; ++i) {
      prompt += (i > 2 ? " " : "") + std::string(argv[i]);
    }
    const char* message = std::getenv("OKARU_AUTH_MESSAGE");
    const nlohmann::json request{{"prompt", prompt}, {"message", message != nullptr ? message : ""}};

    const auto opened = IpcClient::request("auth-prompt-open " + request.dump());
    if (!opened.has_value() || !opened->starts_with("ok ")) {
      std::println(stderr, "okaru askpass: {}", opened.has_value() ? *opened : "okaru is not running");
      return 1;
    }
    std::string id = opened->substr(3);
    while (!id.empty() && (id.back() == '\n' || id.back() == ' ')) {
      id.pop_back();
    }

    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    while (std::chrono::steady_clock::now() < deadline) {
      auto reply = IpcClient::request("auth-prompt-result " + id);
      if (!reply.has_value()) {
        return 1; // la shell se cerró
      }
      if (reply->starts_with("ok ")) {
        // La respuesta es "ok <secreto>\n": se imprime tal cual sin el prefijo.
        std::fwrite(reply->data() + 3, 1, reply->size() - 3, stdout);
        std::fflush(stdout);
        wipe(*reply);
        return 0;
      }
      if (!reply->starts_with("pending")) {
        return 1; // cancelada, expirada o desconocida
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    return 1;
  }

} // namespace okaru::cli
