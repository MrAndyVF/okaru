#include "apps/files/file_ops.h"

#include "apps/files/file_model.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"

#include <chrono>
#include <format>
#include <future>
#include <gio/gio.h>

namespace okaru::files {

  namespace {

    using Clock = std::chrono::steady_clock;

    std::string uriOf(GFile* file) {
      char* raw = g_file_get_uri(file);
      std::string out = raw != nullptr ? raw : "";
      g_free(raw);
      return out;
    }

    std::string nameOf(GFile* file) { return displayNameOf(uriOf(file)); }

    GFileType typeOf(GFile* file) { return g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr); }

    struct Unref {
      void operator()(void* object) const {
        if (object != nullptr) {
          g_object_unref(object);
        }
      }
    };
    using FilePtr = std::unique_ptr<GFile, Unref>;

    FilePtr fileFor(const std::string& uri) { return FilePtr(g_file_new_for_uri(uri.c_str())); }

  } // namespace

  struct OperationQueue::Job {
    OpRequest request;
    GCancellable* cancellable = nullptr;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::optional<ConflictChoice> forAll;
    OpProgress progress;
    OpResult result;
    std::uint64_t fileBase = 0; // bytes ya contados antes del archivo en curso
    Clock::time_point lastReport{};
    OperationQueue* queue = nullptr;

    [[nodiscard]] bool cancelled() const {
      return cancel->load() || g_cancellable_is_cancelled(cancellable) != FALSE;
    }

    void report(bool force = false) {
      const auto now = Clock::now();
      if (!force && now - lastReport < std::chrono::milliseconds(80)) {
        return;
      }
      lastReport = now;
      std::weak_ptr<int> alive = queue->m_alive;
      DeferredCall::callLater([q = queue, alive, progress = progress] {
        if (!alive.expired() && q->m_onProgress) {
          q->m_onProgress(progress);
        }
      });
    }

    void fail(GFile* file, const GError* error) {
      if (error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        return;
      }
      result.errors.push_back(
          std::format("{}: {}", file != nullptr ? nameOf(file) : std::string(), error != nullptr ? error->message : "?")
      );
    }

    ConflictAnswer ask(GFile* source, GFile* destination) {
      if (forAll) {
        return {*forAll, true};
      }
      Conflict conflict{.source = uriOf(source), .destination = uriOf(destination)};
      const auto fill = [](GFile* file, bool& dir, std::uint64_t& size, std::int64_t& modified) {
        GFileInfo* info = g_file_query_info(
            file, "standard::type,standard::size,time::modified", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
        );
        if (info != nullptr) {
          dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
          size = static_cast<std::uint64_t>(g_file_info_get_size(info));
          modified =
              static_cast<std::int64_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
          g_object_unref(info);
        }
      };
      fill(source, conflict.sourceIsDir, conflict.sourceSize, conflict.sourceModified);
      fill(destination, conflict.destinationIsDir, conflict.destinationSize, conflict.destinationModified);

      auto promise = std::make_shared<std::promise<ConflictAnswer>>();
      auto future = promise->get_future();
      std::weak_ptr<int> alive = queue->m_alive;
      DeferredCall::callLater([q = queue, alive, conflict, promise] {
        if (alive.expired() || !q->m_onConflict) {
          promise->set_value({ConflictChoice::Cancel, true});
          return;
        }
        auto once = std::make_shared<std::once_flag>();
        q->m_onConflict(conflict, [promise, once](ConflictAnswer answer) {
          std::call_once(*once, [&] { promise->set_value(answer); });
        });
      });
      // Esperar la respuesta sin quedar colgados si se cierra la ventana.
      while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (cancel->load()) {
          return {ConflictChoice::Cancel, true};
        }
      }
      ConflictAnswer answer = future.get();
      if (answer.applyToAll) {
        forAll = answer.choice;
      }
      return answer;
    }
  };

  namespace {

    using Job = OperationQueue::Job;

    void countTree(Job& job, GFile* file, std::uint64_t& bytes, std::size_t& items) {
      GFileInfo* info = g_file_query_info(
          file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job.cancellable, nullptr
      );
      if (info == nullptr) {
        return;
      }
      const bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
      bytes += static_cast<std::uint64_t>(g_file_info_get_size(info));
      ++items;
      g_object_unref(info);
      if (!dir) {
        return;
      }
      GFileEnumerator* it = g_file_enumerate_children(
          file, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job.cancellable, nullptr
      );
      if (it == nullptr) {
        return;
      }
      GFileInfo* child = nullptr;
      while (!job.cancelled() && (child = g_file_enumerator_next_file(it, job.cancellable, nullptr)) != nullptr) {
        FilePtr childFile(g_file_get_child(file, g_file_info_get_name(child)));
        countTree(job, childFile.get(), bytes, items);
        g_object_unref(child);
      }
      g_object_unref(it);
    }

    bool deleteTree(Job& job, GFile* file) {
      if (typeOf(file) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator* it = g_file_enumerate_children(
            file, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job.cancellable, nullptr
        );
        if (it != nullptr) {
          GFileInfo* child = nullptr;
          while (!job.cancelled() && (child = g_file_enumerator_next_file(it, job.cancellable, nullptr)) != nullptr) {
            FilePtr childFile(g_file_get_child(file, g_file_info_get_name(child)));
            (void)deleteTree(job, childFile.get());
            g_object_unref(child);
          }
          g_object_unref(it);
        }
      }
      if (job.cancelled()) {
        return false;
      }
      GError* error = nullptr;
      if (g_file_delete(file, job.cancellable, &error) == FALSE) {
        job.fail(file, error);
        g_clear_error(&error);
        return false;
      }
      ++job.progress.itemsDone;
      job.report();
      return true;
    }

    void onCopyProgress(goffset current, goffset /*total*/, gpointer data) {
      auto* job = static_cast<Job*>(data);
      job->progress.bytesDone = job->fileBase + static_cast<std::uint64_t>(current);
      job->report();
    }

    // Copia (o mueve) `source` a `destination`, recursiva para carpetas. `merge`: la carpeta destino ya existe y
    // se completa en vez de reemplazarse.
    bool copyTree(Job& job, GFile* source, GFile* destination, bool move) {
      if (job.cancelled()) {
        return false;
      }
      job.progress.current = nameOf(source);
      GError* error = nullptr;
      const GFileType type = typeOf(source);

      if (move) {
        // Mismo sistema de archivos: renombrar, sin copiar bytes.
        if (g_file_move(
                source, destination,
                static_cast<GFileCopyFlags>(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE
                                            | G_FILE_COPY_ALL_METADATA),
                job.cancellable, nullptr, nullptr, &error
            )
            != FALSE) {
          std::uint64_t bytes = 0;
          std::size_t items = 0;
          countTree(job, destination, bytes, items);
          job.progress.bytesDone += bytes;
          job.fileBase = job.progress.bytesDone;
          job.progress.itemsDone += items;
          job.report();
          return true;
        }
        const bool fallback = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
            || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)
            || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE);
        if (!fallback) {
          job.fail(source, error);
          g_clear_error(&error);
          return false;
        }
        g_clear_error(&error);
      }

      if (type == G_FILE_TYPE_DIRECTORY) {
        if (g_file_make_directory(destination, job.cancellable, &error) == FALSE
            && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
          job.fail(destination, error);
          g_clear_error(&error);
          return false;
        }
        g_clear_error(&error);
        ++job.progress.itemsDone;
        bool ok = true;
        GFileEnumerator* it = g_file_enumerate_children(
            source, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job.cancellable, &error
        );
        if (it == nullptr) {
          job.fail(source, error);
          g_clear_error(&error);
          return false;
        }
        GFileInfo* child = nullptr;
        while (!job.cancelled() && (child = g_file_enumerator_next_file(it, job.cancellable, nullptr)) != nullptr) {
          const char* name = g_file_info_get_name(child);
          FilePtr from(g_file_get_child(source, name));
          FilePtr to(g_file_get_child(destination, name));
          if (g_file_query_exists(to.get(), nullptr) != FALSE) {
            const bool bothDirs = typeOf(from.get()) == G_FILE_TYPE_DIRECTORY && typeOf(to.get()) == G_FILE_TYPE_DIRECTORY;
            if (!bothDirs) {
              const ConflictAnswer answer = job.ask(from.get(), to.get());
              if (answer.choice == ConflictChoice::Cancel) {
                job.cancel->store(true);
                g_object_unref(child);
                break;
              }
              if (answer.choice == ConflictChoice::Skip) {
                g_object_unref(child);
                continue;
              }
              if (answer.choice == ConflictChoice::KeepBoth) {
                to.reset(g_file_get_child(destination, uniqueName(uriOf(destination), name).c_str()));
              } else {
                (void)deleteTree(job, to.get());
              }
            }
          }
          ok = copyTree(job, from.get(), to.get(), false) && ok;
          g_object_unref(child);
        }
        g_object_unref(it);
        if (move && ok && !job.cancelled()) {
          (void)g_file_delete(source, job.cancellable, nullptr);
        }
        return ok;
      }

      job.fileBase = job.progress.bytesDone;
      const auto flags = static_cast<GFileCopyFlags>(
          G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA | G_FILE_COPY_OVERWRITE
      );
      const bool ok =
          (move ? g_file_move(source, destination, flags, job.cancellable, onCopyProgress, &job, &error)
                : g_file_copy(source, destination, flags, job.cancellable, onCopyProgress, &job, &error))
          != FALSE;
      if (!ok) {
        job.fail(source, error);
        g_clear_error(&error);
        return false;
      }
      GFileInfo* info = g_file_query_info(destination, "standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
      job.progress.bytesDone = job.fileBase + (info != nullptr ? static_cast<std::uint64_t>(g_file_info_get_size(info)) : 0);
      if (info != nullptr) {
        g_object_unref(info);
      }
      ++job.progress.itemsDone;
      job.report();
      return true;
    }

    // Busca en la papelera el elemento que vino de `originalPath` (el borrado más reciente).
    FilePtr findInTrash(const std::string& originalPath) {
      FilePtr trash(g_file_new_for_uri("trash:///"));
      GFileEnumerator* it = g_file_enumerate_children(
          trash.get(), "standard::name,trash::orig-path,trash::deletion-date", G_FILE_QUERY_INFO_NONE, nullptr, nullptr
      );
      if (it == nullptr) {
        return nullptr;
      }
      FilePtr best;
      std::string bestDate;
      GFileInfo* info = nullptr;
      while ((info = g_file_enumerator_next_file(it, nullptr, nullptr)) != nullptr) {
        const char* orig = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
        const char* date = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE);
        if (orig != nullptr && originalPath == orig && (best == nullptr || (date != nullptr && bestDate < date))) {
          best.reset(g_file_get_child(trash.get(), g_file_info_get_name(info)));
          bestDate = date != nullptr ? date : "";
        }
        g_object_unref(info);
      }
      g_object_unref(it);
      return best;
    }

  } // namespace

  // ── Cola ───────────────────────────────────────────────────────────────────────────────────────────

  OperationQueue::OperationQueue() : m_thread([this] { worker(); }) {}

  OperationQueue::~OperationQueue() {
    {
      std::scoped_lock lock(m_mutex);
      m_quit = true;
      m_pending.clear();
      if (m_cancel) {
        m_cancel->store(true);
      }
    }
    m_wake.notify_all();
    m_alive.reset();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  void OperationQueue::submit(OpRequest request) {
    {
      std::scoped_lock lock(m_mutex);
      m_pending.push_back(std::move(request));
    }
    m_wake.notify_one();
  }

  void OperationQueue::cancelCurrent() {
    std::scoped_lock lock(m_mutex);
    if (m_cancel) {
      m_cancel->store(true);
    }
  }

  void OperationQueue::worker() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this] { return m_quit || !m_pending.empty(); });
        if (m_quit) {
          return;
        }
        job.request = std::move(m_pending.front());
        m_pending.pop_front();
        job.progress.queued = m_pending.size();
        m_cancel = std::make_shared<std::atomic<bool>>(false);
        job.cancel = m_cancel;
      }
      m_busy = true;
      job.queue = this;
      job.cancellable = g_cancellable_new();
      run(job);
      g_object_unref(job.cancellable);
      job.result.kind = job.request.kind;
      job.result.cancelled = job.cancel->load();
      job.result.record = job.request.record;
      m_busy = false;
      std::weak_ptr<int> alive = m_alive;
      DeferredCall::callLater([this, alive, result = std::move(job.result)]() mutable {
        if (!alive.expired() && m_onDone) {
          m_onDone(std::move(result));
        }
      });
    }
  }

  void OperationQueue::run(Job& job) {
    const OpRequest& request = job.request;
    job.progress.kind = request.kind;
    UndoRecord record{.kind = request.kind};
    GError* error = nullptr;

    switch (request.kind) {
    case OpKind::Copy:
    case OpKind::Move:
    case OpKind::Link: {
      const bool move = request.kind == OpKind::Move;
      if (request.kind != OpKind::Link) {
        for (const auto& uri : request.sources) {
          FilePtr source = fileFor(uri);
          countTree(job, source.get(), job.progress.bytesTotal, job.progress.itemsTotal);
        }
      } else {
        job.progress.itemsTotal = request.sources.size();
      }
      job.report(true);
      FilePtr folder = fileFor(request.destination);
      for (std::size_t i = 0; i < request.sources.size() && !job.cancelled(); ++i) {
        FilePtr source = fileFor(request.sources[i]);
        FilePtr destination;
        if (i < request.targets.size()) {
          destination = fileFor(request.targets[i]);
        } else {
          char* base = g_file_get_basename(source.get());
          destination.reset(g_file_get_child(folder.get(), base != nullptr ? base : "file"));
          g_free(base);
        }
        if (g_file_equal(source.get(), destination.get()) != FALSE) {
          if (move) {
            continue; // mover a la misma carpeta: nada que hacer
          }
          // Duplicar en la misma carpeta: "nombre (2)".
          char* base = g_file_get_basename(source.get());
          destination.reset(g_file_get_child(folder.get(), uniqueName(request.destination, base).c_str()));
          g_free(base);
        } else if (g_file_has_prefix(destination.get(), source.get()) != FALSE) {
          job.result.errors.push_back(i18n::tr("apps.files.error.into-itself", "name", nameOf(source.get())));
          continue;
        } else if (g_file_query_exists(destination.get(), nullptr) != FALSE) {
          const bool merge = typeOf(source.get()) == G_FILE_TYPE_DIRECTORY
              && typeOf(destination.get()) == G_FILE_TYPE_DIRECTORY;
          const ConflictAnswer answer = job.ask(source.get(), destination.get());
          if (answer.choice == ConflictChoice::Cancel) {
            job.cancel->store(true);
            break;
          }
          if (answer.choice == ConflictChoice::Skip) {
            continue;
          }
          if (answer.choice == ConflictChoice::KeepBoth) {
            char* base = g_file_get_basename(destination.get());
            destination.reset(g_file_get_child(folder.get(), uniqueName(uriOf(folder.get()), base).c_str()));
            g_free(base);
          } else if (!merge) {
            (void)deleteTree(job, destination.get());
          }
          // Reemplazar carpeta con carpeta = combinar (como Nautilus y Dolphin).
        }
        bool ok = false;
        if (request.kind == OpKind::Link) {
          char* target = g_file_get_path(source.get());
          ok = g_file_make_symbolic_link(
                   destination.get(), target != nullptr ? target : request.sources[i].c_str(), job.cancellable, &error
               )
              != FALSE;
          g_free(target);
          if (!ok) {
            job.fail(source.get(), error);
            g_clear_error(&error);
          }
          ++job.progress.itemsDone;
        } else {
          ok = copyTree(job, source.get(), destination.get(), move);
        }
        if (ok) {
          record.items.emplace_back(request.sources[i], uriOf(destination.get()));
          job.result.created.push_back(uriOf(destination.get()));
        }
      }
      break;
    }

    case OpKind::Trash:
    case OpKind::Delete: {
      job.progress.itemsTotal = request.sources.size();
      job.report(true);
      for (const auto& uri : request.sources) {
        if (job.cancelled()) {
          break;
        }
        FilePtr file = fileFor(uri);
        job.progress.current = nameOf(file.get());
        if (request.kind == OpKind::Trash) {
          if (g_file_trash(file.get(), job.cancellable, &error) == FALSE) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
              job.result.untrashable.push_back(uri);
            } else {
              job.fail(file.get(), error);
            }
            g_clear_error(&error);
          } else {
            record.items.emplace_back(uri, "");
          }
          ++job.progress.itemsDone;
        } else {
          (void)deleteTree(job, file.get());
        }
        job.report();
      }
      break;
    }

    case OpKind::Restore: {
      job.progress.itemsTotal = request.sources.size();
      for (const auto& uri : request.sources) {
        if (job.cancelled()) {
          break;
        }
        // Desde la papelera (trash:///x) o deshaciendo una papelera (la ruta original).
        FilePtr trashed;
        std::string original;
        if (uri.starts_with("trash:")) {
          trashed = fileFor(uri);
          GFileInfo* info =
              g_file_query_info(trashed.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
          if (info != nullptr) {
            if (const char* orig = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
                orig != nullptr) {
              original = orig;
            }
            g_object_unref(info);
          }
        } else {
          original = uriToPath(uri);
          trashed = findInTrash(original);
        }
        if (trashed == nullptr || original.empty()) {
          job.result.errors.push_back(i18n::tr("apps.files.error.not-in-trash", "name", displayNameOf(uri)));
          continue;
        }
        FilePtr destination(g_file_new_for_path(original.c_str()));
        if (FilePtr parent(g_file_get_parent(destination.get())); parent != nullptr) {
          (void)g_file_make_directory_with_parents(parent.get(), nullptr, nullptr);
        }
        if (g_file_query_exists(destination.get(), nullptr) != FALSE) {
          const ConflictAnswer answer = job.ask(trashed.get(), destination.get());
          if (answer.choice == ConflictChoice::Cancel) {
            job.cancel->store(true);
            break;
          }
          if (answer.choice == ConflictChoice::Skip) {
            continue;
          }
          if (answer.choice == ConflictChoice::KeepBoth) {
            FilePtr parent(g_file_get_parent(destination.get()));
            char* base = g_file_get_basename(destination.get());
            destination.reset(g_file_get_child(parent.get(), uniqueName(uriOf(parent.get()), base).c_str()));
            g_free(base);
          } else {
            (void)deleteTree(job, destination.get());
          }
        }
        job.progress.current = nameOf(destination.get());
        if (g_file_move(trashed.get(), destination.get(), G_FILE_COPY_NOFOLLOW_SYMLINKS, job.cancellable, nullptr,
                        nullptr, &error)
            == FALSE) {
          job.fail(destination.get(), error);
          g_clear_error(&error);
        } else {
          record.items.emplace_back(uri, uriOf(destination.get()));
          job.result.created.push_back(uriOf(destination.get()));
        }
        ++job.progress.itemsDone;
        job.report();
      }
      break;
    }

    case OpKind::Rename: {
      // Uno (destination) o en lote (targets: nombre nuevo de cada origen).
      for (std::size_t i = 0; i < request.sources.size(); ++i) {
        const std::string& name = i < request.targets.size() ? request.targets[i] : request.destination;
        if (name.empty() || (request.targets.empty() && i > 0)) {
          continue;
        }
        FilePtr file = fileFor(request.sources[i]);
        if (displayNameOf(request.sources[i]) == name) {
          continue;
        }
        GFile* renamed = g_file_set_display_name(file.get(), name.c_str(), job.cancellable, &error);
        if (renamed == nullptr) {
          job.fail(file.get(), error);
          g_clear_error(&error);
          continue;
        }
        record.items.emplace_back(request.sources[i], uriOf(renamed));
        job.result.created.push_back(uriOf(renamed));
        g_object_unref(renamed);
      }
      break;
    }

    case OpKind::NewFolder:
    case OpKind::NewFile: {
      FilePtr folder = fileFor(request.destination);
      FilePtr file(g_file_get_child(folder.get(), uniqueName(request.destination, request.name).c_str()));
      bool ok = false;
      if (request.kind == OpKind::NewFolder) {
        ok = g_file_make_directory(file.get(), job.cancellable, &error) != FALSE;
      } else if (GFileOutputStream* stream = g_file_create(file.get(), G_FILE_CREATE_NONE, job.cancellable, &error);
                 stream != nullptr) {
        g_output_stream_close(G_OUTPUT_STREAM(stream), nullptr, nullptr);
        g_object_unref(stream);
        ok = true;
      }
      if (!ok) {
        job.fail(file.get(), error);
        g_clear_error(&error);
        break;
      }
      record.items.emplace_back("", uriOf(file.get()));
      job.result.created.push_back(uriOf(file.get()));
      break;
    }
    }

    job.progress.bytesDone = job.progress.bytesTotal;
    job.report(true);
    // Borrar para siempre no se deshace; lo demás sí, aunque se haya cancelado a medias.
    if (request.kind != OpKind::Delete && !record.items.empty()) {
      job.result.undo = std::move(record);
    }
  }

  // ── Renombrado en lote ─────────────────────────────────────────────────────────────────────────────

  std::vector<std::string> bulkRenameNames(const std::vector<std::string>& names, const BulkRule& rule) {
    std::vector<std::string> out;
    const std::size_t digits = std::max<std::size_t>(2, std::to_string(rule.start + static_cast<int>(names.size()) - 1).size());
    for (std::size_t i = 0; i < names.size(); ++i) {
      const std::string& name = names[i];
      // La extensión se conserva (no se cuenta la de los archivos ocultos ".bashrc").
      std::size_t dot = name.rfind('.');
      if (dot == 0 || dot == std::string::npos) {
        dot = name.size();
      }
      std::string stem = name.substr(0, dot);
      const std::string ext = name.substr(dot);
      switch (rule.mode) {
      case BulkMode::Replace:
        if (!rule.find.empty()) {
          for (std::size_t at = stem.find(rule.find); at != std::string::npos; at = stem.find(rule.find, at + rule.replace.size())) {
            stem.replace(at, rule.find.size(), rule.replace);
          }
        }
        break;
      case BulkMode::Number: {
        std::string number = std::to_string(rule.start + static_cast<int>(i));
        number.insert(0, digits > number.size() ? digits - number.size() : 0, '0');
        stem = rule.base.empty() ? number : rule.base + " " + number;
        break;
      }
      case BulkMode::Affix:
        stem = rule.prefix + stem + rule.suffix;
        break;
      }
      out.push_back(stem + ext);
    }
    return out;
  }

  // ── Deshacer / rehacer ─────────────────────────────────────────────────────────────────────────────

  std::vector<OpRequest> inverseOf(const UndoRecord& record) {
    std::vector<OpRequest> out;
    OpRequest request{.record = false};
    for (const auto& [from, to] : record.items) {
      switch (record.kind) {
      case OpKind::Copy:
      case OpKind::Link:
      case OpKind::NewFolder:
      case OpKind::NewFile:
        request.kind = OpKind::Delete;
        request.sources.push_back(to);
        break;
      case OpKind::Move:
        request.kind = OpKind::Move;
        request.sources.push_back(to);
        request.targets.push_back(from);
        break;
      case OpKind::Trash:
        request.kind = OpKind::Restore;
        request.sources.push_back(from);
        break;
      case OpKind::Restore:
        request.kind = OpKind::Trash;
        request.sources.push_back(to);
        break;
      case OpKind::Rename:
        // Un lote se deshace en una sola operación.
        request.kind = OpKind::Rename;
        request.sources.push_back(to);
        request.targets.push_back(displayNameOf(from));
        break;
      case OpKind::Delete:
        break;
      }
    }
    if (!request.sources.empty()) {
      out.push_back(std::move(request));
    }
    return out;
  }

  std::vector<OpRequest> replayOf(const UndoRecord& record) {
    std::vector<OpRequest> out;
    OpRequest request{.kind = record.kind, .record = false};
    for (const auto& [from, to] : record.items) {
      switch (record.kind) {
      case OpKind::Copy:
      case OpKind::Move:
      case OpKind::Link:
        request.sources.push_back(from);
        request.targets.push_back(to);
        break;
      case OpKind::Trash:
        request.sources.push_back(from);
        break;
      case OpKind::Restore:
        request.sources.push_back(to); // deshacer lo devolvió a la papelera: se busca por su ruta original
        break;
      case OpKind::Rename:
        request.sources.push_back(from);
        request.targets.push_back(displayNameOf(to));
        break;
      case OpKind::NewFolder:
      case OpKind::NewFile:
        out.push_back({.kind = record.kind, .destination = parentUri(to), .name = displayNameOf(to), .record = false});
        break;
      case OpKind::Delete:
        break;
      }
    }
    if (!request.sources.empty()) {
      out.push_back(std::move(request));
    }
    return out;
  }

  void OperationQueue::remember(const OpResult& result) {
    if (!result.undo) {
      return;
    }
    if (result.record) {
      m_undo.push_back(*result.undo);
      m_redo.clear();
    } else if (m_redoing) {
      m_undo.push_back(*result.undo);
    }
  }

  void OperationQueue::undo() {
    if (m_undo.empty()) {
      return;
    }
    UndoRecord record = std::move(m_undo.back());
    m_undo.pop_back();
    m_redoing = false;
    for (auto& request : inverseOf(record)) {
      submit(std::move(request));
    }
    m_redo.push_back(std::move(record));
  }

  void OperationQueue::redo() {
    if (m_redo.empty()) {
      return;
    }
    UndoRecord record = std::move(m_redo.back());
    m_redo.pop_back();
    m_redoing = true;
    for (auto& request : replayOf(record)) {
      submit(std::move(request));
    }
  }

} // namespace okaru::files
