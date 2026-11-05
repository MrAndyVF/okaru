#include "dbus/kdeconnect/kdeconnect_service.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <algorithm>
#include <chrono>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <utility>

namespace {

  constexpr Logger kLog("kdeconnect");

  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  constexpr auto kService = "org.kde.kdeconnect";
  const sdbus::ServiceName kServiceName{kService};
  constexpr auto kDaemonPath = "/modules/kdeconnect";
  constexpr auto kDaemonInterface = "org.kde.kdeconnect.daemon";
  constexpr auto kDeviceInterface = "org.kde.kdeconnect.device";
  constexpr auto kBatteryInterface = "org.kde.kdeconnect.device.battery";
  constexpr auto kConnectivityInterface = "org.kde.kdeconnect.device.connectivity_report";
  constexpr auto kClipboardInterface = "org.kde.kdeconnect.device.clipboard";
  constexpr auto kNotificationsInterface = "org.kde.kdeconnect.device.notifications";

  // SFTP monta y espera al teléfono: puede tardar bastante más que una llamada normal.
  constexpr auto kBrowseTimeout = std::chrono::seconds(60);

  std::string devicePath(const std::string& id, const std::string& subPath = {}) {
    std::string path = std::string{kDaemonPath} + "/devices/" + id;
    if (!subPath.empty()) {
      path += "/" + subPath;
    }
    return path;
  }

  template <typename T> void read(const kdeconnect::PropertyMap& props, const char* key, T& out) {
    const auto it = props.find(key);
    if (it != props.end() && it->second.containsValueOfType<T>()) {
      out = it->second.get<T>();
    }
  }

  void report(const KdeConnectService::ResultCallback& done, const std::optional<sdbus::Error>& err,
              const char* what) {
    if (err.has_value()) {
      kLog.warn("{} failed: {}", what, err->getMessage());
    }
    if (done) {
      done(!err.has_value(), err.has_value() ? err->getMessage() : std::string{});
    }
  }

} // namespace

bool PhoneDevice::hasPlugin(std::string_view plugin) const { return std::ranges::contains(plugins, plugin); }

namespace kdeconnect {

  void applyDeviceProps(PhoneDevice& device, const PropertyMap& props) {
    read(props, "name", device.name);
    read(props, "type", device.type);
    read(props, "isReachable", device.reachable);
    read(props, "isPaired", device.paired);
    read(props, "isPairRequested", device.pairRequested);
    read(props, "isPairRequestedByPeer", device.pairRequestedByPeer);
    read(props, "verificationKey", device.verificationKey);
    read(props, "reachableAddresses", device.addresses);
    read(props, "supportedPlugins", device.plugins);
  }

  void applyBatteryProps(PhoneDevice& device, const PropertyMap& props) {
    read(props, "charge", device.battery);
    read(props, "isCharging", device.charging);
  }

  void applyConnectivityProps(PhoneDevice& device, const PropertyMap& props) {
    read(props, "cellularNetworkType", device.networkType);
    read(props, "cellularNetworkStrength", device.networkStrength);
  }

  std::string firstAddress(const std::vector<std::string>& addresses, bool ipv6) {
    for (const auto& address : addresses) {
      if (address.contains(':') == ipv6) {
        return address;
      }
    }
    return {};
  }

  std::string fileUrl(std::string_view path) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string url = "file://";
    for (const char ch : path) {
      const auto c = static_cast<unsigned char>(ch);
      const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
          || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
      if (plain) {
        url += static_cast<char>(c);
      } else {
        url += '%';
        url += kHex[c >> 4];
        url += kHex[c & 0x0F];
      }
    }
    return url;
  }

} // namespace kdeconnect

struct KdeConnectService::DeviceProxies {
  std::unique_ptr<sdbus::IProxy> device;
  std::unique_ptr<sdbus::IProxy> battery;
  std::unique_ptr<sdbus::IProxy> connectivity;
  std::unique_ptr<sdbus::IProxy> clipboard;
  std::unique_ptr<sdbus::IProxy> notifications;
  // Proxies de acciones (ping, share, sftp, ...): se crean al primer uso y viven con el dispositivo.
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> actions;
};

sdbus::IProxy* KdeConnectService::actionProxy(const std::string& id, const std::string& subPath) {
  const auto it = m_proxies.find(id);
  if (it == m_proxies.end()) {
    return nullptr;
  }
  auto& proxy = it->second->actions[subPath];
  if (!proxy) {
    proxy = sdbus::createProxy(m_bus.connection(), kServiceName, sdbus::ObjectPath{devicePath(id, subPath)});
  }
  return proxy.get();
}

KdeConnectService::KdeConnectService(SessionBus& bus)
    : m_bus(bus), m_dbusProxy(sdbus::createProxy(bus.connection(), kDbusName, kDbusPath)),
      m_daemonProxy(sdbus::createProxy(bus.connection(), kServiceName, sdbus::ObjectPath{kDaemonPath})) {
  // Cualquier cambio en la lista (alta, baja, visibilidad, pedido de emparejamiento) vuelve a leerla.
  auto onListChanged = [this] { resyncDevices(); };
  m_daemonProxy->uponSignal("deviceAdded").onInterface(kDaemonInterface).call([onListChanged](const std::string&) {
    onListChanged();
  });
  m_daemonProxy->uponSignal("deviceRemoved").onInterface(kDaemonInterface).call([onListChanged](const std::string&) {
    onListChanged();
  });
  m_daemonProxy->uponSignal("deviceVisibilityChanged")
      .onInterface(kDaemonInterface)
      .call([onListChanged](const std::string&, bool) { onListChanged(); });
  m_daemonProxy->uponSignal("deviceListChanged").onInterface(kDaemonInterface).call(onListChanged);
  m_daemonProxy->uponSignal("pairingRequestsChanged").onInterface(kDaemonInterface).call(onListChanged);

  watchBusName();
}

KdeConnectService::~KdeConnectService() = default;

void KdeConnectService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

const PhoneDevice* KdeConnectService::find(std::string_view id) const {
  const auto it = std::ranges::find(m_devices, id, &PhoneDevice::id);
  return it != m_devices.end() ? &*it : nullptr;
}

PhoneDevice* KdeConnectService::findMutable(std::string_view id) {
  const auto it = std::ranges::find(m_devices, id, &PhoneDevice::id);
  return it != m_devices.end() ? &*it : nullptr;
}

void KdeConnectService::watchBusName() {
  m_dbusProxy->uponSignal("NameOwnerChanged")
      .onInterface(kDbusInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kService) {
          return;
        }
        if (newOwner.empty()) {
          kLog.info("kdeconnectd left the bus");
          setUnavailable();
        } else {
          kLog.info("kdeconnectd appeared on the bus");
          resyncDevices();
        }
      });

  const std::weak_ptr<void> alive = m_aliveGuard;
  try {
    m_dbusProxy->callMethodAsync("NameHasOwner")
        .onInterface(kDbusInterface)
        .withArguments(std::string{kService})
        .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, // NOLINT(performance-unnecessary-value-param)
 bool owned) {
          if (alive.expired()) {
            return;
          }
          if (!err.has_value() && owned) {
            resyncDevices();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("NameHasOwner dispatch failed: {}", e.what());
  }
}

void KdeConnectService::refresh() {
  for (const auto& device : m_devices) {
    fetchDevice(device.id);
  }
  resyncDevices();
}

void KdeConnectService::resyncDevices() {
  // Las señales del daemon llegan en ráfagas (varias por cambio): una sola lectura por vuelta del bucle.
  if (m_resyncQueued) {
    return;
  }
  m_resyncQueued = true;
  const std::weak_ptr<void> alive = m_aliveGuard;
  DeferredCall::callLater([this, alive] {
    if (alive.expired()) {
      return;
    }
    m_resyncQueued = false;
    try {
      m_daemonProxy->callMethodAsync("devices")
          .onInterface(kDaemonInterface)
          .withArguments(false, false)
          .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, // NOLINT(performance-unnecessary-value-param)
 const std::vector<std::string>& ids) {
            if (alive.expired()) {
              return;
            }
            if (err.has_value()) {
              kLog.warn("devices() failed: {}", err->getMessage());
              setUnavailable();
              return;
            }
            syncDeviceList(ids);
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("devices() dispatch failed: {}", e.what());
    }
  });
}

void KdeConnectService::syncDeviceList(const std::vector<std::string>& ids) {
  const bool wasAvailable = m_available;
  m_available = true;
  if (!wasAvailable) {
    kLog.info("kdeconnectd ready, {} device(s)", ids.size());
  }

  std::vector<PhoneDevice> next;
  next.reserve(ids.size());
  for (const auto& id : ids) {
    if (const auto* existing = find(id)) {
      next.push_back(*existing);
    } else {
      next.push_back(PhoneDevice{.id = id});
    }
  }
  std::erase_if(m_proxies, [&ids](const auto& entry) { return !std::ranges::contains(ids, entry.first); });
  const bool changed = !wasAvailable || next != m_devices;
  m_devices = std::move(next);

  for (const auto& id : ids) {
    if (!m_proxies.contains(id)) {
      subscribeDevice(id);
    }
    // Sale barato (una llamada por interfaz) y cubre lo que cambió sin señal (emparejamiento, direcciones).
    fetchDevice(id);
  }
  if (changed) {
    scheduleEmit();
  }
}

void KdeConnectService::subscribeDevice(const std::string& id) {
  auto proxies = std::make_unique<DeviceProxies>();
  auto make = [this, &id](const std::string& subPath) {
    return sdbus::createProxy(m_bus.connection(), kServiceName, sdbus::ObjectPath{devicePath(id, subPath)});
  };

  try {
    proxies->device = make({});
    proxies->device->uponSignal("reachableChanged").onInterface(kDeviceInterface).call([this, id](bool reachable) {
      if (auto* d = findMutable(id)) {
        d->reachable = reachable;
        scheduleEmit();
      }
      if (reachable) {
        fetchExtras(id);
      }
    });
    proxies->device->uponSignal("nameChanged").onInterface(kDeviceInterface).call([this, id](const std::string& name) {
      if (auto* d = findMutable(id)) {
        d->name = name;
        scheduleEmit();
      }
    });
    proxies->device->uponSignal("typeChanged").onInterface(kDeviceInterface).call([this, id](const std::string& type) {
      if (auto* d = findMutable(id)) {
        d->type = type;
        scheduleEmit();
      }
    });
    // Estos no traen todo el estado nuevo: se relee la interfaz del dispositivo.
    proxies->device->uponSignal("pairStateChanged").onInterface(kDeviceInterface).call([this, id](int) {
      fetchDevice(id);
    });
    proxies->device->uponSignal("linksChanged").onInterface(kDeviceInterface).call([this, id] { fetchDevice(id); });
    proxies->device->uponSignal("pluginsChanged").onInterface(kDeviceInterface).call([this, id] { fetchDevice(id); });

    proxies->battery = make("battery");
    proxies->battery->uponSignal("refreshed")
        .onInterface(kBatteryInterface)
        .call([this, id](bool charging, int charge) {
          if (auto* d = findMutable(id)) {
            d->charging = charging;
            d->battery = charge;
            scheduleEmit();
          }
        });

    proxies->connectivity = make("connectivity_report");
    proxies->connectivity->uponSignal("refreshed")
        .onInterface(kConnectivityInterface)
        .call([this, id](const std::string& type, int strength) {
          if (auto* d = findMutable(id)) {
            d->networkType = type;
            d->networkStrength = strength;
            scheduleEmit();
          }
        });

    proxies->clipboard = make("clipboard");
    proxies->clipboard->uponSignal("autoShareDisabledChanged")
        .onInterface(kClipboardInterface)
        .call([this, id](bool disabled) {
          if (auto* d = findMutable(id)) {
            d->clipboardAutoShare = !disabled;
            scheduleEmit();
          }
        });

    proxies->notifications = make("notifications");
    for (const char* signal : {"notificationPosted", "notificationRemoved"}) {
      proxies->notifications->uponSignal(signal).onInterface(kNotificationsInterface).call(
          [this, id](const std::string&) { fetchNotifications(id); }
      );
    }
    proxies->notifications->uponSignal("allNotificationsRemoved")
        .onInterface(kNotificationsInterface)
        .call([this, id] {
          if (auto* d = findMutable(id)) {
            d->notifications = 0;
            scheduleEmit();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("subscribe {} failed: {}", id, e.what());
  }
  m_proxies[id] = std::move(proxies);
}

void KdeConnectService::fetchAll(const std::string& id, const std::string& subPath, const char* interface,
                                 std::function<void(PhoneDevice&, const kdeconnect::PropertyMap&)> apply) {
  const auto it = m_proxies.find(id);
  if (it == m_proxies.end()) {
    return;
  }
  sdbus::IProxy* proxy = subPath.empty()                ? it->second->device.get()
      : subPath == "battery"                            ? it->second->battery.get()
      : subPath == "connectivity_report"                ? it->second->connectivity.get()
      : subPath == "clipboard"                          ? it->second->clipboard.get()
                                                        : nullptr;
  if (proxy == nullptr) {
    return;
  }
  const std::weak_ptr<void> alive = m_aliveGuard;
  try {
    proxy->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{interface})
        .uponReplyInvoke([this, alive, id, apply = std::move(apply)](std::optional<sdbus::Error> err, // NOLINT(performance-unnecessary-value-param)

                                                                     const kdeconnect::PropertyMap& props) {
          if (alive.expired() || err.has_value()) {
            // Sin el complemento (o sin emparejar) la interfaz no existe: no es un error.
            return;
          }
          auto* d = findMutable(id);
          if (d == nullptr) {
            return;
          }
          const PhoneDevice before = *d;
          apply(*d, props);
          if (*d != before) {
            scheduleEmit();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("GetAll {} {} dispatch failed: {}", id, interface, e.what());
  }
}

void KdeConnectService::fetchDevice(const std::string& id) {
  const std::weak_ptr<void> alive = m_aliveGuard;
  fetchAll(id, {}, kDeviceInterface, [this, alive, id](PhoneDevice& d, const kdeconnect::PropertyMap& props) {
    const bool wasUsable = d.paired && d.reachable;
    kdeconnect::applyDeviceProps(d, props);
    if (!wasUsable && d.paired && d.reachable) {
      // Recién conectado: batería, red y portapapeles al momento (fuera del callback de GetAll).
      DeferredCall::callLater([this, alive, id] {
        if (!alive.expired()) {
          fetchExtras(id);
        }
      });
    }
  });
}

void KdeConnectService::fetchExtras(const std::string& id) {
  fetchAll(id, "battery", kBatteryInterface, kdeconnect::applyBatteryProps);
  fetchAll(id, "connectivity_report", kConnectivityInterface, kdeconnect::applyConnectivityProps);
  fetchAll(id, "clipboard", kClipboardInterface, [](PhoneDevice& d, const kdeconnect::PropertyMap& props) {
    bool disabled = true;
    read(props, "isAutoShareDisabled", disabled);
    d.clipboardAutoShare = !disabled;
  });
  fetchNotifications(id);
}

void KdeConnectService::fetchNotifications(const std::string& id) {
  const auto it = m_proxies.find(id);
  if (it == m_proxies.end() || !it->second->notifications) {
    return;
  }
  const std::weak_ptr<void> alive = m_aliveGuard;
  try {
    it->second->notifications->callMethodAsync("activeNotifications")
        .onInterface(kNotificationsInterface)
        .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> err, // NOLINT(performance-unnecessary-value-param)
                                           const std::vector<std::string>& active) {
          if (alive.expired() || err.has_value()) {
            return;
          }
          if (auto* d = findMutable(id); d != nullptr && d->notifications != static_cast<int>(active.size())) {
            d->notifications = static_cast<int>(active.size());
            scheduleEmit();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("activeNotifications {} dispatch failed: {}", id, e.what());
  }
}

void KdeConnectService::setUnavailable() {
  if (!m_available && m_devices.empty()) {
    return;
  }
  m_available = false;
  m_devices.clear();
  m_proxies.clear();
  scheduleEmit();
}

void KdeConnectService::scheduleEmit() {
  if (m_emitQueued) {
    return;
  }
  m_emitQueued = true;
  const std::weak_ptr<void> alive = m_aliveGuard;
  DeferredCall::callLater([this, alive] {
    if (alive.expired()) {
      return;
    }
    m_emitQueued = false;
    if (m_changeCallback) {
      m_changeCallback();
    }
  });
}

template <typename... Args>
void KdeConnectService::callDevice(const std::string& id, const std::string& subPath, const char* interface,
                                   const char* method, ResultCallback done, Args&&... args) {
  try {
    sdbus::IProxy* proxy = actionProxy(id, subPath);
    if (proxy == nullptr) {
      if (done) {
        done(false, "unknown device");
      }
      return;
    }
    proxy->callMethodAsync(method)
        .onInterface(interface)
        .withArguments(std::forward<Args>(args)...)
        .uponReplyInvoke([done = std::move(done), method](std::optional<sdbus::Error> err) { // NOLINT(performance-unnecessary-value-param)
          report(done, err, method);
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("{} dispatch failed: {}", method, e.what());
    if (done) {
      done(false, e.getMessage());
    }
  }
}

void KdeConnectService::ring(const std::string& id, ResultCallback done) {
  callDevice(id, "findmyphone", "org.kde.kdeconnect.device.findmyphone", "ring", std::move(done));
}

void KdeConnectService::ping(const std::string& id, const std::string& message, ResultCallback done) {
  if (message.empty()) {
    callDevice(id, "ping", "org.kde.kdeconnect.device.ping", "sendPing", std::move(done));
  } else {
    callDevice(id, "ping", "org.kde.kdeconnect.device.ping", "sendPing", std::move(done), message);
  }
}

void KdeConnectService::sendClipboard(const std::string& id, ResultCallback done) {
  callDevice(id, "clipboard", kClipboardInterface, "sendClipboard", std::move(done));
}

void KdeConnectService::shareText(const std::string& id, const std::string& text, ResultCallback done) {
  callDevice(id, "share", "org.kde.kdeconnect.device.share", "shareText", std::move(done), text);
}

void KdeConnectService::shareUrl(const std::string& id, const std::string& url, ResultCallback done) {
  callDevice(id, "share", "org.kde.kdeconnect.device.share", "shareUrl", std::move(done), url);
}

void KdeConnectService::shareFiles(const std::string& id, const std::vector<std::string>& paths, ResultCallback done) {
  std::vector<std::string> urls;
  urls.reserve(paths.size());
  for (const auto& path : paths) {
    urls.push_back(kdeconnect::fileUrl(path));
  }
  callDevice(id, "share", "org.kde.kdeconnect.device.share", "shareUrls", std::move(done), urls);
}

void KdeConnectService::browse(const std::string& id, ResultCallback done) {
  try {
    sdbus::IProxy* proxy = actionProxy(id, "sftp");
    if (proxy == nullptr) {
      if (done) {
        done(false, "unknown device");
      }
      return;
    }
    proxy->callMethodAsync("startBrowsing")
        .onInterface("org.kde.kdeconnect.device.sftp")
        .withTimeout(kBrowseTimeout)
        .uponReplyInvoke([done = std::move(done)](std::optional<sdbus::Error> err, // NOLINT(performance-unnecessary-value-param)
 bool ok) {
          if (!err.has_value() && !ok) {
            // El montaje falló sin error de D-Bus (teléfono bloqueado, sshfs ausente...).
            err = sdbus::Error{sdbus::Error::Name{"dev.okaru.KdeConnect.MountFailed"}, "mount failed"};
          }
          report(done, err, "startBrowsing");
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("startBrowsing dispatch failed: {}", e.what());
    if (done) {
      done(false, e.getMessage());
    }
  }
}

void KdeConnectService::sendSms(const std::string& id, const std::string& destination, const std::string& text,
                                ResultCallback done) {
  // sendSms(av addresses, s text, av attachments): cada dirección es un ConversationAddress = (s).
  const std::vector<sdbus::Variant> addresses{sdbus::Variant{sdbus::Struct<std::string>{destination}}};
  callDevice(id, "sms", "org.kde.kdeconnect.device.sms", "sendSms", std::move(done), addresses, text,
             std::vector<sdbus::Variant>{});
}

void KdeConnectService::launchSmsApp(const std::string& id, ResultCallback done) {
  callDevice(id, "sms", "org.kde.kdeconnect.device.sms", "launchApp", std::move(done));
}

void KdeConnectService::requestPairing(const std::string& id, ResultCallback done) {
  callDevice(id, {}, kDeviceInterface, "requestPairing", std::move(done));
}

void KdeConnectService::acceptPairing(const std::string& id, ResultCallback done) {
  callDevice(id, {}, kDeviceInterface, "acceptPairing", std::move(done));
}

void KdeConnectService::cancelPairing(const std::string& id, ResultCallback done) {
  callDevice(id, {}, kDeviceInterface, "cancelPairing", std::move(done));
}

void KdeConnectService::unpair(const std::string& id, ResultCallback done) {
  callDevice(id, {}, kDeviceInterface, "unpair", std::move(done));
}

void KdeConnectService::setPluginEnabled(const std::string& id, const std::string& plugin, bool enabled,
                                         ResultCallback done) {
  callDevice(id, {}, kDeviceInterface, "setPluginEnabled", std::move(done), plugin, enabled);
}
