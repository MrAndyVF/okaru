// OKARU: funciones puras de Ajustes → Aplicaciones (mimeapps.list, globs2, ocultar del lanzador).
#include "system/okaru_apps.h"

#include <cassert>
#include <string>

using namespace okaru::apps;

int main() {
  // Reemplaza un tipo existente, añade los nuevos dentro del grupo y respeta los demás grupos.
  const std::string list = "[Added Associations]\ntext/plain=a.desktop;\n\n[Default Applications]\n"
                           "text/plain=old.desktop\nimage/png=v.desktop\n\n[Removed Associations]\nx=y\n";
  const std::string out = setDefaultsInText(list, {"text/plain", "text/markdown"}, "ed.desktop");
  assert(defaultInText(out, "text/plain") == "ed.desktop");
  assert(defaultInText(out, "text/markdown") == "ed.desktop");
  assert(defaultInText(out, "image/png") == "v.desktop");
  assert(out.find("[Added Associations]\ntext/plain=a.desktop;") == 0);
  assert(out.find("text/markdown=ed.desktop") < out.find("[Removed Associations]"));
  assert(out.find("old.desktop") == std::string::npos);

  // Sin grupo, lo crea; con id vacío, quita el tipo.
  const std::string created = setDefaultsInText("", {"inode/directory"}, "files.desktop");
  assert(created == "[Default Applications]\ninode/directory=files.desktop\n");
  assert(defaultInText(setDefaultsInText(created, {"inode/directory"}, ""), "inode/directory").empty());
  assert(defaultInText("[Default Applications]\nimage/png = v.desktop;w.desktop;\n", "image/png") == "v.desktop");

  // Solo globs simples "*.ext", sin repetir.
  std::vector<std::string> exts;
  extensionsFromGlobs("# c\n50:image/png:*.png\n50:image/png:*.PNG:cs\n50:text/plain:*.txt\n10:image/png:*.png\n"
                      "50:image/png:foo*.bar\n", "image/png", exts);
  assert((exts == std::vector<std::string>{".png", ".PNG"}));

  // Ocultar: NoDisplay=true + marca en [Desktop Entry]; mostrar lo deja como estaba.
  const std::string entry = "[Desktop Entry]\nName=App\nNoDisplay=false\n\n[Desktop Action new]\nNoDisplay=x\n";
  const std::string hidden = hideInText(entry, true);
  assert(hidden.find("[Desktop Entry]\nNoDisplay=true\nX-Okaru-Hidden=true\nName=App\n") == 0);
  assert(hidden.find("NoDisplay=false") == std::string::npos);
  assert(hidden.find("NoDisplay=x") != std::string::npos);
  assert(hideInText(hidden, false) == "[Desktop Entry]\nName=App\n\n[Desktop Action new]\nNoDisplay=x\n");
  return 0;
}
