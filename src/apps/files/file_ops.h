#pragma once

// OKARU: operaciones de okaru-files con GIO en un hilo aparte, una tras otra: copiar, mover, enlazar, papelera,
// borrar, restaurar, crear y renombrar. Progreso y conflictos vuelven al hilo principal; cada operación
// terminada deja un registro para deshacer/rehacer.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace okaru::files {

  enum class OpKind : std::uint8_t { Copy, Move, Link, Trash, Delete, Restore, Rename, NewFolder, NewFile };

  struct OpRequest {
    OpKind kind = OpKind::Copy;
    std::vector<std::string> sources; // URI
    std::string destination;          // carpeta destino (Copy/Move/Link/New*) o nombre nuevo (Rename)
    std::string name;                 // New*: nombre a crear
    std::vector<std::string> targets; // Copy/Move/Link: destino exacto de cada origen; Rename: nombre nuevo de cada uno
    bool record = true;               // guardar para deshacer (false al deshacer/rehacer)
  };

  // Qué pasó, para deshacerlo: pares origen → resultado.
  struct UndoRecord {
    OpKind kind = OpKind::Copy;
    std::vector<std::pair<std::string, std::string>> items;
  };

  struct Conflict {
    std::string source;
    std::string destination;
    bool sourceIsDir = false;
    bool destinationIsDir = false;
    std::uint64_t sourceSize = 0;
    std::uint64_t destinationSize = 0;
    std::int64_t sourceModified = 0;
    std::int64_t destinationModified = 0;
  };

  enum class ConflictChoice : std::uint8_t { Replace, Skip, KeepBoth, Cancel };

  struct ConflictAnswer {
    ConflictChoice choice = ConflictChoice::Skip;
    bool applyToAll = false;
  };

  struct OpProgress {
    OpKind kind = OpKind::Copy;
    std::string current; // nombre del archivo en curso
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::size_t itemsDone = 0;
    std::size_t itemsTotal = 0;
    std::size_t queued = 0; // operaciones esperando detrás
  };

  struct OpResult {
    OpKind kind = OpKind::Copy;
    bool cancelled = false;
    std::vector<std::string> errors;  // mensajes legibles
    std::vector<std::string> created; // URI resultantes (para seleccionarlas)
    std::vector<std::string> untrashable; // no admiten papelera (otro sistema de archivos): ofrecer borrar
    std::optional<UndoRecord> undo;
    bool record = true;
  };

  class OperationQueue {
  public:
    using ProgressFn = std::function<void(const OpProgress&)>;
    using ConflictFn = std::function<void(const Conflict&, std::function<void(ConflictAnswer)> reply)>;
    using DoneFn = std::function<void(OpResult)>;

    OperationQueue();
    ~OperationQueue();

    OperationQueue(const OperationQueue&) = delete;
    OperationQueue& operator=(const OperationQueue&) = delete;

    void setOnProgress(ProgressFn callback) { m_onProgress = std::move(callback); }
    void setOnConflict(ConflictFn callback) { m_onConflict = std::move(callback); }
    void setOnDone(DoneFn callback) { m_onDone = std::move(callback); }

    void submit(OpRequest request);
    void cancelCurrent();
    [[nodiscard]] bool busy() const noexcept { return m_busy.load(); }

    // Deshacer/rehacer: la operación inversa (o la misma) se encola como cualquier otra.
    [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }
    void undo();
    void redo();
    // Llamado desde onDone en el hilo principal.
    void remember(const OpResult& result);

    struct Job; // estado de la operación en curso (hilo de trabajo)

  private:
    void worker();
    void run(Job& job);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<OpRequest> m_pending;
    bool m_quit = false;
    std::atomic<bool> m_busy{false};
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);

    std::vector<UndoRecord> m_undo;
    std::vector<UndoRecord> m_redo;
    bool m_redoing = false;

    ProgressFn m_onProgress;
    ConflictFn m_onConflict;
    DoneFn m_onDone;
  };

  enum class BulkMode : std::uint8_t { Replace, Number, Affix };
  struct BulkRule {
    BulkMode mode = BulkMode::Replace;
    std::string find;
    std::string replace;
    std::string base; // numerar: "Vacaciones" → "Vacaciones 01.jpg"
    int start = 1;
    std::string prefix;
    std::string suffix;
  };
  // Nombres nuevos (mismo orden) conservando la extensión.
  [[nodiscard]] std::vector<std::string> bulkRenameNames(const std::vector<std::string>& names, const BulkRule& rule);

  // Inversa de un registro (lo que hay que encolar para deshacerlo).
  [[nodiscard]] std::vector<OpRequest> inverseOf(const UndoRecord& record);
  // La misma operación otra vez (rehacer).
  [[nodiscard]] std::vector<OpRequest> replayOf(const UndoRecord& record);

} // namespace okaru::files
