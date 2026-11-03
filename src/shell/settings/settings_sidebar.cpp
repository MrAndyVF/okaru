#include "shell/settings/settings_sidebar.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/settings/okaru_settings_categories.h"
#include "shell/settings/settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {

  namespace {

    constexpr float kSidebarWidth = 200.0F;
    constexpr float kSidebarPadding = 6.0F;
    constexpr float kSidebarGap = 2.0F;
    constexpr float kPrimaryNavGlyphSize = 18.0F;
    constexpr float kPrimaryNavGap = 6.0F;
    constexpr float kPrimaryNavPaddingH = 10.0F;

    void addNavButton(RovingListNavHost& nav, std::unique_ptr<Button> button, std::function<void()> onClick) {
      Button* raw = button.get();
      nav.registerItem(raw, std::move(onClick));
      nav.addChild(std::move(button));
    }

    // OKARU: botón de módulo: icono y, a su derecha, el nombre con su frase debajo, todo dentro del mismo
    // recuadro de selección (como en dux-shell).
    std::unique_ptr<Button> makeModuleNavButton(
        std::string_view glyph, std::string title, std::string hint, float scale, bool selected, std::function<void()> onClick
    ) {
      auto button = ui::button({
          .glyph = std::string(glyph),
          .glyphSize = kPrimaryNavGlyphSize * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = selected ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = kPrimaryNavPaddingH * scale,
          .gap = kPrimaryNavGap * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [](Button& b) { b.setTabStop(false); },
      });
      auto text = ui::column({.align = FlexAlign::Start, .gap = 1.0F * scale, .flexGrow = 1.0F});
      text->addChild(ui::label({
          .text = std::move(title),
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurface),
      }));
      text->addChild(ui::label({
          .text = std::move(hint),
          .fontSize = Style::fontSizeCaption * 0.9F * scale,
          .color = colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .opacity = selected ? 0.85F : 1.0F,
      }));
      button->addChild(std::move(text));
      return button;
    }

  } // namespace

  std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx) {

    auto* scroll = &ctx.contentScrollState;
    auto* selectedSection = &ctx.selectedSection;
    auto* selectedBarName = &ctx.selectedBarName;

    const auto clearTransientState = std::move(ctx.clearTransientState);
    const auto clearSearchQuery = std::move(ctx.clearSearchQuery);
    const auto requestRebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;
    const bool searchActive = ctx.globalSearchActive;
    const bool showActiveTab = !searchActive;

    auto sidebarScroll = ui::scrollView({
        .state = &ctx.sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fill = ctx.config.shell.settingsWindowTranslucent ? clearColorSpec() : colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusXl(scale),
        .minWidth = kSidebarWidth * scale,
        .fillHeight = true,
        .width = kSidebarWidth * scale,
        .height = 0.0F,
        .configure = [](ScrollView& scrollView) { scrollView.clearBorder(); },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .scrollIntoView = std::move(ctx.scrollSidebarNodeIntoView),
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("settings.sidebar");
    sidebarNav->setGap(kSidebarGap * scale);
    sidebarNav->setPadding(kSidebarPadding * scale);
    RovingListNavHost* nav = sidebarNav.get();

    // OKARU: un botón por módulo (con su frase debajo, como en dux-shell) y separadores entre bloques.
    // Las secciones de cada módulo se eligen con el selector horizontal de la página.
    int lastBlock = -1;
    for (const auto& group : groupSettingsModules(ctx.sections)) {
      const auto& module = *group.module;
      if (lastBlock >= 0 && module.block != lastBlock) {
        sidebarNav->addChild(ui::column({
            .fill = colorSpecFromRole(ColorRole::OutlineVariant),
            .minHeight = 1.0F,
            .fillWidth = true,
            .height = 1.0F,
        }));
      }
      lastBlock = module.block;
      bool selected = false;
      for (const auto section : group.sections) {
        selected = selected || settingsSectionId(section) == *selectedSection;
      }
      selected = showActiveTab && selected;
      std::string target = lastModuleSection(module.key);
      const bool targetVisible =
          std::ranges::any_of(group.sections, [&](SettingsSection s) { return settingsSectionId(s) == target; });
      if (!targetVisible) {
        target = std::string(settingsSectionId(group.sections.front()));
      }
      const std::string barName = !ctx.availableBars.empty() ? ctx.availableBars.front() : std::string("default");
      const auto onClick = [selectedSection, selectedBarName, scroll, target, barName,
                            searchActive, clearTransientState, clearSearchQuery, requestRebuild] {
        if (searchActive || *selectedSection != target) {
          scroll->offset = 0.0F;
        }
        *selectedSection = target;
        if (target == "bar") {
          *selectedBarName = barName;
        }
        clearSearchQuery();
        clearTransientState();
        requestRebuild();
      };
      addNavButton(
          *nav,
          makeModuleNavButton(
              module.glyph, i18n::tr(settingsModuleLabelKey(module)), i18n::tr(settingsModuleHintKey(module)), scale,
              selected, onClick
          ),
          onClick
      );
    }

    auto* sidebar = sidebarScroll->content();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(sidebarNav));

    if (ctx.outNav != nullptr) {
      *ctx.outNav = nav;
    }

    return sidebarScroll;
  }

} // namespace settings
