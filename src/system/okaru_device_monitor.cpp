#include "system/okaru_device_monitor.h"

#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"

#include <cstdint>
#include <gio/gio.h>
#include <string>
#include <unordered_map>
#include <vector>

// OKARU: avisa al conectar o quitar un dispositivo extraíble. GVolumeMonitor vive en un GMainContext propio
// que este PollSource bombea desde el bucle principal (mismo puente que WirePlumberMixer).

namespace {

  constexpr Logger kLog("devices");
  constexpr std::int32_t kRemovedTimeoutMs = 3000;

  std::string volumeName(GVolume* volume) {
    g_autofree char* name = g_volume_get_name(volume);
    return name != nullptr ? name : "";
  }

  // gvfs-mtp y gvfs-gphoto2 publican el teléfono como volumen con raíz mtp:// o gphoto2://.
  bool isPhone(GVolume* volume) {
    g_autoptr(GFile) root = g_volume_get_activation_root(volume);
    if (root == nullptr) {
      return false;
    }
    g_autofree char* scheme = g_file_get_uri_scheme(root);
    return scheme != nullptr && (g_str_equal(scheme, "mtp") || g_str_equal(scheme, "gphoto2"));
  }

  // Sin unidad (teléfonos, gvfs) o unidad extraíble; los discos internos que udisks añade al vuelo no avisan.
  bool isRemovable(GVolume* volume) {
    g_autoptr(GDrive) drive = g_volume_get_drive(volume);
    return drive == nullptr || g_drive_is_removable(drive) || g_drive_is_media_removable(drive);
  }

  void openMount(GMount* mount, const std::string& activationToken) {
    g_autoptr(GFile) root = g_mount_get_root(mount);
    g_autofree char* uri = g_file_get_uri(root);
    const std::string program = process::commandExists("okaru-files") ? "okaru-files" : "xdg-open";
    if (!process::runAsync(std::vector<std::string>{program, uri}, activationToken)) {
      kLog.warn("could not open {} with {}", uri, program);
    }
  }

  struct MountRequest {
    std::string activationToken;
  };

  void onMounted(GObject* source, GAsyncResult* result, gpointer data) {
    auto* request = static_cast<MountRequest*>(data);
    auto* volume = G_VOLUME(source);
    g_autoptr(GError) error = nullptr;
    if (g_volume_mount_finish(volume, result, &error) == FALSE) {
      kLog.warn("could not mount {}: {}", volumeName(volume), error->message);
    } else if (g_autoptr(GMount) mount = g_volume_get_mount(volume); mount != nullptr) {
      openMount(mount, request->activationToken);
    }
    delete request;
  }

} // namespace

struct OkaruDeviceMonitor::Impl {
  NotificationManager& notifications;
  GMainContext* context = nullptr;
  GVolumeMonitor* monitor = nullptr;
  // Volúmenes avisados → id de su notificación de «conectado» (la cierra al quitarlo).
  std::unordered_map<GVolume*, std::uint32_t> notified;

  std::vector<GPollFD> glibPollFds;
  gint glibMaxPriority = G_PRIORITY_DEFAULT;
  int glibPollTimeoutMs = -1;

  explicit Impl(NotificationManager& n) : notifications(n) {
    context = g_main_context_new();
    // El monitor emite sus señales en el contexto que era thread-default al crearlo.
    g_main_context_push_thread_default(context);
    monitor = g_volume_monitor_get();
    g_signal_connect(monitor, "volume-added", G_CALLBACK(+[](GVolumeMonitor*, GVolume* v, gpointer self) {
      static_cast<Impl*>(self)->onAdded(v);
    }), this);
    g_signal_connect(monitor, "volume-removed", G_CALLBACK(+[](GVolumeMonitor*, GVolume* v, gpointer self) {
      static_cast<Impl*>(self)->onRemoved(v);
    }), this);
    g_main_context_pop_thread_default(context);

    notifications.addInternalActionCallback(
        [this](std::uint32_t id, const std::string& actionKey, const std::string& token, const std::string&) {
          onAction(id, actionKey, token);
        }
    );
  }

  ~Impl() {
    g_signal_handlers_disconnect_by_data(monitor, this);
    for (auto& [volume, id] : notified) {
      (void)id;
      g_object_unref(volume);
    }
    g_object_unref(monitor);
    g_main_context_unref(context);
  }

  void onAdded(GVolume* volume) {
    if (!isRemovable(volume) || notified.contains(volume)) {
      return;
    }
    const std::string open = i18n::tr("notifications.internal.device-open");
    NotificationRequest request;
    request.appName = i18n::tr("notifications.internal.devices");
    request.summary = i18n::tr(
        isPhone(volume) ? "notifications.internal.device-phone-connected" : "notifications.internal.device-connected"
    );
    request.body = volumeName(volume);
    request.origin = NotificationOrigin::Internal;
    request.icon = std::string(isPhone(volume) ? "okaru-glyph:device-mobile" : "okaru-glyph:usb");
    request.actions = {"default", open, "open", open};
    const std::uint32_t id = notifications.addOrReplace(std::move(request));
    if (id != 0) {
      notified.emplace(G_VOLUME(g_object_ref(volume)), id);
    }
  }

  void onRemoved(GVolume* volume) {
    const auto it = notified.find(volume);
    if (it == notified.end()) {
      return;
    }
    (void)notifications.close(it->second);
    (void)notifications.addInternal(
        i18n::tr("notifications.internal.devices"), i18n::tr("notifications.internal.device-removed"),
        volumeName(volume), Urgency::Normal, kRemovedTimeoutMs,
        std::string(isPhone(volume) ? "okaru-glyph:device-mobile" : "okaru-glyph:usb")
    );
    g_object_unref(it->first);
    notified.erase(it);
  }

  void onAction(std::uint32_t id, const std::string& actionKey, const std::string& token) {
    if (actionKey != "default" && actionKey != "open") {
      return;
    }
    for (const auto& [volume, notificationId] : notified) {
      if (notificationId != id) {
        continue;
      }
      if (g_autoptr(GMount) mount = g_volume_get_mount(volume); mount != nullptr) {
        openMount(mount, token);
        return;
      }
      // El callback corre en nuestro contexto: el push hace que el GTask del montaje lo capture.
      g_main_context_push_thread_default(context);
      g_volume_mount(volume, G_MOUNT_MOUNT_NONE, nullptr, nullptr, onMounted, new MountRequest{token});
      g_main_context_pop_thread_default(context);
      return;
    }
  }

  void addPollFds(std::vector<pollfd>& fds) {
    if (g_main_context_acquire(context) == FALSE) {
      return;
    }
    const gboolean ready = g_main_context_prepare(context, &glibMaxPriority);
    gint timeout = -1;
    const gint count = g_main_context_query(context, glibMaxPriority, &timeout, nullptr, 0);
    glibPollFds.resize(static_cast<std::size_t>(count));
    if (count > 0) {
      g_main_context_query(context, glibMaxPriority, &timeout, glibPollFds.data(), count);
    }
    glibPollTimeoutMs = ready != FALSE ? 0 : timeout;
    for (const GPollFD& glibFd : glibPollFds) {
      fds.push_back({.fd = glibFd.fd, .events = static_cast<short>(glibFd.events), .revents = 0});
    }
    g_main_context_release(context);
  }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (g_main_context_acquire(context) == FALSE) {
      return;
    }
    g_main_context_push_thread_default(context);
    for (std::size_t i = 0; i < glibPollFds.size(); ++i) {
      const std::size_t pollIndex = startIdx + i;
      glibPollFds[i].revents = pollIndex < fds.size() ? static_cast<gushort>(fds[pollIndex].revents) : 0;
    }
    if (g_main_context_check(context, glibMaxPriority, glibPollFds.data(), static_cast<gint>(glibPollFds.size()))
        != FALSE) {
      g_main_context_dispatch(context);
    }
    g_main_context_pop_thread_default(context);
    g_main_context_release(context);
  }
};

OkaruDeviceMonitor::OkaruDeviceMonitor(NotificationManager& notifications)
    : m_impl(std::make_unique<Impl>(notifications)) {}

OkaruDeviceMonitor::~OkaruDeviceMonitor() = default;

int OkaruDeviceMonitor::pollTimeoutMs() const { return m_impl->glibPollTimeoutMs; }

void OkaruDeviceMonitor::doAddPollFds(std::vector<pollfd>& fds) { m_impl->addPollFds(fds); }

void OkaruDeviceMonitor::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  m_impl->dispatch(fds, startIdx);
}
