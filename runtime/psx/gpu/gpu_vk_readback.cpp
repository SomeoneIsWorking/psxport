// gpu_vk_readback.cpp — the readback owner: GPU → host capture of what this Game's VRAM image and
// presented picture actually hold.
//
// MOVED out of gpu_vk.cpp along its existing seam (the `readback (shot / vram dump)` banner and the
// `present_shot` banner it belongs with): every instrument that asks the GPU for BYTES rather than
// asking it to draw. They are one owner because they share one hazard — a download that did not
// happen, or did not happen this frame — and one answer to it: a bounded submit-and-wait that
// refuses rather than handing back a stale transfer buffer. Splitting them from the present/raster
// halves of gpu_vk.cpp is what let a capture instrument sit in its own file instead of inside the
// per-frame render path.
//
// The bodies are unchanged: they reached the device through the same handle macros (now declared
// once in gpu_vk_device_handles.h) and reached each other through gpu_vk_internal.h.
#include "cfg.h" // cfg_on — the opt-in GPU trace counters around a download
#include "core.h"
#include "fs_util.h" // host diagnostic-output directory creation
#include "game.h"
#include "gpu_vk.h"                // gpu_vk_enabled, gpu_vk_present_sink_size
#include "gpu_vk_check.h"          // GPUCHK — the one rule for a handle the run depends on
#include "gpu_vk_device_handles.h" // s_dev / s_inited / s_headless, spelled once for every renderer TU
#include "gpu_vk_internal.h"
#include "image_writer.h" // one checked RGB24 capture-file boundary
#include "present_fade_state.h"
#include "present_plan.h" // present_fade_rgb — the present shader's fade, on the CPU

#include <lucent/log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// ---- readback (shot / vram dump): download THIS Game's VRAM image → host, decode 1555 → PPM ---------
const uint16_t *readback_vram(GpuVkState &g) {
  // STEP TRACE (PSXPORT_DEBUG=rbtrace). Kept, not temporary: this is what falsified issue 0018's
  // recorded diagnosis. That issue said this function BLOCKED from three call sites; the trace shows
  // enter -> targets ok -> cmd acquired -> submitted -> fence signalled every time, and the real fault
  // was a null optional GameHooks call at the DUMP site. A claim that a specific call blocks should be
  // cheap to check rather than re-argued.
  lucent::debug("rbtrace", "enter");
  g.ensure_targets();
  lucent::debug("rbtrace", "targets ok");
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  lucent::debug("rbtrace", "cmd acquired");
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {};
  srcr.texture = g.s_vram_tex;
  srcr.w = VRAM_W;
  srcr.h = VRAM_H;
  srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {};
  dsti.transfer_buffer = g.s_rb_xfer;
  dsti.pixels_per_row = VRAM_W;
  dsti.rows_per_layer = VRAM_H;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  lucent::debug("rbtrace", "copy pass ended, submitting");
  // Bounded submit+wait. On a fault the transfer buffer holds whatever was last in it, so returning it
  // would hand the caller a STALE VRAM image that looks like a real readback — refuse instead.
  if (!gpu_submit_and_wait(cmd, "readback_vram")) {
    lucent::error("gpu_vk",
                  "readback_vram: no VRAM image this call — the GPU is latched off. Callers get "
                  "nullptr rather than the previous frame's bytes.");
    return nullptr;
  }
  lucent::debug("rbtrace", "fence signalled");
  const uint16_t *p = (const uint16_t *)SDL_MapGPUTransferBuffer(s_dev, g.s_rb_xfer, false);
  if (cfg_on("PSXPORT_GPU_TRACE")) {
    long nz = 0;
    for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) {
      if (p[i]) {
        nz++;
      }
    }
    lucent::info("gpu_vk", "readback nonzero={}/{}", nz, VRAM_W * VRAM_H);
  }
  return p;
}
// The download half of present_shot, shared with the presented-image probe so "is there anything
// on screen" and "what is on screen" cannot disagree about which image they read.
static const uint8_t *download_present_image(GpuVkState &g, bool *ok) {
  *ok = false;
  if (!gpu_vk_enabled() || !s_inited || !g.s_present_img) {
    return nullptr;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_dev);
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
  if (!gpu_submit_and_wait(cmd, "present image download")) {
    lucent::error("gpu_vk",
                  "present image download failed; the GPU is latched off and the transfer buffer "
                  "still holds an OLDER frame, so it must not be read as this one");
    return nullptr;
  }
  *ok = true;
  return static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(s_dev, g.s_present_rb, false));
}

// presentProbeResult — what one readback of the presented image says.
struct PresentProbe {
  bool filled = false;             // this frame carries a real, coherent picture
  bool advertises = false;         // ...and it MOVED since the last probe: a demo, not a held card
  const uint8_t *pixels = nullptr; // the readback, mapped; null when nothing could be read
};

// probePresentImage() — ONE readback that answers both questions the picker asks of a panel: is this
// frame a picture, and where is the picture inside it. They were two downloads before, which doubled
// the stall for one answer.
static PresentProbe probePresentImage(GpuVkState &g) {
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
    g.measurePresentedContent(rgba, w, h);
  }
  return probe;
}

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
  // on its publisher card, that frame is the card, forever. Refreshing every frame would mean a
  // GPU->CPU->GPU round trip per panel per field, which is the whole reason the hold exists; so the
  // cadence is bounded instead: at most one refresh every kHoldRefreshPresents new presents, which is
  // a quarter second of a panel's own time at 60 Hz. A hold is what this is for — it survives fades,
  // loads and black frames — not a way to stop time.
  constexpr std::uint64_t kHoldRefreshPresents = 15;
  if (s_present_filled != nullptr && s_present_serial - s_present_probed_serial < kHoldRefreshPresents) {
    return true;
  }
  s_present_probed_serial = s_present_serial;
  if (!gpu_vk_enabled() || !s_inited || !s_present_img || !s_present_viewport.w || !s_present_viewport.h) {
    return false;
  }
  const PresentProbe probe = probePresentImage(*this);
  if (!probe.filled) {
    if (probe.pixels != nullptr) {
      SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb);
    }
    return false; // keep what is held: a fade or a load screen is not a reason to show nothing
  }
  if (s_present_filled == nullptr || s_present_filled_w != s_present_img_w || s_present_filled_h != s_present_img_h) {
    if (s_present_filled != nullptr) {
      SDL_ReleaseGPUTexture(s_dev, s_present_filled);
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
    s_present_filled = SDL_CreateGPUTexture(s_dev, &info);
    GPUCHK(s_present_filled, "CreateGPUTexture(present filled)");
    s_present_filled_w = s_present_filled ? s_present_img_w : 0;
    s_present_filled_h = s_present_filled ? s_present_img_h : 0;
  }
  if (s_present_filled == nullptr) {
    SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb);
    return false;
  }
  // The mapped readback is still the source: uploading it back is a copy the SDL actually performs.
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_dev);
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
  const bool submitted = gpu_submit_and_wait(cmd, "present filled upload");
  SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb);
  if (!submitted) {
    return false; // the held frame stays as it was
  }
  s_present_filled_viewport = s_present_viewport;
  return true;
}

void GpuVkState::releaseFilledPresentImage() {
  if (s_present_filled != nullptr && s_dev != nullptr) {
    SDL_ReleaseGPUTexture(s_dev, s_present_filled);
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
void GpuVkState::measurePresentedContent(const uint8_t *rgba, int w, int h) {
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
  if (!gpu_vk_enabled() || !s_inited) {
    lucent::warn("present_shot", "GPU not active — NOTHING captured");
    return;
  }
  if (!s_present_img) {
    lucent::warn("present_shot", "no present image yet (no frame has been composited) — NOTHING captured");
    return;
  }
  const int w = s_present_img_w, h = s_present_img_h;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_dev);
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
  if (!gpu_submit_and_wait(cmd, "present_shot")) {
    lucent::error("present_shot",
                  "NOTHING captured for {} — the GPU faulted and is latched off. The "
                  "transfer buffer still holds an older frame; writing it would be a "
                  "capture of the wrong moment presented as this one.",
                  path ? path : "(null)");
    return;
  }
  const uint8_t *rgba = (const uint8_t *)SDL_MapGPUTransferBuffer(s_dev, s_present_rb, false);
  unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
  if (!rgb) {
    SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb);
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
  SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb);
  const bool wrote = image_write_rgb24(path, rgb, w, h);
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
               s_headless ? "headless" : "windowed",
               nonblack,
               (long)w * h,
               100.0 * (double)nonblack / ((double)w * h));
}

bool dump_to(GpuVkState &g,
             const char *path,
             int sx,
             int sy,
             int w,
             int h,
             int fade_mode,
             uint8_t fade_r,
             uint8_t fade_g,
             uint8_t fade_b) {
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t *vram = readback_vram(g);
  if (!vram) {
    lucent::error("gpu_vk", "shot24: NOTHING captured — GPU latched off");
    return false;
  }
  unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
  if (!rgb) {
    SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
    return false;
  }
  const int fade[4] = {fade_mode, fade_r, fade_g, fade_b};
  // 24bpp packs RGB888 across 1.5 VRAM halfwords per pixel, so column x of the display sits at BYTE
  // offset sx*2 + x*3 in the row — not at halfword sx+x. Decoding it as 1555 is what scrambled the
  // colours and showed two thirds of the width.
  const int rgb24 = g.s_disp_rgb24;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int r, g_, b;
      if (rgb24) {
        const uint8_t *row = (const uint8_t *)&vram[((sy + y) % VRAM_H) * VRAM_W];
        int bx = sx * 2 + x * 3; // VRAM rows are VRAM_W halfwords = VRAM_W*2 bytes
        r = row[bx % (VRAM_W * 2)];
        g_ = row[(bx + 1) % (VRAM_W * 2)];
        b = row[(bx + 2) % (VRAM_W * 2)];
      } else {
        uint16_t p = vram[((sy + y) % VRAM_H) * VRAM_W + ((sx + x) & 1023)];
        r = (p & 31) << 3;
        g_ = ((p >> 5) & 31) << 3;
        b = ((p >> 10) & 31) << 3;
      }
      int g = g_;
      present_fade_rgb(fade, r, g, b);
      unsigned char *c = &rgb[((size_t)y * w + x) * 3];
      c[0] = (unsigned char)r;
      c[1] = (unsigned char)g;
      c[2] = (unsigned char)b;
    }
  }
  // Propagated, not swallowed: every VRAM shot below reports the file it ACTUALLY wrote. These
  // callers logged "wrote <path>" unconditionally too — the same lie as present_shot's, and measured
  // in the same run — so they are fixed in the same pass rather than left as the older half of a
  // rule that now only holds in one place.
  const bool wrote = image_write_rgb24(path, rgb, w, h);
  free(rgb);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
  return wrote;
}
void GpuVkState::shot(const char *path) {
  if (!gpu_vk_enabled() || !s_inited) {
    lucent::warn("gpu_shot", "GPU not active — NOTHING captured");
    return;
  }
  if (s_present_record) {
    record_shot(path);
    return;
  }
  FadeState f = present_fade_state(&game->core);
  const bool wrote = dump_to(*this, path, s_last_sx, s_last_sy, s_last_w, s_last_h, f.mode, f.r, f.g, f.b);
  if (!wrote) {
    lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)");
    return;
  }
  lucent::info(
      "gpu_shot", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", s_last_w, s_last_h, s_last_sx, s_last_sy);
}
void GpuVkState::shot_b(const char *path) {
  shot(path);
} // Pass 1: single target
// GP1(0x08) bit 4 changed. The decode lives in gpu_native; the two things that DECODE the display region
// live here, so the bit has to cross over. Silently ignoring it is what made a 24bpp still render with
// every colour scrambled and only two thirds of the width shown.
void gpu_vk_set_display_depth(Core *core, int rgb24) {
  if (!core || !core->game) {
    return;
  }
  core->game->gpu_vk.s_disp_rgb24 = rgb24 ? 1 : 0;
}
void gpu_vk_shot_region(Core *core, const char *path, int sx, int sy, int w, int h) {
  if (!gpu_vk_enabled() || !s_inited) {
    return;
  }
  FadeState f = present_fade_state(core);
  if (!dump_to(core->game->gpu_vk, path, sx, sy, w, h, f.mode, f.r, f.g, f.b)) {
    lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)");
    return;
  }
  lucent::info("gpu_shot", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", w, h, sx, sy);
}
void gpu_vk_vram_region(Core *core, const char *path, int x, int y, int w, int h) {
  if (!gpu_vk_enabled() || !s_inited) {
    return;
  }
  if (!dump_to(core->game->gpu_vk, path, x, y, w, h, 0, 0, 0, 0)) { // raw VRAM region dump — no engine fade applied
    lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)");
  }
  lucent::info("gpu_vram", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", w, h, x, y);
}
// Raw 16-bit VRAM words at (x,y..y+n-1 wrapped along X) — dark-outline STP-bit diag (2026-07-01,
// scratch/handoff.md): tells apart a genuine opaque texel (STP=0, faithful) from a lost/miscomputed
// STP bit (would-be-blended texel drawing solid instead of translucent).
void gpu_vk_vram_words(Core *core, int x, int y, int n, uint16_t *out) {
  if (!gpu_vk_enabled() || !s_inited) {
    for (int i = 0; i < n; i++) {
      out[i] = 0;
    }
    return;
  }
  GpuVkState &g = core->game->gpu_vk;
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t *vram = readback_vram(g);
  if (!vram) {
    lucent::error("gpu_vk",
                  "vram row read of {} word(s) at ({},{}) UNAVAILABLE — GPU latched "
                  "off; `out` is left untouched, not zero-filled, so a caller cannot "
                  "mistake a fault for a black row",
                  n,
                  x,
                  y);
    return;
  }
  for (int i = 0; i < n; i++) {
    out[i] = vram[(y % VRAM_H) * VRAM_W + ((x + i) & 1023)];
  }
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
}
void gpu_vk_vram_raw(Core *core, const char *path) {
  if (!gpu_vk_enabled() || !s_inited) {
    return;
  }
  GpuVkState &g = core->game->gpu_vk;
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t *vram = readback_vram(g);
  if (!vram) {
    lucent::error("gpu_vk", "vramdump: NOTHING written to {} — GPU latched off", path ? path : "(null)");
    return;
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
    return;
  }
  for (int y = 0; y < VRAM_H; y++) {
    fwrite(&vram[y * VRAM_W], 2, VRAM_W, f);
  }
  fclose(f);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
  lucent::info("gpu_vram", "wrote RAW {} ({}x{} u16)", path ? path : "(null)", VRAM_W, VRAM_H);
}
