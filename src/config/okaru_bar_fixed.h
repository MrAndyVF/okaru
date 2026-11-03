#pragma once

// OKARU: la barra es parte del diseño, no algo que se arma. Una sola barra, con su aspecto fijo; el
// usuario solo ordena los widgets (workspaces siempre primero), agrupa con acordeón y ajusta la
// experiencia (autoocultar, reservar espacio, opacidad, blur, tooltips).

struct Config;

namespace okaru::bar {

  // Se aplica al leer la config del usuario: impone el aspecto fijo, deja una
  // sola barra sin overrides por monitor, pone workspaces primero y quita el aspecto propio de cada widget.
  // Del dock solo conserva, por monitor, si se muestra.
  void applyFixedBar(Config& config);

  // Solo para los tests de upstream que prueban varias barras o estilos por grupo.
  void setFixedBarEnabled(bool enabled);

} // namespace okaru::bar
