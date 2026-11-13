#include "apps/archive/archive_backend.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace okaru::archive {

  namespace {

    constexpr std::size_t kBlock = 64 * 1024;

    bool cancelled(const Progress& progress) {
      return progress.cancel != nullptr && progress.cancel->load(std::memory_order_relaxed);
    }

    void report(const Progress& progress, double value) {
      if (progress.update) {
        progress.update(std::clamp(value, 0.0, 1.0));
      }
    }

    std::string lower(std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }

    std::string trimSlashes(std::string path) {
      while (path.starts_with("./")) {
        path.erase(0, 2);
      }
      while (!path.empty() && path.front() == '/') {
        path.erase(0, 1);
      }
      while (!path.empty() && path.back() == '/') {
        path.pop_back();
      }
      return path;
    }

    bool insideOrSame(const std::string& path, const std::string& folder) {
      return path == folder || (path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/');
    }

    // libarchive no sabe seguir (cabecera cifrada de 7z, rar cifrado): eso lo hace 7-Zip.
    bool needsSevenZip(const std::string& message) {
      const std::string text = lower(message);
      return text.contains("encrypt") || text.contains("not supported") || text.contains("unsupported");
    }

    bool wrongPassword(const std::string& message) {
      const std::string text = lower(message);
      return text.contains("passphrase") || text.contains("password") || text.contains("decrypt");
    }

    struct Reader {
      ::archive* handle = nullptr;
      explicit Reader(const std::string& path, const std::string& password) {
        handle = archive_read_new();
        archive_read_support_filter_all(handle);
        archive_read_support_format_all(handle);
        archive_read_support_format_raw(handle); // .gz/.xz sueltos (un solo archivo)
        if (!password.empty()) {
          archive_read_add_passphrase(handle, password.c_str());
        }
        if (archive_read_open_filename(handle, path.c_str(), 10240) != ARCHIVE_OK) {
          opened = false;
        }
      }
      ~Reader() { archive_read_free(handle); }
      Reader(const Reader&) = delete;
      Reader& operator=(const Reader&) = delete;
      [[nodiscard]] std::string error() const {
        const char* message = archive_error_string(handle);
        return message != nullptr ? message : "libarchive";
      }
      bool opened = true;
    };

    // ¿libarchive no puede con este comprimido (cabecera cifrada de 7z, rar cifrado)? Mira hasta la primera cabecera.
    bool sevenZipOnly(const std::string& path, const std::string& password) {
      Reader probe(path, password);
      if (!probe.opened) {
        return needsSevenZip(probe.error());
      }
      archive_entry* entry = nullptr;
      const int status = archive_read_next_header(probe.handle, &entry);
      return status < ARCHIVE_WARN && status != ARCHIVE_EOF && needsSevenZip(probe.error());
    }

    std::string entryPath(archive_entry* entry) {
      const char* utf8 = archive_entry_pathname_utf8(entry);
      return trimSlashes(
          utf8 != nullptr ? utf8 : (archive_entry_pathname(entry) != nullptr ? archive_entry_pathname(entry) : "")
      );
    }

    // ── 7-Zip por línea de comandos ─────────────────────────────────────────────────────────────────────────────

    struct RunResult {
      int code = -1;
      std::string output;
    };

    // Corre 7z con la contraseña por stdin (siempre se escribe algo: sin ella, 7z pregunta y espera). Las líneas
    // con "NN%" van a `percent`.
    RunResult runSevenZip(
        const std::vector<std::string>& args, const std::string& cwd, const std::string& password,
        const Progress& progress, bool collect
    ) {
      RunResult result;
      int in[2];
      int out[2];
      if (::pipe2(in, O_CLOEXEC) != 0) {
        return result;
      }
      if (::pipe2(out, O_CLOEXEC) != 0) {
        ::close(in[0]);
        ::close(in[1]);
        return result;
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
      posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
      if (!cwd.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
      }
      std::vector<std::string> full{"7z"};
      full.insert(full.end(), args.begin(), args.end());
      std::vector<char*> argv;
      argv.reserve(full.size() + 1);
      for (auto& arg : full) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);
      pid_t pid = 0;
      const int spawned = ::posix_spawnp(&pid, "7z", &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      ::close(in[0]);
      ::close(out[1]);
      if (spawned != 0) {
        ::close(in[1]);
        ::close(out[0]);
        result.output = "7z no está instalado";
        return result;
      }
      // Dos líneas: 7z pide la contraseña una vez (o dos al crear con verificación).
      const std::string secret = password + "\n" + password + "\n";
      (void)!::write(in[1], secret.data(), secret.size());
      ::close(in[1]);

      std::string pending;
      char buffer[4096];
      pollfd pfd{.fd = out[0], .events = POLLIN, .revents = 0};
      bool killed = false;
      while (true) {
        if (cancelled(progress) && !killed) {
          ::kill(pid, SIGTERM);
          killed = true;
        }
        const int ready = ::poll(&pfd, 1, 200);
        if (ready <= 0) {
          continue;
        }
        const ssize_t n = ::read(out[0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        for (ssize_t i = 0; i < n; ++i) {
          const char c = buffer[i];
          if (c == '\n' || c == '\r' || c == '\b') {
            if (const auto pos = pending.find('%'); pos != std::string::npos && pos > 0) {
              std::size_t start = pos;
              while (start > 0 && std::isdigit(static_cast<unsigned char>(pending[start - 1])) != 0) {
                --start;
              }
              if (start < pos) {
                report(progress, std::stoi(pending.substr(start, pos - start)) / 100.0);
              }
            }
            if (c == '\n' && collect) {
              result.output += pending;
              result.output += '\n';
            } else if (c == '\n' && !pending.empty()) {
              result.output += pending + "\n"; // errores y avisos
            }
            pending.clear();
          } else {
            pending += c;
          }
        }
      }
      result.output += pending;
      ::close(out[0]);
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      result.code = killed ? -2 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return result;
    }

    Result sevenZipResult(const RunResult& run) {
      if (run.code == 0) {
        return {};
      }
      if (run.code == -2) {
        return {.status = Status::Cancelled, .error = {}};
      }
      const std::string text = lower(run.output);
      if (text.contains("wrong password") || text.contains("cannot open encrypted")) {
        return {.status = Status::WrongPassword, .error = "7z"};
      }
      // La última línea con algo es el motivo más útil.
      std::string reason;
      std::size_t end = run.output.size();
      while (end > 0 && reason.empty()) {
        const std::size_t start = run.output.rfind('\n', end - 1);
        const std::size_t from = start == std::string::npos ? 0 : start + 1;
        reason = run.output.substr(from, end - from);
        while (!reason.empty() && std::isspace(static_cast<unsigned char>(reason.back())) != 0) {
          reason.pop_back();
        }
        end = start == std::string::npos ? 0 : start;
      }
      return {.status = Status::Error, .error = reason.empty() ? "7z" : reason};
    }

    Result listSevenZip(const std::string& archive, const std::string& password, std::vector<Item>& items) {
      const RunResult run = runSevenZip({"l", "-slt", "-ba", "--", archive}, {}, password, {}, true);
      if (run.code != 0) {
        Result failed = sevenZipResult(run);
        if (failed.status == Status::WrongPassword && password.empty()) {
          failed.status = Status::NeedPassword;
        }
        return failed;
      }
      Item current;
      bool any = false;
      auto flush = [&] {
        if (any && !current.path.empty()) {
          items.push_back(current);
        }
        current = {};
        any = false;
      };
      std::size_t start = 0;
      while (start < run.output.size()) {
        std::size_t end = run.output.find('\n', start);
        if (end == std::string::npos) {
          end = run.output.size();
        }
        const std::string line = run.output.substr(start, end - start);
        start = end + 1;
        const auto eq = line.find(" = ");
        if (eq == std::string::npos) {
          continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 3);
        if (key == "Path") {
          flush();
          current.path = trimSlashes(value);
          any = true;
        } else if (key == "Size") {
          current.size = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "Folder") {
          current.directory = value == "+";
        } else if (key == "Attributes") {
          current.directory = current.directory || value.starts_with('D');
        } else if (key == "Encrypted") {
          current.encrypted = value == "+";
        } else if (key == "Modified" && value.size() >= 19) {
          std::tm tm{};
          if (::strptime(value.c_str(), "%Y-%m-%d %H:%M:%S", &tm) != nullptr) {
            tm.tm_isdst = -1;
            current.modified = std::mktime(&tm);
          }
        }
      }
      flush();
      if (password.empty() && std::ranges::any_of(items, [](const Item& i) { return i.encrypted; })) {
        // Lista sin cifrar pero datos cifrados: hace falta la contraseña para extraer (se pide al abrir).
        return {.status = Status::NeedPassword, .error = {}};
      }
      return {};
    }

    // Carpetas implícitas: "a/b/c.txt" sin entrada para "a" ni "a/b".
    void addImplicitFolders(std::vector<Item>& items) {
      std::set<std::string> folders;
      std::set<std::string> known;
      for (const auto& item : items) {
        if (item.directory) {
          known.insert(item.path);
        }
        for (std::size_t slash = item.path.find('/'); slash != std::string::npos;
             slash = item.path.find('/', slash + 1)) {
          folders.insert(item.path.substr(0, slash));
        }
      }
      for (const auto& folder : folders) {
        if (!known.contains(folder)) {
          items.push_back({.path = folder, .directory = true});
        }
      }
    }

    // Copia los datos de la entrada actual del lector al escritor (o al disco).
    // `toDisk`: el escritor es archive_write_disk (por bloques con desplazamiento); si no, un comprimido.
    bool copyData(
        ::archive* reader, ::archive* writer, bool toDisk, const Progress& progress, std::uint64_t& done,
        std::uint64_t total, std::string& error
    ) {
      const void* block = nullptr;
      std::size_t size = 0;
      la_int64_t offset = 0;
      while (true) {
        if (cancelled(progress)) {
          return false;
        }
        const int status = archive_read_data_block(reader, &block, &size, &offset);
        if (status == ARCHIVE_EOF) {
          return true;
        }
        if (status < ARCHIVE_WARN) {
          error = archive_error_string(reader) != nullptr ? archive_error_string(reader) : "libarchive";
          return false;
        }
        const la_ssize_t wrote = writer == nullptr ? 0
            : toDisk                               ? archive_write_data_block(writer, block, size, offset)
                                                   : archive_write_data(writer, block, size);
        if (wrote < ARCHIVE_WARN) {
          error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
          return false;
        }
        done += size;
        if (total > 0) {
          report(progress, static_cast<double>(done) / static_cast<double>(total));
        }
      }
    }

    std::uint64_t totalSize(const std::string& archive, const std::string& password) {
      std::vector<Item> items;
      std::uint64_t total = 0;
      if (list(archive, password, items).ok()) {
        for (const auto& item : items) {
          total += item.size;
        }
      }
      return total;
    }

    // Escritor de libarchive según la extensión; nullptr si okaru-archive no escribe ese formato.
    ::archive* newWriter(const std::string& format, int level, const std::string& password, std::string& error) {
      ::archive* writer = archive_write_new();
      const std::string lvl = std::to_string(level);
      auto option = [&](const char* module, const char* key, const std::string& value) {
        if (archive_write_set_option(writer, module, key, value.c_str()) < ARCHIVE_WARN) {
          error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : key;
          return false;
        }
        return true;
      };
      bool ok = true;
      if (format == "zip") {
        archive_write_set_format_zip(writer);
        if (level >= 0) {
          ok = option("zip", "compression-level", lvl) && (level > 0 || option("zip", "compression", "store"));
        }
        if (ok && !password.empty()) {
          ok = option("zip", "encryption", "aes256")
              && archive_write_set_passphrase(writer, password.c_str()) == ARCHIVE_OK;
        }
      } else if (format == "7z") {
        archive_write_set_format_7zip(writer);
        ok = option("7zip", "compression", level == 0 ? "store" : "lzma2");
        if (ok && level > 0) {
          ok = option("7zip", "compression-level", lvl);
        }
      } else if (format.starts_with("tar")) {
        archive_write_set_format_pax_restricted(writer);
        if (format == "tar.gz") {
          archive_write_add_filter_gzip(writer);
          ok = level < 0 || option("gzip", "compression-level", lvl);
        } else if (format == "tar.xz") {
          archive_write_add_filter_xz(writer);
          ok = level < 0 || option("xz", "compression-level", lvl);
        } else if (format == "tar.bz2") {
          archive_write_add_filter_bzip2(writer);
          ok = level < 0 || option("bzip2", "compression-level", std::to_string(std::max(1, level)));
        } else if (format == "tar.zst") {
          archive_write_add_filter_zstd(writer);
          // 1..9 de la interfaz → 1..19 de zstd.
          ok = level < 0 || option("zstd", "compression-level", std::to_string(std::max(1, level * 2 + 1)));
        }
      } else {
        error = "formato sin escritura";
        ok = false;
      }
      if (!ok) {
        archive_write_free(writer);
        return nullptr;
      }
      return writer;
    }

    // Agrega `source` (archivo o carpeta, recursivo) como `name` dentro del escritor.
    bool addPath(
        ::archive* writer, ::archive* disk, const fs::path& source, const std::string& name, const Progress& progress,
        std::uint64_t& done, std::uint64_t total, std::string& error
    ) {
      std::vector<std::pair<fs::path, std::string>> pending{{source, name}};
      std::error_code ec;
      if (fs::is_directory(fs::symlink_status(source, ec))) {
        for (auto it = fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
          if (ec) {
            break;
          }
          pending.emplace_back(it->path(), name + "/" + fs::relative(it->path(), source, ec).generic_string());
        }
      }
      for (const auto& [path, inside] : pending) {
        if (cancelled(progress)) {
          return false;
        }
        archive_entry* entry = archive_entry_new();
        archive_entry_copy_sourcepath(entry, path.c_str());
        if (archive_read_disk_entry_from_file(disk, entry, -1, nullptr) != ARCHIVE_OK) {
          error = path.string() + ": " + (archive_error_string(disk) != nullptr ? archive_error_string(disk) : "");
          archive_entry_free(entry);
          return false;
        }
        archive_entry_set_pathname_utf8(entry, inside.c_str());
        if (archive_write_header(writer, entry) < ARCHIVE_WARN) {
          error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
          archive_entry_free(entry);
          return false;
        }
        if (archive_entry_filetype(entry) == AE_IFREG && archive_entry_size(entry) > 0) {
          std::ifstream in(path, std::ios::binary);
          std::vector<char> buffer(kBlock);
          while (in) {
            if (cancelled(progress)) {
              archive_entry_free(entry);
              return false;
            }
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) {
              break;
            }
            if (archive_write_data(writer, buffer.data(), got) < 0) {
              error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
              archive_entry_free(entry);
              return false;
            }
            done += got;
            if (total > 0) {
              report(progress, static_cast<double>(done) / static_cast<double>(total));
            }
          }
        }
        archive_entry_free(entry);
      }
      return true;
    }

    std::uint64_t diskSize(const std::vector<fs::path>& sources) {
      std::uint64_t total = 0;
      std::error_code ec;
      for (const auto& source : sources) {
        if (fs::is_regular_file(source, ec)) {
          total += fs::file_size(source, ec);
        } else if (fs::is_directory(source, ec)) {
          for (auto it = fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied, ec);
               it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!ec && it->is_regular_file(ec)) {
              total += it->file_size(ec);
            }
          }
        }
      }
      return total;
    }

    std::string tempNear(const std::string& path, const char* tag) {
      const fs::path target(path);
      return (target.parent_path() / ("." + target.filename().string() + "." + tag + "-" + std::to_string(::getpid())))
          .string();
    }

    // Mueve lo que quedó en `staging/base` a `destination` (extraer quitando la carpeta del comprimido).
    bool moveOut(const fs::path& staging, const std::string& base, const fs::path& destination, std::string& error) {
      const fs::path from = base.empty() ? staging : staging / base;
      std::error_code ec;
      fs::create_directories(destination, ec);
      for (const auto& child : fs::directory_iterator(from, ec)) {
        fs::path to = destination / child.path().filename();
        if (fs::exists(fs::symlink_status(to, ec))) {
          fs::remove_all(to, ec); // reemplazar: la ventana ya preguntó antes de extraer encima
        }
        fs::rename(child.path(), to, ec);
        if (ec) {
          error = ec.message();
          return false;
        }
      }
      return true;
    }

  } // namespace

  std::string writableFormat(const std::string& name) {
    const std::string n = lower(name);
    for (const char* format : {"tar.gz", "tar.xz", "tar.bz2", "tar.zst"}) {
      if (n.ends_with(std::string(".") + format)) {
        return format;
      }
    }
    if (n.ends_with(".tgz")) {
      return "tar.gz";
    }
    if (n.ends_with(".txz")) {
      return "tar.xz";
    }
    if (n.ends_with(".tbz2") || n.ends_with(".tbz")) {
      return "tar.bz2";
    }
    if (n.ends_with(".tzst")) {
      return "tar.zst";
    }
    for (const char* format : {"zip", "7z", "tar"}) {
      if (n.ends_with(std::string(".") + format)) {
        return format;
      }
    }
    return {};
  }

  bool sevenZipAvailable() {
    static const bool available = [] {
      const char* path = std::getenv("PATH");
      std::string dirs = path != nullptr ? path : "/usr/bin";
      std::size_t start = 0;
      while (start <= dirs.size()) {
        const std::size_t end = dirs.find(':', start);
        const std::string dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && ::access((dir + "/7z").c_str(), X_OK) == 0) {
          return true;
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return false;
    }();
    return available;
  }

  Result list(const std::string& archive, const std::string& password, std::vector<Item>& items) {
    items.clear();
    Reader reader(archive, password);
    if (!reader.opened) {
      const std::string message = reader.error();
      if (needsSevenZip(message) && sevenZipAvailable()) {
        Result viaSeven = listSevenZip(archive, password, items);
        addImplicitFolders(items);
        return viaSeven;
      }
      return {.status = Status::Error, .error = message};
    }
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    bool encrypted = false;
    while ((status = archive_read_next_header(reader.handle, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
      Item item;
      item.path = entryPath(entry);
      if (archive_format(reader.handle) == ARCHIVE_FORMAT_RAW && (item.path.empty() || item.path == "data")) {
        // .gz/.xz suelto: el nombre del archivo sin la extensión.
        item.path = fs::path(archive).stem().string();
      }
      if (item.path.empty()) {
        continue;
      }
      item.size = archive_entry_size_is_set(entry) != 0 ? static_cast<std::uint64_t>(archive_entry_size(entry)) : 0;
      item.modified = archive_entry_mtime(entry);
      item.mode = archive_entry_mode(entry);
      item.directory = archive_entry_filetype(entry) == AE_IFDIR;
      item.encrypted = archive_entry_is_encrypted(entry) != 0;
      encrypted = encrypted || item.encrypted;
      items.push_back(std::move(item));
      archive_read_data_skip(reader.handle);
    }
    if (status != ARCHIVE_EOF) {
      const std::string message = reader.error();
      if (needsSevenZip(message) && sevenZipAvailable()) {
        items.clear();
        Result viaSeven = listSevenZip(archive, password, items);
        addImplicitFolders(items);
        return viaSeven;
      }
      if (items.empty()) {
        return {.status = Status::Error, .error = message};
      }
    }
    addImplicitFolders(items);
    if (encrypted && password.empty()) {
      return {.status = Status::NeedPassword, .error = {}};
    }
    return {};
  }

  Result extract(
      const std::string& archive, const std::string& password, const std::vector<std::string>& paths,
      const std::string& base, const std::string& destination, const Progress& progress
  ) {
    const std::string prefix = trimSlashes(base);
    std::vector<std::string> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths) {
      wanted.push_back(trimSlashes(path));
    }
    const auto selected = [&](const std::string& path) {
      return wanted.empty() ? (prefix.empty() || insideOrSame(path, prefix))
                            : std::ranges::any_of(wanted, [&](const std::string& w) { return insideOrSame(path, w); });
    };

    // Se extrae a una carpeta temporal junto al destino y luego se mueve: así quitar `base` es un rename.
    const fs::path staging = tempNear((fs::path(destination) / "x").string(), "extract");
    std::error_code ec;
    fs::create_directories(staging, ec);
    auto cleanup = [&] { fs::remove_all(staging, ec); };

    Reader reader(archive, password);
    std::string message = reader.opened ? std::string{} : reader.error();
    bool viaSeven = !reader.opened && needsSevenZip(message);
    if (!reader.opened && !viaSeven) {
      cleanup();
      return {.status = Status::Error, .error = message};
    }

    if (!viaSeven) {
      ::archive* disk = archive_write_disk_new();
      // Sin SECURE_NOABSOLUTEPATHS: la ruta final es absoluta (la carpeta temporal); la del comprimido llega sin '/'
      // inicial (trimSlashes) y NODOTDOT rechaza "..", así que nada sale de la carpeta temporal.
      archive_write_disk_set_options(
          disk,
          ARCHIVE_EXTRACT_TIME
              | ARCHIVE_EXTRACT_PERM
              | ARCHIVE_EXTRACT_SECURE_NODOTDOT
              | ARCHIVE_EXTRACT_SECURE_SYMLINKS
      );
      archive_write_disk_set_standard_lookup(disk);
      const std::uint64_t total = totalSize(archive, password);
      std::uint64_t done = 0;
      archive_entry* entry = nullptr;
      int status = ARCHIVE_OK;
      Result result;
      while ((status = archive_read_next_header(reader.handle, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        std::string path = entryPath(entry);
        if (archive_format(reader.handle) == ARCHIVE_FORMAT_RAW && (path.empty() || path == "data")) {
          path = fs::path(archive).stem().string();
        }
        if (path.empty() || !selected(path)) {
          archive_read_data_skip(reader.handle);
          continue;
        }
        const std::string target = (staging / path).string();
        archive_entry_copy_pathname(entry, target.c_str());
        if (const char* link = archive_entry_hardlink(entry); link != nullptr) {
          archive_entry_copy_hardlink(entry, (staging / trimSlashes(link)).c_str()); // enlaces duros de un tar
        }
        if (archive_write_header(disk, entry) < ARCHIVE_WARN) {
          result = {
              .status = Status::Error, .error = archive_error_string(disk) != nullptr ? archive_error_string(disk) : ""
          };
          break;
        }
        std::string error;
        if (!copyData(reader.handle, disk, true, progress, done, total, error)) {
          if (cancelled(progress)) {
            result = {.status = Status::Cancelled, .error = {}};
          } else if (wrongPassword(error)) {
            result = {.status = password.empty() ? Status::NeedPassword : Status::WrongPassword, .error = error};
          } else {
            result = {.status = Status::Error, .error = error};
          }
          break;
        }
        archive_write_finish_entry(disk);
      }
      if (result.ok() && status != ARCHIVE_EOF) {
        message = reader.error();
        if (needsSevenZip(message)) {
          viaSeven = true;
        } else {
          result = {.status = Status::Error, .error = message};
        }
      }
      archive_write_free(disk);
      if (!viaSeven) {
        if (result.ok() && !moveOut(staging, prefix, destination, result.error)) {
          result.status = Status::Error;
        }
        cleanup();
        return result;
      }
    }

    // 7-Zip: cabecera cifrada de 7z o rar cifrado.
    if (!sevenZipAvailable()) {
      cleanup();
      return {.status = Status::Error, .error = message};
    }
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    std::vector<std::string> args{"x", "-y", "-bsp1", "-o" + staging.string(), "--", archive};
    if (!wanted.empty()) {
      args.insert(args.end(), wanted.begin(), wanted.end());
    } else if (!prefix.empty()) {
      args.push_back(prefix);
    }
    Result result = sevenZipResult(runSevenZip(args, {}, password, progress, false));
    if (result.status == Status::WrongPassword && password.empty()) {
      result.status = Status::NeedPassword;
    }
    if (result.ok() && !moveOut(staging, prefix, destination, result.error)) {
      result.status = Status::Error;
    }
    cleanup();
    return result;
  }

  Result test(const std::string& archive, const std::string& password, const Progress& progress) {
    Reader reader(archive, password);
    std::string message = reader.opened ? std::string{} : reader.error();
    if (reader.opened) {
      const std::uint64_t total = totalSize(archive, password);
      std::uint64_t done = 0;
      archive_entry* entry = nullptr;
      int status = ARCHIVE_OK;
      while ((status = archive_read_next_header(reader.handle, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        std::string error;
        if (!copyData(reader.handle, nullptr, false, progress, done, total, error)) {
          if (cancelled(progress)) {
            return {.status = Status::Cancelled, .error = {}};
          }
          if (wrongPassword(error)) {
            return {.status = password.empty() ? Status::NeedPassword : Status::WrongPassword, .error = error};
          }
          return {.status = Status::Error, .error = entryPath(entry) + ": " + error};
        }
      }
      if (status == ARCHIVE_EOF) {
        return {};
      }
      message = reader.error();
    }
    if (needsSevenZip(message) && sevenZipAvailable()) {
      Result result = sevenZipResult(runSevenZip({"t", "-bsp1", "--", archive}, {}, password, progress, false));
      if (result.status == Status::WrongPassword && password.empty()) {
        result.status = Status::NeedPassword;
      }
      return result;
    }
    return {.status = Status::Error, .error = message};
  }

  Result create(
      const std::string& output, const std::string& folder, const std::vector<std::string>& names,
      const CreateOptions& options, const Progress& progress
  ) {
    const std::string format = writableFormat(output);
    if (format.empty() || names.empty()) {
      return {.status = Status::Error, .error = "formato sin escritura"};
    }
    // 7-Zip: 7z con contraseña (con nombres cifrados) o cualquier formato en volúmenes.
    if ((format == "7z" && !options.password.empty()) || options.volumeSize > 0) {
      if (!sevenZipAvailable()) {
        return {.status = Status::Error, .error = "7z no está instalado"};
      }
      std::vector<std::string> args{"a", "-y", "-bsp1"};
      args.emplace_back(format == "7z" ? "-t7z" : format == "zip" ? "-tzip" : "-ttar");
      if (options.level >= 0 && (format == "7z" || format == "zip")) {
        args.push_back("-mx=" + std::to_string(options.level));
      }
      if (!options.password.empty()) {
        args.emplace_back("-p");
        if (format == "7z") {
          args.emplace_back("-mhe=on");
        } else {
          args.emplace_back("-mem=AES256");
        }
      }
      if (options.volumeSize > 0) {
        args.push_back("-v" + std::to_string(options.volumeSize) + "b");
      }
      args.emplace_back("--");
      args.push_back(output);
      args.insert(args.end(), names.begin(), names.end());
      return sevenZipResult(runSevenZip(args, folder, options.password, progress, false));
    }

    std::string error;
    ::archive* writer = newWriter(format, options.level, options.password, error);
    if (writer == nullptr) {
      return {.status = Status::Error, .error = error};
    }
    const std::string temp = tempNear(output, "create");
    if (archive_write_open_filename(writer, temp.c_str()) != ARCHIVE_OK) {
      error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
      archive_write_free(writer);
      return {.status = Status::Error, .error = error};
    }
    ::archive* disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);
    std::vector<fs::path> sources;
    sources.reserve(names.size());
    for (const auto& name : names) {
      sources.push_back(fs::path(folder) / name);
    }
    const std::uint64_t total = diskSize(sources);
    std::uint64_t done = 0;
    bool ok = true;
    for (std::size_t i = 0; i < names.size() && ok; ++i) {
      ok = addPath(writer, disk, sources[i], names[i], progress, done, total, error);
    }
    archive_read_free(disk);
    if (archive_write_close(writer) != ARCHIVE_OK && ok) {
      error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
      ok = false;
    }
    archive_write_free(writer);
    std::error_code ec;
    if (!ok) {
      fs::remove(temp, ec);
      return {.status = cancelled(progress) ? Status::Cancelled : Status::Error, .error = error};
    }
    fs::rename(temp, output, ec);
    if (ec) {
      fs::remove(temp, ec);
      return {.status = Status::Error, .error = ec.message()};
    }
    return {};
  }

  Result edit(const std::string& archive, const std::string& password, const Edit& changes, const Progress& progress) {
    const std::string format = writableFormat(archive);
    if (format.empty()) {
      return {.status = Status::Error, .error = "formato sin escritura"};
    }
    const std::string into = trimSlashes(changes.addInto);
    auto removed = [&](const std::string& path) {
      return std::ranges::any_of(changes.remove, [&](const std::string& r) {
        return insideOrSame(path, trimSlashes(r));
      });
    };
    auto renamed = [&](std::string path) {
      for (const auto& [from, to] : changes.rename) {
        const std::string f = trimSlashes(from);
        if (insideOrSame(path, f)) {
          return trimSlashes(to) + path.substr(f.size());
        }
      }
      return path;
    };

    const bool viaSeven = sevenZipOnly(archive, password);
    Reader reader(archive, password);
    if (!reader.opened && !viaSeven) {
      return {.status = Status::Error, .error = reader.error()};
    }

    if (viaSeven) {
      // 7z cifrado: borrar y renombrar con 7z; agregar armando la carpeta destino en un temporal.
      if (!sevenZipAvailable()) {
        return {.status = Status::Error, .error = "7z no está instalado"};
      }
      if (!changes.remove.empty()) {
        std::vector<std::string> args{"d", "-y", "-bsp1", "--", archive};
        args.insert(args.end(), changes.remove.begin(), changes.remove.end());
        if (Result r = sevenZipResult(runSevenZip(args, {}, password, progress, false)); !r.ok()) {
          return r;
        }
      }
      for (const auto& [from, to] : changes.rename) {
        if (Result r =
                sevenZipResult(runSevenZip({"rn", "-y", "--", archive, from, to}, {}, password, progress, false));
            !r.ok()) {
          return r;
        }
      }
      if (!changes.addFiles.empty() || !changes.folders.empty()) {
        const fs::path staging = tempNear(archive, "add");
        std::error_code ec;
        fs::create_directories(staging / into, ec);
        for (const auto& folder : changes.folders) {
          fs::create_directories(staging / trimSlashes(folder), ec);
        }
        // shortcut: copia lo que se agrega a un temporal (7z no tiene "agregar dentro de"); con archivos enormes
        // conviene enlazarlos (hardlink) cuando estén en el mismo sistema de archivos.
        for (const auto& file : changes.addFiles) {
          fs::copy(
              file, staging / into / fs::path(file).filename(),
              fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec
          );
        }
        std::vector<std::string> top;
        for (const auto& child : fs::directory_iterator(staging, ec)) {
          top.push_back(child.path().filename().string());
        }
        std::vector<std::string> args{"a", "-y", "-bsp1"};
        if (!password.empty()) {
          args.insert(args.end(), {"-p", "-mhe=on"});
        }
        args.insert(args.end(), {"--", archive});
        args.insert(args.end(), top.begin(), top.end());
        Result r = sevenZipResult(runSevenZip(args, staging.string(), password, progress, false));
        fs::remove_all(staging, ec);
        return r;
      }
      return {};
    }

    std::string error;
    ::archive* writer = newWriter(format, -1, format == "zip" ? password : std::string{}, error);
    if (writer == nullptr) {
      return {.status = Status::Error, .error = error};
    }
    const std::string temp = tempNear(archive, "edit");
    if (archive_write_open_filename(writer, temp.c_str()) != ARCHIVE_OK) {
      error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
      archive_write_free(writer);
      return {.status = Status::Error, .error = error};
    }
    std::vector<fs::path> added(changes.addFiles.begin(), changes.addFiles.end());
    const std::uint64_t total = totalSize(archive, password) + diskSize(added);
    std::uint64_t done = 0;
    bool ok = true;
    std::set<std::string> written;
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while (ok && ((status = archive_read_next_header(reader.handle, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN)) {
      const std::string path = entryPath(entry);
      if (path.empty() || removed(path)) {
        archive_read_data_skip(reader.handle);
        continue;
      }
      const std::string target = renamed(path);
      archive_entry_set_pathname_utf8(
          entry, (archive_entry_filetype(entry) == AE_IFDIR ? target + "/" : target).c_str()
      );
      if (archive_write_header(writer, entry) < ARCHIVE_WARN) {
        error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
        ok = false;
        break;
      }
      written.insert(target);
      ok = copyData(reader.handle, writer, false, progress, done, total, error);
      if (!ok && wrongPassword(error)) {
        archive_write_free(writer);
        fs::remove(temp);
        return {.status = password.empty() ? Status::NeedPassword : Status::WrongPassword, .error = error};
      }
    }
    if (ok && status != ARCHIVE_EOF && status != ARCHIVE_OK && status != ARCHIVE_WARN) {
      error = reader.error();
      ok = false;
    }
    for (const auto& folder : changes.folders) {
      if (!ok) {
        break;
      }
      const std::string name = trimSlashes(folder);
      if (written.contains(name)) {
        continue;
      }
      archive_entry* dir = archive_entry_new();
      archive_entry_set_pathname_utf8(dir, (name + "/").c_str());
      archive_entry_set_filetype(dir, AE_IFDIR);
      archive_entry_set_perm(dir, 0755);
      archive_entry_set_mtime(dir, std::time(nullptr), 0);
      ok = archive_write_header(writer, dir) >= ARCHIVE_WARN;
      archive_entry_free(dir);
    }
    if (ok && !added.empty()) {
      ::archive* disk = archive_read_disk_new();
      archive_read_disk_set_standard_lookup(disk);
      for (const auto& source : added) {
        const std::string name = (into.empty() ? "" : into + "/") + source.filename().string();
        if (written.contains(name)) {
          error = name + ": ya existe en el comprimido";
          ok = false;
          break;
        }
        if (!(ok = addPath(writer, disk, source, name, progress, done, total, error))) {
          break;
        }
      }
      archive_read_free(disk);
    }
    if (archive_write_close(writer) != ARCHIVE_OK && ok) {
      error = archive_error_string(writer) != nullptr ? archive_error_string(writer) : "libarchive";
      ok = false;
    }
    archive_write_free(writer);
    std::error_code ec;
    if (!ok) {
      fs::remove(temp, ec);
      return {.status = cancelled(progress) ? Status::Cancelled : Status::Error, .error = error};
    }
    // Conserva los permisos del original.
    const auto perms = fs::status(archive, ec).permissions();
    fs::permissions(temp, perms, ec);
    fs::rename(temp, archive, ec);
    if (ec) {
      fs::remove(temp, ec);
      return {.status = Status::Error, .error = ec.message()};
    }
    return {};
  }

  std::optional<std::string> zipComment(const std::string& archive) {
    std::ifstream in(archive, std::ios::binary | std::ios::ate);
    if (!in) {
      return std::nullopt;
    }
    const auto size = static_cast<std::int64_t>(in.tellg());
    // Registro final: 22 bytes + comentario de hasta 65535; se busca su firma desde el final.
    const std::int64_t span = std::min<std::int64_t>(size, 22 + 65535);
    std::string tail(static_cast<std::size_t>(span), '\0');
    in.seekg(size - span);
    in.read(tail.data(), span);
    for (std::int64_t i = span - 22; i >= 0; --i) {
      if (tail.compare(static_cast<std::size_t>(i), 4, "PK\x05\x06") == 0) {
        const auto lo = static_cast<unsigned char>(tail[static_cast<std::size_t>(i) + 20]);
        const auto hi = static_cast<unsigned char>(tail[static_cast<std::size_t>(i) + 21]);
        const std::size_t length = lo | (static_cast<std::size_t>(hi) << 8U);
        if (static_cast<std::int64_t>(i + 22 + length) != span) {
          continue; // falsa firma dentro de datos
        }
        return tail.substr(static_cast<std::size_t>(i) + 22, length);
      }
    }
    return std::nullopt;
  }

  bool setZipComment(const std::string& archive, const std::string& comment, std::string& error) {
    const auto current = zipComment(archive);
    if (!current) {
      error = "no es un zip";
      return false;
    }
    if (comment.size() > 65535) {
      error = "comentario demasiado largo";
      return false;
    }
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    const auto commentStart = size - current->size();
    // Truncar el comentario viejo, escribir el nuevo y su largo (2 bytes antes del comentario).
    fs::resize_file(archive, commentStart, ec);
    std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
    if (ec || !file) {
      error = ec ? ec.message() : "no se pudo abrir";
      return false;
    }
    const char length[2] = {
        static_cast<char>(comment.size() & 0xffU), static_cast<char>((comment.size() >> 8U) & 0xffU)
    };
    file.seekp(static_cast<std::streamoff>(commentStart) - 2);
    file.write(length, 2);
    file.write(comment.data(), static_cast<std::streamsize>(comment.size()));
    return static_cast<bool>(file);
  }

} // namespace okaru::archive
