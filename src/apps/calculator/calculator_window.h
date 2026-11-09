#pragma once

// OKARU: interfaz de okaru-calc. Modos como GNOME Calculator (básico, avanzado, financiero, programación y
// conversión), historial, variables y preferencias; el estado vive en `State` y la escena se reconstruye entera
// cuando cambia algo más que el texto de la expresión.

#include "apps/calculator/calc_engine.h"
#include "apps/calculator/calc_state.h"

#include <memory>
#include <string>
#include <vector>

class Input;
class Label;
class Node;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
}

namespace okaru::calc {

  class CalculatorWindow {
  public:
    explicit CalculatorWindow(apps::OkaruApp& app);
    ~CalculatorWindow();

  private:
    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHistory(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildDisplay(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildKeypad(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildProgrammer(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildFinancial(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildConversion(float scale);
    [[nodiscard]] std::unique_ptr<Node> buildPreferences(float scale);

    [[nodiscard]] Settings effectiveSettings() const;
    [[nodiscard]] bool onKey(const KeyboardEvent& event);
    void typeText(std::string_view text);
    // Tras "=", lo que empieza un número nuevo reemplaza el resultado; un operador lo continúa.
    void startFreshIfNeeded(char32_t first);
    void backspace();
    void clear();
    void submit();
    void updatePreview();
    void recallHistory(int direction);
    void copyResult();
    void convertNow();
    void refreshExchangeRates();
    void changed(bool rebuild = true);

    apps::OkaruApp& m_app;
    Engine m_engine;
    State m_state;
    std::string m_expression;
    std::string m_lastResult;
    std::string m_message; // vista previa, aviso o error bajo la expresión
    bool m_messageIsError = false;
    bool m_showPreferences = false;
    bool m_justEvaluated = false;
    int m_historyCursor = -1; // -1 = escribiendo; si no, índice del historial que se está viendo
    std::vector<std::string> m_financialArgs;
    std::string m_conversionValue = "1";
    std::string m_conversionResult;
    bool m_fetchingRates = false;
    bool m_ratesFetched = false;

    Input* m_input = nullptr;
    Label* m_messageLabel = nullptr;
    Label* m_conversionLabel = nullptr;
  };

} // namespace okaru::calc
