#include "apps/common/file_drop.h"

#include "apps/common/portal_file_chooser.h"
#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

namespace okaru::apps {

  namespace {

    constexpr Logger kLog("drop");
    constexpr const char* kUriList = "text/uri-list";
    constexpr int kReadTimeoutMs = 2000;

    FileDrop* self(void* data) { return static_cast<FileDrop*>(data); }

    void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
      if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        const auto bindVersion = std::min<std::uint32_t>(version, 3);
        self(data)->onBind(
            static_cast<wl_data_device_manager*>(
                wl_registry_bind(registry, name, &wl_data_device_manager_interface, bindVersion)
            ),
            bindVersion
        );
      }
    }

    void onGlobalRemove(void* /*data*/, wl_registry* /*registry*/, std::uint32_t /*name*/) {}

    const wl_registry_listener kRegistryListener = {.global = onGlobal, .global_remove = onGlobalRemove};

    const wl_data_offer_listener kOfferListener = {
        .offer = [](void* data, wl_data_offer* offer, const char* mime) { self(data)->onOfferMime(offer, mime); },
        .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
        .action = [](void*, wl_data_offer*, std::uint32_t) {},
    };

    const wl_data_device_listener kDeviceListener = {
        .data_offer = [](void* data, wl_data_device*, wl_data_offer* offer) { self(data)->onOffer(offer); },
        .enter = [](void* data, wl_data_device*, std::uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t,
                    wl_data_offer* offer) { self(data)->onEnter(serial, offer); },
        .leave = [](void* data, wl_data_device*) { self(data)->onLeave(); },
        .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
        .drop = [](void* data, wl_data_device*) { self(data)->onDrop(); },
        .selection = [](void* data, wl_data_device*, wl_data_offer* offer) { self(data)->onSelection(offer); },
    };

    // shortcut: lectura bloqueante con tope de 2 s; basta para listas de rutas. Pasar al bucle si llega a trabarse.
    std::string readAll(int fd) {
      std::string out;
      char buffer[4096];
      pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
      while (::poll(&pfd, 1, kReadTimeoutMs) > 0) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        out.append(buffer, static_cast<std::size_t>(n));
      }
      return out;
    }

  } // namespace

  std::vector<std::string> parseUriList(std::string_view list) {
    std::vector<std::string> paths;
    while (!list.empty()) {
      const auto end = list.find('\n');
      std::string_view line = list.substr(0, end);
      list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
      }
      if (line.empty() || line.front() == '#') {
        continue;
      }
      if (line.starts_with("file://")) {
        if (auto path = pathFromFileUri(line); !path.empty()) {
          paths.push_back(std::move(path));
        }
      } else {
        paths.emplace_back(line);
      }
    }
    return paths;
  }

  FileDrop::~FileDrop() {
    onLeave();
    for (auto* offer : m_uriOffers) {
      wl_data_offer_destroy(offer);
    }
    if (m_device != nullptr) {
      m_version >= 2 ? wl_data_device_release(m_device) : wl_data_device_destroy(m_device);
    }
    if (m_manager != nullptr) {
      wl_data_device_manager_destroy(m_manager);
    }
  }

  void FileDrop::attach(wl_display* display, wl_seat* seat) {
    m_display = display;
    if (display == nullptr || seat == nullptr) {
      return;
    }
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, this);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
    if (m_manager == nullptr) {
      kLog.warn("no wl_data_device_manager: dropping files is disabled");
      return;
    }
    m_device = wl_data_device_manager_get_data_device(m_manager, seat);
    wl_data_device_add_listener(m_device, &kDeviceListener, this);
  }

  void FileDrop::onBind(wl_data_device_manager* manager, unsigned version) {
    m_manager = manager;
    m_version = version;
  }

  void FileDrop::onOffer(wl_data_offer* offer) { wl_data_offer_add_listener(offer, &kOfferListener, this); }

  void FileDrop::onOfferMime(wl_data_offer* offer, const char* mime) {
    if (std::strcmp(mime, kUriList) == 0) {
      m_uriOffers.insert(offer);
    }
  }

  void FileDrop::onEnter(unsigned serial, wl_data_offer* offer) {
    onLeave();
    m_dragOffer = offer;
    if (offer == nullptr) {
      return;
    }
    const bool accepted = m_uriOffers.contains(offer);
    wl_data_offer_accept(offer, serial, accepted ? kUriList : nullptr);
    if (m_version >= 3) {
      const std::uint32_t copy = accepted ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0;
      wl_data_offer_set_actions(offer, copy, copy);
    }
  }

  void FileDrop::onLeave() {
    if (m_dragOffer != nullptr) {
      dropOffer(m_dragOffer);
      m_dragOffer = nullptr;
    }
  }

  void FileDrop::onDrop() {
    wl_data_offer* offer = m_dragOffer;
    m_dragOffer = nullptr;
    if (offer == nullptr) {
      return;
    }
    std::vector<std::string> paths;
    int fds[2];
    if (m_uriOffers.contains(offer) && ::pipe2(fds, O_CLOEXEC) == 0) {
      wl_data_offer_receive(offer, kUriList, fds[1]);
      ::close(fds[1]);
      wl_display_flush(m_display);
      paths = parseUriList(readAll(fds[0]));
      ::close(fds[0]);
      if (m_version >= 3) {
        wl_data_offer_finish(offer);
      }
    }
    dropOffer(offer);
    if (!paths.empty() && m_onDrop) {
      // Fuera del despacho de Wayland: abrir puede reconstruir la escena o abrir diálogos.
      DeferredCall::callLater([this, paths = std::move(paths)] mutable { m_onDrop(std::move(paths)); });
    }
  }

  void FileDrop::onSelection(wl_data_offer* offer) {
    // El portapapeles va por wl-clipboard: la oferta de la selección no se usa.
    if (offer != nullptr) {
      dropOffer(offer);
    }
  }

  void FileDrop::dropOffer(wl_data_offer* offer) {
    m_uriOffers.erase(offer);
    wl_data_offer_destroy(offer);
  }

} // namespace okaru::apps
