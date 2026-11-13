#include "apps/pdf/render_worker.h"

#include "core/deferred_call.h"

namespace okaru::pdf {

  RenderWorker::RenderWorker(std::filesystem::path path, std::string password, RenderDone onRender)
      : m_path(std::move(path)), m_password(std::move(password)), m_onRender(std::move(onRender)),
        m_thread([this] { run(); }) {}

  RenderWorker::~RenderWorker() {
    *m_alive = false;
    {
      std::scoped_lock lock(m_mutex);
      m_stop = true;
      m_jobs.clear();
      m_search.reset();
    }
    m_wake.notify_all();
    m_thread.join();
  }

  void RenderWorker::request(std::vector<RenderJob> jobs) {
    {
      std::scoped_lock lock(m_mutex);
      m_jobs.clear();
      for (const RenderJob& job : jobs) {
        if (job != m_current) {
          m_jobs.push_back(job);
        }
      }
    }
    m_wake.notify_all();
  }

  void
  RenderWorker::search(std::string text, bool caseSensitive, bool wholeWords, int firstPage, SearchDone onResults) {
    {
      std::scoped_lock lock(m_mutex);
      ++m_searchGeneration;
      m_search = Search{std::move(text), caseSensitive, wholeWords, firstPage, std::move(onResults)};
    }
    m_wake.notify_all();
  }

  void RenderWorker::edit(Edit change) {
    {
      std::scoped_lock lock(m_mutex);
      m_edits.push_back(std::move(change));
    }
    m_wake.notify_all();
  }

  void RenderWorker::cancelSearch() {
    std::scoped_lock lock(m_mutex);
    ++m_searchGeneration;
    m_search.reset();
  }

  void RenderWorker::run() {
    auto opened = Document::open(m_path, m_password);
    if (!opened) {
      return;
    }
    const std::unique_ptr<Document> doc = std::move(*opened);
    const int pages = doc->pageCount();
    // Búsqueda en curso: se avanza de a una página entre render y render, así lo visible no espera.
    std::optional<Search> search;
    std::uint64_t searchGeneration = 0;
    int searched = 0;
    while (true) {
      RenderJob job;
      bool haveJob = false;
      std::vector<Edit> edits;
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [&] {
          return m_stop || !m_jobs.empty() || !m_edits.empty() || m_search.has_value() || search.has_value();
        });
        if (m_stop) {
          return;
        }
        edits.swap(m_edits);
        if (m_search.has_value()) {
          search = std::move(m_search);
          m_search.reset();
          searchGeneration = m_searchGeneration;
          searched = 0;
        } else if (searchGeneration != m_searchGeneration) {
          search.reset(); // cancelada
        }
        if (!m_jobs.empty()) {
          job = m_jobs.front();
          m_jobs.pop_front();
          haveJob = true;
          m_current = job;
        }
      }
      for (const Edit& change : edits) {
        (void)doc->apply(change);
      }
      if (haveJob) {
        RenderedPage page = doc->render(job.page, job.scale, job.rotation, job.invert);
        auto shared = std::make_shared<RenderedPage>(std::move(page));
        {
          std::scoped_lock lock(m_mutex);
          m_current.reset();
        }
        DeferredCall::callLater([this, alive = m_alive, job, shared] {
          if (*alive) {
            m_onRender(job, std::move(*shared));
          }
        });
        continue;
      }
      if (search.has_value() && searched < pages) {
        const int page = (search->firstPage + searched) % pages;
        std::vector<SearchHit> hits;
        for (const Rect& rect : doc->find(page, search->text, search->caseSensitive, search->wholeWords)) {
          hits.push_back({page, rect});
        }
        ++searched;
        const bool done = searched >= pages;
        if (!hits.empty() || done) {
          auto callback = search->onResults;
          const std::uint64_t generation = searchGeneration;
          DeferredCall::callLater([this, alive = m_alive, callback, hits = std::move(hits), done, generation] mutable {
            // Una búsqueda vieja que todavía tenía resultados en camino no llega.
            if (*alive && generation == m_searchGeneration) {
              callback(std::move(hits), done);
            }
          });
        }
        if (done) {
          search.reset();
        }
      }
    }
  }

} // namespace okaru::pdf
