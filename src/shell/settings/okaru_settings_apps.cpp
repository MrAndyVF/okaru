#include "shell/settings/okaru_settings_apps.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/dock/pinned_apps.h"
#include "shell/settings/settings_content_common.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/icon_resolver.h"
#include "system/okaru_apps.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/virtual_list_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace settings {
  namespace {

    using okaru::apps::PackageInfo;
    using okaru::apps::PackageManager;

    struct AppsState {
      std::function<void()> rebuild;
      std::string error;
      std::string notice;
      std::string fileTypesApp; // id elegido en Tipos de archivo
      std::string filter;       // búsqueda en Instaladas
      std::string expanded;     // id con los detalles abiertos
      std::string confirmUninstall;
      std::string uninstalling;
      std::map<std::string, std::optional<PackageInfo>> packages; // id -> paquete (consultado al abrir)
      std::vector<std::string> rows;  // ids visibles en Instaladas (filtrados)
      VirtualListView* list = nullptr; // la de la página actual; se rehace con cada página
    };

    AppsState& state() {
      static AppsState s;
      return s;
    }

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.apps." + key); }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    void report(const std::optional<std::string>& error) {
      state().error = error.value_or(std::string());
      state().notice.clear();
      rebuild();
    }

    const DesktopEntry* findApp(std::string_view id) {
      const auto& apps = desktopEntries();
      const auto it = std::ranges::find(apps, id, &DesktopEntry::id);
      return it == apps.end() ? nullptr : &*it;
    }

    std::string appName(std::string_view id) {
      const DesktopEntry* app = findApp(id);
      return app != nullptr ? app->name : std::string(id);
    }

    std::unique_ptr<Flex> textColumn(float scale, const std::string& title, const std::string& detail) {
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (!detail.empty()) {
        auto label = makeLabel(detail, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
        label->setMaxLines(2);
        text->addChild(std::move(label));
      }
      return text;
    }

    std::unique_ptr<Flex> row(float scale) {
      return ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
    }

    std::unique_ptr<Node> intro(float scale, const std::string& key) {
      auto label = makeLabel(tr(key), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      label->setMaxLines(3);
      return label;
    }

    // Apps que se muestran en las listas: visibles, o escondidas por OKARU (para poder mostrarlas otra vez).
    std::vector<const DesktopEntry*> listedApps() {
      std::vector<const DesktopEntry*> out;
      for (const auto& app : desktopEntries()) {
        if (!app.hidden && (!app.noDisplay || okaru::apps::hiddenByOkaru(app))) {
          out.push_back(&app);
        }
      }
      std::ranges::sort(out, {}, [](const DesktopEntry* app) { return app->nameLower; });
      return out;
    }

    // ── Apps por defecto ──────────────────────────────────────────────────────────────────────────────

    void addDefaultsPage(Flex& content, float scale) {
      Flex* body = addSettingsCard(content, tr("defaults"), scale);
      body->addChild(intro(scale, "defaults-intro"));
      const auto defaults = okaru::apps::loadDefaults();
      for (const auto& category : okaru::apps::defaultCategories()) {
        const auto candidates = okaru::apps::candidatesFor(category);
        const std::string current = defaults.get(category.mimes.front());
        std::vector<std::string> names;
        std::vector<std::string> ids;
        std::optional<std::size_t> selected;
        for (const DesktopEntry* app : candidates) {
          if (app->id == current) {
            selected = names.size();
          }
          names.push_back(app->name);
          ids.push_back(app->id);
        }
        auto line = row(scale);
        line->addChild(ui::glyph({
            .glyph = std::string(category.glyph),
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        }));
        std::string detail;
        if (candidates.empty()) {
          detail = tr("no-candidates");
        } else if (!selected.has_value() && !current.empty()) {
          detail = i18n::tr("settings.okaru.apps.current-other", "app", appName(current));
        }
        line->addChild(textColumn(scale, tr("default-" + std::string(category.key)), detail));
        if (!candidates.empty()) {
          line->addChild(ui::select({
              .options = names,
              .selectedIndex = selected,
              .placeholder = tr("not-set"),
              .width = 280.0F * scale,
              .onSelectionChanged =
                  [&category, ids](std::size_t index, std::string_view /*label*/) {
                    const DesktopEntry* app = index < ids.size() ? findApp(ids[index]) : nullptr;
                    if (app != nullptr) {
                      report(okaru::apps::setDefault(okaru::apps::mimesToSet(category, *app), app->id));
                    }
                  },
          }));
        }
        body->addChild(std::move(line));
      }
    }

    // ── Tipos de archivo por app ──────────────────────────────────────────────────────────────────────

    void addFileTypesPage(Flex& content, float scale) {
      auto& s = state();
      Flex* pick = addSettingsCard(content, tr("file-types"), scale);
      pick->addChild(intro(scale, "file-types-intro"));
      std::vector<std::string> names;
      std::vector<std::string> ids;
      std::optional<std::size_t> selected;
      for (const DesktopEntry* app : listedApps()) {
        if (app->mimeTypes.empty()) {
          continue;
        }
        if (app->id == s.fileTypesApp) {
          selected = names.size();
        }
        names.push_back(app->name);
        ids.push_back(app->id);
      }
      auto line = row(scale);
      line->addChild(textColumn(scale, tr("app"), {}));
      line->addChild(ui::select({
          .options = names,
          .selectedIndex = selected,
          .placeholder = tr("choose-app"),
          .width = 320.0F * scale,
          .onSelectionChanged =
              [ids](std::size_t index, std::string_view /*label*/) {
                if (index < ids.size()) {
                  state().fileTypesApp = ids[index];
                  rebuild();
                }
              },
      }));
      pick->addChild(std::move(line));

      const DesktopEntry* app = selected.has_value() ? findApp(s.fileTypesApp) : nullptr;
      if (app == nullptr) {
        return;
      }
      Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.apps.types-of", "app", app->name), scale);
      const auto defaults = okaru::apps::loadDefaults();
      std::vector<std::string> mimes = app->mimeTypes;
      std::ranges::sort(mimes);
      mimes.erase(std::ranges::unique(mimes).begin(), mimes.end());

      auto actions = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true}
      );
      actions->addChild(ui::button({
          .text = tr("use-for-all"),
          .glyph = "checks",
          .variant = ButtonVariant::Default,
          .onClick = [mimes, id = app->id] { report(okaru::apps::setDefault(mimes, id)); },
      }));
      body->addChild(std::move(actions));

      for (const auto& mime : mimes) {
        const auto extensions = okaru::apps::extensionsFor(mime);
        std::string title;
        for (const auto& ext : extensions) {
          title += title.empty() ? ext : "  " + ext;
        }
        const std::string current = defaults.get(mime);
        const bool isDefault = current == app->id;
        std::string detail = mime;
        if (!current.empty() && !isDefault) {
          detail += "  ·  " + i18n::tr("settings.okaru.apps.opens-with", "app", appName(current));
        }
        auto typeRow = row(scale);
        typeRow->addChild(textColumn(scale, title.empty() ? mime : title, title.empty() ? std::string() : detail));
        typeRow->addChild(ui::toggle({
            .checked = isDefault,
            .scale = scale,
            .onChange =
                [mime, id = app->id](bool on) {
                  // Apagar quita la elección del usuario: vuelve a mandar la del sistema.
                  report(okaru::apps::setDefault({mime}, on ? id : std::string()));
                },
        }));
        body->addChild(std::move(typeRow));
      }
    }

    // ── Apps instaladas ───────────────────────────────────────────────────────────────────────────────

    std::string managerLabel(PackageManager manager) {
      switch (manager) {
      case PackageManager::Pacman:
        return "pacman";
      case PackageManager::Aur:
        return "AUR";
      case PackageManager::Flatpak:
        return "Flatpak";
      case PackageManager::Snap:
        return "Snap";
      }
      return {};
    }

    void addInfoLine(Flex& body, float scale, const std::string& label, const std::string& value) {
      if (value.empty()) {
        return;
      }
      auto line = ui::row({.align = FlexAlign::Start, .gap = Style::spaceMd * scale, .fillWidth = true});
      auto key = makeLabel(label, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      key->setMinWidth(110.0F * scale);
      line->addChild(std::move(key));
      auto text = makeLabel(value, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface));
      text->setMaxLines(2);
      text->setFlexGrow(1.0F);
      line->addChild(std::move(text));
      body.addChild(std::move(line));
    }

    void addDetails(Flex& body, float scale, const DesktopEntry& app) {
      auto& s = state();
      auto cached = s.packages.find(app.id);
      if (cached == s.packages.end()) {
        cached = s.packages.emplace(app.id, okaru::apps::packageFor(app)).first;
      }
      const auto& package = cached->second;
      auto details = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = (28.0F + Style::spaceMd) * scale,
      });
      if (package.has_value()) {
        addInfoLine(*details, scale, tr("source"), managerLabel(package->manager));
        addInfoLine(*details, scale, tr("package"), package->name);
        addInfoLine(*details, scale, tr("version"), package->version);
        addInfoLine(*details, scale, tr("size"), package->size);
      } else {
        addInfoLine(*details, scale, tr("source"), tr("no-package"));
      }
      addInfoLine(*details, scale, tr("file"), app.path);
      if (!app.mimeTypes.empty()) {
        addInfoLine(*details, scale, tr("file-types-count"), std::to_string(app.mimeTypes.size()));
      }

      if (package.has_value()) {
        auto actions = ui::row(
            {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true}
        );
        if (s.uninstalling == app.id) {
          actions->addChild(
              makeLabel(tr("uninstalling"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
          );
        } else {
          const bool armed = s.confirmUninstall == app.id;
          if (armed) {
            actions->addChild(ui::button({
                .text = tr("cancel"),
                .variant = ButtonVariant::Outline,
                .onClick =
                    [] {
                      state().confirmUninstall.clear();
                      rebuild();
                    },
            }));
          }
          actions->addChild(ui::button({
              .text = armed ? i18n::tr("settings.okaru.apps.uninstall-confirm", "package", package->name)
                            : tr("uninstall"),
              .glyph = "trash",
              .enabled = s.uninstalling.empty(),
              .variant = armed ? ButtonVariant::Destructive : ButtonVariant::Outline,
              .onClick =
                  [armed, id = app.id, name = app.name, pkg = *package] {
                    auto& st = state();
                    if (!armed) {
                      st.confirmUninstall = id;
                      rebuild();
                      return;
                    }
                    st.confirmUninstall.clear();
                    st.uninstalling = id;
                    rebuild();
                    okaru::apps::uninstall(pkg, [id, name](const std::optional<std::string>& error) {
                      auto& done = state();
                      done.uninstalling.clear();
                      done.packages.erase(id);
                      if (done.expanded == id) {
                        done.expanded.clear();
                      }
                      done.error = error.value_or(std::string());
                      done.notice = error ? std::string() : i18n::tr("settings.okaru.apps.uninstalled", "app", name);
                      rebuild();
                    });
                  },
          }));
        }
        details->addChild(std::move(actions));
      }
      body.addChild(std::move(details));
    }

    constexpr float kRowHeight = 52.0F;
    constexpr float kIconSize = 28.0F;
    constexpr std::size_t kVisibleRows = 9;

    void filterRows() {
      auto& s = state();
      std::string filter = s.filter;
      std::ranges::transform(filter, filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      s.rows.clear();
      for (const DesktopEntry* app : listedApps()) {
        if (filter.empty() || app->nameLower.contains(filter) || app->idLower.contains(filter)) {
          s.rows.push_back(app->id);
        }
      }
    }

    // Fila reutilizable de la lista virtual: solo existen las que se ven.
    class AppRow final : public Flex {
    public:
      explicit AppRow(float scale) : m_scale(scale) {
        setDirection(FlexDirection::Horizontal);
        setAlign(FlexAlign::Center);
        setGap(Style::spaceMd * scale);
        setPadding(0.0F, Style::spaceSm * scale);
        const float size = kIconSize * scale;
        m_icon = static_cast<Image*>(addChild(ui::image({.fit = ImageFit::Contain, .width = size, .height = size})));
        m_glyph = static_cast<Glyph*>(addChild(ui::glyph({
            .glyph = "app-window",
            .glyphSize = size,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })));
        auto text = ui::column({.align = FlexAlign::Stretch, .gap = 2.0F * scale, .flexGrow = 1.0F});
        m_name = static_cast<Label*>(text->addChild(makeLabel(
            "", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
        )));
        m_name->setMaxLines(1);
        m_detail = static_cast<Label*>(text->addChild(
            makeLabel("", Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
        ));
        m_detail->setMaxLines(1);
        addChild(std::move(text));
        m_open = addButton("player-play");
        m_pin = addButton("pin");
        m_hide = addButton("eye-off");
        m_details = addButton("chevron-down");
        m_open->setTooltip(tr("open"));
        m_details->setTooltip(tr("details"));
      }

      void bind(
          const DesktopEntry& app, Renderer& renderer, AsyncTextureCache* cache, ConfigService* config,
          float width
      ) {
        static IconResolver resolver;
        const auto& s = state();
        const bool hidden = app.noDisplay;
        const bool pinned = config != nullptr && shell::dock::pinned_apps::isPinned(*config, app);
        const bool expanded = s.expanded == app.id;
        const int target = static_cast<int>(kIconSize * m_scale * 2.0F);
        const std::string& path = app.icon.empty() ? std::string() : resolver.resolve(app.icon, target);
        if (path != m_iconPath) {
          m_iconPath = path;
          if (config != nullptr) {
            m_icon->setAppIconColorization(effectiveShellAppIconColorizationTint(config->config().shell));
          }
          // La carga asíncrona devuelve false mientras decodifica: con ruta, el icono se muestra igual.
          const bool ok = !path.empty();
          if (ok && cache != nullptr) {
            (void)m_icon->setSourceFileAsync(renderer, *cache, path, target, true);
          } else if (ok) {
            (void)m_icon->setSourceFile(renderer, path, target, true);
          } else {
            m_icon->clear(renderer);
          }
          m_icon->setVisible(ok);
          m_icon->setParticipatesInLayout(ok);
          m_glyph->setVisible(!ok);
          m_glyph->setParticipatesInLayout(!ok);
        }
        m_icon->setOpacity(hidden ? 0.5F : 1.0F);
        m_name->setText(app.name);
        m_detail->setText(hidden ? tr("hidden") : app.comment);

        const std::string id = app.id;
        m_open->setVisible(!hidden);
        m_open->setParticipatesInLayout(!hidden);
        m_open->setOnClick([id, config] {
          const DesktopEntry* entry = findApp(id);
          if (entry == nullptr) {
            return;
          }
          desktop_entry_launch::LaunchOptions options{.dbusActivatable = entry->dbusActivatable, .dbusAppId = id};
          if (config != nullptr) {
            options.runAsSystemdService = config->config().shell.launchAppsAsSystemdServices;
            options.customCommand = config->config().shell.launchAppsCustomCommand;
          }
          (void)desktop_entry_launch::launchEntry(*entry, options);
        });
        m_pin->setVisible(config != nullptr && !hidden);
        m_pin->setParticipatesInLayout(config != nullptr && !hidden);
        m_pin->setGlyph(pinned ? "pinned-off" : "pin");
        m_pin->setTooltip(tr(pinned ? "unpin" : "pin"));
        m_pin->setOnClick([id, config, pinned] {
          if (const DesktopEntry* entry = findApp(id); entry != nullptr && config != nullptr) {
            shell::dock::pinned_apps::setPinned(*config, *entry, !pinned);
            if (state().list != nullptr) {
              state().list->notifyDataChanged();
            }
          }
        });
        m_hide->setGlyph(hidden ? "eye" : "eye-off");
        m_hide->setTooltip(tr(hidden ? "show" : "hide"));
        m_hide->setOnClick([id, hidden] {
          if (const DesktopEntry* entry = findApp(id)) {
            report(okaru::apps::setHidden(*entry, !hidden));
          }
        });
        m_details->setGlyph(expanded ? "chevron-up" : "chevron-down");
        m_details->setOnClick([id, expanded] {
          state().expanded = expanded ? std::string() : id;
          state().confirmUninstall.clear();
          rebuild();
        });
        setSize(width, kRowHeight * m_scale);
      }

    private:
      Button* addButton(const char* glyph) {
        auto button = ui::button({.glyph = std::string(glyph), .variant = ButtonVariant::Ghost});
        Button* raw = button.get();
        addChild(std::move(button));
        return raw;
      }

      float m_scale;
      std::string m_iconPath = "\n"; // nunca coincide con una ruta: fuerza la primera carga
      Image* m_icon = nullptr;
      Glyph* m_glyph = nullptr;
      Label* m_name = nullptr;
      Label* m_detail = nullptr;
      Button* m_open = nullptr;
      Button* m_pin = nullptr;
      Button* m_hide = nullptr;
      Button* m_details = nullptr;
    };

    class AppsAdapter final : public VirtualListAdapter {
    public:
      void setContext(float scale, AsyncTextureCache* cache, ConfigService* config) {
        m_scale = scale;
        m_cache = cache;
        m_config = config;
        ++m_revision;
      }
      void touch() { ++m_revision; }

      [[nodiscard]] std::size_t itemCount() const override { return state().rows.size(); }
      [[nodiscard]] std::uint64_t itemRevision(std::size_t /*index*/) const override { return m_revision; }
      [[nodiscard]] float measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) override {
        return kRowHeight * m_scale;
      }
      [[nodiscard]] std::unique_ptr<Node> createItem() override { return std::make_unique<AppRow>(m_scale); }
      void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool /*hovered*/) override {
        const auto& rows = state().rows;
        const DesktopEntry* app = index < rows.size() ? findApp(rows[index]) : nullptr;
        if (app != nullptr) {
          static_cast<AppRow&>(item).bind(*app, renderer, m_cache, m_config, width);
        }
      }

    private:
      float m_scale = 1.0F;
      AsyncTextureCache* m_cache = nullptr;
      ConfigService* m_config = nullptr;
      std::uint64_t m_revision = 0;
    };

    AppsAdapter& adapter() {
      static AppsAdapter a;
      return a;
    }

    void addInstalledPage(Flex& content, float scale, ConfigService* config, AsyncTextureCache* cache) {
      auto& s = state();
      if (const DesktopEntry* app = s.expanded.empty() ? nullptr : findApp(s.expanded)) {
        Flex* details = addSettingsCard(content, app->name, scale);
        addDetails(*details, scale, *app);
      }
      Flex* body = addSettingsCard(content, tr("installed"), scale);
      body->addChild(intro(scale, "installed-intro"));
      body->addChild(ui::input({
          .value = s.filter,
          .placeholder = tr("search"),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .flexGrow = 1.0F,
          .onChange =
              [](const std::string& value) {
                // Filtra sin rehacer la ventana: solo cambia lo que muestra la lista.
                state().filter = value;
                filterRows();
                adapter().touch();
                if (state().list != nullptr) {
                  state().list->notifyDataChanged();
                }
              },
      }));
      filterRows();
      adapter().setContext(scale, cache, config);
      static ScrollViewState scrollState;
      // La lista virtual no se mide sola: va en una caja de alto fijo y la llena.
      const float listHeight = kRowHeight * scale * static_cast<float>(kVisibleRows);
      auto box = ui::column({.align = FlexAlign::Stretch, .minHeight = listHeight, .height = listHeight});
      box->addChild(ui::virtualListView({
          .out = &s.list,
          .state = &scrollState,
          .contentScale = scale,
          .adapter = &adapter(),
          .flexGrow = 1.0F,
      }));
      body->addChild(std::move(box));
    }

  } // namespace

  void addSettingsApps(Flex& content, const SettingsAppsContext& ctx) {
    auto& s = state();
    s.list = nullptr;
    s.rebuild = ctx.requestRebuild;
    const float scale = ctx.scale;

    if (!s.error.empty() || !s.notice.empty()) {
      const bool isError = !s.error.empty();
      auto message = makeLabel(
          isError ? s.error : s.notice, Style::fontSizeBody * scale,
          colorSpecFromRole(isError ? ColorRole::Error : ColorRole::Primary), FontWeight::Bold
      );
      message->setMaxLines(4);
      content.addChild(std::move(message));
    }

    if (ctx.page == "apps-defaults") {
      addDefaultsPage(content, scale);
    } else if (ctx.page == "apps-file-types") {
      addFileTypesPage(content, scale);
    } else {
      addInstalledPage(content, scale, ctx.config, ctx.textures);
    }
  }

} // namespace settings
