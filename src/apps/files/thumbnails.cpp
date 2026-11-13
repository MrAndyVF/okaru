#include "apps/files/thumbnails.h"

#include "apps/files/file_model.h"
#include "core/deferred_call.h"
#include "core/process/process.h"

#include <algorithm>
#include <cairo.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <poppler.h>
#include <sstream>
#include <unistd.h>

namespace okaru::files {

  namespace {

    namespace fs = std::filesystem;

    constexpr int kNormal = 128;
    constexpr int kLarge = 256;
    constexpr std::uint64_t kMaxImageBytes = 200ULL * 1024 * 1024; // más grande: el icono basta
    constexpr const char* kFailDir = "okaru-files";

    fs::path cacheRoot() { return fs::path(g_get_user_cache_dir()) / "thumbnails"; }

    std::string md5(const std::string& text) {
      gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, text.c_str(), -1);
      std::string out = sum;
      g_free(sum);
      return out;
    }

    fs::path failPath(const std::string& uri) { return cacheRoot() / "fail" / kFailDir / (md5(uri) + ".png"); }

    // Guarda con las etiquetas de la especificación, en un temporal que luego se renombra (atómico).
    bool saveThumbnail(GdkPixbuf* pixbuf, const fs::path& output, const std::string& uri, std::int64_t modified) {
      std::error_code ec;
      fs::create_directories(output.parent_path(), ec);
      fs::permissions(output.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
      const std::string temp = output.string() + "." + std::to_string(::getpid()) + ".tmp";
      const std::string mtime = std::to_string(modified);
      GError* error = nullptr;
      const gboolean ok = gdk_pixbuf_save(
          pixbuf, temp.c_str(), "png", &error, "tEXt::Thumb::URI", uri.c_str(), "tEXt::Thumb::MTime", mtime.c_str(),
          "tEXt::Software", "OKARU Files", nullptr
      );
      g_clear_error(&error);
      if (ok == FALSE) {
        fs::remove(temp, ec);
        return false;
      }
      fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
      fs::rename(temp, output, ec);
      return !ec;
    }

    // Escala para que el lado mayor quepa en `size` (sin agrandar).
    GdkPixbuf* scaleToFit(GdkPixbuf* source, int size) {
      const int w = gdk_pixbuf_get_width(source);
      const int h = gdk_pixbuf_get_height(source);
      if (w <= size && h <= size) {
        return GDK_PIXBUF(g_object_ref(source));
      }
      const double ratio = static_cast<double>(size) / std::max(w, h);
      return gdk_pixbuf_scale_simple(source, std::max(1, static_cast<int>(w * ratio)),
                                     std::max(1, static_cast<int>(h * ratio)), GDK_INTERP_BILINEAR);
    }

    GdkPixbuf* imageThumbnail(const std::string& path, int size) {
      GError* error = nullptr;
      GdkPixbuf* loaded = gdk_pixbuf_new_from_file_at_size(path.c_str(), size, size, &error);
      g_clear_error(&error);
      if (loaded == nullptr) {
        return nullptr;
      }
      // Fotos del móvil: respetar la orientación EXIF.
      GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(loaded);
      g_object_unref(loaded);
      return oriented;
    }

    GdkPixbuf* pdfThumbnail(const std::string& uri, int size) {
      GError* error = nullptr;
      PopplerDocument* doc = poppler_document_new_from_file(uri.c_str(), nullptr, &error);
      g_clear_error(&error);
      if (doc == nullptr) {
        return nullptr;
      }
      GdkPixbuf* out = nullptr;
      if (PopplerPage* page = poppler_document_get_page(doc, 0); page != nullptr) {
        double pw = 0;
        double ph = 0;
        poppler_page_get_size(page, &pw, &ph);
        const double scale = size / std::max({pw, ph, 1.0});
        const int w = std::max(1, static_cast<int>(pw * scale));
        const int h = std::max(1, static_cast<int>(ph * scale));
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        poppler_page_render(page, cr);
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        // ARGB32 de cairo (BGRA premultiplicado, papel opaco) → RGB de gdk-pixbuf.
        out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
        const unsigned char* src = cairo_image_surface_get_data(surface);
        const int srcStride = cairo_image_surface_get_stride(surface);
        unsigned char* dst = gdk_pixbuf_get_pixels(out);
        const int dstStride = gdk_pixbuf_get_rowstride(out);
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            const unsigned char* p = src + y * srcStride + x * 4;
            unsigned char* q = dst + y * dstStride + x * 3;
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
          }
        }
        cairo_surface_destroy(surface);
        g_object_unref(page);
      }
      g_object_unref(doc);
      return out;
    }

    bool pixbufHandles(const std::string& contentType) {
      static const std::unordered_set<std::string> types = [] {
        std::unordered_set<std::string> out;
        GSList* formats = gdk_pixbuf_get_formats();
        for (GSList* l = formats; l != nullptr; l = l->next) {
          auto* format = static_cast<GdkPixbufFormat*>(l->data);
          if (gdk_pixbuf_format_is_disabled(format) != FALSE) {
            continue;
          }
          gchar** mimes = gdk_pixbuf_format_get_mime_types(format);
          for (gchar** m = mimes; m != nullptr && *m != nullptr; ++m) {
            out.insert(*m);
          }
          g_strfreev(mimes);
        }
        g_slist_free(formats);
        return out;
      }();
      return types.contains(contentType);
    }

  } // namespace

  std::string thumbnailPath(const std::string& uri, bool large) {
    return (cacheRoot() / (large ? "large" : "normal") / (md5(uri) + ".png")).string();
  }

  bool thumbnailValid(const std::string& path, const std::string& uri, std::int64_t modified) {
    if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
      return false;
    }
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), nullptr);
    if (pixbuf == nullptr) {
      return false;
    }
    const char* thumbUri = gdk_pixbuf_get_option(pixbuf, "tEXt::Thumb::URI");
    const char* thumbTime = gdk_pixbuf_get_option(pixbuf, "tEXt::Thumb::MTime");
    const bool ok = thumbUri != nullptr && uri == thumbUri && thumbTime != nullptr
        && std::to_string(modified) == thumbTime;
    g_object_unref(pixbuf);
    return ok;
  }

  std::optional<ExternalThumbnailer> parseThumbnailer(const std::string& contents) {
    GKeyFile* file = g_key_file_new();
    if (g_key_file_load_from_data(file, contents.c_str(), contents.size(), G_KEY_FILE_NONE, nullptr) == FALSE) {
      g_key_file_free(file);
      return std::nullopt;
    }
    ExternalThumbnailer out;
    gchar* exec = g_key_file_get_string(file, "Thumbnailer Entry", "Exec", nullptr);
    gsize count = 0;
    gchar** mimes = g_key_file_get_string_list(file, "Thumbnailer Entry", "MimeType", &count, nullptr);
    if (exec != nullptr) {
      out.exec = exec;
    }
    for (gsize i = 0; mimes != nullptr && i < count; ++i) {
      if (*mimes[i] != '\0') {
        out.mimeTypes.emplace_back(mimes[i]);
      }
    }
    g_free(exec);
    g_strfreev(mimes);
    g_key_file_free(file);
    if (out.exec.empty() || out.mimeTypes.empty()) {
      return std::nullopt;
    }
    return out;
  }

  std::vector<std::string> expandThumbnailerExec(const std::string& exec, const std::string& path,
                                                 const std::string& uri, const std::string& output, int size) {
    std::vector<std::string> out;
    gint argc = 0;
    gchar** argv = nullptr;
    if (g_shell_parse_argv(exec.c_str(), &argc, &argv, nullptr) == FALSE) {
      return out;
    }
    for (gint i = 0; i < argc; ++i) {
      std::string arg = argv[i];
      std::string expanded;
      for (std::size_t k = 0; k < arg.size(); ++k) {
        if (arg[k] == '%' && k + 1 < arg.size()) {
          const char code = arg[++k];
          expanded += code == 'i' ? path
              : code == 'u'       ? uri
              : code == 'o'       ? output
              : code == 's'       ? std::to_string(size)
              : code == '%'       ? std::string("%")
                                  : std::string();
        } else {
          expanded += arg[k];
        }
      }
      out.push_back(std::move(expanded));
    }
    g_strfreev(argv);
    return out;
  }

  // ── Thumbnailer ────────────────────────────────────────────────────────────────────────────────────

  Thumbnailer::Thumbnailer() {
    loadExternal();
    const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 2U, 4U);
    for (unsigned i = 0; i < count; ++i) {
      m_threads.emplace_back([this] { worker(); });
    }
  }

  Thumbnailer::~Thumbnailer() {
    {
      std::scoped_lock lock(m_mutex);
      m_quit = true;
      m_queue.clear();
    }
    m_wake.notify_all();
    m_alive.reset();
    for (auto& thread : m_threads) {
      thread.join();
    }
  }

  void Thumbnailer::loadExternal() {
    // Los de la cuenta primero (ganan sobre los del sistema), como dice la especificación.
    std::vector<fs::path> dirs{fs::path(g_get_user_data_dir()) / "thumbnailers"};
    for (const gchar* const* d = g_get_system_data_dirs(); *d != nullptr; ++d) {
      dirs.emplace_back(fs::path(*d) / "thumbnailers");
    }
    std::error_code ec;
    for (const auto& dir : dirs) {
      for (const auto& item : fs::directory_iterator(dir, ec)) {
        if (item.path().extension() != ".thumbnailer") {
          continue;
        }
        std::ifstream in(item.path());
        std::stringstream contents;
        contents << in.rdbuf();
        if (auto parsed = parseThumbnailer(contents.str())) {
          m_external.push_back(std::move(*parsed));
        }
      }
    }
  }

  const ExternalThumbnailer* Thumbnailer::externalFor(const std::string& contentType) const {
    for (const auto& t : m_external) {
      if (std::ranges::find(t.mimeTypes, contentType) != t.mimeTypes.end()) {
        return &t;
      }
    }
    return nullptr;
  }

  bool Thumbnailer::supports(const FileEntry& entry) const {
    if (entry.directory || entry.path.empty() || entry.contentType.empty()) {
      return false;
    }
    // No hacerle miniaturas a la propia caché de miniaturas.
    if (entry.path.starts_with(cacheRoot().string())) {
      return false;
    }
    if (entry.contentType == "application/pdf") {
      return true;
    }
    if (pixbufHandles(entry.contentType)) {
      return entry.size <= kMaxImageBytes;
    }
    return externalFor(entry.contentType) != nullptr;
  }

  std::optional<std::string> Thumbnailer::get(const FileEntry& entry, bool large) {
    // Lo que terminaron los hilos desde la última vez.
    {
      std::scoped_lock lock(m_mutex);
      for (auto& [key, slot] : m_finished) {
        m_slots[key] = std::move(slot);
      }
      m_finished.clear();
    }
    const std::string key = entry.uri + (large ? "#L" : "#N");
    if (const auto it = m_slots.find(key); it != m_slots.end() && it->second.modified == entry.modified) {
      if (it->second.state == State::Ready) {
        return it->second.path;
      }
      return std::nullopt; // pendiente o fallida
    }
    m_slots[key] = {.modified = entry.modified, .state = State::Pending};
    {
      std::scoped_lock lock(m_mutex);
      if (m_queued.insert(key).second) {
        m_queue.push_front({key, entry.uri, entry.path, entry.contentType, entry.modified, large});
        // Al desplazarse rápido se acumulan pedidos de lo que ya no se ve: se descartan los más viejos.
        while (m_queue.size() > 400) {
          m_queued.erase(m_queue.back().key);
          m_slots.erase(m_queue.back().key); // se volverá a pedir si vuelve a verse
          m_queue.pop_back();
        }
      }
    }
    m_wake.notify_one();
    return std::nullopt;
  }

  void Thumbnailer::worker() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this] { return m_quit || !m_queue.empty(); });
        if (m_quit) {
          return;
        }
        job = std::move(m_queue.front());
        m_queue.pop_front();
      }
      Slot slot{.modified = job.modified, .state = State::Failed};
      const std::string output = thumbnailPath(job.uri, job.large);
      if (thumbnailValid(output, job.uri, job.modified)) {
        slot = {.modified = job.modified, .state = State::Ready, .path = output};
      } else if (!thumbnailValid(failPath(job.uri).string(), job.uri, job.modified)) {
        if (generate(job, output)) {
          slot = {.modified = job.modified, .state = State::Ready, .path = output};
        } else if (GdkPixbuf* mark = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1); mark != nullptr) {
          // Marca de fallo (también de la especificación) para no reintentar en cada visita.
          gdk_pixbuf_fill(mark, 0);
          (void)saveThumbnail(mark, failPath(job.uri), job.uri, job.modified);
          g_object_unref(mark);
        }
      }
      bool notify = false;
      {
        std::scoped_lock lock(m_mutex);
        m_queued.erase(job.key);
        m_finished.emplace_back(job.key, std::move(slot));
        notify = !m_notifyPending;
        m_notifyPending = true;
      }
      if (notify) {
        // Varias listas a la vez → un solo redibujado.
        std::weak_ptr<int> alive = m_alive;
        DeferredCall::callLater([this, alive] {
          if (alive.expired()) {
            return;
          }
          {
            std::scoped_lock lock(m_mutex);
            m_notifyPending = false;
          }
          if (m_onReady) {
            m_onReady();
          }
        });
      }
    }
  }

  bool Thumbnailer::generate(const Job& job, const std::string& output) {
    const int size = job.large ? kLarge : kNormal;
    GdkPixbuf* pixbuf = nullptr;
    if (job.contentType == "application/pdf") {
      pixbuf = pdfThumbnail(job.uri, size);
    } else if (pixbufHandles(job.contentType)) {
      pixbuf = imageThumbnail(job.path, size);
    }
    if (pixbuf == nullptr) {
      const ExternalThumbnailer* external = externalFor(job.contentType);
      if (external == nullptr) {
        return false;
      }
      // El miniaturador escribe en un temporal; luego se reescala y se le ponen las etiquetas.
      gchar* tempDir = g_dir_make_tmp("okaru-thumb-XXXXXX", nullptr);
      if (tempDir == nullptr) {
        return false;
      }
      const std::string temp = std::string(tempDir) + "/thumb.png";
      const auto args = expandThumbnailerExec(external->exec, job.path, job.uri, temp, size);
      const auto result = process::runSyncWithTimeout(args, std::chrono::seconds(30));
      if (result) {
        GdkPixbuf* raw = gdk_pixbuf_new_from_file(temp.c_str(), nullptr);
        if (raw != nullptr) {
          pixbuf = scaleToFit(raw, size);
          g_object_unref(raw);
        }
      }
      std::error_code ec;
      fs::remove_all(tempDir, ec);
      g_free(tempDir);
    }
    if (pixbuf == nullptr) {
      return false;
    }
    const bool ok = saveThumbnail(pixbuf, output, job.uri, job.modified);
    g_object_unref(pixbuf);
    return ok;
  }

} // namespace okaru::files
