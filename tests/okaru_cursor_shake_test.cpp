// Prueba del cursor agrandado al agitar: el detector de agitación y la lectura de un archivo Xcursor.
#include "shell/okaru_cursor_shake/okaru_shake_detector.h"
#include "shell/okaru_cursor_shake/okaru_xcursor.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using okaru::cursor_shake::ShakeDetector;

namespace {

  // Muestras cada 33 ms (como el sondeo) de una función de posición; true si alguna detecta agitación.
  template <typename F> bool run(F position, int samples) {
    ShakeDetector detector;
    bool shook = false;
    for (int i = 0; i < samples; ++i) {
      const auto [x, y] = position(i);
      shook = detector.update(i * 33.0, x, y) || shook;
    }
    return shook;
  }

  void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
  }

} // namespace

int main() {
  // Zigzag horizontal rápido de 300 px: agitar.
  assert(run([](int i) { return std::pair{1000.0 + ((i % 4) < 2 ? 0.0 : 300.0), 500.0 + (i % 3)}; }, 30));
  // Zigzag vertical: también.
  assert(run([](int i) { return std::pair{800.0, 300.0 + ((i % 4) < 2 ? 0.0 : 280.0)}; }, 30));
  // Línea recta larga (cruzar la pantalla): no.
  assert(!run([](int i) { return std::pair{i * 60.0, 400.0}; }, 30));
  // Círculo amplio: mucho recorrido, pero sin ida y vuelta suficiente frente a la zona cubierta: no.
  assert(!run([](int i) { return std::pair{1000.0 + 300.0 * std::cos(i * 0.2), 500.0 + 300.0 * std::sin(i * 0.2)}; }, 30));
  // Temblor pequeño de la mano: no.
  assert(!run([](int i) { return std::pair{1000.0 + (i % 2) * 3.0, 500.0 + (i % 3)}; }, 60));
  // Zigzag lento (una ida por segundo): no entra en la ventana de 1 s.
  assert(!run([](int i) { return std::pair{1000.0 + ((i / 30) % 2) * 300.0, 500.0}; }, 120));

  // Xcursor mínimo con dos tamaños: se elige el mayor y se despremultiplica.
  std::vector<std::uint8_t> file{'X', 'c', 'u', 'r'};
  put32(file, 16);
  put32(file, 0x10000);
  put32(file, 2);
  const std::uint32_t small = 16 + 2 * 12;
  const std::uint32_t big = small + 36 + 4;
  put32(file, 0xfffd0002U); put32(file, 24); put32(file, small);
  put32(file, 0xfffd0002U); put32(file, 48); put32(file, big);
  for (const auto& [size, w, hot, pixels] : {std::tuple{24U, 1U, 0U, std::vector<std::uint32_t>{0xff000000U}},
                                              std::tuple{48U, 2U, 1U, std::vector<std::uint32_t>{0x80400000U, 0x00000000U}}}) {
    put32(file, 36); put32(file, 0xfffd0002U); put32(file, size); put32(file, 1);
    put32(file, w); put32(file, 1); put32(file, hot); put32(file, 0); put32(file, 0);
    for (auto p : pixels) {
      put32(file, p);
    }
  }
  const auto path = std::filesystem::temp_directory_path() / "okaru_cursor_shake_test.xcur";
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  const auto image = okaru::cursor_shake::loadXcursorFile(path);
  std::filesystem::remove(path);
  assert(image.has_value() && image->nominalSize == 48 && image->width == 2 && image->hotX == 1);
  // 0x80 alfa con rojo premultiplicado 0x40 → rojo ~0x80 sin premultiplicar.
  assert(image->rgba[3] == 0x80 && image->rgba[0] >= 0x7f && image->rgba[0] <= 0x81);
  assert(image->rgba[7] == 0);
  assert(!okaru::cursor_shake::loadXcursorFile("/no/existe").has_value());

  std::puts("okaru_cursor_shake: OK");
  return 0;
}
