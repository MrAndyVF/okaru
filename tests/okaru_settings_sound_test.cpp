// OKARU: los nombres personalizados de audio se recortan y nunca parten un carácter UTF-8.
#include "shell/settings/okaru_settings_sound.h"

#include <cstdlib>
#include <print>
#include <string>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_settings_sound_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  using settings::normalizeAudioDeviceAlias;
  bool ok = true;
  ok &= expect(normalizeAudioDeviceAlias("  Audífonos \n") == "Audífonos", "recorta espacios");
  ok &= expect(normalizeAudioDeviceAlias("   ").empty(), "solo espacios = vacío");
  ok &= expect(normalizeAudioDeviceAlias(std::string(80, 'a')).size() == 64, "máximo 64 bytes");
  // 63 'a' + 'í' (2 bytes) = 65 bytes: el corte no puede dejar medio 'í'.
  ok &= expect(normalizeAudioDeviceAlias(std::string(63, 'a') + "í") == std::string(63, 'a'), "no parte UTF-8");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
