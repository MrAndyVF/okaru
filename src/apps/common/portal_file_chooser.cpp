#include "apps/common/portal_file_chooser.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "util/string_utils.h"

#include <algorithm>
#include <format>
#include <map>
#include <sdbus-c++/sdbus-c++.h>

namespace okaru::apps {

  namespace {

    constexpr Logger kLog("portal");
    constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
    constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";

    using FilterRule = sdbus::Struct<std::uint32_t, std::string>;
    using Filter = sdbus::Struct<std::string, std::vector<FilterRule>>;

  } // namespace

  std::string pathFromFileUri(std::string_view uri) {
    constexpr std::string_view kScheme = "file://";
    if (!uri.starts_with(kScheme)) {
      return {};
    }
    uri.remove_prefix(kScheme.size());
    // file://host/ruta: el host local puede venir vacío o como "localhost".
    if (const auto slash = uri.find('/'); slash != 0 && slash != std::string_view::npos) {
      uri.remove_prefix(slash);
    }
    return StringUtils::urlDecode(uri);
  }

  PortalFileChooser::PortalFileChooser(SessionBus& bus) : m_bus(bus) {}

  PortalFileChooser::~PortalFileChooser() = default;

  void
  PortalFileChooser::open(const std::string& title, const std::vector<FileFilter>& filters, bool multiple, Done done) {
    auto& connection = m_bus.connection();
    // La ruta de la respuesta se conoce de antemano (handle_token); suscribirse antes de llamar evita perderla.
    std::string sender = connection.getUniqueName();
    sender.erase(0, sender.starts_with(':') ? 1 : 0);
    std::ranges::replace(sender, '.', '_');
    const std::string token = std::format("okaru{}", ++m_token);
    const std::string requestPath = std::format("{}/request/{}/{}", kPortalPath, sender, token);

    try {
      m_request = sdbus::createProxy(connection, sdbus::ServiceName{kPortalService}, sdbus::ObjectPath{requestPath});
      m_request->uponSignal("Response")
          .onInterface("org.freedesktop.portal.Request")
          .call([this, done](std::uint32_t response, const std::map<std::string, sdbus::Variant>& results) {
            std::vector<std::string> paths;
            if (response == 0) {
              if (const auto it = results.find("uris"); it != results.end()) {
                for (const auto& uri : it->second.get<std::vector<std::string>>()) {
                  if (auto path = pathFromFileUri(uri); !path.empty()) {
                    paths.push_back(std::move(path));
                  }
                }
              }
            }
            // El proxy no se puede destruir dentro de su propia señal.
            DeferredCall::callLater([this, done, paths = std::move(paths)] mutable {
              m_request.reset();
              done(std::move(paths));
            });
          });

      std::vector<Filter> portalFilters;
      for (const auto& filter : filters) {
        std::vector<FilterRule> rules;
        for (const auto& mime : filter.mimeTypes) {
          rules.emplace_back(1U, mime); // 1 = tipo MIME (0 sería patrón glob)
        }
        portalFilters.emplace_back(filter.name, std::move(rules));
      }
      std::map<std::string, sdbus::Variant> options{
          {"handle_token", sdbus::Variant(token)},
          {"multiple", sdbus::Variant(multiple)},
      };
      if (!portalFilters.empty()) {
        options.emplace("filters", sdbus::Variant(portalFilters));
      }
      auto portal = sdbus::createProxy(connection, sdbus::ServiceName{kPortalService}, sdbus::ObjectPath{kPortalPath});
      sdbus::ObjectPath handle;
      portal->callMethod("OpenFile")
          .onInterface("org.freedesktop.portal.FileChooser")
          .withArguments(std::string(), title, options)
          .storeResultsTo(handle);
    } catch (const sdbus::Error& e) {
      kLog.warn("file chooser portal unavailable: {}", e.what());
      m_request.reset();
      DeferredCall::callLater([done] { done({}); });
    }
  }

} // namespace okaru::apps
