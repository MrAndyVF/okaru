// OKARU: motor de okaru-archive sobre archivos reales en un temporal.
#include "apps/archive/archive_backend.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace okaru::archive;

namespace {

  void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  std::string read(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  bool has(const std::vector<Item>& items, const std::string& path, bool directory = false) {
    return std::ranges::any_of(items, [&](const Item& i) { return i.path == path && i.directory == directory; });
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("okaru-archive-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  write(root / "src/a.txt", "hola");
  write(root / "src/docs/b.txt", "mundo");
  write(root / "extra.txt", "nuevo");

  assert(writableFormat("x.TAR.GZ") == "tar.gz" && writableFormat("x.tgz") == "tar.gz");
  assert(writableFormat("x.zip") == "zip" && writableFormat("x.7z") == "7z" && writableFormat("x.rar").empty());

  for (const char* name : {"t.zip", "t.tar.gz", "t.tar.xz", "t.tar.zst", "t.7z"}) {
    const std::string archive = (root / name).string();
    assert(create(archive, (root / "src").string(), {"a.txt", "docs"}, {.level = 6}, {}).ok());
    std::vector<Item> items;
    assert(list(archive, {}, items).ok());
    assert(has(items, "a.txt") && has(items, "docs", true) && has(items, "docs/b.txt"));

    // Extraer una selección desde dentro de "docs": queda b.txt sin la carpeta.
    const fs::path out = root / (std::string(name) + "-out");
    {
      const auto r = extract(archive, {}, {"docs/b.txt"}, "docs", out.string(), {});
      if (!r.ok())
        std::fprintf(stderr, "%s: %d %s\n", name, int(r.status), r.error.c_str());
      assert(r.ok());
    }
    assert(read(out / "b.txt") == "mundo" && !fs::exists(out / "docs"));
    assert(extract(archive, {}, {}, {}, (out / "todo").string(), {}).ok());
    assert(read(out / "todo/docs/b.txt") == "mundo");
    assert(test(archive, {}, {}).ok());

    // Editar: borrar, renombrar, carpeta nueva y agregar dentro de ella.
    {
      const auto r = edit(
          archive, {},
          {.remove = {"a.txt"},
           .rename = {{"docs", "papeles"}},
           .folders = {"nueva"},
           .addFiles = {(root / "extra.txt").string()},
           .addInto = "nueva"},
          {}
      );
      if (!r.ok())
        std::fprintf(stderr, "edit %s: %d %s\n", name, int(r.status), r.error.c_str());
      assert(r.ok());
    }
    assert(list(archive, {}, items).ok());
    assert(
        !has(items, "a.txt")
        && has(items, "papeles/b.txt")
        && has(items, "nueva", true)
        && has(items, "nueva/extra.txt")
    );
  }

  // Zip con contraseña (AES-256): sin ella pide contraseña; con la mala falla; con la buena extrae.
  const std::string secret = (root / "secreto.zip").string();
  assert(create(secret, (root / "src").string(), {"a.txt"}, {.password = "clave"}, {}).ok());
  std::vector<Item> items;
  assert(list(secret, {}, items).status == Status::NeedPassword);
  assert(extract(secret, "mala", {}, {}, (root / "mal").string(), {}).status == Status::WrongPassword);
  assert(extract(secret, "clave", {}, {}, (root / "bien").string(), {}).ok());
  assert(read(root / "bien/a.txt") == "hola");

  // Comentario del zip.
  const std::string zip = (root / "t.zip").string();
  assert(zipComment(zip).value_or("x").empty());
  std::string error;
  assert(setZipComment(zip, "Hecho con OKARU", error));
  assert(zipComment(zip) == "Hecho con OKARU");
  assert(list(zip, {}, items).ok());

  if (sevenZipAvailable()) {
    // 7z con la cabecera cifrada: libarchive no puede; lo hace 7-Zip con la contraseña por stdin.
    const std::string seven = (root / "cifrado.7z").string();
    assert(create(seven, (root / "src").string(), {"a.txt", "docs"}, {.password = "clave"}, {}).ok());
    assert(list(seven, {}, items).status == Status::NeedPassword);
    assert(list(seven, "clave", items).ok() && has(items, "docs/b.txt"));
    assert(extract(seven, "clave", {"docs/b.txt"}, "docs", (root / "s7").string(), {}).ok());
    assert(read(root / "s7/b.txt") == "mundo");
    assert(extract(seven, "mala", {}, {}, (root / "s7mal").string(), {}).status == Status::WrongPassword);
    {
      const auto r = edit(
          seven, "clave", {.remove = {"a.txt"}, .addFiles = {(root / "extra.txt").string()}, .addInto = "docs"}, {}
      );
      if (!r.ok())
        std::fprintf(stderr, "edit7z: %d %s\n", int(r.status), r.error.c_str());
      assert(r.ok());
    }
    assert(list(seven, "clave", items).ok() && !has(items, "a.txt") && has(items, "docs/extra.txt"));
    assert(test(seven, "clave", {}).ok());

    // Volúmenes de 1 KiB.
    write(root / "big/datos.bin", std::string(5000, 'x'));
    assert(create(
               (root / "partes.7z").string(), (root / "big").string(), {"datos.bin"}, {.level = 0, .volumeSize = 1024},
               {}
    )
               .ok());
    assert(fs::exists(root / "partes.7z.001") && fs::exists(root / "partes.7z.002"));
  }

  fs::remove_all(root);
  return 0;
}
