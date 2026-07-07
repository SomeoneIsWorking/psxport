// vram_xfer.cpp — PC-native VRAM TRANSFER GUARD + texture-region registry.
//
// WHY THIS EXISTS
// ---------------
// The port already owns the CPU->VRAM upload library (engine/asset.cpp ov_upload_image == FUN_80081218):
// EVERY texture-group atlas page, font page, and per-frame CLUT funnels through gpu_native_load_image. The
// render path's VRAM->VRAM copy (GP0 0x80) and the (now rarely used) DMA CPU->VRAM stream (GP0 0xA0) are
// the OTHER two VRAM writers, in gpu_native.cpp. All three address VRAM through `vram(x,y)`, which masks
// x & 1023, y & 511. That masking is CORRECT PSX behavior for an in-bounds transfer (PSX VRAM wraps), but
// it is ALSO the amplifier for the non-deterministic "stripe corruption" bug: a transfer fed a GARBAGE
// base/size (a stray render-OT 0x80 copy from a parse desync — the later-72 family — or a bad upload
// descriptor computed by a still-recomp content path under live timing) does not fault or clip; the wrap
// silently FOLDS the bad write onto a live texpage/font/CLUT, and the next draw samples 8bpp index data as
// if it were a different page/palette => vertical colored stripes. Because the trigger is timing-dependent
// (live audio/frame-pacing/async-CD scheduling), it never reproduces in deterministic headless replay, so
// it cannot be caught by a static A/B diff — it must be caught the moment it fires on the running game.
//
// WHAT THIS DOES (one guarded chokepoint for VRAM placement bookkeeping)
//   * vram_register_atlas(x,y,w,h,tag): the native upload registers each big texture-group page it lands as
//     a PROTECTED, populated region. These are exactly the atlas/font/CLUT rectangles the characters and UI
//     sample from. Re-uploads to the same rect refresh in place (the region is re-confirmed live, not stale).
//   * vram_guard_check(path,x,y,w,h,src): every VRAM-writing transfer calls this. It (a) bounds-reports a
//     transfer whose base or extent is OUT OF the 1024x512 VRAM page (the smoking gun of a garbage
//     descriptor — a correct transfer never has an out-of-page base), and (b) under `debug vramguard`, logs
//     any transfer that OVERLAPS a registered, still-resident atlas region but is NOT itself an atlas
//     upload — i.e. a stray copy/load clobbering a live texpage. The log carries the writer path, the rect,
//     the guest source addr, and the current OT node, so the offending write is identified deterministically
//     even though the corruption is rare.
//
// This is PURE DIAGNOSTIC BOOKKEEPING: it does NOT mutate VRAM or guest RAM, so it can never perturb the
// content/interface state (one behavior, no env A/B gate; `debug vramguard` is a diagnostic channel, not a
// behavior toggle, exactly like the existing texwatch/clobber/clutwatch channels). The transfer itself is
// performed by the caller as before; the guard only validates + reports. The user (who CAN reproduce the
// live corruption) enables `debug vramguard`, plays until the stripes appear, and the first
// `[vramguard] CLOBBER ...` line names the exact write that hit the atlas — turning a rare visual into a
// deterministic, pinpointed root cause.
#include "core.h"
#include "cfg.h"
#include "gpu_native_internal.h"
#include <stdio.h>
#include <string.h>

// A transfer is "in page" iff its whole rect lies inside the 1024x512 VRAM. PSX transfers CAN legitimately
// wrap, but the engine's own atlas/render transfers never do, so an out-of-page base/extent flags a bad
// descriptor (the corruption vector). w<=0 / h<=0 are no-ops the callers already skip; treat as in-page.
static inline int rect_in_page(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return 1;
  return x >= 0 && y >= 0 && x + w <= VRAM_W && y + h <= VRAM_H;
}
static inline int rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

void GpuState::vram_register_atlas(int x, int y, int w, int h, const char* tag) {
  if (w <= 0 || h <= 0) return;
  // Refresh an existing region with the same origin in place (the game re-uploads pages each area load).
  for (int i = 0; i < s_vg_n; i++) {
    if (s_vg[i].x == x && s_vg[i].y == y) {
      s_vg[i].w = w; s_vg[i].h = h; s_vg[i].frame = s_frame; s_vg[i].live = 1;
      snprintf(s_vg[i].tag, sizeof s_vg[i].tag, "%s", tag ? tag : "tex");
      return;
    }
  }
  if (cfg_dbg("vramguard")) fprintf(stderr, "[vramguard] register %s (%d,%d %dx%d) [%d protected]\n",
                                    tag ? tag : "tex", x, y, w, h, s_vg_n + 1);
  if (s_vg_n >= VG_MAX) {
    // Full: recycle the oldest slot (the registry is a moving window of currently-resident pages).
    int oldest = 0; for (int i = 1; i < s_vg_n; i++) if (s_vg[i].frame < s_vg[oldest].frame) oldest = i;
    s_vg[oldest] = VgRegion{x, y, w, h, s_frame, 1, {0}};
    snprintf(s_vg[oldest].tag, sizeof s_vg[oldest].tag, "%s", tag ? tag : "tex");
    return;
  }
  VgRegion& r = s_vg[s_vg_n++];
  r.x = x; r.y = y; r.w = w; r.h = h; r.frame = s_frame; r.live = 1;
  snprintf(r.tag, sizeof r.tag, "%s", tag ? tag : "tex");
}

void GpuState::vram_guard_check(Core* core, const char* path, int x, int y, int w, int h, uint32_t src) {
  if (!cfg_dbg("vramguard")) return;
  if (w <= 0 || h <= 0) return;

  // (a) Out-of-page base/extent: a correct atlas/render transfer is always wholly inside VRAM; an
  // out-of-page rect is a garbage descriptor that the vram() wrap would silently fold onto live VRAM.
  if (!rect_in_page(x, y, w, h)) {
    if (s_vg_oob_log++ < 40)
      fprintf(stderr, "[vramguard] OUT-OF-PAGE %s f%d rect=(%d,%d %dx%d) src=0x%08X node=0x%08X "
              "-> wraps onto VRAM (likely the clobber vector)\n",
              path, s_frame, x, y, w, h, src, s_cur_node);
  }

  // (b) Clobber of a registered, resident atlas region by a NON-atlas writer. Atlas uploads themselves
  // (path "native") legitimately (re)write these regions — they are how the region got registered; skip
  // them. Any OTHER path (a render-OT 0x80 VRAM->VRAM copy, a 0xA0 DMA load) that lands on a live texpage
  // is the corruption: it overwrites pixels a draw will sample as the wrong page/palette = stripes.
  int is_atlas_upload = (path && path[0] == 'n');   // "native" upload re-fills its own region
  if (is_atlas_upload) return;
  for (int i = 0; i < s_vg_n; i++) {
    if (!s_vg[i].live) continue;
    if (rects_overlap(x, y, w, h, s_vg[i].x, s_vg[i].y, s_vg[i].w, s_vg[i].h)) {
      if (s_vg_clobber_log++ < 80)
        fprintf(stderr, "[vramguard] CLOBBER %s f%d rect=(%d,%d %dx%d) HITS atlas[%s] "
                "(%d,%d %dx%d) src=0x%08X node=0x%08X\n",
                path, s_frame, x, y, w, h, s_vg[i].tag,
                s_vg[i].x, s_vg[i].y, s_vg[i].w, s_vg[i].h, src, s_cur_node);
      return;
    }
  }
}
