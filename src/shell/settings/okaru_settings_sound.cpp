#include "shell/settings/okaru_settings_sound.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/okaru_audio/okaru_audio_device_row.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace settings {
  namespace {

    constexpr std::size_t kMaxAliasBytes = 64;

    struct SoundState {
      std::function<void()> rebuild;
      std::string editing; // node.name del dispositivo que se está renombrando
      std::string draft;
      std::string devicesKey;                // ids visibles de la última construcción
      std::vector<OkaruAudioDeviceRow*> rows; // filas de la última construcción, salidas y luego entradas
    };

    SoundState& state() {
      static SoundState s;
      return s;
    }

    std::string tr(const std::string& key) { return i18n::tr("settings.okaru.sound." + key); }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    std::vector<std::string> aliasPath(const std::string& nodeName) { return {"audio", "device_names", nodeName}; }

    // Como el panel: oculta las rutas sin conectar, pero nunca el predeterminado.
    std::vector<AudioNode> visibleDevices(const std::vector<AudioNode>& devices, std::uint32_t defaultId) {
      std::vector<AudioNode> out;
      for (const auto& device : devices) {
        if (device.available || device.id == defaultId) {
          out.push_back(device);
        }
      }
      std::ranges::sort(out, [](const AudioNode& a, const AudioNode& b) {
        return audioDeviceOriginalLabel(a) < audioDeviceOriginalLabel(b);
      });
      return out;
    }

    std::unique_ptr<Button>
    button(float scale, std::string text, const char* glyph, ButtonVariant variant, std::function<void()> onClick) {
      return ui::button({
          .text = std::move(text),
          .glyph = std::string(glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .variant = variant,
          .minHeight = Style::controlHeightSm * scale,
          .onClick = std::move(onClick),
      });
    }

    std::string devicesKey(const std::vector<AudioNode>& outputs, const std::vector<AudioNode>& inputs) {
      std::string key;
      for (const auto* list : {&outputs, &inputs}) {
        for (const auto& node : *list) {
          key += std::to_string(node.id) + ',';
        }
        key += ';';
      }
      return key;
    }

    void saveAlias(ConfigService& config, const AudioNode& node) {
      const std::string alias = normalizeAudioDeviceAlias(state().draft);
      if (alias.empty() || alias == audioDeviceOriginalLabel(node)) {
        (void)config.clearOverride(aliasPath(node.name));
      } else {
        (void)config.setOverride(aliasPath(node.name), alias);
      }
      state().editing.clear();
      rebuild();
    }

    void addDeviceCard(
        Flex& content, float scale, PipeWireService& audio, ConfigService& config, bool input,
        const std::vector<AudioNode>& devices
    ) {
      Flex* body = addSettingsCard(content, tr(input ? "inputs" : "outputs"), scale);
      if (devices.empty()) {
        body->addChild(makeLabel(
            i18n::tr(input ? "control-center.audio.no-input-devices" : "control-center.audio.no-output-devices"),
            Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
        ));
        return;
      }
      const auto& aliases = config.config().audio.deviceNames;
      const float sliderMax = maxAudioVolume(config.config().audio);
      auto rows = std::make_shared<std::vector<OkaruAudioDeviceRow*>>();

      for (const auto& node : devices) {
        const bool hasAlias = aliases.contains(node.name);
        auto trailing = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        auto rename = button(scale, {}, "pencil", ButtonVariant::Ghost, [node] {
          state().editing = node.name;
          state().draft = audioDeviceLabel(node);
          rebuild();
        });
        rename->setTooltip(tr("rename"));
        trailing->addChild(std::move(rename));
        if (hasAlias) {
          auto reset = button(scale, {}, "restore", ButtonVariant::Ghost, [&config, name = node.name] {
            (void)config.clearOverride(aliasPath(name));
            rebuild();
          });
          reset->setTooltip(tr("reset-name"));
          trailing->addChild(std::move(reset));
        }

        auto row = std::make_unique<OkaruAudioDeviceRow>(&audio, node, input, scale, sliderMax, std::move(trailing));
        row->setOnDefaultChosen([rows](std::uint32_t id) {
          for (auto* other : *rows) {
            other->setDefaultMark(other->deviceId() == id);
          }
        });
        rows->push_back(row.get());
        state().rows.push_back(row.get());
        body->addChild(std::move(row));

        if (state().editing == node.name) {
          auto edit = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
          edit->addChild(ui::input({
              .value = state().draft,
              .placeholder = audioDeviceOriginalLabel(node),
              .fontSize = Style::fontSizeCaption * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .flexGrow = 1.0F,
              .onChange = [](const std::string& value) { state().draft = value; },
              .onSubmit = [&config, node](const std::string& value) {
                state().draft = value;
                saveAlias(config, node);
              },
          }));
          edit->addChild(button(scale, tr("cancel"), "circle-x", ButtonVariant::Ghost, [] {
            state().editing.clear();
            rebuild();
          }));
          edit->addChild(button(scale, tr("save"), "device-floppy", ButtonVariant::Primary, [&config, node] {
            saveAlias(config, node);
          }));
          body->addChild(std::move(edit));
        }
      }
    }

  } // namespace

  std::string normalizeAudioDeviceAlias(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return {};
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.size() > kMaxAliasBytes) {
      std::size_t cut = kMaxAliasBytes;
      while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U) {
        --cut; // no partir un carácter UTF-8
      }
      value.resize(cut);
    }
    return value;
  }

  void addSettingsSound(Flex& content, SettingsSoundContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;
    if (ctx.audio == nullptr || ctx.config == nullptr) {
      Flex* body = addSettingsCard(content, tr("outputs"), scale);
      body->addChild(makeLabel(
          i18n::tr("control-center.audio.unavailable-body"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }
    const AudioState& audio = ctx.audio->state();
    const auto outputs = visibleDevices(audio.sinks, audio.defaultSinkId);
    const auto inputs = visibleDevices(audio.sources, audio.defaultSourceId);
    s.devicesKey = devicesKey(outputs, inputs);
    s.rows.clear();
    addDeviceCard(content, scale, *ctx.audio, *ctx.config, false, outputs);
    addDeviceCard(content, scale, *ctx.audio, *ctx.config, true, inputs);
  }

  bool syncSettingsSound(PipeWireService& audio, ConfigService& config) {
    auto& s = state();
    const AudioState& current = audio.state();
    const auto outputs = visibleDevices(current.sinks, current.defaultSinkId);
    const auto inputs = visibleDevices(current.sources, current.defaultSourceId);
    if (devicesKey(outputs, inputs) != s.devicesKey) {
      rebuild();
      return true;
    }
    const float sliderMax = maxAudioVolume(config.config().audio);
    std::size_t i = 0;
    for (const auto* list : {&outputs, &inputs}) {
      for (const auto& node : *list) {
        if (i < s.rows.size() && !s.rows[i]->dragging()) {
          s.rows[i]->sync(node, sliderMax);
        }
        ++i;
      }
    }
    return true;
  }

} // namespace settings
