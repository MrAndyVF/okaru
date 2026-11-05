#pragma once

// OKARU: servicio nativo de KDE Connect (reemplaza al módulo Luau phone-connect).
// Habla con kdeconnectd por el bus de sesión sin procesos externos. kdeconnectd declara sus
// propiedades como "emits-change" pero no emite PropertiesChanged: el estado se mantiene con sus
// señales propias (refreshed, reachableChanged, pairStateChanged, ...), que ya traen el valor nuevo.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
  class Variant;
} // namespace sdbus

struct PhoneDevice {
  std::string id;
  std::string name;
  std::string type; // "phone", "tablet", "desktop", "laptop", "tv"
  bool reachable = false;
  bool paired = false;
  bool pairRequested = false;       // este equipo pidió emparejar y espera respuesta
  bool pairRequestedByPeer = false; // el teléfono pide emparejar: hay que aceptar o rechazar
  std::string verificationKey;
  std::vector<std::string> addresses;
  std::vector<std::string> plugins; // supportedPlugins ("kdeconnect_battery", ...)
  int battery = -1;                 // -1 = sin dato
  bool charging = false;
  std::string networkType;
  int networkStrength = -1; // 0-4, -1 = sin dato
  bool clipboardAutoShare = false;
  int notifications = 0; // notificaciones activas en el teléfono

  [[nodiscard]] bool hasPlugin(std::string_view plugin) const;
  bool operator==(const PhoneDevice&) const = default;
};

namespace kdeconnect {
  // Partes puras (con prueba en tests/okaru_kdeconnect_test.cpp).
  using PropertyMap = std::map<std::string, sdbus::Variant>;
  void applyDeviceProps(PhoneDevice& device, const PropertyMap& props);
  void applyBatteryProps(PhoneDevice& device, const PropertyMap& props);
  void applyConnectivityProps(PhoneDevice& device, const PropertyMap& props);
  // Primera dirección IPv4 (ipv6=false) o IPv6 (ipv6=true) de la lista; vacío si no hay.
  [[nodiscard]] std::string firstAddress(const std::vector<std::string>& addresses, bool ipv6);
  // Fija key=value en la sección [General] de un INI de QSettings, conservando el resto del archivo.
  [[nodiscard]] std::string setIniGeneralKey(std::string_view ini, std::string_view key, std::string_view value);
  // Ruta local -> URL file:// con los caracteres reservados codificados (shareUrl espera una URL).
  [[nodiscard]] std::string fileUrl(std::string_view path);
} // namespace kdeconnect

class KdeConnectService {
public:
  using ChangeCallback = std::function<void()>;
  // ok=false trae el error de D-Bus en detail.
  using ResultCallback = std::function<void(bool ok, const std::string& detail)>;

  explicit KdeConnectService(SessionBus& bus);
  ~KdeConnectService();

  KdeConnectService(const KdeConnectService&) = delete;
  KdeConnectService& operator=(const KdeConnectService&) = delete;

  void setChangeCallback(ChangeCallback callback);

  // true cuando kdeconnectd está en el bus.
  [[nodiscard]] bool available() const noexcept { return m_available; }
  // En el orden en que los devuelve kdeconnectd.
  [[nodiscard]] const std::vector<PhoneDevice>& devices() const noexcept { return m_devices; }
  [[nodiscard]] const PhoneDevice* find(std::string_view id) const;

  void ring(const std::string& id, ResultCallback done = {});
  void ping(const std::string& id, const std::string& message = {}, ResultCallback done = {});
  void sendClipboard(const std::string& id, ResultCallback done = {});
  void shareText(const std::string& id, const std::string& text, ResultCallback done = {});
  void shareUrl(const std::string& id, const std::string& url, ResultCallback done = {});
  void shareFiles(const std::string& id, const std::vector<std::string>& paths, ResultCallback done = {});
  // Monta el almacenamiento por SFTP y kdeconnectd lo abre en el gestor de archivos.
  void browse(const std::string& id, ResultCallback done = {});
  void sendSms(const std::string& id, const std::string& destination, const std::string& text,
               ResultCallback done = {});
  void launchSmsApp(const std::string& id, ResultCallback done = {});

  void requestPairing(const std::string& id, ResultCallback done = {});
  void acceptPairing(const std::string& id, ResultCallback done = {});
  void cancelPairing(const std::string& id, ResultCallback done = {});
  void unpair(const std::string& id, ResultCallback done = {});
  void setPluginEnabled(const std::string& id, const std::string& plugin, bool enabled, ResultCallback done = {});
  // Envío automático del portapapeles de la PC al teléfono: escribe autoShare en la configuración del
  // complemento (~/.config/kdeconnect/<id>/kdeconnect_clipboard/config) y avisa a kdeconnectd.
  void setClipboardAutoShare(const std::string& id, bool enabled, ResultCallback done = {});

  // Vuelve a pedir la lista y todas las propiedades (al reanudar, o desde IPC).
  void refresh();

private:
  struct DeviceProxies;

  void watchBusName();
  void resyncDevices();
  void syncDeviceList(const std::vector<std::string>& ids);
  void subscribeDevice(const std::string& id);
  void fetchDevice(const std::string& id);
  void fetchExtras(const std::string& id);
  void fetchAll(const std::string& id, const std::string& subPath, const char* interface,
                std::function<void(PhoneDevice&, const kdeconnect::PropertyMap&)> apply);
  void fetchNotifications(const std::string& id);
  void setUnavailable();
  PhoneDevice* findMutable(std::string_view id);
  sdbus::IProxy* actionProxy(const std::string& id, const std::string& subPath);
  void scheduleEmit();

  // Llamada asíncrona a un método de /modules/kdeconnect/devices/<id>[/<subPath>].
  template <typename... Args>
  void callDevice(const std::string& id, const std::string& subPath, const char* interface, const char* method,
                  ResultCallback done, Args&&... args);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_dbusProxy;
  std::unique_ptr<sdbus::IProxy> m_daemonProxy;
  std::unordered_map<std::string, std::unique_ptr<DeviceProxies>> m_proxies;
  std::vector<PhoneDevice> m_devices;
  bool m_available = false;
  bool m_resyncQueued = false;
  bool m_emitQueued = false;
  ChangeCallback m_changeCallback;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
