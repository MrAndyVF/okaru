#pragma once

#include <cstddef>
#include <cstdint>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>

// libjxl is loaded with dlopen on first use instead of being linked: its soname changes with every
// minor release (libjxl.so.0.12 → 0.13), and a linked build stops starting the moment the system
// updates it. The unversioned libjxl.so accepts any installed release from kJxlMinVersion on; when
// it is missing or too old, JPEG XL images fail to decode/encode and everything else keeps working.
// The headers above only provide types and enums; no libjxl symbol is linked.
struct JxlApi {
  decltype(&JxlDecoderCreate) decoderCreate = nullptr;
  decltype(&JxlDecoderDestroy) decoderDestroy = nullptr;
  decltype(&JxlDecoderSubscribeEvents) decoderSubscribeEvents = nullptr;
  decltype(&JxlDecoderSetParallelRunner) decoderSetParallelRunner = nullptr;
  decltype(&JxlDecoderSetUnpremultiplyAlpha) decoderSetUnpremultiplyAlpha = nullptr;
  decltype(&JxlDecoderSetInput) decoderSetInput = nullptr;
  decltype(&JxlDecoderCloseInput) decoderCloseInput = nullptr;
  decltype(&JxlDecoderProcessInput) decoderProcessInput = nullptr;
  decltype(&JxlDecoderGetBasicInfo) decoderGetBasicInfo = nullptr;
  decltype(&JxlDecoderImageOutBufferSize) decoderImageOutBufferSize = nullptr;
  decltype(&JxlDecoderSetImageOutBuffer) decoderSetImageOutBuffer = nullptr;

  decltype(&JxlEncoderCreate) encoderCreate = nullptr;
  decltype(&JxlEncoderDestroy) encoderDestroy = nullptr;
  decltype(&JxlEncoderSetParallelRunner) encoderSetParallelRunner = nullptr;
  decltype(&JxlEncoderInitBasicInfo) encoderInitBasicInfo = nullptr;
  decltype(&JxlEncoderSetBasicInfo) encoderSetBasicInfo = nullptr;
  decltype(&JxlColorEncodingSetToSRGB) colorEncodingSetToSRGB = nullptr;
  decltype(&JxlEncoderSetColorEncoding) encoderSetColorEncoding = nullptr;
  decltype(&JxlEncoderFrameSettingsCreate) encoderFrameSettingsCreate = nullptr;
  decltype(&JxlEncoderSetFrameLossless) encoderSetFrameLossless = nullptr;
  decltype(&JxlEncoderSetFrameDistance) encoderSetFrameDistance = nullptr;
  decltype(&JxlEncoderAddImageFrame) encoderAddImageFrame = nullptr;
  decltype(&JxlEncoderCloseInput) encoderCloseInput = nullptr;
  decltype(&JxlEncoderProcessOutput) encoderProcessOutput = nullptr;

  // From libjxl_threads.so.
  decltype(&JxlResizableParallelRunner) resizableParallelRunner = nullptr;
  decltype(&JxlResizableParallelRunnerCreate) resizableParallelRunnerCreate = nullptr;
  decltype(&JxlResizableParallelRunnerDestroy) resizableParallelRunnerDestroy = nullptr;
  decltype(&JxlResizableParallelRunnerSetThreads) resizableParallelRunnerSetThreads = nullptr;
  decltype(&JxlResizableParallelRunnerSuggestThreads) resizableParallelRunnerSuggestThreads = nullptr;
};

// Oldest libjxl whose API matches the calls above (JxlDecoderSetUnpremultiplyAlpha appeared in 0.7).
inline constexpr std::uint32_t kJxlMinVersion = 7000; // major * 1000000 + minor * 1000 + patch

// The loaded API, or nullptr when libjxl is unavailable. Loaded once and never unloaded.
[[nodiscard]] const JxlApi* jxlApi();

// JPEG XL signature (bare codestream or ISOBMFF container), checked without loading libjxl.
[[nodiscard]] bool isJxlSignature(const std::uint8_t* data, std::size_t size);
