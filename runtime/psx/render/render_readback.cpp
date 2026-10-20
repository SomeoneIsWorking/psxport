// render_readback.cpp — what this Game presented, read back: the present image (present shot, the pane
// hold and its content measure), and the device's VRAM written out as images or raw words.
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "image_writer.h"
#include "render.h"
#include "render_device.h"
#include "render_state.h"

#include <lucent/log.h>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace psx::render {

void writePresentShot(const char *channel, const char *path, const std::uint8_t *rgba, present::PixelSize size) {
  const std::size_t pixels = static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
  std::vector<unsigned char> rgb(pixels * 3);
  long nonBlack = 0;
  for (std::size_t i = 0; i < pixels; i++) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    nonBlack += (rgba[i * 4 + 0] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) != 0 ? 1 : 0;
  }
  const double share = pixels > 0 ? 100.0 * static_cast<double>(nonBlack) / static_cast<double>(pixels) : 0.0;
  if (!debug::writeImageRgb24(path, rgb.data(), size.width, size.height)) {
    lucent::error(channel, "nothing captured for {}; the picture was {:.2f}% non-black", path, share);
    return;
  }
  lucent::info(
      channel, "wrote {} ({}x{}) non-black {}/{} ({:.2f}%)", path, size.width, size.height, nonBlack, pixels, share);
}

void RenderState::presentShot(const char *path) {
  if (!device_.enabled() || !device_.up() || presentImage_ == nullptr) {
    lucent::warn("present_shot", "no present image yet, nothing captured for {}", path);
    return;
  }
  const MappedTransfer rgba = device_.download(
      presentImage_, {0, 0, presentImageSize_.width, presentImageSize_.height}, presentReadback_, "present shot");
  if (!rgba) {
    lucent::error("present_shot", "the GPU refused the readback, nothing captured for {}", path);
    return;
  }
  writePresentShot("present_shot", path, rgba.bytes(), presentImageSize_);
}

void RenderState::shot(const char *path) {
  if (presentsRecord_) {
    recordShot(path);
    return;
  }
  gpu::GpuDevice &guest = game_.core.gpuDevice();
  const gpu::Rgb24Picture picture = guest.picture(lastDisplay_, guest.displayArea().depth24);
  if (!debug::writeImageRgb24(path, picture.pixels.data(), picture.width, picture.height)) {
    lucent::error("gpu_shot", "nothing captured for {}", path);
    return;
  }
  lucent::info("gpu_shot",
               "wrote {} ({}x{} @ {},{})",
               path,
               lastDisplay_.width,
               lastDisplay_.height,
               lastDisplay_.x,
               lastDisplay_.y);
}

PresentedImage RenderState::newestPresentedPicture() const {
  if (blankSerial_ == presentSerial_ && filled_ != nullptr) {
    return {filled_, filledSize_.width, filledSize_.height, filledViewport_, content_};
  }
  return lastPresented();
}

// A picture is lit on a quarter of a sample grid and locally coherent on two thirds of its neighbour
// pairs, which rejects a logo on black and uninitialised memory.
bool RenderState::probePicture(const std::uint8_t *rgba) {
  const int w = presentImageSize_.width;
  const int h = presentImageSize_.height;
  constexpr int kGrid = 16;
  constexpr int kSamples = kGrid * kGrid;
  int lit = 0;
  int coherent = 0;
  int pairs = 0;
  int grid[kSamples];
  for (int gy = 0; gy < kGrid; ++gy) {
    const int y = static_cast<int>(static_cast<long>(gy) * h / kGrid);
    for (int gx = 0; gx < kGrid; ++gx) {
      const int x = static_cast<int>(static_cast<long>(gx) * w / kGrid);
      const std::uint8_t *px = rgba + (static_cast<long>(y) * w + x) * 4;
      const int lum = (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;
      grid[gy * kGrid + gx] = lum;
      lit += (px[0] | px[1] | px[2]) != 0 ? 1 : 0;
      if (gx > 0) {
        ++pairs;
        coherent += std::abs(grid[gy * kGrid + gx - 1] - lum) <= 24 ? 1 : 0;
      }
      if (gy > 0) {
        ++pairs;
        coherent += std::abs(grid[(gy - 1) * kGrid + gx] - lum) <= 24 ? 1 : 0;
      }
    }
  }
  return lit >= kSamples / 4 && coherent >= (pairs * 2) / 3;
}

// The hold is refreshed every kHoldRefreshPresents presents from the probe's own readback: this SDL has
// no COPY_DST usage, so a texture-to-texture copy into a sampled texture would be dropped.
bool RenderState::retainFilledPresentImage() {
  if (presentSerial_ == probedSerial_) {
    return filled_ != nullptr;
  }
  constexpr std::uint64_t kHoldRefreshPresents = 15;
  if (filled_ != nullptr && presentSerial_ - probedSerial_ < kHoldRefreshPresents) {
    return true;
  }
  probedSerial_ = presentSerial_;
  if (!device_.enabled() || !device_.up() || presentImage_ == nullptr || viewport_.w == 0 || viewport_.h == 0) {
    return false;
  }
  const MappedTransfer rgba = device_.download(
      presentImage_, {0, 0, presentImageSize_.width, presentImageSize_.height}, presentReadback_, "present probe");
  if (!rgba) {
    return filled_ != nullptr;
  }
  if (!probePicture(rgba.bytes())) {
    // A fade or a load screen keeps what is held.
    blankSerial_ = presentSerial_;
    return filled_ != nullptr;
  }
  measurePresentedContent(rgba.bytes());
  if (filled_ == nullptr || filledSize_ != presentImageSize_) {
    device_.release(filled_);
    filled_ = device_.makeTexture(presentImageSize_, kPresentImageFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    filledSize_ = presentImageSize_;
  }
  SDL_GPUCommandBuffer *cmd = device_.acquire();
  device_.upload(cmd, presentReadback_, filled_, filledSize_);
  if (!device_.submitAndWait(cmd, "present hold upload")) {
    return true;
  }
  filledViewport_ = viewport_;
  return true;
}

// The picture's own lit extent inside the viewport on a coarse grid, only ever widened, so a dark
// scene cannot shrink the crop.
void RenderState::measurePresentedContent(const std::uint8_t *rgba) {
  const int w = presentImageSize_.width;
  const int h = presentImageSize_.height;
  const present::PaneRect area = viewport_;
  if (area.w <= 0 || area.h <= 0) {
    return;
  }
  constexpr int kGrid = 24;
  int top = -1;
  int bottom = -1;
  int left = w;
  int right = -1;
  for (int gy = 0; gy < kGrid; ++gy) {
    const int y = area.y + static_cast<int>(static_cast<long>(gy) * area.h / kGrid);
    if (y < 0 || y >= h) {
      continue;
    }
    for (int gx = 0; gx < kGrid; ++gx) {
      const int x = area.x + static_cast<int>(static_cast<long>(gx) * area.w / kGrid);
      if (x < 0 || x >= w) {
        continue;
      }
      const std::uint8_t *px = rgba + (static_cast<long>(y) * w + x) * 4;
      if ((px[0] | px[1] | px[2]) != 0) {
        top = top < 0 ? y : top;
        bottom = y + 1;
        left = std::min(left, x);
        right = std::max(right, x + 1);
      }
    }
  }
  if (top < 0 || right <= left) {
    return;
  }
  if (content_.w == 0 || content_.h == 0) {
    content_ = present::PaneRect{left, top, right - left, bottom - top};
    return;
  }
  const int x0 = std::min(content_.x, left);
  const int y0 = std::min(content_.y, top);
  const int x1 = std::max(content_.x + content_.w, right);
  const int y1 = std::max(content_.y + content_.h, bottom);
  content_ = present::PaneRect{x0, y0, x1 - x0, y1 - y0};
}

void vramRegion(Core &core, const char *path, gpu::VramRect rect) {
  const gpu::Rgb24Picture picture = core.gpuDevice().picture(rect, false);
  if (!debug::writeImageRgb24(path, picture.pixels.data(), picture.width, picture.height)) {
    lucent::error("gpu_vram", "nothing captured for {}", path);
    return;
  }
  lucent::info("gpu_vram", "wrote {} ({}x{} @ {},{})", path, rect.width, rect.height, rect.x, rect.y);
}

void vramWords(Core &core, gpu::VramPoint at, std::span<std::uint16_t> out) {
  const std::span<const std::uint16_t> vram = core.gpuDevice().vram();
  const std::size_t row = static_cast<std::size_t>(at.y % gpu::kVramHeight) * gpu::kVramWidth;
  for (std::size_t i = 0; i < out.size(); i++) {
    out[i] = vram[row + ((static_cast<std::size_t>(at.x) + i) & (gpu::kVramWidth - 1))];
  }
}

void vramRaw(Core &core, const char *path) {
  FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    lucent::error("gpu_vram", "could not open {}, nothing written", path);
    return;
  }
  const std::span<const std::uint16_t> vram = core.gpuDevice().vram();
  std::fwrite(vram.data(), 2, vram.size(), file);
  std::fclose(file);
  lucent::info("gpu_vram", "wrote raw {} ({}x{} u16)", path, gpu::kVramWidth, gpu::kVramHeight);
}

} // namespace psx::render
