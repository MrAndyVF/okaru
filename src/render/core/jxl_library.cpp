#include "render/core/jxl_library.h"

#include "core/log.h"

#include <array>
#include <cstring>
#include <dlfcn.h>
#include <mutex>

namespace {

  constexpr Logger kLog("jxl");

  template <typename T> bool loadSymbol(void* library, const char* name, T& out) {
    void* symbol = dlsym(library, name);
    if (symbol == nullptr) {
      return false;
    }
    std::memcpy(&out, &symbol, sizeof(out));
    return true;
  }

  bool loadAll(void* jxl, void* threads, JxlApi& api) {
    return loadSymbol(jxl, "JxlDecoderCreate", api.decoderCreate)
        && loadSymbol(jxl, "JxlDecoderDestroy", api.decoderDestroy)
        && loadSymbol(jxl, "JxlDecoderSubscribeEvents", api.decoderSubscribeEvents)
        && loadSymbol(jxl, "JxlDecoderSetParallelRunner", api.decoderSetParallelRunner)
        && loadSymbol(jxl, "JxlDecoderSetUnpremultiplyAlpha", api.decoderSetUnpremultiplyAlpha)
        && loadSymbol(jxl, "JxlDecoderSetInput", api.decoderSetInput)
        && loadSymbol(jxl, "JxlDecoderCloseInput", api.decoderCloseInput)
        && loadSymbol(jxl, "JxlDecoderProcessInput", api.decoderProcessInput)
        && loadSymbol(jxl, "JxlDecoderGetBasicInfo", api.decoderGetBasicInfo)
        && loadSymbol(jxl, "JxlDecoderImageOutBufferSize", api.decoderImageOutBufferSize)
        && loadSymbol(jxl, "JxlDecoderSetImageOutBuffer", api.decoderSetImageOutBuffer)
        && loadSymbol(jxl, "JxlEncoderCreate", api.encoderCreate)
        && loadSymbol(jxl, "JxlEncoderDestroy", api.encoderDestroy)
        && loadSymbol(jxl, "JxlEncoderSetParallelRunner", api.encoderSetParallelRunner)
        && loadSymbol(jxl, "JxlEncoderInitBasicInfo", api.encoderInitBasicInfo)
        && loadSymbol(jxl, "JxlEncoderSetBasicInfo", api.encoderSetBasicInfo)
        && loadSymbol(jxl, "JxlColorEncodingSetToSRGB", api.colorEncodingSetToSRGB)
        && loadSymbol(jxl, "JxlEncoderSetColorEncoding", api.encoderSetColorEncoding)
        && loadSymbol(jxl, "JxlEncoderFrameSettingsCreate", api.encoderFrameSettingsCreate)
        && loadSymbol(jxl, "JxlEncoderSetFrameLossless", api.encoderSetFrameLossless)
        && loadSymbol(jxl, "JxlEncoderSetFrameDistance", api.encoderSetFrameDistance)
        && loadSymbol(jxl, "JxlEncoderAddImageFrame", api.encoderAddImageFrame)
        && loadSymbol(jxl, "JxlEncoderCloseInput", api.encoderCloseInput)
        && loadSymbol(jxl, "JxlEncoderProcessOutput", api.encoderProcessOutput)
        && loadSymbol(threads, "JxlResizableParallelRunner", api.resizableParallelRunner)
        && loadSymbol(threads, "JxlResizableParallelRunnerCreate", api.resizableParallelRunnerCreate)
        && loadSymbol(threads, "JxlResizableParallelRunnerDestroy", api.resizableParallelRunnerDestroy)
        && loadSymbol(threads, "JxlResizableParallelRunnerSetThreads", api.resizableParallelRunnerSetThreads)
        && loadSymbol(threads, "JxlResizableParallelRunnerSuggestThreads", api.resizableParallelRunnerSuggestThreads);
  }

} // namespace

const JxlApi* jxlApi() {
  static std::once_flag once;
  static JxlApi api;
  static bool available = false;

  std::call_once(once, [] {
    void* jxl = dlopen("libjxl.so", RTLD_LAZY | RTLD_LOCAL);
    void* threads = jxl != nullptr ? dlopen("libjxl_threads.so", RTLD_LAZY | RTLD_LOCAL) : nullptr;
    if (threads == nullptr) {
      kLog.info("libjxl.so not available, JPEG XL images will not decode or encode");
      if (jxl != nullptr) {
        dlclose(jxl);
      }
      return;
    }

    decltype(&JxlDecoderVersion) version = nullptr;
    if (!loadSymbol(jxl, "JxlDecoderVersion", version) || version() < kJxlMinVersion || !loadAll(jxl, threads, api)) {
      kLog.info("libjxl.so is older than 0.7 or lacks required symbols, JPEG XL images are disabled");
      api = {};
      dlclose(threads);
      dlclose(jxl);
      return;
    }
    (void)dlerror();

    available = true;
    kLog.debug("loaded libjxl {}.{}.{}", version() / 1000000, version() / 1000 % 1000, version() % 1000);
  });

  return available ? &api : nullptr;
}

bool isJxlSignature(const std::uint8_t* data, std::size_t size) {
  static constexpr std::array<std::uint8_t, 12> kContainer = {
      0x00, 0x00, 0x00, 0x0C, 0x4A, 0x58, 0x4C, 0x20, 0x0D, 0x0A, 0x87, 0x0A,
  };
  if (data == nullptr) {
    return false;
  }
  return (size >= 2 && data[0] == 0xFF && data[1] == 0x0A)
      || (size >= kContainer.size() && std::memcmp(data, kContainer.data(), kContainer.size()) == 0);
}
