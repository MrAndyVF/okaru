#pragma once

// OKARU: previsualización de okaru-files (vista en columnas): fotos con zoom y arrastre, PDF con el visor de
// okaru-pdf embebido y documentos de oficina (docx, xlsx, pptx, odt, ods, odp…) convertidos a PDF con LibreOffice.

#include "render/core/texture_handle.h"
#include "render/scene/node.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Image;
class InputArea;

namespace okaru::files {

  enum class PreviewKind : std::uint8_t { None, Image, Pdf, Office, Text };

  [[nodiscard]] PreviewKind previewKindFor(const std::string& contentType);

  // Foto decodificada en un hilo (RGBA sin relleno), lista para subir a la GPU en el hilo principal.
  struct DecodedImage {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    int originalWidth = 0; // tamaño real del archivo (la decodificada puede ser más chica)
    int originalHeight = 0;
  };
  // Lado mayor hasta `maxSize`, con la orientación EXIF aplicada. Vacía si no se pudo.
  [[nodiscard]] DecodedImage decodeImage(const std::string& path, int maxSize);

  // Primeros `maxBytes` de un archivo de texto, como UTF-8 válido y hasta `maxLines` líneas.
  [[nodiscard]] std::string readTextPreview(const std::string& path, std::size_t maxBytes, std::size_t maxLines);

  // PDF convertido de un documento de oficina, en la caché (uno por archivo y fecha de modificación).
  [[nodiscard]] std::string officePreviewPath(const std::string& uri, std::int64_t modified);
  // Convierte con LibreOffice sin interfaz (perfil propio: no choca con un LibreOffice abierto). Lento: en un hilo.
  [[nodiscard]] bool convertOfficeToPdf(const std::string& path, const std::string& output, std::string& error);

  // Foto con zoom: rueda acerca/aleja bajo el puntero, arrastrar mueve, doble clic alterna ajustar / 100 %.
  class ImagePreview : public Node {
  public:
    ImagePreview();

    void setTexture(TextureHandle texture, int width, int height);
    void zoomBy(float factor);
    void fit();
    void actualSize();
    [[nodiscard]] float zoomPercent() const;
    void setOnZoomChanged(std::function<void()> callback) { m_onZoomChanged = std::move(callback); }

    void setSize(float width, float height) override;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    [[nodiscard]] float fitScale() const;
    [[nodiscard]] float scale() const { return m_fit ? fitScale() : m_scale; }
    void zoomAround(float factor, float x, float y);
    void clampOffset();
    void notifyZoom();

    Image* m_image = nullptr;
    InputArea* m_input = nullptr;
    TextureHandle m_texture;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    bool m_fit = true;
    float m_scale = 1.0F;
    float m_offsetX = 0.0F; // centro de la vista en coordenadas de la imagen (píxeles)
    float m_offsetY = 0.0F;
    bool m_dragging = false;
    float m_lastX = 0.0F;
    float m_lastY = 0.0F;
    std::uint32_t m_lastClick = 0;
    std::function<void()> m_onZoomChanged;
  };

} // namespace okaru::files
