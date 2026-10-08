#include "panel_logo.h"

#include "gpu_vk_device.h"

#include <lucent/log.h>

#include <cstring>
#include <memory>
#include <vector>

namespace psx::host {

PanelLogo::PanelLogo(int width, int height, std::vector<std::uint8_t> rgba)
    : m_width(width), m_height(height), m_rgba(std::move(rgba)) {}

const PanelLogo &PanelLogo::magnifiedNearest(int factor) const {
  if (factor <= 1 || m_width <= 0 || m_height <= 0) {
    return *this;
  }
  if (m_magnified != nullptr && m_magnifiedFactor == factor) {
    return *m_magnified;
  }
  // Each texel becomes a factor x factor block of itself, so the logo keeps the game's pixel grid.
  std::vector<std::uint8_t> scaled(
      static_cast<std::size_t>(m_width) * factor * static_cast<std::size_t>(m_height) * factor * 4, 0);
  const int outW = m_width * factor;
  const int outH = m_height * factor;
  for (int y = 0; y < outH; ++y) {
    const int sy = y / factor;
    for (int x = 0; x < outW; ++x) {
      const int sx = x / factor;
      const std::size_t from = (static_cast<std::size_t>(sy) * m_width + sx) * 4;
      const std::size_t to = (static_cast<std::size_t>(y) * outW + x) * 4;
      std::memcpy(&scaled[to], &m_rgba[from], 4);
    }
  }
  m_magnified = std::make_unique<PanelLogo>(outW, outH, std::move(scaled));
  m_magnifiedFactor = factor;
  return *m_magnified;
}

PanelLogo::~PanelLogo() {
  if (m_texture != nullptr) {
    // Released here because the presentation outlives every panel by the picker's contract, and
    // a picker rebuilt after leaving a title would otherwise leak one texture per title.
    SDL_ReleaseGPUTexture(m_textureDevice, m_texture);
  }
}

SDL_GPUTexture *PanelLogo::texture(GpuDevice &device) const {
  if (empty()) {
    return nullptr;
  }
  // One texture per device: a rebuilt picker on the same presentation must not re-upload, and one
  // on a new presentation must not draw through a texture that device does not own.
  if (m_texture != nullptr && m_textureDevice == device.s_dev) {
    return m_texture;
  }
  if (m_texture != nullptr) {
    SDL_ReleaseGPUTexture(m_textureDevice, m_texture);
    m_texture = nullptr;
  }
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.width = static_cast<Uint32>(m_width);
  info.height = static_cast<Uint32>(m_height);
  info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  // Pixels arrive through a transfer buffer: this SDL has no COPY_DST texture usage, so a copy
  // pass into a plain texture is dropped and the texture stays allocator noise.
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(device.s_dev, &info);
  if (texture == nullptr) {
    lucent::error("picker", "the logo's {}x{} texture could not be created", m_width, m_height);
    return nullptr;
  }
  SDL_GPUTransferBufferCreateInfo upload = {};
  upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  upload.size = m_rgba.size();
  SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(device.s_dev, &upload);
  if (xfer == nullptr) {
    SDL_ReleaseGPUTexture(device.s_dev, texture);
    lucent::error("picker", "the logo's {}x{} upload buffer could not be created", m_width, m_height);
    return nullptr;
  }
  void *mapped = SDL_MapGPUTransferBuffer(device.s_dev, xfer, false);
  if (mapped == nullptr) {
    SDL_ReleaseGPUTransferBuffer(device.s_dev, xfer);
    SDL_ReleaseGPUTexture(device.s_dev, texture);
    lucent::error("picker", "the logo's {}x{} pixels could not be mapped", m_width, m_height);
    return nullptr;
  }
  SDL_memcpy(mapped, m_rgba.data(), m_rgba.size());
  SDL_UnmapGPUTransferBuffer(device.s_dev, xfer);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device.s_dev);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion region = {};
  region.texture = texture;
  region.w = static_cast<Uint32>(m_width);
  region.h = static_cast<Uint32>(m_height);
  region.d = 1;
  SDL_GPUTextureTransferInfo from = {};
  from.transfer_buffer = xfer;
  from.pixels_per_row = static_cast<Uint32>(m_width);
  from.rows_per_layer = static_cast<Uint32>(m_height);
  SDL_UploadToGPUTexture(copy, &from, &region, false);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(device.s_dev, xfer);
  m_texture = texture;
  m_textureDevice = device.s_dev;
  return m_texture;
}

} // namespace psx::host
