#pragma once

// OKARU: grabación de pantalla con gpu-screen-recorder, parte de la shell (reemplaza al plugin
// okaru/screen_recorder). Las opciones viven en [widget.capture]; el widget Captura y su menú
// la controlan. Detener = SIGINT (gpu-screen-recorder guarda el archivo); replay: SIGUSR1 guarda.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct WidgetConfig;

namespace okaru::capture {

  struct RecorderOptions {
    std::string source = "portal";       // portal | region | <salida, p. ej. DP-2>
    std::string resolution = "original"; // original | 3840x2160 | 2560x1440 | 1920x1080 | 1280x720
    int frameRate = 60;
    std::string format = "mp4";        // mp4 | mkv | webm
    std::string quality = "very_high"; // medium | high | very_high | ultra
    std::string audio = "system";      // none | system | mic | both
    std::string codec = "auto";        // auto | h264 | hevc | av1 | vp9
    std::string directory = "~/Videos/Recordings";
    std::string filenamePattern = "recording_%Y%m%d_%H%M%S";
    bool showCursor = true;
    bool copyToClipboard = false;
    int replaySeconds = 30;

    [[nodiscard]] static RecorderOptions fromConfig(const WidgetConfig* config);
  };

  enum class RecorderState : std::uint8_t { Idle, Recording, Replay };

  // Valores que se pueden elegir desde el menú (y que se validan antes de llegar al comando).
  [[nodiscard]] const std::vector<std::string>& resolutionChoices();
  [[nodiscard]] const std::vector<int>& frameRateChoices();
  [[nodiscard]] const std::vector<std::string>& formatChoices();
  [[nodiscard]] const std::vector<std::string>& qualityChoices();
  [[nodiscard]] const std::vector<std::string>& audioChoices();

  // "~/x" -> "$HOME/x"; strftime del patrón; extensión según el formato.
  [[nodiscard]] std::string expandHome(std::string_view path);
  [[nodiscard]] std::string outputFileName(const RecorderOptions& options, std::chrono::system_clock::time_point now);

  // argv de gpu-screen-recorder. `target` = "portal", "region" o una salida; `region` = "WxH+X+Y" de slurp.
  [[nodiscard]] std::vector<std::string> buildRecordArgs(
      const RecorderOptions& options, std::string_view target, std::string_view region, std::string_view outputPath
  );
  // Replay: guarda los últimos `replaySeconds` en `outputDir` cada vez que recibe SIGUSR1.
  [[nodiscard]] std::vector<std::string> buildReplayArgs(
      const RecorderOptions& options, std::string_view target, std::string_view region, std::string_view outputDir
  );

  class Recorder {
  public:
    static Recorder& instance();

    [[nodiscard]] bool available() const;
    [[nodiscard]] RecorderState state() const noexcept { return m_state; }
    [[nodiscard]] std::chrono::seconds elapsed() const;
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    void startRecording(const RecorderOptions& options);
    void startReplay(const RecorderOptions& options);
    void stop();
    void saveReplay();

    // Se llama en el hilo principal cada vez que cambia el estado. Devuelve un id para quitarlo.
    int addListener(std::function<void()> listener);
    void removeListener(int id);

  private:
    Recorder() = default;
    void launch(std::vector<std::string> args, RecorderState state, std::string savedPath, bool copy);
    void withTarget(const RecorderOptions& options, std::function<void(std::string target, std::string region)> run);
    void setState(RecorderState state);

    RecorderState m_state = RecorderState::Idle;
    std::string m_replayDir; // carpeta del replay activo
    std::chrono::steady_clock::time_point m_startedAt;
    std::string m_lastError;
    std::vector<std::pair<int, std::function<void()>>> m_listeners;
    int m_nextListener = 1;
  };

} // namespace okaru::capture
