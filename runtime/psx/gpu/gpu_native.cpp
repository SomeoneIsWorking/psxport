#include "c_subsys.h"
#include "core.h"
#include "game.h"
#include "gpu_vk.h" // Core*-threaded VK present API (de-globalized R2)
// Native GPU — PC rendering of the game's own draw primitives (NOT PSX-GPU emulation).
//
// The game emits GP0 command packets (polygons/sprites/lines + VRAM transfers + draw-env)
// as its output protocol, usually via GPU DMA (channel 2) walking ordering-table linked
// lists. We parse that stream and rasterize it with our OWN renderer into a VRAM-backed
// framebuffer, then present it. No PSX GPU hardware is emulated; the renderer is ours, so
// resolution/widescreen/60fps are under our control (fps60 tier builds on this).
//
// VRAM is 1024x512 16-bit (5-5-5 BGR + mask), holding both textures (sampled by textured
// primitives via texpage+CLUT) and the framebuffer regions the game composes & displays.
#include "cfg.h"
#include "config_vars.h"
#include "display_scanout.h"
#include "field_rate.h"  // THE display field rate, in milli-hertz (one definition)
#include "fs_util.h"     // host diagnostic-output directory creation
#include "gp0_command.h" // the guest's GP0 word, decoded by field name
#include "gpu_device.h"
#include "gpu_native_internal.h" // shared VRAM/state/helpers (also used by gpu_debug.cpp)
#include "gpu_primitive_dump.h"  // primitive-census CSV diagnostic owner
#include "host_backtrace.h"
#include "image_writer.h"   // one checked RGB24 capture-file boundary
#include "ordering_table.h" // the guest DrawOTag chain: node header decoded by field name
#include "r3000.h"
#include "wide_2d_layout.h" // the presented width, whichever widening mechanism owns it
#include <lucent/log.h>

#include "mods.h"             // g_mods.fps60 (was g_fps60_on)
#include "render_substrate.h" // Render::mDbgRenderNode (was g_dbg_render_node)
#include "scea_asset.h"       // baked SCEA license-screen texture+CLUT (PC-native boot splash)
#include "vram_pixel.h"       // the VRAM halfword channel layout, in named pieces
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#endif

// g_fps60_on retired — read g_mods.fps60 (mods.h; #included above)
// VRAM_W/VRAM_H and vram() now live in gpu_native_internal.h

namespace {
// The GP0 command FIFO's capacity, and the number of words held back from it. The capacity is the
// hardware's own command-FIFO depth rounded up for the longest packet this port assembles; the
// headroom is what a poly-line with no terminator can never consume, so a malformed guest stream is
// refused before it can run off the end of the buffer rather than after.
constexpr int kGp0FifoCapacity = 256; // GpuState::s_fifo / s_fifo_addr (gpu_native_internal.h)
constexpr int kGp0FifoHeadroom = 6;   // the longest fixed command's words, plus slack
// The most vertices one poly-line may contribute. This is the PORT's limit, not the guest's: the
// hardware's poly-line length is not decoded, so a longer list is truncated here. Stated so a reader
// does not mistake it for a hardware constant.
constexpr int kMaxPolyLineVertices = 64;
} // namespace

// The guest address of a main-RAM offset, from the one owner that defines the mapping. Used wherever
// this file prints an address the guest itself wrote, so a diagnostic names a location guest code can
// go and read rather than a bare offset.
using psx::gpu::guestAddressOf;

// ---- Draw state (set by GP0 env commands E1..E6) ------------------------------------
int gpu_vk_enabled(void); // gpu_vk.c (declared early for the gp0 tee)

// ---- Display control (GP1) ----------------------------------------------------------

// s_prims moved to GpuState (deglobalize 2026-07-03) — was cross-core-shared per-frame draw counter.
// s_seen3d / s_prev_had3d are per-instance GpuState members now (gpu_native_internal.h). The OT walk
// tees geometry before present, so s_seen3d is final by the time the present/compose path queries it.
// A frame with no tee'd 3D is a VRAM-resident 2D screen (SCEA/FMV/title/menu) — nothing in the scratch FB.
int gpu_had3d_last_frame(Core *core) {
  return core->game->gpu.s_prev_had3d;
}
// KEPT DESPITE having no in-framework caller: Tomba2Engine's render_walk.cpp calls it, declaring it
// LOCALLY (`int gpu_seen3d_this_frame(Core *);` at its call site) rather than including a framework
// header. A grep of psxport's headers therefore finds no caller and reads as dead — which is exactly
// how the GpuState de-globalization deleted it and broke the reference consumer's link. A consumer's
// local forward declaration is invisible to a framework-side caller audit; check the game trees too.
int gpu_seen3d_this_frame(Core *core) {
  return core->game->gpu.s_seen3d;
}
// Backdrop-vs-HUD for a screen-space 2D prim — by SCREEN COVERAGE, which is order-independent. The water/
// sky backdrop is a FULL-SCREEN 2D layer (it tiles the whole display); HUD/text/icons cover a small part.
// So a 2D prim spanning most of the display is a backdrop (far depth band, behind the world); anything
// smaller is HUD (overlay band, over the world). This is what the PSXPORT_PRIMDUMP capture showed: the
// backdrop = full-frame sprites (op 0x65, x[0..320] y[~0..232]); HUD = small polys/sprites. It does NOT
// depend on draw order, which matters because the PC-native renderer draws world geometry EAGERLY during
// the per-object flush (before the OT walk) — so the old "no 3D teed yet => backdrop" rule mislabeled the
// ocean as HUD and painted it over everything. `bx0..by1` = the prim's screen-space bbox (pre-offset).
int GpuState::bg_2d(int bx0, int by0, int bx1, int by1) {
  int dw = s_disp_w > 0 ? s_disp_w : 320, dh = s_disp_h > 0 ? s_disp_h : 240;
  int w = bx1 - bx0, h = by1 - by0;
  return (w * 4 >= dw * 3) && (h * 4 >= dh * 3); // covers >=3/4 of the display in both axes = backdrop
}
// FULL-SCREEN PSX-OVERLAY coverage test (issue #21). NOTE (FADE ownership, 2026-06-25): the cutscene/area
// SCREEN-FADE is NO LONGER delivered as a PSX OT rect — it is engine-owned (class ScreenFade, applied in
// present.frag + the headless readback), so it never reaches this path. This test now serves ONLY the
// RESIDUAL genuinely-PSX full-screen semi overlays still emitted as OT rects (the slot-0x74 transition/wipe
// effect FUN_80034548 0x404040, and a pause-menu dim if it fires): such a near-full-screen SEMI prim must
// NOT be a backdrop (it composites OVER the world, topmost band) and its COVERAGE must span the whole WIDE
// framebuffer (else widescreen leaves undimmed margins). Returns 1 for a full-screen SEMI prim; the 2D-X
// mapping then stretches it to fill the wide FB while the layer/ordering keeps it on top. Keep this minimal
// guard until those residual PSX overlays are owned PC-native too. dims passed in (GpuState decl is in
// gpu_native_internal.h, not editable here).
static int fade_full_2d(int dw, int dh, int bx0, int by0, int bx1, int by1) {
  if (dw <= 0) {
    dw = 320;
  }
  if (dh <= 0) {
    dh = 240;
  }
  int w = bx1 - bx0, h = by1 - by0;
  return (w * 4 >= dw * 3) && (h * 4 >= dh * 3); // full-screen (>=3/4 both axes)
}
// M3 provenance: record [lo,hi) (KSEG0 packet-pool addresses) as a BACKGROUND drawer's output for the
// current frame. Stamps the frame so a stale span from a prior frame is never honored.
void GpuState::bg_range_add(uint32_t lo, uint32_t hi) {
  if (hi <= lo) {
    return;
  }
  if (s_bg_frame != s_frame) {
    s_bg_nrange = 0;
    s_bg_frame = s_frame;
  } // new frame -> clear prior spans
  if (s_bg_nrange < BG_RANGE_MAX) {
    s_bg_lo[s_bg_nrange] = lo;
    s_bg_hi[s_bg_nrange] = hi;
    s_bg_nrange++;
  }
}
// Is this OT node inside a background drawer's span recorded THIS frame? (provenance backdrop test)
int GpuState::node_is_bg(uint32_t node) {
  if (s_bg_frame != s_frame) {
    return 0;
  }
  uint32_t n = node | 0x80000000u;
  for (int i = 0; i < s_bg_nrange; i++) {
    if (n >= s_bg_lo[i] && n < s_bg_hi[i]) {
      return 1;
    }
  }
  return 0;
}
// Public wrapper: the engine's background-drawer override (submit.cpp ov_bg_tilemap) records the
// pool span it produced so the OT-walk classifies those prims as RQ_BACKGROUND by provenance.
void gpu_bg_range_add(Core *core, uint32_t lo, uint32_t hi) {
  core->game->gpu.bg_range_add(lo, hi);
}

// TEXPAGE-PROVENANCE backdrop test (replaces the dead packet-span ov_bg_tilemap provenance). The native
// backdrop drawer (submit.cpp ov_bg_tilemap_native) publishes the active sky/sea tilemap texpage
// here each frame; the OT-walk then recognizes the GUEST background drawer's redundant tiles (same texpage)
// and classifies them RQ_BACKGROUND so the field's 2D-only walk DROPS them (the native backdrop owns the
// sky/sea). Stamped per frame so a stale value from a prior frame/area is never honored. (render.md OPEN #1)
void gpu_bg_texpage_set(Core *core, int tp_x, int tp_y) {
  GpuState &s = core->game->gpu;
  s.s_bgtp_x = tp_x;
  s.s_bgtp_y = tp_y;
  s.s_bgtp_frame = s.s_frame;
}
// Does this sprite's texpage match THIS frame's published backdrop texpage? (redundant guest backdrop tile)
static int sprite_is_bg_texpage(Core *core, int tp_x, int tp_y) {
  GpuState &s = core->game->gpu;
  return s.s_bgtp_frame == s.s_frame && tp_x == s.s_bgtp_x && tp_y == s.s_bgtp_y;
}

// s_gp0_words / s_dma2 moved to GpuState (per-Core; was cross-core-shared per-frame diag).
// g_nd_3d/nd_2d retired 2026-07-03 — Render::stats.nd3d/nd2d (RenderStats).

// Engine-owned 2D WIDESCREEN layout. The wide 3D world is centered in the scratch FB by fb_x0=margin*ss
// (push_wide); 2D prims share that relocation shader, so they get the same +margin. We map each native-320
// screen X to the pre-shader local X so that, AFTER the shader adds margin, the element lands anchored:
//   backdrop (full-screen) -> STRETCH to fill the wide FB (no gaps);
//   left-anchored element   -> hug the wide LEFT edge (native size preserved);
//   right-anchored element  -> hug the wide RIGHT edge (native size preserved);
//   center-anchored element -> shift by margin, registering with the centered 3D world.
// This replaces the old uniform stretch-about-x0 (which distorted + mis-anchored every HUD element).
//
// ANCHOR by the element's CENTER, not its edge. The old EDGE=48 band classified by whether either
// bbox edge reached the screen edge; an element whose left edge sat at x<=48 but whose body was mid-
// screen got dragged to the wide-left edge, and any element animating across x=48 / x=272 FLIPPED
// anchor class frame-to-frame -> visible jump. Classifying by the bbox center into native-320 thirds
// is stable (no straddle flip) and matches real HUD intent: corner/edge HUD hugs its side, a centered
// prompt/meter stays centered with the world. The whole element shifts by one offset (its size and
// internal layout are preserved exactly — no stretch), so multi-vertex prims stay rigid.
int gpu_vk_wide_engine_w(Core *);
// 2D X mapping for widescreen. Unlike the old VK renderer, the SDL_GPU tritex.vert does NOT add a
// per-vertex fb_x0=margin (there is no scaled scratch FB in Pass 1 — geometry is in absolute VRAM px).
// So this mapping must place 2D prims into the wide [0,ww) band itself:
//   backdrop-> STRETCH the native-320 span to fill the whole wide FB: x*ww/320 -> [0,ww). Sky/water tiles
//              then cover the full wide frame (no VRAM-atlas garbage in the wide margins).
//   HUD/UI  -> CENTER the native-320 element: x + margin, so a native-x lands in the centered [margin,
//              margin+320] band (matches 4:3, just centered). (Was identity, which left HUD left-anchored.)
// In 4:3 margin==0 -> backdrop x*320/320=x, HUD x+0=x — byte-identical.
static int ws_2d_local_x(Core *core, int x, int is_bg) {
  // RELATIVE TO THIS GAME'S OWN 4:3 WIDTH, not to 320. The 320 this used to hardcode is the fourth
  // instance of that assumption in the widescreen path (after wide_native_w, the GP0 E4 draw-area
  // widen, and the display blank), and here it is the most visible: for a 512-wide game it centred
  // HUD elements by (ww-320)/2 = 182 columns instead of 86, shoving them off the right edge, and
  // stretched backdrops by ww/320 = 2.14x instead of ww/512 = 1.34x. Identical at 320.
  const int native = core->game->gpu.s_disp_w > 0 ? core->game->gpu.s_disp_w : 320;
  int ww = gpu_vk_wide_engine_w(core), margin = (ww - native) / 2;
  if (margin <= 0) {
    return x; // 4:3 -> no-op
  }
  if (is_bg) {
    return x * ww / native; // backdrop: stretch to fill [0,ww)
  }
  return x + margin; // HUD: center the native-width element in the wide FB
}

// Fade-flash diagnostic (PSXPORT_FADEDBG="a:b"): per-frame max emitted prim brightness + how the
// scene is drawn, to settle whether a bright fade frame is in the GP0 (engine emits it) or invented
// by VK. Works identically under SW and VK (same tee'd colors), so one playthrough pins the locus.
void GpuState::fade_note(int r, int g, int b, int offy, int semi) {
  int m = r > g ? r : g;
  if (b > m) {
    m = b;
  }
  if (m > s_fade_maxc) {
    s_fade_maxc = m;
  }
  s_fade_npoly++;
  if (semi) {
    s_fade_nsemi++;
    if (m > s_fade_semimax) {
      s_fade_semimax = m;
    }
    if (m < s_fade_semimin) {
      s_fade_semimin = m;
    }
  }
  s_fade_lasty = offy;
}
// flag a semi prim wider than ~half the screen (a full-screen fade overlay tile)
void GpuState::fade_note_size(int w, int h, int semi) {
  if (semi && w >= 160 && h >= 120) {
    s_fade_bigsemi++;
  }
}
// PSXPORT_SEMIDUMP=frame: log each SEMI prim (blend mode + color + bbox) at `frame`, to see how the
// fade overlay tiles stack (VK draws them all vs one snapshot, so stacked tiles don't accumulate).
void GpuState::semi_dump(const char *kind, int blend, int r, int g, int b, int x0, int y0, int x1, int y1, int offy) {
  static int sf = -2;
  if (sf == -2) {
    const char *e = cfg_str("PSXPORT_SEMIDUMP");
    sf = e ? atoi(e) : -1;
  }
  if (sf >= 0 && s_frame == sf) {
    lucent::info("semidump",
                 "f{} {} blend={} col=({},{},{}) bbox=({},{})-({},{}) offY={}",
                 s_frame,
                 kind,
                 blend,
                 r,
                 g,
                 b,
                 x0,
                 y0,
                 x1,
                 y1,
                 offy);
  }
}

// ---- GP0 command FIFO ---------------------------------------------------------------
// VRAM transfer state (GP0 0xA0 CPU->VRAM)

// PC-native CPU->VRAM upload. The game's libgs-style upload library (FUN_80081218 and the
// GsSortObject ring at 0x800A5AC8) is replaced by writing the rect directly here, so the GPU
// library does not need to be a faithful guest. `src` is a RAM (or physical) address holding
// w*h contiguous 16-bit pixels, row-major; mem_r16 masks the region so KSEG0/physical both work.
// Identical effect to the GP0 0xA0 stream below, minus the FIFO/DMA round-trip.
// Transplant harness: overwrite our full VRAM from a raw 1024x512x16 dump (oracle's, via
// PSXPORT_VRAMDUMP). Lets us drop the oracle's clean green-field VRAM into our running port and
// watch whether our continued execution keeps it clean or re-corrupts (accumulation test).
int GpuState::gpu_native_load_vram(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  size_t n = fread(s_vram, 2, (size_t)VRAM_W * VRAM_H, f);
  fclose(f);
  lucent::info("transplant", "loaded VRAM {} px from {}", n, path ? path : "(null)");
  return n == (size_t)VRAM_W * VRAM_H;
}
void GpuState::gpu_native_load_image(
    Core *core, int x, int y, int w, int h, uint32_t src, std::span<const uint16_t> pixels) {
  // VRAM-transfer guard: bounds-report + atlas-clobber catch (vram_xfer.cpp, `debug vramguard`). This is
  // the single CPU->VRAM upload chokepoint for every texture-group page, font page, and CLUT, so it is
  // both the place to REGISTER the protected atlas regions and a transfer to validate. Diagnostic only.
  vram_guard_check("native", x, y, w, h, src);
  // Register texture-region uploads (anything right of the two 320-wide framebuffers) as a protected,
  // resident page: these are exactly the atlas/font/CLUT rects a later draw samples; a non-upload write
  // landing on one is the stripe-corruption clobber. Framebuffer uploads (x<320) are NOT atlas data.
  if (x >= 320) {
    vram_register_atlas(x, y, w, h, (w <= 16) ? "clut" : "atlas");
  }
  for (int v = 0; v < h; v++) {
    for (int u = 0; u < w; u++) {
      *vram(x + u, y + v) = pixels[(size_t)v * w + u];
    }
  }
  // Mirror the upload into the VK VRAM image, exactly like the GP0 0xA0 / VRAM-copy / fill paths.
  // This native upload is a VRAM-writing path too; without the mirror its textures land only in the
  // SW s_vram. The VK opaque pass samples a full s_vram snapshot so it still saw them, but the VK
  // SEMI pass samples the post-opaque s_tex (dirty regions only) — so a SEMI-transparent textured
  // prim whose texture arrived here read zeros and discarded (the invisible in-game puddle water).
  if (gpu_vk_enabled()) {
    gpu_vk_dirty(core, x, y, w, h);
  }
  // TEXWATCH coverage: this is the third VRAM writer and the watch did not see it either (see the
  // FILL path). Completing the set is what makes a texwatch NEGATIVE mean "nothing wrote here".
  if (texwatch_overlap(x, y, w, h)) {
    lucent::info(
        "texwatch", "f{} NATIVE dest=({},{}) {}x{} src=0x{:08X} first={:04X}", s_frame, x, y, w, h, src, pixels[0]);
  }
  lucent::debug("upload", "f{} NATIVE dest=({},{}) {}x{} src=0x{:08X}", s_frame, x, y, w, h, src);
}

void GpuState::set_texpage(uint16_t tp, TexPageFrom from) {
  s_tp_x = (tp & 0xF) * 64;
  s_tp_y = ((tp >> 4) & 1) * 256;
  s_tp_blend = (tp >> 5) & 3;
  s_tp_mode = (tp >> 7) & 3;
  if (s_tp_mode > 2) {
    s_tp_mode = 2;
  }
  // ONLY GP0(0xE1). beetle's Command_DrawMode calls SetTPage(cmdw) and THEN assigns dtd; SetTPage
  // itself — the path a primitive's embedded word takes — never touches it. See TexPageFrom.
  if (from == TexPageFrom::DrawMode) {
    s_tp_dither = (tp >> 9) & 1; // ordered 4x4 dither enable
  }
  // A PAGE A DRAW SAMPLES IS, BY DEFINITION, LIVE ATLAS. That is the port-agnostic registration the
  // guard needs: it asks the game what it READS rather than guessing from where an upload landed, so
  // it works for a game that uploads through GP0(0xA0) (Spider-Man) exactly as for one that uses the
  // framework's native upload path (Tomba!2). Registering on the texpage CHANGE, not per texel, keeps
  // it to a handful of calls per frame; re-registration refreshes in place.
  //
  // VRAM FOOTPRINT OF A PAGE, by colour mode — a page is 256 texels wide, and a VRAM cell is 16 bits,
  // so 4bpp packs 4 texels per cell (64 cells), 8bpp packs 2 (128), and 15bpp is 1:1 (256).
  const int page_w = (s_tp_mode == 0) ? 64 : (s_tp_mode == 1) ? 128 : 256;
  vram_register_sampled(s_tp_x, s_tp_y, page_w, 256, "texpage");
}
void GpuState::set_clut(uint16_t cl) {
  s_clut_x = (cl & 0x3F) * 16;
  s_clut_y = (cl >> 6) & 0x1FF;
  // A CLUT is one VRAM row: 16 entries for 4bpp, 256 for 8bpp. The mode is whatever the current
  // texpage selected; a 15bpp page samples no CLUT at all, so nothing is registered for it.
  if (s_tp_mode < 2) {
    vram_register_sampled(s_clut_x, s_clut_y, s_tp_mode == 0 ? 16 : 256, 1, "clut");
  }
}

// RenderQueue::emitOrQueue (game/render/render_queue.cpp) is the one place this file's guest
// GP0/OT-walk poly + sprite submit paths (below) funnel their queued items through.

// PSXPORT_CLUTWATCH[=x,y] — log every VRAM upload whose rect covers a watched CLUT point
// (default 880,507 = the wrong grass palette found via the oracle compare, journal later 39),
// in order, with the resulting 16-entry palette. Reveals whether the right palette is written
// then overwritten, or never written, and by which transfer.
void GpuState::clutwatch_dump(const char *tag, int rx, int ry, int rw, int rh) {
  lucent::Line ln;
  ln.add("{} f{} rect=({},{} {}x{}) covers ({},{}) palette:", tag, s_frame, rx, ry, rw, rh, s_cw_x, s_cw_y);
  for (int k = 0; k < 16; k++) {
    ln.add(" {:04X}", *vram(s_cw_x + k, s_cw_y));
  }
  ln.flush(lucent::Level::Info, "clutwatch");
}
int GpuState::clutwatch_covers(int rx, int ry, int rw, int rh) {
  if (s_cw_x < 0) {
    return 0;
  }
  return s_cw_y >= ry && s_cw_y < ry + rh && s_cw_x >= rx && s_cw_x < rx + rw;
}
// For 0xA0 the pixels stream in AFTER setup, so mark pending and dump on completion; for 0x80 the
// copy already happened, so dump immediately.
void GpuState::clutwatch_xfer(const char *tag, int rx, int ry, int rw, int rh) {
  if (!clutwatch_covers(rx, ry, rw, rh)) {
    return;
  }
  if (tag[0] == 'A') {
    s_cw_pending = 1;
    lucent::info(
        "clutwatch", "A0 upload START f{} rect=({},{} {}x{}) covers ({},{})", s_frame, rx, ry, rw, rh, s_cw_x, s_cw_y);
  } else {
    clutwatch_dump(tag, rx, ry, rw, rh);
  }
}

// PSXPORT_TEXWATCH="x0,y0,x1,y1" — log every A0 CPU->VRAM upload or 0x80 VRAM->VRAM copy whose
// DEST rect overlaps the watched VRAM rect (e.g. a character's texpage), with frame, dest rect,
// DMA source addr, and the first source/dest bytes. Traces exactly when a model's texture pixels
// change and what data fed them (gameplay sprite/CLUT corruption hunt).
int GpuState::texwatch_overlap(int rx, int ry, int rw, int rh) {
  if (!s_tw_init) {
    s_tw_init = 1;
    const char *e = cfg_str("PSXPORT_TEXWATCH");
    if (e) {
      sscanf(e, "%d,%d,%d,%d", &s_tw_x0, &s_tw_y0, &s_tw_x1, &s_tw_y1);
    }
  }
  if (s_tw_x0 < 0) {
    return 0;
  }
  return rx < s_tw_x1 && rx + rw > s_tw_x0 && ry < s_tw_y1 && ry + rh > s_tw_y0;
}

// Record one payload halfword of a watched CPU->VRAM transfer (see gpu_native_internal.h).
void GpuState::twp_note(uint16_t v) {
  s_twp_px++;
  for (int i = 0; i < s_twp_nvals; i++) {
    if (s_twp_vals[i] == v) {
      return;
    }
  }
  if (s_twp_nvals < 8) {
    s_twp_vals[s_twp_nvals++] = v;
  } else {
    s_twp_more = 1;
  }
}

// Emit the payload summary for a watched transfer. Unconditional for a watched transfer: the
// UNIFORM case (distinct=1) is the answer this instrument exists to be able to state, so it must
// never be the branch that prints nothing.
void GpuState::twp_flush(const char *tag) {
  if (!s_twp_active) {
    return;
  }
  lucent::Line ln;
  ln.add("f{} {} PAYLOAD dest=({},{}) {}x{} px={}/{} firstwordaddr=0x{:08X} distinct={}{}:",
         s_frame,
         tag,
         s_xfer_x,
         s_xfer_y,
         s_xfer_w,
         s_xfer_h,
         s_twp_px,
         (long)s_xfer_w * s_xfer_h,
         s_twp_addr0,
         s_twp_nvals,
         s_twp_more ? "+" : "");
  for (int i = 0; i < s_twp_nvals; i++) {
    ln.add(" {:04X}", s_twp_vals[i]);
  }
  ln.flush(lucent::Level::Info, "texwatch");
  s_twp_active = 0;
  s_twp_px = 0;
  s_twp_nvals = 0;
  s_twp_more = 0;
  s_twp_addr0 = 0;
}

// PC-native texture EXPORT (proves the texture DECODE is owned, not the PSX's). Decodes a w×h block of
// texels at the CURRENT texpage (s_tp_x/y, s_tp_mode) through the CURRENT CLUT (s_clut_x/y) with MY OWN
// decoder — the same CLUT/bit-depth logic as sample_tex but standalone — and writes an RGB PPM to
// scratch/export/. The texels come from VRAM that the PC-owned upload chain (lz_decompress → group unpack
// → ov_upload_image) filled, so neither the decompression nor the CLUT decode runs PSX code.
void GpuState::tex_export(const char *name, int u0, int v0, int w, int h) {
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
    return;
  }
  char dir[] = "scratch/export";
  char path[256];
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("texexport", "cannot create the parent directory of {} — NOTHING written", path);
    return;
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int dy = 0; dy < h; dy++) {
    for (int dx = 0; dx < w; dx++) {
      int u = u0 + dx, v = v0 + dy;
      uint16_t t;
      if (s_tp_mode == 2) {
        t = *vram(s_tp_x + u, s_tp_y + v); // 15bpp direct
      } else if (s_tp_mode == 1) {         // 8bpp -> CLUT
        uint16_t word = *vram(s_tp_x + (u >> 1), s_tp_y + v);
        int idx = (u & 1) ? (word >> 8) : (word & 0xFF);
        t = *vram(s_clut_x + idx, s_clut_y);
      } else { // 4bpp -> CLUT
        uint16_t word = *vram(s_tp_x + (u >> 2), s_tp_y + v);
        int idx = (word >> ((u & 3) * 4)) & 0xF;
        t = *vram(s_clut_x + idx, s_clut_y);
      }
      unsigned char rgb[3] = {(unsigned char)((t & 31) << 3),
                              (unsigned char)(((t >> 5) & 31) << 3),
                              (unsigned char)(((t >> 10) & 31) << 3)};
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
  lucent::info("texexport",
               "wrote {} ({}x{}, tp=({},{}) mode={} clut=({},{}) uv0=({},{}))",
               path,
               w,
               h,
               s_tp_x,
               s_tp_y,
               s_tp_mode,
               s_clut_x,
               s_clut_y,
               u0,
               v0);
}

// Execute a complete GP0 primitive packet held in s_fifo[0..s_fcount).
void GpuState::gp0_exec(Core *core) {
  // Producer census: everything this function pushes into the native render queue is GUEST-ORIGIN — it is
  // the guest's own GP0 word being executed. Marking it here means the chokepoint counts those prims as
  // guest-origin instead of as an undeclared NATIVE producer, which is a claim that can never be
  // discharged (a guest prim has no native producer) and which invited the one wrong fix: opening a
  // ProducerScope on a guest function. Host-only; writes no guest memory.
  GuestGp0Scope guestGp0(&core->rsub);
  const uint32_t c = s_fifo[0];
  const psx::gpu::Gp0Command command(c);
  const uint8_t op = command.opcodeByte();
  // The owner tag was captured when this packet was written, while the guest producer's call
  // frame still existed.  DMA/OT execution happens after that call returned, so no current-stack
  // lookup could identify it now.  A miss deliberately stays visible: incomplete provenance must
  // never suppress a plausible-but-unrelated packet.
  const bool suppressDraw = core->rsub.guestPacketFilter.suppressesPacket(core->rsub.otAttr, s_fifo_addr[0]);
  if (command.isPolygon()) {
    const psx::gpu::Gp0PrimitiveFlags flags = command.flags();
    const int gouraud = flags.gouraud ? 1 : 0;
    const int quad = flags.quadOrPolyLine ? 1 : 0;
    // NOT const: the PSXPORT_PAINTFG diagnostic below forces a 2D-FG poly to untextured, and a
    // const would make that override a compile error instead of the deliberate it is.
    int textured = flags.textured ? 1 : 0;
    const int semi = flags.semiTransparent ? 1 : 0;
    // bit0 = texture-blend-disable (raw texel, no modulation). A FLAT polygon has no texel to disable,
    // so the flag only means anything when there is a texture to leave alone.
    const int raw = (textured && flags.rawTexel) ? 1 : 0;
    const int nv = command.polygonVertexCount();
    Vtx v[4];
    uint32_t vaddr[4];
    int idx = 1;
    const psx::gpu::Gp0Colour commandColour = command.colour();
    for (int i = 0; i < nv; i++) {
      // A gouraud polygon's FIRST vertex colour is the command word's own colour; every later vertex
      // carries a colour word of its own, which is why this is the one place `idx` moves for colour.
      const psx::gpu::Gp0Colour vertexColour =
          gouraud ? psx::gpu::Gp0Command(i == 0 ? c : s_fifo[idx++]).colour() : commandColour;
      vaddr[i] = s_fifo_addr[idx]; // guest addr of this vertex's XY word (Phase-1 attach)
      const psx::gpu::Gp0VertexPos pos = psx::gpu::Gp0Command(s_fifo[idx++]).vertexPos();
      v[i].x = pos.x;
      v[i].y = pos.y;
      v[i].r = vertexColour.red;
      v[i].g = vertexColour.green;
      v[i].b = vertexColour.blue;
      if (textured) {
        const psx::gpu::Gp0TextureCoord tex = psx::gpu::Gp0Command(s_fifo[idx++]).textureCoord();
        v[i].u = tex.u;
        v[i].v = tex.v;
        // The high halfword means DIFFERENT things at the two ends of the vertex list: vertex 0's is
        // the CLUT this prim samples, vertex 1's is the texpage. Every other vertex repeats vertex 1's
        // texpage in the same slot. Both binds are side effects of DECODING the packet, which is why
        // they sit inside the loop and not in a later "apply state" pass.
        if (i == 0) {
          set_clut(tex.selector);
        }
        if (i == 1) {
          set_texpage(tex.selector, TexPageFrom::Primitive);
        }
      }
    }
    int shade = gouraud || !textured; // flat-untextured uses the command color
    {
      int mr = 0, mg = 0, mb = 0, xmn = 99999, xmx = -99999, ymn = 99999, ymx = -99999;
      for (int i = 0; i < nv; i++) {
        if (v[i].r > mr) {
          mr = v[i].r;
        }
        if (v[i].g > mg) {
          mg = v[i].g;
        }
        if (v[i].b > mb) {
          mb = v[i].b;
        }
        if (v[i].x < xmn) {
          xmn = v[i].x;
        }
        if (v[i].x > xmx) {
          xmx = v[i].x;
        }
        if (v[i].y < ymn) {
          ymn = v[i].y;
        }
        if (v[i].y > ymx) {
          ymx = v[i].y;
        }
      }
      fade_note(mr, mg, mb, s_off_y, semi);
      fade_note_size(xmx - xmn, ymx - ymn, semi);
      if (semi) {
        semi_dump("poly", s_tp_blend, mr, mg, mb, xmn, ymn, xmx, ymx, s_off_y);
      }
    }
    // VK backend (M5): tee polys to the GPU rasterizer in absolute VRAM coords. Opaque textured/
    // untextured -> opaque batch; semi -> semi batch (mode 3 = untextured flat). VK owns these now.
    if (vk_path() && !suppressDraw) {
      unsigned ord_idx = s_prim_order++;
      int xs[4], ys[4], us[4], vs[4];
      unsigned char rs[4], gs[4], bs[4];
      for (int i = 0; i < nv; i++) {
        xs[i] = v[i].x + s_off_x;
        ys[i] = v[i].y + s_off_y;
        us[i] = v[i].u;
        vs[i] = v[i].v;
        rs[i] = v[i].r;
        gs[i] = v[i].g;
        bs[i] = v[i].b;
      }
      int mode = textured ? s_tp_mode : 3, rw = raw ? 1 : 0;
      // Classify 3D-vs-2D (and 2D backdrop-vs-HUD) once; this drives both the inline draw and the engine
      // render queue. Phase 2 (NATIVE_DEPTH): a poly whose every vertex resolves to a projected vertex is
      // 3D world geometry -> carries real per-vertex view-Z (D32 occlusion); otherwise it is a 2D element
      // -> a backdrop (FAR band) or HUD (near band) by screen coverage.
      int use_rq = rq_active(); // engine render queue owns ordering (PSXPORT_RQ)
      float dep[4];
      int is3d = 0, bg = 0;
      int fade_full = 0; // full-screen SEMI overlay (fade/dim) -> stretch-to-fill, stay on top (#21)
      {
        float proj_pz_to_ord(float);
        is3d = 1;
        for (int i = 0; i < nv; i++) {
          float pz;
          if (vaddr[i] && core->rsub.projprim.lookupPz(core, vaddr[i], &pz)) {
            dep[i] = proj_pz_to_ord(pz);
          } else {
            is3d = 0;
            break;
          }
        }
        if (is3d) {
          s_seen3d = 1; // a projected world prim has now been drawn this frame
        }
        int bx0 = xs[0], by0 = ys[0], bx1 = xs[0], by1 = ys[0];
        for (int i = 1; i < nv; i++) {
          if (xs[i] < bx0) {
            bx0 = xs[i];
          }
          if (xs[i] > bx1) {
            bx1 = xs[i];
          }
          if (ys[i] < by0) {
            by0 = ys[i];
          }
          if (ys[i] > by1) {
            by1 = ys[i];
          }
        }
        // 2D prim: backdrop vs HUD. PROVENANCE first — a node produced by the engine's owned background
        // drawer is the backdrop regardless of size (fixes the tiled background, blind to bg_2d's coverage
        // test); fall back to screen-coverage for scenes whose background drawer isn't owned yet.
        // A full-screen prim is a BACKDROP only if OPAQUE (sky/sea). A full-screen SEMI prim is a
        // fade/overlay -> must NOT be a backdrop (else it draws UNDER the world); leave it in the HUD
        // (topmost) band so fades composite on top. (Owned backdrops still match via node_is_bg.)
        if (!is3d) {
          bg = node_is_bg(s_cur_node) || (!semi && bg_2d(bx0, by0, bx1, by1));
        }
        // FADE/DIM (#21): a full-screen SEMI prim is a fade/dim overlay, NOT a backdrop. It must cover the
        // WHOLE wide FB (else green field shows in the widescreen margins) but composite ON TOP (HUD band).
        // Tag it so the 2D-X mapping below stretches it to fill while the layer stays topmost.
        if (!is3d && !bg && semi && fade_full_2d(s_disp_w, s_disp_h, bx0, by0, bx1, by1)) {
          fade_full = 1;
        }
        // Per-primitive, so the channel name is resolved ONCE (lucent::Channel) rather than hashed under
        // a mutex on every 2D prim; the histogram itself is real non-logging work, so the guard stays.
        {
          static const lucent::Channel nd_ch{"ndepth"};
          if (!is3d && nd_ch) { // categorize what lands in the 2D band: op + gouraud/quad/tex
            s_nd2d_hist[op]++;
          }
        }
        if (!is3d && !bg && s_frame == s_primdump_frame) {
          lucent::debug("objz",
                        "[polynode] id={} op={:02x} bbox=({},{})-({},{}) node={:08x}",
                        ord_idx,
                        op,
                        bx0,
                        by0,
                        bx1,
                        by1,
                        s_cur_node);
        }
        // PSXPORT_PRIMDUMP=<frame>: dump every prim (poly) of that frame as an individual PNG (named by its
        // OT-walk ID) so the backdrop can be identified by eye and its band corrected. id=ord_idx.
        {
          gpu_primitive_dump_polygon(core,
                                     s_frame,
                                     ord_idx,
                                     op,
                                     nv,
                                     is3d,
                                     is3d ? -1 : bg,
                                     xs,
                                     ys,
                                     us,
                                     vs,
                                     rs[0],
                                     gs[0],
                                     bs[0],
                                     textured ? 1 : 0,
                                     semi,
                                     rw);
        }
        if (is3d) {
          core->rsub.stats.nd3d++;
          core->rsub.stats.nd3dTotal++;
        } else {
          core->rsub.stats.nd2d++;
          core->rsub.stats.nd2dTotal++;
        }
        // PSXPORT_PRIMAT="x,y" (DISPLAY coords): log EVERY poly whose triangle covers that display pixel,
        // with its 3D/2D classification + per-vertex depth (ord) + node + color, so it sees the actual
        // occlusion contestants. Frontmost opaque =
        // max ord. Tagged f%d so a multi-frame run can be grepped for the shot frame. (diag, 2026-06-24)
        {
          int ax = 0, ay = 0;
          if (pixel_probe_target(ax, ay)) {
            auto intri = [&](int i0, int i1, int i2) {
              return rq_point_in_triangle(ax, ay, xs[i0], ys[i0], xs[i1], ys[i1], xs[i2], ys[i2]);
            };
            int cover = intri(0, 1, 2) || (nv == 4 && intri(1, 2, 3));
            if (cover) {
              static int n = 0;
              if (n++ < 6000) {
                lucent::info("primat",
                             "f{} objnode={:08X} pktnode={:08X} op={:02X} is3d={} bg={} semi={} tex={} mode={} raw={} "
                             "tp=({},{}) clut=({},{}) uv0=({},{}) da=({},{})-({},{}) off=({},{}) col=({},{},{}) "
                             "bbox=({},{})-({},{}) xy=[({},{}) ({},{}) ({},{}) ({},{})]",
                             s_frame,
                             core->rsub.diag.currentNode(),
                             s_cur_node,
                             op,
                             is3d,
                             bg,
                             semi,
                             textured ? 1 : 0,
                             mode,
                             rw,
                             s_tp_x,
                             s_tp_y,
                             s_clut_x,
                             s_clut_y,
                             us[0],
                             vs[0],
                             s_da_x0,
                             s_da_y0,
                             s_da_x1,
                             s_da_y1,
                             s_off_x,
                             s_off_y,
                             rs[0],
                             gs[0],
                             bs[0],
                             bx0,
                             by0,
                             bx1,
                             by1,
                             xs[0],
                             ys[0],
                             xs[1],
                             ys[1],
                             xs[2],
                             ys[2],
                             xs[3],
                             ys[3]);
              }
            }
          }
        }
        // PSXPORT_PAINTFG=1 (diag): force every 2D-FG (HUD-band) poly to opaque solid magenta so we can SEE
        // whether these prims rasterize at all (vs being culled / texture-transparent).
        {
          static int pf = -2;
          if (pf == -2) {
            const char *e = cfg_str("PSXPORT_PAINTFG");
            pf = e ? atoi(e) : 0;
          }
          if (pf && !is3d && !bg) {
            textured = 0;
            mode = 3;
            for (int i = 0; i < nv; i++) {
              rs[i] = 255;
              gs[i] = 0;
              bs[i] = 255;
            }
          }
        }
      }
      // Genuine engine-wide: a poly with is3d==0 is a SCREEN-SPACE 2D element (HUD banner, full-screen
      // overlay) drawn as polys rather than sprites. The 3D world widens via the projection (OFX); these
      // 2D polys would stay left-anchored at 320 (the banner gets cut). Widen them like the 2D sprites:
      // scale the 2D plane uniformly to the wide width about the framebuffer origin so they fill the frame.
      {
        int gpu_vk_wide_engine(Core *);
        // #38: stretch-fill uses PROVENANCE only (node_is_bg), not `bg`'s coverage heuristic — a
        // large screen-space panel (weapon carousel) can exceed bg_2d's >=3/4-screen threshold and get
        // mis-tagged backdrop, bleeding it to the wide-screen edges. `bg` itself (RQ_BACKGROUND depth
        // band) is left untouched.
        int fill = !is3d && (node_is_bg(s_cur_node) || fade_full);
        if (fill) {
          s_seen_bg2d = 1; // #54: this frame owns a full-screen 2D backdrop redraw (menu/title)
        }
        // 2D widen on gameplay frames (3D last frame) OR a full-screen 2D backdrop redraw last frame (#54:
        // a pure-2D screen like the title menu never sets s_prev_had3d, but its own backdrop is just as
        // legitimate a "this frame repaints the whole width" signal).
        if (!is3d && gpu_vk_wide_engine(core) && (s_prev_had3d || s_prev_had_bg2d)) {
          for (int i = 0; i < nv; i++) {
            xs[i] = ws_2d_local_x(core, xs[i], fill);
          }
        }
      } // engine-owned 2D layout
      // DIAG PSXPORT_PAINTER=1: force PURE PSX OT painter order (is3d=0 / no bg split) for EVERY prim, so the
      // frame composites exactly as the PSX ordering table would. Render the field with and without this and
      // diff: the differing pixels are precisely where native per-pixel depth changes the picture (the
      // object-occlusion bug — terrain/atlas not obeying world-depth). Diagnostic only.
      // A PURE RENDER PATH forces this too: PSX painter order is exactly the unenhanced reference (no
      // native per-pixel depth, no bg-band split), so every prim composites in OT order like the real PSX.
      // Keyed on the render path rather than on `game->oracle` — the bundle used to stand in for the
      // question, so `PSXPORT_RENDER_PATH=gte` without ORACLE would have kept native depth in a picture
      // that is meant to be pure.
      {
        static int pm = -2;
        if (pm == -2) {
          const char *e = cfg_str("PSXPORT_PAINTER");
          pm = e ? atoi(e) : 0;
        }
        if (pm || !core->rsub.mode.enhancementsAllowed()) {
          is3d = 0;
          bg = 0;
        }
      }
      if (use_rq) {
        // Engine owns ordering: hand the prim to the render queue tagged with its layer + depth mode.
        int layer = is3d ? RQ_WORLD : (bg ? RQ_BACKGROUND : RQ_HUD);
        int om = is3d ? RQ_OM_DEPTH : (bg ? RQ_OM_2D_BG : RQ_OM_2D_FG);
        // The packet's own vertex words ride with the item: they link a drawn vertex back to the guest
        // projection that produced it, which xs/ys (offset, possibly widened) cannot.
        RqGuestXy guestXy{};
        for (int i = 0; i < nv; i++) {
          guestXy.x[i] = static_cast<int16_t>(v[i].x);
          guestXy.y[i] = static_cast<int16_t>(v[i].y);
        }
        core->game->activeRq().emitOrQueue(core,
                                           1,
                                           layer,
                                           om,
                                           nv,
                                           semi,
                                           rw,
                                           xs,
                                           ys,
                                           0,
                                           0,
                                           us,
                                           vs,
                                           rs,
                                           gs,
                                           bs,
                                           is3d ? dep : 0,
                                           mode,
                                           s_tp_x,
                                           s_tp_y,
                                           s_clut_x,
                                           s_clut_y,
                                           s_tw_mx,
                                           s_tw_my,
                                           s_tw_ox,
                                           s_tw_oy,
                                           s_da_x0,
                                           s_da_y0,
                                           s_da_x1,
                                           s_da_y1,
                                           s_tp_blend,
                                           nullptr,
                                           -1,
                                           0.0f,
                                           0,
                                           0,
                                           {},
                                           s_cur_node,
                                           ord_idx,
                                           &guestXy);
      } else {
        gpu_vk_set_order(core, ord_idx); // OT submission order -> depth (preserve opaque/semi order)
        if (!is3d) {                     // 2D band select
          if (bg) {
            gpu_vk_set_order_2d_bg(core, ord_idx);
          } else {
            gpu_vk_set_order_2d(core, ord_idx);
          }
        }
#define SBS_OR_ND_SETVD(p)                                                                                             \
  do {                                                                                                                 \
    if (is3d)                                                                                                          \
      gpu_vk_set_vd(core, p);                                                                                          \
  } while (0)
        if (semi) {
          { // OT-order grouping (overlap -> fresh fb snapshot)
            int bx0 = xs[0], by0 = ys[0], bx1 = xs[0], by1 = ys[0];
            for (int i = 1; i < nv; i++) {
              if (xs[i] < bx0) {
                bx0 = xs[i];
              }
              if (xs[i] > bx1) {
                bx1 = xs[i];
              }
              if (ys[i] < by0) {
                by0 = ys[i];
              }
              if (ys[i] > by1) {
                by1 = ys[i];
              }
            }
            gpu_vk_semi_group(core, bx0, by0, bx1, by1);
          }
          SBS_OR_ND_SETVD(dep);
          gpu_vk_draw_semi(core,
                           xs,
                           ys,
                           us,
                           vs,
                           rs,
                           gs,
                           bs,
                           s_tp_x,
                           s_tp_y,
                           mode,
                           rw,
                           s_clut_x,
                           s_clut_y,
                           s_tw_mx,
                           s_tw_my,
                           s_tw_ox,
                           s_tw_oy,
                           s_da_x0,
                           s_da_y0,
                           s_da_x1,
                           s_da_y1,
                           s_tp_blend);
          if (nv == 4) {
            SBS_OR_ND_SETVD(&dep[1]);
            gpu_vk_draw_semi(core,
                             &xs[1],
                             &ys[1],
                             &us[1],
                             &vs[1],
                             &rs[1],
                             &gs[1],
                             &bs[1],
                             s_tp_x,
                             s_tp_y,
                             mode,
                             rw,
                             s_clut_x,
                             s_clut_y,
                             s_tw_mx,
                             s_tw_my,
                             s_tw_ox,
                             s_tw_oy,
                             s_da_x0,
                             s_da_y0,
                             s_da_x1,
                             s_da_y1,
                             s_tp_blend);
          }
        } else {
          SBS_OR_ND_SETVD(dep);
          gpu_vk_draw_tritri(core,
                             xs,
                             ys,
                             us,
                             vs,
                             rs,
                             gs,
                             bs,
                             s_tp_x,
                             s_tp_y,
                             mode,
                             rw,
                             s_clut_x,
                             s_clut_y,
                             s_tw_mx,
                             s_tw_my,
                             s_tw_ox,
                             s_tw_oy,
                             s_da_x0,
                             s_da_y0,
                             s_da_x1,
                             s_da_y1);
          if (nv == 4) {
            SBS_OR_ND_SETVD(&dep[1]);
            gpu_vk_draw_tritri(core,
                               &xs[1],
                               &ys[1],
                               &us[1],
                               &vs[1],
                               &rs[1],
                               &gs[1],
                               &bs[1],
                               s_tp_x,
                               s_tp_y,
                               mode,
                               rw,
                               s_clut_x,
                               s_clut_y,
                               s_tw_mx,
                               s_tw_my,
                               s_tw_ox,
                               s_tw_oy,
                               s_da_x0,
                               s_da_y0,
                               s_da_x1,
                               s_da_y1);
          }
        }
#undef SBS_OR_ND_SETVD
      }
    }
    // PSXPORT_POLYDUMP=frame — log every poly at `frame` (our port side, to compare vs oracle
    // polywatch). Finds the garbage-block prims in the GAME level.
    {
      static int pd = -2, pax = -1, pay = -1;
      if (pd == -2) {
        const char *e = cfg_str("PSXPORT_POLYDUMP");
        pd = e ? atoi(e) : -1;
        const char *pa = cfg_str("PSXPORT_POLYAT");
        if (pa) {
          sscanf(pa, "%d,%d", &pax, &pay);
        }
      }
      if (pd >= 0 && s_frame == pd) {
        int hit = (pax < 0); // no point filter -> log all
        if (pax >= 0) {      // log only prims whose screen bbox (incl offset) covers (pax,pay)
          int xmin = 99999, xmax = -99999, ymin = 99999, ymax = -99999;
          for (int i = 0; i < nv; i++) {
            int X = v[i].x + s_off_x, Y = v[i].y + s_off_y;
            if (X < xmin) {
              xmin = X;
            }
            if (X > xmax) {
              xmax = X;
            }
            if (Y < ymin) {
              ymin = Y;
            }
            if (Y > ymax) {
              ymax = Y;
            }
          }
          hit = (pax >= xmin && pax <= xmax && pay >= ymin && pay <= ymax);
        }
        static int n = 0;
        if (hit && n++ < 2000) {
          lucent::info("polydump",
                       "f{} node={:08X} op={:02X} tex={} gou={} clut=({},{}) tp=({},{}) "
                       "cols[({},{},{})({},{},{})({},{},{})({},{},{})] V[({},{})({},{})({},{})({},{})] off=({},{})",
                       s_frame,
                       s_cur_node,
                       op,
                       textured ? 1 : 0,
                       gouraud ? 1 : 0,
                       s_clut_x,
                       s_clut_y,
                       s_tp_x,
                       s_tp_y,
                       v[0].r,
                       v[0].g,
                       v[0].b,
                       v[1].r,
                       v[1].g,
                       v[1].b,
                       v[2].r,
                       v[2].g,
                       v[2].b,
                       v[3].r,
                       v[3].g,
                       v[3].b,
                       v[0].x,
                       v[0].y,
                       v[1].x,
                       v[1].y,
                       v[2].x,
                       v[2].y,
                       v[3].x,
                       v[3].y,
                       s_off_x,
                       s_off_y);
        }
      }
    }
    if (s_reddbg && textured && s_cw_x >= 0 && s_clut_x == s_cw_x && s_clut_y == s_cw_y) {
      static int n = 0;
      if (n++ < 12) {
        lucent::info("redpkt",
                     "f{} stage={:08X} node=0x{:08X} op={:02X} nv={} gou={} semi={} clut=({},{}) tp=({},{}) blend={} "
                     "mode={} V[({},{})uv({},{}) ({},{})uv({},{}) ({},{})uv({},{}){}] off=({},{})",
                     s_frame,
                     // The engine's task-0 entry latch. A TITLE global: its field, its struct, and what a
                     // zero means are all unattributed. Named here so the reader knows the word is a
                     // guest global rather than a GPU register. See
                     // docs/issues/0130-title-globals-in-the-framework.md.
                     core->mem_r32(0x801fe00c),
                     s_cur_node,
                     op,
                     nv,
                     gouraud,
                     semi,
                     s_clut_x,
                     s_clut_y,
                     s_tp_x,
                     s_tp_y,
                     s_tp_blend,
                     s_tp_mode,
                     v[0].x,
                     v[0].y,
                     v[0].u,
                     v[0].v,
                     v[1].x,
                     v[1].y,
                     v[1].u,
                     v[1].v,
                     v[2].x,
                     v[2].y,
                     v[2].u,
                     v[2].v,
                     quad ? " +q" : "",
                     s_off_x,
                     s_off_y);
      }
    }
    s_prims++;
  } else if (command.isRectangleOrSprite()) {
    const psx::gpu::Gp0PrimitiveFlags flags = command.flags();
    const int textured = flags.textured ? 1 : 0;
    const int semi = flags.semiTransparent ? 1 : 0;
    const int size = command.rectangleSizeCode();
    const psx::gpu::Gp0Colour colour = command.colour();
    const uint8_t cr = colour.red, cg = colour.green, cb = colour.blue;
    int idx = 1;
    const psx::gpu::Gp0VertexPos topLeft = psx::gpu::Gp0Command(s_fifo[idx++]).vertexPos();
    const int x = topLeft.x, y = topLeft.y;
    int u0 = 0, v0 = 0;
    if (textured) {
      // A rectangle's texture-coordinate word carries the CLUT in its high halfword. There is no
      // per-vertex texpage on a rectangle: the texpage in force when the command is decoded is the one
      // it samples, which is the whole reason an E1 state node has to precede it in the OT.
      const psx::gpu::Gp0TextureCoord tex = psx::gpu::Gp0Command(s_fifo[idx++]).textureCoord();
      u0 = tex.u;
      v0 = tex.v;
      set_clut(tex.selector);
    }
    int w, h;
    if (size == 0) {
      // Variable size: the width/height word. The width is 10 bits and the height 9, and NEITHER is
      // rounded up to anything — a rectangle is exactly the size the guest asked for.
      const uint32_t wh = s_fifo[idx++];
      w = wh & 0x3FF;
      h = (wh >> 16) & 0x1FF;
    } else {
      w = h = command.rectangleFixedSize();
    }
    // PSXPORT_POLYDUMP (+POLYAT): also log sprites/rects, so the garbage-block source can be a sprite.
    {
      static int pd = -2, pax = -1, pay = -1;
      if (pd == -2) {
        const char *e = cfg_str("PSXPORT_POLYDUMP");
        pd = e ? atoi(e) : -1;
        const char *pa = cfg_str("PSXPORT_POLYAT");
        if (pa) {
          sscanf(pa, "%d,%d", &pax, &pay);
        }
      }
      if (pd >= 0 && s_frame == pd) {
        int X = x + s_off_x, Y = y + s_off_y;
        int hit = (pax < 0) || (pax >= X && pax < X + w && pay >= Y && pay < Y + h);
        static int n = 0;
        if (hit && n++ < 2000) {
          lucent::info("polydump",
                       "f{} node={:08X} SPRITE op={:02X} tex={} semi={} clut=({},{}) tp=({},{}) col=({},{},{}) "
                       "at=({},{}) {}x{} uv0=({},{}) off=({},{})",
                       s_frame,
                       s_cur_node,
                       op,
                       textured ? 1 : 0,
                       semi,
                       s_clut_x,
                       s_clut_y,
                       s_tp_x,
                       s_tp_y,
                       cr,
                       cg,
                       cb,
                       x,
                       y,
                       w,
                       h,
                       u0,
                       v0,
                       s_off_x,
                       s_off_y);
        }
      }
    }
    // PSXPORT_TEXEXPORT=<frame> — export the texture of each large textured sprite (backgrounds) on that
    // frame via the PC-native decoder. The menu/sea backdrops are big sprites; this writes their decoded
    // pixels to scratch/export/*.ppm with no PSX code in the decode path.
    {
      static int tex_f = -2;
      if (tex_f == -2) {
        const char *e = cfg_str("PSXPORT_TEXEXPORT");
        tex_f = e ? atoi(e) : -1;
      }
      if (tex_f >= 0 && s_frame == tex_f && textured) {
        // Backgrounds (menu/sea) are 16×16 TILEMAPS sampling a shared atlas texpage. Export the whole
        // atlas (256×256 texels at the texpage origin) ONCE per unique (texpage,clut,mode), not per tile.
        static int seen_tpx[64], seen_tpy[64], seen_clx[64], seen_cly[64], nseen = 0;
        int dup = 0;
        for (int k = 0; k < nseen; k++) {
          if (seen_tpx[k] == s_tp_x && seen_tpy[k] == s_tp_y && seen_clx[k] == s_clut_x && seen_cly[k] == s_clut_y) {
            dup = 1;
            break;
          }
        }
        if (!dup && nseen < 64) {
          seen_tpx[nseen] = s_tp_x;
          seen_tpy[nseen] = s_tp_y;
          seen_clx[nseen] = s_clut_x;
          seen_cly[nseen] = s_clut_y;
          nseen++;
          char nm[96];
          snprintf(nm, sizeof nm, "atlas_tp%d_%d_clut%d_%d_m%d", s_tp_x, s_tp_y, s_clut_x, s_clut_y, s_tp_mode);
          tex_export(nm, 0, 0, 256, 256);
        }
      }
    }
    fade_note(cr, cg, cb, s_off_y, semi);
    fade_note_size(w, h, semi);
    if (semi) {
      semi_dump("sprite", s_tp_blend, cr, cg, cb, x, y, x + w, y + h, s_off_y);
    }
    // VK backend (M5): tee rects/sprites as two triangles (opaque or semi; mode 3 = untextured solid).
    if (vk_path() && !suppressDraw) {
      unsigned ord_idx = s_prim_order++;
      // sprites/rects are screen-space (no GTE projection) -> 2D backdrop/HUD band by screen coverage.
      int use_rq = rq_active();
      // PROVENANCE first (owned background drawer -> backdrop, any size), coverage fallback otherwise.
      // A full-screen SEMI sprite is a fade/overlay (NOT a backdrop) -> keep it out of the bg band so it
      // composites on top of the world (opaque full-screen sprites stay backdrops).
      int bg = node_is_bg(s_cur_node) || (!semi && bg_2d(x, y, x + w, y + h));
      // Texpage provenance: a field sprite sampling THIS frame's native backdrop texpage is a redundant copy
      // of the sky/sea the native drawer already owns (ov_bg_tilemap_native) — classify it bg so the 2D-only
      // field walk drops it (else its 16×16 tiles fall to RQ_HUD and occlude the world; render.md OPEN #1).
      if (!semi && sprite_is_bg_texpage(core, s_tp_x, s_tp_y)) {
        bg = 1;
      }
      {
        static int pm = -2;
        if (pm == -2) {
          const char *e = cfg_str("PSXPORT_PAINTER");
          pm = e ? atoi(e) : 0;
        }
        if (pm) {
          bg = 0;
        }
      } // DIAG painter
      // FADE/DIM (#21): a full-screen SEMI sprite is a fade/dim overlay -> stretch-to-fill the wide FB so it
      // covers the margins too, while staying in the topmost (HUD) band (not a backdrop). See ws_2d_local_x.
      int fade_full = (!bg && semi && fade_full_2d(s_disp_w, s_disp_h, x, y, x + w, y + h));
      if (!bg && s_frame == s_primdump_frame) {
        lucent::debug("objz",
                      "[sprnode] op={:02x} at({},{} {}x{}) rgb=({},{},{}) node={:08x}",
                      op,
                      x,
                      y,
                      w,
                      h,
                      cr,
                      cg,
                      cb,
                      s_cur_node);
      }
      {
        gpu_primitive_dump_sprite(
            core, s_frame, ord_idx, op, x, y, w, h, bg, cr, cg, cb, textured ? 1 : 0, semi, u0, v0, (op & 1) ? 1 : 0);
      }
      int X = x + s_off_x, Y = y + s_off_y;
      int XL = X, XR = X + w;
      // Widescreen 2D handling. Genuine engine-wide widens the 3D world at the projection (OFX); 2D
      // sprites bypass the GTE, so they are mapped here. A backdrop (sky/water) STRETCHES to fill the wide
      // FB; HUD/UI is identity (the relocation shader's +margin already centers it). See ws_2d_local_x.
      {
        int gpu_vk_wide_engine(Core *);
        // #38: PROVENANCE-only backdrop test for stretch-fill (node_is_bg / sprite_is_bg_texpage), not
        // `bg`'s coverage heuristic — pins the weapon carousel panel to the centered/HUD branch instead
        // of stretch-filling it to the screen edges. `bg` (RQ_BACKGROUND band) is unchanged.
        int fill = (node_is_bg(s_cur_node) || sprite_is_bg_texpage(core, s_tp_x, s_tp_y)) ||
                   fade_full; // backdrop AND full-screen fade/dim stretch-to-fill (#21)
        if (fill) {
          s_seen_bg2d = 1; // #54: this frame owns a full-screen 2D backdrop redraw (menu/title)
        }
        // widen on gameplay frames (3D last frame) OR a full-screen 2D backdrop redraw last frame (#54).
        if (gpu_vk_wide_engine(core) && (s_prev_had3d || s_prev_had_bg2d)) {
          XL = ws_2d_local_x(core, XL, fill); // engine-owned 2D layout (HUD centered, bg/fade fills)
          XR = ws_2d_local_x(core, XR, fill);
        }
      }
      int qx[4] = {XL, XR, XL, XR}, qy[4] = {Y, Y, Y + h, Y + h};
      int qu[4] = {u0, u0 + w, u0, u0 + w}, qv[4] = {v0, v0, v0 + h, v0 + h};
      unsigned char qr[4] = {cr, cr, cr, cr}, qg[4] = {cg, cg, cg, cg}, qb[4] = {cb, cb, cb, cb};
      int mode = textured ? s_tp_mode : 3, rw = op & 1;
      if (use_rq) {
        int layer = bg ? RQ_BACKGROUND : RQ_HUD;
        int om = bg ? RQ_OM_2D_BG : RQ_OM_2D_FG;
        core->game->activeRq().emitOrQueue(core,
                                           1,
                                           layer,
                                           om,
                                           4,
                                           semi,
                                           rw,
                                           qx,
                                           qy,
                                           0,
                                           0,
                                           qu,
                                           qv,
                                           qr,
                                           qg,
                                           qb,
                                           0,
                                           mode,
                                           s_tp_x,
                                           s_tp_y,
                                           s_clut_x,
                                           s_clut_y,
                                           s_tw_mx,
                                           s_tw_my,
                                           s_tw_ox,
                                           s_tw_oy,
                                           s_da_x0,
                                           s_da_y0,
                                           s_da_x1,
                                           s_da_y1,
                                           s_tp_blend,
                                           nullptr,
                                           -1,
                                           0.0f,
                                           0,
                                           0,
                                           {},
                                           s_cur_node,
                                           ord_idx);
      } else {
        gpu_vk_set_order(core, ord_idx); // OT submission order -> depth (preserve opaque/semi order)
        if (bg) {
          gpu_vk_set_order_2d_bg(core, ord_idx);
        } else {
          gpu_vk_set_order_2d(core, ord_idx);
        }
        if (semi) {
          {
            gpu_vk_semi_group(core, X, Y, X + w, Y + h);
          } // OT-order grouping
          gpu_vk_draw_semi(core,
                           qx,
                           qy,
                           qu,
                           qv,
                           qr,
                           qg,
                           qb,
                           s_tp_x,
                           s_tp_y,
                           mode,
                           rw,
                           s_clut_x,
                           s_clut_y,
                           s_tw_mx,
                           s_tw_my,
                           s_tw_ox,
                           s_tw_oy,
                           s_da_x0,
                           s_da_y0,
                           s_da_x1,
                           s_da_y1,
                           s_tp_blend);
          gpu_vk_draw_semi(core,
                           &qx[1],
                           &qy[1],
                           &qu[1],
                           &qv[1],
                           &qr[1],
                           &qg[1],
                           &qb[1],
                           s_tp_x,
                           s_tp_y,
                           mode,
                           rw,
                           s_clut_x,
                           s_clut_y,
                           s_tw_mx,
                           s_tw_my,
                           s_tw_ox,
                           s_tw_oy,
                           s_da_x0,
                           s_da_y0,
                           s_da_x1,
                           s_da_y1,
                           s_tp_blend);
        } else {
          gpu_vk_draw_tritri(core,
                             qx,
                             qy,
                             qu,
                             qv,
                             qr,
                             qg,
                             qb,
                             s_tp_x,
                             s_tp_y,
                             mode,
                             rw,
                             s_clut_x,
                             s_clut_y,
                             s_tw_mx,
                             s_tw_my,
                             s_tw_ox,
                             s_tw_oy,
                             s_da_x0,
                             s_da_y0,
                             s_da_x1,
                             s_da_y1);
          gpu_vk_draw_tritri(core,
                             &qx[1],
                             &qy[1],
                             &qu[1],
                             &qv[1],
                             &qr[1],
                             &qg[1],
                             &qb[1],
                             s_tp_x,
                             s_tp_y,
                             mode,
                             rw,
                             s_clut_x,
                             s_clut_y,
                             s_tw_mx,
                             s_tw_my,
                             s_tw_ox,
                             s_tw_oy,
                             s_da_x0,
                             s_da_y0,
                             s_da_x1,
                             s_da_y1);
        }
      }
    }
    s_prims++;
  } else if (command.opcode() == psx::gpu::Gp0Opcode::FillRect) {
    // A fill rectangle, written straight into VRAM. It ignores the draw area and the draw offset by
    // hardware design — which is why it is the one GP0 command whose pixels the clip cannot stop.
    const psx::gpu::Gp0Colour fillColour = command.colour();
    const uint8_t cr = fillColour.red, cg = fillColour.green, cb = fillColour.blue;
    const psx::gpu::Gp0VramRect fill = psx::gpu::Gp0Command::fillRectRegion(s_fifo[1], s_fifo[2]);
    const int x = fill.x, y = fill.y, w = fill.width, h = fill.height;
    const uint16_t col = psx::gpu::toVram555(cr, cg, cb);
    // A FILL IS A VRAM WRITER, so the atlas guard has to see it. vram_xfer.cpp states that "every
    // VRAM-writing transfer calls this" — that was untrue here, and the gap sat exactly where an
    // atlas clobber is most likely: a full-screen fill is what a pause/menu/blackout does. A clobber
    // arriving via GP0(0x02) produced NO [vramguard] line at all, so the instrument reported a clean
    // bill of health for a writer it could not see. Diagnostic only; the fill itself is unchanged.
    vram_guard_check("fill", x, y, w, h, 0);
    // TEXWATCH must see the FILL too. It watched only A0 and 80copy, so "which write put value V into
    // the CLUT strip?" could be answered SILENTLY WRONG: a fill is a VRAM writer, and a watch that
    // cannot see one writer reports "nothing wrote here" with total confidence. Same coverage gap the
    // vram_guard comment above records for itself. (issue 0007)
    if (texwatch_overlap(x, y, w, h)) {
      lucent::info("texwatch", "f{} FILL dest=({},{}) {}x{} col={:04X}", s_frame, x, y, w, h, col);
    }
    for (int dy = 0; dy < h; dy++) {
      for (int dx = 0; dx < w; dx++) {
        *vram(x + dx, y + dy) = col;
      }
    }
    // Mirror to the VK VRAM image. The native-upload path documents the mirror as happening on "the
    // GP0 0xA0 / VRAM-copy / fill paths" — this one did not do it. The VK opaque pass re-uploads a
    // full s_vram snapshot so it saw the fill anyway, but the SEMI pass samples the dirty-tracked
    // s_tex, so a semi-transparent prim over a filled region sampled stale pixels. Same shape as the
    // invisible-puddle-water bug that put the mirror on the native upload.
    if (vk_path()) {
      gpu_vk_dirty(core, x, y, w, h);
    }
    // WIDESCREEN BACKDROP FILL (#52): FillRect ignores clip/offset by design (PSX hardware behavior,
    // see the comment above) and writes only the native 320x240 VRAM rect — it has no notion of the
    // wide margins at all. The field's sky/sea backdrop never hits this problem because it is an
    // OWNED native drawer that paints the full wide FB directly; but an AUTHORED OT sub-scene (the
    // hut/door interior, #49 authored_subscene) still clears its black backdrop the PSX way, via this
    // very op, once per frame (RE'd: `debug bug52fill` traced a FILL at=(0,0) 320x240 col=(0,0,0) on
    // every interior frame). Because the wide margin columns of the VK render target are LOAD_OP_LOAD
    // (persistent across frames, gpu_vk.cpp render_geom), those columns keep whatever a PRIOR frame's
    // draw left there -> the VRAM-atlas garbage in the widescreen margins.
    // Fix (host-side, read-only, engine-owned — mirrors the existing bg_2d/node_is_bg 2D-widen
    // classification used for polys/sprites): a FillRect that covers the WHOLE base display is a
    // backdrop clear exactly like a full-screen backdrop poly, so queue an equivalent flat quad
    // through the SAME 2D render-queue path (RQ_BACKGROUND / RQ_OM_2D_BG), pre-stretched by
    // ws_2d_local_x's backdrop rule (x*ww/320) so it fills [0,ww) instead of just [0,320). Gated the
    // same way the poly/sprite widen is (gpu_vk_wide_engine + (s_prev_had3d || s_prev_had_bg2d), #54: a
    // full-screen FillRect backdrop is just as legitimate a "this frame repaints the whole width" signal
    // as 3D world geometry — e.g. the title-menu screen, which is pure 2D and never sets s_prev_had3d).
    {
      int gpu_vk_wide_engine(Core *);
      int full = bg_2d(x, y, x + w, y + h);
      if (full) {
        s_seen_bg2d = 1;
      }
      // `debug fillrect` — every FillRect with its rect and the full-screen verdict, plus a running
      // count. Without this, "the margin fix did nothing" has three indistinguishable causes: the game
      // issues no FillRect at all, it issues one that does not cover the base display, or the widen
      // ran and had no visible effect. The count is what separates the first from the others.
      {
        static long n = 0; // RATE LIMIT — the count is the point of the line; keep the counter.
        if (++n <= 24 || (n % 512) == 0) {
          lucent::debug(
              "fillrect", "#{} at=({},{}) {}x{} full={} wide={}", n, x, y, w, h, full, gpu_vk_wide_engine(core));
        }
      }
      // NOT GATED ON THE had3d/had_bg2d LATCH, unlike the poly/sprite widen a few hundred lines up,
      // and the difference is the classification not the caution. Those need to know whether a
      // primitive is screen-space or world-space, which rides on per-primitive DEPTH — and on a port
      // with low depth coverage that gate answers "everything is 2D" and shifts the whole frame twice
      // (measured: sky, ground AND caption each moved a further +86 px). THIS case needs no such
      // judgement: `full` already established GEOMETRICALLY that the rect covers the entire base
      // display, which is what makes it a backdrop clear. Requiring the latch as well made the margin
      // fix unavailable to exactly the ports that need it — one whose pool is double buffered never
      // sets the latch on a prim-bearing frame at all.
      if (full && wide_2d_layout_active(*core)) {
        const int ww = wide_2d_width(*core);
        if (ww > 0) {
          // TO DISPLAY-LOCAL FIRST. A FillRect's rect is VRAM-ABSOLUTE — it ignores clip and offset by
          // design, which is the whole reason this case needs handling separately — while the 2D queue
          // takes display-local coordinates and adds the display origin back on submit. Passing the
          // raw rect therefore offsets the backdrop by the display origin.
          //
          // On a game whose display sits at VRAM y=8 that is an EIGHT-ROW slip, and it is exactly what
          // the residual widescreen artefact was: measured on the margin columns, the atlas noise sat
          // in display rows 0-9 and 230-239 and nowhere else, with rows 10-229 clean. The backdrop was
          // covering rows 8-231 instead of 0-223. x was already going through ws_2d_local_x, which
          // assumes a display-local input, so the two axes disagreed about what frame they were in.
          //
          // THAT FRAME IS THE HOST ENGINE'S. A guest that widened its own projection queues its
          // primitives VRAM-ABSOLUTE (its draw offset already applied), so its backdrop clear must be
          // queued in the same frame. Made display-local, a double-buffered guest's clear of the buffer
          // it is NOT displaying landed a whole buffer away: Spyro 2's (0,12) 512x216 clear, issued
          // while (0,228) was displayed, was queued at rows -216..0, so that buffer's margin never got
          // its base and kept VRAM-atlas noise wherever no primitive reached.
          const bool displayLocal = gpu_vk_wide_engine(core) != 0;
          const int lx = displayLocal ? x - s_disp_x : x, ly = displayLocal ? y - s_disp_y : y;
          int x0 = ws_2d_local_x(core, lx, /*is_bg=*/1), x1 = ws_2d_local_x(core, lx + w, /*is_bg=*/1);
          int xs[4] = {x0, x1, x0, x1}, ys[4] = {ly, ly, ly + h, ly + h};
          int us[4] = {0, 0, 0, 0}, vs[4] = {0, 0, 0, 0};
          unsigned char rs[4] = {cr, cr, cr, cr}, gs[4] = {cg, cg, cg, cg}, bs[4] = {cb, cb, cb, cb};
          core->game->activeRq().push2dQuad(RQ_BACKGROUND,
                                            /*order_2d_fg=*/0,
                                            xs,
                                            ys,
                                            us,
                                            vs,
                                            rs,
                                            gs,
                                            bs,
                                            0,
                                            0,
                                            /*mode=*/3,
                                            /*raw=*/0,
                                            0,
                                            0,
                                            0,
                                            0,
                                            0,
                                            0,
                                            s_da_x0,
                                            s_da_y0,
                                            s_da_x1,
                                            s_da_y1);
        }
      }
    }
    if (vk_path()) {
      gpu_vk_dirty(core, x, y, w, h); // mirror fill to VK
    }
  } else if (command.isLineOrPolyLine()) {
    const psx::gpu::Gp0PrimitiveFlags lineFlags = command.flags();
    const int semi = lineFlags.semiTransparent ? 1 : 0;
    const int gouraud = lineFlags.gouraud ? 1 : 0;
    // Collect the vertex list from s_fifo. A line is a fixed two-vertex strip; a POLY-LINE is N
    // vertices with no count, and the packet's word count is only the minimum before its terminator can
    // appear — so the loop below runs to whatever s_fcount actually holds.
    //
    // The two layouts differ by ONE interleaved word: gouraud carries (colour, position) per vertex,
    // mono carries position only, and the command word supplies the FIRST vertex's colour either way.
    // A poly-line can therefore hold up to 64 vertices, and the cap is this file's, not the guest's.
    const psx::gpu::Gp0Colour firstColour = command.colour();
    const uint8_t r0 = firstColour.red, g0 = firstColour.green, b0 = firstColour.blue;
    int vx[kMaxPolyLineVertices], vy[kMaxPolyLineVertices];
    uint8_t vr[kMaxPolyLineVertices], vg[kMaxPolyLineVertices], vb[kMaxPolyLineVertices];
    const psx::gpu::Gp0VertexPos first = psx::gpu::Gp0Command(s_fifo[1]).vertexPos();
    vx[0] = first.x;
    vy[0] = first.y;
    vr[0] = r0;
    vg[0] = g0;
    vb[0] = b0;
    int nv = 1;
    int i = 2; // past the command word and vertex 0's position word
    while (i < s_fcount && nv < kMaxPolyLineVertices) {
      uint8_t r = r0, g = g0, b = b0;
      if (gouraud) {
        if (i >= s_fcount) {
          break;
        }
        const psx::gpu::Gp0Colour col = psx::gpu::Gp0Command(s_fifo[i++]).colour();
        r = col.red;
        g = col.green;
        b = col.blue;
      }
      if (i >= s_fcount) {
        break;
      }
      const psx::gpu::Gp0VertexPos pos = psx::gpu::Gp0Command(s_fifo[i]).vertexPos();
      vx[nv] = pos.x;
      vy[nv] = pos.y;
      vr[nv] = r;
      vg[nv] = g;
      vb[nv] = b;
      nv++;
      i++;
    }
    // `debug lineprim` — LINE-PRIMITIVE CENSUS (diagnostic only, never draws). Answers "which guest fn
    // emits the GP0 lines pc_render has no producer for" without hand-walking the OT: the packet's own
    // pool address (s_fifo_addr[0], stamped by the OT walk) is looked up in the otattr store-span table,
    // which carries {emitter fn, caller fn, render-walk node}. Run with `debug otattr,lineprim` — the
    // span table is only populated while the `otattr` channel is on.
    // GUARD KEPT (expensive non-logging work): a span-table lookup plus an N-vertex row built piece by
    // piece — Line::add formats eagerly, so this block must not run with the channel off. Per line
    // primitive, so the name is resolved once into a Channel rather than hashed per call.
    {
      static const lucent::Channel lp_ch{"lineprim"};
      if (lp_ch) {
        OtAttr::Span sp{};
        uint32_t pkt = s_fifo_addr[0];
        bool attributed = pkt && core->rsub.otAttr.lookupStore(pkt & 0x1FFFFC, &sp);
        uint32_t node = attributed ? sp.node : 0;
        lucent::Line ln;
        ln.add("f{} op=0x{:02X} nv={} semi={} gouraud={} pkt=0x{:08X} fn=0x{:08X} caller=0x{:08X} node=0x{:08X} "
               "beh=0x{:08X} V[",
               s_frame,
               op,
               nv,
               semi,
               gouraud,
               0x80000000u | (pkt & 0x1FFFFC),
               attributed ? sp.fn : 0,
               attributed ? sp.caller : 0,
               node,
               node ? core->mem_r32((node & 0x1FFFFFFF) + 0x1C) : 0);
        for (int s = 0; s < nv; s++) {
          ln.add("({},{})c({},{},{}) ", vx[s], vy[s], vr[s], vg[s], vb[s]);
        }
        ln.add("] off=({},{}) blend={}", s_off_x, s_off_y, s_tp_blend);
        ln.flush_debug(lp_ch);
      }
    }
    for (int s = 0; !suppressDraw && s + 1 < nv; s++) { // flat colour = start vertex
      if (vk_path()) {                                  // the segment as a 1px-thick quad (mode 3 flat)
        int x0 = vx[s] + s_off_x, y0 = vy[s] + s_off_y, x1 = vx[s + 1] + s_off_x, y1 = vy[s + 1] + s_off_y;
        int ox = (abs(x1 - x0) >= abs(y1 - y0)) ? 0 : 1, oy = ox ? 0 : 1;
        int xa[4] = {x0, x1, x0 + ox, x1 + ox}, ya[4] = {y0, y1, y0 + oy, y1 + oy}, zu[4] = {0, 0, 0, 0};
        unsigned char rr[4] = {vr[s], vr[s + 1], vr[s], vr[s + 1]}, gg[4] = {vg[s], vg[s + 1], vg[s], vg[s + 1]},
                      bb[4] = {vb[s], vb[s + 1], vb[s], vb[s + 1]};
        int o1[3] = {0, 1, 2}, o2[3] = {1, 2, 3}; // tris (p0,p1,p0') and (p1,p0',p1')
        if (semi) {                               // OT-order grouping for the segment quad
          int bx0 = x0 < x1 ? x0 : x1, bx1 = x0 < x1 ? x1 : x0, by0 = y0 < y1 ? y0 : y1, by1 = y0 < y1 ? y1 : y0;
          gpu_vk_semi_group(core, bx0, by0, bx1 + ox, by1 + oy);
        }
        for (int t = 0; t < 2; t++) {
          int *o = t ? o2 : o1;
          int X[3] = {xa[o[0]], xa[o[1]], xa[o[2]]}, Y[3] = {ya[o[0]], ya[o[1]], ya[o[2]]};
          unsigned char R[3] = {rr[o[0]], rr[o[1]], rr[o[2]]}, G[3] = {gg[o[0]], gg[o[1]], gg[o[2]]},
                        B[3] = {bb[o[0]], bb[o[1]], bb[o[2]]};
          if (semi) {
            gpu_vk_draw_semi(core,
                             X,
                             Y,
                             zu,
                             zu,
                             R,
                             G,
                             B,
                             0,
                             0,
                             3,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0,
                             s_da_x0,
                             s_da_y0,
                             s_da_x1,
                             s_da_y1,
                             s_tp_blend);
          } else {
            gpu_vk_draw_tritri(
                core, X, Y, zu, zu, R, G, B, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
          }
        }
      }
    }
    s_prims++;
  }
  // env commands (E1..E6) handled in gpu_gp0 directly (single-word).
}

// PSXPORT_GP0RAW=<frame>: dump the RAW GP0 word stream of that frame to
// scratch/logs/gp0raw_f<frame>.u32 — little-endian uint32 words EXACTLY as the guest wrote them,
// before this file's decoder touches them. Purpose: let an independent decoder (tools/gp0_decode.py)
// read the texpage/texture-window/CLUT the GAME asks for, so "the port decodes it wrong" and "the
// game asks for something wrong" can be told apart. Words belonging to a CPU->VRAM pixel stream are
// texel DATA, not commands: they are excluded from the file but COUNTED, and both counts are logged
// unconditionally at close, so an empty dump can never be mistaken for "no words were issued".
// A WINDOW of GP0RAW_N frames starting at <frame>, not a single frame: this game draws at 30 Hz, so
// half of its logic frames issue ZERO GP0 words. Armed on one frame you have a 50% chance of capturing
// an idle frame and reading it as "the guest issues nothing" — which is exactly what the first run of
// this instrument did (frame 11600: 0 command words). The window makes the alternation visible instead
// of being sampled blindly, and each frame's word count is logged whether or not it was zero.
#define GP0RAW_N 4
static FILE *s_gp0raw_f[GP0RAW_N] = {0, 0, 0, 0};
static int s_gp0raw_frame = -2;
static long s_gp0raw_cmd[GP0RAW_N] = {0, 0, 0, 0}, s_gp0raw_pix[GP0RAW_N] = {0, 0, 0, 0};
static void gp0raw_note(int frame, uint32_t w, int is_pixel_stream) {
  if (s_gp0raw_frame == -2) {
    const char *e = cfg_str("PSXPORT_GP0RAW");
    s_gp0raw_frame = e ? atoi(e) : -1;
  }
  if (s_gp0raw_frame < 0) {
    return;
  }
  int k = frame - s_gp0raw_frame;
  if (k < 0 || k >= GP0RAW_N) {
    return;
  }
  if (is_pixel_stream) {
    s_gp0raw_pix[k]++;
    return;
  }
  if (!s_gp0raw_f[k]) {
    char p[128];
    snprintf(p, sizeof p, "scratch/logs/gp0raw_f%d.u32", frame);
    if (!Fs::ensureParentDirs(p)) {
      lucent::error("gp0raw", "cannot create the parent directory of {} — NOTHING captured", p);
      return;
    }
    s_gp0raw_f[k] = fopen(p, "wb");
    if (!s_gp0raw_f[k]) {
      lucent::error("gp0raw", "cannot open scratch/logs/gp0raw_f{}.u32 — NOTHING captured", frame);
      return;
    }
    lucent::info("gp0raw", "armed on frame {} -> scratch/logs/gp0raw_f{}.u32", frame, frame);
  }
  fwrite(&w, 4, 1, s_gp0raw_f[k]);
  s_gp0raw_cmd[k]++;
}
void gp0raw_close_if_done(int frame) {
  if (s_gp0raw_frame < 0 || frame < s_gp0raw_frame + GP0RAW_N) {
    return;
  }
  for (int k = 0; k < GP0RAW_N; k++) {
    if (s_gp0raw_f[k]) {
      fclose(s_gp0raw_f[k]);
      s_gp0raw_f[k] = 0;
    }
    lucent::info("gp0raw",
                 "frame {}: {} command words, {} pixel-stream words skipped ({})",
                 s_gp0raw_frame + k,
                 s_gp0raw_cmd[k],
                 s_gp0raw_pix[k],
                 s_gp0raw_cmd[k] ? "file written" : "NO file — zero command words this frame");
  }
  s_gp0raw_frame = -1;
}

// One word into the GP0 port (direct write or DMA).
void GpuState::gpu_gp0(Core *core, uint32_t w) {
  s_gp0_words++;
  gp0raw_note(s_frame, w, s_xfer ? 1 : 0); // PSXPORT_GP0RAW: raw word capture, pre-decode
  if (s_xfer) {                            // CPU->VRAM pixel stream (2 px/word)
    if (s_twp_active && s_twp_px == 0) {
      s_twp_addr0 = s_gp0_src;
    }
    for (int k = 0; k < 2; k++) {
      int px = s_xfer_px % s_xfer_w, py = s_xfer_px / s_xfer_w;
      uint16_t hv = (uint16_t)((k ? (w >> 16) : w) & 0xFFFF);
      if (py < s_xfer_h) {
        *vram(s_xfer_x + px, s_xfer_y + py) = hv;
        if (s_twp_active) {
          twp_note(hv);
        }
      }
      s_xfer_px++;
    }
    if (s_xfer_px >= s_xfer_w * s_xfer_h) {
      s_xfer = 0;
      twp_flush("A0");
      if (s_cw_pending) {
        clutwatch_dump("A0 DONE", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);
        s_cw_pending = 0;
      }
    }
    return;
  }
  if (s_fcount == 0) {
    // A word arriving on an EMPTY FIFO starts a packet, so this is where the command is recognised and
    // the two one-word classes are handled: the draw environment (which changes state a later primitive
    // reads) and the commands that are accepted and discarded. Everything else falls through to the
    // FIFO and is executed once the packet is complete.
    const psx::gpu::Gp0Command incoming(w);
    const uint8_t op = incoming.opcodeByte();
    switch (static_cast<psx::gpu::Gp0Opcode>(op)) { // single-word env / state commands
    case psx::gpu::Gp0Opcode::Nop:
      return; // nop
    case psx::gpu::Gp0Opcode::ClearCache:
      return; // clear cache
    case psx::gpu::Gp0Opcode::SetDrawMode:
      // The ONLY command permitted to change the dither enable: a primitive's own embedded texpage word
      // carries page / colour-mode / blend / tex-disable and nothing else. See TexPageFrom.
      set_texpage(incoming.lowHalfWord(), TexPageFrom::DrawMode);
      return;
    case psx::gpu::Gp0Opcode::SetTextureWindow: {
      const psx::gpu::Gp0TextureWindow window = incoming.textureWindow();
      s_tw_mx = window.maskX;
      s_tw_my = window.maskY;
      s_tw_ox = window.offsetX;
      s_tw_oy = window.offsetY;
      return;
    }
    case psx::gpu::Gp0Opcode::SetDrawAreaTopLeft: {
      const psx::gpu::Gp0VramPos corner = incoming.drawAreaCorner();
      s_da_x0 = corner.x;
      s_da_y0 = corner.y;
      lucent::debug("env", "E3 clip_tl=({},{})", s_da_x0, s_da_y0);
      return;
    }
    case psx::gpu::Gp0Opcode::SetDrawAreaBottomRight: {
      const psx::gpu::Gp0VramPos corner = incoming.drawAreaCorner();
      s_da_x1 = corner.x;
      s_da_y1 = corner.y;
      // Widescreen: the guest clips the draw area to the 4:3 FB right edge (s_disp_x+319). The engine
      // renders a wider FOV (OFX shifted to nw/2) into VRAM columns up to s_disp_x+nw, so extend the
      // right clip by (nw-320) — else the wide-side world fragments are clipped and the present shows
      // raw VRAM texture-atlas garbage in the [320,nw) band. Off at 4:3 (wide_engine()==0). This is a
      // render-clip widen consumed by the GPU batch (i_da); it never widens where guest logic reads.
      // The widen is (wide width - THIS GAME'S 4:3 width), not (wide width - 320). Hardcoding 320
      // assumed a 320-wide framebuffer: for a 512-wide game it over-extends the draw area by 192
      // columns past anything the renderer draws, and every column inside the clip that nothing
      // writes presents as raw texture-atlas VRAM. Same wrong assumption as wide_native_w had.
      // At 320 the two forms are identical, so no existing consumer moves.
      {
        int gpu_vk_wide_engine(Core *), gpu_vk_wide_engine_w(Core *);
        if (gpu_vk_wide_engine(core)) {
          s_da_x1 += (gpu_vk_wide_engine_w(core) - (s_disp_w > 0 ? s_disp_w : 320));
        }
      }
      s_draw_rows.note({s_da_y0, s_da_y1 + 1});
      lucent::debug("env", "E4 clip_br=({},{})", s_da_x1, s_da_y1);
      return;
    }
    case psx::gpu::Gp0Opcode::SetDrawOffset: {
      const psx::gpu::Gp0VertexPos offset = incoming.drawOffset();
      s_off_x = offset.x;
      s_off_y = offset.y;
      lucent::debug("env", "E5 offset=({},{})", s_off_x, s_off_y);
      return;
    }
    case psx::gpu::Gp0Opcode::SetMaskBits: {
      // PSXPORT_DEBUG=maskbit — the two mask bits. NEITHER is modelled (see Gp0MaskBits), and the
      // check-mask one is a real correctness gap rather than a missing feature. Print every DISTINCT
      // word with a running total, so "the game never enables it" is a MEASURED ZERO rather than an
      // unlooked-at silence — which is the only way to tell an unused feature from an unnoticed one.
      const psx::gpu::Gp0MaskBits mask = incoming.maskBits();
      if (cfg_dbg("maskbit")) {
        static uint8_t s_seen[4];
        static int s_n = 0, s_total = 0;
        s_total++;
        const std::uint8_t distinct = static_cast<std::uint8_t>((mask.setMask ? 1u : 0u) | (mask.checkMask ? 2u : 0u));
        bool known = false;
        for (int i = 0; i < s_n; i++) {
          known = known || s_seen[i] == distinct;
        }
        if (!known && s_n < 4) {
          s_seen[s_n++] = distinct;
          lucent::warn("maskbit",
                       "GP0(E6) set_mask={} check_mask={} (word {:08X}) — {} distinct value(s) in {} E6 writes",
                       mask.setMask ? 1 : 0,
                       mask.checkMask ? 1 : 0,
                       w,
                       s_n,
                       s_total);
        }
      }
      return; // mask settings (neither bit is modelled as a pixel test)
    }
    default:
      break;
    }
    // A POLY-LINE is the one VARIABLE-length command: op 0x48-0x4F mono / 0x58-0x5F gouraud — the line
    // range with bit 3 set. It is a vertex list terminated by a sentinel word, so its length is NOT in
    // the first word and the packet-framing count below is only the minimum. The FIFO accumulates until
    // the terminator instead. Treating a poly-line as a fixed 3/4-word line drifts the whole GP0 parse
    // and makes a later DATA word decode as a spurious VRAM copy — which is atlas corruption, not a
    // rendering artefact.
    s_pl = incoming.isPolyLine() ? 1 : 0;
    s_pl_g = incoming.flags().gouraud ? 1 : 0;
    s_fneed = static_cast<int>(psx::gpu::Gp0Command::packetWordCount(w));
  }
  // NATIVE-DEPTH COVERAGE COUNTER. A GP0 word only CAN carry depth if we know which guest word it
  // came from (s_gp0_src), because that address is the key projprim stores view-Z against. Words fed
  // through a path that does not stamp an address — a direct GP0 register write, an FMV/block upload —
  // are permanently depth-less, and there is no way to tell that apart at the ndepth counter: it shows
  // "lookups hit=0 miss=0", which reads as "the depth side is broken" when the ADDRESS side is what is
  // missing. Counting both here makes the distinction visible on the ordinary per-frame gpu line.
  if (s_gp0_src) {
    s_gp0_addressed++;
  } else {
    s_gp0_anon++;
  }
  s_fifo_addr[s_fcount] = s_gp0_src;
  s_fifo[s_fcount++] = w;
  if (s_pl) {
    const unsigned idx = static_cast<unsigned>(s_fcount - 1); // index of the word just stored
    // A poly-line's terminator may only appear at a VERTEX-START slot, and never before the mandatory two
    // vertices are in — otherwise a real vertex word that happens to look like the sentinel truncates
    // the line. The two layouts put vertex starts at different indices:
    //   gouraud: cmd,xy0,c1,xy1, then c2/term,xy2,...  -> starts are the EVEN indices from 4
    //   mono:    cmd,xy0,xy1, then xy2/term,xy3,...     -> starts are the indices from 3
    if (psx::gpu::Gp0Command::isPolyLineTerminatorSlot(idx, s_pl_g != 0) &&
        psx::gpu::Gp0Command::isPolyLineTerminator(w)) {
      s_fcount = static_cast<int>(idx); // drop the terminator; render cmd+vertices
      gp0_exec(core);
      s_fcount = 0;
      s_fneed = 0;
      s_pl = 0;
      return;
    }
    if (s_fcount >= kGp0FifoCapacity - kGp0FifoHeadroom) {
      s_fcount = 0;
      s_fneed = 0;
      s_pl = 0;
    } // safety: a poly-line with no terminator must never overflow s_fifo
    return;
  }
  if (s_fcount >= s_fneed) {
    const psx::gpu::Gp0Command packet(s_fifo[0]);
    if (packet.isCpuToVramUpload()) {
      // The upload's pixel STREAM has not arrived yet: the header names a rect and every following word
      // is two pixels of it, until the rect is full. The words are not commands and must not be decoded
      // as such — the branch at the top of this function consumes them.
      const psx::gpu::Gp0VramRect region = psx::gpu::Gp0Command::transferRegion(s_fifo[1], s_fifo[2]);
      s_xfer_x = region.x;
      s_xfer_y = region.y;
      s_xfer_w = region.width;
      s_xfer_h = region.height;
      s_xfer_px = 0;
      s_xfer = 1;
      if (vk_path()) {
        gpu_vk_dirty(core, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h); // mirror upload to VK
      }
      vram_guard_check("A0", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, 0x80000000u | s_dma_src);
      clutwatch_xfer("A0", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);
      lucent::debug("upload",
                    "f{} A0 dest=({},{}) {}x{} src=0x{:08X}",
                    s_frame,
                    s_xfer_x,
                    s_xfer_y,
                    s_xfer_w,
                    s_xfer_h,
                    0x80000000u | s_dma_src);
      if (texwatch_overlap(s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h)) {
        // Arm the payload summary for this transfer (fires at its last word, in the s_xfer branch).
        s_twp_active = 1;
        s_twp_px = 0;
        s_twp_nvals = 0;
        s_twp_more = 0;
        s_twp_addr0 = 0;
        uint32_t src = 0x80000000u | s_dma_src;
        lucent::Line ln;
        ln.add(
            "f{} A0 dest=({},{}) {}x{} src=0x{:08X} srcbytes:", s_frame, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, src);
        for (int k = 0; k < 12; k++) {
          ln.add(" {:02X}", core->mem_r8(s_dma_src + k));
        }
        ln.flush(lucent::Level::Info, "texwatch");
        // PSXPORT_TEXWATCH_BT="w,h" — host backtrace for a watched upload of exactly this size. The
        // guest pc/ra are fiction under guest execution (INST-23), but the HOST stack names the
        // guest_* chain, which is the only way to attribute an upload to guest code. Sized rather
        // than unconditional because the interesting transfer is one specific rect among thousands.
        if (const char *bt = cfg_str("PSXPORT_TEXWATCH_BT")) {
          int bw = 0, bh = 0;
          sscanf(bt, "%d,%d", &bw, &bh);
          if (bw == s_xfer_w && bh == s_xfer_h) {
            static int n_bt = 0;
            if (n_bt++ < 3) {
              lucent::info("texwatch", "backtrace #{} for {}x{} upload:", n_bt, bw, bh);
              void *b[32];
              int n = psxport::host::captureBacktrace(b, 32);
              psxport::host::emitBacktrace(b, n);
            }
          }
        }
      }
    } else if (packet.isVramCopy()) {
      // A VRAM->VRAM copy: source rect, destination rect, size — FOUR words, and the only command with
      // two rects. Note the size word here carries a PLAIN 10/9-bit size with no zero-means-full-axis
      // convention (a copy of "everything" is not expressible) and no 16-pixel alignment (a copy is
      // exactly the rect the guest named).
      const psx::gpu::Gp0VramPos source = psx::gpu::Gp0Command(s_fifo[1]).rectCorner();
      const psx::gpu::Gp0VramPos destination = psx::gpu::Gp0Command(s_fifo[2]).rectCorner();
      const int sx = source.x, sy = source.y;
      const int dx = destination.x, dy = destination.y;
      const int w2 = s_fifo[3] & 0x3FF, h2 = (s_fifo[3] >> 16) & 0x1FF;
      // Guard the DEST rect: a render-OT 0x80 copy whose dest lands on a live texpage is the classic
      // atlas-clobber (later-72 poly-line-desync family). Checked BEFORE the copy so the log names the
      // clobber even though the copy still proceeds (diagnostic, non-mutating; the catch is the point).
      vram_guard_check("80copy", dx, dy, w2, h2, guestAddressOf((uint32_t)(sy * VRAM_W + sx) * 2));
      for (int y = 0; y < h2; y++) {
        for (int x = 0; x < w2; x++) {
          *vram(dx + x, dy + y) = *vram(sx + x, sy + y);
        }
      }
      if (vk_path()) {
        gpu_vk_dirty(core, dx, dy, w2, h2); // mirror VRAM->VRAM copy to VK
      }
      clutwatch_xfer("80copy", dx, dy, w2, h2);
      if (texwatch_overlap(dx, dy, w2, h2)) {
        lucent::info("texwatch",
                     "f{} 80copy src=({},{}) dest=({},{}) {}x{} node=0x{:08X} words={:08X},{:08X},{:08X},{:08X}",
                     s_frame,
                     sx,
                     sy,
                     dx,
                     dy,
                     w2,
                     h2,
                     s_cur_node,
                     s_fifo[0],
                     s_fifo[1],
                     s_fifo[2],
                     s_fifo[3]);
        // Dump RAM + the OT node neighbourhood the first time the atlas-clobbering copy fires, so the
        // malformed node and the chain that reaches it can be examined offline.
        if (cfg_str("PSXPORT_CLOBBERDUMP")) {
          static int done = 0;
          if (!done++) {
            uint32_t na = s_cur_node & 0x1FFFFF;
            lucent::info(
                "clobber", "OT root madr=0x{:08X} node@0x{:08X} neighbourhood:", 0x80000000u | s_ot_madr, s_cur_node);
            for (int k = -8; k <= 16; k++) {
              lucent::info("gpu_native",
                           "  [{:+}] 0x{:08X}: {:08X}",
                           k,
                           0x80000000u | ((na + k * 4) & 0x1FFFFF),
                           core->mem_r32(0x80000000u | ((na + k * 4) & 0x1FFFFF)));
            }
            FILE *mf = fopen(cfg_str("PSXPORT_CLOBBERDUMP"), "wb");
            if (mf) {
              fwrite(core->ram, 1, 0x200000, mf);
              fclose(mf);
              lucent::info("clobber", "RAM dumped -> {}", cfg_str("PSXPORT_CLOBBERDUMP"));
            }
          }
        }
      }
    } else if (packet.isVramToCpuRead()) {
      // GpuDevice answers the readback; nothing here draws.
    } else {
      gp0_exec(core);
    }
    s_fcount = 0;
    s_fneed = 0;
  }
}

// GP1 display/control commands.
void GpuState::gpu_gp1(uint32_t w) {
  uint8_t op = w >> 24;
  lucent::debug("gp1", "f{} {:02X} {:06X}", s_frame, op, w & 0xFFFFFF);
  switch (op) {
  case 0x05:
    s_disp_x = w & 0x3FF;
    s_disp_y = (w >> 10) & 0x1FF;
    break;   // display area start
  case 0x07: // vertical display range (scanlines). In 480i the field is shown twice (two VRAM
    // lines per scanline), so the displayed VRAM height is (y1-y0)*2 — without the doubling the
    // bottom of a 480-line framebuffer is clipped (the SCEA "Presents" line, journal later-46).
    s_disp_vy0 = w & 0x3FF;
    s_disp_vy1 = (w >> 10) & 0x3FF;
    s_disp_vrange_seen = true;
    {
      int n = s_disp_vy1 - s_disp_vy0;
      if (n <= 0) {
        n = 240;
      }
      s_disp_h = s_disp_480i ? n * 2 : n;
    }
    break;
  case 0x08: // display mode: horizontal res (bits0-1, bit6=368), interlace (bit5), VRes 480 (bit2)
    s_disp_w = gp1_display_width(w);
    s_disp_480i = ((w & 0x20) && (w & 0x04)) ? 1 : 0;
    // BIT 4 IS THE DISPLAY COLOUR DEPTH: 0 = 15-bit, 1 = 24-bit. A game that switches to 24bpp for a
    // still (logo screens, FMV frames) packs RGB888 across 1.5 halfwords per pixel, so reading its
    // VRAM as 1555 scrambles every colour AND shows only two thirds of the width — the symptom
    // recorded in the consumer's issue 0016. Both things that DECODE the display region live in
    // gpu_vk (the present shader and the CPU shot/readback), so push the bit over to them.
    {
      const int d24 = (w >> 4) & 1;
      if (d24 != s_disp_rgb24) {
        s_disp_rgb24 = d24;
        extern void gpu_vk_set_display_depth(Core *, int);
        if (game) {
          gpu_vk_set_display_depth(&game->core, d24);
        }
        lucent::info(
            "gpu", "display depth -> {} (GP1(08)={:08X}, {}x{})", d24 ? "24-BIT" : "15-bit", w, s_disp_w, s_disp_h);
      }
    }
    // BIT 3 IS THE DISPLAY STANDARD: 0 = NTSC (60000/1001 Hz fields), 1 = PAL (50 Hz). It was
    // decoded NOWHERE, so the frame pacer had no field rate to pace against and used a literal
    // 60.000 Hz — while the port's vblank counter advanced at the real rate. Decoded here because
    // this write is the game telling the hardware which standard it runs in: the rate is now READ
    // from the guest rather than assumed by the framework (see FramePacer::fieldRateMilliHz below).
    {
      const int pal = (w >> 3) & 1;
      if (!s_disp_std_seen || pal != s_disp_pal) {
        s_disp_std_seen = true;
        s_disp_pal = pal;
        lucent::info("gpu",
                     "display standard -> {} ({}.{:03} Hz fields — the frame pacer's clock, "
                     "GP1(08)={:08X})",
                     pal ? "PAL" : "NTSC",
                     psx::frame::fieldRateMilliHz(pal) / 1000,
                     psx::frame::fieldRateMilliHz(pal) % 1000,
                     w);
      }
    }
    {
      int n = s_disp_vy1 - s_disp_vy0;
      if (n <= 0) {
        n = 240;
      }
      s_disp_h = s_disp_480i ? n * 2 : n;
    }
    break;
  // GP1(06) HORIZONTAL DISPLAY RANGE IS DELIBERATELY NOT DECODED — do not "fix" this without
  // re-taking the measurement below, because driving the display width from it can only make a
  // correct picture worse.
  //
  // The visible width is range/dotclock-divider (10/8/5/4 for 256/320/512/640, 7 for 368), and a
  // game that wants a normal full-width picture programs the standard 2560-clock span, which by
  // construction reproduces the resolution GP1(08) already gave us. Measured on Spider-Man
  // (PSXPORT_DEBUG=gp1, X1 = bits 0-11, X2 = bits 12-23):
  //
  //   C58258  X1=600  X2=3160  span 2560  with GP1(08)=1 (320)  -> 2560/8 = 320   agrees
  //   C67267  X1=615  X2=3175  span 2560  with GP1(08)=2 (512)  -> 2560/5 = 512   agrees
  //   CDA8A7  X1=2215 X2=3290  span 1075                        -> 1075/5 = 215   transient
  //
  // The first two are the steady state and confirm GP1(08) alone is right. The third is a narrow
  // window during a screen change — the same window in which the guest also programs a 2-scanline
  // vertical range (GP1(07)=040900, the libgpu NTSC clamp floor), i.e. a deliberately degenerate
  // display, not a picture anyone sees. An audit read that 215 in isolation and concluded the port
  // was reporting framebuffer width instead of raster width; the full distribution says otherwise.
  //
  // Decoding it into state nothing consumes would just be clutter. If a game ever genuinely
  // letterboxes via GP1(06) — a non-2560 span held across normal frames — decode it THEN, and gate
  // the width on it only for that case.
  case 0x06:
    break;
  default:
    break;
  }
}

// `game` is null only before Game() wires it, before any GP0 word exists.
bool GpuState::deviceScanout() const {
  const RenderPath path = game ? game->core.rsub.mode.path() : RenderPath::Native;
  return path == RenderPath::Device || path == RenderPath::Record;
}

#ifdef PSXPORT_SDL
// The legacy SDL_Renderer software-present window is RETIRED: the SDL_GPU renderer (gpu_vk.cpp) is THE
// present path (gpu_vk_enabled() is always 1), windowed or headless. blit_src forwards the display region
// to it; ensure_window is a no-op (the GPU backend owns the window). This drops the SDL2 SDL_Renderer /
// SDL_Texture code that SDL3 doesn't carry verbatim.
int gpu_vk_enabled(void); // gpu_vk.cpp — SDL_GPU present backend
void GpuState::ensure_window() {}
// The presented height is the decoded display height, or GameConfig::guestDisplayHeight on guest paths
// (display_scanout.h).
void GpuState::present_window() {
  Core *core = &game->core;
  if (deviceScanout()) {
    const psx::gpu::DisplayArea area = core->gpuDevice.displayArea();
    gpu_vk_present(core, core->gpuDevice.vram().data(), area.x, area.y, area.width, presentedHeight(core, area.height));
    return;
  }
  gpu_vk_present(core, s_vram, s_disp_x, s_disp_y, s_disp_w, presentedHeight(core, s_disp_h));
}
// Re-present the CURRENT frame without advancing game logic — the debug-server pause loop's window
// keep-alive. This does NOT go back through present_window(): that BUILDS a frame (VRAM upload +
// render_geom over the live vertex batch), and at a pause point there is no frame left to build — under
// fps60 the batch has already been emitted, presented and reset, so re-running the build cleared the
// composite target to black (kanban #20). gpu_vk_repaint re-shows the composite the last real present
// already filled: no upload, no geometry, no batch reset, no s_frame++ (those belong to gpu_present_ex),
// and identical cost in both fps modes. Because nothing is rebuilt, the readback target is left intact —
// so `shot`/`vkshot` while paused genuinely report the frame that is on screen.
void GpuState::gpu_repaint() {
  gpu_vk_repaint(&game->core);
}
#else
void GpuState::present_window() {}
void GpuState::gpu_repaint() {}
#endif

// Present: copy the displayed VRAM region to an RGB buffer. PSXPORT_GPU_DUMP=dir dumps PPMs;
// PSXPORT_GPU_WINDOW=1 shows a live SDL window.
// REPL `shot <path>`: write the currently-displayed VRAM region to a PPM so I can SEE where the
// interactive driver is (title / menu / attract / gameplay) instead of guessing from stage numbers.
void GpuState::gpu_native_shot(Core *core, const char *path) {
  // VK render lives in the GPU image, not s_vram — read it back over the current display region.
  if (vk_path()) {
    void gpu_vk_shot_region(Core *, const char *, int, int, int, int);
    int gpu_vk_wide_presentation(Core *), gpu_vk_wide_presentation_w(Core *);
    int dw = s_disp_w > 0 ? s_disp_w : 320, dh = s_disp_h > 0 ? s_disp_h : 240;
    // Widescreen: crop the wider FB the engine rendered (nw@aspect), matching the windowed present's
    // wide sample region — otherwise a headless wide shot silently crops back to the 4:3 s_disp_w.
    if (gpu_vk_wide_presentation(core)) {
      dw = gpu_vk_wide_presentation_w(core);
    }
    gpu_vk_shot_region(core, path, s_disp_x, s_disp_y, dw, dh);
    return;
  }
  if (core->rsub.mode.path() == RenderPath::Record) {
    game->gpu_vk.shot(path);
    return;
  }
  const psx::gpu::DisplayArea area = core->gpuDevice.displayArea();
  const psx::gpu::Rgb24Picture picture =
      core->gpuDevice.picture(area.x, area.y, area.width, presentedHeight(core, area.height), area.depth24);
  if (!image_write_rgb24(path, picture.pixels.data(), picture.width, picture.height)) {
    lucent::error("shot", "f{} could NOT write {} — NOTHING captured", s_frame, path ? path : "(null)");
    return;
  }
  lucent::info("shot",
               "f{} -> {} ({}x{} disp@{},{})",
               s_frame,
               path ? path : "(null)",
               picture.width,
               picture.height,
               area.x,
               area.y);
}
static void shot_triggers(Core *core, uint32_t frame); // capture triggers (defined below the presenters)
// gpu_present_ex: the per-frame present + bookkeeping. `do_blit` blits the live front buffer to the
// window; fps60 passes 0 (it owns presentation: it blits the previous real frame + the interpolated
// frame itself) but still wants the bookkeeping (watchdog, s_frame++, diagnostics).
void GpuState::gpu_present_ex(Core *core, int do_blit, GpuPresentCompletion completion) {
  // GUARD KEPT: a full 1024x512 VRAM sweep, not a log call. Once per frame, so the string_view
  // form's ~19 ns is irrelevant next to the 512k-pixel scan it gates.
  if (lucent::channel_on("vramscan")) {
    int minx = 99999, miny = 99999, maxx = -1, maxy = -1;
    long nz = 0;
    for (int y = 0; y < 512; y++) {
      for (int x = 0; x < 1024; x++) {
        if (*vram(x, y) & 0x7FFF) {
          nz++;
          if (x < minx) {
            minx = x;
          }
          if (x > maxx) {
            maxx = x;
          }
          if (y < miny) {
            miny = y;
          }
          if (y > maxy) {
            maxy = y;
          }
        }
      }
    }
    lucent::debug("vramscan",
                  "f{} disp@({},{}) {}x{}  nonblack={} bbox=({},{})-({},{})",
                  s_frame,
                  s_disp_x,
                  s_disp_y,
                  s_disp_w,
                  s_disp_h,
                  nz,
                  minx,
                  miny,
                  maxx,
                  maxy);
  }
  if (do_blit) {
    present_window();
  }
  {
    void ws_sx_dump(const char *); // widescreen RE (later-55): dump GTE screen-X histogram
    if (lucent::channel_on("sxhist") && s_frame > 0 && (s_frame % 500) == 0) {
      char t[32];
      snprintf(t, sizeof t, "f%d", s_frame);
      ws_sx_dump(t);
    }
  }
  {
    void proj_probe_dump(const char *); // Phase-1: native-projection 0-diff verifier (PSXPORT_PROJPROBE)
    if (cfg_on("PSXPORT_PROJPROBE") && s_frame > 0 && (s_frame % 200) == 0) {
      char t[32];
      snprintf(t, sizeof t, "f%d", s_frame);
      proj_probe_dump(t);
    }
  }
  {
    void rtpcaller_dump(Core *, const char *);
    void rtpcaller_reset(void); // Phase-1: pin RTP caller sites
    // Window the histogram to the LAST 50 frames so a dump reflects only the CURRENT scene's submitters
    // (a cumulative-since-boot count is dominated by the title/menu phase before the field is reached).
    if (lucent::channel_on("rtpcaller") && s_frame > 0 && (s_frame % 50) == 0) {
      char t[24];
      snprintf(t, sizeof t, "f%d(last50)", s_frame);
      rtpcaller_dump(core, t);
      rtpcaller_reset();
    }
  }
  {
    {

      if (s_frame > 0 && (s_frame % 60) == 0) {
        lucent::debug("ndepth",
                      "[ndepth f{}] real-depth(3D) prims={}  OT-band(2D) prims={}  3D%={:.1f}",
                      s_frame,
                      core->rsub.stats.nd3d,
                      core->rsub.stats.nd2d,
                      (core->rsub.stats.nd3d + core->rsub.stats.nd2d)
                          ? 100.0 * core->rsub.stats.nd3d / (core->rsub.stats.nd3d + core->rsub.stats.nd2d)
                          : 0.0);
      }
      {
        auto s = core->rsub.projprim.stats();
        if (s_frame > 0 && (s_frame % 60) == 0) {
          // OCCUPANCY AND OVERFLOW ARE PART OF THIS LINE, not an internal detail. The cache drops
          // every record once it is full (ProjPrim::setPz: `if (mN >= kMax) { mOverflow = 1; return; }`)
          // and nothing read that flag, so a port whose taps outgrew the cache saw its records climb
          // and its hit rate stay flat with no way to tell that from "the taps are on the wrong
          // addresses". A silently-dropped record is exactly the failure this channel exists to expose.
          lucent::debug("ndepth",
                        "    projprim(vtx) records={}  lookups hit={} miss={}  cache {}/{}{}",
                        s.set,
                        s.hit,
                        s.miss,
                        core->rsub.projprim.count(),
                        ProjPrim::kMax,
                        core->rsub.projprim.overflowed() ? "  OVERFLOWED — records were DROPPED" : "");
        }
        // UNCONDITIONAL, and it always was — the old indentation only made it look guarded (the `if`
        // above has no braces). nearReport gates itself on its own `pznear` channel.
        core->rsub.projprim.nearReport("ndepth");
        core->rsub.projprim.statsReset();
      }
      // GUARD KEPT: the block also CLEARS the histogram, which is real work and must not happen on a
      // run where nobody asked for the channel (the accumulator above is gated the same way).
      if (lucent::channel_on("ndepth") && s_frame > 0 && (s_frame % 60) == 0) {
        for (int o = 0; o < 256; o++) {
          if (s_nd2d_hist[o]) {
            int gour = o & 0x10, quad = o & 0x08, tex = o & 0x04, semi = o & 0x02;
            const char *k = (o >= 0x60 && o < 0x80)   ? "SPRITE"
                            : (o >= 0x40 && o < 0x60) ? "LINE"
                            : (o >= 0x20 && o < 0x40) ? "POLY"
                            : (o >= 0x80)             ? "BLIT"
                                                      : "misc";
            lucent::debug("ndepth",
                          "    2D-band op 0x{:02X} x{}  [{}{}{}{} {}]",
                          o,
                          s_nd2d_hist[o],
                          gour ? "G" : "-",
                          quad ? "4" : "3",
                          tex ? "T" : "-",
                          semi ? "s" : "-",
                          k);
          }
        }
        for (int o = 0; o < 256; o++) {
          s_nd2d_hist[o] = 0;
        }
      }
      core->rsub.stats.nd3d = core->rsub.stats.nd2d = 0;
    }
  }
  {
    const char *vd = cfg_str("PSXPORT_VRAMDUMP_AT"); // "frame:path" — dump our 1024x512x16 VRAM
    if (vd) {
      int fr = atoi(vd);
      const char *col = strchr(vd, ':');
      if (col && s_frame == fr) {
        FILE *vf = fopen(col + 1, "wb");
        if (vf) {
          fwrite(s_vram, 2, VRAM_W * VRAM_H, vf);
          fclose(vf);
          lucent::info("gpu", "VRAM dump f{} -> {}", s_frame, col + 1);
        }
      }
    }
  }
  if ((s_frame % 200) == 0) {
    lucent::debug("stage", "[stagetl] gpu f{} task0entry={:08X}", s_frame, core->mem_r32(0x801fe00c));
  }
  const char *dir = cfg_str("PSXPORT_GPU_DUMP");
  if ((s_frame % 300) == 0) {
    vram_guard_report(); // vramguard census — the negative with its denominator
  }
  if (s_log) {
    lucent::info("gpu",
                 "frame {}: {} prims, {} gp0words ({} addressed, {} anon), {} dma2, disp {}x{} @ ({},{})",
                 s_frame,
                 s_prims,
                 s_gp0_words,
                 s_gp0_addressed,
                 s_gp0_anon,
                 s_dma2,
                 s_disp_w,
                 s_disp_h,
                 s_disp_x,
                 s_disp_y);
  }
  // PSXPORT_VRAMDUMP="frame:path" — dump our full 1024x512x16 VRAM at `frame` (raw u16, no header),
  // matching the oracle's PSXPORT_VRAMDUMP (main.cpp) so the texture/CLUT ATLAS can be diffed across
  // engines at a scene-aligned frame (the atlas is uploaded once at scene load = static per scene).
  {
    static int vf = -2;
    static char vp[256];
    if (vf == -2) {
      const char *e = cfg_str("PSXPORT_VRAMDUMP");
      vf = -1;
      if (e) {
        const char *col = strchr(e, ':');
        if (col) {
          vf = atoi(e);
          snprintf(vp, sizeof vp, "%s", col + 1);
        }
      }
    }
    if (vf >= 0 && s_frame == vf) {
      FILE *f = fopen(vp, "wb");
      if (f) {
        fwrite(s_vram, 2, (size_t)VRAM_W * VRAM_H, f);
        fclose(f);
        lucent::info("vramdump", "f{} -> {} (1024x512x16)", s_frame, vp);
      }
    }
  }
  // PSXPORT_GRAMDUMP="frame:path" — dump the full 2 MB of guest RAM at `frame`.
  //
  // WHY IT LIVES HERE, next to VRAMDUMP, and not with PSXPORT_RAMDUMP_FRAME: that knob fires from the
  // NATIVE frame loop (native_boot.cpp), which a port running the guest's own main() on the substrate
  // never executes — it produces no file AND no message, so "no dump" and "this knob is inert here" are
  // indistinguishable (instruments.md INST-22). gpu_present_ex runs on BOTH paths, which is exactly why
  // VRAMDUMP works where RAMDUMP_FRAME does not. It also logs unconditionally on the target frame,
  // including the fopen failure, so a silent run means the frame was never reached.
  {
    static int gf = -2;
    static char gp[256];
    if (gf == -2) {
      const char *e = cfg_str("PSXPORT_GRAMDUMP");
      gf = -1;
      if (e) {
        const char *col = strchr(e, ':');
        if (col) {
          gf = atoi(e);
          snprintf(gp, sizeof gp, "%s", col + 1);
        }
      }
    }
    if (gf >= 0 && s_frame == gf) {
      FILE *f = fopen(gp, "wb");
      if (!f) {
        lucent::warn("gramdump", "f{} could not open {}", s_frame, gp);
      } else {
        size_t n = fwrite(core->ram, 1, 0x200000, f);
        fclose(f);
        lucent::info("gramdump", "f{} -> {} ({} bytes of 2097152)", s_frame, gp, n);
      }
    }
  }
  if (dir) {
    // PSXPORT_GPU_DUMP=dir[:every] — dump every Nth frame instead of all of them.
    //
    // Dumping unconditionally is expensive at both ends. A consumer's 40s gate run wrote 19003 PPMs
    // totalling 6.6 GB, and the check that consumes them then had to read all of it back; that
    // post-phase dominated the gate's wall clock and, under load, pushed several runs past their
    // timeouts. The write side is worse than it looks too: it is I/O inside the measured run, so it
    // distorts any timing taken from the same run.
    //
    // "does the picture change over the run" is answered just as well by a few hundred evenly spaced
    // frames. Bare `dir` still means every frame, so nothing changes for a caller that wants them all.
    char dbuf[512];
    snprintf(dbuf, sizeof dbuf, "%s", dir);
    int every = 1;
    // Only treat a trailing ":N" as an interval when N is ALL DIGITS. atoi alone would read
    // "/path/to/a:2b" as interval 2 and silently truncate the directory name; a path whose last
    // component legitimately contains a colon should be left alone.
    {
      char *col = strrchr(dbuf, ':');
      if (col && col[1]) {
        bool alldig = true;
        for (const char *q = col + 1; *q; q++) {
          if (*q < '0' || *q > '9') {
            alldig = false;
            break;
          }
        }
        if (alldig) {
          int n = atoi(col + 1);
          if (n > 0) {
            every = n;
            *col = 0;
          }
        }
      }
    }
    dir = dbuf;
    // Skip only the WRITE on a non-sampled frame. An early return here would also skip
    // frame_finalize() at the end of this function — the depth-table reset, batch reset and s_frame++
    // — so the frame counter would stop advancing and geometry batches would accumulate across
    // frames. Cheap mistake to make, expensive to diagnose.
    const bool want = (every <= 1) || ((s_frame % every) == 0);
    char path[512];
    snprintf(path, sizeof path, "%s/f%05d.ppm", dir, s_frame);
    FILE *f = nullptr;
    if (want && Fs::ensureParentDirs(path)) {
      f = fopen(path, "wb");
    } else if (want) {
      lucent::error("gpudump", "cannot create the parent directory of {} — NOTHING captured", path);
    }
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", s_disp_w, s_disp_h);
      for (int y = 0; y < s_disp_h; y++) {
        for (int x = 0; x < s_disp_w; x++) {
          uint16_t p = *vram(s_disp_x + x, s_disp_y + y);
          uint8_t rgb[3] = {
              (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3)};
          fwrite(rgb, 1, 3, f);
        }
      }
      fclose(f);
    }
  }
  {
    static int fa = -2, fb = -2; // PSXPORT_FADEDBG="a:b": per-frame brightness/draw log over [a,b]
    if (fa == -2) {
      const char *e = cfg_str("PSXPORT_FADEDBG");
      fa = fb = -1;
      if (e) {
        fa = atoi(e);
        const char *col = strchr(e, ':');
        fb = col ? atoi(col + 1) : fa + 200;
      }
    }
    if (fa >= 0 && s_frame >= fa && s_frame <= fb) {
      lucent::info("fadedbg",
                   "f{} disp=({},{}) drawY={} maxcol={} nprim={} nsemi={} semi[{}..{}] bigsemi={}",
                   s_frame,
                   s_disp_x,
                   s_disp_y,
                   s_fade_lasty,
                   s_fade_maxc,
                   s_fade_npoly,
                   s_fade_nsemi,
                   s_fade_semimin == 999 ? -1 : s_fade_semimin,
                   s_fade_semimax,
                   s_fade_bigsemi);
    }
  }
  frame_finalize(core); // depth-table reset, batch reset, s_frame++ / s_prim_order / s_seen3d bookkeeping
  if (completion == GpuPresentCompletion::MainFrame) {
    watchdog_main_present_complete(); // first real main present ends cold-init grace
  } else {
    watchdog_progress(); // bootstrap/transition black is progress, not main-present readiness
  }
}
// Per-frame render finalize: the "advance to the next frame" work that is INDEPENDENT of the window blit —
// native per-vertex depth-table reset, geometry-batch reset, and the per-frame GpuState counters
// (s_frame, the s_prim_order painter-order/VK-depth index, the s_seen3d backdrop-vs-HUD flag). Shared so
// BOTH paths run identical bookkeeping: standalone via gpu_present_ex, and the differential test per-core grab, which
// returns before gpu_present (game_tomba2.cpp output suppression early-return) and so would otherwise NEVER reset
// s_prim_order / s_seen3d / the depth table — they'd accumulate across frames and corrupt cross-frame
// ordering (a semi-transparent sea drawn over a sprite, the fisherman-cutscene bug), diverging each differential test
// pane from its standalone counterpart. The frame counter also drives the per-frame span cache
// (bg_range), which self-clears only when s_frame advances.
void GpuState::frame_finalize(Core *core) {
  // per-vertex depth table (native-depth path): clear after this frame's DrawOTag/lookups, before next
  // frame's projections record into it — so a vertex word never reads an OLD frame's depth (submit.cpp
  // repopulates each frame). Same reset gpu_present_ex used to do inline before its diagnostics.
  {
    int attach_enabled(void);
    if (attach_enabled()) {
      core->rsub.projprim.reset();
    }
  }
  s_fade_maxc = 0;
  s_fade_npoly = 0;
  s_fade_nsemi = 0;
  s_fade_semimax = -1;
  s_fade_semimin = 999;
  s_fade_bigsemi = 0;
  gpu_vk_frame_end(core, s_vram, s_frame); // VK: diff + geometry-batch reset
  // A watched CPU->VRAM transfer still open at the frame boundary NEVER completed: report it rather
  // than dropping it, because "no PAYLOAD line" would otherwise be indistinguishable from "the watch
  // did not fire". px=</expected on this line is the tell that the pixel stream was short.
  if (s_twp_active) {
    twp_flush("A0-INCOMPLETE");
  }
  s_frame++;
  s_prims = 0;
  s_gp0_words = 0;
  s_dma2 = 0;
  s_gp0_addressed = 0;
  s_gp0_anon = 0;
  s_prim_order = 0; // restart the per-frame OT submission order (VK depth) for the next frame
  // NOTE, measured on a consuming port and deliberately NOT changed here. These latches gate the
  // widescreen 2D widen and are rolled every frame — so on a game whose packet pool is double buffered
  // and which therefore submits ~1600 prims on one frame and ZERO on the next (Spyro the Dragon),
  // s_prev_had3d is false on every prim-bearing frame and the 2D widen never fires at all.
  //
  // Making the latch survive an empty frame DOES make it fire, and that made the picture WORSE.
  //
  // THE FIRST EXPLANATION FOR THAT WAS WRONG, and is corrected here rather than left to mislead: it
  // was recorded as an ORDERING problem — 2D widened against a 3D projection that had not been
  // re-centred yet. That port has since re-centred OFX across every renderer that contributes to a
  // frame, so the ordering precondition now holds, and enabling the latch STILL makes it worse.
  // Measured this time instead of reasoned: with the latch firing, the sky, the ground AND the
  // screen-space caption each move a further +86 px — the margin — so the widen is not shifting 2D
  // relative to 3D at all, it is shifting the WHOLE FRAME a second time on top of OFX.
  //
  // THE REAL GATE IS 2D-vs-3D DISCRIMINATION. The widen only makes sense applied to content the
  // renderer can tell is screen-space, and that discrimination rides on per-primitive DEPTH being
  // resolved (s_seen3d is set from a projected world prim). On a port whose depth coverage is a few
  // percent, almost nothing is classified 3D, so "widen the 2D" becomes "widen everything". This
  // gate cannot be enabled there until native depth coverage is real — which is what OWNING the
  // geometry renderers buys, and is now the reason to own them.
  s_prev_had3d = s_seen3d;       // remember whether this frame was a gameplay (3D) frame (wide widen gate)
  s_seen3d = 0;                  // restart backdrop-vs-HUD discrimination (no 3D prim seen yet next frame)
  s_prev_had_bg2d = s_seen_bg2d; // #54: remember whether this frame drew a full-screen 2D backdrop
  s_seen_bg2d = 0;
  {
    gpu_primitive_dump_finish_frame(core, s_frame);
  } // PSXPORT_PRIMDUMP: flush the file
  {
    void gp0raw_close_if_done(int);
    gp0raw_close_if_done(s_frame);
  } // PSXPORT_GP0RAW: flush + report counts
}
// ---- capture triggers: ONE point per FRAME, the end of the frame's presentation --------------------
// These ran at the tail of gpu_present_ex, which was correct only while gpu_present_ex was the last
// presenter of a frame. It is not: a native movie presents through gpu_vk_present_image, which runs
// AFTER this tail, and during a movie the guest draws nothing, so a capture taken here read the empty
// guest-VRAM composite — black, on every frame of a movie that was decoding and presenting the whole
// time (issue 0040).
//
// So the trigger moved to the single point at which the frame's presentation is over:
// psx::frame::FrameLoopShell::step, after stepFrame has returned and the presentation fence has been checked to
// have advanced exactly once. Both presenters now only present, and the capture reads whatever the
// turn ended showing, which is the same thing the window shows. gpu_present_frame_capture() below is
// that entry point.
//
// gpu_present_ex is still the one place both PRESENTERS shared, and that remains true and useful: it
// is what keeps the two instruments reaching fps60 and the native frame path alike. GpuState::gpu_present is NOT the
// only presenter: Fps60::present_vk calls gpu_present_ex directly (fps60.cpp:380), and a port running fps60 never
// reaches gpu_present at all (Tomba2Engine ships fps60=1). So both capture instruments were SILENTLY INERT on that port
// — MEASURED 2026-08-05: PSXPORT_SHOT_AT over 2802 presents produced zero files and zero log lines, while the same
// binary with fps60=0 captured normally. That is a diagnostic that can print nothing, which this project treats as a
// lying instrument: "no capture" and "capture not wired here" are indistinguishable on screen. gpu_present_ex is the
// chokepoint BOTH presenters share. No behaviour change when the env vars are unset, which is every normal run.
static void shot_triggers(Core *core, uint32_t frame) {
  // PSXPORT_SHOT_AT=f0,f1,... — dump the presented frame to scratch/screenshots/shot_<f>.ppm at these
  // present indices. gpu_vk_shot already existed but was reachable ONLY from pad-replay
  // (PSXPORT_PAD_SHOT_AT, pad_input.cpp), so an ordinary boot had no way to produce a picture at all —
  // and "does it render?" was being answered from primitive counters instead of from pixels. Counters
  // say the queue drained; only an image says what the player would see.
  static int init = 0;
  static uint32_t at[32];
  static int n = 0;
  if (!init) {
    init = 1;
    const char *e = cfg_str("PSXPORT_SHOT_AT");
    if (e) {
      char buf[256];
      snprintf(buf, sizeof buf, "%s", e);
      for (char *t = strtok(buf, ","); t && n < 32; t = strtok(nullptr, ",")) {
        at[n++] = (uint32_t)strtoul(t, 0, 0);
      }
    }
  }
  for (int i = 0; i < n; i++) {
    if (at[i] == frame) {
      void gpu_vk_shot(Core *, const char *);
      char pth[128];
      snprintf(pth, sizeof pth, "scratch/screenshots/shot_%u.ppm", frame);
      // Announces the ARM, not the outcome — gpu_vk_shot logs whether a file actually landed, and this
      // line used to sit after it saying "present N -> <path>" even when nothing was written, which
      // read as a second confirmation of a capture that had just failed.
      lucent::info("shot",
                   "present {} -> arming VRAM capture to {} (GUEST VRAM at this present — not "
                   "the presented picture; use PSXPORT_PRESENT_SHOT_AT for that)",
                   frame,
                   pth);
      gpu_vk_shot(core, pth);
    }
  }
  // PSXPORT_PRESENT_SHOT_AT=f0,f1,... — the same trigger over the PRESENT stage. Deliberately a
  // SEPARATE variable rather than a mode of the one above: the two answer different questions ("what
  // did the guest draw into VRAM" vs "what does the player see"), the project has already spent a
  // session confusing them (issue 0005), and a run that wants both should get both files.
  static int pinit = 0;
  static uint32_t pat[32];
  static int pn = 0;
  if (!pinit) {
    pinit = 1;
    const char *e = cfg_str("PSXPORT_PRESENT_SHOT_AT");
    if (e) {
      char buf[256];
      snprintf(buf, sizeof buf, "%s", e);
      for (char *t = strtok(buf, ","); t && pn < 32; t = strtok(nullptr, ",")) {
        pat[pn++] = (uint32_t)strtoul(t, 0, 0);
      }
    }
  }
  for (int i = 0; i < pn; i++) {
    if (pat[i] == frame) {
      void gpu_vk_present_shot(Core *, const char *);
      char pth[128];
      snprintf(pth, sizeof pth, "scratch/screenshots/present_%u.png", frame);
      gpu_vk_present_shot(core, pth); // logs its own path, size, leg and non-black coverage
    }
  }
}

// THE FRAME'S CAPTURES, once the frame's presentation is complete. Called from psx::frame::FrameLoopShell::step,
// the one point every frame passes through after its presenters have run. The frame number it reports
// is the same `s_frame` the old tail-of-present trigger used — that counter is advanced by
// frame_finalize() inside gpu_present_ex, so it already names this frame by the time the step returns.
void gpu_present_frame_capture(Core *core) {
  shot_triggers(core, static_cast<uint32_t>(core->game->gpu.s_frame));
}
void GpuState::gpu_present(Core *core) {
  gpu_present_ex(core, 1, GpuPresentCompletion::MainFrame);
}
// FMV / SCEA-splash teardown (issues #7/#11): black out the DISPLAYED framebuffer region of s_vram and
// present once, so no FMV last-frame or SCEA white-fill survives into the front-end. The resident
// off-display SCEA text page is left alone — the title overwrites that VRAM when it uploads its atlas;
// blacking the DISPLAY region is what removes the visible artifact. Wrap-safe per-pixel (any disp config).
void GpuState::gpu_blank_display() { // zero the display FB rect (NO present) — caller presents later
  int dw = s_disp_w > 0 ? s_disp_w : 320, dh = s_disp_h > 0 ? s_disp_h : 240;
  // #54: the display FB IS the wide width once widescreen is active (present() samples [sx,sx+wide_w) —
  // see GpuVkState::present's disp_w comment) — clamping this clear to the native 320 left the wide
  // margin columns unblanked during the title/loading ramp (Engine::drawOTag's `s48<2` blank-display
  // call), so a transition frame between "blanked" and "backdrop widened" could still show a sliver of
  // stale VRAM-atlas content in the margin. Blank the FULL wide width so this call is a genuine clear of
  // everything present() will sample, matching the same width present()/the widen sites already use.
  // Blank ONLY the native 320-wide framebuffer — NEVER the wide margin. VRAM columns [320,nw) at the
  // display Y are NOT framebuffer, they are the TEXTURE ATLAS (object textures/CLUTs). The earlier #54
  // change widened this clear to cover the whole present-sampled width, but that ZEROED the atlas —
  // corrupting every object whose texture lives there — but ONLY when the game STARTED widescreen (the
  // loader/black-out ran wide while the atlas was resident); starting 4:3 blanked 320 and spared it.
  // The wide margin is the RENDERER's job (native backdrop / pillarbox fills [320,nw) at present), not a
  // guest-VRAM clear, so it must never touch the atlas here.
  // GUARDED, because THIS WRITER HAS ALREADY CLOBBERED THE ATLAS ONCE — see the #54 note above, where
  // a widened clear zeroed the texture pages that live to the right of the framebuffer and corrupted
  // every object sampling them. It was fixed by narrowing the rect, but nothing was ever put in place
  // to CATCH a recurrence, and the atlas guard did not watch this path.
  vram_guard_check("blank", s_disp_x, s_disp_y, dw, dh, 0);
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      *vram(s_disp_x + x, s_disp_y + y) = 0; // opaque black (555, bit15=0)
    }
  }
}
void GpuState::gpu_clear_display(Core *core, int do_blit) {
  gpu_blank_display();
  gpu_present_ex(core, do_blit, GpuPresentCompletion::Transition);
}
void GpuState::gpu_native_init() {
  if (lucent::channel_on("gpu") || cfg_on("PSXPORT_GPU_LOG")) {
    s_log = 1; // diagnostic: per-frame prim log via env
  }
  if (lucent::channel_on("red")) {
    s_reddbg = 1;
  }
  const char *cw = cfg_str("PSXPORT_CLUTWATCH");
  if (cw) {
    s_cw_x = 880;
    s_cw_y = 507;
    int x, y;
    if (sscanf(cw, "%d,%d", &x, &y) == 2) {
      s_cw_x = x;
      s_cw_y = y;
    }
  }
}

// Read-only VRAM inspection accessor (raw 16-bit 555+mask word). Used by the offline GPU-QA
// harness to assert exact rasterized/blended values; harmless in production (read-only).
uint16_t GpuState::gpu_vram_peek(int x, int y) {
  return *vram(x, y);
}

// PC-native SCEA decode: turn the baked 4bpp+CLUT asset (scea_asset.h) into a flat RGBA8 buffer laid out
// at the 640x468 SCEA SCREEN positions (the same 3 rects / texpage / CLUT / UV the PSX boot stub used,
// mirroring scea_splash_composite). Text pixels = the CLUT color (a=255); everywhere else = (0,0,0,0).
// This is the PSX-FREE source for gpu_vk_present_image (no s_vram poke, no VRAM mirror) — `out` must be
// SCEA_DISP_W * SCEA_DISP_H * 4 bytes. The 5-bit PSX color channels are expanded to 8-bit (<<3 | >>2).
void gpu_scea_decode_rgba(uint8_t *out) {
  memset(out, 0, (size_t)SCEA_DISP_W * SCEA_DISP_H * 4); // transparent/black background
  struct {
    int sx, sy, w, h, u0, v0;
  } sp[3] = {// screen rect + texture UV origin (from the SCEA GP0)
             {536, 200, 64, 32, 0, 128},
             {280, 200, 256, 64, 0, 64},
             {24, 200, 256, 64, 0, 0}};
  for (int s = 0; s < 3; s++) {
    for (int r = 0; r < sp[s].h; r++) {
      for (int c = 0; c < sp[s].w; c++) {
        int u = sp[s].u0 + c, v = sp[s].v0 + r;
        // 4bpp texel: the asset word for this (u,v) is scea_tex_words[v*W + (u>>2)] (the same word the
        // PSX boot read from VRAM at texpage (832,256)); the nibble is selected by (u&3). The baked
        // texture is 64 words x 160 rows — exactly covering all 3 rects' (u>>2) in [0,64), v in [0,160).
        int tu = u >> 2, tv = v;
        if (tu < 0 || tu >= SCEA_TEX_W || tv < 0 || tv >= SCEA_TEX_H) {
          continue;
        }
        uint16_t word = scea_tex_words[tv * SCEA_TEX_W + tu];
        int idx = (word >> ((u & 3) * 4)) & 0xF;
        uint16_t cl = scea_clut_words[idx];
        if (cl == 0) {
          continue; // PSX textured: a 0x0000 CLUT entry is transparent
        }
        int R = (cl & 31), G = (cl >> 5) & 31, B = (cl >> 10) & 31;
        int x = sp[s].sx + c, y = sp[s].sy + r;
        if (x < 0 || x >= SCEA_DISP_W || y < 0 || y >= SCEA_DISP_H) {
          continue;
        }
        uint8_t *px = out + ((size_t)y * SCEA_DISP_W + x) * 4;
        px[0] = (uint8_t)((R << 3) | (R >> 2));
        px[1] = (uint8_t)((G << 3) | (G >> 2));
        px[2] = (uint8_t)((B << 3) | (B >> 2));
        px[3] = 255;
      }
    }
  }
}

// (Was `int gpu_prims_since_present(void)` reading a file-scope s_prims — retired 2026-07-03; s_prims
// moved onto GpuState, and this accessor had no external callers. Read via `c->game->gpu.s_prims` now.)

// Bulk VRAM load/save (1024x512x16). Used by the offline GP0 differ harness (tools/gpu_differ):
// seed s_vram with a captured initial VRAM, replay a GP0 word stream via gpu_gp0(), then read back
// the rasterized result for a pixel-exact compare against Beetle on the identical input.
void GpuState::gpu_vram_load(const uint16_t *src) {
  memcpy(s_vram, src, sizeof(s_vram));
}
void GpuState::gpu_vram_save(uint16_t *dst) {
  memcpy(dst, s_vram, sizeof(s_vram));
}

int GpuState::gpu_frame_no() {
  return s_frame;
}
