// OKARU: núcleo de okaru-files: nombres numerados, orden natural, filtro sin tildes y la cola de operaciones
// (crear, copiar con conflicto, mover, papelera con deshacer) sobre una carpeta temporal.
#include "apps/files/file_model.h"
#include "apps/files/file_ops.h"
#include "apps/files/share.h"
#include "core/deferred_call.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace okaru::files;
namespace fs = std::filesystem;

namespace {

  // Corre lo que la cola manda al hilo principal hasta que `done` sea cierto.
  template <typename Pred> void pump(Pred done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
      for (auto& fn : DeferredCall::takePending()) {
        fn();
      }
      assert(std::chrono::steady_clock::now() < until);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  FileEntry entry(std::string name, bool dir = false, std::uint64_t size = 0) {
    FileEntry e;
    e.display = e.name = name;
    e.directory = dir;
    e.size = size;
    e.sortKey = name; // la clave real viene de GLib; aquí basta el nombre
    return e;
  }

} // namespace

int main() {
  assert(numberedName("foto.png", 2, false) == "foto (2).png");
  assert(numberedName("copia.tar.gz", 3, false) == "copia (3).tar.gz");
  assert(numberedName("carpeta.v1", 2, true) == "carpeta.v1 (2)");
  assert(numberedName(".bashrc", 2, false) == ".bashrc (2)");

  std::vector<FileEntry> list{entry("b.txt", false, 10), entry("Docs", true), entry("a.txt", false, 30)};
  sortEntries(list, {});
  assert(list[0].name == "Docs" && list[1].name == "a.txt" && list[2].name == "b.txt");
  sortEntries(list, {.sort = SortKey::Size, .descending = true});
  assert(list[0].name == "Docs" && list[1].name == "a.txt");

  assert(normalizeForMatch("Canción ÁRBOL") == "cancion arbol");
  assert(parseBluetoothDevice("Device 41:42:FB:63:C0:7E Buds Pro").first == "41:42:FB:63:C0:7E");
  assert(parseBluetoothDevice("[CHG] Device 41:42:FB:63:C0:7E Buds Pro").second == "Buds Pro");
  assert(parseBluetoothDevice("No default controller available").first.empty());
  assert(matchesFilter(entry("Música"), normalizeForMatch("musi")));
  assert(!matchesFilter(entry("Música"), normalizeForMatch("video")));

  const fs::path root = fs::temp_directory_path() / ("okaru-files-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "src" / "sub");
  fs::create_directories(root / "dst");
  std::ofstream(root / "src" / "uno.txt") << "uno";
  std::ofstream(root / "src" / "sub" / "dos.txt") << "dos";
  std::ofstream(root / "dst" / "uno.txt") << "viejo";
  const std::string rootUri = pathToUri(root.string());
  const std::string srcUri = pathToUri((root / "src").string());
  const std::string dstUri = pathToUri((root / "dst").string());
  assert(uniqueName(dstUri, "uno.txt") == "uno (2).txt");

  OperationQueue queue;
  std::optional<OpResult> last;
  int conflicts = 0;
  queue.setOnDone([&](OpResult result) {
    queue.remember(result);
    last = std::move(result);
  });
  queue.setOnConflict([&](const Conflict& conflict, std::function<void(ConflictAnswer)> reply) {
    ++conflicts;
    assert(conflict.destination.ends_with("uno.txt"));
    reply({ConflictChoice::KeepBoth, false});
  });
  const auto runOp = [&](OpRequest request) {
    last.reset();
    queue.submit(std::move(request));
    pump([&] { return last.has_value(); });
    assert(last->errors.empty());
  };

  // Copiar la carpeta entera y un archivo en conflicto: "Conservar ambos".
  runOp({.kind = OpKind::Copy, .sources = {childUri(srcUri, "sub"), childUri(srcUri, "uno.txt")}, .destination = dstUri});
  assert(conflicts == 1);
  assert(fs::exists(root / "dst" / "sub" / "dos.txt"));
  assert(fs::exists(root / "dst" / "uno (2).txt"));
  assert(last->created.size() == 2);

  // Deshacer la copia la borra.
  queue.undo();
  last.reset();
  pump([&] { return last.has_value(); });
  assert(!fs::exists(root / "dst" / "sub"));
  assert(!fs::exists(root / "dst" / "uno (2).txt"));
  assert(fs::exists(root / "dst" / "uno.txt"));

  // Duplicar en la misma carpeta.
  runOp({.kind = OpKind::Copy, .sources = {childUri(srcUri, "uno.txt")}, .destination = srcUri});
  assert(fs::exists(root / "src" / "uno (2).txt"));

  // Mover y deshacer.
  runOp({.kind = OpKind::Move, .sources = {childUri(srcUri, "uno (2).txt")}, .destination = dstUri});
  assert(fs::exists(root / "dst" / "uno (2).txt") && !fs::exists(root / "src" / "uno (2).txt"));
  queue.undo();
  last.reset();
  pump([&] { return last.has_value(); });
  assert(fs::exists(root / "src" / "uno (2).txt") && !fs::exists(root / "dst" / "uno (2).txt"));

  // Renombrar, deshacer y rehacer.
  runOp({.kind = OpKind::Rename, .sources = {childUri(srcUri, "uno (2).txt")}, .destination = "tres.txt"});
  assert(fs::exists(root / "src" / "tres.txt"));
  queue.undo();
  last.reset();
  pump([&] { return last.has_value(); });
  assert(fs::exists(root / "src" / "uno (2).txt"));
  queue.redo();
  last.reset();
  pump([&] { return last.has_value(); });
  assert(fs::exists(root / "src" / "tres.txt"));

  // Carpeta nueva con nombre ocupado.
  runOp({.kind = OpKind::NewFolder, .destination = srcUri, .name = "sub"});
  assert(fs::is_directory(root / "src" / "sub (2)"));

  // Listado y modelo.
  std::vector<FileEntry> entries;
  std::string error;
  assert(listDirectory(srcUri, entries, error));
  assert(entries.size() == 4);

  // Borrar para siempre.
  runOp({.kind = OpKind::Delete, .sources = {rootUri}});
  assert(!fs::exists(root));
  assert(!last->undo.has_value());
  return 0;
}
