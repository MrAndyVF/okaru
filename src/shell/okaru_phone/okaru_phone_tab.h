#pragma once

// OKARU: panel del teléfono (okaru-phone), sobre KdeConnectService. Cabecera con foto, estado y datos;
// emparejamiento; acciones rápidas; multimedia del teléfono (su reproductor MPRIS); imágenes recientes
// para enviar; compartir texto o enlaces; SMS; y alias, foto y desemparejar.

#include "shell/control_center/tab.h"

#include <string>
#include <vector>

class AsyncTextureCache;
class ConfigService;
class Flex;
class Image;
class Input;
class KdeConnectService;
class MprisService;
struct PhoneDevice;

class OkaruPhoneTab : public Tab {
public:
  OkaruPhoneTab(KdeConnectService* service, MprisService* mpris, ConfigService* config, AsyncTextureCache* textures);

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  struct PendingImage {
    Image* image;
    std::string path;
    int size;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildIfNeeded();
  void rebuild(const PhoneDevice* device);
  void loadImages(Renderer& renderer);

  void addHero(Flex& parent, const PhoneDevice& device);
  void addPairing(Flex& parent, const PhoneDevice& device);
  void addActions(Flex& parent, const PhoneDevice& device);
  void addMedia(Flex& parent, const PhoneDevice& device);
  void addRecentImages(Flex& parent, const PhoneDevice& device);
  void addShare(Flex& parent, const PhoneDevice& device);
  void addSms(Flex& parent, const PhoneDevice& device);
  void addFooter(Flex& parent, const PhoneDevice& device);
  void addEmpty(Flex& parent, const char* glyph, const std::string& title, const std::string& body);

  void scanRecentImages();
  [[nodiscard]] std::string mediaPlayerFor(const PhoneDevice& device) const;
  [[nodiscard]] std::string stateKey(const PhoneDevice* device) const;
  void selectDevice(const std::string& id);
  void requestRebuild();

  KdeConnectService* m_service = nullptr;
  MprisService* m_mpris = nullptr;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_textures = nullptr;

  Flex* m_root = nullptr;
  Flex* m_content = nullptr;
  Input* m_shareInput = nullptr;
  Input* m_smsNumber = nullptr;
  Input* m_smsText = nullptr;
  std::vector<PendingImage> m_pendingImages;
  std::vector<std::string> m_recentImages;
  std::string m_key;
  bool m_renaming = false;
  bool m_confirmUnpair = false;
  bool m_showSms = false;
};
