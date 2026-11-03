#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>

class AnimationManager;

class MotionService {
public:
  static MotionService& instance();

  MotionService(const MotionService&) = delete;
  MotionService& operator=(const MotionService&) = delete;

  void registerManager(AnimationManager* manager);
  void unregisterManager(AnimationManager* manager);

  void setEnabled(bool enabled);
  void setSpeed(float speed);

  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] float speed() const noexcept { return m_speed; }

  // OKARU: curva del estilo de animación elegido (t 0..1 -> progreso). Si está definida, reemplaza
  // las curvas suaves de las animaciones de la interfaz; las lineales no cambian.
  void setEasingOverride(std::function<float(float)> easing) { m_easingOverride = std::move(easing); }
  [[nodiscard]] const std::function<float(float)>& easingOverride() const noexcept { return m_easingOverride; }

private:
  MotionService() = default;

  bool m_enabled = true;
  float m_speed = 1.0F;
  std::function<float(float)> m_easingOverride;
  std::unordered_set<AnimationManager*> m_managers;
};
