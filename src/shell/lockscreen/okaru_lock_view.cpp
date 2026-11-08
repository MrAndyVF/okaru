#include "shell/lockscreen/okaru_lock_view.h"

#include "core/files/resource_paths.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace lockscreen {

  namespace {

    // Texto claro sobre el velo oscuro, en modo claro y oscuro (como Omarchy).
    constexpr Color kText = rgba(1.0F, 1.0F, 1.0F, 0.94F);
    constexpr Color kMuted = rgba(1.0F, 1.0F, 1.0F, 0.62F);
    constexpr Color kFaint = rgba(1.0F, 1.0F, 1.0F, 0.38F);
    constexpr Color kVeil = rgba(0.0F, 0.0F, 0.0F, 0.55F);
    constexpr Color kFieldFill = rgba(0.0F, 0.0F, 0.0F, 0.35F);
    constexpr std::size_t kMaxDots = 24;
    constexpr float kLogoAspect = kOkaruLogoAspect;
    // Tipografía pixelada para el usuario, a juego con los bloques del logo (paquete ttf-bigblueterminal-nerd).
    constexpr const char* kPixelFont = "BigBlueTermPlus Nerd Font";

    std::string hexOf(const Color& c) {
      const auto ch = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      return std::format("#{:02x}{:02x}{:02x}", ch(c.r), ch(c.g), ch(c.b));
    }

    // Copia de un SVG de assets con un relleno cambiado de color, en $XDG_RUNTIME_DIR (se rehace al cambiar
    // la paleta): el kanji del logo en el acento, el candado en acento o rojo.
    std::string tintedAsset(const std::string& asset, std::string_view fill, const std::string& hex) {
      std::ifstream in(paths::assetPath(asset));
      std::stringstream buffer;
      buffer << in.rdbuf();
      std::string svg = buffer.str();
      if (svg.empty()) {
        return {};
      }
      if (const auto at = svg.find(fill); at != std::string::npos) {
        std::string replaced(fill);
        replaced.replace(replaced.size() - 8, 7, hex); // fill="#rrggbb": el color son los 7 antes de la comilla
        svg.replace(at, fill.size(), replaced);
      }
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const std::filesystem::path dir = runtime != nullptr && runtime[0] != '\0'
          ? std::filesystem::path(runtime)
          : std::filesystem::temp_directory_path();
      std::string stem = std::filesystem::path(asset).stem().string();
      const auto path = dir / std::format("okaru-lock-{}-{}.svg", stem, hex.substr(1));
      std::ofstream(path, std::ios::trunc) << svg;
      return path.string();
    }

    // Todo el texto en la tipografía pixelada, a juego con los bloques del logo y los iconos.
    Label* addLabel(Node& root, std::int32_t zIndex) {
      Label* out = nullptr;
      auto label = ui::label({.out = &out, .fontFamily = std::string(kPixelFont), .maxLines = 1});
      label->setZIndex(zIndex);
      root.addChild(std::move(label));
      return out;
    }

    float placeCentered(Renderer& renderer, Node& node, float width, float y) {
      const LayoutSize size = node.measure(renderer, LayoutConstraints::available(width, 0.0F));
      node.arrange(
          renderer, LayoutRect{std::round((width - size.width) * 0.5F), std::round(y), size.width, size.height}
      );
      return size.height;
    }

  } // namespace

  std::string okaruLogoPath(const Color& kanji, const Color& name) {
    std::ifstream in(paths::assetPath("okaru-logo.svg"));
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string svg = buffer.str();
    if (svg.empty()) {
      return {};
    }
    const std::string kanjiHex = hexOf(kanji);
    const std::string nameHex = hexOf(name);
    const auto recolor = [&svg](std::string_view from, const std::string& to) {
      if (const auto at = svg.find(from); at != std::string::npos) {
        std::string replaced(from);
        replaced.replace(replaced.size() - 8, 7, to); // fill="#rrggbb": el color son los 7 antes de la comilla
        svg.replace(at, from.size(), replaced);
      }
    };
    recolor("id=\"kanji\" fill=\"#7095c8\"", kanjiHex);
    recolor("id=\"name\" fill=\"#ffffff\"", nameHex);
    recolor("id=\"shadow\" fill=\"#ffffff\"", nameHex);
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path dir = runtime != nullptr && runtime[0] != '\0' ? std::filesystem::path(runtime)
                                                                               : std::filesystem::temp_directory_path();
    const auto path = dir / std::format("okaru-logo-{}-{}.svg", kanjiHex.substr(1), nameHex.substr(1));
    std::ofstream(path, std::ios::trunc) << svg;
    return path.string();
  }

  void OkaruLockView::build(Node& root) {
    root.addChild(ui::box({.out = &m_veil, .configure = [](Box& box) { box.setZIndex(1); }}));
    root.addChild(ui::image({.out = &m_logo, .fit = ImageFit::Contain}));
    m_logo->setZIndex(3);
    m_user = addLabel(root, 3);
    root.addChild(ui::image({.out = &m_lockIcon, .fit = ImageFit::Contain}));
    m_lockIcon->setZIndex(3);
    root.addChild(ui::box({.out = &m_field, .configure = [](Box& box) { box.setZIndex(3); }}));
    m_fieldText = addLabel(root, 5);
    m_status = addLabel(root, 3);
    auto actions = ui::node({});
    actions->setZIndex(3);
    m_actions = root.addChild(std::move(actions));
    m_actionHint = addLabel(root, 3);
  }

  void OkaruLockView::adoptPasswordField(Input* field) {
    if (field == nullptr || m_input == field || m_field == nullptr) {
      return;
    }
    Node* parent = field->parent();
    if (parent == nullptr) {
      return;
    }
    auto owned = parent->removeChild(field);
    owned->setZIndex(4);
    m_input = static_cast<Input*>(m_field->parent()->addChild(std::move(owned)));
    // Invisible pero con foco: edita el texto (teclado, IME); los puntos los dibuja la vista.
    // Sin placeholder propio: al enviar la contraseña el valor se vacía y lo mostraba a la izquierda.
    m_input->setPlaceholder("");
    m_input->setOpacity(0.0F);
    m_input->setFrameVisible(false);
  }

  void OkaruLockView::setActions(std::vector<OkaruLockAction> actions) {
    std::vector<std::string> keys;
    for (const auto& action : actions) {
      keys.push_back(action.icon + "|" + action.label);
    }
    if (keys == m_actionKeys) {
      return;
    }
    m_actionKeys = std::move(keys);
    while (!m_actions->children().empty()) {
      m_actions->removeChild(m_actions->children().front().get());
    }
    m_buttons.clear();
    for (auto& action : actions) {
      ActionButton button{.iconName = action.icon};
      auto box = ui::box({.out = &button.box});
      box->addChild(ui::image({.out = &button.icon, .fit = ImageFit::Contain}));
      auto area = ui::inputArea({.out = &button.area});
      area->setOnEnter([this, box = button.box, label = action.label](const InputArea::PointerData&) {
        box->setFill(rgba(1.0F, 1.0F, 1.0F, 0.14F));
        box->setBorder(kMuted, 1.0F);
        m_hovered = label;
        m_actionHint->setText(label);
        m_actionHint->setVisible(true);
      });
      area->setOnLeave([this, box = button.box, label = action.label] {
        box->setFill(rgba(1.0F, 1.0F, 1.0F, 0.0F));
        box->setBorder(kFaint, 1.0F);
        if (m_hovered == label) {
          m_hovered.clear();
          m_actionHint->setVisible(false);
        }
      });
      area->setOnClick([onClick = std::move(action.onClick)](const InputArea::PointerData&) {
        if (onClick) {
          onClick();
        }
      });
      box->addChild(std::move(area));
      m_buttons.push_back(button);
      m_actions->addChild(std::move(box));
    }
  }

  void OkaruLockView::setVisible(bool visible) {
    for (Node* node : std::initializer_list<Node*>{
             m_veil, m_logo, m_user, m_lockIcon, m_field, m_fieldText, m_status, m_actions, m_actionHint
         }) {
      if (node != nullptr) {
        node->setVisible(visible);
      }
    }
    if (m_input != nullptr) {
      m_input->setVisible(visible);
    }
    if (!visible || m_hovered.empty()) {
      m_actionHint->setVisible(false);
    }
  }

  void OkaruLockView::syncLogo(Renderer& renderer, int heightPx) {
    const std::string accent = hexOf(colorForRole(ColorRole::Primary));
    const std::string key = std::format("{}@{}", accent, heightPx);
    if (key == m_logoKey) {
      return;
    }
    const std::string path = okaruLogoPath(colorForRole(ColorRole::Primary), rgba(1.0F, 1.0F, 1.0F, 1.0F));
    // Se rasteriza con el lado mayor al ancho que va a ocupar.
    const int targetSize = static_cast<int>(std::lround(static_cast<float>(heightPx) * kLogoAspect));
    if (!path.empty() && m_logo->setSourceFile(renderer, path, targetSize, false)) {
      m_logoKey = key;
    }
  }

  void OkaruLockView::layout(Renderer& renderer, float width, float height, const OkaruLockState& state) {
    setVisible(true);
    const float s = std::clamp(height / 1080.0F, 0.7F, 2.0F); // todo escala con la altura (1080 p = 1)
    const Color accent = colorForRole(ColorRole::Primary);
    const Color error = colorForRole(ColorRole::Error);

    m_veil->setStyle(RoundedRectStyle{.fill = kVeil, .fillMode = FillMode::Solid});
    m_veil->setPosition(0.0F, 0.0F);
    m_veil->setSize(width, height);

    // Logo: 郁 (acento) + OKARU, centrado un poco por encima de la mitad.
    const float logoHeight = std::round(150.0F * s);
    const float logoWidth = std::round(logoHeight * kLogoAspect);
    syncLogo(renderer, static_cast<int>(logoHeight));
    const float logoY = std::round(height * 0.47F - logoHeight);
    // Centrado óptico: la sombra de OKARU carga el lado derecho; se corre un 2 % del ancho a la izquierda.
    m_logo->setPosition(std::round((width - logoWidth) * 0.5F - logoWidth * 0.02F), logoY);
    m_logo->setSize(logoWidth, logoHeight);

    // Usuario.
    float y = logoY + logoHeight + 48.0F * s;
    m_user->setVisible(!state.user.empty());
    if (!state.user.empty()) {
      m_user->setFontSize(22.0F * s);
      m_user->setColor(kMuted);
      m_user->setText(state.user);
      y += placeCentered(renderer, *m_user, width, y) + 14.0F * s;
    }

    // Campo: rectángulo con borde de acento (rojo si falló) y el candado a la izquierda, fuera del campo.
    const float fieldW = std::round(420.0F * s);
    const float fieldH = std::round(58.0F * s);
    const float fieldX = std::round((width - fieldW) * 0.5F);
    const float fieldY = std::round(y);
    m_field->setStyle(
        RoundedRectStyle{
            .fill = kFieldFill,
            .border = state.statusError ? error : accent,
            .fillMode = FillMode::Solid,
            .borderWidth = std::max(2.0F, std::round(2.0F * s)),
        }
    );
    m_field->setPosition(fieldX, fieldY);
    m_field->setSize(fieldW, fieldH);
    // Iconos de 12 px de rejilla: múltiplos enteros para que cada pixel quede nítido.
    const float lockSize = std::round(12.0F * std::max(2.0F, std::round(2.0F * s)));
    const std::string lockHex = hexOf(state.statusError ? error : accent);
    const std::string lockKey = std::format("{}@{}", lockHex, lockSize);
    if (lockKey != m_lockIconKey) {
      const std::string path = tintedAsset("pixel-icons/lock.svg", "fill=\"#ffffff\"", lockHex);
      if (!path.empty() && m_lockIcon->setSourceFile(renderer, path, static_cast<int>(lockSize), false)) {
        m_lockIconKey = lockKey;
      }
    }
    m_lockIcon->setPosition(fieldX - lockSize - 18.0F * s, std::round(fieldY + (fieldH - lockSize) * 0.5F));
    m_lockIcon->setSize(lockSize, lockSize);
    if (m_input != nullptr) {
      m_input->setOpacity(0.0F);
      m_input->setControlHeight(fieldH);
      m_input->arrange(renderer, LayoutRect{fieldX, fieldY, fieldW, fieldH});
    }
    if (state.passwordLength > 0) {
      std::string dots;
      for (std::size_t i = 0; i < std::min(state.passwordLength, kMaxDots); ++i) {
        dots += i == 0 ? "●" : " ●";
      }
      m_fieldText->setFontSize(22.0F * s);
      m_fieldText->setColor(kText);
      m_fieldText->setText(dots);
    } else {
      // Mismo tamaño y tono que el usuario: con el gris tenue la fuente pixelada se veía distinta.
      m_fieldText->setFontSize(22.0F * s);
      m_fieldText->setColor(kMuted);
      m_fieldText->setText(state.placeholder);
    }
    m_fieldText->setMaxWidth(fieldW - 32.0F * s);
    const LayoutSize textSize =
        static_cast<Node*>(m_fieldText)->measure(renderer, LayoutConstraints::available(fieldW, fieldH));
    m_fieldText->arrange(
        renderer,
        LayoutRect{
            std::round(fieldX + (fieldW - textSize.width) * 0.5F),
            std::round(fieldY + (fieldH - textSize.height) * 0.5F), textSize.width, textSize.height
        }
    );

    // Estado debajo del campo.
    m_status->setVisible(!state.status.empty());
    if (!state.status.empty()) {
      m_status->setFontSize(15.0F * s);
      m_status->setColor(state.statusError ? error : kMuted);
      m_status->setText(state.status);
      placeCentered(renderer, *m_status, width, fieldY + fieldH + 16.0F * s);
    }

    // Acciones abajo al centro: cuadrados con borde tenue; el nombre aparece encima al pasar el puntero.
    const float buttonSize = std::round(56.0F * s);
    const float iconSize = lockSize;
    for (const auto& button : m_buttons) {
      button.box->setFill(rgba(1.0F, 1.0F, 1.0F, 0.0F));
      button.box->setBorder(kFaint, 1.0F);
      button.box->setSize(buttonSize, buttonSize);
      (void)button.icon->setSourceFile(
          renderer, paths::assetPath("pixel-icons/" + button.iconName + ".svg").string(), static_cast<int>(iconSize),
          false
      );
      button.icon->setOpacity(0.94F);
      button.icon->setPosition(std::round((buttonSize - iconSize) * 0.5F), std::round((buttonSize - iconSize) * 0.5F));
      button.icon->setSize(iconSize, iconSize);
      button.area->setPosition(0.0F, 0.0F);
      button.area->setSize(buttonSize, buttonSize);
    }
    const float gap = std::round(18.0F * s);
    const float rowW = buttonSize * static_cast<float>(m_buttons.size())
        + gap * static_cast<float>(std::max<std::size_t>(1, m_buttons.size()) - 1);
    const float rowX = std::round((width - rowW) * 0.5F);
    const float rowY = std::round(height - 56.0F * s - buttonSize);
    m_actions->setPosition(rowX, rowY);
    m_actions->setSize(rowW, buttonSize);
    for (std::size_t i = 0; i < m_buttons.size(); ++i) {
      m_buttons[i].box->setPosition(static_cast<float>(i) * (buttonSize + gap), 0.0F);
    }
    m_actionHint->setFontSize(14.0F * s);
    m_actionHint->setColor(kMuted);
    if (!m_hovered.empty()) {
      placeCentered(renderer, *m_actionHint, width, rowY - 30.0F * s);
    }
  }

} // namespace lockscreen
