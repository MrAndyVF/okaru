#include "shell/settings/okaru_settings_hyprpm.h"

#include "compositors/hyprland/okaru_hyprpm.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "core/timer_manager.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <memory>
#include <string>

namespace settings {
  namespace {

    using okaru::hyprpm::Action;

    constexpr std::size_t kMaxLogBytes = 16 * 1024;

    struct HyprpmState {
      std::function<void()> rebuild;
      std::string urlDraft;
      std::string confirmRemove; // repo con el primer clic de "Quitar" dado
      bool running = false;
      std::string runningLabel;
      std::string log;
      bool failed = false;
    };

    HyprpmState& state() {
      static HyprpmState s;
      return s;
    }

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.hyprpm." + key); }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    // La compilación imprime mucho: se redibuja como mucho cada 250 ms.
    void rebuildSoon() {
      static Timer timer;
      if (!timer.active()) {
        timer.start(std::chrono::milliseconds(250), [] { rebuild(); });
      }
    }

    void appendLog(std::string_view chunk) {
      auto& log = state().log;
      log += chunk;
      if (log.size() > kMaxLogBytes) {
        log.erase(0, log.size() - kMaxLogBytes);
      }
    }

    // Corre una acción de hyprpm en segundo plano; la salida va al registro de la página.
    void run(Action action, const std::string& arg, std::string label, std::string authMessage) {
      auto& s = state();
      const auto argv = okaru::hyprpm::commandFor(action, arg);
      if (s.running || !argv.has_value()) {
        s.failed = !argv.has_value();
        s.log = argv.has_value() ? s.log : tr("invalid") + "\n";
        rebuild();
        return;
      }
      s.running = true;
      s.failed = false;
      s.runningLabel = std::move(label);
      s.log.clear();
      s.confirmRemove.clear();
      process::RunOptions options;
      options.env = okaru::hyprpm::authEnv(std::move(authMessage));
      options.timeout = std::chrono::minutes(20); // compilar hyprland-plugins tarda
      const auto onChunk = [](std::string_view chunk) {
        DeferredCall::callLater([text = std::string(chunk)] {
          appendLog(text);
          rebuildSoon();
        });
      };
      const bool started = process::runAsync(
          *argv,
          process::RunCallbacks{
              .stdOut = onChunk,
              .stdErr = onChunk,
              .onExit =
                  [](process::RunResult result) {
                    DeferredCall::callLater([ok = static_cast<bool>(result)] {
                      state().running = false;
                      state().failed = !ok;
                      rebuild();
                    });
                  },
          },
          options
      );
      if (!started) {
        s.running = false;
        s.failed = true;
        s.log = tr("start-failed") + "\n";
      }
      rebuild();
    }

    std::unique_ptr<Flex> row(float scale, const std::string& title, const std::string& detail, bool detailIsError) {
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (!detail.empty()) {
        auto label = makeLabel(
            detail, Style::fontSizeCaption * scale,
            colorSpecFromRole(detailIsError ? ColorRole::Error : ColorRole::OnSurfaceVariant)
        );
        label->setMaxLines(3);
        text->addChild(std::move(label));
      }
      auto out = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      out->addChild(std::move(text));
      return out;
    }

    std::unique_ptr<Button> button(
        const std::string& text, const char* glyph, ButtonVariant variant, bool enabled, std::function<void()> onClick
    ) {
      Button* raw = nullptr;
      auto out = ui::button({
          .out = &raw,
          .text = text,
          .glyph = std::string(glyph),
          .variant = variant,
          .onClick = std::move(onClick),
      });
      raw->setEnabled(enabled);
      return out;
    }

    void addInstalledCard(Flex& content, float scale, const std::vector<okaru::hyprpm::Repo>& repos) {
      auto& s = state();
      Flex* body = addSettingsCard(content, tr("installed"), scale);
      if (repos.empty()) {
        body->addChild(
            makeLabel(tr("none"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
        );
      }
      for (const auto& repo : repos) {
        const std::string hash = repo.hash.substr(0, 7);
        auto header = row(scale, repo.name, std::format("{} · {} · {}", repo.author, hash, repo.url), false);
        const bool confirming = s.confirmRemove == repo.name;
        header->addChild(button(
            confirming ? tr("remove-confirm") : tr("remove"), "trash",
            confirming ? ButtonVariant::Destructive : ButtonVariant::Outline, !s.running,
            [name = repo.name, confirming] {
              if (!confirming) {
                state().confirmRemove = name; // doble confirmación: el segundo clic quita
                rebuild();
                return;
              }
              run(Action::Remove, name, i18n::tr("settings.okaru.hyprpm.running-remove", "repo", name),
                  i18n::tr("settings.okaru.hyprpm.auth-remove", "repo", name));
            }
        ));
        body->addChild(std::move(header));

        for (const auto& plugin : repo.plugins) {
          auto line = row(scale, "    " + plugin.name, plugin.failed ? tr("failed") : std::string(), plugin.failed);
          line->addChild(
              ui::toggle({
                  .checked = plugin.enabled,
                  .enabled = !s.running,
                  .scale = scale,
                  .onChange = [name = plugin.name](bool on) {
                    run(on ? Action::Enable : Action::Disable, name,
                        i18n::tr(
                            on ? "settings.okaru.hyprpm.running-enable" : "settings.okaru.hyprpm.running-disable",
                            "plugin", name
                        ),
                        i18n::tr(
                            on ? "settings.okaru.hyprpm.auth-enable" : "settings.okaru.hyprpm.auth-disable", "plugin",
                            name
                        ));
                  },
              })
          );
          body->addChild(std::move(line));
        }
      }
      if (!repos.empty()) {
        auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .fillWidth = true});
        actions->addChild(button(tr("update"), "refresh", ButtonVariant::Default, !s.running, [] {
          run(Action::Update, {}, tr("running-update"), tr("auth-update"));
        }));
        body->addChild(std::move(actions));
      }
    }

    void addAddCard(Flex& content, float scale, const std::vector<okaru::hyprpm::Repo>& repos) {
      auto& s = state();
      Flex* body = addSettingsCard(content, tr("add"), scale);

      auto urlRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      Input* input = nullptr;
      urlRow->addChild(
          ui::input({
              .out = &input,
              .value = s.urlDraft,
              .placeholder = tr("url-placeholder"),
              .fontSize = Style::fontSizeCaption * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .flexGrow = 1.0F,
          })
      );
      input->setOnChange([](const std::string& v) { state().urlDraft = v; });
      urlRow->addChild(button(tr("add-button"), "plus", ButtonVariant::Primary, !s.running, [] {
        const std::string url = state().urlDraft;
        run(Action::Add, url, i18n::tr("settings.okaru.hyprpm.running-add", "repo", url),
            i18n::tr("settings.okaru.hyprpm.auth-add", "repo", url));
      }));
      body->addChild(std::move(urlRow));

      body->addChild(
          makeLabel(tr("suggestions"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
      );
      for (const auto& suggestion : okaru::hyprpm::suggestions()) {
        auto line =
            row(scale, std::format("{} ({})", suggestion.name, suggestion.author),
                tr(std::string(suggestion.descriptionKey)), false);
        if (okaru::hyprpm::installed(repos, suggestion.url)) {
          line->addChild(
              makeLabel(tr("already-installed"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Primary))
          );
        } else {
          line->addChild(button(
              tr("add-button"), "plus", ButtonVariant::Outline, !s.running,
              [url = std::string(suggestion.url), name = std::string(suggestion.name)] {
                run(Action::Add, url, i18n::tr("settings.okaru.hyprpm.running-add", "repo", name),
                    i18n::tr("settings.okaru.hyprpm.auth-add", "repo", name));
              }
          ));
        }
        body->addChild(std::move(line));
      }
    }

    void addLogCard(Flex& content, float scale) {
      const auto& s = state();
      if (!s.running && s.log.empty()) {
        return;
      }
      const std::string title = s.running ? s.runningLabel : (s.failed ? tr("finished-error") : tr("finished-ok"));
      Flex* body = addSettingsCard(content, title, scale);
      auto log = makeLabel(
          okaru::hyprpm::cleanOutput(s.log), Style::fontSizeCaption * scale,
          colorSpecFromRole(s.failed && !s.running ? ColorRole::Error : ColorRole::OnSurfaceVariant)
      );
      log->setMaxLines(24);
      body->addChild(std::move(log));
    }

  } // namespace

  void addSettingsHyprpm(Flex& content, SettingsHyprpmContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    if (!process::commandExists("hyprpm")) {
      Flex* body = addSettingsCard(content, tr("title"), scale);
      body->addChild(
          makeLabel(tr("missing"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
      );
      return;
    }

    auto intro = makeLabel(tr("intro"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
    intro->setMaxLines(3);
    content.addChild(std::move(intro));

    const auto repos = okaru::hyprpm::readRepos();
    addLogCard(content, scale);
    addInstalledCard(content, scale, repos);
    addAddCard(content, scale, repos);
  }

} // namespace settings
