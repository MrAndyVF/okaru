#include "apps/archive/archive_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "apps/files/drag_icon.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <gio/gio.h>
#include <libsecret/secret.h>
#include <linux/input-event-codes.h>
#include <set>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace okaru::archive {

  namespace {

    using i18n::tr;
    using i18n::trp;

    constexpr const char* kUriPrefix = "archive:/"; // URI de las filas de la vista (no es un esquema real)
    constexpr std::array<const char*, 5> kFormats{"zip", "7z", "tar.gz", "tar.xz", "tar.zst"};
    constexpr std::array<int, 4> kLevels{0, 1, 5, 9};
    constexpr std::array<std::uint64_t, 5> kVolumes{0, 100ULL << 20U, 700ULL << 20U, 2ULL << 30U, 4000ULL << 20U};

    const SecretSchema* keyringSchema() {
      static const SecretSchema schema = {
          "dev.okaru.Archive",
          SECRET_SCHEMA_NONE,
          {{"path", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
          0,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
          nullptr,
      };
      return &schema;
    }

    std::string baseName(const std::string& path) {
      const auto slash = path.rfind('/');
      return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string parentOf(const std::string& path) {
      const auto slash = path.rfind('/');
      return slash == std::string::npos ? std::string{} : path.substr(0, slash);
    }

    std::string join(const std::string& folder, const std::string& name) {
      return folder.empty() ? name : folder + "/" + name;
    }

    // "fotos.tar.gz" → "fotos".
    std::string stem(const std::string& name) {
      std::string lower = name;
      std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      for (const char* ext :
           {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tgz", ".txz", ".tbz2", ".zip", ".7z",   ".rar",
            ".tar",    ".iso",    ".cab",     ".gz",      ".xz",  ".bz2", ".zst",  ".lz",  ".lzma", ".z"}) {
        if (lower.ends_with(ext) && lower.size() > std::strlen(ext)) {
          return name.substr(0, name.size() - std::strlen(ext));
        }
      }
      return fs::path(name).stem().string();
    }

    std::string uniquePath(const fs::path& wanted) {
      std::error_code ec;
      if (!fs::exists(fs::symlink_status(wanted, ec))) {
        return wanted.string();
      }
      const std::string name = wanted.filename().string();
      const std::string base = stem(name);
      const std::string ext = name.substr(base.size());
      for (int i = 2;; ++i) {
        fs::path candidate = wanted.parent_path() / (base + " (" + std::to_string(i) + ")").append(ext);
        if (!fs::exists(fs::symlink_status(candidate, ec))) {
          return candidate.string();
        }
      }
    }

    files::FileEntry entryFor(const Item& item) {
      files::FileEntry e;
      e.name = baseName(item.path);
      e.display = e.name;
      e.uri = std::string(kUriPrefix) + item.path;
      e.directory = item.directory;
      e.size = item.size;
      e.modified = item.modified;
      e.mode = item.mode;
      if (item.directory) {
        e.contentType = "inode/directory";
      } else {
        gboolean uncertain = FALSE;
        gchar* type = g_content_type_guess(e.name.c_str(), nullptr, 0, &uncertain);
        e.contentType = type != nullptr ? type : "application/octet-stream";
        g_free(type);
      }
      if (gchar* description = g_content_type_get_description(e.contentType.c_str()); description != nullptr) {
        e.typeName = description;
        g_free(description);
      }
      if (GIcon* icon = g_content_type_get_icon(e.contentType.c_str()); icon != nullptr) {
        if (G_IS_THEMED_ICON(icon)) {
          for (const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon)); *names != nullptr; ++names) {
            e.icons.emplace_back(*names);
          }
        }
        g_object_unref(icon);
      }
      e.icons.emplace_back(item.directory ? "folder" : "text-x-generic");
      gchar* key = g_utf8_collate_key_for_filename(e.display.c_str(), -1);
      e.sortKey = key;
      g_free(key);
      return e;
    }

    void launchUri(const std::string& uri) {
      GError* error = nullptr;
      if (g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error) == FALSE) {
        g_clear_error(&error);
      }
    }

  } // namespace

  ArchiveWindow::ArchiveWindow(apps::OkaruApp& app, const std::string& archive, const std::vector<std::string>& create)
      : m_app(app) {
    m_app.setBuild([this](float width, float height, float scale) { return build(width, height, scale); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setReloadCallback([this] {
      if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
        m_icons.clear(*renderer);
      }
    });
    m_app.fileDrop().setOnDropAt([this](const std::vector<std::string>& paths, float, float, bool own) {
      dropAt(paths, own);
    });
    if (!archive.empty()) {
      DeferredCall::callLater([this, archive] { openArchive(archive); });
    } else if (!create.empty()) {
      DeferredCall::callLater([this, create] { createDialog(create); });
    }
  }

  ArchiveWindow::~ArchiveWindow() {
    if (m_cancel) {
      m_cancel->store(true);
    }
    if (!m_temp.empty()) {
      std::error_code ec;
      fs::remove_all(m_temp, ec);
    }
  }

  // ── Trabajo en un hilo ─────────────────────────────────────────────────────────────────────────────

  void ArchiveWindow::run(std::string label, const Work& work, const Done& done) {
    if (m_busy) {
      return;
    }
    m_busy = true;
    m_busyLabel = std::move(label);
    m_fraction = 0.0;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_retry = [this, label = m_busyLabel, work, done] { run(label, work, done); };
    m_app.requestRebuild();
    std::weak_ptr<int> alive = m_alive;
    const std::string password = m_password;
    std::thread([this, alive, work, done, password, cancel = m_cancel] {
      Progress progress{
          .update =
              [this, alive](double value) {
                m_fraction = value;
                // Un solo aviso en cola: el hilo puede informar miles de veces por segundo.
                if (!m_progressQueued.exchange(true)) {
                  DeferredCall::callLater([this, alive] {
                    if (alive.expired()) {
                      return;
                    }
                    m_progressQueued = false;
                    updateStatus();
                  });
                }
              },
          .cancel = cancel.get(),
      };
      Result result = work(password, progress);
      DeferredCall::callLater([this, alive, result, done] {
        if (alive.expired()) {
          return;
        }
        m_busy = false;
        finish(result);
        if (result.status == Status::NeedPassword || result.status == Status::WrongPassword) {
          m_passwordWrong = result.status == Status::WrongPassword;
          m_dialogText.clear();
          m_overlay = Overlay::Password;
          m_app.requestRebuild();
          return;
        }
        m_retry = nullptr;
        if (done) {
          done(result);
        }
        m_app.requestRebuild();
      });
    }).detach();
  }

  void ArchiveWindow::finish(const Result& result) {
    if (result.status == Status::Cancelled) {
      setMessage(tr("apps.archive.cancelled"));
    } else if (result.status == Status::Error) {
      setMessage(tr("apps.archive.failed", "detail", result.error), true);
    }
  }

  // ── Comprimido abierto ─────────────────────────────────────────────────────────────────────────────

  void ArchiveWindow::openArchive(const std::string& path) {
    std::error_code ec;
    m_path = fs::absolute(path, ec).string();
    m_password.clear();
    m_items.clear();
    m_entries.clear();
    m_folder.clear();
    m_state = {};
    m_loaded = false;
    m_password = keyringPassword();
    reload();
  }

  void ArchiveWindow::chooseArchive() {
    if (SessionBus* bus = m_app.sessionBus(); bus != nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
      m_chooser->open(tr("apps.archive.open-title"), {}, false, [this](std::vector<std::string> paths) {
        if (!paths.empty()) {
          openArchive(paths.front());
        }
      });
    }
  }

  void ArchiveWindow::reload() {
    auto items = std::make_shared<std::vector<Item>>();
    const std::string path = m_path;
    run(
        tr("apps.archive.loading"),
        [path, items](const std::string& password, const Progress&) { return list(path, password, *items); },
        [this, items](const Result& result) {
          if (!result.ok()) {
            return;
          }
          m_items = std::move(*items);
          m_encrypted = std::ranges::any_of(m_items, [](const Item& i) { return i.encrypted; }) || !m_password.empty();
          m_loaded = true;
          rememberPassword();
          // La carpeta actual pudo desaparecer (se borró o renombró).
          if (!m_folder.empty()
              && std::ranges::none_of(m_items, [&](const Item& i) { return i.directory && i.path == m_folder; })) {
            m_folder.clear();
          }
          refreshEntries();
        }
    );
  }

  void ArchiveWindow::refreshEntries() {
    m_entries.clear();
    for (const auto& item : m_items) {
      if (parentOf(item.path) == m_folder) {
        m_entries.push_back(entryFor(item));
      }
    }
    files::sortEntries(m_entries, m_options);
    // La selección y el cursor se quedan solo con lo que sigue visible.
    std::set<std::string> visible;
    for (const auto& e : m_entries) {
      visible.insert(e.uri);
    }
    std::erase_if(m_state.selected, [&](const std::string& uri) { return !visible.contains(uri); });
    if (m_state.cursor && *m_state.cursor >= m_entries.size()) {
      m_state.cursor.reset();
    }
    m_app.requestRebuild();
  }

  void ArchiveWindow::navigate(std::string folder) {
    const std::string previous = m_folder;
    m_folder = std::move(folder);
    m_state = {};
    // Al subir, el cursor queda en la carpeta de la que se salió.
    if (previous.starts_with(m_folder)) {
      m_state.revealUri = std::string(kUriPrefix)
          + (m_folder.empty() ? previous.substr(0, previous.find('/')) : join(m_folder, baseName(previous)));
    }
    refreshEntries();
  }

  void ArchiveWindow::goUp() {
    if (!m_folder.empty()) {
      navigate(parentOf(m_folder));
    }
  }

  void ArchiveWindow::activate(std::size_t index) {
    if (index >= m_entries.size()) {
      return;
    }
    const files::FileEntry& entry = m_entries[index];
    const std::string path = join(m_folder, entry.name);
    if (entry.directory) {
      navigate(path);
    } else {
      openItem(path);
    }
  }

  std::vector<std::string> ArchiveWindow::selectedPaths() const {
    std::vector<std::string> paths;
    for (const auto& e : m_entries) {
      if (m_state.selected.contains(e.uri)) {
        paths.push_back(join(m_folder, e.name));
      }
    }
    return paths;
  }

  bool ArchiveWindow::writable() const {
    if (m_path.empty() || writableFormat(m_path).empty()) {
      return false;
    }
    // Volúmenes (.7z.001) y archivos de solo lectura no se editan.
    return ::access(m_path.c_str(), W_OK) == 0;
  }

  std::string ArchiveWindow::tempDir() {
    if (m_temp.empty()) {
      const char* cache = std::getenv("XDG_CACHE_HOME");
      const char* home = std::getenv("HOME");
      const fs::path base =
          cache != nullptr && cache[0] != '\0' ? fs::path(cache) : fs::path(home != nullptr ? home : "/tmp") / ".cache";
      m_temp = (base / "okaru" / "archive" / std::to_string(::getpid())).string();
    }
    const std::string dir = m_temp + "/" + std::to_string(++m_tempCounter);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
  }

  void ArchiveWindow::openItem(const std::string& path) {
    const std::string dir = tempDir();
    const std::string archive = m_path;
    const std::string base = parentOf(path);
    run(
        tr("apps.archive.opening", "name", baseName(path)),
        [archive, path, base, dir](const std::string& password, const Progress& progress) {
          return extract(archive, password, {path}, base, dir, progress);
        },
        [dir, path](const Result& result) {
          if (result.ok()) {
            launchUri(files::pathToUri(dir + "/" + baseName(path)));
          }
        }
    );
  }

  // ── Extraer ────────────────────────────────────────────────────────────────────────────────────────

  void ArchiveWindow::extractDialog(bool selectionOnly) {
    if (!m_loaded) {
      return;
    }
    m_extractSelection = selectionOnly && !m_state.selected.empty();
    if (m_extractTo.empty()) {
      m_extractTo = fs::path(m_path).parent_path().string();
    }
    // Una sola cosa arriba del todo: ya trae su carpeta, no hace falta otra.
    std::size_t topLevel = 0;
    for (const auto& item : m_items) {
      topLevel += item.path.contains('/') ? 0 : 1;
    }
    m_extractOwnFolder = !m_extractSelection && topLevel > 1;
    m_overlay = Overlay::Extract;
    m_app.requestRebuild();
  }

  void ArchiveWindow::chooseExtractFolder() {
    if (SessionBus* bus = m_app.sessionBus(); bus != nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
      m_chooser->openFolder(tr("apps.archive.extract.choose"), [this](std::vector<std::string> paths) {
        if (!paths.empty()) {
          m_extractTo = paths.front();
          m_app.requestRebuild();
        }
      });
    }
  }

  void ArchiveWindow::startExtract(bool confirmed) {
    std::string destination = m_extractTo;
    if (m_extractOwnFolder) {
      destination = uniquePath(fs::path(destination) / stem(baseName(m_path)));
    }
    m_extractPaths = m_extractSelection ? selectedPaths() : std::vector<std::string>{};
    const std::string base = m_extractSelection ? m_folder : std::string{};
    // Lo que quedaría arriba en el destino; si ya existe, se pregunta antes de reemplazar.
    if (!confirmed) {
      m_collisions.clear();
      std::set<std::string> top;
      if (m_extractSelection) {
        for (const auto& path : m_extractPaths) {
          top.insert(baseName(path));
        }
      } else {
        for (const auto& item : m_items) {
          top.insert(item.path.substr(0, item.path.find('/')));
        }
      }
      std::error_code ec;
      for (const auto& name : top) {
        if (fs::exists(fs::symlink_status(fs::path(destination) / name, ec))) {
          m_collisions.push_back(name);
        }
      }
      if (!m_collisions.empty()) {
        m_overlay = Overlay::ConfirmReplace;
        m_app.requestRebuild();
        return;
      }
    }
    m_overlay = Overlay::None;
    const std::string archive = m_path;
    const std::vector<std::string> paths = m_extractPaths;
    run(
        tr("apps.archive.extracting", "name", baseName(m_path)),
        [archive, paths, base, destination](const std::string& password, const Progress& progress) {
          return extract(archive, password, paths, base, destination, progress);
        },
        [this, destination](const Result& result) {
          if (result.ok()) {
            setMessage(tr("apps.archive.extracted", "folder", destination));
            launchUri(files::pathToUri(destination)); // mostrar lo extraído en okaru-files
          }
        }
    );
  }

  // ── Editar ─────────────────────────────────────────────────────────────────────────────────────────

  void ArchiveWindow::applyEdit(const Edit& changes, const std::string& doneMessage) {
    if (!writable()) {
      setMessage(tr("apps.archive.read-only"), true);
      return;
    }
    const std::string archive = m_path;
    run(
        tr("apps.archive.updating"),
        [archive, changes](const std::string& password, const Progress& progress) {
          return edit(archive, password, changes, progress);
        },
        [this, doneMessage](const Result& result) {
          if (result.ok()) {
            setMessage(doneMessage);
            reload();
          }
        }
    );
  }

  void ArchiveWindow::addFiles(const std::vector<std::string>& files) {
    if (files.empty()) {
      return;
    }
    applyEdit({.addFiles = files, .addInto = m_folder}, trp("apps.archive.added", static_cast<long>(files.size())));
  }

  void ArchiveWindow::chooseFilesToAdd() {
    if (SessionBus* bus = m_app.sessionBus(); bus != nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
      m_chooser->open(tr("apps.archive.add-title"), {}, true, [this](const std::vector<std::string>& paths) {
        addFiles(paths);
      });
    }
  }

  void ArchiveWindow::dropAt(const std::vector<std::string>& paths, bool own) {
    if (own || paths.empty()) {
      return; // soltar lo arrastrado desde esta misma ventana no hace nada
    }
    std::vector<std::string> local;
    for (const auto& path : paths) {
      if (!path.empty() && path.front() == '/') {
        local.push_back(path);
      }
    }
    if (m_path.empty()) {
      // Sin comprimido abierto: uno soltado se abre; varios archivos, se comprimen.
      std::error_code ec;
      if (local.size() == 1 && fs::is_regular_file(local.front(), ec)) {
        openArchive(local.front());
      } else {
        createDialog(local);
      }
      return;
    }
    addFiles(local);
  }

  void ArchiveWindow::testArchive() {
    const std::string archive = m_path;
    run(
        tr("apps.archive.testing"),
        [archive](const std::string& password, const Progress& progress) { return test(archive, password, progress); },
        [this](const Result& result) {
          if (result.ok()) {
            setMessage(tr("apps.archive.test-ok"));
          }
        }
    );
  }

  // ── Arrastrar hacia afuera ─────────────────────────────────────────────────────────────────────────

  bool ArchiveWindow::startDrag(std::uint32_t serial) {
    const std::vector<std::string> paths = selectedPaths();
    if (paths.empty() || m_busy) {
      return false;
    }
    const std::string dir = tempDir();
    std::vector<std::string> uris;
    std::vector<std::string> images;
    const float scale = m_app.scale();
    for (const auto& e : m_entries) {
      if (!m_state.selected.contains(e.uri)) {
        continue;
      }
      uris.push_back(files::pathToUri(dir + "/" + e.name));
      if (images.size() < 3) {
        if (std::string icon = m_icons.pathFor(e, static_cast<int>(64.0F * scale)); !icon.empty()) {
          images.push_back(std::move(icon));
        }
      }
    }
    const apps::DragIcon icon = files::makeDragIcon(
        images, uris.size(), colorForRole(ColorRole::Primary), colorForRole(ColorRole::OnPrimary), scale
    );
    // Se extrae cuando el destino pide los datos (al soltar), no al empezar a arrastrar.
    const std::string archive = m_path;
    const std::string password = m_password;
    const std::string base = m_folder;
    return m_app.startDrag(std::move(uris), serial, icon, [archive, password, paths, base, dir] {
      (void)extract(archive, password, paths, base, dir, {});
    });
  }

  // ── Crear ──────────────────────────────────────────────────────────────────────────────────────────

  void ArchiveWindow::createDialog(std::vector<std::string> sources) {
    if (sources.empty()) {
      return;
    }
    m_createFrom = std::move(sources);
    const fs::path first(m_createFrom.front());
    m_dialogText = m_createFrom.size() == 1 ? stem(first.filename().string()) : first.parent_path().filename().string();
    m_createPassword.clear();
    m_overlay = Overlay::Create;
    m_app.requestRebuild();
  }

  void ArchiveWindow::chooseFilesToCreate() {
    if (SessionBus* bus = m_app.sessionBus(); bus != nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
      m_chooser->open(tr("apps.archive.create-choose"), {}, true, [this](std::vector<std::string> paths) {
        createDialog(std::move(paths));
      });
    }
  }

  void ArchiveWindow::startCreate() {
    std::string name = m_dialogText;
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) {
      name.pop_back();
    }
    if (name.empty() || name.contains('/') || m_createFrom.empty()) {
      return;
    }
    const fs::path folder = fs::path(m_createFrom.front()).parent_path();
    std::vector<std::string> names;
    names.reserve(m_createFrom.size());
    for (const auto& source : m_createFrom) {
      names.push_back(fs::path(source).filename().string());
    }
    const std::string format = kFormats[std::min(m_createFormat, kFormats.size() - 1)];
    const std::string output = uniquePath(folder / (name + "." + format));
    const CreateOptions options{
        .level = kLevels[std::min(m_createLevel, kLevels.size() - 1)],
        .password = format == "zip" || format == "7z" ? m_createPassword : std::string{},
        .volumeSize = kVolumes[std::min(m_createVolume, kVolumes.size() - 1)],
    };
    m_overlay = Overlay::None;
    m_password = options.password;
    run(
        tr("apps.archive.creating", "name", baseName(output)),
        [output, folder, names, options](const std::string&, const Progress& progress) {
          return create(output, folder.string(), names, options, progress);
        },
        [this, output, options](const Result& result) {
          if (!result.ok()) {
            return;
          }
          setMessage(tr("apps.archive.created", "name", baseName(output)));
          // Partido en volúmenes: se abre la primera parte.
          const std::string open = options.volumeSize > 0 ? output + ".001" : output;
          const std::string password = options.password;
          openArchive(open);
          if (m_password.empty()) {
            m_password = password;
          }
        }
    );
  }

  // ── Llavero ────────────────────────────────────────────────────────────────────────────────────────

  std::string ArchiveWindow::keyringPassword() const {
    gchar* secret = secret_password_lookup_sync(keyringSchema(), nullptr, nullptr, "path", m_path.c_str(), nullptr);
    std::string password = secret != nullptr ? secret : "";
    secret_password_free(secret);
    return password;
  }

  void ArchiveWindow::rememberPassword() {
    if (!m_rememberPassword || m_password.empty() || m_path.empty()) {
      return;
    }
    m_rememberPassword = false;
    const std::string label = "OKARU · " + baseName(m_path);
    (void)secret_password_store_sync(
        keyringSchema(), SECRET_COLLECTION_DEFAULT, label.c_str(), m_password.c_str(), nullptr, nullptr, "path",
        m_path.c_str(), nullptr
    );
  }

  // ── Mensajes y menús ───────────────────────────────────────────────────────────────────────────────

  void ArchiveWindow::setMessage(std::string message, bool error) {
    m_message = std::move(message);
    m_messageError = error;
    m_messageTimer.start(std::chrono::seconds(error ? 8 : 4), [this] {
      m_message.clear();
      updateStatus();
    });
    updateStatus();
  }

  void ArchiveWindow::updateStatus() {
    if (m_progressBar != nullptr) {
      m_progressBar->setVisible(m_busy);
      m_progressBar->setProgress(static_cast<float>(m_fraction.load()));
    }
    if (m_statusLabel != nullptr) {
      std::string text;
      if (m_busy) {
        text = m_busyLabel;
      } else if (!m_message.empty()) {
        text = m_message;
      } else if (m_loaded) {
        std::uint64_t bytes = 0;
        std::size_t filesCount = 0;
        for (const auto& item : m_items) {
          bytes += item.size;
          filesCount += item.directory ? 0 : 1;
        }
        text = trp("apps.archive.summary", static_cast<long>(filesCount), "size", files::formatSize(bytes));
        if (!m_state.selected.empty()) {
          text += " · " + trp("apps.archive.selected", static_cast<long>(m_state.selected.size()));
        }
        if (!writable()) {
          text += " · " + tr("apps.archive.read-only");
        }
      }
      m_statusLabel->setText(text.empty() ? " " : text);
      m_statusLabel->setColor(
          colorSpecFromRole(m_messageError && !m_busy ? ColorRole::Error : ColorRole::OnSurfaceVariant)
      );
    }
    m_app.invalidate();
  }

  void ArchiveWindow::showMenu(std::vector<MenuEntry> entries, float x, float y) {
    m_menu = std::move(entries);
    m_menuX = x;
    m_menuY = y;
    m_app.requestRebuild();
  }

  void ArchiveWindow::openContextMenu(std::optional<std::size_t> index, float x, float y) {
    const bool canEdit = writable();
    std::vector<MenuEntry> menu;
    if (index && !m_state.selected.empty()) {
      const std::vector<std::string> paths = selectedPaths();
      if (paths.size() == 1) {
        menu.push_back({tr("apps.archive.menu.open"), [this, i = *index] { activate(i); }});
      }
      menu.push_back({tr("apps.archive.menu.extract-selection"), [this] { extractDialog(true); }});
      menu.push_back({.separator = true});
      menu.push_back(
          {tr("apps.archive.menu.rename"),
           [this, paths] {
             m_renameFrom = paths.front();
             m_dialogText = baseName(paths.front());
             m_overlay = Overlay::Rename;
             m_app.requestRebuild();
           },
           canEdit && paths.size() == 1}
      );
      menu.push_back(
          {tr("apps.archive.menu.delete"),
           [this] {
             m_overlay = Overlay::ConfirmDelete;
             m_app.requestRebuild();
           },
           canEdit}
      );
    } else {
      menu.push_back({tr("apps.archive.menu.add"), [this] { chooseFilesToAdd(); }, canEdit});
      menu.push_back(
          {tr("apps.archive.menu.new-folder"),
           [this] {
             m_dialogText.clear();
             m_overlay = Overlay::NewFolder;
             m_app.requestRebuild();
           },
           canEdit}
      );
      menu.push_back({.separator = true});
      menu.push_back({tr("apps.archive.menu.select-all"), [this] {
                        if (m_view != nullptr) {
                          m_view->selectAll();
                        }
                      }});
    }
    showMenu(std::move(menu), x, y);
  }

  void ArchiveWindow::openMainMenu(float x, float y) {
    const bool open = m_loaded;
    std::vector<MenuEntry> menu;
    menu.push_back({tr("apps.archive.menu.open-archive"), [this] { chooseArchive(); }});
    menu.push_back({tr("apps.archive.menu.create"), [this] { chooseFilesToCreate(); }});
    menu.push_back({.separator = true});
    menu.push_back({tr("apps.archive.menu.extract"), [this] { extractDialog(false); }, open});
    menu.push_back({tr("apps.archive.menu.test"), [this] { testArchive(); }, open});
    menu.push_back(
        {tr("apps.archive.menu.comment"),
         [this] {
           m_comment = zipComment(m_path).value_or("");
           m_dialogText = m_comment;
           m_overlay = Overlay::Comment;
           m_app.requestRebuild();
         },
         open && writableFormat(m_path) == "zip" && writable()}
    );
    menu.push_back({tr("apps.archive.menu.properties"), [this] { showProperties(); }, open});
    menu.push_back(
        {tr("apps.archive.menu.show-folder"),
         [this] { launchUri(files::pathToUri(fs::path(m_path).parent_path().string())); }, open}
    );
    showMenu(std::move(menu), x, y);
  }

  void ArchiveWindow::showProperties() {
    m_overlay = Overlay::Properties;
    m_comment = zipComment(m_path).value_or("");
    m_app.requestRebuild();
  }

  // ── Escena ─────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> ArchiveWindow::build(float width, float height, float scale) {
    m_view = nullptr;
    m_dialogInput = nullptr;
    m_statusLabel = nullptr;
    m_progressBar = nullptr;
    m_menuControl = nullptr;
    m_width = width;
    m_icons.setAccent(colorForRole(ColorRole::Primary));

    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    auto main = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    Node* header = main->addChild(buildHeader(scale));
    m_app.setDragNode(header);

    if (!m_loaded) {
      main->addChild(buildWelcome(scale));
    } else {
      auto view = std::make_unique<files::FileView>();
      view->setScale(scale);
      view->setIconSize(32.0F);
      view->setSort(m_options.sort, m_options.descending);
      view->setEntries(&m_entries, &m_state);
      view->setEmptyText(tr("apps.archive.empty-folder"));
      view->setIconProvider([this](Renderer& renderer, const files::FileEntry& entry, int pixels) {
        return m_icons.get(renderer, entry, pixels);
      });
      view->setOnActivate([this](std::size_t index) { DeferredCall::callLater([this, index] { activate(index); }); });
      view->setOnSelectionChanged([this] { updateStatus(); });
      view->setOnContextMenu([this](std::optional<std::size_t> index, float x, float y) {
        DeferredCall::callLater([this, index, x, y] { openContextMenu(index, x, y); });
      });
      view->setOnSortChanged([this](files::SortKey key) {
        m_options.descending = m_options.sort == key ? !m_options.descending : false;
        m_options.sort = key;
        refreshEntries();
      });
      view->setOnBack([this](bool forward) {
        if (!forward) {
          goUp();
        }
      });
      view->setOnDragStart([this](const std::vector<std::string>&, std::uint32_t serial) { return startDrag(serial); });
      view->setFlexGrow(1.0F);
      m_view = view.get();
      main->addChild(std::move(view));
    }
    main->addChild(buildStatus(scale));
    root->addChild(std::move(main));
    if (m_overlay != Overlay::None) {
      root->addChild(buildOverlay(width, height, scale));
    }
    if (!m_menu.empty()) {
      root->addChild(buildMenu(width, height, scale));
    }
    m_app.setAfterBuild([this] {
      if (m_dialogInput != nullptr) {
        m_app.focus(m_dialogInput->inputArea());
      } else if (m_view != nullptr) {
        m_app.focus(m_view->inputArea());
      }
    });
    return root;
  }

  std::unique_ptr<Node> ArchiveWindow::buildHeader(float scale) {
    const auto ghost = [](const char* glyph, std::string tooltip, std::function<void()> onClick, bool enabled = true) {
      return ui::button({
          .glyph = glyph,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .height = 52.0F * scale,
    });
    header->addChild(ghost("folder-open", tr("apps.archive.menu.open-archive"), [this] { chooseArchive(); }));
    header->addChild(ghost("arrow-up", tr("apps.archive.up"), [this] { goUp(); }, m_loaded && !m_folder.empty()));
    std::string title = tr("apps.archive.title");
    if (!m_path.empty()) {
      title = baseName(m_path) + (m_folder.empty() ? "" : "  ›  " + m_folder);
    }
    header->addChild(
        ui::glyph(
            {.glyph = m_encrypted ? "lock" : "file-zip",
             .glyphSize = 18.0F * scale,
             .color = colorSpecFromRole(ColorRole::Primary)}
        )
    );
    header->addChild(
        ui::label(
            {.text = std::move(title),
             .fontSize = Style::fontSizeBody * scale,
             .fontWeight = FontWeight::SemiBold,
             .maxLines = 1,
             .ellipsize = TextEllipsize::Middle,
             .flexGrow = 1.0F}
        )
    );
    if (m_loaded) {
      const bool canEdit = writable();
      header->addChild(ghost("plus", tr("apps.archive.menu.add"), [this] { chooseFilesToAdd(); }, canEdit && !m_busy));
      header->addChild(ghost(
          "folder-plus", tr("apps.archive.menu.new-folder"),
          [this] {
            m_dialogText.clear();
            m_overlay = Overlay::NewFolder;
            m_app.requestRebuild();
          },
          canEdit && !m_busy
      ));
      header->addChild(
          ui::button({
              .text = tr("apps.archive.extract.action"),
              .glyph = "package-export",
              .enabled = !m_busy,
              .variant = ButtonVariant::Primary,
              .onClick = [this] { extractDialog(!m_state.selected.empty()); },
          })
      );
    }
    header->addChild(
        ui::button({
            .glyph = "dots-vertical",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.archive.menu.title"),
            .onClick = [this, scale] { openMainMenu(m_width - 260.0F * scale, 48.0F * scale); },
        })
    );
    return header;
  }

  std::unique_ptr<Node> ArchiveWindow::buildWelcome(float scale) {
    auto column = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceMd * scale,
        .flexGrow = 1.0F,
    });
    column->addChild(
        ui::glyph({.glyph = "file-zip", .glyphSize = 64.0F * scale, .color = colorSpecFromRole(ColorRole::Primary)})
    );
    column->addChild(
        ui::label(
            {.text = tr("apps.archive.welcome"),
             .fontSize = Style::fontSizeTitle * scale,
             .fontWeight = FontWeight::SemiBold}
        )
    );
    column->addChild(
        ui::label(
            {.text = tr("apps.archive.welcome-hint"),
             .fontSize = Style::fontSizeBody * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        )
    );
    column->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::button(
                {.text = tr("apps.archive.menu.open-archive"),
                 .glyph = "folder-open",
                 .variant = ButtonVariant::Primary,
                 .onClick = [this] { chooseArchive(); }}
            ),
            ui::button(
                {.text = tr("apps.archive.menu.create"),
                 .glyph = "file-plus",
                 .variant = ButtonVariant::Default,
                 .onClick = [this] { chooseFilesToCreate(); }}
            )
        )
    );
    return column;
  }

  std::unique_ptr<Node> ArchiveWindow::buildStatus(float scale) {
    auto bar = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .height = 30.0F * scale,
    });
    bar->addChild(
        ui::label({
            .out = &m_statusLabel,
            .text = " ",
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .flexGrow = 1.0F,
        })
    );
    bar->addChild(
        ui::progressBar({
            .out = &m_progressBar,
            .fill = colorSpecFromRole(ColorRole::Primary),
            .track = colorSpecFromRole(ColorRole::OnSurface, 0.1F),
            .radius = 2.0F * scale,
            .width = 160.0F * scale,
            .height = 4.0F * scale,
            .visible = false,
        })
    );
    if (m_busy) {
      bar->addChild(
          ui::button({
              .glyph = "x",
              .glyphSize = 12.0F * scale,
              .controlHeight = 22.0F * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = tr("apps.archive.cancel"),
              .onClick = [this] {
                if (m_cancel) {
                  m_cancel->store(true);
                }
              },
          })
      );
    }
    updateStatus();
    return bar;
  }

  std::unique_ptr<Node> ArchiveWindow::buildMenu(float width, float height, float scale) {
    auto layer = ui::node({.width = width, .height = height, .zIndex = 20});
    layer->addChild(
        ui::inputArea({
            .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}),
            .width = width,
            .height = height,
            .onPress = [this](const InputArea::PointerData&) {
              DeferredCall::callLater([this] {
                m_menu.clear();
                m_app.requestRebuild();
              });
            },
        })
    );
    std::vector<ContextMenuControlEntry> entries;
    entries.reserve(m_menu.size());
    for (std::size_t i = 0; i < m_menu.size(); ++i) {
      entries.push_back({
          .id = static_cast<std::int32_t>(i),
          .label = m_menu[i].label,
          .enabled = m_menu[i].enabled,
          .separator = m_menu[i].separator,
      });
    }
    Renderer* renderer = m_app.renderer();
    const float menuWidth = std::clamp(
        renderer != nullptr ? ContextMenuControl::preferredWidth(*renderer, entries, scale, false) : 240.0F * scale,
        200.0F * scale, 360.0F * scale
    );
    const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size(), scale);
    const float x = std::clamp(m_menuX, 4.0F, std::max(4.0F, width - menuWidth - 4.0F));
    const float y = std::clamp(m_menuY, 4.0F, std::max(4.0F, height - menuHeight - 4.0F));
    layer->addChild(
        ui::box({
            .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
            .border = colorSpecFromRole(ColorRole::OutlineVariant),
            .borderWidth = 1.0F,
            .radius = Style::radiusLg * scale,
            .width = menuWidth,
            .height = menuHeight,
            .configure = [x, y](Box& box) { box.setPosition(x, y); },
        })
    );
    auto menu = std::make_unique<ContextMenuControl>();
    menu->setContentScale(scale);
    menu->setMenuWidth(menuWidth);
    menu->setMaxVisible(entries.size());
    menu->setEntries(std::move(entries));
    menu->setRedrawCallback([this] { m_app.invalidate(); });
    menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
      const auto index = static_cast<std::size_t>(entry.id);
      DeferredCall::callLater([this, index] {
        if (index >= m_menu.size()) {
          return;
        }
        auto action = m_menu[index].action;
        m_menu.clear();
        m_app.requestRebuild();
        if (action) {
          action();
        }
      });
    });
    menu->setPosition(x, y);
    menu->setSize(menuWidth, menuHeight);
    m_menuControl = menu.get();
    layer->addChild(std::move(menu));
    return layer;
  }

  std::unique_ptr<Node> ArchiveWindow::buildOverlay(float width, float height, float scale) {
    auto overlay = ui::node({.width = width, .height = height, .zIndex = 10});
    overlay->addChild(ui::inputArea({.width = width, .height = height}));
    overlay->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Scrim, 0.45F), .width = width, .height = height}));
    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .radius = Style::radiusLg * scale,
        .width = std::min(500.0F * scale, width - 32.0F),
    });
    const auto heading = [&](std::string text) {
      card->addChild(
          ui::label(
              {.text = std::move(text),
               .fontSize = Style::fontSizeTitle * scale,
               .fontWeight = FontWeight::SemiBold,
               .maxLines = 2,
               .ellipsize = TextEllipsize::Middle}
          )
      );
    };
    const auto body = [&](std::string text, ColorRole role = ColorRole::OnSurfaceVariant) {
      card->addChild(
          ui::label(
              {.text = std::move(text),
               .fontSize = Style::fontSizeBody * scale,
               .color = colorSpecFromRole(role),
               .maxLines = 8}
          )
      );
    };
    const auto caption = [&](std::string text) {
      card->addChild(
          ui::label(
              {.text = std::move(text),
               .fontSize = Style::fontSizeCaption * scale,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          )
      );
    };
    const auto close = [this] {
      DeferredCall::callLater([this] {
        m_overlay = Overlay::None;
        m_retry = nullptr;
        m_app.requestRebuild();
      });
    };
    const auto buttons = [&](auto... children) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
              std::move(children)...
          )
      );
    };
    const auto cancelButton = [&] {
      return ui::button({.text = tr("apps.archive.cancel"), .variant = ButtonVariant::Ghost, .onClick = close});
    };
    const auto textField = [&](std::string placeholder, std::string& value, bool password,
                               const std::function<void()>& submit) {
      card->addChild(
          ui::input({
              .out = &m_dialogInput,
              .value = value,
              .placeholder = std::move(placeholder),
              .fontSize = Style::fontSizeBody * scale,
              .passwordMode = password,
              .onChange = [&value](const std::string& v) { value = v; },
              .onSubmit = [submit](const std::string&) { submit(); },
          })
      );
    };
    const auto checkRow = [&](std::string label, bool checked, std::function<void(bool)> onChange) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
              ui::checkbox({.checked = checked, .scale = scale, .onChange = std::move(onChange)}),
              ui::label({.text = std::move(label), .fontSize = Style::fontSizeBody * scale})
          )
      );
    };
    const auto segmented = [&](std::vector<std::string> labels, std::size_t selected,
                               std::function<void(std::size_t)> onChange) {
      std::vector<ui::SegmentedOption> options;
      options.reserve(labels.size());
      for (auto& label : labels) {
        options.push_back({.label = std::move(label)});
      }
      card->addChild(
          ui::segmented({
              .options = std::move(options),
              .selectedIndex = selected,
              .fontSize = Style::fontSizeCaption * scale,
              .scale = scale,
              .onChange = std::move(onChange),
          })
      );
    };
    const auto info = [&](std::string name, std::string value) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Start, .gap = Style::spaceMd * scale},
              ui::label(
                  {.text = std::move(name),
                   .fontSize = Style::fontSizeCaption * scale,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                   .width = 140.0F * scale}
              ),
              ui::label(
                  {.text = std::move(value),
                   .fontSize = Style::fontSizeCaption * scale,
                   .maxLines = 4,
                   .flexGrow = 1.0F}
              )
          )
      );
    };

    switch (m_overlay) {
    case Overlay::Password: {
      heading(tr("apps.archive.password.title"));
      body(
          m_passwordWrong ? tr("apps.archive.password.wrong")
                          : tr("apps.archive.password.body", "name", baseName(m_path)),
          m_passwordWrong ? ColorRole::Error : ColorRole::OnSurfaceVariant
      );
      const auto submit = [this] {
        DeferredCall::callLater([this] {
          if (m_dialogText.empty()) {
            return;
          }
          m_password = m_dialogText;
          m_overlay = Overlay::None;
          if (auto retry = m_retry; retry) {
            retry();
          }
        });
      };
      textField(tr("apps.archive.password.placeholder"), m_dialogText, true, submit);
      checkRow(tr("apps.archive.password.remember"), m_rememberPassword, [this](bool value) {
        m_rememberPassword = value;
      });
      buttons(
          cancelButton(),
          ui::button({.text = tr("apps.archive.password.unlock"), .variant = ButtonVariant::Primary, .onClick = submit})
      );
      break;
    }
    case Overlay::Extract: {
      heading(
          m_extractSelection ? trp("apps.archive.extract.title-selection", static_cast<long>(m_state.selected.size()))
                             : tr("apps.archive.extract.title")
      );
      caption(tr("apps.archive.extract.destination"));
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
              ui::label(
                  {.text = m_extractTo,
                   .fontSize = Style::fontSizeBody * scale,
                   .maxLines = 1,
                   .ellipsize = TextEllipsize::Start,
                   .flexGrow = 1.0F}
              ),
              ui::button(
                  {.text = tr("apps.archive.extract.choose"), .variant = ButtonVariant::Default, .onClick = [this] {
                     DeferredCall::callLater([this] { chooseExtractFolder(); });
                   }}
              )
          )
      );
      checkRow(
          tr("apps.archive.extract.own-folder", "name", stem(baseName(m_path))), m_extractOwnFolder,
          [this](bool value) { m_extractOwnFolder = value; }
      );
      if (!m_state.selected.empty()) {
        checkRow(tr("apps.archive.extract.only-selection"), m_extractSelection, [this](bool value) {
          m_extractSelection = value;
          m_app.requestRebuild();
        });
      }
      buttons(
          cancelButton(),
          ui::button({.text = tr("apps.archive.extract.action"), .variant = ButtonVariant::Primary, .onClick = [this] {
                        DeferredCall::callLater([this] { startExtract(false); });
                      }})
      );
      break;
    }
    case Overlay::ConfirmReplace: {
      heading(tr("apps.archive.replace.title"));
      std::string names;
      for (std::size_t i = 0; i < m_collisions.size() && i < 5; ++i) {
        names += (i == 0 ? "" : ", ") + m_collisions[i];
      }
      if (m_collisions.size() > 5) {
        names += "…";
      }
      body(trp("apps.archive.replace.body", static_cast<long>(m_collisions.size()), "names", names));
      buttons(
          cancelButton(),
          ui::button({.text = tr("apps.archive.replace.action"), .variant = ButtonVariant::Primary, .onClick = [this] {
                        DeferredCall::callLater([this] { startExtract(true); });
                      }})
      );
      break;
    }
    case Overlay::NewFolder:
    case Overlay::Rename: {
      const bool rename = m_overlay == Overlay::Rename;
      heading(tr(rename ? "apps.archive.rename.title" : "apps.archive.new-folder.title"));
      const auto submit = [this, rename] {
        DeferredCall::callLater([this, rename] {
          const std::string name = m_dialogText;
          if (name.empty() || name.contains('/') || name == "." || name == "..") {
            return;
          }
          m_overlay = Overlay::None;
          if (rename) {
            if (name != baseName(m_renameFrom)) {
              applyEdit({.rename = {{m_renameFrom, join(parentOf(m_renameFrom), name)}}}, tr("apps.archive.renamed"));
            } else {
              m_app.requestRebuild();
            }
          } else {
            applyEdit({.folders = {join(m_folder, name)}}, tr("apps.archive.folder-created"));
          }
        });
      };
      textField(tr("apps.archive.name"), m_dialogText, false, submit);
      buttons(
          cancelButton(),
          ui::button(
              {.text = tr(rename ? "apps.archive.rename.action" : "apps.archive.new-folder.action"),
               .variant = ButtonVariant::Primary,
               .onClick = submit}
          )
      );
      break;
    }
    case Overlay::ConfirmDelete: {
      const std::vector<std::string> paths = selectedPaths();
      heading(trp("apps.archive.delete.title", static_cast<long>(paths.size())));
      body(tr("apps.archive.delete.body"));
      buttons(
          cancelButton(),
          ui::button(
              {.text = tr("apps.archive.delete.action"),
               .variant = ButtonVariant::Destructive,
               .onClick = [this, paths] {
                 DeferredCall::callLater([this, paths] {
                   m_overlay = Overlay::None;
                   applyEdit({.remove = paths}, trp("apps.archive.deleted", static_cast<long>(paths.size())));
                 });
               }}
          )
      );
      break;
    }
    case Overlay::Comment: {
      heading(tr("apps.archive.comment.title"));
      const auto submit = [this] {
        DeferredCall::callLater([this] {
          std::string error;
          m_overlay = Overlay::None;
          if (setZipComment(m_path, m_dialogText, error)) {
            setMessage(tr("apps.archive.comment.saved"));
          } else {
            setMessage(tr("apps.archive.failed", "detail", error), true);
          }
          m_app.requestRebuild();
        });
      };
      textField(tr("apps.archive.comment.placeholder"), m_dialogText, false, submit);
      buttons(
          cancelButton(),
          ui::button({.text = tr("apps.archive.comment.save"), .variant = ButtonVariant::Primary, .onClick = submit})
      );
      break;
    }
    case Overlay::Properties: {
      heading(baseName(m_path));
      std::uint64_t bytes = 0;
      std::size_t fileCount = 0;
      std::size_t folderCount = 0;
      for (const auto& item : m_items) {
        bytes += item.size;
        (item.directory ? folderCount : fileCount) += 1;
      }
      std::error_code ec;
      const std::uint64_t packed = fs::file_size(m_path, ec);
      const std::string format = writableFormat(m_path);
      info(tr("apps.archive.props.location"), fs::path(m_path).parent_path().string());
      info(
          tr("apps.archive.props.format"),
          format.empty() ? fs::path(m_path).extension().string().substr(
                               std::min<std::size_t>(1, fs::path(m_path).extension().string().size())
                           )
                         : format
      );
      info(
          tr("apps.archive.props.contents"),
          trp("apps.archive.props.files", static_cast<long>(fileCount))
              + " · "
              + trp("apps.archive.props.folders", static_cast<long>(folderCount))
      );
      info(tr("apps.archive.props.size"), files::formatSize(bytes));
      info(tr("apps.archive.props.packed"), files::formatSize(packed));
      if (bytes > 0) {
        info(
            tr("apps.archive.props.ratio"),
            std::to_string(static_cast<int>(100.0 * static_cast<double>(packed) / static_cast<double>(bytes))) + " %"
        );
      }
      info(tr("apps.archive.props.encrypted"), tr(m_encrypted ? "apps.archive.yes" : "apps.archive.no"));
      if (!m_comment.empty()) {
        info(tr("apps.archive.props.comment"), m_comment);
      }
      buttons(ui::button({.text = tr("apps.archive.close"), .variant = ButtonVariant::Primary, .onClick = close}));
      break;
    }
    case Overlay::Create: {
      heading(trp("apps.archive.create.title", static_cast<long>(m_createFrom.size())));
      textField(tr("apps.archive.name"), m_dialogText, false, [this] {
        DeferredCall::callLater([this] { startCreate(); });
      });
      caption(tr("apps.archive.create.format"));
      segmented({".zip", ".7z", ".tar.gz", ".tar.xz", ".tar.zst"}, m_createFormat, [this](std::size_t index) {
        m_createFormat = index;
        m_app.requestRebuild();
      });
      caption(tr("apps.archive.create.level"));
      segmented(
          {tr("apps.archive.create.level-store"), tr("apps.archive.create.level-fast"),
           tr("apps.archive.create.level-normal"), tr("apps.archive.create.level-max")},
          m_createLevel, [this](std::size_t index) { m_createLevel = index; }
      );
      if (m_createFormat <= 1) {
        caption(tr("apps.archive.create.password"));
        card->addChild(
            ui::input({
                .value = m_createPassword,
                .placeholder = tr("apps.archive.create.password-placeholder"),
                .fontSize = Style::fontSizeBody * scale,
                .passwordMode = true,
                .onChange = [this](const std::string& v) { m_createPassword = v; },
            })
        );
      }
      caption(tr("apps.archive.create.volumes"));
      segmented(
          {tr("apps.archive.create.volume-none"), "100 MB", "700 MB", "2 GB", "4 GB"}, m_createVolume,
          [this](std::size_t index) { m_createVolume = index; }
      );
      buttons(
          cancelButton(),
          ui::button({.text = tr("apps.archive.create.action"), .variant = ButtonVariant::Primary, .onClick = [this] {
                        DeferredCall::callLater([this] { startCreate(); });
                      }})
      );
      break;
    }
    case Overlay::None:
      break;
    }

    auto center =
        ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .width = width, .height = height});
    center->addChild(std::move(card));
    overlay->addChild(std::move(center));
    return overlay;
  }

  // ── Teclado ────────────────────────────────────────────────────────────────────────────────────────

  bool ArchiveWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed) {
      return false;
    }
    const std::uint32_t mods = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt);
    const bool ctrl = (mods & KeyMod::Ctrl) != 0;
    const std::uint32_t sym = event.sym;
    if (!m_menu.empty()) {
      if (sym == XKB_KEY_Escape) {
        m_menu.clear();
        m_app.requestRebuild();
      } else if (m_menuControl != nullptr && (sym == XKB_KEY_Down || sym == XKB_KEY_Up)) {
        (void)m_menuControl->moveHighlight(sym == XKB_KEY_Down ? 1 : -1);
        m_app.invalidate();
      } else if (m_menuControl != nullptr && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
        (void)m_menuControl->activateHighlighted();
      }
      return true;
    }
    if (m_overlay != Overlay::None) {
      if (sym == XKB_KEY_Escape) {
        m_overlay = Overlay::None;
        m_retry = nullptr;
        m_app.requestRebuild();
        return true;
      }
      return false;
    }
    if (ctrl && sym == XKB_KEY_o) {
      chooseArchive();
      return true;
    }
    if (ctrl && sym == XKB_KEY_n) {
      chooseFilesToCreate();
      return true;
    }
    if (!m_loaded || m_busy) {
      if (sym == XKB_KEY_Escape && m_busy && m_cancel) {
        m_cancel->store(true);
        return true;
      }
      return false;
    }
    if (ctrl && sym == XKB_KEY_e) {
      extractDialog(!m_state.selected.empty());
      return true;
    }
    if ((sym == XKB_KEY_BackSpace && mods == 0) || (sym == XKB_KEY_Up && (mods & KeyMod::Alt) != 0)) {
      goUp();
      return true;
    }
    if (sym == XKB_KEY_Delete && writable() && !m_state.selected.empty()) {
      m_overlay = Overlay::ConfirmDelete;
      m_app.requestRebuild();
      return true;
    }
    if (sym == XKB_KEY_F2 && writable()) {
      const auto paths = selectedPaths();
      if (paths.size() == 1) {
        m_renameFrom = paths.front();
        m_dialogText = baseName(paths.front());
        m_overlay = Overlay::Rename;
        m_app.requestRebuild();
      }
      return true;
    }
    if (ctrl && sym == XKB_KEY_a && m_view != nullptr) {
      m_view->selectAll();
      return true;
    }
    return false;
  }

} // namespace okaru::archive
