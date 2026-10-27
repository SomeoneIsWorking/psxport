#include "present_clip.h"

#include "image_writer.h"
#include "render_device.h"
#include "viewport_layout.h"

#include <lucent/log.h>

#include <algorithm>
#include <vector>

namespace psx::render {
namespace {

constexpr std::uint16_t kLastFrameMs = 17;
constexpr std::int64_t kLongestFrameMs = 1000;

} // namespace

PresentClip::~PresentClip() {
  if (device_.up()) {
    device_.release(frames_);
  }
}

void PresentClip::record(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *image, PixelSize size) {
  if (image == nullptr || size.width <= 0 || size.height <= 0) {
    return;
  }
  if (frames_ == nullptr) {
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = kPresentImageFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(kSize.width);
    info.height = static_cast<Uint32>(kSize.height);
    info.layer_count_or_depth = kFrames;
    info.num_levels = 1;
    frames_ = SDL_CreateGPUTexture(device_.handle(), &info);
    if (frames_ == nullptr) {
      lucent::error("present_clip", "no {}-frame clip texture: {}", kFrames, SDL_GetError());
      return;
    }
  }
  const present::PaneRect fit = present::letterbox(size.width, size.height, kSize.width, kSize.height);
  SDL_GPUBlitInfo blit = {};
  blit.source.texture = image;
  blit.source.w = static_cast<Uint32>(size.width);
  blit.source.h = static_cast<Uint32>(size.height);
  blit.destination.texture = frames_;
  blit.destination.layer_or_depth_plane = static_cast<Uint32>(next_);
  blit.destination.x = static_cast<Uint32>(fit.x);
  blit.destination.y = static_cast<Uint32>(fit.y);
  blit.destination.w = static_cast<Uint32>(fit.w);
  blit.destination.h = static_cast<Uint32>(fit.h);
  blit.load_op = SDL_GPU_LOADOP_CLEAR;
  blit.filter = SDL_GPU_FILTER_LINEAR;
  SDL_BlitGPUTexture(cmd, &blit);
  times_[next_] = std::chrono::steady_clock::now();
  next_ = (next_ + 1) % kFrames;
  held_ = std::min(held_ + 1, kFrames);
}

bool PresentClip::write(const char *path) {
  if (frames_ == nullptr || held_ == 0) {
    return false;
  }
  const std::uint32_t frameBytes = static_cast<std::uint32_t>(kSize.width) * kSize.height * 4;
  SDL_GPUTransferBuffer *readback =
      device_.makeTransferBuffer(SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, frameBytes * held_);
  if (readback == nullptr) {
    return false;
  }
  const int oldest = (next_ - held_ + kFrames) % kFrames;
  SDL_GPUCommandBuffer *cmd = device_.acquire();
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  for (int i = 0; i < held_; ++i) {
    SDL_GPUTextureRegion source = {};
    source.texture = frames_;
    source.layer = static_cast<Uint32>((oldest + i) % kFrames);
    source.w = static_cast<Uint32>(kSize.width);
    source.h = static_cast<Uint32>(kSize.height);
    source.d = 1;
    SDL_GPUTextureTransferInfo destination = {};
    destination.transfer_buffer = readback;
    destination.offset = frameBytes * static_cast<Uint32>(i);
    destination.pixels_per_row = static_cast<Uint32>(kSize.width);
    destination.rows_per_layer = static_cast<Uint32>(kSize.height);
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
  }
  SDL_EndGPUCopyPass(copy);
  bool ok = false;
  if (device_.submitAndWait(cmd, "present clip")) {
    const MappedTransfer mapped(device_.handle(), readback);
    if (mapped) {
      std::vector<std::uint16_t> delays(static_cast<std::size_t>(held_), kLastFrameMs);
      for (int i = 0; i + 1 < held_; ++i) {
        const auto shown = times_[(oldest + i + 1) % kFrames] - times_[(oldest + i) % kFrames];
        const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(shown).count();
        delays[static_cast<std::size_t>(i)] =
            static_cast<std::uint16_t>(std::clamp<std::int64_t>(ms, 1, kLongestFrameMs));
      }
      ok = psx::debug::writeAnimationRgba(
          path, {mapped.bytes(), static_cast<std::size_t>(frameBytes) * held_}, kSize.width, kSize.height, delays);
    }
  }
  device_.release(readback);
  return ok;
}

} // namespace psx::render
