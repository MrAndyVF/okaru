#pragma once

// OKARU: widget Captura. Un solo icono para capturas y grabación (reemplaza al widget de captura, al
// plugin de grabación y a su grupo). Clic: menú flotante Captura | Grabación. Clic central: detener.
// Mientras graba muestra el tiempo y dos botones: pausar/reanudar y terminar.

#include "shell/bar/widget.h"

#include <cstdint>
#include <string>

class Glyph;
class InputArea;
class Label;
class Timer;
struct wl_output;

class OkaruCaptureWidget : public Widget {
public:
  // Las opciones las lee el grabador desde [widget.capture]; aquí solo existen para el esquema y el editor.
  struct Options {
    std::string source = "portal";
    std::string resolution = "original";
    std::int64_t frameRate = 60;
    std::string format = "mp4";
    std::string quality = "very_high";
    std::string audio = "system";
    std::string videoCodec = "auto";
    std::string directory = "~/Videos/Recordings";
    std::string filenamePattern = "recording_%Y%m%d_%H%M%S";
    bool showCursor = true;
    bool copyToClipboard = false;
    std::int64_t replayDuration = 30;
  };

  OkaruCaptureWidget(wl_output* output, Options options);
  ~OkaruCaptureWidget() override;

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void syncState();

  Glyph* m_glyph = nullptr;
  Label* m_time = nullptr;
  InputArea* m_pauseArea = nullptr;
  Glyph* m_pauseGlyph = nullptr;
  InputArea* m_stopArea = nullptr;
  Glyph* m_stopGlyph = nullptr;
  bool m_isVertical = false;
  int m_listener = 0;
  std::unique_ptr<Timer> m_tick; // refresca el tiempo mientras graba
};
