#include "shell/okaru_dashboard/okaru_dashboard_cards.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/build_info.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "pipewire/audio_glyphs.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/hardware_info.h"
#include "system/screen_time_service.h"
#include "system/system_monitor_service.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_set>

namespace okaru::dashboard {

  namespace {

    std::string tr(const std::string& key) { return i18n::tr("okaru.dashboard." + key); }

    // Icono principal de una tarjeta: el mismo en todas (pequeña / mediana y grande).
    float cardGlyphSize(bool small, float scale) { return Style::fontSizeTitle * (small ? 1.4F : 1.6F) * scale; }

    void togglePanel(std::string_view id) {
      if (!id.empty() && !openHostedDetail(id)) {
        PanelManager::instance().togglePanel(std::string(id));
      }
    }

    std::string_view panelFor(std::string_view type) {
      const CardKind* kind = findKind(type);
      return kind != nullptr ? kind->panel : std::string_view{};
    }

    std::unique_ptr<Label>
    makeText(float size, const ColorSpec& color, FontWeight weight = FontWeight::Normal, float grow = 0.0F) {
      return ui::label({
          .text = " ",
          .fontSize = size,
          .fontWeight = weight,
          .color = color,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = grow,
      });
    }

    ColorSpec onSurface() { return colorSpecFromRole(ColorRole::OnSurface); }
    ColorSpec muted() { return colorSpecFromRole(ColorRole::OnSurfaceVariant); }

    std::unique_ptr<Button> iconButton(float scale, std::string glyph, std::function<void()> onClick) {
      return ui::button({
          .glyph = std::move(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = std::move(onClick),
      });
    }

    // ── Tarjeta de información genérica: icono + título + valor + detalle ─────────────────────────────

    struct InfoView {
      std::string glyph;
      std::string value;
      std::string detail;
      bool active = false;
    };

    class InfoCard : public DashboardCard {
    public:
      InfoCard(
          CardSize size, float scale, float opacity, const std::string& title, std::function<InfoView()> read,
          std::function<void()> onClick, std::function<void()> onRightClick
      )
          : DashboardCard(size, scale, opacity), m_read(std::move(read)) {
        const bool small = compact();
        setDirection(small ? FlexDirection::Vertical : FlexDirection::Horizontal);
        setAlign(small ? FlexAlign::Center : FlexAlign::Center);
        setJustify(FlexJustify::Center);
        addChild(ui::glyph({
            .out = &m_glyph,
            .glyph = "circle",
            .glyphSize = cardGlyphSize(small, scale),
            .color = onSurface(),
        }));
        auto text = ui::column({.align = small ? FlexAlign::Center : FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
        if (!small) {
          auto label = makeText(Style::fontSizeCaption * scale, muted());
          label->setText(title);
          text->addChild(std::move(label));
        }
        auto value = makeText(
            (small ? Style::fontSizeCaption : Style::fontSizeBody) * scale, onSurface(), FontWeight::Bold
        );
        m_value = value.get();
        text->addChild(std::move(value));
        auto detail = makeText((small ? Style::fontSizeMini : Style::fontSizeCaption) * scale, muted());
        m_detail = detail.get();
        text->addChild(std::move(detail));
        if (small) {
          m_value->setTextAlign(TextAlign::Center);
          m_detail->setTextAlign(TextAlign::Center);
        }
        addChild(std::move(text));
        setActivation(std::move(onClick), std::move(onRightClick));
      }

      void sync(Renderer& /*renderer*/) override {
        if (!m_read) {
          return;
        }
        const InfoView view = m_read();
        m_glyph->setGlyph(view.glyph);
        m_glyph->setColor(colorSpecFromRole(view.active ? ColorRole::Primary : ColorRole::OnSurface));
        if (m_value->text() != view.value) {
          m_value->setText(view.value.empty() ? " " : view.value);
        }
        m_detail->setText(view.detail.empty() ? " " : view.detail);
      }

    private:
      std::function<InfoView()> m_read;
      Glyph* m_glyph = nullptr;
      Label* m_value = nullptr;
      Label* m_detail = nullptr;
    };

    // ── Lectores de cada tarjeta de información ──────────────────────────────────────────────────────

    std::function<InfoView()> networkReader(INetworkService* network) {
      return [network] {
        if (network == nullptr) {
          return InfoView{"network-off", tr("unavailable"), {}, false};
        }
        const NetworkState& s = network->state();
        InfoView v{network_display::glyphForState(s), {}, s.ipv4, s.connected};
        if (!s.connected && !s.resolving) {
          v.value = tr("disconnected");
        } else if (s.kind == NetworkConnectivity::Wireless) {
          v.value = s.ssid.empty() ? tr("wifi") : s.ssid;
        } else if (s.kind == NetworkConnectivity::Wired) {
          v.value = tr("wired");
        } else if (s.kind == NetworkConnectivity::Cellular) {
          v.value = tr("cellular");
        } else {
          v.value = tr("connected");
        }
        if (s.vpnConnected) {
          v.detail = v.detail.empty() ? std::string("VPN") : v.detail + " · VPN";
        }
        return v;
      };
    }

    std::function<InfoView()> bluetoothReader(BluetoothService* bluetooth) {
      return [bluetooth] {
        if (bluetooth == nullptr || !bluetooth->state().adapterPresent) {
          return InfoView{"bluetooth-off", tr("unavailable"), {}, false};
        }
        if (!bluetooth->state().powered) {
          return InfoView{"bluetooth-off", tr("off"), {}, false};
        }
        std::vector<const BluetoothDeviceInfo*> connected;
        for (const auto& device : bluetooth->devices()) {
          if (device.connected) {
            connected.push_back(&device);
          }
        }
        if (connected.empty()) {
          return InfoView{"bluetooth", tr("on"), tr("no-devices"), true};
        }
        std::string detail = connected.size() > 1 ? std::format("+{}", connected.size() - 1) : std::string{};
        if (connected.front()->hasBattery) {
          detail = std::format("{}%{}", connected.front()->batteryPercent, detail.empty() ? "" : " · " + detail);
        }
        return InfoView{"bluetooth-connected", connected.front()->alias, detail, true};
      };
    }

    std::function<InfoView()> powerReader(UPowerService* upower, PowerProfilesService* profiles) {
      return [upower, profiles] {
        const std::string profile = profiles != nullptr ? profiles->activeProfile() : std::string{};
        if (upower != nullptr && upower->state().isPresent) {
          const auto& s = upower->state();
          return InfoView{
              batteryGlyphName(s.percentage, s.state), std::format("{:.0f}%", s.percentage),
              profile.empty() ? batteryStateLabel(s.state) : profileLabel(profile), !s.onBattery
          };
        }
        if (!profile.empty()) {
          return InfoView{std::string(profileGlyphName(profile)), profileLabel(profile), tr("power-profile"), true};
        }
        return InfoView{"bolt", tr("unavailable"), {}, false};
      };
    }

    std::function<InfoView()> systemReader(SystemMonitorService* sysmon) {
      return [sysmon] {
        if (sysmon == nullptr || !sysmon->isRunning()) {
          return InfoView{"activity", tr("unavailable"), {}, false};
        }
        const SystemStats s = sysmon->latest();
        std::string detail = std::format("RAM {:.0f}%", s.ramUsagePercent);
        if (s.cpuTempC.has_value()) {
          detail += std::format(" · {:.0f}°C", *s.cpuTempC);
        }
        return InfoView{"activity", std::format("CPU {:.0f}%", s.cpuUsagePercent), detail, s.cpuUsagePercent >= 80.0};
      };
    }

    std::function<InfoView()> weatherReader(WeatherService* weather) {
      return [weather] {
        if (weather == nullptr || !weather->enabled()) {
          return InfoView{"cloud-off", tr("off"), {}, false};
        }
        if (!weather->locationConfigured()) {
          return InfoView{"cloud", tr("no-location"), {}, false};
        }
        const auto& snapshot = weather->snapshot();
        if (!snapshot.valid) {
          return InfoView{"cloud", weather->loading() ? tr("loading") : tr("unavailable"), {}, false};
        }
        const int t = static_cast<int>(std::lround(weather->displayTemperature(snapshot.current.temperatureC)));
        return InfoView{
            std::string(WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay)),
            std::format("{}{}", t, weather->displayTemperatureUnit()),
            WeatherService::descriptionForCode(snapshot.current.weatherCode), true
        };
      };
    }

    std::function<InfoView()> notificationsReader(NotificationManager* notifications) {
      return [notifications] {
        if (notifications == nullptr) {
          return InfoView{"bell", tr("unavailable"), {}, false};
        }
        const std::size_t count = notifications->history().size();
        const bool dnd = notifications->doNotDisturb();
        return InfoView{
            dnd ? "bell-off" : (count > 0 ? "bell-ringing" : "bell"),
            count == 0 ? tr("none") : i18n::tr("okaru.dashboard.count", "count", std::to_string(count)),
            dnd ? tr("dnd") : std::string{}, count > 0 && !dnd
        };
      };
    }

    std::function<InfoView()> screenTimeReader(ScreenTimeService* screenTime) {
      return [screenTime] {
        if (screenTime == nullptr || !screenTime->enabled()) {
          return InfoView{"hourglass", tr("off"), {}, false};
        }
        const ScreenTimeSnapshot snap = screenTime->snapshot(1);
        std::string detail;
        if (!snap.apps.empty()) {
          detail = snap.apps.front().displayName;
        }
        return InfoView{"hourglass", formatDuration(snap.total), detail, true};
      };
    }

    std::function<InfoView()> calendarReader(CalendarService* calendar) {
      return [calendar] {
        const std::string today = formatLocalTime("%A %d");
        if (calendar == nullptr || !calendar->enabled() || !calendar->hasData()) {
          return InfoView{"calendar", today, tr("no-events"), false};
        }
        const auto now = std::chrono::system_clock::now();
        const CalendarEvent* next = nullptr;
        for (const auto& event : calendar->snapshot().events) {
          if (event.end > now && (next == nullptr || event.start < next->start)) {
            next = &event;
          }
        }
        if (next == nullptr) {
          return InfoView{"calendar", today, tr("no-events"), false};
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(next->start.time_since_epoch()).count();
        const std::string when = next->allDay ? tr("all-day") : formatLocalUnixTime(seconds, "%a %H:%M");
        return InfoView{"calendar-event", next->title, when, next->start <= now};
      };
    }

    // ── Reloj ──────────────────────────────────────────────────────────────────────────────────────────

    class ClockCard : public DashboardCard {
    public:
      ClockCard(CardSize size, float scale, float opacity, ConfigService* config, WeatherService* weather)
          : DashboardCard(size, scale, opacity), m_config(config), m_weather(weather) {
        setDirection(FlexDirection::Vertical);
        setAlign(compact() ? FlexAlign::Center : FlexAlign::Start);
        setJustify(FlexJustify::Center);
        setGap(0.0F);
        auto time = makeText(Style::fontSizeTitle * (compact() ? 1.0F : 1.6F) * scale, onSurface(), FontWeight::Bold);
        m_time = time.get();
        addChild(std::move(time));
        auto date = makeText(Style::fontSizeCaption * scale, muted());
        m_date = date.get();
        addChild(std::move(date));
        if (!compact()) {
          auto weatherLine = makeText(Style::fontSizeCaption * scale, muted());
          m_weatherLine = weatherLine.get();
          addChild(std::move(weatherLine));
        }
        setActivation([] { togglePanel("okaru-calendar"); });
      }

      void sync(Renderer& /*renderer*/) override {
        const auto& shell = m_config->config().shell;
        m_time->setText(formatLocalTime(shell.timeFormat.c_str()));
        m_date->setText(formatLocalTime(compact() ? "%a %d" : shell.dateFormat.c_str()));
        if (m_weatherLine != nullptr) {
          const InfoView w = weatherReader(m_weather)();
          m_weatherLine->setText(w.active ? std::format("{} · {}", w.value, w.detail) : std::string(" "));
        }
      }

    private:
      ConfigService* m_config;
      WeatherService* m_weather;
      Label* m_time = nullptr;
      Label* m_date = nullptr;
      Label* m_weatherLine = nullptr;
    };

    // ── Perfil ─────────────────────────────────────────────────────────────────────────────────────────

    class ProfileCard : public DashboardCard {
    public:
      ProfileCard(CardSize size, float scale, float opacity, const CardDeps& deps)
          : DashboardCard(size, scale, opacity), m_config(deps.services.config), m_accounts(deps.services.accounts),
            m_textures(deps.services.asyncTextures) {
        setDirection(FlexDirection::Horizontal);
        setAlign(FlexAlign::Center);
        // Fondo de pantalla actual detrás del contenido, con un velo del color de superficie para leer el texto.
        addChild(ui::image({.out = &m_wallpaper, .fit = ImageFit::Cover, .participatesInLayout = false}));
        addChild(ui::column({
            .out = &m_shade,
            .participatesInLayout = false,
            .configure = [](Flex& f) { f.setFill(colorSpecFromRole(ColorRole::Surface, 0.6F)); },
        }));
        const float avatar = Style::controlHeightLg * 2.6F * scale;
        addChild(ui::image({.out = &m_avatar, .fit = ImageFit::Cover, .width = avatar, .height = avatar}));
        m_avatar->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
        auto text = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
        auto name = makeText(Style::fontSizeTitle * scale, onSurface(), FontWeight::Bold);
        name->setText(sessionDisplayName());
        text->addChild(std::move(name));
        auto host = makeText(Style::fontSizeCaption * scale, muted());
        host->setText(std::format("{}@{}", sessionUserName(), hostName()));
        text->addChild(std::move(host));
        auto uptime = makeText(Style::fontSizeCaption * scale, muted());
        m_uptime = uptime.get();
        text->addChild(std::move(uptime));
        addChild(std::move(text));
        // Grande: versión y compositor en una segunda columna, a la derecha.
        if (m_size == CardSize::Large) {
          auto extra = ui::column({.align = FlexAlign::End, .gap = 0.0F, .flexGrow = 1.0F});
          auto version = makeText(Style::fontSizeCaption * scale, muted());
          version->setText(std::format("OKARU v{}", okaru::build_info::version()));
          version->setTextAlign(TextAlign::End);
          extra->addChild(std::move(version));
          auto compositor = makeText(Style::fontSizeCaption * scale, muted());
          compositor->setTextAlign(TextAlign::End);
          m_compositor = compositor.get();
          extra->addChild(std::move(compositor));
          addChild(std::move(extra));
        }
        setActivation([] { togglePanel("wallpaper"); });
      }

      void sync(Renderer& renderer) override {
        const auto uptime = systemUptime();
        m_uptime->setText(i18n::tr(
            "okaru.dashboard.uptime", "uptime", uptime.has_value() ? formatDuration(*uptime) : std::string("?")
        ));
        if (m_compositor != nullptr) {
          const auto line = compositorVersionLine({});
          m_compositor->setText(line.has_value() && !line->empty() ? *line : compositorLabel());
        }
        const std::string path = shell::resolvedAvatarPath(m_accounts, m_config->config());
        if (path != m_loadedPath && m_textures != nullptr) {
          m_loadedPath = path;
          if (path.empty()) {
            m_avatar->clear(renderer);
          } else {
            (void)m_avatar->setSourceFileAsync(
                renderer, *m_textures, path, static_cast<int>(std::round(m_avatar->width())), false
            );
          }
        }
        std::string wallpaper = m_config->getPaletteWallpaperPath();
        std::error_code ec;
        if (!wallpaper.empty() && !std::filesystem::is_regular_file(wallpaper, ec)) {
          wallpaper.clear(); // color sólido o archivo que ya no existe
        }
        if (wallpaper != m_wallpaperPath && m_textures != nullptr) {
          m_wallpaperPath = wallpaper;
          if (wallpaper.empty()) {
            m_wallpaper->clear(renderer);
          } else {
            (void)m_wallpaper->setSourceFileAsync(
                renderer, *m_textures, wallpaper, 1024, false // el primer sync llega antes del layout (ancho 0)
            );
          }
        }
      }

    protected:
      void doLayout(Renderer& renderer) override {
        DashboardCard::doLayout(renderer);
        // Dentro del borde de la tarjeta, para no taparlo.
        const float inset = Style::borderWidth;
        for (Node* layer : {static_cast<Node*>(m_wallpaper), static_cast<Node*>(m_shade)}) {
          layer->setPosition(inset, inset);
          layer->setSize(std::max(0.0F, width() - inset * 2.0F), std::max(0.0F, height() - inset * 2.0F));
        }
      }

    private:
      ConfigService* m_config;
      AccountsService* m_accounts;
      AsyncTextureCache* m_textures;
      Image* m_avatar = nullptr;
      Image* m_wallpaper = nullptr;
      Flex* m_shade = nullptr;
      Label* m_uptime = nullptr;
      Label* m_compositor = nullptr;
      std::string m_loadedPath;
      std::string m_wallpaperPath;
    };

    // ── Multimedia ─────────────────────────────────────────────────────────────────────────────────────

    class MediaCard : public DashboardCard {
    public:
      MediaCard(CardSize size, float scale, float opacity, MprisService* mpris, HttpClient* http)
          : DashboardCard(size, scale, opacity), m_mpris(mpris), m_http(http) {
        setDirection(FlexDirection::Horizontal);
        setAlign(FlexAlign::Center);
        const float art = Style::controlHeightLg * 1.6F * scale;
        auto artBox = std::make_unique<ClickFlex>();
        artBox->setAlign(FlexAlign::Center);
        artBox->setJustify(FlexJustify::Center);
        artBox->setMinWidth(art);
        artBox->setMinHeight(art);
        artBox->addChild(ui::glyph({.out = &m_artFallback, .glyph = "disc", .glyphSize = art * 0.5F, .color = muted()}));
        artBox->addChild(ui::image({
            .out = &m_art,
            .fit = ImageFit::Cover,
            .width = art,
            .height = art,
            .participatesInLayout = false,
        }));
        artBox->setActivation([] { togglePanel("okaru-media"); });
        addChild(std::move(artBox));

        auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
        auto text = std::make_unique<ClickFlex>();
        text->setDirection(FlexDirection::Vertical);
        text->setAlign(FlexAlign::Stretch);
        auto title = makeText(Style::fontSizeBody * scale, onSurface(), FontWeight::Bold);
        m_title = title.get();
        text->addChild(std::move(title));
        auto artist = makeText(Style::fontSizeCaption * scale, muted());
        m_artist = artist.get();
        text->addChild(std::move(artist));
        text->setActivation([] { togglePanel("okaru-media"); });
        column->addChild(std::move(text));

        auto controls = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        controls->addChild(iconButton(scale, "media-prev", [this] { (void)m_mpris->previousActive(); }));
        auto play = iconButton(scale, "media-play", [this] { (void)m_mpris->playPauseActive(); });
        m_play = play.get();
        controls->addChild(std::move(play));
        controls->addChild(iconButton(scale, "media-next", [this] { (void)m_mpris->nextActive(); }));
        column->addChild(std::move(controls));
        addChild(std::move(column));
      }

      void sync(Renderer& renderer) override {
        const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
        if (!active.has_value()) {
          m_title->setText(tr("nothing-playing"));
          m_artist->setText(" ");
          m_play->setGlyph("media-play");
          m_play->setEnabled(false);
          showArt(renderer, {});
          return;
        }
        m_title->setText(active->title.empty() ? tr("unknown-track") : active->title);
        const std::string artists = mpris::joinArtists(active->artists);
        m_artist->setText(artists.empty() ? std::string(" ") : artists);
        m_play->setEnabled(true);
        m_play->setGlyph(active->playbackStatus == "Playing" ? "media-pause" : "media-play");
        const std::string url = mpris::effectiveArtUrl(*active);
        if (url != m_artUrl || (!url.empty() && !m_art->hasImage())) {
          m_artUrl = url;
          showArt(
              renderer,
              url.empty() ? std::string{}
                          : mpris::resolveArtworkSource(
                                m_http, m_pending, url,
                                [this] {
                                  m_artUrl.clear();
                                  PanelManager::instance().refresh();
                                },
                                m_alive
                            )
          );
        }
      }

    private:
      void showArt(Renderer& renderer, const std::string& path) {
        bool loaded = false;
        if (!path.empty()) {
          loaded = m_art->setSourceFile(renderer, path, static_cast<int>(std::round(m_art->width() * 2.0F)), true, true);
        }
        if (!loaded) {
          m_art->clear(renderer);
        }
        m_art->setVisible(loaded);
        m_artFallback->setVisible(!loaded);
      }

      MprisService* m_mpris;
      HttpClient* m_http;
      std::shared_ptr<int> m_alive = std::make_shared<int>(0);
      std::unordered_set<std::string> m_pending;
      std::string m_artUrl;
      Image* m_art = nullptr;
      Glyph* m_artFallback = nullptr;
      Label* m_title = nullptr;
      Label* m_artist = nullptr;
      Button* m_play = nullptr;
    };

    // ── Deslizadores: volumen, micrófono y brillo ──────────────────────────────────────────────────────

    struct SliderModel {
      std::function<std::optional<float>()> value; // nullopt = sin dispositivo
      std::function<void(float)> set;
      std::function<float()> max;
      std::function<std::string()> label;
      std::function<std::string()> glyph;
      std::function<bool()> mutedState;      // vacío = sin botón de silencio
      std::function<void()> toggleMute;
    };

    class SliderCard : public DashboardCard {
    public:
      SliderCard(CardSize size, float scale, float opacity, std::string_view panel, SliderModel model)
          : DashboardCard(size, scale, opacity), m_model(std::move(model)) {
        setDirection(FlexDirection::Vertical);
        setJustify(FlexJustify::Center);
        setGap(Style::spaceXs * scale);
        auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        auto open = iconButton(scale, "volume", [id = std::string(panel)] { togglePanel(id); });
        m_icon = open.get();
        header->addChild(std::move(open));
        auto label = makeText(Style::fontSizeCaption * scale, muted(), FontWeight::Normal, 1.0F);
        m_label = label.get();
        header->addChild(std::move(label));
        auto value = makeText(Style::fontSizeBody * scale, onSurface(), FontWeight::Bold);
        m_value = value.get();
        header->addChild(std::move(value));
        addChild(std::move(header));

        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        row->addChild(ui::slider({
            .out = &m_slider,
            .minValue = 0.0F,
            .maxValue = 1.0F,
            .step = 0.01F,
            .trackHeight = Style::sliderTrackHeight * scale,
            .thumbSize = Style::sliderThumbSize * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 1.0F,
            .onValueChanged = [this](double v) {
              if (m_syncing) {
                return;
              }
              m_value->setText(std::format("{:.0f}%", v * 100.0));
              const auto now = std::chrono::steady_clock::now();
              m_pending = static_cast<float>(v);
              if (now - m_lastSent >= std::chrono::milliseconds(16)) {
                m_model.set(m_pending);
                m_lastSent = now;
              }
            },
            .onDragEnd = [this] {
              if (m_pending >= 0.0F) {
                m_model.set(m_pending);
              }
            },
        }));
        if (m_model.mutedState) {
          auto mute = iconButton(scale, "volume-mute", [this] {
            m_model.toggleMute();
            PanelManager::instance().refresh();
          });
          m_mute = mute.get();
          row->addChild(std::move(mute));
        }
        addChild(std::move(row));
      }

      void sync(Renderer& /*renderer*/) override {
        const auto value = m_model.value();
        m_label->setText(m_model.label());
        m_icon->setGlyph(m_model.glyph());
        m_slider->setEnabled(value.has_value());
        if (m_mute != nullptr) {
          const bool mutedNow = m_model.mutedState();
          m_mute->setEnabled(value.has_value());
          m_mute->setVariant(mutedNow ? ButtonVariant::Destructive : ButtonVariant::Ghost);
        }
        if (m_slider->dragging()) {
          return;
        }
        m_syncing = true;
        m_slider->setRange(0.0F, m_model.max());
        m_slider->setValue(value.value_or(0.0F));
        m_syncing = false;
        m_value->setText(value.has_value() ? std::format("{:.0f}%", *value * 100.0F) : std::string("—"));
      }

    private:
      SliderModel m_model;
      Button* m_icon = nullptr;
      Label* m_label = nullptr;
      Label* m_value = nullptr;
      Slider* m_slider = nullptr;
      Button* m_mute = nullptr;
      bool m_syncing = false;
      float m_pending = -1.0F;
      std::chrono::steady_clock::time_point m_lastSent;
    };

    SliderModel audioModel(PipeWireService* audio, ConfigService* config, bool input) {
      const auto node = [audio, input]() -> const AudioNode* {
        if (audio == nullptr) {
          return nullptr;
        }
        return input ? audio->defaultSource() : audio->defaultSink();
      };
      return SliderModel{
          .value = [node]() -> std::optional<float> {
            const AudioNode* n = node();
            return n != nullptr ? std::optional<float>(n->volume) : std::nullopt;
          },
          .set = [audio, node, input](float v) {
            if (const AudioNode* n = node(); n != nullptr) {
              if (input) {
                audio->setSourceVolume(n->id, v);
              } else {
                audio->setSinkVolume(n->id, v);
              }
              audio->emitVolumePreview(input, n->id, v);
            }
          },
          .max = [config] { return maxAudioVolume(config->config().audio); },
          .label = [node, input] {
            const AudioNode* n = node();
            return n != nullptr ? audioDeviceLabel(*n) : tr(input ? "no-input" : "no-output");
          },
          .glyph = [node, input] {
            const AudioNode* n = node();
            return std::string(audioVolumeGlyph(n != nullptr ? n->volume : 0.0F, n == nullptr || n->muted, input));
          },
          .mutedState = [node] {
            const AudioNode* n = node();
            return n != nullptr && n->muted;
          },
          .toggleMute = [audio, node, input] {
            if (const AudioNode* n = node(); n != nullptr) {
              if (input) {
                audio->setSourceMuted(n->id, !n->muted);
              } else {
                audio->setSinkMuted(n->id, !n->muted);
              }
            }
          },
      };
    }

    SliderModel brightnessModel(BrightnessService* brightness) {
      const auto first = [brightness]() -> const BrightnessDisplay* {
        if (brightness == nullptr || !brightness->available()) {
          return nullptr;
        }
        const auto& displays = brightness->displays();
        const auto it = std::ranges::find_if(displays, &BrightnessDisplay::controllable);
        return it == displays.end() ? nullptr : &*it;
      };
      return SliderModel{
          .value = [first]() -> std::optional<float> {
            const BrightnessDisplay* d = first();
            return d != nullptr ? std::optional<float>(d->brightness) : std::nullopt;
          },
          .set = [brightness](float v) { brightness->setAllBrightness(v); },
          .max = [] { return 1.0F; },
          .label = [first] {
            const BrightnessDisplay* d = first();
            return d != nullptr ? tr("brightness-all") : tr("unavailable");
          },
          .glyph = [] { return std::string("brightness-half"); },
          .mutedState = {},
          .toggleMute = {},
      };
    }

    // ── Sesión ─────────────────────────────────────────────────────────────────────────────────────────

    class SessionCard : public DashboardCard {
    public:
      SessionCard(CardSize size, float scale, float opacity, ConfigService* config, SessionActionRunner* runner)
          : DashboardCard(size, scale, opacity), m_runner(runner) {
        setAlign(FlexAlign::Center);
        setJustify(FlexJustify::Center);
        if (compact()) {
          setDirection(FlexDirection::Vertical);
          addChild(ui::glyph({.glyph = "power", .glyphSize = cardGlyphSize(true, scale), .color = onSurface()}));
          auto label = makeText(Style::fontSizeCaption * scale, onSurface(), FontWeight::Bold);
          label->setText(tr("cards.session"));
          addChild(std::move(label));
          setActivation([] { togglePanel("session"); });
          return;
        }
        setDirection(FlexDirection::Horizontal);
        setGap(Style::spaceXs * scale);
        for (const auto& action : config->config().shell.session.actions) {
          if (!action.enabled) {
            continue;
          }
          const std::string glyph = action.glyph.value_or(session_action::defaultGlyph(action.action));
          auto button = iconButton(scale, glyph, {});
          Button* raw = button.get();
          raw->setTooltip(action.label.value_or(i18n::tr(session_action::labelKey(action.action))));
          raw->setOnClick([this, raw, action] { activate(raw, action); });
          m_buttons.push_back(raw);
          addChild(std::move(button));
        }
      }

    private:
      // Bloquear va directo; lo demás pide un segundo clic en 3 s.
      void activate(Button* button, const SessionPanelActionConfig& action) {
        const auto now = std::chrono::steady_clock::now();
        const bool confirmed = action.action == "lock" || (m_armed == button && now - m_armedAt < std::chrono::seconds(3));
        for (Button* b : m_buttons) {
          b->setVariant(ButtonVariant::Ghost);
        }
        if (!confirmed) {
          m_armed = button;
          m_armedAt = now;
          button->setVariant(ButtonVariant::Destructive);
          PanelManager::instance().refresh();
          return;
        }
        m_armed = nullptr;
        PanelManager::instance().close();
        if (m_runner != nullptr) {
          m_runner->invoke(action);
        }
      }

      SessionActionRunner* m_runner;
      std::vector<Button*> m_buttons;
      Button* m_armed = nullptr;
      std::chrono::steady_clock::time_point m_armedAt;
    };

    // ── Acceso directo ─────────────────────────────────────────────────────────────────────────────────

    class ShortcutCard : public DashboardCard {
    public:
      ShortcutCard(CardSize size, float scale, float opacity, Shortcut* shortcut)
          : DashboardCard(size, scale, opacity), m_shortcut(shortcut), m_opacity(opacity) {
        setPadding(0.0F);
        setAlign(FlexAlign::Stretch);
        auto button = ui::button({
            .text = compact() ? std::nullopt : std::optional<std::string>(shortcut->displayLabel()),
            .glyph = shortcut->displayIcon(),
            .glyphSize = cardGlyphSize(compact(), scale),
            .contentAlign = compact() ? ButtonContentAlign::Center : ButtonContentAlign::Start,
            .padding = Style::spaceMd * scale,
            .gap = Style::spaceSm * scale,
            .flexGrow = 1.0F,
            .onClick = [this] {
              m_shortcut->onClick();
              PanelManager::instance().refresh();
            },
            .onRightClick = [this] { m_shortcut->onRightClick(); },
        });
        m_button = button.get();
        if (compact()) {
          m_button->setTooltip(shortcut->displayLabel());
        }
        if (auto* area = m_button->inputArea(); area != nullptr) {
          area->setOnAxisHandler([this](const InputArea::PointerData& data) -> bool {
            const float steps = data.scrollSteps();
            if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || steps == 0.0F) {
              return false;
            }
            m_shortcut->onScroll(steps > 0.0F ? -1 : 1);
            return true;
          });
        }
        addChild(std::move(button));
      }

      void sync(Renderer& /*renderer*/) override {
        const bool on = m_shortcut->isToggle() && m_shortcut->active();
        m_button->setEnabled(m_shortcut->enabled());
        m_button->setVariant(on ? ButtonVariant::Primary : ButtonVariant::Default);
        m_button->setSurfaceOpacity(on ? 1.0F : m_opacity);
        m_button->setGlyph(m_shortcut->displayIcon());
        if (!compact() && m_button->label() != nullptr && m_button->label()->text() != m_shortcut->displayLabel()) {
          m_button->setText(m_shortcut->displayLabel());
        }
      }

    private:
      Shortcut* m_shortcut;
      float m_opacity;
      Button* m_button = nullptr;
    };

  } // namespace

  // ── ClickFlex ──────────────────────────────────────────────────────────────────────────────────────────

  void ClickFlex::setActivation(std::function<void()> onClick, std::function<void()> onRightClick) {
    auto area = ui::inputArea({});
    area->setParticipatesInLayout(false);
    area->setAcceptedButtons(
        onRightClick ? InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}) : InputArea::buttonMask(BTN_LEFT)
    );
    area->setOnEnter([this](const InputArea::PointerData&) { applyHover(true); });
    area->setOnLeave([this] { applyHover(false); });
    area->setOnClick([onClick = std::move(onClick), onRightClick = std::move(onRightClick)](
                         const InputArea::PointerData& data
                     ) {
      if (data.button == BTN_RIGHT) {
        if (onRightClick) {
          onRightClick();
        }
      } else if (onClick) {
        onClick();
      }
    });
    m_area = static_cast<InputArea*>(addChild(std::move(area)));
  }

  void ClickFlex::applyHover(bool hovered) {
    if (hovered) {
      setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
    } else if (m_restBorder && Style::cardBordersEnabled()) {
      setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    } else {
      clearBorder();
    }
  }

  void ClickFlex::doLayout(Renderer& renderer) {
    Flex::doLayout(renderer);
    if (m_area != nullptr) {
      m_area->setPosition(0.0F, 0.0F);
      m_area->setSize(width(), height());
    }
  }

  LayoutSize ClickFlex::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
    return measureByLayout(renderer, constraints);
  }

  void ClickFlex::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

  // ── DashboardCard ──────────────────────────────────────────────────────────────────────────────────────

  DashboardCard::DashboardCard(CardSize size, float scale, float fillOpacity) : m_size(size), m_scale(scale) {
    control_center::applySectionCardStyle(*this, scale, fillOpacity);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceMd * scale);
    setClipChildren(true);
    m_restBorder = true;
  }

  std::string cardLabel(std::string_view type, const Shortcut* shortcut) {
    if (isShortcutType(type)) {
      return shortcut != nullptr ? shortcut->displayLabel() : std::string(shortcutId(type));
    }
    return tr("cards." + std::string(type));
  }

  std::unique_ptr<DashboardCard> createCard(
      std::string_view type, CardSize size, const CardDeps& deps, float scale, float opacity, Shortcut* shortcut
  ) {
    const auto& s = deps.services;
    const auto info = [&](std::function<InfoView()> read, std::function<void()> rightClick = {}) {
      return std::make_unique<InfoCard>(
          size, scale, opacity, cardLabel(type, nullptr), std::move(read),
          [panel = std::string(panelFor(type))] { togglePanel(panel); }, std::move(rightClick)
      );
    };
    if (isShortcutType(type)) {
      return shortcut != nullptr ? std::make_unique<ShortcutCard>(size, scale, opacity, shortcut) : nullptr;
    }
    if (type == "profile") {
      return std::make_unique<ProfileCard>(size, scale, opacity, deps);
    }
    if (type == "clock") {
      return std::make_unique<ClockCard>(size, scale, opacity, s.config, s.weather);
    }
    if (type == "media") {
      return std::make_unique<MediaCard>(size, scale, opacity, s.mpris, s.httpClient);
    }
    if (type == "volume" || type == "microphone") {
      return std::make_unique<SliderCard>(
          size, scale, opacity, panelFor(type), audioModel(s.audio, s.config, type == "microphone")
      );
    }
    if (type == "brightness") {
      return std::make_unique<SliderCard>(size, scale, opacity, panelFor(type), brightnessModel(s.brightness));
    }
    if (type == "session") {
      return std::make_unique<SessionCard>(size, scale, opacity, s.config, deps.session);
    }
    if (type == "network") {
      return info(networkReader(s.network));
    }
    if (type == "bluetooth") {
      return info(bluetoothReader(s.bluetooth));
    }
    if (type == "power") {
      return info(powerReader(s.upower, s.powerProfiles));
    }
    if (type == "system") {
      return info(systemReader(s.sysmon));
    }
    if (type == "weather") {
      return info(weatherReader(s.weather));
    }
    if (type == "notifications") {
      return info(notificationsReader(s.notifications), [n = s.notifications] {
        if (n != nullptr) {
          (void)n->toggleDoNotDisturb();
          PanelManager::instance().refresh();
        }
      });
    }
    if (type == "screen-time") {
      return info(screenTimeReader(s.screenTime));
    }
    if (type == "calendar") {
      return info(calendarReader(s.calendar));
    }
    return nullptr;
  }

} // namespace okaru::dashboard
