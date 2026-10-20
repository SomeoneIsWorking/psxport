// gpu_vk_readback.cpp — what this Game presented, read back: the present image (present shot, the
// pane hold and its content measure), and the device's VRAM written out as images or raw words.
//
// A GPU download goes through a bounded submit-and-wait and refuses rather than handing back a stale
// transfer buffer. VRAM captures read the GPU device's VRAM on the CPU: it is the guest's VRAM.
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_check.h"
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"
#include "image_writer.h" // one checked RGB24 capture-file boundary

#include <lucent/log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdio>
#include <span>
#include <vector>

namespace {

constexpr int kVramW = psx::gpu::kRecordVramWidth;
constexpr int kVramH = psx::gpu::kRecordVramHeight;

// A VRAM rect as RGB24: 15-bit pixels, or a 24bpp display's packed RGB888 (1.5 halfwords a pixel).
bool writeVramImage(std::span<const std::uint16_t> vram, const char *path, psx::gpu::VramRect rect, bool rgb24) {
  const int sx = rect.x;
  const int sy = rect.y;
  const int w = rect.width;
  const int h = rect.height;
  std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(vram.data());
  for (int y = 0; y < h; y++) {
    const int row = (sy + y) % kVramH;
    for (int x = 0; x < w; x++) {
      unsigned char *c = &rgb[(static_cast<std::size_t>(y) * w + x) * 3];
      if (rgb24) {
        const std::uint8_t *line = bytes + static_cast<std::size_t>(row) * kVramW * 2;
        const int bx = sx * 2 + x * 3;
        c[0] = line[bx % (kVramW * 2)];
        c[1] = line[(bx + 1) % (kVramW * 2)];
        c[2] = line[(bx + 2) % (kVramW * 2)];
      } else {
        const std::uint16_t p = vram[static_cast<std::size_t>(row) * kVramW + ((sx + x) & (kVramW - 1))];
        c[0] = static_cast<unsigned char>((p & 31) << 3);
        c[1] = static_cast<unsigned char>(((p >> 5) & 31) << 3);
        c[2] = static_cast<unsigned char>(((p >> 10) & 31) << 3);
      }
    }
  }
  return psx::debug::writeImageRgb24(path, rgb.data(), w, h);
}

} // namespace

// The download half of present_shot, shared with the presented-image probe so "is there anything
// on screen" and "what is on screen" cannot disagree about which image they read.
namespace {

const uint8_t *download_present_image(GpuVkState &g, bool *ok) {
  *ok = false;
  if (!g.device->enabled() || g.device->s_inited == 0 || g.s_present_img == nullptr) {
    return nullptr;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g.device->s_dev);
  GPUCHK(cmd, "present image download cmd");
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {};
  srcr.texture = g.s_present_img;
  srcr.w = (Uint32)g.s_present_img_w;
  srcr.h = (Uint32)g.s_present_img_h;
  srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {};
  dsti.transfer_buffer = g.s_present_rb;
  dsti.pixels_per_row = (Uint32)g.s_present_img_w;
  dsti.rows_per_layer = (Uint32)g.s_present_img_h;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  if (!g.device->submitAndWait(cmd, "present image download")) {
    lucent::error("gpu_vk",
                  "present image download failed; the GPU is latched off and the transfer buffer "
                  "still holds an OLDER frame, so it must not be read as this one");
    return nullptr;
  }
  *ok = true;
  return static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(g.device->s_dev, g.s_present_rb, false));
}

// presentProbeResult — what one readback of the presented image says.
struct PresentProbe {
  bool filled = false;             // this frame carries a real, coherent picture
  const uint8_t *pixels = nullptr; // the readback, mapped; null when nothing could be read
};

// probePresentImage() — ONE readback that answers both questions the picker asks of a panel: is this
// frame a picture, and where is the picture inside it. They were two downloads before, which doubled
// the stall for one answer.
PresentProbe probePresentImage(GpuVkState &g) {
  PresentProbe probe;
  bool ok = false;
  const uint8_t *rgba = download_present_image(g, &ok);
  if (!ok || rgba == nullptr) {
    return probe;
  }
  probe.pixels = rgba;

  // A SAMPLE GRID, not every pixel: the question is "does this frame carry a picture", and a frame of
  // solid colour answers the grid exactly as it answers the full scan. Sampling keeps the check's cost
  // independent of the pane's pixel count.
  //
  // Two questions, one grid. Being non-black is not being a PICTURE: an uninitialised framebuffer is
  // every colour at once — vividly lit, and not a game. A real picture is LOCALLY COHERENT, because
  // neighbouring samples of a rendered frame are near each other whatever the scene is doing, while
  // noise differs from its neighbour as often as not. So: a quarter of the samples must carry colour
  // (which rejects a publisher's logo on black), and two thirds of the neighbouring pairs must be
  // close (which rejects uninitialised memory).
  const int w = g.s_present_img_w;
  const int h = g.s_present_img_h;
  const int kGrid = 16;
  const int kSamples = kGrid * kGrid;
  int lit = 0;
  int coherent = 0;
  int pairs = 0;
  int grid[kGrid * kGrid];
  for (int gy = 0; gy < kGrid; ++gy) {
    const int y = (int)((long)gy * h / kGrid);
    for (int gx = 0; gx < kGrid; ++gx) {
      const int x = (int)((long)gx * w / kGrid);
      const uint8_t *px = rgba + ((long)y * w + x) * 4;
      const int lum = (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;
      grid[gy * kGrid + gx] = lum;
      if (px[0] || px[1] || px[2]) {
        ++lit;
      }
      if (gx > 0) {
        const int delta = grid[gy * kGrid + gx - 1] - lum;
        ++pairs;
        coherent += (delta <= 24 && delta >= -24) ? 1 : 0;
      }
      if (gy > 0) {
        const int delta = grid[(gy - 1) * kGrid + gx] - lum;
        ++pairs;
        coherent += (delta <= 24 && delta >= -24) ? 1 : 0;
      }
    }
  }
  probe.filled = lit >= kSamples / 4 && coherent >= (pairs * 2) / 3;
  // WHAT "A PICTURE" MEANS HERE IS THE TITLE'S BUSINESS, NOT OURS. This probe only answers whether the
  // presented image holds a real, coherent picture; whether that picture is the title's ADVERTISEMENT —
  // its attract demo rather than its publisher card — is answered by the title's own driver, which knows
  // when its boot prefix returned. Pixel heuristics for that were tried and are wrong twice over: a
  // card's sphere spins for minutes (motion cannot tell), and a demo over a wide landscape leaves most
  // of the frame unchanged (coverage cannot tell either).
  if (probe.filled) {
    // Only a frame that HAS a picture says anything about where the picture is: measuring a fade would
    // measure the fade.
    g.measurePresentedContent(rgba, {w, h});
  }
  return probe;
}

} // namespace

// retainFilledPresentImage() — hold the newest presented frame that HAS a picture, and report whether
// this session has one to show.
//
// The hold is filled by UPLOADING the readback this probe already made, not by a GPU-side copy. This
// SDL has no COPY_DST texture usage flag, so a copy pass into a plain texture is dropped by the driver
// and the texture stays whatever the allocator gave it — which is to say noise, drawn as if it were
// the game. The transfer path (download for the probe, upload for the hold) is the one this codebase
// and this SDL already use everywhere.
bool GpuVkState::retainFilledPresentImage() {
  // The probe is per NEW present, not per call: a caller that asks twice about one frame pays once.
  if (s_present_serial == s_present_probed_serial) {
    return s_present_filled != nullptr;
  }
  // A session that already holds a frame still gets that frame REPLACED, on a slow cadence. Holding
  // forever is what it used to do, and it is wrong: a panel's demo plays on, so a panel frozen on the
  // first picture it ever had is a slideshow of one stale frame — and for a title that spends a minute
  // on its publisher card, that frame is the card, forever. The cadence is bounded because the round
  // trip is the cost. It does not bound what a pane SHOWS, which is the live present image (issue
  // 0162); it bounds only how fresh the fallback is.
  constexpr std::uint64_t kHoldRefreshPresents = 15;
  if (s_present_filled != nullptr && s_present_serial - s_present_probed_serial < kHoldRefreshPresents) {
    return true;
  }
  s_present_probed_serial = s_present_serial;
  if (!device->enabled() || device->s_inited == 0 || s_present_img == nullptr || !s_present_viewport.w ||
      !s_present_viewport.h) {
    return false;
  }
  const PresentProbe probe = probePresentImage(*this);
  if (!probe.filled) {
    if (probe.pixels != nullptr) {
      SDL_UnmapGPUTransferBuffer(device->s_dev, s_present_rb);
    }
    // A pane must not show this one, so the picture selector answers with the retained copy.
    s_present_blank_serial = s_present_serial;
    return s_present_filled != nullptr; // keep what is held: a fade or a load screen is not a reason to show nothing
  }
  if (s_present_filled == nullptr || s_present_filled_w != s_present_img_w || s_present_filled_h != s_present_img_h) {
    if (s_present_filled != nullptr) {
      SDL_ReleaseGPUTexture(device->s_dev, s_present_filled);
    }
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    // This SDL has no COPY_DST usage flag: uploads go through a copy pass, which is why the hold is
    // filled by uploading the probe's readback rather than by a texture-to-texture copy.
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = (Uint32)s_present_img_w;
    info.height = (Uint32)s_present_img_h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    s_present_filled = SDL_CreateGPUTexture(device->s_dev, &info);
    GPUCHK(s_present_filled, "CreateGPUTexture(present filled)");
    s_present_filled_w = s_present_filled ? s_present_img_w : 0;
    s_present_filled_h = s_present_filled ? s_present_img_h : 0;
  }
  if (s_present_filled == nullptr) {
    SDL_UnmapGPUTransferBuffer(device->s_dev, s_present_rb);
    return false;
  }
  // The mapped readback is still the source: uploading it back is a copy the SDL actually performs.
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
  GPUCHK(cmd, "present filled upload cmd");
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion dst = {};
  dst.texture = s_present_filled;
  dst.w = (Uint32)s_present_filled_w;
  dst.h = (Uint32)s_present_filled_h;
  dst.d = 1;
  SDL_GPUTextureTransferInfo src = {};
  src.transfer_buffer = s_present_rb;
  src.pixels_per_row = (Uint32)s_present_img_w;
  src.rows_per_layer = (Uint32)s_present_img_h;
  SDL_UploadToGPUTexture(cp, &src, &dst, /*cycle=*/true);
  SDL_EndGPUCopyPass(cp);
  const bool submitted = device->submitAndWait(cmd, "present filled upload");
  SDL_UnmapGPUTransferBuffer(device->s_dev, s_present_rb);
  if (!submitted) {
    return s_present_filled != nullptr; // the held frame stays as it was
  }
  s_present_filled_viewport = s_present_viewport;
  return true;
}

void GpuVkState::releaseFilledPresentImage() {
  if (s_present_filled != nullptr && device != nullptr && device->s_dev != nullptr) {
    SDL_ReleaseGPUTexture(device->s_dev, s_present_filled);
  }
  s_present_filled = nullptr;
  s_present_filled_w = 0;
  s_present_filled_h = 0;
  s_present_filled_viewport = PaneRect{0, 0, 0, 0};
}

// measurePresentedContent() — the picture's own extent inside the presented image, on a coarse grid.
// Titles frame their own image: Spyro 3 draws about fourteen black rows at the top and bottom of its
// 240 lines, and a consumer that crops to the viewport alone carries those into the middle of its own
// output as bars. This is where they are measured out.
//
// It only ever WIDENS what is known. A dark scene (a fade, a night interior) must not be able to
// shrink the crop and zoom into the middle of the picture, so each measurement is a union with the
// last: the rect converges on the picture's true extent and stays there.
void GpuVkState::measurePresentedContent(const uint8_t *rgba, psx::present::PixelSize size) {
  const int w = size.width;
  const int h = size.height;
  // The region of interest: the viewport (the picture inside the letterbox), not the whole image.
  const PaneRect area = s_present_viewport;
  if (area.w <= 0 || area.h <= 0) {
    return;
  }
  const int kGrid = 24;
  int top = -1;
  int bottom = -1;
  int left = -1;
  int right = -1;
  for (int gy = 0; gy < kGrid; ++gy) {
    const int y = area.y + (int)((long)gy * area.h / kGrid);
    if (y < 0 || y >= h) {
      continue;
    }
    for (int gx = 0; gx < kGrid; ++gx) {
      const int x = area.x + (int)((long)gx * area.w / kGrid);
      if (x < 0 || x >= w) {
        continue;
      }
      const uint8_t *px = rgba + ((long)y * w + x) * 4;
      if (px[0] || px[1] || px[2]) {
        if (top < 0) {
          top = y;
        }
        bottom = y + 1;
        if (left < 0) {
          left = x;
        }
        right = x + 1;
      }
    }
  }
  if (top < 0 || bottom <= top || right <= left) {
    return; // a grid with no lit sample in it: nothing to learn
  }
  if (s_present_content.w == 0 || s_present_content.h == 0) {
    s_present_content = PaneRect{left, top, right - left, bottom - top};
    return;
  }
  const int nx = s_present_content.x < left ? s_present_content.x : left;
  const int ny = s_present_content.y < top ? s_present_content.y : top;
  const int ex = s_present_content.x + s_present_content.w > right ? s_present_content.x + s_present_content.w : right;
  const int ey =
      s_present_content.y + s_present_content.h > bottom ? s_present_content.y + s_present_content.h : bottom;
  s_present_content = PaneRect{nx, ny, ex - nx, ey - ny};
}

// ---- present_shot: THE INSTRUMENT THAT WAS MISSING — read back what the player sees ----------------
//
// Every other capture in this framework (shot / dump_to / gpu_vk_render_readback / PSXPORT_GPU_DUMP)
// reads GUEST VRAM, i.e. the stage BEFORE the composite. instruments.md INST-18 records what that cost:
// "nothing in this port samples the swapchain, so every 'the picture is correct' result in this repo is
// a claim about VRAM", after PSXPORT_SHOT_AT certified an intro at 99.95% non-black that the user was
// watching go black. This reads back s_present_img instead — after the letterbox, the fade, the source
// selection and the 24bpp decode — so its answer is about the picture, in EITHER leg.
//
// It cannot silently return an empty file: with no image it says so and writes nothing, because a
// plausible black PPM is exactly how the earlier instruments lied.
void GpuVkState::present_shot(const char *path) {
  if (!device->enabled() || device->s_inited == 0) {
    lucent::warn("present_shot", "GPU not active — NOTHING captured");
    return;
  }
  if (!s_present_img) {
    lucent::warn("present_shot", "no present image yet (no frame has been composited) — NOTHING captured");
    return;
  }
  const int w = s_present_img_w, h = s_present_img_h;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
  GPUCHK(cmd, "present_shot cmd");
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {};
  srcr.texture = s_present_img;
  srcr.w = (Uint32)w;
  srcr.h = (Uint32)h;
  srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {};
  dsti.transfer_buffer = s_present_rb;
  dsti.pixels_per_row = (Uint32)w;
  dsti.rows_per_layer = (Uint32)h;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  if (!device->submitAndWait(cmd, "present_shot")) {
    lucent::error("present_shot",
                  "NOTHING captured for {} — the GPU faulted and is latched off. The "
                  "transfer buffer still holds an older frame; writing it would be a "
                  "capture of the wrong moment presented as this one.",
                  path ? path : "(null)");
    return;
  }
  const uint8_t *rgba = (const uint8_t *)SDL_MapGPUTransferBuffer(device->s_dev, s_present_rb, false);
  unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
  if (!rgb) {
    SDL_UnmapGPUTransferBuffer(device->s_dev, s_present_rb);
    lucent::error("present_shot", "out of memory — NOTHING captured");
    return;
  }
  // The picture is already final: no fade to apply, no format to decode. Anything this function did to
  // the pixels beyond dropping alpha would be the instrument editing its own measurement.
  long nonblack = 0;
  for (long i = 0; i < (long)w * h; i++) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    if (rgba[i * 4 + 0] || rgba[i * 4 + 1] || rgba[i * 4 + 2]) {
      nonblack++;
    }
  }
  SDL_UnmapGPUTransferBuffer(device->s_dev, s_present_rb);
  const bool wrote = psx::debug::writeImageRgb24(path, rgb, w, h);
  free(rgb);
  if (!wrote) {
    // The measurement is real but the FILE is not, and saying "wrote" here is how this instrument
    // would certify a capture that does not exist. errno carries the reason (ENOENT, EACCES, ENOSPC).
    lucent::error("present_shot",
                  "NOTHING captured for {} (image_write said why) — the picture itself was {:.2f}% non-black",
                  path ? path : "(null)",
                  100.0 * (double)nonblack / ((double)w * h));
    return;
  }
  // The coverage rides WITH the file, unconditionally: an all-black present shot is a real and
  // important answer, and it must be distinguishable from a capture that never happened.
  lucent::info("present_shot",
               "wrote {} ({}x{} {} sink) non-black {}/{} ({:.2f}%)",
               path ? path : "(null)",
               w,
               h,
               device->s_headless != 0 ? "headless" : "windowed",
               nonblack,
               (long)w * h,
               100.0 * (double)nonblack / ((double)w * h));
}

void GpuVkState::shot(const char *path) {
  if (s_present_record) {
    record_shot(path);
    return;
  }
  const bool rgb24 = game->core.gpuDevice().displayArea().depth24;
  if (!writeVramImage(game->core.gpuDevice().vram(), path, {s_last_sx, s_last_sy, s_last_w, s_last_h}, rgb24)) {
    lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)");
    return;
  }
  lucent::info(
      "gpu_shot", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", s_last_w, s_last_h, s_last_sx, s_last_sy);
}

namespace psx::gpu {

void vramRegion(Core &core, const char *path, VramRect rect) {
  if (!writeVramImage(core.gpuDevice().vram(), path, rect, false)) {
    lucent::error("gpu_vram", "NOTHING captured for {} (image_write said why)", path ? path : "(null)");
    return;
  }
  lucent::info("gpu_vram", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", rect.width, rect.height, rect.x, rect.y);
}

void vramWords(Core &core, VramPoint at, std::span<std::uint16_t> out) {
  const std::span<const std::uint16_t> vram = core.gpuDevice().vram();
  for (std::size_t i = 0; i < out.size(); i++) {
    out[i] = vram[static_cast<std::size_t>(at.y % kVramH) * kVramW + ((at.x + static_cast<int>(i)) & (kVramW - 1))];
  }
}

void vramRaw(Core &core, const char *path) {
  FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    lucent::error("gpu_vram", "could not open {}, nothing written", path ? path : "(null)");
    return;
  }
  const std::span<const std::uint16_t> vram = core.gpuDevice().vram();
  std::fwrite(vram.data(), 2, vram.size(), file);
  std::fclose(file);
  lucent::info("gpu_vram", "wrote RAW {} ({}x{} u16)", path ? path : "(null)", kVramW, kVramH);
}

} // namespace psx::gpu
