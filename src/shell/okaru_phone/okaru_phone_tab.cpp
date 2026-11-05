#include "shell/okaru_phone/okaru_phone_tab.h"

#include "config/config_service.h"
#include "dbus/kdeconnect/kdeconnect_service.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/core/renderer.h"
#include "shell/okaru_phone/okaru_phone_prefs.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <filesystem>
#include <format>

using namespace control_center;
namespace phone = okaru::phone;
namespace fs = std::filesystem;

namespace {

  constexpr std::size_t kRecentImageCount = 4;
  constexpr float kAvatarSize = 64.0F;
  constexpr float kThumbHeight = 72.0F;

  // Resultado de una acción: solo los errores avisan; los aciertos ya se ven en el teléfono.
  KdeConnectService::ResultCallback reportFailure(std::string what) {
    return [what = std::move(what)](bool ok, const std::string& detail) {
      if (!ok) {
        notify::error("OKARU", i18n::tr("okaru.phone.action-failed", "action", what), detail);
      }
    };
  }

  bool isImageFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif";
  }

  bool looksLikeUrl(const std::string& text) {
    return text.starts_with("http://") || text.starts_with("https://") || text.starts_with("geo:")
        || text.starts_with("tel:") || text.starts_with("mailto:");
  }

  std::unique_ptr<Flex> card(float scale, float opacity) {
    return ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .configure = [scale, opacity](Flex& c) { applySectionCardStyle(c, scale, opacity); },
    });
  }

  // Dato pequeño de la cabecera: icono + texto.
  std::unique_ptr<Flex> chip(float scale, const char* glyph, const std::string& text, ColorRole role) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .paddingV = 2.0F * scale,
         .paddingH = Style::spaceSm * scale, .fill = colorSpecFromRole(role, 0.12F), .radius = Style::radiusSm * scale},
        ui::glyph({.glyph = glyph, .glyphSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(role)}),
        ui::label({.text = text, .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(role)})
    );
  }

} // namespace

OkaruPhoneTab::OkaruPhoneTab(KdeConnectService* service, MprisService* mpris, ConfigService* config,
                             AsyncTextureCache* textures)
    : m_service(service), m_mpris(mpris), m_config(config), m_textures(textures) {}

std::unique_ptr<Flex> OkaruPhoneTab::create() {
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
  scanRecentImages();
  return root;
}

std::unique_ptr<Flex> OkaruPhoneTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("okaru.phone.refresh"),
          .onClick =
              [this] {
                if (m_service != nullptr) {
                  m_service->refresh();
                }
                scanRecentImages();
                requestRebuild();
              },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void OkaruPhoneTab::setActive(bool active) {
  if (active) {
    scanRecentImages();
    requestRebuild();
  } else {
    m_renaming = false;
    m_confirmUnpair = false;
  }
}

void OkaruPhoneTab::onClose() {
  m_root = nullptr;
  m_content = nullptr;
  m_shareInput = nullptr;
  m_smsNumber = nullptr;
  m_smsText = nullptr;
  m_pendingImages.clear();
  m_renaming = false;
  m_confirmUnpair = false;
}

void OkaruPhoneTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  rebuildIfNeeded();
  loadImages(renderer);
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void OkaruPhoneTab::doUpdate(Renderer& renderer) {
  rebuildIfNeeded();
  loadImages(renderer);
}

void OkaruPhoneTab::requestRebuild() {
  m_key.clear();
  PanelManager::instance().refresh();
}

void OkaruPhoneTab::selectDevice(const std::string& id) {
  phone::prefs().selected = id;
  phone::save();
  m_renaming = false;
  m_confirmUnpair = false;
  requestRebuild();
}

std::string OkaruPhoneTab::mediaPlayerFor(const PhoneDevice& device) const {
  if (m_mpris == nullptr) {
    return {};
  }
  // kdeconnectd publica el reproductor del teléfono como org.mpris.MediaPlayer2.kdeconnect.* con la
  // identidad "<reproductor> - <nombre del dispositivo>".
  const std::string suffix = " - " + device.name;
  for (const auto& player : m_mpris->listPlayers()) {
    if (player.busName.starts_with("org.mpris.MediaPlayer2.kdeconnect.") && player.identity.ends_with(suffix)) {
      return player.busName;
    }
  }
  return {};
}

std::string OkaruPhoneTab::stateKey(const PhoneDevice* d) const {
  std::string key = std::format("{}|{}|{}|{}|", m_service != nullptr && m_service->available(), m_renaming,
                                m_confirmUnpair, m_showSms);
  if (m_service != nullptr) {
    for (const auto& other : m_service->devices()) {
      key += other.id + (phone::usable(other) ? "+" : "-");
    }
  }
  if (d == nullptr) {
    return key;
  }
  key += std::format("|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", d->id, phone::displayName(*d), phone::avatarPath(*d),
                     d->reachable, d->paired, d->pairRequested, d->pairRequestedByPeer, d->battery, d->charging,
                     d->networkType, d->networkStrength, d->notifications, d->plugins.size());
  if (const std::string bus = mediaPlayerFor(*d); !bus.empty()) {
    for (const auto& player : m_mpris->listPlayers()) {
      if (player.busName == bus) {
        key += std::format("|{}|{}|{}", player.title, player.playbackStatus, player.artists.size());
      }
    }
  }
  for (const auto& image : m_recentImages) {
    key += "|" + image;
  }
  return key;
}

void OkaruPhoneTab::rebuildIfNeeded() {
  if (m_content == nullptr) {
    return;
  }
  const PhoneDevice* device =
      m_service != nullptr && m_service->available() ? phone::selectedDevice(*m_service) : nullptr;
  std::string key = stateKey(device);
  if (key == m_key) {
    return;
  }
  m_key = std::move(key);
  rebuild(device);
}

void OkaruPhoneTab::rebuild(const PhoneDevice* device) {
  // Lo escrito en los campos sobrevive a la reconstrucción (cambios de batería, de pista...).
  const std::string shareDraft = m_shareInput != nullptr ? m_shareInput->value() : std::string{};
  const std::string smsNumberDraft = m_smsNumber != nullptr ? m_smsNumber->value() : std::string{};
  const std::string smsTextDraft = m_smsText != nullptr ? m_smsText->value() : std::string{};
  m_shareInput = nullptr;
  m_smsNumber = nullptr;
  m_smsText = nullptr;
  m_pendingImages.clear();
  while (!m_content->children().empty()) {
    m_content->removeChild(m_content->children().back().get());
  }

  if (m_service == nullptr || !m_service->available()) {
    addEmpty(*m_content, "device-mobile-off", i18n::tr("okaru.phone.no-daemon"), i18n::tr("okaru.phone.no-daemon-hint"));
    return;
  }
  if (device == nullptr) {
    addEmpty(*m_content, "device-mobile-off", i18n::tr("okaru.phone.no-devices"),
             i18n::tr("okaru.phone.no-devices-hint"));
    return;
  }

  addHero(*m_content, *device);
  addPairing(*m_content, *device);
  if (phone::usable(*device)) {
    addActions(*m_content, *device);
    addMedia(*m_content, *device);
    addRecentImages(*m_content, *device);
    addShare(*m_content, *device);
    if (m_showSms) {
      addSms(*m_content, *device);
    }
  }
  addFooter(*m_content, *device);

  if (m_shareInput != nullptr) {
    m_shareInput->setValue(shareDraft);
  }
  if (m_smsNumber != nullptr) {
    m_smsNumber->setValue(smsNumberDraft);
    m_smsText->setValue(smsTextDraft);
  }
}

void OkaruPhoneTab::addEmpty(Flex& parent, const char* glyph, const std::string& title, const std::string& body) {
  const float scale = contentScale();
  parent.addChild(ui::column(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingV = Style::spaceLg * scale},
      ui::glyph({.glyph = glyph, .glyphSize = 48.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}),
      ui::label({.text = title, .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::Bold}),
      ui::label({.text = body, .fontSize = Style::fontSizeBody * scale,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .textAlign = TextAlign::Center})
  ));
}

void OkaruPhoneTab::addHero(Flex& parent, const PhoneDevice& d) {
  const float scale = contentScale();
  auto hero = card(scale, panelCardOpacity());
  auto top = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});

  // Foto propia o icono del tipo de dispositivo.
  const std::string avatar = phone::avatarPath(d);
  const float avatarSize = kAvatarSize * scale;
  if (!avatar.empty() && fs::exists(avatar)) {
    Image* image = nullptr;
    top->addChild(ui::image({.out = &image, .fit = ImageFit::Cover, .radius = avatarSize * 0.5F,
                             .width = avatarSize, .height = avatarSize}));
    m_pendingImages.push_back({image, avatar, static_cast<int>(avatarSize)});
  } else {
    top->addChild(ui::row(
        {.align = FlexAlign::Center, .justify = FlexJustify::Center,
         .fill = colorSpecFromRole(ColorRole::Primary, 0.12F), .radius = avatarSize * 0.5F, .width = avatarSize,
         .height = avatarSize},
        ui::glyph({.glyph = phone::deviceGlyph(d), .glyphSize = avatarSize * 0.5F,
                   .color = colorSpecFromRole(ColorRole::Primary)})
    ));
  }

  auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
  if (m_renaming) {
    info->addChild(ui::input({
        .value = phone::displayName(d),
        .placeholder = d.name,
        .fontSize = Style::fontSizeBody * scale,
        .flexGrow = 1.0F,
        .onSubmit =
            [this, id = d.id](const std::string& text) {
              auto& alias = phone::prefs().alias;
              if (text.empty()) {
                alias.erase(id);
              } else {
                alias[id] = text;
              }
              phone::save();
              m_renaming = false;
              requestRebuild();
            },
    }));
  } else {
    info->addChild(ui::label({.text = phone::displayName(d), .fontSize = Style::fontSizeTitle * scale,
                              .fontWeight = FontWeight::Bold, .maxLines = 1}));
  }
  std::string status = d.pairRequestedByPeer ? i18n::tr("okaru.phone.pair-requested")
      : d.pairRequested                     ? i18n::tr("okaru.phone.pairing")
      : !d.paired                           ? i18n::tr("okaru.phone.not-paired")
      : d.reachable                         ? i18n::tr("okaru.phone.connected")
                                            : i18n::tr("okaru.phone.offline");
  if (const std::string ip = phone::usable(d) ? kdeconnect::firstAddress(d.addresses, false) : std::string{};
      !ip.empty()) {
    status += " · " + ip;
  }
  info->addChild(ui::label({.text = status, .fontSize = Style::fontSizeCaption * scale,
                            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1}));
  top->addChild(std::move(info));
  hero->addChild(std::move(top));

  if (phone::usable(d)) {
    auto chips = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale});
    if (d.battery >= 0) {
      const ColorRole role = d.charging ? ColorRole::Primary : d.battery <= 20 ? ColorRole::Error : ColorRole::OnSurface;
      chips->addChild(chip(scale, phone::batteryGlyph(d.battery, d.charging),
                           d.charging ? i18n::tr("okaru.phone.battery-charging", "percent", std::to_string(d.battery))
                                      : std::format("{}%", d.battery),
                           role));
    }
    if (d.networkStrength >= 0) {
      const std::string type = d.networkType.empty() || d.networkType == "Unknown" ? std::string{} : d.networkType;
      chips->addChild(chip(scale, phone::signalGlyph(d.networkStrength),
                           type.empty() ? i18n::tr("okaru.phone.signal") : type, ColorRole::OnSurface));
    }
    if (d.notifications > 0) {
      chips->addChild(chip(scale, "bell", std::to_string(d.notifications), ColorRole::Tertiary));
    }
    hero->addChild(std::move(chips));
  }

  // Selector cuando hay más de un dispositivo.
  if (m_service->devices().size() > 1) {
    auto picker = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale});
    for (const auto& other : m_service->devices()) {
      picker->addChild(ui::button({
          .text = phone::displayName(other),
          .glyph = phone::deviceGlyph(other),
          .fontSize = Style::fontSizeCaption * scale,
          .selected = other.id == d.id,
          .variant = other.id == d.id ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .onClick = [this, id = other.id] { selectDevice(id); },
      }));
    }
    hero->addChild(std::move(picker));
  }
  parent.addChild(std::move(hero));
}

void OkaruPhoneTab::addPairing(Flex& parent, const PhoneDevice& d) {
  const float scale = contentScale();
  if (d.paired && !d.pairRequestedByPeer) {
    return;
  }
  auto pairing = card(scale, panelCardOpacity());
  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (d.pairRequestedByPeer) {
    pairing->addChild(ui::label({.text = i18n::tr("okaru.phone.pair-request-body", "name", d.name),
                                 .fontSize = Style::fontSizeBody * scale}));
    pairing->addChild(ui::label({.text = i18n::tr("okaru.phone.verification-key", "key", d.verificationKey),
                                 .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::Bold,
                                 .fontFamily = "monospace", .color = colorSpecFromRole(ColorRole::Primary)}));
    buttons->addChild(ui::button({.text = i18n::tr("okaru.phone.accept"), .glyph = "check",
                                  .variant = ButtonVariant::Primary, .flexGrow = 1.0F,
                                  .onClick = [this, id = d.id] {
                                    m_service->acceptPairing(id, reportFailure(i18n::tr("okaru.phone.accept")));
                                  }}));
    buttons->addChild(ui::button({.text = i18n::tr("okaru.phone.reject"), .glyph = "x", .flexGrow = 1.0F,
                                  .onClick = [this, id = d.id] {
                                    m_service->cancelPairing(id, reportFailure(i18n::tr("okaru.phone.reject")));
                                  }}));
  } else if (d.pairRequested) {
    pairing->addChild(ui::label({.text = i18n::tr("okaru.phone.pairing-wait", "key", d.verificationKey),
                                 .fontSize = Style::fontSizeBody * scale}));
    buttons->addChild(ui::button({.text = i18n::tr("okaru.phone.cancel"), .glyph = "x", .flexGrow = 1.0F,
                                  .onClick = [this, id = d.id] { m_service->cancelPairing(id); }}));
  } else {
    pairing->addChild(ui::label({.text = i18n::tr("okaru.phone.pair-hint"), .fontSize = Style::fontSizeBody * scale,
                                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    buttons->addChild(ui::button({.text = i18n::tr("okaru.phone.pair"), .glyph = "link", .enabled = d.reachable,
                                  .variant = ButtonVariant::Primary, .flexGrow = 1.0F,
                                  .onClick = [this, id = d.id] {
                                    m_service->requestPairing(id, reportFailure(i18n::tr("okaru.phone.pair")));
                                  }}));
  }
  pairing->addChild(std::move(buttons));
  parent.addChild(std::move(pairing));
}

void OkaruPhoneTab::addActions(Flex& parent, const PhoneDevice& d) {
  const float scale = contentScale();
  struct Action {
    const char* plugin;
    const char* glyph;
    const char* key;
    std::function<void()> run;
  };
  const std::string id = d.id;
  const std::vector<Action> actions = {
      {"kdeconnect_findmyphone", "bell-ringing", "okaru.phone.ring",
       [this, id] { m_service->ring(id, reportFailure(i18n::tr("okaru.phone.ring"))); }},
      {"kdeconnect_ping", "send", "okaru.phone.ping",
       [this, id] { m_service->ping(id, {}, reportFailure(i18n::tr("okaru.phone.ping"))); }},
      {"kdeconnect_clipboard", "clipboard-copy", "okaru.phone.clipboard",
       [this, id] { m_service->sendClipboard(id, reportFailure(i18n::tr("okaru.phone.clipboard"))); }},
      {"kdeconnect_share", "file-upload", "okaru.phone.send-file",
       [this, id] {
         FileDialogOptions options;
         options.title = i18n::tr("okaru.phone.send-file");
         (void)FileDialog::open(std::move(options), [this, id](std::optional<fs::path> path) {
           if (path.has_value()) {
             m_service->shareFiles(id, {path->string()}, reportFailure(i18n::tr("okaru.phone.send-file")));
           }
         });
       }},
      {"kdeconnect_sftp", "folder-open", "okaru.phone.browse",
       [this, id] {
         m_service->browse(id, reportFailure(i18n::tr("okaru.phone.browse")));
         PanelManager::instance().closePanel();
       }},
      {"kdeconnect_sms", "message", "okaru.phone.sms",
       [this] {
         m_showSms = !m_showSms;
         requestRebuild();
       }},
  };

  auto grid = ui::row({.align = FlexAlign::Stretch, .wrap = true, .gap = Style::spaceSm * scale});
  for (const auto& action : actions) {
    if (!d.hasPlugin(action.plugin)) {
      continue;
    }
    grid->addChild(ui::button({
        .text = i18n::tr(action.key),
        .glyph = action.glyph,
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::baseGlyphSize * scale,
        .selected = std::string_view{action.plugin} == "kdeconnect_sms" && m_showSms,
        .contentAlign = ButtonContentAlign::Center,
        .minWidth = 120.0F * scale,
        .minHeight = 56.0F * scale,
        .flexGrow = 1.0F,
        .onClick = action.run,
    }));
  }
  parent.addChild(std::move(grid));
}

void OkaruPhoneTab::addMedia(Flex& parent, const PhoneDevice& d) {
  const std::string bus = mediaPlayerFor(d);
  if (bus.empty()) {
    return;
  }
  const auto players = m_mpris->listPlayers();
  const auto it = std::ranges::find(players, bus, &MprisPlayerInfo::busName);
  if (it == players.end() || it->title.empty()) {
    return;
  }
  const float scale = contentScale();
  auto media = card(scale, panelCardOpacity());
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
  row->addChild(ui::glyph({.glyph = "music", .glyphSize = Style::baseGlyphSize * 1.4F * scale,
                           .color = colorSpecFromRole(ColorRole::Primary)}));
  std::string artists;
  for (const auto& artist : it->artists) {
    artists += (artists.empty() ? "" : ", ") + artist;
  }
  row->addChild(ui::column(
      {.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F},
      ui::label({.text = it->title, .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::Bold,
                 .maxLines = 1}),
      ui::label({.text = artists.empty() ? it->identity : artists, .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
  ));
  const bool playing = it->playbackStatus == "Playing";
  const auto control = [this, scale, bus](const char* glyph, bool enabled, auto action) {
    return ui::button({.glyph = glyph, .glyphSize = Style::baseGlyphSize * scale, .enabled = enabled,
                       .variant = ButtonVariant::Ghost, .onClick = [this, bus, action] { (m_mpris->*action)(bus); }});
  };
  row->addChild(control("player-skip-back", it->canGoPrevious, &MprisService::previous));
  row->addChild(control(playing ? "player-pause" : "player-play", it->canPlay || it->canPause, &MprisService::playPause));
  row->addChild(control("player-skip-forward", it->canGoNext, &MprisService::next));
  media->addChild(std::move(row));
  parent.addChild(std::move(media));
}

void OkaruPhoneTab::scanRecentImages() {
  m_recentImages.clear();
  if (m_config == nullptr) {
    return;
  }
  const std::string& configured = m_config->config().shell.screenshot.directory;
  const fs::path dir = configured.empty() ? FileUtils::defaultPicturesDirectory() : FileUtils::expandUserPath(configured);
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, std::string>> found;
  for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (entry.is_regular_file(ec) && isImageFile(entry.path())) {
      found.emplace_back(entry.last_write_time(ec), entry.path().string());
    }
  }
  const std::size_t count = std::min(kRecentImageCount, found.size());
  std::ranges::partial_sort(found, found.begin() + static_cast<std::ptrdiff_t>(count), std::ranges::greater{});
  for (std::size_t i = 0; i < count; ++i) {
    m_recentImages.push_back(std::move(found[i].second));
  }
}

void OkaruPhoneTab::addRecentImages(Flex& parent, const PhoneDevice& d) {
  if (m_recentImages.empty() || !d.hasPlugin("kdeconnect_share")) {
    return;
  }
  const float scale = contentScale();
  auto recent = card(scale, panelCardOpacity());
  recent->addChild(makeCardHeaderRow(i18n::tr("okaru.phone.recent-images"), scale));
  auto thumbs = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  for (const auto& path : m_recentImages) {
    Image* image = nullptr;
    auto button = ui::button({
        .tooltip = i18n::tr("okaru.phone.send-image", "file", fs::path(path).filename().string()),
        .padding = 0.0F,
        .height = kThumbHeight * scale,
        .flexGrow = 1.0F,
        .onClick =
            [this, id = d.id, path] {
              m_service->shareFiles(id, {path}, reportFailure(i18n::tr("okaru.phone.send-file")));
              notify::info("OKARU", i18n::tr("okaru.phone.sent"), fs::path(path).filename().string());
            },
    });
    button->addChild(ui::image({.out = &image, .fit = ImageFit::Cover, .radius = Style::radiusMd * scale,
                                .height = kThumbHeight * scale, .flexGrow = 1.0F}));
    m_pendingImages.push_back({image, path, static_cast<int>(kThumbHeight * 2.0F * scale)});
    thumbs->addChild(std::move(button));
  }
  recent->addChild(std::move(thumbs));
  parent.addChild(std::move(recent));
}

void OkaruPhoneTab::addShare(Flex& parent, const PhoneDevice& d) {
  if (!d.hasPlugin("kdeconnect_share")) {
    return;
  }
  const float scale = contentScale();
  auto share = card(scale, panelCardOpacity());
  share->addChild(makeCardHeaderRow(i18n::tr("okaru.phone.share"), scale));
  auto send = [this, id = d.id] {
    if (m_shareInput == nullptr || m_shareInput->value().empty()) {
      return;
    }
    const std::string text = m_shareInput->value();
    if (looksLikeUrl(text)) {
      m_service->shareUrl(id, text, reportFailure(i18n::tr("okaru.phone.share")));
    } else {
      m_service->shareText(id, text, reportFailure(i18n::tr("okaru.phone.share")));
    }
    m_shareInput->setValue("");
  };
  share->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::input({.out = &m_shareInput, .placeholder = i18n::tr("okaru.phone.share-placeholder"),
                 .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F,
                 .onSubmit = [send](const std::string&) { send(); }}),
      ui::button({.glyph = "send", .variant = ButtonVariant::Primary, .tooltip = i18n::tr("okaru.phone.share"),
                  .onClick = send})
  ));
  parent.addChild(std::move(share));
}

void OkaruPhoneTab::addSms(Flex& parent, const PhoneDevice& d) {
  const float scale = contentScale();
  auto sms = card(scale, panelCardOpacity());
  auto header = makeCardHeaderRow(i18n::tr("okaru.phone.sms"), scale);
  header->addChild(ui::button({.text = i18n::tr("okaru.phone.open-sms-app"), .fontSize = Style::fontSizeCaption * scale,
                               .variant = ButtonVariant::Ghost,
                               .onClick = [this, id = d.id] {
                                 m_service->launchSmsApp(id, reportFailure(i18n::tr("okaru.phone.open-sms-app")));
                               }}));
  sms->addChild(std::move(header));
  sms->addChild(ui::input({.out = &m_smsNumber, .placeholder = i18n::tr("okaru.phone.sms-number"),
                           .fontSize = Style::fontSizeBody * scale}));
  auto send = [this, id = d.id] {
    if (m_smsNumber == nullptr || m_smsText == nullptr || m_smsNumber->value().empty() || m_smsText->value().empty()) {
      return;
    }
    const std::string number = m_smsNumber->value();
    m_service->sendSms(id, number, m_smsText->value(), [number](bool ok, const std::string& detail) {
      if (ok) {
        notify::info("OKARU", i18n::tr("okaru.phone.sms-sent"), number);
      } else {
        notify::error("OKARU", i18n::tr("okaru.phone.action-failed", "action", i18n::tr("okaru.phone.sms")), detail);
      }
    });
    m_smsText->setValue("");
  };
  sms->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::input({.out = &m_smsText, .placeholder = i18n::tr("okaru.phone.sms-message"),
                 .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F,
                 .onSubmit = [send](const std::string&) { send(); }}),
      ui::button({.glyph = "send", .variant = ButtonVariant::Primary, .tooltip = i18n::tr("okaru.phone.sms-send"),
                  .onClick = send})
  ));
  parent.addChild(std::move(sms));
}

void OkaruPhoneTab::addFooter(Flex& parent, const PhoneDevice& d) {
  const float scale = contentScale();
  auto footer = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale});
  const auto ghost = [scale](std::string text, const char* glyph, std::function<void()> onClick,
                             ButtonVariant variant = ButtonVariant::Ghost) {
    return ui::button({.text = std::move(text), .glyph = glyph, .fontSize = Style::fontSizeCaption * scale,
                       .variant = variant, .onClick = std::move(onClick)});
  };
  footer->addChild(ghost(i18n::tr("okaru.phone.rename"), "pencil", [this] {
    m_renaming = !m_renaming;
    requestRebuild();
  }));
  footer->addChild(ghost(i18n::tr("okaru.phone.change-photo"), "photo", [this, id = d.id] {
    FileDialogOptions options;
    options.title = i18n::tr("okaru.phone.change-photo");
    options.extensions = {"png", "jpg", "jpeg", "webp"};
    options.startDirectory = FileUtils::defaultPicturesDirectory();
    (void)FileDialog::open(std::move(options), [this, id](std::optional<fs::path> path) {
      if (path.has_value()) {
        phone::prefs().avatar[id] = path->string();
        phone::save();
        requestRebuild();
      }
    });
  }));
  if (!phone::avatarPath(d).empty()) {
    footer->addChild(ghost(i18n::tr("okaru.phone.remove-photo"), "x", [this, id = d.id] {
      phone::prefs().avatar.erase(id);
      phone::save();
      requestRebuild();
    }));
  }
  if (d.paired) {
    footer->addChild(ui::spacer());
    footer->addChild(ghost(
        i18n::tr(m_confirmUnpair ? "okaru.phone.unpair-confirm" : "okaru.phone.unpair"), "unlink",
        [this, id = d.id] {
          if (!m_confirmUnpair) {
            m_confirmUnpair = true;
          } else {
            m_confirmUnpair = false;
            m_service->unpair(id, reportFailure(i18n::tr("okaru.phone.unpair")));
          }
          requestRebuild();
        },
        m_confirmUnpair ? ButtonVariant::Destructive : ButtonVariant::Ghost
    ));
  }
  parent.addChild(std::move(footer));
}

void OkaruPhoneTab::loadImages(Renderer& renderer) {
  if (m_textures == nullptr) {
    return;
  }
  // Las imágenes se piden a la caché asíncrona; las que aún no están se reintentan en la próxima vuelta.
  std::erase_if(m_pendingImages, [&](const PendingImage& pending) {
    if (pending.image == nullptr) {
      return true;
    }
    pending.image->setAsyncReadyCallback([] { PanelManager::instance().refresh(); });
    return pending.image->setSourceFileAsync(renderer, *m_textures, pending.path, pending.size);
  });
}
