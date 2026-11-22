#include "shell/launcher/okaru_launcher_panel.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h" // XKB_KEY_*
#include "i18n/i18n.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "shell/launcher/okaru_launcher_commands.h"
#include "shell/launcher/okaru_launcher_keybinds.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/virtual_list_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace {

  using i18n::tr;

  constexpr std::string_view kApplicationsProviderId = "Applications";
  constexpr std::size_t kMaxSuggestions = 5;
  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;
  constexpr float kRowHeight = 46.0F;
  constexpr float kHeaderHeight = 38.0F;
  constexpr float kIconSize = 28.0F;

  // El uso empuja lo frecuente hacia arriba; al escribir, solo desempata coincidencias parecidas.
  double usageBoost(double score, int usageCount, bool typedQuery) {
    if (usageCount <= 0) {
      return 0.0;
    }
    const double boost = static_cast<double>(usageCount) * kUsageScorePerCount;
    if (!typedQuery) {
      return boost;
    }
    return FuzzyMatch::isMatch(score) ? std::min(boost, kTypedUsageScoreCap) : 0.0;
  }

} // namespace

namespace okaru::launcher {

  // Tecla dibujada como tecla (pie y vista de atajos).
  std::unique_ptr<Node> keycap(std::string text, float scale) {
    auto cap = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .paddingV = 3.0F * scale,
        .paddingH = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::OnSurface, 0.08F),
        .radius = Style::scaledRadiusSm(scale),
        .minWidth = 26.0F * scale,
        .minHeight = 24.0F * scale,
    });
    cap->addChild(ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    }));
    return cap;
  }

  // Una fila de la lista: título de sección o resultado (icono, título, subtítulo tenue y tipo a la derecha).
  class LauncherRow final : public Node {
  public:
    LauncherRow(float scale, AsyncTextureCache* cache) : m_scale(scale), m_cache(cache) {
      auto row = ui::row({
          .out = &m_row,
          .align = FlexAlign::Center,
          .gap = Style::spaceMd * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
      });
      row->addChild(ui::image({.out = &m_icon, .width = kIconSize * scale, .height = kIconSize * scale}));
      row->addChild(ui::glyph({.out = &m_glyph, .glyphSize = kIconSize * 0.8F * scale, .visible = false}));
      row->addChild(ui::label({
          .out = &m_title,
          .fontSize = (Style::fontSizeBody + 1.0F) * scale,
          .fontWeight = FontWeight::SemiBold,
          .maxLines = 1,
      }));
      row->addChild(ui::label({
          .out = &m_subtitle,
          .fontSize = Style::fontSizeBody * scale,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      }));
      row->addChild(ui::label({.out = &m_kind, .fontSize = Style::fontSizeBody * scale, .maxLines = 1}));
      row->addChild(ui::row({.out = &m_keys, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false}));
      addChild(std::move(row));
      // Título de sección: abajo de su franja, con aire arriba (separa secciones) y poco abajo (va con sus filas).
      auto header = ui::column({
          .out = &m_headerBox,
          .align = FlexAlign::Start,
          .justify = FlexJustify::End,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .visible = false,
      });
      header->addChild(ui::label({
          .out = &m_header,
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }));
      addChild(std::move(header));
    }

    void bindHeader(const std::string& label, float width, float height) {
      setSize(width, height);
      m_row->setVisible(false);
      m_headerBox->setVisible(true);
      m_headerBox->setFrameSize(width, height);
      m_header->setText(label);
    }

    void bindKeybind(const OkaruLauncherPanel::Row& data, float width, float height, bool selected, bool hovered) {
      setSize(width, height);
      m_headerBox->setVisible(false);
      m_row->setVisible(true);
      m_row->setFrameSize(width, height);
      paintBackground(selected, hovered);
      for (Node* node : {static_cast<Node*>(m_icon), static_cast<Node*>(m_glyph), static_cast<Node*>(m_kind)}) {
        node->setVisible(false);
        node->setParticipatesInLayout(false);
      }
      m_title->setText(data.label);
      m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_subtitle->setText("");
      if (data.keys != m_boundKeys) {
        m_boundKeys = data.keys;
        while (!m_keys->children().empty()) {
          (void)m_keys->removeChild(m_keys->children().back().get());
        }
        for (const auto& key : data.keys) {
          m_keys->addChild(keycap(key, m_scale));
        }
      }
      m_keys->setVisible(true);
      m_keys->setParticipatesInLayout(true);
    }

    void bindResult(
        Renderer& renderer, const LauncherResult& result, const std::string& kind, float width, float height,
        bool selected, bool hovered
    ) {
      setSize(width, height);
      m_headerBox->setVisible(false);
      m_row->setVisible(true);
      m_row->setFrameSize(width, height);
      paintBackground(selected, hovered);
      m_keys->setVisible(false);
      m_keys->setParticipatesInLayout(false);
      m_kind->setParticipatesInLayout(true);
      m_title->setText(result.title);
      m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_subtitle->setText(result.subtitle);
      m_subtitle->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_kind->setText(kind);
      m_kind->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_kind->setVisible(!kind.empty());

      const int size = static_cast<int>(std::round(kIconSize * m_scale));
      bool ready = false;
      if (!result.iconPath.empty()) {
        ready = m_cache != nullptr ? m_icon->setSourceFileAsync(renderer, *m_cache, result.iconPath, size, true)
                                   : m_icon->setSourceFile(renderer, result.iconPath, size, true);
      } else {
        m_icon->clear(renderer);
      }
      m_iconPath = result.iconPath;
      m_icon->setVisible(ready);
      m_icon->setParticipatesInLayout(ready || !result.iconPath.empty());
      const bool useGlyph = !ready && result.iconPath.empty();
      m_glyph->setGlyph(result.glyphName.empty() ? "app-window" : result.glyphName);
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_glyph->setVisible(useGlyph);
      m_glyph->setParticipatesInLayout(useGlyph);
      if (!ready && !result.iconPath.empty()) {
        // Icono aún cargando en la caché: se muestra cuando llega.
        m_icon->setAsyncReadyCallback([this] { m_icon->setVisible(m_icon->hasImage()); });
      }
    }

  private:
    void paintBackground(bool selected, bool hovered) {
      if (selected) {
        m_row->setFill(colorSpecFromRole(ColorRole::OnSurface, 0.10F));
      } else if (hovered) {
        m_row->setFill(colorSpecFromRole(ColorRole::OnSurface, 0.05F));
      } else {
        m_row->clearFill();
      }
    }

    float m_scale = 1.0F;
    AsyncTextureCache* m_cache = nullptr;
    std::string m_iconPath;
    Flex* m_keys = nullptr;
    std::vector<std::string> m_boundKeys;
    Flex* m_row = nullptr;
    Image* m_icon = nullptr;
    Glyph* m_glyph = nullptr;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    Label* m_kind = nullptr;
    Label* m_header = nullptr;
    Flex* m_headerBox = nullptr;
  };

  class RowsAdapter final : public VirtualListAdapter {
  public:
    using ActivateFn = std::function<void(std::size_t)>;
    using KindFn = std::function<std::string(const LauncherResult&)>;

    RowsAdapter(float scale, AsyncTextureCache* cache, ActivateFn onActivate, KindFn kindOf)
        : m_scale(scale), m_cache(cache), m_onActivate(std::move(onActivate)), m_kindOf(std::move(kindOf)) {}

    void set(const std::vector<OkaruLauncherPanel::Row>* rows, const std::vector<LauncherResult>* results) {
      m_rows = rows;
      m_results = results;
      ++m_revision;
    }
    void setSelected(std::optional<std::size_t> row) {
      m_selected = row;
      ++m_revision;
    }

    [[nodiscard]] std::size_t itemCount() const override { return m_rows != nullptr ? m_rows->size() : 0; }
    [[nodiscard]] std::uint64_t itemRevision(std::size_t /*index*/) const override { return m_revision; }
    [[nodiscard]] bool itemInteractive(std::size_t index) const override { return !isHeader(index); }

    [[nodiscard]] float measureItem(Renderer& /*renderer*/, std::size_t index, float /*width*/) override {
      return (isHeader(index) ? kHeaderHeight : kRowHeight) * m_scale;
    }

    [[nodiscard]] std::unique_ptr<Node> createItem() override { return std::make_unique<LauncherRow>(m_scale, m_cache); }

    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override {
      if (m_rows == nullptr || index >= m_rows->size()) {
        return;
      }
      auto& row = static_cast<LauncherRow&>(item);
      const auto& data = (*m_rows)[index];
      if (data.kind == OkaruLauncherPanel::Row::Kind::Header) {
        row.bindHeader(data.label, width, kHeaderHeight * m_scale);
        return;
      }
      if (data.kind == OkaruLauncherPanel::Row::Kind::Keybind) {
        row.bindKeybind(data, width, kRowHeight * m_scale, m_selected == index, hovered);
        return;
      }
      const LauncherResult& result = (*m_results)[data.result];
      row.bindResult(renderer, result, m_kindOf(result), width, kRowHeight * m_scale, m_selected == index, hovered);
    }

    void onActivate(std::size_t index) override { m_onActivate(index); }

  private:
    [[nodiscard]] bool isHeader(std::size_t index) const {
      return m_rows != nullptr && index < m_rows->size()
          && (*m_rows)[index].kind == OkaruLauncherPanel::Row::Kind::Header;
    }

    float m_scale = 1.0F;
    AsyncTextureCache* m_cache = nullptr;
    ActivateFn m_onActivate;
    KindFn m_kindOf;
    const std::vector<OkaruLauncherPanel::Row>* m_rows = nullptr;
    const std::vector<LauncherResult>* m_results = nullptr;
    std::optional<std::size_t> m_selected;
    std::uint64_t m_revision = 0;
  };

} // namespace okaru::launcher

using okaru::launcher::keycap;

OkaruLauncherPanel::OkaruLauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures)
    : m_config(config), m_asyncTextures(asyncTextures), m_iconResolver(true) {
  addProvider(std::make_unique<OkaruLauncherCommands>([this] { showKeybinds(false); }));
}

OkaruLauncherPanel::~OkaruLauncherPanel() {
  if (m_list != nullptr) {
    m_list->setAdapter(nullptr);
  }
}

void OkaruLauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  provider->initialize();
  m_providers.push_back(std::move(provider));
}

void OkaruLauncherPanel::clearProvidersWithIdPrefix(std::string_view prefix) {
  std::erase_if(m_providers, [&](const auto& provider) { return provider->id().starts_with(prefix); });
}

void OkaruLauncherPanel::setScopedProvider(std::string_view providerId, std::string_view placeholder) {
  m_scopedProviderId = providerId;
  m_scopedPlaceholder = placeholder;
}

void OkaruLauncherPanel::schedulePrewarm() {
  m_prewarmTimer.start(std::chrono::seconds(2), [this] {
    if (m_input != nullptr) {
      return; // ya está abierto: la apertura hizo el trabajo
    }
    const int iconSize = static_cast<int>(std::round(kIconSize * contentScale()));
    for (auto& provider : m_providers) {
      if (provider->id() != kApplicationsProviderId) {
        continue;
      }
      for (const auto& result : provider->query("")) {
        if (!result.iconName.empty()) {
          (void)m_iconResolver.resolve(result.iconName, iconSize);
        }
      }
    }
  });
}

void OkaruLauncherPanel::clearUsage() {
  m_usage.clear();
  if (m_input != nullptr) {
    onQueryChanged(m_query);
  }
}

bool OkaruLauncherPanel::spotlight() const noexcept {
  return m_config != nullptr && m_config->config().shell.launcher.style == ShellConfig::LauncherStyle::Spotlight;
}

bool OkaruLauncherPanel::collapsed() const {
  return spotlight() && m_mode == Mode::Search && m_scopedProviderId.empty() && StringUtils::trim(m_query).empty();
}

void OkaruLauncherPanel::syncCollapsed() {
  const bool hide = collapsed();
  for (Node* node : m_expandable) {
    node->setVisible(!hide);
    node->setParticipatesInLayout(!hide);
  }
}

float OkaruLauncherPanel::measureContentHeight(Renderer& /*renderer*/, float /*contentWidth*/) {
  // Plegado: el alto de la barra; desplegado: todo lo que permita el panel (el gestor lo limita a preferredHeight).
  if (collapsed() && m_header != nullptr) {
    return m_header->height();
  }
  return preferredHeight();
}

bool OkaruLauncherPanel::trackUsage() const {
  return m_config == nullptr || m_config->config().shell.launcher.trackUsage;
}

LauncherProvider* OkaruLauncherPanel::providerById(std::string_view id) const {
  const auto it = std::ranges::find_if(m_providers, [&](const auto& provider) { return provider->id() == id; });
  return it != m_providers.end() ? it->get() : nullptr;
}

float OkaruLauncherPanel::preferredWidth() const {
  const int width = m_config != nullptr ? m_config->config().shell.launcher.width : 760;
  const int clamped = std::clamp(width, 480, 1400);
  return scaled(static_cast<float>(spotlight() ? std::min(clamped, 640) : clamped));
}

float OkaruLauncherPanel::preferredHeight() const { return scaled(520.0F); }

InputArea* OkaruLauncherPanel::initialFocusArea() const {
  return m_input != nullptr ? m_input->inputArea() : nullptr;
}

std::unique_ptr<Node> OkaruLauncherPanel::buildFooter(float scale) {
  auto footer = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingV = Style::spaceMd * scale,
      .paddingH = Style::spaceLg * scale,
      .fillWidth = true,
  });
  const ColorSpec muted = colorSpecFromRole(ColorRole::OnSurfaceVariant);
  footer->addChild(ui::label({.out = &m_footerTitle, .text = "Okaru Launcher", .fontSize = Style::fontSizeBody * scale, .color = muted}));
  footer->addChild(ui::spacer());
  const auto hint = [&](std::string text) {
    return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale, .color = muted});
  };
  // Teclas de la búsqueda.
  auto search = ui::row({.out = &m_searchHints, .align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  search->addChild(hint(tr("launcher.okaru.open")));
  search->addChild(keycap("↵", scale));
  if (m_scopedProviderId.empty()) {
    search->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule, .height = 18.0F * scale}));
    search->addChild(hint(tr("launcher.okaru.new-window")));
    search->addChild(keycap("⇧", scale));
    search->addChild(keycap("↵", scale));
  }
  footer->addChild(std::move(search));
  // Teclas de la vista de atajos.
  auto keybinds = ui::row({.out = &m_keybindHints, .align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  keybinds->addChild(hint(tr("launcher.okaru.back")));
  keybinds->addChild(keycap("Esc", scale));
  footer->addChild(std::move(keybinds));
  return footer;
}

void OkaruLauncherPanel::syncChrome() {
  const bool keybinds = m_mode == Mode::Keybinds;
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        keybinds ? tr("launcher.okaru.keybinds.placeholder")
                 : (m_scopedPlaceholder.empty() ? tr("launcher.okaru.placeholder") : m_scopedPlaceholder)
    );
  }
  if (m_footerTitle != nullptr) {
    m_footerTitle->setText(keybinds ? tr("launcher.okaru.keybinds.title") : std::string("Okaru Launcher"));
  }
  if (m_searchHints != nullptr && m_keybindHints != nullptr) {
    m_searchHints->setVisible(!keybinds);
    m_searchHints->setParticipatesInLayout(!keybinds);
    m_keybindHints->setVisible(keybinds);
    m_keybindHints->setParticipatesInLayout(keybinds);
    // Abierta directo en atajos: Esc cierra, no hay a dónde volver.
    m_keybindHints->setVisible(keybinds && !m_keybindsDirect);
  }
}

void OkaruLauncherPanel::showKeybinds(bool direct) {
  m_mode = Mode::Keybinds;
  m_keybindsDirect = direct;
  // Se leen al entrar: así reflejan lo que el usuario acaba de cambiar en Ajustes o a mano.
  m_keybinds.clear();
  if (compositors::isHyprland()) {
    for (auto& item : okaru::launcher::curatedKeybinds(okaru::hypr::readAllBinds())) {
      KeybindEntry entry{
          .section = tr("launcher.okaru.keybinds.group." + item.group),
          .description =
              item.group == "custom" ? item.description : tr("launcher.okaru.keybinds.item." + item.description),
          .keys = std::move(item.keys),
      };
      std::string keys;
      for (const auto& key : entry.keys) {
        keys += ' ' + key;
      }
      entry.search = StringUtils::toLower(entry.description + ' ' + entry.section + keys);
      m_keybinds.push_back(std::move(entry));
    }
  }
  if (m_input != nullptr) {
    m_input->setValue("");
  }
  m_query.clear();
  syncChrome();
  rebuildKeybindRows();
}

void OkaruLauncherPanel::showSearch() {
  m_mode = Mode::Search;
  m_keybindsDirect = false;
  if (m_input != nullptr) {
    m_input->setValue("");
  }
  syncChrome();
  onQueryChanged("");
}

void OkaruLauncherPanel::rebuildKeybindRows() {
  syncCollapsed();
  m_rows.clear();
  const std::string query = StringUtils::toLower(StringUtils::trim(m_query));
  std::string lastSection;
  bool any = false;
  for (const auto& entry : m_keybinds) {
    if (!query.empty() && !entry.search.contains(query)) {
      continue;
    }
    if (!any || entry.section != lastSection) {
      m_rows.push_back({.kind = Row::Kind::Header, .label = entry.section});
      lastSection = entry.section;
      any = true;
    }
    m_rows.push_back({.kind = Row::Kind::Keybind, .label = entry.description, .keys = entry.keys});
  }
  if (m_adapter != nullptr) {
    m_adapter->set(&m_rows, &m_results);
  }
  if (m_empty != nullptr) {
    m_empty->setVisible(m_rows.empty());
  }
  const auto first = std::ranges::find(m_rows, Row::Kind::Keybind, &Row::kind);
  select(first != m_rows.end() ? std::optional<std::size_t>(first - m_rows.begin()) : std::nullopt, false);
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
    m_list->scrollView().setScrollOffset(0.0F);
  }
  PanelManager::instance().requestLayout();
}

void OkaruLauncherPanel::create() {
  const float scale = contentScale();
  m_adapter = std::make_unique<okaru::launcher::RowsAdapter>(
      scale, m_asyncTextures, [this](std::size_t row) { activate(row, false); },
      [this](const LauncherResult& result) {
        if (!m_scopedProviderId.empty()) {
          return std::string{};
        }
        return result.providerId == kApplicationsProviderId ? tr("launcher.okaru.kind.application")
                                                            : tr("launcher.okaru.kind.command");
      }
  );

  auto container = ui::column({.out = &m_container, .align = FlexAlign::Stretch, .gap = 0.0F});

  // Buscador grande con el logo.
  // Spotlight: barra más baja (y el panel más estrecho, ver preferredWidth).
  const bool compact = spotlight();
  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .paddingV = (compact ? Style::spaceXs : Style::spaceMd) * scale,
      .paddingH = (compact ? Style::spaceMd : Style::spaceLg) * scale,
  });
  header->addChild(ui::label({
      .text = "郁",
      .fontSize = Style::fontSizeTitle * (compact ? 1.1F : 1.3F) * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  header->addChild(ui::input({
      .out = &m_input,
      .fontSize = (compact ? Style::fontSizeBody + 2.0F : Style::fontSizeTitle) * scale,
      .controlHeight = (compact ? Style::controlHeight : Style::controlHeightLg) * scale,
      .frameVisible = false,
      .flexGrow = 1.0F,
      .onChange = [this](const std::string& text) { onQueryChanged(text); },
  }));
  m_header = container->addChild(std::move(header));
  m_expandable.clear();
  m_expandable.push_back(container->addChild(ui::separator({})));

  auto body = ui::column({
      .align = FlexAlign::Stretch,
      .paddingV = Style::spaceXs * scale,
      .paddingH = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  body->addChild(ui::virtualListView({
      .out = &m_list,
      .contentScale = scale,
      .itemGap = 0.0F,
      .overscanItems = 4,
      .adapter = m_adapter.get(),
      .flexGrow = 1.0F,
  }));
  body->addChild(ui::label({
      .out = &m_empty,
      .text = tr("launcher.okaru.no-results"),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .visible = false,
  }));
  m_expandable.push_back(container->addChild(std::move(body)));
  m_expandable.push_back(container->addChild(ui::separator({})));
  m_expandable.push_back(container->addChild(buildFooter(scale)));

  setRoot(std::move(container));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void OkaruLauncherPanel::onOpen(std::string_view context) {
  for (auto& provider : m_providers) {
    provider->onLauncherOpened();
  }
  // Apps instaladas desde la última vez (stat barato; solo vuelve a leer si algo cambió).
  refreshDesktopEntriesIfSourcesChanged();
  if (m_list != nullptr) {
    m_list->scrollView().setScrollOffset(0.0F);
  }
  if (context == ":keybinds" && m_scopedProviderId.empty()) {
    showKeybinds(true);
    return;
  }
  m_mode = Mode::Search;
  m_keybindsDirect = false;
  syncChrome();
  if (m_input != nullptr) {
    m_input->setValue(std::string(context));
  }
  onQueryChanged(std::string(context));
}

void OkaruLauncherPanel::onClose() {
  // dmenu: cerrar sin elegir es cancelar (el script recibe la salida 1) y el lanzador vuelve a la búsqueda normal.
  // reset() puede quitar el proveedor de m_providers (dmenu_ipc lo difiere), así que se recorre una copia.
  std::vector<LauncherProvider*> providers;
  for (auto& provider : m_providers) {
    providers.push_back(provider.get());
  }
  for (LauncherProvider* provider : providers) {
    provider->reset();
  }
  m_scopedProviderId.clear();
  m_scopedPlaceholder.clear();
  if (m_asyncTextures != nullptr) {
    m_asyncTextures->trimUnused(0);
  }
  if (m_list != nullptr) {
    m_list->setAdapter(nullptr);
  }
  m_container = nullptr;
  m_input = nullptr;
  m_list = nullptr;
  m_empty = nullptr;
  m_results.clear();
  m_rows.clear();
  m_selected.reset();
  m_mode = Mode::Search;
  m_keybinds.clear();
  m_footerTitle = nullptr;
  m_header = nullptr;
  m_expandable.clear();
  m_searchHints = nullptr;
  m_keybindHints = nullptr;
  clearReleasedRoot();
}

void OkaruLauncherPanel::onIconThemeChanged() {
  m_iconResolver.invalidateMissingCache();
  if (m_input != nullptr) {
    onQueryChanged(m_query);
  }
}

void OkaruLauncherPanel::onQueryChanged(const std::string& text) {
  m_query = text;
  if (m_mode == Mode::Keybinds) {
    rebuildKeybindRows();
    return;
  }
  m_results.clear();
  const bool typed = !StringUtils::trim(text).empty();
  const bool usage = trackUsage();
  for (auto& provider : m_providers) {
    const bool scoped = !m_scopedProviderId.empty();
    if (scoped ? provider->id() != m_scopedProviderId : provider->id().starts_with("dmenu.")) {
      continue;
    }
    auto results = provider->query(text);
    for (auto& result : results) {
      result.providerId = provider->id();
      if (usage && provider->trackUsage()) {
        result.score += usageBoost(result.score, m_usage.getCount(provider->id(), result.id), typed);
      }
    }
    m_results.insert(m_results.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end()));
  }
  if (typed) {
    std::ranges::stable_sort(m_results, std::ranges::greater{}, &LauncherResult::score);
  }
  const int iconSize = static_cast<int>(std::round(kIconSize * contentScale()));
  for (auto& result : m_results) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconSize);
      result.iconPath = !resolved.empty() ? resolved : m_iconResolver.resolve("application-x-executable", iconSize);
      result.iconName.clear();
    }
  }
  rebuildRows();
}

void OkaruLauncherPanel::rebuildRows() {
  syncCollapsed();
  m_rows.clear();
  const bool typed = !StringUtils::trim(m_query).empty();
  const auto section = [this](std::string label, const std::vector<std::size_t>& indices) {
    if (indices.empty()) {
      return;
    }
    if (!label.empty()) {
      m_rows.push_back({.kind = Row::Kind::Header, .label = std::move(label)});
    }
    for (const std::size_t index : indices) {
      m_rows.push_back({.kind = Row::Kind::Result, .result = index});
    }
  };

  if (!m_scopedProviderId.empty() || typed) {
    std::vector<std::size_t> all(m_results.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
      all[i] = i;
    }
    section(m_scopedProviderId.empty() ? tr("launcher.okaru.section.results") : std::string{}, all);
  } else {
    // Sin escribir: lo más usado arriba y luego todas las apps en orden alfabético.
    std::vector<std::pair<int, std::size_t>> used;
    if (trackUsage()) {
      for (std::size_t i = 0; i < m_results.size(); ++i) {
        const int count = m_usage.getCount(m_results[i].providerId, m_results[i].id);
        if (count > 0 && m_results[i].providerId == kApplicationsProviderId) {
          used.emplace_back(count, i);
        }
      }
      std::ranges::stable_sort(used, std::ranges::greater{}, &std::pair<int, std::size_t>::first);
    }
    std::vector<std::size_t> suggestions;
    for (std::size_t i = 0; i < std::min(used.size(), kMaxSuggestions); ++i) {
      suggestions.push_back(used[i].second);
    }
    std::vector<std::size_t> commands;
    std::vector<std::size_t> apps;
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      (m_results[i].providerId == kApplicationsProviderId ? apps : commands).push_back(i);
    }
    section(tr("launcher.okaru.section.suggestions"), suggestions);
    section(tr("launcher.okaru.section.commands"), commands);
    section(tr("launcher.okaru.section.applications"), apps);
  }

  if (m_adapter != nullptr) {
    m_adapter->set(&m_rows, &m_results);
  }
  if (m_empty != nullptr) {
    m_empty->setVisible(m_rows.empty());
  }
  const auto first = std::ranges::find(m_rows, Row::Kind::Result, &Row::kind);
  select(first != m_rows.end() ? std::optional<std::size_t>(first - m_rows.begin()) : std::nullopt, false);
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
    m_list->scrollView().setScrollOffset(0.0F);
  }
  PanelManager::instance().requestLayout();
}

void OkaruLauncherPanel::select(std::optional<std::size_t> row, bool scroll) {
  m_selected = row;
  if (m_adapter != nullptr) {
    m_adapter->setSelected(row);
  }
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
    if (scroll && row.has_value()) {
      m_list->scrollToIndex(*row);
    }
  }
  PanelManager::instance().requestRedraw();
}

void OkaruLauncherPanel::moveSelection(int delta) {
  if (m_rows.empty()) {
    return;
  }
  std::size_t index = m_selected.value_or(0);
  const auto step = [&](std::size_t from) -> std::optional<std::size_t> {
    std::size_t i = from;
    while (true) {
      if (delta < 0 ? i == 0 : i + 1 >= m_rows.size()) {
        return std::nullopt;
      }
      i = delta < 0 ? i - 1 : i + 1;
      if (m_rows[i].kind != Row::Kind::Header) {
        return i;
      }
    }
  };
  for (int n = std::abs(delta); n > 0; --n) {
    const auto next = step(index);
    if (!next.has_value()) {
      break;
    }
    index = *next;
  }
  select(index);
}

void OkaruLauncherPanel::activate(std::size_t row, bool newInstance) {
  if (row >= m_rows.size() || m_rows[row].kind != Row::Kind::Result) {
    return;
  }
  LauncherResult result = m_results[m_rows[row].result];
  LauncherProvider* provider = providerById(result.providerId);
  if (provider == nullptr) {
    return;
  }
  result.newInstance = newInstance;
  if (!provider->activate(result)) {
    return;
  }
  finishActivation(*provider, result.id, provider->supportsAutoPaste(), !result.desktopEntryPath.empty());
}

void OkaruLauncherPanel::finishActivation(
    LauncherProvider& provider, const std::string& resultId, bool copied, bool appLaunch
) {
  if (trackUsage() && provider.trackUsage()) {
    m_usage.record(provider.id(), resultId);
  }
  PanelManager::instance().closePanel(false);
  if (copied && provider.supportsAutoPaste() && m_onCopiedActivation) {
    m_onCopiedActivation();
  }
  if (appLaunch && m_onAppLaunch) {
    m_onAppLaunch();
  }
}

bool OkaruLauncherPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }
  const bool ctrl = (modifiers & KeyMod::Ctrl) != 0;
  const bool shift = (modifiers & KeyMod::Shift) != 0;
  if (sym == XKB_KEY_Escape && m_mode == Mode::Keybinds && !m_keybindsDirect) {
    showSearch();
    return true;
  }
  if (m_mode == Mode::Keybinds && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
    return true;
  }
  switch (sym) {
  case XKB_KEY_Up:
    moveSelection(-1);
    return true;
  case XKB_KEY_Down:
    moveSelection(1);
    return true;
  case XKB_KEY_Page_Up:
    moveSelection(-8);
    return true;
  case XKB_KEY_Page_Down:
    moveSelection(8);
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (m_selected.has_value()) {
      activate(*m_selected, shift);
    }
    return true;
  default:
    break;
  }
  if (ctrl && (sym == XKB_KEY_n || sym == XKB_KEY_j)) {
    moveSelection(1);
    return true;
  }
  if (ctrl && (sym == XKB_KEY_p || sym == XKB_KEY_k)) {
    moveSelection(-1);
    return true;
  }
  return false;
}

void OkaruLauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr) {
    return;
  }
  m_container->setSize(width, height);
  m_container->layout(renderer);
}
