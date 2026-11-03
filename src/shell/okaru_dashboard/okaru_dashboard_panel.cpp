#include "shell/okaru_dashboard/okaru_dashboard_panel.h"

#include "config/config_service.h"
#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "scripting/plugin_id.h"
#include "render/scene/input_area.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <linux/input-event-codes.h>

using namespace okaru::dashboard;

namespace {

  constexpr const char* kPanelId = "control-center";
  constexpr float kCell = 112.0F;      // ancho de una columna (sin escalar)
  constexpr float kCardHeight = 104.0F; // todas las filas miden lo mismo

  std::string tr(const std::string& key) { return i18n::tr("okaru.dashboard." + key); }

  std::unique_ptr<Button>
  smallButton(float scale, std::string text, std::string glyph, ButtonVariant variant, std::function<void()> onClick) {
    return ui::button({
        .text = text.empty() ? std::nullopt : std::optional<std::string>(std::move(text)),
        .glyph = glyph.empty() ? std::nullopt : std::optional<std::string>(std::move(glyph)),
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = glyph.empty() ? std::nullopt : std::optional<float>(Style::fontSizeBody * scale),
        .variant = variant,
        .minWidth = Style::controlHeightSm * scale,
        .minHeight = Style::controlHeightSm * scale,
        .padding = Style::spaceXs * scale,
        .onClick = std::move(onClick),
    });
  }

} // namespace

OkaruDashboardPanel::OkaruDashboardPanel(CardDeps deps) : m_deps(deps) {
  setHostedDetailOpener([this](std::string_view id) {
    auto* manager = PanelManager::current();
    if (manager == nullptr || !manager->isOpenPanel(kPanelId) || !m_details.contains(std::string(id))) {
      return false;
    }
    requestDetail(std::string(id));
    return true;
  });
}

OkaruDashboardPanel::~OkaruDashboardPanel() { setHostedDetailOpener(nullptr); }

void OkaruDashboardPanel::addDetail(
    std::string id, std::string titleKey, std::string glyph, std::unique_ptr<Tab> tab, float height
) {
  m_details[std::move(id)] = Detail{std::move(titleKey), std::move(glyph), std::move(tab), height};
}

OkaruDashboardPanel::Detail* OkaruDashboardPanel::activeDetail() {
  const auto it = m_details.find(m_activeDetail);
  return it == m_details.end() ? nullptr : &it->second;
}

// Detalle emergente: una capa que atenúa el centro de control (clic en ella = cerrar) y encima la pestaña en
// una tarjeta. Pulsar la misma tarjeta otra vez lo cierra.
void OkaruDashboardPanel::applyDetail(const std::string& id) {
  const bool same = id == m_activeDetail;
  closeDetail();
  if (same || m_root == nullptr) {
    markDirty();
    return;
  }
  Detail& detail = m_details.at(id);
  m_activeDetail = id;
  const float scale = contentScale();
  detail.tab->setContentScale(scale);
  detail.tab->setPanelCardOpacity(panelCardOpacity());

  auto scrim = std::make_unique<ClickFlex>();
  scrim->setParticipatesInLayout(false);
  scrim->setFill(colorSpecFromRole(ColorRole::Surface, 0.7F));
  scrim->setActivation([this] { requestDetail({}); });
  m_scrim = static_cast<Flex*>(m_root->addChild(std::move(scrim)));

  auto card = ui::column({
      .out = &m_detailCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
      .participatesInLayout = false,
      .configure = [scale](Flex& f) {
        f.setCardStyle(scale, 1.0F, true);
        f.setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
        f.setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      },
  });
  // Fondo de la tarjeta: absorbe los clics para que no lleguen a la capa de atrás.
  auto blocker = ui::inputArea({});
  blocker->setParticipatesInLayout(false);
  blocker->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
  card->addChild(std::move(blocker));

  auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  header->addChild(ui::glyph({
      .glyph = detail.glyph,
      .glyphSize = Style::fontSizeTitle * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  header->addChild(ui::label({
      .text = i18n::tr(detail.titleKey),
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::Primary),
      .flexGrow = 1.0F,
  }));
  if (auto actions = detail.tab->createHeaderActions(); actions != nullptr) {
    header->addChild(std::move(actions));
  }
  header->addChild(ui::button({
      .glyph = "x",
      .tooltip = tr("close"),
      .onClick = [this] { requestDetail({}); },
      .configure = [scale](Button& b) { panel_button_style::configureHeaderIconButton(b, scale); },
  }));
  card->addChild(std::move(header));
  auto body = ui::column({.out = &m_detailBody, .align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F});
  auto container = detail.tab->create();
  container->setParticipatesInLayout(false); // se ubica a mano en doLayout, como en OkaruTabPanel
  m_detailContainer = container.get();
  body->addChild(std::move(container));
  card->addChild(std::move(body));
  m_root->addChild(std::move(card));
  detail.tab->setActive(true);
  // Entrada con la curva y la velocidad del estilo de animación elegido (MotionService).
  if (m_animations != nullptr) {
    m_scrim->setOpacity(0.0F);
    m_detailCard->setOpacity(0.0F);
    m_animations->animate(0.0F, 1.0F, static_cast<float>(Style::animNormal), Easing::EaseOutCubic, [this](float v) {
      if (m_scrim != nullptr && m_detailCard != nullptr) {
        m_scrim->setOpacity(v);
        m_detailCard->setOpacity(v);
        PanelManager::instance().requestRedraw();
      }
    }, {}, m_detailCard);
  }
  markDirty();
}

// Abrir/cerrar se aplica en la siguiente actualización: los clics que lo piden vienen de nodos que se quitan.
void OkaruDashboardPanel::requestDetail(std::string id) {
  m_pendingDetail = std::move(id);
  m_detailPending = true;
  markDirty();
}

void OkaruDashboardPanel::closeDetail() {
  if (m_animations != nullptr && m_detailCard != nullptr) {
    m_animations->cancelForOwner(m_detailCard);
  }
  Detail* detail = activeDetail();
  m_activeDetail.clear();
  if (detail != nullptr) {
    detail->tab->setActive(false);
    detail->tab->onClose();
  }
  if (m_root != nullptr) {
    if (m_detailCard != nullptr) {
      (void)m_root->removeChild(m_detailCard);
    }
    if (m_scrim != nullptr) {
      (void)m_root->removeChild(m_scrim);
    }
  }
  m_scrim = nullptr;
  m_detailCard = nullptr;
  m_detailBody = nullptr;
  m_detailContainer = nullptr;
}

void OkaruDashboardPanel::onFrameTick(float deltaMs) {
  if (Detail* detail = activeDetail(); detail != nullptr) {
    detail->tab->onFrameTick(deltaMs);
  }
}

bool OkaruDashboardPanel::dismissTransientUi() {
  Detail* detail = activeDetail();
  return detail != nullptr && detail->tab->dismissTransientUi();
}

bool OkaruDashboardPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  Detail* detail = activeDetail();
  if (detail == nullptr) {
    return false;
  }
  if (detail->tab->handleKey(sym, modifiers, pressed, preedit)) {
    return true;
  }
  // Esc cierra primero el detalle; el siguiente Esc, el centro de control.
  if (pressed && KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    requestDetail({});
    return true;
  }
  return false;
}

float OkaruDashboardPanel::cellWidth() const { return scaled(kCell); }

float OkaruDashboardPanel::preferredWidth() const {
  const float gap = Style::spaceSm * contentScale();
  // Cuadrícula + margen propio + margen de la decoración del panel (PanelManager lo resta del ancho).
  return cellWidth() * kColumns + gap * (kColumns - 1)
      + (Style::spaceLg + Style::panelPadding) * contentScale() * 2.0F;
}

std::vector<ControlCenterCardConfig> OkaruDashboardPanel::currentCards() const {
  return m_deps.services.config != nullptr ? m_deps.services.config->config().controlCenter.cards
                                           : defaultControlCenterCards();
}

void OkaruDashboardPanel::save(std::vector<ControlCenterCardConfig> cards) {
  if (m_deps.services.config != nullptr) {
    (void)m_deps.services.config->setOverride({"control_center", "cards"}, std::move(cards));
  }
  markDirty();
}

Shortcut* OkaruDashboardPanel::shortcutFor(std::string_view type) {
  const std::string id(shortcutId(type));
  if (id.empty()) {
    return nullptr;
  }
  auto it = m_shortcuts.find(id);
  if (it == m_shortcuts.end()) {
    auto created = ShortcutRegistry::create(id, m_deps.services.shortcutServices());
    if (created == nullptr) {
      return nullptr;
    }
    it = m_shortcuts.emplace(id, std::move(created)).first;
  }
  return it->second.get();
}

void OkaruDashboardPanel::create() {
  const float scale = contentScale();
  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });
  root->addChild(ui::row({.out = &m_header, .align = FlexAlign::Center, .gap = Style::spaceSm * scale}));
  // La cuadrícula va en un scroll que llena lo que deja la cabecera: con muchas tarjetas el panel llega al alto
  // de la pantalla y el resto se desplaza. Sin barra visible: su canal le quitaría ancho a la cuarta columna.
  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .contentScale = scale,
      .scrollbarVisible = false,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .participatesInLayout = false,
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
      },
  });
  m_body = scroll->content();
  m_body->setDirection(FlexDirection::Vertical);
  m_body->setAlign(FlexAlign::Stretch);
  m_body->setGap(Style::spaceSm * scale);
  root->addChild(std::move(scroll));
  setRoot(std::move(root));
  m_dirty = true;
}

void OkaruDashboardPanel::onOpen(std::string_view context) {
  // "panel-toggle control-center edit" abre directo en modo edición.
  m_mode = context == "edit" ? Mode::Edit : Mode::View;
  // "panel-toggle control-center okaru-network" abre con ese detalle encima.
  m_detailPending = m_details.contains(std::string(context));
  m_pendingDetail = m_detailPending ? std::string(context) : std::string();
  m_confirmReset = false;
  // Un plugin con ajustes cambiados necesita un acceso directo nuevo (siembra su runtime al crearse).
  if (m_deps.services.config != nullptr && !(m_deps.services.config->config().plugins == m_lastPlugins)) {
    m_lastPlugins = m_deps.services.config->config().plugins;
    std::erase_if(m_shortcuts, [](const auto& entry) { return scripting::isValidPluginEntryId(entry.first); });
  }
  for (auto& [id, shortcut] : m_shortcuts) {
    shortcut->onPanelOpen();
  }
  // Reloj, sistema, tiempo de pantalla…: una actualización por segundo mientras está abierto.
  m_tick.startRepeating(std::chrono::seconds(1), [] {
    if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel(kPanelId)) {
      manager->refresh();
    }
  });
  m_dirty = true;
}

void OkaruDashboardPanel::onClose() {
  closeDetail();
  m_tick.stop();
  for (auto& [id, shortcut] : m_shortcuts) {
    shortcut->onPanelClose();
  }
  m_cards.clear();
  clearReleasedRoot();
  m_root = nullptr;
  m_header = nullptr;
  m_body = nullptr;
  m_scroll = nullptr;
}

void OkaruDashboardPanel::markDirty() {
  m_dirty = true;
  if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel(kPanelId)) {
    manager->refresh();
  }
}

float OkaruDashboardPanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (m_root == nullptr) {
    return 0.0F;
  }
  if (m_dirty) {
    rebuild();
  }
  m_root->setSize(contentWidth, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(contentWidth);
  // m_root solo mide la cabecera (el scroll no participa); se suma la altura natural de la cuadrícula.
  const float pad = Style::spaceLg * contentScale();
  LayoutConstraints bodyConstraints;
  bodyConstraints.setExactWidth(std::max(0.0F, contentWidth - pad * 2.0F));
  const float grid = m_root->measure(renderer, constraints).height + Style::spaceMd * contentScale()
      + m_body->measure(renderer, bodyConstraints).height;
  // Con un detalle abierto el panel crece lo necesario para que quepa encima.
  const Detail* detail = activeDetail();
  return detail != nullptr ? std::max(grid, scaled(detail->height)) : grid;
}

void OkaruDashboardPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr) {
    return;
  }
  m_root->setSize(width, height);
  m_root->layout(renderer);
  if (m_scroll != nullptr && m_header != nullptr) {
    const float pad = Style::spaceLg * contentScale();
    const float top = m_header->y() + m_header->height() + Style::spaceMd * contentScale();
    m_scroll->setPosition(pad, top);
    m_scroll->setSize(std::max(0.0F, width - pad * 2.0F), std::max(0.0F, height - top - pad));
    m_scroll->layout(renderer);
  }
  Detail* detail = activeDetail();
  if (detail == nullptr || m_scrim == nullptr || m_detailCard == nullptr) {
    return;
  }
  m_scrim->setPosition(0.0F, 0.0F);
  m_scrim->setSize(width, height);
  m_scrim->layout(renderer);
  const float inset = Style::spaceSm * contentScale();
  m_detailCard->setPosition(inset, inset);
  m_detailCard->setSize(width - inset * 2.0F, height - inset * 2.0F);
  m_detailCard->layout(renderer);
  if (!m_detailCard->children().empty()) {
    Node* blocker = m_detailCard->children().front().get();
    blocker->setPosition(0.0F, 0.0F);
    blocker->setSize(m_detailCard->width(), m_detailCard->height());
  }
  if (m_detailBody != nullptr && m_detailContainer != nullptr) {
    m_detailContainer->setPosition(0.0F, 0.0F);
    m_detailContainer->setSize(m_detailBody->width(), m_detailBody->height());
    detail->tab->layout(renderer, m_detailBody->width(), m_detailBody->height());
  }
}

void OkaruDashboardPanel::doUpdate(Renderer& renderer) {
  if (m_detailPending) {
    m_detailPending = false;
    if (m_pendingDetail.empty()) {
      closeDetail();
    } else {
      applyDetail(m_pendingDetail);
    }
  }
  if (m_dirty) {
    rebuild();
  }
  for (DashboardCard* card : m_cards) {
    card->sync(renderer);
  }
  if (Detail* detail = activeDetail(); detail != nullptr) {
    detail->tab->update(renderer);
  }
}

void OkaruDashboardPanel::rebuild() {
  if (m_body == nullptr || m_header == nullptr) {
    return;
  }
  m_dirty = false;
  m_cards.clear();
  while (!m_body->children().empty()) {
    (void)m_body->removeChild(m_body->children().back().get());
  }
  rebuildHeader();
  const float scale = contentScale();
  if (m_mode == Mode::Add) {
    buildAddList(scale);
  } else {
    buildGrid(scale);
  }
}

void OkaruDashboardPanel::rebuildHeader() {
  while (!m_header->children().empty()) {
    (void)m_header->removeChild(m_header->children().back().get());
  }
  const float scale = contentScale();
  m_header->addChild(ui::label({
      .text = i18n::tr(m_mode == Mode::View ? "okaru.dashboard.title" : "okaru.dashboard.editing"),
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::Primary),
      .flexGrow = 1.0F,
  }));
  const auto headerButton = [&](std::string glyph, std::string tooltip, std::function<void()> onClick) {
    auto button = ui::button({
        .glyph = std::move(glyph),
        .tooltip = std::move(tooltip),
        .onClick = std::move(onClick),
        .configure = [scale](Button& b) { panel_button_style::configureHeaderIconButton(b, scale); },
    });
    m_header->addChild(std::move(button));
  };
  if (m_mode == Mode::View) {
    headerButton("pencil", tr("edit"), [this] {
      m_mode = Mode::Edit;
      markDirty();
    });
    headerButton("settings", tr("settings"), [] { PanelManager::instance().openSettingsWindow(); });
    return;
  }
  // Restablecer el diseño por defecto: doble confirmación.
  m_header->addChild(smallButton(
      scale, m_confirmReset ? tr("reset-confirm") : tr("reset"), "restore",
      m_confirmReset ? ButtonVariant::Destructive : ButtonVariant::Ghost,
      [this] {
        if (!m_confirmReset) {
          m_confirmReset = true;
          markDirty();
          return;
        }
        m_confirmReset = false;
        if (m_deps.services.config != nullptr) {
          (void)m_deps.services.config->clearOverride({"control_center", "cards"});
        }
        markDirty();
      }
  ));
  m_header->addChild(smallButton(scale, tr("done"), "check", ButtonVariant::Primary, [this] {
    m_mode = Mode::View;
    m_confirmReset = false;
    markDirty();
  }));
}

void OkaruDashboardPanel::buildGrid(float scale) {
  const auto cards = currentCards();
  const bool editing = m_mode == Mode::Edit;
  std::vector<CardSize> sizes;
  sizes.reserve(cards.size() + 1);
  for (const auto& card : cards) {
    sizes.push_back(normalizeSize(card.type, card.size));
  }
  if (editing) {
    sizes.push_back(CardSize::Small); // la tarjeta "+ Agregar"
  }
  const float gap = Style::spaceSm * scale;
  // El perfil es más alto; la fila toma la altura de su tarjeta más alta para quedar pareja.
  const auto heightFor = [&](std::size_t index) {
    const bool tall = index < cards.size() && cards[index].type == "profile";
    return scaled(kCardHeight * (tall ? 1.5F : 1.0F));
  };
  const auto widthFor = [&](CardSize size) {
    const int columns = span(size);
    return cellWidth() * static_cast<float>(columns) + gap * static_cast<float>(columns - 1);
  };
  const auto fix = [&](Flex& node, CardSize size, float height) {
    const float w = widthFor(size);
    node.setMinWidth(w);
    node.setMaxWidth(w);
    node.setMinHeight(height);
    node.setMaxHeight(height);
  };

  if (cards.empty() && !editing) {
    auto empty = ui::label({
        .text = tr("empty"),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    });
    m_body->addChild(std::move(empty));
  }

  for (const auto& row : packRows(sizes)) {
    float rowHeight = 0.0F;
    for (const std::size_t index : row) {
      rowHeight = std::max(rowHeight, heightFor(index));
    }
    auto line = ui::row({.align = FlexAlign::Start, .gap = gap});
    for (const std::size_t index : row) {
      std::unique_ptr<Flex> node;
      if (index >= cards.size()) {
        auto add = ui::button({
            .text = tr("add"),
            .glyph = "plus",
            .variant = ButtonVariant::Outline,
            .onClick = [this] {
              m_mode = Mode::Add;
              markDirty();
            },
        });
        add->setDirection(FlexDirection::Vertical);
        node = std::move(add);
      } else if (editing) {
        node = makeEditTile(index, sizes[index], scale);
      } else {
        auto card = createCard(
            cards[index].type, sizes[index], m_deps, scale, panelCardOpacity(), shortcutFor(cards[index].type)
        );
        if (card == nullptr) {
          continue; // tipo desconocido o acceso directo de un plugin que ya no está
        }
        m_cards.push_back(card.get());
        node = std::move(card);
      }
      fix(*node, sizes[index], rowHeight);
      line->addChild(std::move(node));
    }
    m_body->addChild(std::move(line));
  }
}

std::unique_ptr<Flex> OkaruDashboardPanel::makeEditTile(std::size_t index, CardSize size, float scale) {
  const auto cards = currentCards();
  const std::string type = cards[index].type;
  const CardKind* kind = findKind(type);
  auto tile = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceXs * scale,
      .padding = Style::spaceSm * scale,
      .configure = [scale, opacity = panelCardOpacity()](Flex& f) {
        f.setCardStyle(scale, opacity, true);
        f.setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      },
  });
  auto title = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
  Shortcut* shortcut = isShortcutType(type) ? shortcutFor(type) : nullptr;
  title->addChild(ui::glyph({
      .glyph = shortcut != nullptr ? shortcut->displayIcon() : std::string(kind != nullptr ? kind->glyph : "help"),
      .glyphSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  title->addChild(ui::label({
      .text = cardLabel(type, shortcut),
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
      .flexGrow = 1.0F,
  }));
  tile->addChild(std::move(title));

  const auto move = [this, index](int delta) {
    auto list = currentCards();
    const auto target = static_cast<std::ptrdiff_t>(index) + delta;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(list.size())) {
      return;
    }
    std::swap(list[index], list[static_cast<std::size_t>(target)]);
    save(std::move(list));
  };
  auto first = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceXs * scale});
  first->addChild(smallButton(scale, {}, "chevron-left", ButtonVariant::Ghost, [move] { move(-1); }));
  first->addChild(smallButton(scale, {}, "chevron-right", ButtonVariant::Ghost, [move] { move(1); }));
  auto second = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceXs * scale});
  const bool resizable = kind != nullptr && kind->sizes.size() > 1;
  auto sizeButton = smallButton(
      scale, std::string(sizeLetter(size)), {}, ButtonVariant::Outline,
      [this, index, type, size] {
        auto list = currentCards();
        list[index].size = std::string(sizeName(nextSize(type, size)));
        save(std::move(list));
      }
  );
  sizeButton->setEnabled(resizable);
  sizeButton->setTooltip(tr("size"));
  second->addChild(std::move(sizeButton));
  auto remove = smallButton(scale, {}, "x", ButtonVariant::Ghost, [this, index] {
    auto list = currentCards();
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
    save(std::move(list));
  });
  remove->setTooltip(tr("remove"));
  second->addChild(std::move(remove));

  // En una tarjeta pequeña los cuatro botones van en dos filas; en las demás, en una.
  if (size == CardSize::Small) {
    tile->addChild(std::move(first));
    tile->addChild(std::move(second));
  } else {
    auto all = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    all->addChild(std::move(first));
    all->addChild(ui::column({.flexGrow = 1.0F}));
    all->addChild(std::move(second));
    tile->addChild(std::move(all));
  }
  return tile;
}

void OkaruDashboardPanel::buildAddList(float scale) {
  const auto cards = currentCards();
  const auto present = [&cards](std::string_view type) {
    return std::ranges::any_of(cards, [type](const ControlCenterCardConfig& c) { return c.type == type; });
  };
  struct Entry {
    std::string type;
    std::string label;
    std::string glyph;
  };
  std::vector<Entry> entries;
  for (const auto& kind : builtinCardKinds()) {
    if (!present(kind.type)) {
      entries.push_back({std::string(kind.type), cardLabel(kind.type, nullptr), std::string(kind.glyph)});
    }
  }
  if (m_deps.services.config != nullptr) {
    for (auto& item : ShortcutRegistry::catalog()) {
      const std::string type = std::string(kShortcutPrefix) + item.type;
      if (present(type) || !ShortcutRegistry::isAvailable(item.type, m_deps.services.config->config())) {
        continue;
      }
      Shortcut* shortcut = shortcutFor(type);
      entries.push_back({
          type,
          item.literalLabel ? item.labelKey : i18n::tr(item.labelKey),
          shortcut != nullptr ? shortcut->displayIcon() : std::string("click"),
      });
    }
  }

  auto top = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  top->addChild(smallButton(scale, tr("back"), "chevron-left", ButtonVariant::Ghost, [this] {
    m_mode = Mode::Edit;
    markDirty();
  }));
  top->addChild(ui::label({
      .text = tr("add-title"),
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  m_body->addChild(std::move(top));

  if (entries.empty()) {
    m_body->addChild(ui::label({
        .text = tr("all-added"),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    }));
    return;
  }
  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .height = scaled(460.0F),
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
      },
  });
  Flex* list = scroll->content();
  list->setDirection(FlexDirection::Vertical);
  list->setAlign(FlexAlign::Stretch);
  list->setGap(Style::spaceXs * scale);
  for (auto& entry : entries) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceSm * scale,
        .configure = [scale, opacity = panelCardOpacity()](Flex& f) { f.setCardStyle(scale, opacity); },
    });
    row->addChild(ui::glyph({
        .glyph = entry.glyph,
        .glyphSize = Style::fontSizeTitle * scale,
        .color = colorSpecFromRole(ColorRole::Primary),
    }));
    row->addChild(ui::label({
        .text = entry.label,
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .flexGrow = 1.0F,
    }));
    row->addChild(smallButton(scale, tr("add"), "plus", ButtonVariant::Primary, [this, type = entry.type] {
      auto next = currentCards();
      next.push_back({type, std::string(sizeName(defaultSize(type)))});
      m_mode = Mode::Edit;
      save(std::move(next));
    }));
    list->addChild(std::move(row));
  }
  m_body->addChild(std::move(scroll));
}
