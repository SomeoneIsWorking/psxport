#include "panel_logo.h"

#include "render_device.h"
#include "title_catalog.h"

#include <lucent/log.h>
#include <lucent/platform.h>

#include <array>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace psx::host {
namespace {

constexpr std::array<char, 8> kSavedLogoMagic{'P', 'S', 'X', 'L', 'O', 'G', 'O', '1'};
// A wordmark is at most a VRAM page wide and tall.
constexpr std::uint32_t kSavedLogoMaximumSide = 1024;

} // namespace

SavedLogo::SavedLogo(const TitleIdentity &title)
    : file_([&title]() -> std::optional<std::filesystem::path> {
        const std::optional<std::filesystem::path> directory = lucent::platform::user_data_directory(title.slug);
        if (!directory) {
          return std::nullopt;
        }
        return *directory / "picker-logo.rgba";
      }()) {}

std::optional<LogoImage> SavedLogo::load() const {
  if (!file_) {
    return std::nullopt;
  }
  std::ifstream in(*file_, std::ios::binary);
  std::array<char, kSavedLogoMagic.size()> magic{};
  std::array<std::uint32_t, 2> size{};
  if (!in.read(magic.data(), magic.size()) || magic != kSavedLogoMagic ||
      !in.read(reinterpret_cast<char *>(size.data()), sizeof size)) {
    return std::nullopt;
  }
  if (size[0] == 0 || size[1] == 0 || size[0] > kSavedLogoMaximumSide || size[1] > kSavedLogoMaximumSide) {
    lucent::warn("picker", "{} holds a {}x{} logo; ignored", file_->string(), size[0], size[1]);
    return std::nullopt;
  }
  LogoImage image{static_cast<int>(size[0]), static_cast<int>(size[1]), {}};
  image.rgba.resize(static_cast<std::size_t>(size[0]) * size[1] * 4);
  if (!in.read(reinterpret_cast<char *>(image.rgba.data()), static_cast<std::streamsize>(image.rgba.size()))) {
    lucent::warn("picker", "{} is cut short; ignored", file_->string());
    return std::nullopt;
  }
  return image;
}

bool SavedLogo::save(const LogoImage &image) const {
  if (!file_) {
    return false;
  }
  std::string error;
  if (!lucent::platform::ensure_user_data_directory(file_->parent_path(), error)) {
    lucent::warn("picker", "cannot keep the logo in {}: {}", file_->parent_path().string(), error);
    return false;
  }
  const std::filesystem::path pending = std::filesystem::path(*file_) += ".pending";
  {
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    const std::array<std::uint32_t, 2> size{static_cast<std::uint32_t>(image.width),
                                            static_cast<std::uint32_t>(image.height)};
    out.write(kSavedLogoMagic.data(), kSavedLogoMagic.size());
    out.write(reinterpret_cast<const char *>(size.data()), sizeof size);
    out.write(reinterpret_cast<const char *>(image.rgba.data()), static_cast<std::streamsize>(image.rgba.size()));
    if (!out.flush()) {
      lucent::warn("picker", "cannot write {}", pending.string());
      return false;
    }
  }
  std::error_code renamed;
  std::filesystem::rename(pending, *file_, renamed);
  if (renamed) {
    lucent::warn("picker", "cannot replace {}: {}", file_->string(), renamed.message());
    return false;
  }
  return true;
}

PanelLogo::PanelLogo(LogoImage image) : m_width(image.width), m_height(image.height), m_rgba(std::move(image.rgba)) {}

// The device outlives every panel, so a rebuilt picker does not leak one texture per title.
PanelLogo::~PanelLogo() {
  if (m_textureDevice != nullptr) {
    m_textureDevice->release(m_texture);
  }
}

SDL_GPUTexture *PanelLogo::texture(psx::render::RenderDevice &device) const {
  if (empty()) {
    return nullptr;
  }
  if (m_texture != nullptr && m_textureDevice == &device) {
    return m_texture;
  }
  if (m_textureDevice != nullptr) {
    m_textureDevice->release(m_texture);
  }
  m_texture = device.makeFilledTexture({m_width, m_height}, m_rgba);
  m_textureDevice = &device;
  return m_texture;
}

} // namespace psx::host
