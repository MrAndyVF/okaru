#include "capture/okaru_recorder.h"

#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace okaru::capture {

  namespace {

    template <typename T> bool contains(const std::vector<T>& list, const T& value) {
      return std::ranges::find(list, value) != list.end();
    }

    std::string
    pick(const WidgetConfig* config, const char* key, const std::vector<std::string>& choices, std::string fallback) {
      const std::string value = config != nullptr ? config->getString(key, fallback) : fallback;
      return contains(choices, value) ? value : fallback;
    }

    bool isOutputName(std::string_view value) {
      return !value.empty()
          && value.size() < 64
          && std::ranges::all_of(value, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; });
    }

    std::vector<std::string> commonArgs(const RecorderOptions& o, std::string_view target, std::string_view region) {
      std::vector<std::string> args{"gpu-screen-recorder", "-w", std::string(target)};
      if (target == "region" && !region.empty()) {
        args.insert(args.end(), {"-region", std::string(region)});
      }
      std::string codec = o.codec;
      if (o.format == "webm" && codec != "vp9" && codec != "av1") {
        codec = "vp9"; // webm solo admite vp8/vp9/av1
      }
      args.insert(args.end(), {"-c", o.format, "-f", std::to_string(o.frameRate), "-k", codec, "-q", o.quality});
      if (o.audio == "system") {
        args.insert(args.end(), {"-a", "default_output"});
      } else if (o.audio == "mic") {
        args.insert(args.end(), {"-a", "default_input"});
      } else if (o.audio == "both") {
        args.insert(args.end(), {"-a", "default_output|default_input"});
      }
      if (o.resolution != "original") {
        args.insert(args.end(), {"-s", o.resolution});
      }
      args.insert(args.end(), {"-cursor", o.showCursor ? "yes" : "no", "-v", "no"});
      return args;
    }

  } // namespace

  const std::vector<std::string>& resolutionChoices() {
    static const std::vector<std::string> k{"original", "3840x2160", "2560x1440", "1920x1080", "1280x720"};
    return k;
  }
  const std::vector<int>& frameRateChoices() {
    static const std::vector<int> k{30, 60, 120, 144};
    return k;
  }
  const std::vector<std::string>& formatChoices() {
    static const std::vector<std::string> k{"mp4", "mkv", "webm"};
    return k;
  }
  const std::vector<std::string>& qualityChoices() {
    static const std::vector<std::string> k{"medium", "high", "very_high", "ultra"};
    return k;
  }
  const std::vector<std::string>& audioChoices() {
    static const std::vector<std::string> k{"none", "system", "mic", "both"};
    return k;
  }

  RecorderOptions RecorderOptions::fromConfig(const WidgetConfig* config) {
    RecorderOptions o;
    if (config == nullptr) {
      return o;
    }
    const std::string source = config->getString("source", o.source);
    o.source = source == "portal" || source == "region" || isOutputName(source) ? source : o.source;
    o.resolution = pick(config, "resolution", resolutionChoices(), o.resolution);
    const int fps = static_cast<int>(config->getInt("frame_rate", o.frameRate));
    o.frameRate = contains(frameRateChoices(), fps) ? fps : o.frameRate;
    o.format = pick(config, "format", formatChoices(), o.format);
    o.quality = pick(config, "quality", qualityChoices(), o.quality);
    o.audio = pick(config, "audio", audioChoices(), o.audio);
    o.codec = pick(config, "video_codec", {"auto", "h264", "hevc", "av1", "vp9"}, o.codec);
    o.directory = config->getString("directory", o.directory);
    o.filenamePattern = config->getString("filename_pattern", o.filenamePattern);
    o.showCursor = config->getBool("show_cursor", o.showCursor);
    o.copyToClipboard = config->getBool("copy_to_clipboard", o.copyToClipboard);
    o.replaySeconds =
        static_cast<int>(std::clamp<std::int64_t>(config->getInt("replay_duration", o.replaySeconds), 5, 1800));
    return o;
  }

  std::string expandHome(std::string_view path) {
    if (path.starts_with("~/") || path == "~") {
      const char* home = std::getenv("HOME");
      return std::string(home != nullptr ? home : "") + std::string(path.substr(1));
    }
    return std::string(path);
  }

  std::string outputFileName(const RecorderOptions& options, std::chrono::system_clock::time_point now) {
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buffer[256];
    const std::size_t n = std::strftime(buffer, sizeof(buffer), options.filenamePattern.c_str(), &tm);
    std::string name = n > 0 ? std::string(buffer, n) : std::string("recording");
    std::ranges::replace(name, '/', '_'); // el patrón no puede salirse de la carpeta
    return name + "." + options.format;
  }

  std::vector<std::string> buildRecordArgs(
      const RecorderOptions& options, std::string_view target, std::string_view region, std::string_view outputPath
  ) {
    auto args = commonArgs(options, target, region);
    args.insert(args.end(), {"-o", std::string(outputPath)});
    return args;
  }

  std::vector<std::string> buildReplayArgs(
      const RecorderOptions& options, std::string_view target, std::string_view region, std::string_view outputDir
  ) {
    auto args = commonArgs(options, target, region);
    args.insert(args.end(), {"-r", std::to_string(options.replaySeconds), "-o", std::string(outputDir)});
    return args;
  }

  Recorder& Recorder::instance() {
    static Recorder recorder;
    return recorder;
  }

  bool Recorder::available() const { return process::commandExists("gpu-screen-recorder"); }

  std::chrono::seconds Recorder::elapsed() const {
    if (m_state == RecorderState::Idle) {
      return std::chrono::seconds(0);
    }
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_startedAt);
  }

  void Recorder::withTarget(const RecorderOptions& options, std::function<void(std::string, std::string)> run) {
    if (options.source != "region") {
      run(options.source, {});
      return;
    }
    // Región: se elige con slurp y luego se graba esa geometría.
    const bool started = process::runAsync(
        std::vector<std::string>{"slurp", "-f", "%wx%h+%x+%y"},
        process::RunCallbacks{
            .stdOut = nullptr,
            .stdErr = nullptr,
            .onExit = [run](process::RunResult result) {
              std::string geometry = result.out;
              while (!geometry.empty() && std::isspace(static_cast<unsigned char>(geometry.back())) != 0) {
                geometry.pop_back();
              }
              if (!result || geometry.empty()) {
                return; // el usuario canceló la selección
              }
              DeferredCall::callLater([run, geometry] { run("region", geometry); });
            },
        }
    );
    if (!started) {
      notify::error("OKARU", i18n::tr("okaru.capture.record-failed"), i18n::tr("okaru.capture.slurp-missing"));
    }
  }

  void Recorder::launch(std::vector<std::string> args, RecorderState state, std::string savedPath, bool copy) {
    m_lastError.clear();
    const bool started = process::runAsync(
        std::move(args),
        process::RunCallbacks{
            .stdOut = [](std::string_view) {},
            .stdErr = nullptr,
            .onExit =
                [this, state, savedPath, copy](process::RunResult result) {
                  DeferredCall::callLater([this, state, savedPath, copy, ok = result.exitCode == 0, err = result.err] {
                    if (m_state != state) {
                      return;
                    }
                    setState(RecorderState::Idle);
                    const bool saved = !savedPath.empty() && std::filesystem::exists(savedPath);
                    if (state == RecorderState::Recording && saved) {
                      notify::info("OKARU", i18n::tr("okaru.capture.saved"), savedPath);
                      if (copy) {
                        (void)process::runAsync(
                            {"sh", "-c", "printf 'file://%s' \"$1\" | wl-copy -t text/uri-list", "sh",
                             savedPath.c_str()}
                        );
                      }
                    } else if (!ok && !saved) {
                      m_lastError = err.substr(0, 400);
                      notify::error("OKARU", i18n::tr("okaru.capture.record-failed"), m_lastError);
                    }
                  });
                },
        }
    );
    if (!started) {
      notify::error("OKARU", i18n::tr("okaru.capture.record-failed"), "gpu-screen-recorder");
      return;
    }
    m_startedAt = std::chrono::steady_clock::now();
    setState(state);
  }

  void Recorder::startRecording(const RecorderOptions& options) {
    if (m_state != RecorderState::Idle || !available()) {
      return;
    }
    withTarget(options, [this, options](std::string target, std::string region) {
      const std::string dir = expandHome(options.directory);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::string path = dir + "/" + outputFileName(options, std::chrono::system_clock::now());
      launch(buildRecordArgs(options, target, region, path), RecorderState::Recording, path, options.copyToClipboard);
    });
  }

  void Recorder::startReplay(const RecorderOptions& options) {
    if (m_state != RecorderState::Idle || !available()) {
      return;
    }
    withTarget(options, [this, options](std::string target, std::string region) {
      const std::string dir = expandHome(options.directory);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      m_replayDir = dir;
      launch(buildReplayArgs(options, target, region, dir), RecorderState::Replay, {}, false);
    });
  }

  void Recorder::stop() {
    if (m_state == RecorderState::Idle) {
      return;
    }
    // gpu-screen-recorder guarda el archivo al recibir SIGINT.
    (void)process::runAsync({"pkill", "-INT", "-f", "^([^ ]*/)?gpu-screen-recorder( |$)"});
  }

  void Recorder::saveReplay() {
    if (m_state == RecorderState::Replay) {
      (void)process::runAsync({"pkill", "-USR1", "-f", "^([^ ]*/)?gpu-screen-recorder .*-r "});
      notify::info("OKARU", i18n::tr("okaru.capture.replay-saved"), m_replayDir);
    }
  }

  int Recorder::addListener(std::function<void()> listener) {
    const int id = m_nextListener++;
    m_listeners.emplace_back(id, std::move(listener));
    return id;
  }

  void Recorder::removeListener(int id) {
    std::erase_if(m_listeners, [id](const auto& entry) { return entry.first == id; });
  }

  void Recorder::setState(RecorderState state) {
    m_state = state;
    const auto listeners = m_listeners; // un listener puede quitarse a sí mismo
    for (const auto& [id, listener] : listeners) {
      listener();
    }
  }

} // namespace okaru::capture
