#include "apps/viewer/viewer_image.h"

#include "render/core/image_decoder.h"
#include "render/core/image_file_loader.h"

#include <algorithm>
#include <array>
#include <exiv2/exiv2.hpp>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <libraw/libraw.h>
#include <stb/stb_image_resize2.h>
#include <webp/demux.h>

namespace okaru::viewer {

  namespace {

    namespace fs = std::filesystem;

    // Una animación enorme se recorta en vez de agotar la memoria (como el sticker de escritorio, pero más holgado).
    constexpr int kMaxFrames = 2000;
    constexpr std::size_t kMaxAnimationBytes = std::size_t{1536} << 20U;

    constexpr std::array<std::string_view, 28> kRasterExtensions{
        "png", "jpg", "jpeg", "jpe", "jfif", "gif", "webp", "avif", "heic", "heif", "jxl", "bmp",  "dib", "ico",
        "cur", "tga", "qoi",  "pbm", "pgm",  "ppm", "pnm",  "pam",  "svg",  "svgz", "tif", "tiff", "xpm", "icns",
    };
    constexpr std::array<std::string_view, 26> kRawExtensions{
        "cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "dng", "raf", "orf", "rw2", "pef",
        "srw", "x3f", "erf", "kdc", "dcr", "mrw", "3fr", "mef", "mos", "raw", "rwl", "iiq", "gpr",
    };

    std::string extension(const fs::path& path) {
      std::string ext = path.extension().string();
      if (!ext.empty()) {
        ext.erase(0, 1);
      }
      std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return ext;
    }

    bool isRaw(const fs::path& path) { return std::ranges::contains(kRawExtensions, extension(path)); }

    std::vector<std::uint8_t> readBytes(const fs::path& path) {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    DecodedImage still(std::vector<std::uint8_t> rgba, int width, int height) {
      DecodedImage image{.width = width, .height = height, .sourceWidth = width, .sourceHeight = height};
      image.frames.push_back({.rgba = std::move(rgba)});
      return image;
    }

    std::expected<DecodedImage, std::string> decodeAnimatedWebP(const std::vector<std::uint8_t>& bytes) {
      WebPAnimDecoderOptions options;
      if (!WebPAnimDecoderOptionsInit(&options)) {
        return std::unexpected("libwebp");
      }
      options.color_mode = MODE_RGBA;
      const WebPData data{.bytes = bytes.data(), .size = bytes.size()};
      WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
      if (decoder == nullptr) {
        return std::unexpected("libwebp: not an animation");
      }
      WebPAnimInfo info;
      DecodedImage image;
      if (WebPAnimDecoderGetInfo(decoder, &info)) {
        image.width = image.sourceWidth = static_cast<int>(info.canvas_width);
        image.height = image.sourceHeight = static_cast<int>(info.canvas_height);
        const std::size_t frameBytes = std::size_t{info.canvas_width} * info.canvas_height * 4U;
        int previous = 0;
        std::uint8_t* pixels = nullptr;
        int timestamp = 0;
        while (WebPAnimDecoderHasMoreFrames(decoder) && WebPAnimDecoderGetNext(decoder, &pixels, &timestamp)) {
          if (image.frames.size() >= kMaxFrames || (image.frames.size() + 1) * frameBytes > kMaxAnimationBytes) {
            image.truncated = true;
            break;
          }
          image.frames.push_back(
              {.rgba = {pixels, pixels + frameBytes},
               .durationMs = static_cast<std::uint32_t>(std::max(20, timestamp - previous))}
          );
          previous = timestamp;
        }
      }
      WebPAnimDecoderDelete(decoder);
      if (image.frames.empty()) {
        return std::unexpected("libwebp: no frames");
      }
      return image;
    }

    std::expected<DecodedImage, std::string> decodeWithPixbuf(const fs::path& path) {
      GError* error = nullptr;
      GdkPixbuf* loaded = gdk_pixbuf_new_from_file(path.c_str(), &error);
      if (loaded == nullptr) {
        std::string message = error != nullptr ? error->message : "gdk-pixbuf";
        g_clear_error(&error);
        return std::unexpected(std::move(message));
      }
      GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(loaded);
      g_object_unref(loaded);
      GdkPixbuf* rgba = gdk_pixbuf_add_alpha(oriented, FALSE, 0, 0, 0);
      g_object_unref(oriented);
      const int width = gdk_pixbuf_get_width(rgba);
      const int height = gdk_pixbuf_get_height(rgba);
      const int stride = gdk_pixbuf_get_rowstride(rgba);
      const guchar* pixels = gdk_pixbuf_read_pixels(rgba);
      std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4U);
      for (int y = 0; y < height; ++y) {
        std::copy_n(
            pixels + static_cast<std::ptrdiff_t>(y) * stride, width * 4, out.data() + std::size_t{4U} * width * y
        );
      }
      g_object_unref(rgba);
      return still(std::move(out), width, height);
    }

    std::expected<DecodedImage, std::string> decodeRaw(const fs::path& path) {
      libraw_data_t* raw = libraw_init(0);
      if (raw == nullptr) {
        return std::unexpected("libraw");
      }
      raw->params.use_camera_wb = 1;
      raw->params.output_bps = 8;
      int status = libraw_open_file(raw, path.c_str());
      if (status == LIBRAW_SUCCESS) {
        status = libraw_unpack(raw);
      }
      if (status == LIBRAW_SUCCESS) {
        status = libraw_dcraw_process(raw);
      }
      libraw_processed_image_t* processed =
          status == LIBRAW_SUCCESS ? libraw_dcraw_make_mem_image(raw, &status) : nullptr;
      if (processed == nullptr
          || processed->type != LIBRAW_IMAGE_BITMAP
          || processed->colors != 3
          || processed->bits != 8) {
        const std::string message = libraw_strerror(status);
        if (processed != nullptr) {
          libraw_dcraw_clear_mem(processed);
        }
        libraw_close(raw);
        return std::unexpected(message);
      }
      const int width = processed->width;
      const int height = processed->height;
      std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4U);
      for (std::size_t i = 0, pixels = std::size_t{1U} * width * height; i < pixels; ++i) {
        std::copy_n(processed->data + i * 3U, 3, out.data() + i * 4U);
        out[i * 4U + 3U] = 0xFF;
      }
      libraw_dcraw_clear_mem(processed);
      libraw_close(raw);
      return still(std::move(out), width, height);
    }

    // Reduce los cuadros si superan el tamaño máximo de textura de la GPU.
    void fitTexture(DecodedImage& image, int maxSize) {
      const int largest = std::max(image.width, image.height);
      if (maxSize <= 0 || largest <= maxSize) {
        return;
      }
      const float scale = static_cast<float>(maxSize) / static_cast<float>(largest);
      const int width = std::max(1, static_cast<int>(static_cast<float>(image.width) * scale));
      const int height = std::max(1, static_cast<int>(static_cast<float>(image.height) * scale));
      for (auto& frame : image.frames) {
        std::vector<std::uint8_t> resized(static_cast<std::size_t>(width) * height * 4U);
        stbir_resize_uint8_srgb(
            frame.rgba.data(), image.width, image.height, 0, resized.data(), width, height, 0, STBIR_RGBA
        );
        frame.rgba = std::move(resized);
      }
      image.width = width;
      image.height = height;
    }

  } // namespace

  bool isSupportedImage(const fs::path& path) {
    const std::string ext = extension(path);
    return std::ranges::contains(kRasterExtensions, ext) || std::ranges::contains(kRawExtensions, ext);
  }

  std::vector<fs::path> siblingImages(const fs::path& file) {
    std::vector<std::pair<std::string, fs::path>> keyed;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(file.parent_path(), ec)) {
      if (entry.is_regular_file(ec) && isSupportedImage(entry.path())) {
        const std::string name = entry.path().filename().string();
        gchar* key = g_utf8_collate_key_for_filename(name.c_str(), -1);
        keyed.emplace_back(key, entry.path());
        g_free(key);
      }
    }
    std::ranges::sort(keyed, {}, &std::pair<std::string, fs::path>::first);
    std::vector<fs::path> out;
    out.reserve(keyed.size());
    for (auto& [key, path] : keyed) {
      out.push_back(std::move(path));
    }
    return out;
  }

  std::string formatName(const fs::path& path) {
    std::string ext = extension(path);
    if (ext == "jpg" || ext == "jpe" || ext == "jfif") {
      ext = "jpeg";
    } else if (ext == "tif") {
      ext = "tiff";
    } else if (ext == "svgz") {
      ext = "svg";
    }
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext;
  }

  std::expected<DecodedImage, std::string> decodeImage(const fs::path& path, int maxSize, int svgSize) {
    const std::string ext = extension(path);
    if (ext == "svg" || ext == "svgz") {
      auto loaded = loadImageFile(path.string(), maxSize > 0 && svgSize > maxSize ? maxSize : svgSize);
      if (!loaded) {
        return std::unexpected(loaded.error());
      }
      DecodedImage image = still(std::move(loaded->rgba), loaded->width, loaded->height);
      image.sourceWidth = loaded->sourceWidth;
      image.sourceHeight = loaded->sourceHeight;
      image.vector = true;
      return image;
    }
    if (isRaw(path)) {
      auto raw = decodeRaw(path);
      if (raw) {
        fitTexture(*raw, maxSize);
      }
      return raw;
    }

    const auto bytes = readBytes(path);
    if (bytes.empty()) {
      return std::unexpected("empty file");
    }
    std::expected<DecodedImage, std::string> result = std::unexpected("");
    if (ext == "gif") {
      if (auto gif = decodeAnimatedGif(bytes.data(), bytes.size(), kMaxFrames, kMaxAnimationBytes);
          gif && gif->frames.size() > 1) {
        DecodedImage image{
            .width = gif->width,
            .height = gif->height,
            .sourceWidth = gif->width,
            .sourceHeight = gif->height,
            .truncated = gif->truncated
        };
        for (auto& frame : gif->frames) {
          image.frames.push_back(
              {.rgba = std::move(frame.rgba), .durationMs = std::max<std::uint32_t>(20, frame.durationMs)}
          );
        }
        result = std::move(image);
      }
    } else if (ext == "webp") {
      if (auto webp = decodeAnimatedWebP(bytes); webp && webp->frames.size() > 1) {
        result = std::move(webp);
      }
    }
    if (!result) {
      if (auto decoded = decodeRasterImage(bytes.data(), bytes.size())) {
        result = still(std::move(decoded->pixels), decoded->width, decoded->height);
      } else {
        result = decodeWithPixbuf(path);
        if (!result) {
          result = std::unexpected(decoded.error());
        }
      }
    }
    if (result) {
      fitTexture(*result, maxSize);
    }
    return result;
  }

  std::vector<MetadataField> cameraMetadata(const fs::path& path) {
    std::vector<MetadataField> fields;
    try {
      Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
      auto image = Exiv2::ImageFactory::open(path.string());
      image->readMetadata();
      const Exiv2::ExifData& exif = image->exifData();
      const auto value = [&](const char* key) -> std::string {
        const auto it = exif.findKey(Exiv2::ExifKey(key));
        return it != exif.end() ? it->print(&exif) : std::string();
      };
      const auto add = [&](std::string field, std::string text) {
        if (!text.empty()) {
          fields.push_back({.key = std::move(field), .value = std::move(text)});
        }
      };
      std::string make = value("Exif.Image.Make");
      std::string model = value("Exif.Image.Model");
      // Muchos modelos ya empiezan por la marca ("Canon EOS R6").
      add("camera", model.starts_with(make) ? model : (make.empty() ? model : make + " " + model));
      add("lens", value("Exif.Photo.LensModel"));
      add("taken", value("Exif.Photo.DateTimeOriginal"));
      add("exposure", value("Exif.Photo.ExposureTime"));
      add("aperture", value("Exif.Photo.FNumber"));
      add("iso", value("Exif.Photo.ISOSpeedRatings"));
      add("focal", value("Exif.Photo.FocalLength"));
      const std::string lat = value("Exif.GPSInfo.GPSLatitude");
      const std::string lon = value("Exif.GPSInfo.GPSLongitude");
      if (!lat.empty() && !lon.empty()) {
        add(
            "location",
            lat + " " + value("Exif.GPSInfo.GPSLatitudeRef") + ", " + lon + " " + value("Exif.GPSInfo.GPSLongitudeRef")
        );
      }
    } catch (const std::exception&) {
      // Formato sin soporte en exiv2 o archivo dañado: solo no hay datos de cámara.
    }
    return fields;
  }

} // namespace okaru::viewer
