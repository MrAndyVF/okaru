#pragma once

// OKARU: `okaru askpass [texto...]` pide una contraseña con el diálogo de la shell y la imprime por stdout.
// Sirve como SUDO_ASKPASS. Sale con 1 si se cancela o la shell no está corriendo.
namespace okaru::cli {
  int runAskpass(int argc, char* argv[]);
}
