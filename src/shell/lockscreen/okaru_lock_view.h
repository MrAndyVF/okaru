#pragma once

// OKARU: pantalla de bloqueo propia, al estilo de Omarchy y con la marca del instalador. Mismo diseño que el
// greeter: fondo = wallpaper con mucho blur bajo un velo oscuro; al centro el logo
// (郁 en el color de acento + OKARU, assets/okaru-logo.svg); debajo el usuario y un campo de contraseña
// rectangular con candado; abajo las acciones de sesión como iconos. Texto siempre claro sobre el velo, en
// modo claro y oscuro.
//
// La edición de la contraseña sigue en el Input de LockSurface (teclado, IME, foco): la vista lo coloca dentro
// de su campo con opacidad 0 y dibuja encima los puntos o el texto de ayuda.

#include <functional>
#include <string>
#include <vector>

class Box;
class Image;
class InputArea;
class Input;
class Label;
class Node;
class Renderer;

namespace lockscreen {

  struct OkaruLockAction {
    std::string label;
    std::string icon; // assets/pixel-icons/<icon>.svg
    std::function<void()> onClick;
  };

  struct OkaruLockState {
    std::string user;
    std::size_t passwordLength = 0;
    std::string placeholder;
    std::string status; // "Verificando…", error de autenticación…
    bool statusError = false;
    bool authenticating = false;
  };

  class OkaruLockView {
  public:
    // Crea sus nodos como hijos de `root`; el velo va sobre el fondo y debajo de todo lo demás.
    void build(Node& root);
    // Mueve el Input de contraseña a la vista. Se llama una vez.
    void adoptPasswordField(Input* field);
    void setActions(std::vector<OkaruLockAction> actions);
    void setVisible(bool visible);
    void layout(Renderer& renderer, float width, float height, const OkaruLockState& state);

  private:
    struct ActionButton {
      Box* box = nullptr;
      Image* icon = nullptr;
      std::string iconName;
      InputArea* area = nullptr;
    };

    void syncLogo(Renderer& renderer, int heightPx);

    Box* m_veil = nullptr;
    Image* m_logo = nullptr;
    Label* m_user = nullptr;
    Image* m_lockIcon = nullptr;
    std::string m_lockIconKey;
    Box* m_field = nullptr;
    Label* m_fieldText = nullptr;
    Label* m_status = nullptr;
    Node* m_actions = nullptr; // posiciones a mano: un Flex las recolocaría
    Label* m_actionHint = nullptr;
    Input* m_input = nullptr;
    std::vector<ActionButton> m_buttons;
    std::string m_hovered;
    std::vector<std::string> m_actionKeys;
    std::string m_logoKey;
  };

} // namespace lockscreen
