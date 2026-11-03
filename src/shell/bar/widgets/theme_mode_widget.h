#pragma once

#include "shell/bar/widget.h"

class Glyph;
class InputArea;

namespace okaru::theme {
  class ThemeService;
}

class ThemeModeWidget : public Widget {
public:
  explicit ThemeModeWidget(okaru::theme::ThemeService* themeService);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  okaru::theme::ThemeService* m_themeService = nullptr;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  bool m_lastIsLight = false;
};
