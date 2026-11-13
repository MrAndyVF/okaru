#include "shell/control_center/tabs/vpn_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <sstream>

using namespace control_center;

namespace {

  constexpr const char* kPanelId = "okaru-vpn";

  std::unique_ptr<Flex> card(float scale, float opacity) {
    return ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure = [scale, opacity](Flex& c) { applySectionCardStyle(c, scale, opacity); },
    });
  }

  std::unique_ptr<Label> caption(float scale, std::string text, ColorRole role = ColorRole::OnSurfaceVariant) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(role),
        .maxLines = 3,
    });
  }

  std::unique_ptr<Button>
  iconButton(const char* glyph, std::string tooltip, std::function<void()> onClick, bool on = false) {
    return ui::button({
        .glyph = glyph,
        .selected = on,
        .variant = on ? ButtonVariant::Secondary : ButtonVariant::Ghost,
        .tooltip = std::move(tooltip),
        .onClick = std::move(onClick),
    });
  }

  // "file:///home/a/b%20c.ovpn" → "/home/a/b c.ovpn"
  std::string pathFromUri(std::string uri) {
    while (!uri.empty() && (uri.back() == '\n' || uri.back() == '\r')) {
      uri.pop_back();
    }
    if (!uri.starts_with("file://")) {
      return uri;
    }
    std::string out;
    for (std::size_t i = 7; i < uri.size(); ++i) {
      if (uri[i] == '%' && i + 2 < uri.size()) {
        out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        out += uri[i];
      }
    }
    return out;
  }

} // namespace

VpnTab::VpnTab(INetworkService* network, ConfigService* config) : m_network(network), m_config(config) {}

std::unique_ptr<Flex> VpnTab::create() {
  const float scale = contentScale();
  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch});
  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
      },
  });
  m_content = scroll->content();
  m_content->setDirection(FlexDirection::Vertical);
  m_content->setAlign(FlexAlign::Stretch);
  m_content->setGap(Style::spaceMd * scale);
  root->addChild(std::move(scroll));
  m_key.clear();
  return root;
}

std::unique_ptr<Flex> VpnTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "file-import",
          .tooltip = i18n::tr("okaru.vpn.import"),
          .onClick = [this] { importVpn(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .glyph = "plus",
          .tooltip = i18n::tr("okaru.vpn.new"),
          .onClick =
              [this] {
                const auto types = vpn::installedTypes();
                // Por defecto, el protocolo de la primera VPN que ya tengas (o el primero instalado).
                std::string type = types.empty() ? std::string("wireguard") : types.front().name;
                if (m_network != nullptr) {
                  for (const auto& v : m_network->vpnConnections()) {
                    if (!v.type.empty()) {
                      type = v.type;
                      break;
                    }
                  }
                }
                startCreate(type);
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void VpnTab::setActive(bool active) {
  if (!active) {
    m_armed.clear();
    return;
  }
  if (m_network != nullptr) {
    m_network->refresh();
  }
  requestRebuild();
}

void VpnTab::onClose() {
  m_root = nullptr;
  m_content = nullptr;
  m_armed.clear();
}

void VpnTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  rebuildIfNeeded();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void VpnTab::doUpdate(Renderer& /*renderer*/) { rebuildIfNeeded(); }

void VpnTab::requestRebuild() {
  m_key.clear();
  PanelManager::instance().refresh();
}

std::string VpnTab::stateKey() const {
  // Editando no entra el estado de la red: un refresco de NetworkManager no debe borrar lo que se escribe.
  if (m_mode == Mode::Edit) {
    return "edit|" + std::to_string(m_editGeneration) + "|" + m_edit.type + "|" + m_message + (m_busy ? "|busy" : "");
  }
  std::string key = "list|" + m_armed + "|" + m_message + (m_busy ? "|busy" : "");
  if (m_network != nullptr) {
    for (const auto& v : m_network->vpnConnections()) {
      key += "|" + v.uuid + v.name + (v.active ? "1" : "0");
    }
    key += "|" + currentNetwork();
  }
  if (m_config != nullptr) {
    const auto& cfg = m_config->config().vpn;
    key += "|" + cfg.defaultConnection + (cfg.autoConnect ? "A" : "a");
    for (const auto& t : cfg.trusted) {
      key += "|t" + t;
    }
  }
  for (const auto& uuid : m_expanded) {
    key += "|x" + uuid;
    if (const auto it = m_details.find(uuid); it != m_details.end()) {
      key += it->second.state + it->second.ipv4;
    }
  }
  return key;
}

void VpnTab::rebuildIfNeeded() {
  if (m_content == nullptr) {
    return;
  }
  std::string key = stateKey();
  if (key == m_key) {
    return;
  }
  m_key = std::move(key);
  while (!m_content->children().empty()) {
    m_content->removeChild(m_content->children().back().get());
  }
  if (!m_message.empty()) {
    m_content->addChild(caption(contentScale(), m_message, m_messageError ? ColorRole::Error : ColorRole::Primary));
  }
  if (m_mode == Mode::Edit) {
    addEditor(*m_content);
    return;
  }
  addConnections(*m_content);
  addAutoConnect(*m_content);
}

void VpnTab::setMessage(std::string message, bool error) {
  m_message = std::move(message);
  m_messageError = error;
  requestRebuild();
}

std::string VpnTab::currentNetwork() const {
  if (m_network == nullptr) {
    return {};
  }
  const NetworkState& state = m_network->state();
  if (!state.connected) {
    return {};
  }
  return state.kind == NetworkConnectivity::Wireless ? state.ssid : state.interfaceName;
}

// ── Lista ────────────────────────────────────────────────────────────────────────────────────────────

void VpnTab::addConnections(Flex& parent) {
  const float scale = contentScale();
  auto c = card(scale, panelCardOpacity());
  c->addChild(makeCardHeaderRow(i18n::tr("okaru.vpn.connections"), scale));
  const auto& vpns = m_network != nullptr ? m_network->vpnConnections() : std::vector<VpnConnectionInfo>{};
  if (vpns.empty()) {
    c->addChild(caption(scale, i18n::tr("okaru.vpn.empty")));
  }
  for (const auto& v : vpns) {
    addConnectionRow(*c, v);
  }
  parent.addChild(std::move(c));
}

void VpnTab::addConnectionRow(Flex& c, const VpnConnectionInfo& v) {
  const float scale = contentScale();
  const bool isDefault = m_config != nullptr && !v.uuid.empty() && m_config->config().vpn.defaultConnection == v.uuid;
  const bool open = m_expanded.contains(v.uuid);
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  row->addChild(
      ui::glyph({
          .glyph = v.active ? "shield-check" : "shield-lock",
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(v.active ? ColorRole::Primary : ColorRole::OnSurfaceVariant),
      })
  );
  auto names = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
  names->addChild(
      ui::label(
          {.text = v.name,
           .fontSize = Style::fontSizeBody * scale,
           .fontWeight = FontWeight::SemiBold,
           .maxLines = 1,
           .ellipsize = TextEllipsize::End}
      )
  );
  names->addChild(caption(scale, v.type.empty() ? std::string("VPN") : vpn::typeLabel(v.type)));
  row->addChild(std::move(names));
  row->addChild(iconButton(
      isDefault ? "star-filled" : "star", i18n::tr("okaru.vpn.default"),
      [this, uuid = v.uuid, isDefault] { setDefault(isDefault ? std::string{} : uuid); }, isDefault
  ));
  row->addChild(iconButton(open ? "chevron-up" : "chevron-down", i18n::tr("okaru.vpn.details"), [this, v, open] {
    if (open) {
      m_expanded.erase(v.uuid);
    } else {
      m_expanded.insert(v.uuid);
      if (v.active) {
        vpn::details(v.uuid, [this, uuid = v.uuid](vpn::VpnDetails details) {
          m_details[uuid] = std::move(details);
          requestRebuild();
        });
      }
    }
    requestRebuild();
  }));
  row->addChild(
      ui::toggle({
          .checked = v.active,
          .scale = scale,
          .onChange = [this, v](bool) { toggle(v); },
      })
  );
  c.addChild(std::move(row));
  if (!open) {
    return;
  }

  auto details = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
  const auto info = [&](const char* key, const std::string& value) {
    if (value.empty()) {
      return;
    }
    details->addChild(
        ui::row(
            {.align = FlexAlign::Start, .gap = Style::spaceMd * scale},
            ui::label(
                {.text = i18n::tr(key),
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .width = 110.0F * scale}
            ),
            ui::label({.text = value, .fontSize = Style::fontSizeCaption * scale, .maxLines = 3, .flexGrow = 1.0F})
        )
    );
  };
  if (v.active) {
    if (const auto it = m_details.find(v.uuid); it != m_details.end()) {
      info("okaru.vpn.state", it->second.state);
      info("okaru.vpn.device", it->second.device);
      info("okaru.vpn.ipv4", it->second.ipv4);
      info("okaru.vpn.gateway", it->second.gateway);
      info("okaru.vpn.dns", it->second.dns);
      info("okaru.vpn.ipv6", it->second.ipv6);
    }
  } else {
    details->addChild(caption(scale, i18n::tr("okaru.vpn.inactive")));
  }
  const bool armed = m_armed == v.uuid;
  details->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
          ui::button(
              {.text = i18n::tr("okaru.vpn.edit"),
               .glyph = "pencil",
               .variant = ButtonVariant::Default,
               .onClick = [this, v] { startEdit(v); }}
          ),
          ui::button(
              {.text = i18n::tr(armed ? "okaru.vpn.confirm-delete" : "okaru.vpn.delete"),
               .glyph = "trash",
               .variant = armed ? ButtonVariant::Destructive : ButtonVariant::Ghost,
               .onClick = [this, uuid = v.uuid, name = v.name, armed] {
                 if (!armed) {
                   m_armed = uuid;
                   requestRebuild();
                   return;
                 }
                 m_armed.clear();
                 vpn::remove(uuid, [this, name](bool ok, const std::string& error) {
                   setMessage(
                       ok ? i18n::tr("okaru.vpn.deleted", "name", name) : i18n::tr("okaru.vpn.failed", "detail", error),
                       !ok
                   );
                   if (m_network != nullptr) {
                     m_network->refresh();
                   }
                 });
               }}
          )
      )
  );
  c.addChild(std::move(details));
}

void VpnTab::toggle(const VpnConnectionInfo& v) {
  if (m_network == nullptr) {
    return;
  }
  const bool ok = v.active ? m_network->deactivateVpnConnection(v) : m_network->activateVpnConnection(v);
  if (!ok) {
    setMessage(i18n::tr("okaru.vpn.failed", "detail", v.name), true);
  }
}

void VpnTab::setDefault(const std::string& uuid) {
  if (m_config != nullptr) {
    (void)m_config->setOverride({"vpn", "default"}, uuid);
  }
  requestRebuild();
}

void VpnTab::setTrusted(const std::string& name, bool trusted) {
  if (m_config == nullptr || name.empty()) {
    return;
  }
  auto list = m_config->config().vpn.trusted;
  std::erase(list, name);
  if (trusted) {
    list.push_back(name);
  }
  (void)m_config->setOverride({"vpn", "trusted"}, list);
  requestRebuild();
}

// ── Autoconexión ─────────────────────────────────────────────────────────────────────────────────────

void VpnTab::addAutoConnect(Flex& parent) {
  if (m_config == nullptr) {
    return;
  }
  const float scale = contentScale();
  const auto& cfg = m_config->config().vpn;
  auto c = card(scale, panelCardOpacity());
  auto header = makeCardHeaderRow(i18n::tr("okaru.vpn.auto-connect"), scale);
  header->addChild(
      ui::toggle({
          .checked = cfg.autoConnect,
          .scale = scale,
          .onChange = [this](bool on) {
            (void)m_config->setOverride({"vpn", "auto_connect"}, on);
            requestRebuild();
          },
      })
  );
  c->addChild(std::move(header));
  c->addChild(caption(
      scale,
      i18n::tr(
          cfg.defaultConnection.empty() && cfg.autoConnect ? "okaru.vpn.auto-connect-no-default"
                                                           : "okaru.vpn.auto-connect-hint"
      )
  ));
  c->addChild(
      ui::label(
          {.text = i18n::tr("okaru.vpn.trusted"),
           .fontSize = Style::fontSizeBody * scale,
           .fontWeight = FontWeight::SemiBold}
      )
  );
  if (cfg.trusted.empty()) {
    c->addChild(caption(scale, i18n::tr("okaru.vpn.trusted-empty")));
  }
  for (const auto& name : cfg.trusted) {
    c->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::glyph(
                {.glyph = "wifi",
                 .glyphSize = Style::fontSizeBody * scale,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
            ),
            ui::label({.text = name, .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F}),
            iconButton("x", i18n::tr("okaru.vpn.untrust"), [this, name] { setTrusted(name, false); })
        )
    );
  }
  const std::string here = currentNetwork();
  // Con "cualquier red por cable es de confianza", confiar en el cable no cambia nada: solo se ofrece la Wi-Fi.
  const bool wired = m_network != nullptr && m_network->state().kind != NetworkConnectivity::Wireless;
  if (!here.empty() && !std::ranges::contains(cfg.trusted, here) && !(wired && cfg.trustAllEthernet)) {
    c->addChild(
        ui::button(
            {.text = i18n::tr("okaru.vpn.trust-current", "name", here),
             .glyph = "shield-check",
             .variant = ButtonVariant::Default,
             .onClick = [this, here] { setTrusted(here, true); }}
        )
    );
  }
  c->addChild(
      ui::button(
          {.text = i18n::tr("okaru.vpn.more-settings"),
           .glyph = "settings",
           .variant = ButtonVariant::Ghost,
           .onClick = [] {
             PanelManager::instance().closePanel();
             (void)process::runAsync({"okaru", "msg", "settings-open", "services"});
           }}
      )
  );
  parent.addChild(std::move(c));
}

// ── Editor ───────────────────────────────────────────────────────────────────────────────────────────

void VpnTab::startCreate(const std::string& type) {
  m_edit = {.uuid = {}, .name = {}, .type = type, .data = {}};
  m_fields.clear();
  m_advanced.clear();
  m_message.clear();
  m_mode = Mode::Edit;
  ++m_editGeneration;
  requestRebuild();
}

void VpnTab::startEdit(const VpnConnectionInfo& v) {
  m_busy = true;
  requestRebuild();
  vpn::load(v.uuid, [this](bool ok, vpn::VpnProfile profile) {
    m_busy = false;
    if (!ok) {
      setMessage(i18n::tr("okaru.vpn.failed", "detail", profile.uuid), true);
      return;
    }
    m_edit = std::move(profile);
    m_fields.clear();
    m_advanced.clear();
    // Los campos conocidos van a su casilla; el resto, a opciones avanzadas (sin las banderas de contraseña).
    const auto known = vpn::fieldsFor(m_edit.type);
    for (const auto& [key, value] : m_edit.data) {
      if (std::ranges::any_of(known, [&](const vpn::VpnField& f) { return f.key == key; })) {
        m_fields[key] = value;
      } else {
        m_advanced += key + "=" + value + "\n";
      }
    }
    m_message.clear();
    m_mode = Mode::Edit;
    ++m_editGeneration;
    requestRebuild();
  });
}

void VpnTab::addEditor(Flex& parent) {
  const float scale = contentScale();
  auto c = card(scale, panelCardOpacity());
  const bool creating = m_edit.uuid.empty();
  c->addChild(makeCardHeaderRow(
      creating ? i18n::tr("okaru.vpn.new") : i18n::tr("okaru.vpn.edit-title", "name", m_edit.name), scale
  ));
  const auto field = [&](std::string label, std::unique_ptr<Node> control) {
    c->addChild(caption(scale, std::move(label)));
    c->addChild(std::move(control));
  };

  if (creating) {
    const auto types = vpn::installedTypes();
    std::vector<std::string> labels;
    std::size_t selected = 0;
    for (std::size_t i = 0; i < types.size(); ++i) {
      labels.push_back(types[i].label);
      if (types[i].name == m_edit.type) {
        selected = i;
      }
    }
    field(
        i18n::tr("okaru.vpn.type"),
        ui::select(
            {.options = labels,
             .selectedIndex = selected,
             .fontSize = Style::fontSizeBody * scale,
             .onSelectionChanged = [this, types](std::size_t index, std::string_view) {
               if (index < types.size() && types[index].name != m_edit.type) {
                 DeferredCall::callLater([this, type = types[index].name] {
                   const std::string name = m_edit.name;
                   startCreate(type);
                   m_edit.name = name;
                 });
               }
             }}
        )
    );
  } else {
    c->addChild(caption(scale, vpn::typeLabel(m_edit.type)));
  }
  field(
      i18n::tr("okaru.vpn.name"),
      ui::input({
          .value = m_edit.name,
          .placeholder = i18n::tr("okaru.vpn.name-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .onChange = [this](const std::string& value) { m_edit.name = value; },
      })
  );

  for (const vpn::VpnField& f : vpn::fieldsFor(m_edit.type)) {
    std::string label = i18n::tr(f.labelKey);
    if (f.required) {
      label += " *";
    }
    const std::string value = m_fields.contains(f.key) ? m_fields[f.key] : std::string{};
    if (f.kind == vpn::VpnField::Kind::Choice) {
      std::size_t selected = 0;
      for (std::size_t i = 0; i < f.choices.size(); ++i) {
        if (f.choices[i] == value) {
          selected = i;
        }
      }
      if (value.empty() && !f.choices.empty()) {
        m_fields[f.key] = f.choices.front();
      }
      field(
          std::move(label),
          ui::select(
              {.options = f.choices,
               .selectedIndex = selected,
               .fontSize = Style::fontSizeBody * scale,
               .onSelectionChanged = [this, key = f.key](std::size_t, std::string_view text) {
                 m_fields[key] = std::string(text);
               }}
          )
      );
      continue;
    }
    auto input = ui::input({
        .value = value,
        .placeholder = f.placeholder,
        .fontSize = Style::fontSizeBody * scale,
        .flexGrow = 1.0F,
        .onChange = [this, key = f.key](const std::string& v) { m_fields[key] = v; },
    });
    if (f.kind == vpn::VpnField::Kind::File) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      row->addChild(std::move(input));
      row->addChild(iconButton("folder-open", i18n::tr("okaru.vpn.choose-file"), [this, key = f.key, label] {
        pickFile(label, [this, key](const std::string& path) {
          if (!path.empty()) {
            m_fields[key] = path;
            ++m_editGeneration; // rehace los campos con la ruta elegida
          }
        });
      }));
      field(std::move(label), std::move(row));
    } else {
      field(std::move(label), std::move(input));
    }
  }

  if (m_edit.type != "wireguard") {
    auto advanced = ui::input({
        .value = m_advanced,
        .placeholder = "clave=valor",
        .fontSize = Style::fontSizeCaption * scale,
        .onChange = [this](const std::string& v) { m_advanced = v; },
        .configure =
            [scale](Input& input) {
              input.setMultiline(true);
              input.setControlHeight(90.0F * scale);
            },
    });
    field(i18n::tr("okaru.vpn.advanced"), std::move(advanced));
    c->addChild(caption(scale, i18n::tr("okaru.vpn.password-hint")));
  } else if (!creating) {
    c->addChild(caption(scale, i18n::tr("okaru.vpn.wireguard-edit-hint")));
  }

  c->addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale},
          ui::button(
              {.text = i18n::tr("okaru.vpn.cancel"),
               .variant = ButtonVariant::Ghost,
               .onClick =
                   [this] {
                     m_mode = Mode::List;
                     m_message.clear();
                     requestRebuild();
                   }}
          ),
          ui::button(
              {.text = i18n::tr("okaru.vpn.save"),
               .glyph = "device-floppy",
               .enabled = !m_busy,
               .variant = ButtonVariant::Primary,
               .onClick = [this] { DeferredCall::callLater([this] { saveEdit(); }); }}
          )
      )
  );
  parent.addChild(std::move(c));
}

void VpnTab::saveEdit() {
  if (m_edit.name.empty()) {
    setMessage(i18n::tr("okaru.vpn.need-name"), true);
    return;
  }
  for (const vpn::VpnField& f : vpn::fieldsFor(m_edit.type)) {
    if (f.required && (!m_fields.contains(f.key) || m_fields[f.key].empty())) {
      setMessage(i18n::tr("okaru.vpn.need-field", "field", i18n::tr(f.labelKey)), true);
      return;
    }
  }
  vpn::VpnProfile profile = m_edit;
  if (profile.type == "wireguard") {
    profile.data = m_fields;
  } else {
    // vpn.data = opciones avanzadas + campos conocidos (los vacíos se quitan).
    profile.data.clear();
    std::istringstream lines(m_advanced);
    std::string line;
    while (std::getline(lines, line)) {
      const auto eq = line.find('=');
      if (eq != std::string::npos && eq > 0) {
        profile.data[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    for (const auto& [key, value] : m_fields) {
      if (value.empty()) {
        profile.data.erase(key);
      } else {
        profile.data[key] = value;
      }
    }
  }
  m_busy = true;
  requestRebuild();
  vpn::save(profile, [this, name = profile.name](bool ok, const std::string& error) {
    m_busy = false;
    if (!ok) {
      setMessage(i18n::tr("okaru.vpn.failed", "detail", error), true);
      return;
    }
    m_mode = Mode::List;
    setMessage(i18n::tr("okaru.vpn.saved", "name", name), false);
    if (m_network != nullptr) {
      m_network->refresh();
    }
  });
}

// ── Archivos ─────────────────────────────────────────────────────────────────────────────────────────

void VpnTab::pickFile(const std::string& title, std::function<void(const std::string&)> done) {
  PanelManager::instance().closePanel();
  auto output = std::make_shared<std::string>();
  const bool started = process::runAsync(
      {"okaru-files", "--pick", "open", "--title", title},
      {.stdOut = [output](std::string_view chunk) { output->append(chunk); },
       .onExit =
           [output, done = std::move(done)](const process::RunResult&) {
             DeferredCall::callLater([output, done] {
               const std::string first = output->substr(0, output->find('\n'));
               done(pathFromUri(first));
               PanelManager::instance().openPanel(kPanelId);
             });
           }}
  );
  if (!started) {
    notify::error("OKARU", i18n::tr("okaru.vpn.title"), i18n::tr("okaru.vpn.no-picker"));
  }
}

void VpnTab::importVpn() {
  pickFile(i18n::tr("okaru.vpn.import"), [this](const std::string& path) {
    if (path.empty()) {
      return;
    }
    vpn::importFile(path, [this, path](bool ok, const std::string& error) {
      if (ok) {
        setMessage(i18n::tr("okaru.vpn.imported"), false);
      } else if (error == "unknown-format") {
        setMessage(i18n::tr("okaru.vpn.import-unknown"), true);
      } else {
        setMessage(i18n::tr("okaru.vpn.failed", "detail", error), true);
      }
      if (m_network != nullptr) {
        m_network->refresh();
      }
    });
  });
}
