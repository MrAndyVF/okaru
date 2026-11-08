#include "shell/okaru_capture/okaru_capture_panel.h"

#include "capture/okaru_recorder.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <format>

using okaru::capture::Recorder;
using okaru::capture::RecorderOptions;
using okaru::capture::RecorderState;

namespace {

  constexpr const char* kPanelId = "okaru-capture";

  std::string tr(const std::string& key) { return i18n::tr("okaru.capture." + key); }

  std::unique_ptr<Flex> labeledRow(float scale, const std::string& label, std::unique_ptr<Node> control) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
    row->addChild(
        ui::column(
            {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
            settings::makeLabel(label, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface))
        )
    );
    row->addChild(std::move(control));
    return row;
  }

} // namespace

OkaruCapturePanel::OkaruCapturePanel(
    ConfigService* config, WaylandConnection* wayland, std::function<void(const std::string&)> runCommand
)
    : m_config(config), m_wayland(wayland), m_runCommand(std::move(runCommand)) {}

OkaruCapturePanel::~OkaruCapturePanel() {
  if (m_listener != 0) {
    Recorder::instance().removeListener(m_listener);
  }
}

float OkaruCapturePanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  if (m_dirty) {
    rebuild();
  }
  m_rootLayout->setSize(contentWidth, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(contentWidth);
  return m_rootLayout->measure(renderer, constraints).height;
}

void OkaruCapturePanel::create() {
  const float scale = contentScale();
  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
  });
  root->addChild(
      ui::segmented({
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = tr("tab-capture"), .glyph = "camera", .tooltip = {}},
                  {.label = tr("tab-record"), .glyph = "video", .tooltip = {}},
              },
          .selectedIndex = m_tab,
          .scale = scale,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) {
            m_tab = index;
            markDirty();
          },
      })
  );
  root->addChild(ui::column({.out = &m_body, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale}));
  setRoot(std::move(root));
  m_dirty = true;
}

void OkaruCapturePanel::onOpen(std::string_view /*context*/) {
  // Si está grabando, se abre directo en Grabación.
  if (Recorder::instance().state() != RecorderState::Idle) {
    m_tab = 1;
  }
  m_listener = Recorder::instance().addListener([this] { markDirty(); });
  m_tick.startRepeating(std::chrono::seconds(1), [this] {
    if (Recorder::instance().state() != RecorderState::Idle) {
      markDirty(); // el tiempo transcurrido
    }
  });
  m_dirty = true;
}

void OkaruCapturePanel::onClose() {
  m_tick.stop();
  if (m_listener != 0) {
    Recorder::instance().removeListener(m_listener);
    m_listener = 0;
  }
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_body = nullptr;
}

void OkaruCapturePanel::markDirty() {
  m_dirty = true;
  if (auto* manager = PanelManager::current(); manager != nullptr && manager->isOpenPanel(kPanelId)) {
    manager->refresh();
  }
}

void OkaruCapturePanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
}

void OkaruCapturePanel::doUpdate(Renderer& /*renderer*/) {
  if (m_dirty) {
    rebuild();
  }
}

void OkaruCapturePanel::rebuild() {
  if (m_body == nullptr) {
    return;
  }
  m_dirty = false;
  while (!m_body->children().empty()) {
    (void)m_body->removeChild(m_body->children().back().get());
  }
  const float scale = contentScale();
  if (m_tab == 0) {
    addCaptureTab(*m_body, scale);
  } else {
    addRecordTab(*m_body, scale);
  }
}

void OkaruCapturePanel::runAfterClose(std::string command) {
  // El menú se cierra antes de capturar para no salir en la captura.
  PanelManager::instance().close();
  m_deferred.start(std::chrono::milliseconds(250), [this, command = std::move(command)] {
    if (m_runCommand) {
      m_runCommand(command);
    }
  });
}

void OkaruCapturePanel::setOption(const std::string& key, const std::string& value) {
  if (m_config != nullptr) {
    (void)m_config->setOverride({"widget", "capture", key}, value);
  }
  markDirty();
}

void OkaruCapturePanel::setOption(const std::string& key, std::int64_t value) {
  if (m_config != nullptr) {
    (void)m_config->setOverride({"widget", "capture", key}, value);
  }
  markDirty();
}

void OkaruCapturePanel::addCaptureTab(Flex& body, float scale) {
  struct Action {
    const char* key;
    const char* glyph;
    const char* command;
  };
  static constexpr Action kActions[] = {
      {"screen", "device-desktop", "screenshot-fullscreen"},
      {"region", "crop", "screenshot-region"},
      {"window", "app-window", "screenshot-window"},
  };
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  for (const auto& action : kActions) {
    row->addChild(
        ui::button({
            .text = tr(action.key),
            .glyph = std::string(action.glyph),
            .variant = ButtonVariant::Default,
            .flexGrow = 1.0F,
            .onClick = [this, command = std::string(action.command)] { runAfterClose(command); },
        })
    );
  }
  body.addChild(std::move(row));
}

void OkaruCapturePanel::addRecordTab(Flex& body, float scale) {
  auto& recorder = Recorder::instance();
  if (!recorder.available()) {
    body.addChild(settings::makeLabel(tr("missing"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error)));
    return;
  }
  const WidgetConfig* widget = nullptr;
  if (m_config != nullptr) {
    const auto& widgets = m_config->config().widgets;
    if (const auto it = widgets.find("capture"); it != widgets.end()) {
      widget = &it->second;
    }
  }
  const RecorderOptions options = RecorderOptions::fromConfig(widget);
  const RecorderState state = recorder.state();

  // Acción principal.
  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  if (state == RecorderState::Idle) {
    actions->addChild(
        ui::button({
            .text = tr("record"),
            .glyph = "player-record",
            .variant = ButtonVariant::Primary,
            .flexGrow = 1.0F,
            .onClick = [this, options] {
              PanelManager::instance().close(); // que el menú no salga en la grabación
              Recorder::instance().startRecording(options);
            },
        })
    );
    actions->addChild(
        ui::button({
            .text = tr("replay-start"),
            .glyph = "history",
            .variant = ButtonVariant::Default,
            .onClick = [options] { Recorder::instance().startReplay(options); },
        })
    );
  } else {
    const auto secs = recorder.elapsed().count();
    actions->addChild(
        ui::button({
            .text = state == RecorderState::Recording ? std::format("{}  {:02}:{:02}", tr("stop"), secs / 60, secs % 60)
                                                      : tr("replay-stop"),
            .glyph = "player-stop",
            .variant = ButtonVariant::Destructive,
            .flexGrow = 1.0F,
            .onClick = [] { Recorder::instance().stop(); },
        })
    );
    if (state == RecorderState::Recording) {
      actions->addChild(
          ui::button({
              .text = tr(recorder.paused() ? "resume" : "pause"),
              .glyph = recorder.paused() ? "player-play" : "player-pause",
              .variant = ButtonVariant::Default,
              .onClick = [] { Recorder::instance().togglePause(); },
          })
      );
    }
    if (state == RecorderState::Replay) {
      actions->addChild(
          ui::button({
              .text = tr("replay-save"),
              .glyph = "device-floppy",
              .variant = ButtonVariant::Primary,
              .onClick = [] { Recorder::instance().saveReplay(); },
          })
      );
    }
  }
  body.addChild(std::move(actions));

  // Opciones rápidas (se guardan en [widget.capture]); no se cambian mientras graba.
  const bool idle = state == RecorderState::Idle;
  const auto select = [&](const std::string& labelKey, const std::vector<std::string>& values,
                          const std::vector<std::string>& labels, const std::string& current,
                          std::function<void(const std::string&)> apply) {
    const auto it = std::ranges::find(values, current);
    body.addChild(labeledRow(
        scale, tr(labelKey),
        ui::select({
            .options = labels,
            .selectedIndex = it == values.end() ? 0 : static_cast<std::size_t>(it - values.begin()),
            .fontSize = Style::fontSizeCaption * scale,
            .controlHeight = Style::controlHeightSm * scale,
            .enabled = idle,
            .width = 190.0F * scale,
            .onSelectionChanged = [values, apply](std::size_t index, std::string_view) { apply(values[index]); },
        })
    ));
  };

  std::vector<std::string> sources{"portal", "region"};
  std::vector<std::string> sourceLabels{tr("source-portal"), tr("source-region")};
  if (m_wayland != nullptr) {
    for (const auto& output : m_wayland->outputs()) {
      if (!output.connectorName.empty()) {
        sources.push_back(output.connectorName);
        sourceLabels.push_back(output.connectorName);
      }
    }
  }
  select("source", sources, sourceLabels, options.source, [this](const std::string& v) { setOption("source", v); });

  std::vector<std::string> resLabels;
  for (const auto& r : okaru::capture::resolutionChoices()) {
    resLabels.push_back(r == "original" ? tr("resolution-original") : r);
  }
  select(
      "resolution", okaru::capture::resolutionChoices(), resLabels, options.resolution,
      [this](const std::string& v) { setOption("resolution", v); }
  );

  std::vector<std::string> fps;
  for (const int f : okaru::capture::frameRateChoices()) {
    fps.push_back(std::to_string(f));
  }
  select("fps", fps, fps, std::to_string(options.frameRate), [this](const std::string& v) {
    setOption("frame_rate", static_cast<std::int64_t>(std::stoi(v)));
  });

  select(
      "format", okaru::capture::formatChoices(), okaru::capture::formatChoices(), options.format,
      [this](const std::string& v) { setOption("format", v); }
  );

  std::vector<std::string> qualityLabels;
  for (const auto& q : okaru::capture::qualityChoices()) {
    qualityLabels.push_back(tr("quality-" + q));
  }
  select("quality", okaru::capture::qualityChoices(), qualityLabels, options.quality, [this](const std::string& v) {
    setOption("quality", v);
  });

  std::vector<std::string> audioLabels;
  for (const auto& a : okaru::capture::audioChoices()) {
    audioLabels.push_back(tr("audio-" + a));
  }
  select("audio", okaru::capture::audioChoices(), audioLabels, options.audio, [this](const std::string& v) {
    setOption("audio", v);
  });

  // Carpeta de las grabaciones: Enter la guarda (se crea al grabar si no existe).
  body.addChild(labeledRow(
      scale, tr("directory"),
      ui::input({
          .value = options.directory,
          .placeholder = "~/Videos/Recordings",
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .enabled = idle,
          .width = 230.0F * scale,
          .onSubmit = [this](const std::string& value) {
            if (!value.empty() && value.find('\n') == std::string::npos) {
              setOption("directory", value);
            }
          },
      })
  ));
  body.addChild(
      settings::makeLabel(
          tr("directory-hint"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      )
  );
}
