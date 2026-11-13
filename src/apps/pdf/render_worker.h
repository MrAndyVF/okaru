#pragma once

// OKARU: hilo de renderizado de okaru-pdf. Abre su propio documento (poppler no se comparte entre hilos) y atiende una
// cola: cada vez que cambia lo que se ve, la ventana manda la lista nueva y lo pendiente que ya no sirve se descarta.
// Los resultados llegan al hilo principal con DeferredCall.

#include "apps/pdf/pdf_document.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace okaru::pdf {

  struct RenderJob {
    int page = 0;
    double scale = 1.0; // píxeles por punto
    int rotation = 0;
    bool invert = false;
    bool thumbnail = false;
    std::uint32_t revision = 0; // cambios aplicados al documento (formularios, resaltados) cuando se pidió

    bool operator==(const RenderJob&) const = default;
  };

  struct SearchHit {
    int page = 0;
    Rect rect;
  };

  class RenderWorker {
  public:
    using RenderDone = std::function<void(const RenderJob& job, RenderedPage page)>;
    // Resultados de búsqueda de una página (en orden de páginas); `done` = terminó todo el documento.
    using SearchDone = std::function<void(std::vector<SearchHit> hits, bool done)>;

    RenderWorker(std::filesystem::path path, std::string password, RenderDone onRender);
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Reemplaza la cola (lo primero, primero).
    void request(std::vector<RenderJob> jobs);
    // Busca en todo el documento desde `firstPage`, dando la vuelta; cancela la búsqueda anterior.
    void search(std::string text, bool caseSensitive, bool wholeWords, int firstPage, SearchDone onResults);
    void cancelSearch();
    // Aplica el cambio a su documento antes de lo siguiente que dibuje.
    void edit(Edit change);

  private:
    void run();

    std::filesystem::path m_path;
    std::string m_password;
    RenderDone m_onRender;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<RenderJob> m_jobs;
    std::vector<Edit> m_edits;
    std::optional<RenderJob> m_current; // el que se está dibujando: no se vuelve a encolar
    struct Search {
      std::string text;
      bool caseSensitive = false;
      bool wholeWords = false;
      int firstPage = 0;
      SearchDone onResults;
    };
    std::optional<Search> m_search;
    std::uint64_t m_searchGeneration = 0;
    bool m_stop = false;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    std::thread m_thread;
  };

} // namespace okaru::pdf
