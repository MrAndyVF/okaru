#pragma once

// OKARU: modelo de okaru-files sobre GIO. Lista carpetas locales y de gvfs (smb://, sftp://, mtp://, trash:///,
// recent:///, network:///) en un hilo, las mantiene al día con GFileMonitor y ordena/filtra sin tocar el disco.

#include "core/timer_manager.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef struct _GFileMonitor GFileMonitor;

namespace okaru::files {

  struct FileEntry {
    std::string name;    // nombre en el sistema de archivos
    std::string display; // nombre visible (UTF-8)
    std::string uri;
    std::string path; // ruta local, vacía si no hay
    std::string contentType;
    std::string typeName;           // descripción legible del tipo
    std::vector<std::string> icons; // nombres del tema de iconos, del más específico al genérico
    std::string target;             // standard::target-uri (montables, recientes, papelera)
    std::string trashOrigin;        // papelera: ruta original
    std::string location;           // búsqueda: carpeta que lo contiene, relativa a la raíz buscada
    std::string customIcon;         // metadata::custom-icon (ruta local de una imagen), como en Nautilus
    std::string customIconName;     // metadata::custom-icon-name (nombre del tema)
    std::string folderColor;        // metadata::okaru-folder-color: id de color o "#rrggbb"
    std::uint64_t size = 0;
    std::int64_t modified = 0; // segundos desde la época
    std::int64_t deleted = 0;  // papelera: fecha de borrado
    std::uint32_t mode = 0;
    std::string owner;
    std::uint32_t childCount = 0; // carpetas: elementos (si se pidió contarlos)
    bool directory = false;
    bool hidden = false;
    bool symlink = false;
    bool canWrite = true;
    bool canExecute = false;
    bool mountable = false;
    std::string sortKey; // g_utf8_collate_key_for_filename(display)
  };

  enum class SortKey : std::uint8_t { Name, Size, Modified, Type };

  struct ViewOptions {
    SortKey sort = SortKey::Name;
    bool descending = false;
    bool foldersFirst = true;
    bool showHidden = false;
  };

  // Atributos que se piden a GIO para cada elemento.
  extern const char* const kQueryAttributes;

  [[nodiscard]] FileEntry entryFromInfo(void* parentGFile, void* gfileInfo);
  // Lista una carpeta de forma síncrona (para hilos). Devuelve false y deja el motivo en `error`.
  // `code`: GIOErrorEnum del fallo (p. ej. G_IO_ERROR_NOT_MOUNTED), si se pide.
  [[nodiscard]] bool listDirectory(const std::string& uri, std::vector<FileEntry>& out, std::string& error,
                                   const std::atomic<bool>* cancel = nullptr, int* code = nullptr);

  void sortEntries(std::vector<FileEntry>& entries, const ViewOptions& options);
  // Coincidencia sin distinguir mayúsculas ni tildes; `needle` ya normalizada con normalizeForMatch.
  [[nodiscard]] bool matchesFilter(const FileEntry& entry, std::string_view needle);
  [[nodiscard]] std::string normalizeForMatch(std::string_view text);

  [[nodiscard]] std::string formatSize(std::uint64_t bytes);
  [[nodiscard]] std::string formatDate(std::int64_t seconds);
  [[nodiscard]] std::string formatPermissions(std::uint32_t mode);
  // "file:///home/a/x" → "/home/a/x"; otras URI sin cambios.
  [[nodiscard]] std::string uriToPath(const std::string& uri);
  [[nodiscard]] std::string pathToUri(const std::string& path);
  [[nodiscard]] std::string parentUri(const std::string& uri);
  [[nodiscard]] std::string childUri(const std::string& parent, const std::string& name);
  [[nodiscard]] std::string displayNameOf(const std::string& uri);
  // Nombre libre en la carpeta: "foto.png" → "foto (2).png", "foto (3).png"…
  [[nodiscard]] std::string uniqueName(const std::string& dirUri, const std::string& name);
  // "foto (2).png" a partir de "foto.png" y n (sin consultar el disco).
  [[nodiscard]] std::string numberedName(const std::string& name, int n, bool directory);

  class DirectoryModel {
  public:
    using ChangedFn = std::function<void()>;

    DirectoryModel();
    ~DirectoryModel();

    DirectoryModel(const DirectoryModel&) = delete;
    DirectoryModel& operator=(const DirectoryModel&) = delete;

    void setOnChanged(ChangedFn callback) { m_onChanged = std::move(callback); }
    void load(const std::string& uri);
    void reload();
    void setOptions(const ViewOptions& options);
    void setFilter(std::string filter);
    // Selector de archivos: qué archivos se muestran (las carpetas siempre).
    void setPickFilter(std::function<bool(const FileEntry&)> filter);
    // Búsqueda recursiva desde la carpeta actual; vacía vuelve al listado normal.
    void setSearch(std::string query);

    [[nodiscard]] const std::string& uri() const noexcept { return m_uri; }
    [[nodiscard]] const std::vector<FileEntry>& entries() const noexcept { return m_visible; }
    [[nodiscard]] std::size_t totalCount() const noexcept { return m_all.size(); }
    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] bool searching() const noexcept { return !m_search.empty(); }
    [[nodiscard]] bool searchRunning() const noexcept { return m_searchRunning; }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] int errorCode() const noexcept { return m_errorCode; }
    [[nodiscard]] const ViewOptions& options() const noexcept { return m_options; }
    [[nodiscard]] const std::string& filter() const noexcept { return m_filter; }
    [[nodiscard]] const std::string& search() const noexcept { return m_search; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

  private:
    void rebuildVisible();
    void startMonitor();
    void stopMonitor();
    void cancelSearch();
    static void onMonitorEvent(GFileMonitor* monitor, void* file, void* other, int event, void* self);

    std::string m_uri;
    std::vector<FileEntry> m_all;
    std::vector<FileEntry> m_found; // resultados de búsqueda
    std::vector<FileEntry> m_visible;
    ViewOptions m_options;
    std::string m_filter;
    std::function<bool(const FileEntry&)> m_pickFilter;
    std::string m_search;
    std::string m_error;
    int m_errorCode = -1;
    bool m_loading = false;
    bool m_searchRunning = false;
    std::uint64_t m_generation = 0; // sube en cada carga: los resultados viejos se descartan
    std::shared_ptr<std::atomic<bool>> m_loadCancel;
    std::shared_ptr<std::atomic<bool>> m_searchCancel;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0); // los hilos no tocan el modelo si ya no existe
    GFileMonitor* m_monitor = nullptr;
    Timer m_reloadTimer;
    ChangedFn m_onChanged;
  };

} // namespace okaru::files
