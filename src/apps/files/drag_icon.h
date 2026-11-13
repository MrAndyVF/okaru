#pragma once

// OKARU: la imagen que acompaña al puntero al arrastrar archivos desde okaru-files (miniaturas o iconos apilados y,
// con varios, una insignia con el número).

#include "apps/common/file_drop.h"
#include "render/core/color.h"

#include <cstddef>
#include <string>
#include <vector>

namespace okaru::files {

  // `images`: hasta 3 rutas (miniatura o icono), la primera encima; `count`: archivos arrastrados.
  [[nodiscard]] apps::DragIcon makeDragIcon(
      const std::vector<std::string>& images, std::size_t count, const Color& badge, const Color& onBadge, float scale
  );

} // namespace okaru::files
