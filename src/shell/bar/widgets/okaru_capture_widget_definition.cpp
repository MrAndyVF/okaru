#include "shell/bar/widgets/okaru_capture_widget_definition.h"

const okaru::bar::WidgetDefinition<OkaruCaptureWidget::Options>& okaruCaptureWidgetDefinition() {
  using Options = OkaruCaptureWidget::Options;
  using okaru::bar::field;

  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "capture",
      .fields =
          {
              field<&Options::source>({.key = "source"}),
              field<&Options::resolution>({.key = "resolution"}),
              field<&Options::frameRate>({.key = "frame_rate", .minValue = 1, .maxValue = 240}),
              field<&Options::format>({.key = "format"}),
              field<&Options::quality>({.key = "quality"}),
              field<&Options::audio>({.key = "audio"}),
              field<&Options::videoCodec>({.key = "video_codec"}),
              field<&Options::directory>({.key = "directory"}),
              field<&Options::filenamePattern>({.key = "filename_pattern"}),
              field<&Options::showCursor>({.key = "show_cursor"}),
              field<&Options::copyToClipboard>({.key = "copy_to_clipboard"}),
              field<&Options::replayDuration>({.key = "replay_duration", .minValue = 5, .maxValue = 1800}),
          },
      .glyph = [](const Options&) { return std::string("capture"); },
  };
  return definition;
}
