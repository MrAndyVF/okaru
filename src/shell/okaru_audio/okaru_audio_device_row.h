#pragma once

// OKARU: fila de un dispositivo de audio (salida o entrada): predeterminado, nombre, volumen y silencio.
// La usan los paneles okaru-audio / okaru-microphone y la página Sonido → Dispositivos de los ajustes.

#include "ui/controls/flex.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Button;
class Label;
class PipeWireService;
class RadioButton;
class Slider;
struct AudioNode;

class OkaruAudioDeviceRow : public Flex {
public:
  // `trailing` va a la derecha del nombre (los ajustes ponen ahí los botones de renombrar).
  OkaruAudioDeviceRow(
      PipeWireService* audio, const AudioNode& node, bool input, float scale, float sliderMax,
      std::unique_ptr<Node> trailing = nullptr
  );

  // Refleja el estado actual (los paneles lo llaman en cada actualización). No mueve el deslizador
  // mientras se arrastra.
  void sync(const AudioNode& node, float sliderMax);
  void setOnDefaultChosen(std::function<void(std::uint32_t)> callback) { m_onDefaultChosen = std::move(callback); }
  void setDefaultMark(bool isDefault);
  [[nodiscard]] bool dragging() const noexcept;
  [[nodiscard]] std::uint32_t deviceId() const noexcept { return m_id; }

private:
  void sendVolume(float value, bool force);
  void applyMuted(bool muted);

  PipeWireService* m_audio = nullptr;
  std::uint32_t m_id = 0;
  bool m_input = false;
  bool m_muted = false;
  RadioButton* m_radio = nullptr;
  Label* m_name = nullptr;
  Slider* m_slider = nullptr;
  Label* m_value = nullptr;
  Button* m_mute = nullptr;
  bool m_syncing = false;
  float m_pending = -1.0F;
  std::chrono::steady_clock::time_point m_lastSentAt;
  std::function<void(std::uint32_t)> m_onDefaultChosen;
};
