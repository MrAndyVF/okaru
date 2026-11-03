// OKARU: opciones y argv de gpu-screen-recorder.
#include "capture/okaru_recorder.h"
#include "config/config_types.h"

#include <algorithm>
#include <cstdlib>
#include <print>

using namespace okaru::capture;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_recorder_test: {}", message);
    }
    return condition;
  }
  bool has(const std::vector<std::string>& args, std::string_view flag, std::string_view value) {
    const auto it = std::ranges::find(args, flag);
    return it != args.end() && it + 1 != args.end() && *(it + 1) == value;
  }
} // namespace

int main() {
  bool ok = true;

  WidgetConfig config;
  config.settings = {
      {"source", std::string("DP-2")}, {"resolution", std::string("1920x1080")}, {"frame_rate", std::int64_t{144}},
      {"format", std::string("mkv")},  {"quality", std::string("ultra")},        {"audio", std::string("both")},
      {"show_cursor", false},
  };
  const auto o = RecorderOptions::fromConfig(&config);
  ok = expect(o.source == "DP-2" && o.frameRate == 144 && o.format == "mkv", "lee la config") && ok;

  const auto args = buildRecordArgs(o, o.source, "", "/tmp/x.mkv");
  ok = expect(args.front() == "gpu-screen-recorder" && has(args, "-w", "DP-2"), "fuente") && ok;
  ok = expect(has(args, "-c", "mkv") && has(args, "-f", "144") && has(args, "-q", "ultra"), "formato, fps, calidad")
      && ok;
  ok = expect(has(args, "-a", "default_output|default_input") && has(args, "-s", "1920x1080"), "audio y resolución")
      && ok;
  ok = expect(has(args, "-cursor", "no") && has(args, "-o", "/tmp/x.mkv"), "cursor y salida") && ok;

  // Valores no válidos vuelven al predeterminado: nada raro llega al comando.
  WidgetConfig bad;
  bad.settings = {
      {"source", std::string("x; rm -rf ~")}, {"frame_rate", std::int64_t{7}}, {"format", std::string("exe")}
  };
  const auto safe = RecorderOptions::fromConfig(&bad);
  ok = expect(safe.source == "portal" && safe.frameRate == 60 && safe.format == "mp4", "valida la config") && ok;

  // Región con slurp, webm fuerza vp9, sin audio, resolución original.
  RecorderOptions region;
  region.format = "webm";
  region.audio = "none";
  const auto rargs = buildRecordArgs(region, "region", "800x600+10+20", "/tmp/r.webm");
  ok = expect(has(rargs, "-region", "800x600+10+20") && has(rargs, "-k", "vp9"), "región y webm") && ok;
  ok = expect(
           std::ranges::find(rargs, "-a") == rargs.end() && std::ranges::find(rargs, "-s") == rargs.end(), "sin audio"
       )
      && ok;

  const auto replay = buildReplayArgs(RecorderOptions{}, "portal", "", "/tmp/videos");
  ok = expect(has(replay, "-r", "30") && has(replay, "-o", "/tmp/videos"), "replay") && ok;

  RecorderOptions named;
  named.filenamePattern = "a/b_%Y";
  const auto name = outputFileName(named, std::chrono::system_clock::from_time_t(0));
  ok = expect(name.find('/') == std::string::npos && name.ends_with(".mp4"), "nombre sin barras y con extensión") && ok;
  ok = expect(expandHome("/abs") == "/abs", "ruta absoluta intacta") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
