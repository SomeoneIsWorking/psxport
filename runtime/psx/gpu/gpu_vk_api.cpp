// gpu_vk_api.cpp — the Core*-facing entry points of the SDL_GPU present backend.
//
// MOVED out of gpu_vk.cpp along its own existing seams: the dual-view (SBS) hooks
// and the public-API wrapper block that stands between the guest-facing `gpu_vk_*` API (gpu_vk.h) and
// the per-instance `GpuVkState` methods. These two live together because they are the SAME direction
// of travel — one call in, one method (or one policy) reached — and because a wrapper is the
// cheapest thing in the file to read and the easiest thing to misread as the owner. The methods they
// reach stay in gpu_vk.cpp; nothing here decides anything a method does not already decide.
//
// The bodies are unchanged.
#include "core.h"
#include "fs_util.h" // host diagnostic-output directory creation (preseq arm)
#include "game.h"
#include "gpu_vk.h"
#include "gpu_vk_device_handles.h" // s_dev / s_inited, spelled once for every renderer TU
#include "gpu_vk_fadewatch.h"
#include "gpu_vk_internal.h"
#include "image_writer.h"
#include "overlay_glue.h"
#include "picture_announce.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <stdint.h>
#include <stdio.h>

// ---- dual-view / SBS target selection (single target in Pass 1) -------------------------------------
void gpu_vk_select_target(int t) {
  (void)t;
}
int gpu_vk_target_count(int t) {
  (void)t;
  return 0;
}
void gpu_vk_rawdump_arm(const char *path, int frame) {
  (void)path;
  (void)frame;
}

// ---- Public API: thin free-function wrappers over the per-instance GpuVkState methods ---------------
void gpu_vk_set_vd(Core *core, const float *d3) {
  core->game->gpu_vk.set_vd(d3);
}
void gpu_vk_set_vd_n(Core *core, const float *d3) {
  core->game->gpu_vk.set_vd_n(d3);
}
void gpu_vk_set_xyf(Core *core, const float *xf, const float *yf) {
  core->game->gpu_vk.set_xyf(xf, yf);
}
void gpu_vk_set_order_override(Core *core, uint32_t seq) {
  core->game->gpu_vk.s_order_override = seq;
}
void gpu_vk_set_untextured_material(Core *core, int gouraud, int dither) {
  core->game->gpu_vk.s_untextured_gouraud = gouraud;
  core->game->gpu_vk.s_untextured_dither = dither;
}
void gpu_vk_set_order(Core *core, unsigned idx) {
  core->game->gpu_vk.set_order(idx);
}
void gpu_vk_set_order_2d(Core *core, unsigned idx) {
  core->game->gpu_vk.set_order_2d(idx);
}
void gpu_vk_set_order_2d_n(Core *core, unsigned idx) {
  core->game->gpu_vk.set_order_2d_n(idx);
}
void gpu_vk_set_order_2d_bg(Core *core, unsigned idx) {
  core->game->gpu_vk.set_order_2d_bg(idx);
}
void gpu_vk_set_order_2d_bg_n(Core *core, unsigned idx) {
  core->game->gpu_vk.set_order_2d_bg_n(idx);
}
void gpu_vk_semi_group(Core *core, int x0, int y0, int x1, int y1) {
  core->game->gpu_vk.semi_group(x0, y0, x1, y1);
}
void gpu_vk_stats(Core *core, int *tri, int *tex, int *semi) {
  core->game->gpu_vk.stats(tri, tex, semi);
}
void gpu_vk_dirty(Core *core, int x, int y, int w, int h) {
  core->game->gpu_vk.dirty(x, y, w, h);
}
void gpu_vk_present(Core *core, const uint16_t *src, int sx, int sy, int w, int h) {
  overlay_glue_frame_begin(core);
  psx::picture::announceOnChange(*core, w); // `w` is why: see picture_announce.h
  core->game->gpu_vk.present(src, sx, sy, w, h);
  gpu_vk_fadewatch_tap(core, sx, sy, w, h);
}
void gpu_vk_repaint(Core *core) {
  overlay_glue_frame_begin(core); // the RmlUi overlay stays interactive while the game is frozen
  core->game->gpu_vk.repaint();
}
void gpu_vk_draw_tri(Core *core,
                     int x0,
                     int y0,
                     int r0,
                     int g0,
                     int b0,
                     int x1,
                     int y1,
                     int r1,
                     int g1,
                     int b1,
                     int x2,
                     int y2,
                     int r2,
                     int g2,
                     int b2,
                     int dax0,
                     int day0,
                     int dax1,
                     int day1) {
  core->game->gpu_vk.draw_tri(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, x2, y2, r2, g2, b2, dax0, day0, dax1, day1);
}
void gpu_vk_draw_line(Core *core,
                      int x0,
                      int y0,
                      int r0,
                      int g0,
                      int b0,
                      int x1,
                      int y1,
                      int r1,
                      int g1,
                      int b1,
                      int dax0,
                      int day0,
                      int dax1,
                      int day1) {
  core->game->gpu_vk.draw_line(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, dax0, day0, dax1, day1);
}
void gpu_vk_draw_tritri(Core *core,
                        const int *xs,
                        const int *ys,
                        const int *us,
                        const int *vs,
                        const unsigned char *rs,
                        const unsigned char *gs,
                        const unsigned char *bs,
                        int tpx,
                        int tpy,
                        int mode,
                        int raw,
                        int clutx,
                        int cluty,
                        int twmx,
                        int twmy,
                        int twox,
                        int twoy,
                        int dax0,
                        int day0,
                        int dax1,
                        int day1) {
  core->game->gpu_vk.draw_tritri(
      xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty, twmx, twmy, twox, twoy, dax0, day0, dax1, day1);
}
void gpu_vk_draw_semi(Core *core,
                      const int *xs,
                      const int *ys,
                      const int *us,
                      const int *vs,
                      const unsigned char *rs,
                      const unsigned char *gs,
                      const unsigned char *bs,
                      int tpx,
                      int tpy,
                      int mode,
                      int raw,
                      int clutx,
                      int cluty,
                      int twmx,
                      int twmy,
                      int twox,
                      int twoy,
                      int dax0,
                      int day0,
                      int dax1,
                      int day1,
                      int blend) {
  core->game->gpu_vk.draw_semi(xs,
                               ys,
                               us,
                               vs,
                               rs,
                               gs,
                               bs,
                               tpx,
                               tpy,
                               mode,
                               raw,
                               clutx,
                               cluty,
                               twmx,
                               twmy,
                               twox,
                               twoy,
                               dax0,
                               day0,
                               dax1,
                               day1,
                               blend);
}
void gpu_vk_shot(Core *core, const char *path) {
  core->game->gpu_vk.shot(path);
}

bool gpu_vk_read_vram_rect(Core *core, int x, int y, int w, int h, uint16_t *out) {
  if (core == nullptr || out == nullptr || w <= 0 || h <= 0) {
    return false;
  }
  GpuVkState &g = core->game->gpu_vk;
  if (!gpu_vk_enabled() || !s_inited) {
    lucent::error("gpu_vk", "read_vram_rect: the GPU is off — NOTHING read");
    return false;
  }
  // A rectangle partly or wholly off VRAM is refused rather than clamped: a clamped read returns a
  // picture that is mostly real pixels and silently not the artwork asked for, which is exactly the
  // kind of plausible lie that gets drawn on a title screen as somebody's logo.
  if (x < 0 || y < 0 || x + w > VRAM_W || y + h > VRAM_H) {
    lucent::error(
        "gpu_vk", "read_vram_rect: {}x{}+{}+{} is outside VRAM ({}x{}) — NOTHING read", w, h, x, y, VRAM_W, VRAM_H);
    return false;
  }
  const uint16_t *vram = readback_vram(g);
  if (vram == nullptr) {
    return false; // readback_vram already said why, and said it in the log
  }
  for (int row = 0; row < h; ++row) {
    const uint16_t *src = &vram[(y + row) * VRAM_W + x];
    for (int col = 0; col < w; ++col) {
      out[row * w + col] = src[col];
    }
  }
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
  return true;
}
void gpu_vk_present_shot(Core *core, const char *path) {
  core->game->gpu_vk.present_shot(path);
}

void gpu_vk_pump_host_events(Core *core) {
  if (core != nullptr) {
    core->game->hostInput.drainEvents();
  }
}
void gpu_vk_shot_b(Core *core, const char *path) {
  core->game->gpu_vk.shot_b(path);
}
void gpu_vk_frame_end(Core *core, const uint16_t *svram, int frame) {
  core->game->gpu_vk.frame_end(svram, frame);
}
// REPL `preseq` arm (repl.cpp) — creates the dir and arms the per-present dump in present().
void gpu_vk_preseq_arm(Core *core, int n, const char *dir) {
  GpuVkState &s = core->game->gpu_vk;
  snprintf(s.s_preseq_dir, sizeof s.s_preseq_dir, "%s", dir);
  char capture_path[sizeof s.s_preseq_dir + 16];
  snprintf(capture_path, sizeof capture_path, "%s/p0000.ppm", s.s_preseq_dir);
  if (!Fs::ensureParentDirs(capture_path)) {
    lucent::error("preseq", "cannot create the parent directory of {} — capture not armed", capture_path);
    s.s_preseq_left = 0;
    return;
  }
  s.s_preseq_idx = 0;
  s.s_preseq_left = n;
}
// Present index (0-based) that THIS emit pass will dump to `p<idx>.ppm` at frame_end, or -1 when no
// preseq capture is armed. The emit passes (RenderQueue::emitItem) consult this for the `preseqobj`
// per-object motion log so each logged line is keyed to the exact present frame it belongs to.
int gpu_vk_preseq_present_index(Core *core) {
  GpuVkState &s = core->game->gpu_vk;
  return s.s_preseq_left > 0 ? s.s_preseq_idx : -1;
}
