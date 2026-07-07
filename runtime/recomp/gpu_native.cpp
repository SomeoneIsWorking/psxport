#include "core.h"
#include "game.h"
#include "gpu_gpu.h"   // Core*-threaded VK present API (de-globalized R2)
#include "c_subsys.h"
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
#include "r3000.h"
#include "cfg.h"
#include "gpu_native_internal.h"   // shared VRAM/state/helpers (also used by gpu_debug.cpp)
#include "mods.h"                   // g_mods.fps60 (was g_fps60_on)
#include "render/render.h"          // Render::mDbgRenderNode (was g_dbg_render_node)
#include "scea_asset.h"            // baked SCEA license-screen texture+CLUT (PC-native boot splash)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>    // clock_gettime / nanosleep — portable PC-owned frame pacer (gpu_pace_subframe)
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#endif

// g_fps60_on retired — read g_mods.fps60 (mods.h; #included above)
// VRAM_W/VRAM_H and vram() now live in gpu_native_internal.h

// ---- Draw state (set by GP0 env commands E1..E6) ------------------------------------
int gpu_gpu_enabled(void);                                    // gpu_gpu.c (declared early for the gp0 tee)
// s_reddbg is a process-level env-gate cache (PSXPORT_REDDBG), so it stays file-scope.
static int s_reddbg;              // PSXPORT_REDDBG: dark-red output anomaly probe

// ---- Display control (GP1) ----------------------------------------------------------


static int s_log = 0;             // PSXPORT_GPU_LOG (process-level env-gate cache)
// s_prims moved to GpuState (deglobalize 2026-07-03) — was cross-core-shared per-frame draw counter.
// s_seen3d / s_prev_had3d are per-instance GpuState members now (gpu_native_internal.h). The OT walk
// tees geometry before present, so s_seen3d is final by the time the present/compose path queries it.
// A frame with no tee'd 3D is a VRAM-resident 2D screen (SCEA/FMV/title/menu) — nothing in the scratch FB.
int gpu_had3d_last_frame(Core* core) { return core->game->gpu.s_prev_had3d; }
int gpu_seen3d_this_frame(Core* core) { return core->game->gpu.s_seen3d; }
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
  return (w * 4 >= dw * 3) && (h * 4 >= dh * 3);    // covers >=3/4 of the display in both axes = backdrop
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
  if (dw <= 0) dw = 320; if (dh <= 0) dh = 240;
  int w = bx1 - bx0, h = by1 - by0;
  return (w * 4 >= dw * 3) && (h * 4 >= dh * 3);    // full-screen (>=3/4 both axes)
}
// M3 provenance: record [lo,hi) (KSEG0 packet-pool addresses) as a BACKGROUND drawer's output for the
// current frame. Stamps the frame so a stale span from a prior frame is never honored.
void GpuState::bg_range_add(uint32_t lo, uint32_t hi) {
  if (hi <= lo) return;
  if (s_bg_frame != s_frame) { s_bg_nrange = 0; s_bg_frame = s_frame; }  // new frame -> clear prior spans
  if (s_bg_nrange < BG_RANGE_MAX) { s_bg_lo[s_bg_nrange] = lo; s_bg_hi[s_bg_nrange] = hi; s_bg_nrange++; }
}
// Is this OT node inside a background drawer's span recorded THIS frame? (provenance backdrop test)
int GpuState::node_is_bg(uint32_t node) {
  if (s_bg_frame != s_frame) return 0;
  uint32_t n = node | 0x80000000u;
  for (int i = 0; i < s_bg_nrange; i++) if (n >= s_bg_lo[i] && n < s_bg_hi[i]) return 1;
  return 0;
}
// Public wrapper: the engine's background-drawer override (engine_submit.cpp ov_bg_tilemap) records the
// pool span it produced so the OT-walk classifies those prims as RQ_BACKGROUND by provenance.
void gpu_bg_range_add(Core* core, uint32_t lo, uint32_t hi) { core->game->gpu.bg_range_add(lo, hi); }

// TEXPAGE-PROVENANCE backdrop test (replaces the dead packet-span ov_bg_tilemap provenance). The native
// backdrop drawer (engine_submit.cpp ov_bg_tilemap_native) publishes the active sky/sea tilemap texpage
// here each frame; the OT-walk then recognizes the GUEST background drawer's redundant tiles (same texpage)
// and classifies them RQ_BACKGROUND so the field's 2D-only walk DROPS them (the native backdrop owns the
// sky/sea). Stamped per frame so a stale value from a prior frame/area is never honored. (render.md OPEN #1)
void gpu_bg_texpage_set(Core* core, int tp_x, int tp_y) {
  GpuState& s = core->game->gpu;
  s.s_bgtp_x = tp_x; s.s_bgtp_y = tp_y; s.s_bgtp_frame = s.s_frame;
}
// Does this sprite's texpage match THIS frame's published backdrop texpage? (redundant guest backdrop tile)
static int sprite_is_bg_texpage(Core* core, int tp_x, int tp_y) {
  GpuState& s = core->game->gpu;
  return s.s_bgtp_frame == s.s_frame && tp_x == s.s_bgtp_x && tp_y == s.s_bgtp_y;
}

// PC-native per-object depth: the engine's native render walk records each object's packet-pool span +
// its WORLD-POSITION view-depth, so a 2D billboard prim rasterized later (deferred OT walk) occludes by
// the object's real depth instead of sprite order. Stamped per frame (a stale span is never honored).
// g_od_add/hit/miss retired 2026-07-03 — Render::stats.odAdd/odHit/odMiss (RenderStats).
void GpuState::obj_depth_add(uint32_t lo, uint32_t hi, float ord) {
  if (hi <= lo) return;
  if (s_od_frame != s_frame) { s_od_n = 0; s_od_frame = s_frame; }   // new frame -> clear prior spans
  // The billboard now occludes by its TRUE world-position view-Z — no implicit camera-ward bias (issue #4).
  // The old blanket `ord += 1/512` nudge pushed EVERY free-standing billboard (flame/brazier/pickup) toward
  // the camera so it would sit in front of its host SURFACE — that was the wall-decal (#5 coplanar) bandaid,
  // and it is exactly what made the flame draw in front of the wall behind it that DOES carry real depth.
  // A free-standing billboard must sort by its real Z like any world quad, so it is occluded by nearer
  // geometry and occludes farther geometry. (The genuinely-coplanar decal case adds its own stable epsilon
  // at its OWN call site if it needs one; it is not a property of every object span.) The depth is already
  // the object's PC-native world view-Z (proj_obj_center_ord / object_world_view_depth → proj_pz_to_ord),
  // so this is engine-owned occlusion, never a PSX OT order. `ord` is stored AS GIVEN.
  if (ord < 0.0f) ord = 0.0f; if (ord > 1.0f) ord = 1.0f;
  if (s_od_n < OBJ_DEPTH_MAX) { s_od_lo[s_od_n] = lo; s_od_hi[s_od_n] = hi; s_od_ord[s_od_n] = ord; s_od_n++; game->core.mRender->stats.odAdd++; }
}
// PC-native COPLANAR FACE SEPARATION for OT-walked objects (issue #5 — barrel red/blue z-fight).
//
// A faceted object (barrel/container/crate) that renders through the GUEST OT walk — rather than the
// owned per-vertex GT3/GT4 float path — has ALL of its face packets in ONE packet-pool span tagged with a
// SINGLE whole-object world depth (object_world_view_depth, engine_submit.cpp). obj_depth_lookup then
// stamped the IDENTICAL `od` on every face (gp0_exec: `for(i) dep[i]=od`). With every face at the exact
// same D32 depth and a GREATER_OR_EQUAL test, which face wins a shared pixel is decided purely by draw
// order WITHIN the frame — and that order is not stable across the double-buffered queue swap → the red
// band and the blue band trade contested pixels each frame → the red/blue flicker. The whole-object depth
// is also COARSE (object_world_view_depth returns dot>>12, the 12-bit fraction discarded), so genuinely
// distinct faces a few units apart collapse to the same value too.
//
// The PROPER PC-owned fix is a STABLE, DETERMINISTIC per-FACE depth separation — an ordering RULE the
// engine owns, NOT a re-sync with the PSX OT and NOT the removed blanket camera-ward bias (issue #4):
// each consecutive prim that inherits the SAME object span gets a tiny monotonic camera-ward epsilon by
// its position in that object's submission run. The OT walk visits a given object's faces in the same
// link order every frame, so a given face ALWAYS gets the same epsilon → it wins/loses the contested
// pixel identically every frame → no flicker. Later faces (later in paint order) get a larger ord (=
// nearer, wins under GREATER_OR_EQUAL) → the same stacking the PSX painter produced, now stable.
//
// Why this does NOT reintroduce #4: a free-standing single-prim billboard (a flame/brazier/pickup quad)
// is the FIRST (and only) prim in its span → k==0 → epsilon 0 → it keeps its true world depth with NO
// camera-ward bias. The epsilon only separates MULTIPLE prims sharing ONE object span — exactly the
// faceted-object z-fight case, never the lone billboard. The per-vertex float world path (a barrel that
// IS owned-GT4) is untouched: it never calls obj_depth_lookup (it carries real per-vertex depth).
//
// EPS_STEP (1/65536 in pre-ord3d [0,1] units → ~1.3e-5 after ord3d's ×0.875 band map) is FAR above D32
// float resolution near these values (~1e-7) yet FAR below the gap between distinct world objects, and
// the run is capped so even a many-faced object can never drift a face in front of genuinely nearer geo.
// s_od_eps_frame/span/k moved to GpuState — see gpu_native_internal.h. Two SBS cores contaminate each
// other's per-face epsilon depth counter otherwise (deglobalize 2026-07-03).
static const float OD_EPS_STEP = 1.0f / 65536.0f;   // per-face camera-ward step (pre-ord3d ord units)
static const int   OD_EPS_KMAX = 256;               // cap the run so total drift stays << inter-object gap
int GpuState::obj_depth_lookup(uint32_t node, float* ord) {
  if (s_od_frame != s_frame) { game->core.mRender->stats.odMiss++; return 0; }
  uint32_t n = node | 0x80000000u;
  for (int i = 0; i < s_od_n; i++) if (n >= s_od_lo[i] && n < s_od_hi[i]) {
    // Stable per-face epsilon: reset the run on a new frame or when a DIFFERENT object span is hit; the
    // k-th consecutive face of the SAME span gets k camera-ward steps. Deterministic across frames.
    if (s_od_eps_frame != s_frame) { s_od_eps_frame = s_frame; s_od_eps_span = -1; s_od_eps_k = 0; }
    if (i != s_od_eps_span) { s_od_eps_span = i; s_od_eps_k = 0; }
    int k = s_od_eps_k < OD_EPS_KMAX ? s_od_eps_k : OD_EPS_KMAX;
    s_od_eps_k++;
    float v = s_od_ord[i] + (float)k * OD_EPS_STEP;     // camera-ward = larger ord (GREATER_OR_EQUAL wins)
    if (v > 1.0f) v = 1.0f;
    if (ord) *ord = v;
    game->core.mRender->stats.odHit++; return 1;
  }
  game->core.mRender->stats.odMiss++;
  return 0;
}
void gpu_obj_depth_add(Core* core, uint32_t lo, uint32_t hi, float ord) { core->game->gpu.obj_depth_add(lo, hi, ord); }
// s_gp0_words / s_dma2 moved to GpuState (per-Core; was cross-core-shared per-frame diag).
static int s_oracle_prim_log = 0;  // ORACLE diag (was g_oracle_prim_log): when >0, log each soft_gpu primitive
// g_nd_3d/nd_2d retired 2026-07-03 — Render::stats.nd3d/nd2d (RenderStats).
// 2D-OVERLAY-ONLY OT enumeration. When the FIELD render path owns the 3D world + backdrop natively
// (ov_scene_native), it STILL needs the guest's leftover 2D overlay prims — the opening-cutscene
// narration glyphs, in-game dialog/item bubbles, menus, HUD — which the field code submits as PSX
// sprites/polys into the OT. Those are not owned natively yet, so we enumerate them from the OT and
// queue them as RQ_HUD on top of the native world. Set during that 2nd OT walk so the gpu_gp0 prim
// classifier DROPS the 3D-world (RQ_WORLD) and backdrop (RQ_BACKGROUND) drawables (the native render
// already produced them — keeping them would DOUBLE-draw the world); only RQ_HUD 2D prims are queued.
// State commands (E1 texpage / E2 texwindow / draw-area/offset) are still applied for every node, so
// the kept 2D prims bind the correct texpage. (engine owns 3D + bg; guest OT supplies leftover 2D.)
// g_ot_2d_only retired 2026-07-03 — now a parameter of gpu_dma2_linked_list, stashed onto the walk's
// per-Core GpuState as s_ot_2d_only for gp0_exec to read, cleared at walk exit. (deglobalize-game)

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
int gpu_gpu_wide_engine_w(void);
// 2D X mapping for widescreen. The relocation shader (tritex.vert) already adds fb_x0 = margin*ss to
// every 2D vertex, so an UNMODIFIED PSX-320 prim lands in a centered [margin, margin+320] band. Therefore:
//   HUD/UI  -> identity: keep the native-320 X; the shader's +margin centers it in the wide FB (matches 4:3,
//              just centered). Previously this anchored thirds out to the wide edges (issue #16 smearing).
//   backdrop-> STRETCH to fill the wide FB (after the shader's +margin -> [0,fbw)) for sky/water backdrops.
// In 4:3 margin==0 -> identity for both (byte-identical).
static int ws_2d_local_x(int x, int is_bg) {
  int ww = gpu_gpu_wide_engine_w(), margin = (ww - 320) / 2;
  if (margin <= 0) return x;                          // 4:3 -> no-op
  if (is_bg) return x * ww / 320 - margin;            // backdrop: fill (after +margin -> [0,fbw))
  return x;                                           // HUD: identity (shader's +margin keeps it centered)
}

// PSXPORT_PRIMDUMP=<frame>: dump every prim drawn on that frame, in OT-walk order, to
// scratch/logs/prims_f<frame>.csv — one row per prim: id (walk order), kind, op, is3d, bg(ackdrop),
// bbox, rgb, textured, semi. Lets a human identify which IDs are the water/sky backdrop so the
// backdrop-vs-HUD band split can be made correct (and order-independent).
static FILE* s_primdump_f = 0; static int s_primdump_frame = -2;
static FILE* primdump_open(int frame) {
  if (s_primdump_frame == -2) { const char* e = cfg_str("PSXPORT_PRIMDUMP"); s_primdump_frame = e ? atoi(e) : -1; }
  if (s_primdump_frame < 0 || frame != s_primdump_frame) return 0;
  if (!s_primdump_f) {
    int r = system("mkdir -p scratch/logs"); (void)r;
    char p[128]; snprintf(p, sizeof p, "scratch/logs/prims_f%d.csv", frame);
    s_primdump_f = fopen(p, "w");
    if (s_primdump_f) fprintf(s_primdump_f, "id,kind,op,is3d,bg,x0,y0,x1,y1,r,g,b,tex,semi\n");
  }
  return s_primdump_f;
}
void prim_dump_poly(Core* core, int frame, unsigned id, uint8_t op, int nv, int is3d, int bg,
                    const int* xs, const int* ys, uint8_t r, uint8_t g, uint8_t b, int tex, int semi) {
  (void)core; FILE* f = primdump_open(frame); if (!f) return;
  int x0=xs[0],y0=ys[0],x1=xs[0],y1=ys[0];
  for (int i = 1; i < nv; i++) { if(xs[i]<x0)x0=xs[i]; if(xs[i]>x1)x1=xs[i]; if(ys[i]<y0)y0=ys[i]; if(ys[i]>y1)y1=ys[i]; }
  fprintf(f, "%u,poly,%02X,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", id, op, is3d, bg, x0,y0,x1,y1, r,g,b, tex, semi);
}
void prim_dump_sprite(Core* core, int frame, unsigned id, uint8_t op, int x, int y, int w, int h,
                      int bg, uint8_t r, uint8_t g, uint8_t b, int tex, int semi) {
  (void)core; FILE* f = primdump_open(frame); if (!f) return;
  fprintf(f, "%u,sprite,%02X,0,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", id, op, bg, x,y,x+w,y+h, r,g,b, tex, semi);
}
void prim_dump_close_if_done(int frame) {
  if (s_primdump_f && s_primdump_frame >= 0 && frame > s_primdump_frame) {
    fclose(s_primdump_f); s_primdump_f = 0;
    fprintf(stderr, "[primdump] wrote scratch/logs/prims_f%d.csv\n", s_primdump_frame);
  }
}
long g_nd2d_hist[256];           // op histogram of prims that fall to the 2D band (ndepth diag)

// Fade-flash diagnostic (PSXPORT_FADEDBG="a:b"): per-frame max emitted prim brightness + how the
// scene is drawn, to settle whether a bright fade frame is in the GP0 (engine emits it) or invented
// by VK. Works identically under SW and VK (same tee'd colors), so one playthrough pins the locus.
static int s_fade_maxc = 0, s_fade_npoly = 0, s_fade_nsemi = 0, s_fade_lasty = 0;
static int s_fade_semimax = -1, s_fade_semimin = 999, s_fade_bigsemi = 0;
static void fade_note(int r, int g, int b, int offy, int semi) {
  int m = r > g ? r : g; if (b > m) m = b;
  if (m > s_fade_maxc) s_fade_maxc = m;
  s_fade_npoly++; if (semi) { s_fade_nsemi++;
    if (m > s_fade_semimax) s_fade_semimax = m; if (m < s_fade_semimin) s_fade_semimin = m; }
  s_fade_lasty = offy;
}
// flag a semi prim wider than ~half the screen (a full-screen fade overlay tile)
static void fade_note_size(int w, int h, int semi) { if (semi && w >= 160 && h >= 120) s_fade_bigsemi++; }
// PSXPORT_SEMIDUMP=frame: log each SEMI prim (blend mode + color + bbox) at `frame`, to see how the
// fade overlay tiles stack (VK draws them all vs one snapshot, so stacked tiles don't accumulate).
void GpuState::semi_dump(const char* kind, int blend, int r, int g, int b, int x0, int y0, int x1, int y1, int offy) {
  static int sf = -2; if (sf == -2) { const char* e = cfg_str("PSXPORT_SEMIDUMP"); sf = e ? atoi(e) : -1; }
  if (sf >= 0 && s_frame == sf)
    fprintf(stderr, "[semidump] f%d %s blend=%d col=(%d,%d,%d) bbox=(%d,%d)-(%d,%d) offY=%d\n",
            s_frame, kind, blend, r, g, b, x0, y0, x1, y1, offy);
}

// ---- Per-pixel primitive provenance (PSXPORT_PROVAT="x,y[:frame]") --------------------------
// Records, for every VRAM pixel, the global id of the primitive that last wrote it. A wrong
// DISPLAYED pixel can then be traced to the exact prim that produced it — or shown to be STALE
// (last written many frames ago = revealed through a terrain/coverage gap, never overdrawn this
// frame). Queried in DISPLAY space at present time, which sidesteps the GPU double-buffer offset
// entirely (no more guessing which buffer / which native frame drew the shown pixel).
// ProvMeta / PROVRING and the s_prov / s_provmeta / s_prov_on state live in gpu_native_internal.h
// (shared with gpu_debug.c, which formats the provenance/scene dumps). Canonical defs here:
void gpu_provat_display(FILE* out, int qx, int qy);   // present-time provenance at display coords (gpu_debug.c)
// (gpu_provat_enable is a method on GpuState — no free-function fwd decl needed here)

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint16_t to555(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// ---- Semi-transparency (blend) ------------------------------------------------------
// PSX blends a source pixel (foreground, F) over the existing VRAM pixel (background, B)
// in 5-bit-per-channel space, using one of four modes selected by the texpage blend bits
// (s_tp_blend, also reachable per-poly via the prim's texpage). The formulas (per channel):
//   mode0: B/2 + F/2   mode1: B + F   mode2: B - F   mode3: B + F/4
// All results saturate to [0,31]. blend555() takes already-5-bit dest (existing VRAM 555,
// mask bit stripped) and 5-bit source channels, returns the blended 555 word.
static inline int sat5(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }
static inline uint16_t blend555(uint16_t bg, int fr, int fg, int fb, int mode) {
  int br = bg & 31, bgn = (bg >> 5) & 31, bb = (bg >> 10) & 31, rr, rg, rb;
  switch (mode) {
    case 0: rr = (br + fr) >> 1; rg = (bgn + fg) >> 1; rb = (bb + fb) >> 1; break;
    case 1: rr = sat5(br + fr); rg = sat5(bgn + fg); rb = sat5(bb + fb); break;
    case 2: rr = sat5(br - fr); rg = sat5(bgn - fg); rb = sat5(bb - fb); break;
    default: rr = sat5(br + (fr >> 2)); rg = sat5(bgn + (fg >> 2)); rb = sat5(bb + (fb >> 2)); break;
  }
  return (uint16_t)(rr | (rg << 5) | (rb << 10));
}

// Sample a texel at texture coords (u,v) through the current texpage/CLUT. Returns 0 if the
// texel is fully transparent (PSX: a 16-bit value of 0 = transparent).
uint16_t GpuState::sample_tex(int u, int v) {
  u = (u & ~(s_tw_mx * 8)) | ((s_tw_ox & s_tw_mx) * 8);  // texture window wrap
  v = (v & ~(s_tw_my * 8)) | ((s_tw_oy & s_tw_my) * 8);
  int bx = s_tp_x, by = s_tp_y;
  if (s_tp_mode == 2) return *vram(bx + u, by + v);       // 15bpp direct
  if (s_tp_mode == 1) {                                   // 8bpp: index -> CLUT
    uint16_t w = *vram(bx + (u >> 1), by + v);
    int idx = (u & 1) ? (w >> 8) : (w & 0xFF);
    return *vram(s_clut_x + idx, s_clut_y);
  }
  uint16_t w = *vram(bx + (u >> 2), by + v);              // 4bpp: nibble -> CLUT
  int idx = (w >> ((u & 3) * 4)) & 0xF;
  return *vram(s_clut_x + idx, s_clut_y);
}

// Write one pixel. If `semi` is set, blend the source (r,g,b) over the existing VRAM pixel
// using the current texpage blend mode (s_tp_blend); otherwise overwrite. The mask bit is
// always set on the written pixel (we don't model mask-test reads).
void GpuState::put_px_b(int x, int y, uint8_t r, uint8_t g, uint8_t b, int semi) {
  if (x < s_da_x0 || x > s_da_x1 || y < s_da_y0 || y > s_da_y1) return;
  uint16_t out;
  if (semi) out = blend555(*fb(x, y) & 0x7FFF, r >> 3, g >> 3, b >> 3, s_tp_blend);
  else out = to555(r, g, b);
  *fb(x, y) = out | 0x8000;
  if (s_prov_on > 0) s_prov[(y & 511) * VRAM_W + (x & 1023)] = s_prim_gid;
}
void GpuState::put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b) { put_px_b(x, y, r, g, b, 0); }

// PSX ordered 4x4 dither matrix (applied to 8-bit channels before 5-bit truncation, when
// the texpage dither bit is set, on gouraud + texture-modulated pixels). We add the per-pixel
// bias then clamp to [0,255] so the subsequent >>3 truncation effectively rounds.
static const int s_dither4[4][4] = {
  { -4,  0, -3,  1 },
  {  2, -2,  3, -1 },
  { -3,  1, -4,  0 },
  {  3, -1,  2, -2 },
};
static inline uint8_t dith(int v, int x, int y) {
  v += s_dither4[y & 3][x & 3];
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// ---- mednafen-exact triangle coverage (integer scanline edge-walk) ------------------
// To match the oracle's rasterizer COVERAGE exactly (which pixels a triangle claims), we
// replicate Beetle/mednafen's gpu_polygon.c edge-walk verbatim, rather than a half-space
// test. mednafen walks scanlines computing a fixed-point left/right edge per row and fills
// the span [x_start, x_bound) (left/top inclusive, right/bottom exclusive). A generic
// top-left half-space rule gets the DIRECTION right but not the exact sub-pixel endpoint
// rounding (MakePolyXFP/Step), so abutting prims still mis-claim a pixel here and there
// (journal: text-banner residual — our coverage over-claimed one edge, under-claimed
// another). Porting the exact integer math removes that variable entirely. These three
// helpers are mednafen's fixed-point edge primitives (COORD_FBS world, 32-frac fixed point).
static inline int64_t MakePolyXFP(int x) {
  return ((int64_t)x << 32) + (((int64_t)1 << 32) - (1 << 11));
}
static inline int64_t MakePolyXFPStep(int dx, int dy) {   // dy is always > 0 at our call sites
  int64_t dx_ex = (int64_t)dx << 32;
  if (dx_ex < 0) dx_ex -= dy - 1;
  if (dx_ex > 0) dx_ex += dy - 1;
  return dx_ex / dy;
}
static inline int GetPolyXFP_Int(int64_t xfp) { return (int)(xfp >> 32); }

// Shade + write ONE covered pixel of triangle (a,b,c) at integer screen (x,y). Coverage is
// decided by the caller (tri()); this only does the per-pixel math, which stays barycentric
// off the ORIGINAL (unsorted) a,b,c and the doubled signed area `aa` — already validated to
// match Beetle's per-pixel output (modulation/UV-round/dither). `tex`/`shade`/`semi` as tri().
void GpuState::tri_px(Vtx a, Vtx b, Vtx c, int x, int y, int tex, int shade, int semi, int raw, long aa) {
      long l0 = (long)((b.x-x)*(c.y-y)-(b.y-y)*(c.x-x));
      long l1 = (long)((c.x-x)*(a.y-y)-(c.y-y)*(a.x-x));
      long l2 = aa - l0 - l1;
      uint8_t r, g, bl;
      int px_semi = semi;                           // whether THIS pixel blends
      int dithered = 0;                             // PSX dithers gouraud + modulated-texture
      int pt_u = 0, pt_v = 0; uint16_t pt_t = 0;    // PSXPORT_PIXTRACE capture
      int pt_cr = a.r, pt_cg = a.g, pt_cb = a.b;    // interpolated modulation color (set below)
      if (tex) {
        // Affine UV, ROUND-TO-NEAREST (not truncate): PSX/Beetle add a +0.5-texel bias before the
        // integer truncation (gpu_polygon.c affine seed `+(1<<(COORD_FBS-1))`), i.e. sample the
        // nearest texel. Truncating instead biases sampling half a texel toward the origin, picking a
        // neighbouring texel at fractional coords (journal later-44 residual). Round in sign-
        // normalized form since `aa` (doubled area) may be negative.
        long su = l0*a.u + l1*b.u + l2*c.u, sv = l0*a.v + l1*b.v + l2*c.v, den = aa;
        if (den < 0) { su = -su; sv = -sv; den = -den; }
        int u = (int)((su + den/2) / den);
        int v = (int)((sv + den/2) / den);
        uint16_t t = sample_tex(u, v);
        pt_u = u; pt_v = v; pt_t = t;
        if (t == 0) return;                         // transparent texel — skip this pixel
        // PSX: a textured pixel blends only when its bit15 is set AND the prim semi bit is set.
        px_semi = semi && (t & 0x8000);
        r = (t & 31) << 3; g = ((t >> 5) & 31) << 3; bl = ((t >> 10) & 31) << 3;
        // RAW TEXTURE (PSX poly cmd bit0 = texture-blend-disable): output the texel verbatim — NO
        // modulation by vertex color and NO dither. Beetle's TM0 template path does exactly this
        // (journal: the op-2D banner-board residual — ours modulated raw texel 2E12 by the command
        // color (168,72,31) → near-black, while Beetle left it raw (18,16,11)). Same bit0 gating
        // the sprite path already honors (commit fb0c228); the polygon path was missing it.
        if (!raw) {
          // texture*color modulation (texel * vertexcolor / 128). PSX textured polygons modulate
          // the texel by the vertex color, INTERPOLATED per pixel across the face (the command color
          // for flat-shaded prims, where all vertices carry it). The modulation color must be the
          // barycentric-interpolated (cr,cg,cb), NOT vertex A's color held flat — using v0 flat
          // collapses a gouraud gradient (a soft shadow quad: dark center vertex, bright edges) into
          // a uniform block (journal later 44: black-wedge shadow). PSX hardware SATURATES the
          // product to 0xFF; doing it in uint8_t wraps mod 256, turning a bright grass texel red, so
          // compute wide and clamp (the grass red-block bug, journal later 42).
          int cr = (int)((l0*a.r + l1*b.r + l2*c.r) / aa);
          int cg = (int)((l0*a.g + l1*b.g + l2*c.g) / aa);
          int cb = (int)((l0*a.b + l1*b.b + l2*c.b) / aa);
          pt_cr = cr; pt_cg = cg; pt_cb = cb;
          int rr = r * cr / 128, gg = g * cg / 128, bb = bl * cb / 128;
          r = rr > 255 ? 255 : rr; g = gg > 255 ? 255 : gg; bl = bb > 255 ? 255 : bb;
          dithered = 1;
        } else { pt_cr = pt_cg = pt_cb = 128; }   // raw: undithered texel, modulation color = neutral
      } else if (shade) {
        r = (uint8_t)((l0*a.r + l1*b.r + l2*c.r) / aa);
        g = (uint8_t)((l0*a.g + l1*b.g + l2*c.g) / aa);
        bl = (uint8_t)((l0*a.b + l1*b.b + l2*c.b) / aa);
        dithered = 1;
      } else { r = a.r; g = a.g; bl = a.b; }
      if (s_tp_dither && dithered) { r = dith(r, x, y); g = dith(g, x, y); bl = dith(bl, x, y); }
      // PSXPORT_PIXTRACE="vx,vy": dump every prim that writes this absolute VRAM pixel (post-offset),
      // with its sampled texel + interpolated color + modulated output — for per-pixel-math diffing
      // against Beetle's gpu_polygon.c (which carries the matching [pixtrace beetle] log).
      { static int tx = -2, ty; if (tx == -2) { const char* e = cfg_str("PSXPORT_PIXTRACE");
          if (e) sscanf(e, "%d,%d", &tx, &ty); else tx = -1; }
        if (tx >= 0 && x == tx && y == ty)
          fprintf(stderr, "[pixtrace ours] (%d,%d) tex=%d shade=%d semi=%d px_semi=%d blend=%d dith=%d "
                  "uv=(%d,%d) texel=%04X out8=(%d,%d,%d) out5=(%d,%d,%d) vcol=(%d,%d,%d)\n",
                  x, y, tex, shade, semi, px_semi, s_tp_blend, (s_tp_dither && dithered),
                  pt_u, pt_v, pt_t, r, g, bl, r >> 3, g >> 3, bl >> 3, pt_cr, pt_cg, pt_cb); }
      // REDDBG: dark-red output anomaly probe (grass blocks). Log the prim's params once.
      if (s_reddbg && tex && r >= 64 && g < 24 && bl < 24 && x >= s_da_x0 && x <= s_da_x1) {
        static int n = 0;
        if (n++ < 6) {
          int uu = (int)((l0*a.u + l1*b.u + l2*c.u) / aa);
          int vv = (int)((l0*a.v + l1*b.v + l2*c.v) / aa);
          fprintf(stderr, "[reddbg] @(%d,%d) out=(%d,%d,%d) tpmode=%d clut=(%d,%d) tp=(%d,%d) uv=(%d,%d)\n",
                  x, y, r, g, bl, s_tp_mode, s_clut_x, s_clut_y, s_tp_x, s_tp_y, uu, vv);
          fprintf(stderr, "[reddbg]   palette[16]@(%d,%d):", s_clut_x, s_clut_y);
          for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", *vram(s_clut_x + k, s_clut_y));
          fprintf(stderr, "\n[reddbg]   texrow@(%d,%d) words:", s_tp_x + (uu>>2), s_tp_y + vv);
          for (int k = 0; k < 8; k++) fprintf(stderr, " %04X", *vram(s_tp_x + (uu>>2) + k, s_tp_y + vv));
          fprintf(stderr, "\n");
        }
      }
      put_px_b(x, y, r, g, bl, px_semi);
}

// Rasterize a gouraud/textured triangle. `tex` selects textured sampling, `semi` requests
// semi-transparency. Coverage = mednafen's exact integer edge-walk (so it matches the oracle
// pixel-for-pixel); per-pixel shading = tri_px (barycentric off the original a,b,c).
void GpuState::tri(Vtx a, Vtx b, Vtx c, int tex, int shade, int semi, int raw) {
  a.x += s_off_x; a.y += s_off_y; b.x += s_off_x; b.y += s_off_y; c.x += s_off_x; c.y += s_off_y;
  long aa = (long)((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x));
  if (aa == 0) return;                          // degenerate (zero area)

  // --- Exact port of mednafen's DEFINE_DrawTriangle coverage (gpu_polygon.c). Operates on a
  // y-sorted copy of the vertices; shading (tri_px) still uses the original a,b,c order. ---
  int vx[3] = { a.x, b.x, c.x }, vy[3] = { a.y, b.y, c.y };
  unsigned cvtemp;                               // "core vertex" select (rasterisation order)
  if (vx[1] <= vx[0]) cvtemp = (vx[2] <= vx[1]) ? (1u << 2) : (1u << 1);
  else if (vx[2] < vx[0]) cvtemp = (1u << 2);
  else cvtemp = (1u << 0);
  #define VSWAP(i,j) do { int t; t=vx[i];vx[i]=vx[j];vx[j]=t; t=vy[i];vy[i]=vy[j];vy[j]=t; } while (0)
  if (vy[2] < vy[1]) { VSWAP(2,1); cvtemp = ((cvtemp>>1)&0x2)|((cvtemp<<1)&0x4)|(cvtemp&0x1); }
  if (vy[1] < vy[0]) { VSWAP(1,0); cvtemp = ((cvtemp>>1)&0x1)|((cvtemp<<1)&0x2)|(cvtemp&0x4); }
  if (vy[2] < vy[1]) { VSWAP(2,1); cvtemp = ((cvtemp>>1)&0x2)|((cvtemp<<1)&0x4)|(cvtemp&0x1); }
  #undef VSWAP
  unsigned core_vertex = cvtemp >> 1;
  if (vy[0] == vy[2]) return;                    // 0-height after sort

  int64_t base_coord = MakePolyXFP(vx[0]);
  int64_t base_step  = MakePolyXFPStep(vx[2]-vx[0], vy[2]-vy[0]);
  int64_t bound_coord_us, bound_coord_ls;
  int right_facing;
  if (vy[1] == vy[0]) { bound_coord_us = 0; right_facing = (vx[1] > vx[0]); }
  else { bound_coord_us = MakePolyXFPStep(vx[1]-vx[0], vy[1]-vy[0]); right_facing = (bound_coord_us > base_step); }
  bound_coord_ls = (vy[2] == vy[1]) ? 0 : MakePolyXFPStep(vx[2]-vx[1], vy[2]-vy[1]);

  unsigned vo = core_vertex ? 1 : 0;
  unsigned vp = (core_vertex == 2) ? 3 : 0;
  struct { int64_t x_coord[2], x_step[2]; int y_coord, y_bound, dec_mode; } tp[2];
  { int k = vo;
    tp[k].y_coord = vy[0 ^ vo]; tp[k].y_bound = vy[1 ^ vo];
    tp[k].x_coord[right_facing] = MakePolyXFP(vx[0 ^ vo]);
    tp[k].x_step[right_facing] = bound_coord_us;
    tp[k].x_coord[!right_facing] = base_coord + (int64_t)(vy[vo]-vy[0]) * base_step;
    tp[k].x_step[!right_facing] = base_step;
    tp[k].dec_mode = (vo != 0); }
  { int k = vo ^ 1;
    tp[k].y_coord = vy[1 ^ vp]; tp[k].y_bound = vy[2 ^ vp];
    tp[k].x_coord[right_facing] = MakePolyXFP(vx[1 ^ vp]);
    tp[k].x_step[right_facing] = bound_coord_ls;
    tp[k].x_coord[!right_facing] = base_coord + (int64_t)(vy[1 ^ vp]-vy[0]) * base_step;
    tp[k].x_step[!right_facing] = base_step;
    tp[k].dec_mode = (vp != 0); }

  for (int i = 0; i < 2; i++) {
    int yi = tp[i].y_coord, yb = tp[i].y_bound;
    int64_t lc = tp[i].x_coord[0], ls = tp[i].x_step[0];
    int64_t rc = tp[i].x_coord[1], rs = tp[i].x_step[1];
    if (tp[i].dec_mode) {
      while (yi > yb) {
        yi--; lc -= ls; rc -= rs;
        if (yi < s_da_y0) break;
        if (yi > s_da_y1) continue;
        int xs = GetPolyXFP_Int(lc), xb = GetPolyXFP_Int(rc);
        if (xs < s_da_x0) xs = s_da_x0;
        if (xb > s_da_x1 + 1) xb = s_da_x1 + 1;
        for (int x = xs; x < xb; x++) tri_px(a, b, c, x, yi, tex, shade, semi, raw, aa);
      }
    } else {
      while (yi < yb) {
        if (yi > s_da_y1) break;
        if (yi >= s_da_y0) {
          int xs = GetPolyXFP_Int(lc), xb = GetPolyXFP_Int(rc);
          if (xs < s_da_x0) xs = s_da_x0;
          if (xb > s_da_x1 + 1) xb = s_da_x1 + 1;
          for (int x = xs; x < xb; x++) tri_px(a, b, c, x, yi, tex, shade, semi, raw, aa);
        }
        yi++; lc += ls; rc += rs;
      }
    }
  }
}

// ---- GP0 command FIFO ---------------------------------------------------------------
// VRAM transfer state (GP0 0xA0 CPU->VRAM)

// PC-native CPU->VRAM upload. The game's libgs-style upload library (FUN_80081218 and the
// GsSortObject ring at 0x800A5AC8) is replaced by writing the rect directly here, so the GPU
// library does not need to be a faithful recomp. `src` is a RAM (or physical) address holding
// w*h contiguous 16-bit pixels, row-major; mem_r16 masks the region so KSEG0/physical both work.
// Identical effect to the GP0 0xA0 stream below, minus the FIFO/DMA round-trip.
// Transplant harness: overwrite our full VRAM from a raw 1024x512x16 dump (oracle's, via
// PSXPORT_VRAMDUMP). Lets us drop the oracle's clean green-field VRAM into our running port and
// watch whether our continued execution keeps it clean or re-corrupts (accumulation test).
int GpuState::gpu_native_load_vram(const char* path) {
  FILE* f = fopen(path, "rb"); if (!f) return 0;
  size_t n = fread(s_vram, 2, (size_t)VRAM_W * VRAM_H, f); fclose(f);
  fprintf(stderr, "[transplant] loaded VRAM %zu px from %s\n", n, path);
  return n == (size_t)VRAM_W * VRAM_H;
}
void GpuState::gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src) {
  // VRAM-transfer guard: bounds-report + atlas-clobber catch (vram_xfer.cpp, `debug vramguard`). This is
  // the single CPU->VRAM upload chokepoint for every texture-group page, font page, and CLUT, so it is
  // both the place to REGISTER the protected atlas regions and a transfer to validate. Diagnostic only.
  vram_guard_check(core, "native", x, y, w, h, src);
  // Register texture-region uploads (anything right of the two 320-wide framebuffers) as a protected,
  // resident page: these are exactly the atlas/font/CLUT rects a later draw samples; a non-upload write
  // landing on one is the stripe-corruption clobber. Framebuffer uploads (x<320) are NOT atlas data.
  if (x >= 320) vram_register_atlas(x, y, w, h, (w <= 16) ? "clut" : "atlas");
  for (int v = 0; v < h; v++)
    for (int u = 0; u < w; u++)
      *vram(x + u, y + v) = core->mem_r16(src + (uint32_t)((v * w + u) * 2));
  // Mirror the upload into the VK VRAM image, exactly like the GP0 0xA0 / VRAM-copy / fill paths.
  // This native upload is a VRAM-writing path too; without the mirror its textures land only in the
  // SW s_vram. The VK opaque pass samples a full s_vram snapshot so it still saw them, but the VK
  // SEMI pass samples the post-opaque s_tex (dirty regions only) — so a SEMI-transparent textured
  // prim whose texture arrived here read zeros and discarded (the invisible in-game puddle water).
  if (gpu_gpu_enabled()) gpu_gpu_dirty(core, x, y, w, h);
  if (cfg_dbg("upload"))
    fprintf(stderr, "[upload] f%d NATIVE dest=(%d,%d) %dx%d src=0x%08X\n", s_frame, x, y, w, h, src);
}

// GP0 command-word color packs as 0x00BBGGRR — R in the low byte, B in the high byte.
static inline uint8_t cmd_r(uint32_t c) { return c & 0xFF; }
static inline uint8_t cmd_g(uint32_t c) { return (c >> 8) & 0xFF; }
static inline uint8_t cmd_b(uint32_t c) { return (c >> 16) & 0xFF; }
static inline int cx(uint32_t w) { int v = w & 0x7FF; return v >= 0x400 ? v - 0x800 : v; }
static inline int cy(uint32_t w) { int v = (w >> 16) & 0x7FF; return v >= 0x400 ? v - 0x800 : v; }

void GpuState::set_texpage(uint16_t tp) {
  s_tp_x = (tp & 0xF) * 64;
  s_tp_y = ((tp >> 4) & 1) * 256;
  s_tp_blend = (tp >> 5) & 3;
  s_tp_mode = (tp >> 7) & 3; if (s_tp_mode > 2) s_tp_mode = 2;
  s_tp_dither = (tp >> 9) & 1;     // ordered 4x4 dither enable
}
void GpuState::set_clut(uint16_t cl) { s_clut_x = (cl & 0x3F) * 16; s_clut_y = (cl >> 6) & 0x1FF; }

// sv (optional, NULL = no shadow): the prim's 4 VIEW-SPACE verts (x=vx, y=vy, z=pz) for the shadow map.
// When non-NULL and opaque, the queued item carries them and RenderQueue::emitItem re-pushes them as two tris
// to the shadow VBO on every emit (= on both 60fps present passes — see render_queue.h sh_cast).

// RenderQueue::emitOrQueue (game/render/render_queue.cpp) is the one place this file's guest
// GP0/OT-walk poly + sprite submit paths (below) funnel their queued items through.

// Begin a primitive for provenance tracking: bump the global id and record this prim's details
// (frame/node/op/clut/texpage/color/first-vertex) so put_px_b can stamp each pixel it writes.
void GpuState::prov_begin(uint8_t op, int tex, int semi, uint8_t r, uint8_t g, uint8_t b,
                       int x0, int y0, int u0, int v0) {
  if (s_prov_on < 0) s_prov_on = cfg_str("PSXPORT_PROVAT") ? 1 : 0;
  if (!s_prov_on) return;
  s_prim_gid++;
  ProvMeta* m = &s_provmeta[s_prim_gid % PROVRING];
  m->gid = s_prim_gid; m->frame = (uint32_t)s_frame; m->node = s_cur_node; m->op = op;
  m->clut_x = s_clut_x; m->clut_y = s_clut_y; m->tp_x = s_tp_x; m->tp_y = s_tp_y;
  m->x0 = x0; m->y0 = y0; m->u0 = u0; m->v0 = v0; m->r = r; m->g = g; m->b = b;
  m->semi = (uint8_t)semi; m->tex = (uint8_t)tex; m->mode = (uint8_t)s_tp_mode;
  m->blend = (uint8_t)s_tp_blend;
}

// PSXPORT_CLUTWATCH[=x,y] — log every VRAM upload whose rect covers a watched CLUT point
// (default 880,507 = the wrong grass palette found via the oracle compare, journal later 39),
// in order, with the resulting 16-entry palette. Reveals whether the right palette is written
// then overwritten, or never written, and by which transfer.
static int s_cw_x = -1, s_cw_y = 0, s_cw_pending = 0;
void GpuState::clutwatch_dump(const char* tag, int rx, int ry, int rw, int rh) {
  fprintf(stderr, "[clutwatch] %s f%d rect=(%d,%d %dx%d) covers (%d,%d) palette:",
          tag, s_frame, rx, ry, rw, rh, s_cw_x, s_cw_y);
  for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", *vram(s_cw_x + k, s_cw_y));
  fprintf(stderr, "\n");
}
static int clutwatch_covers(int rx, int ry, int rw, int rh) {
  if (s_cw_x < 0) return 0;
  return s_cw_y >= ry && s_cw_y < ry + rh && s_cw_x >= rx && s_cw_x < rx + rw;
}
// For 0xA0 the pixels stream in AFTER setup, so mark pending and dump on completion; for 0x80 the
// copy already happened, so dump immediately.
void GpuState::clutwatch_xfer(const char* tag, int rx, int ry, int rw, int rh) {
  if (!clutwatch_covers(rx, ry, rw, rh)) return;
  if (tag[0] == 'A') { s_cw_pending = 1; fprintf(stderr,
      "[clutwatch] A0 upload START f%d rect=(%d,%d %dx%d) covers (%d,%d)\n",
      s_frame, rx, ry, rw, rh, s_cw_x, s_cw_y); }
  else clutwatch_dump(tag, rx, ry, rw, rh);
}

// PSXPORT_TEXWATCH="x0,y0,x1,y1" — log every A0 CPU->VRAM upload or 0x80 VRAM->VRAM copy whose
// DEST rect overlaps the watched VRAM rect (e.g. a character's texpage), with frame, dest rect,
// DMA source addr, and the first source/dest bytes. Traces exactly when a model's texture pixels
// change and what data fed them (gameplay sprite/CLUT corruption hunt).
static int s_tw_init = 0, s_tw_x0 = -1, s_tw_y0 = 0, s_tw_x1 = 0, s_tw_y1 = 0;
static int texwatch_overlap(int rx, int ry, int rw, int rh) {
  if (!s_tw_init) {
    s_tw_init = 1;
    const char* e = cfg_str("PSXPORT_TEXWATCH");
    if (e) sscanf(e, "%d,%d,%d,%d", &s_tw_x0, &s_tw_y0, &s_tw_x1, &s_tw_y1);
  }
  if (s_tw_x0 < 0) return 0;
  return rx < s_tw_x1 && rx + rw > s_tw_x0 && ry < s_tw_y1 && ry + rh > s_tw_y0;
}

// Rasterize one sprite/rect with the CURRENT draw state (s_off, s_da clip, texpage via sample_tex,
// command color). Shared by gp0_exec and the fps60 in-between synthesizer so both go through the
// exact same texel/blend/clip logic. (op bit0 = raw-texel select; semi = semi-transparency.)
void GpuState::raster_sprite(int op, int x, int y, int u0, int v0, int w, int h,
                          uint8_t cr, uint8_t cg, uint8_t cb, int textured, int semi) {
  // Clip the iteration to the drawing area up front: off-screen sprites otherwise spin w*h
  // sample_tex calls for pixels put_px_b would discard (could burn millions of iterations).
  int dx0 = 0, dx1 = w, dy0 = 0, dy1 = h;
  if (s_da_x0 - x - s_off_x > dx0) dx0 = s_da_x0 - x - s_off_x;
  if (s_da_x1 - x - s_off_x + 1 < dx1) dx1 = s_da_x1 - x - s_off_x + 1;
  if (s_da_y0 - y - s_off_y > dy0) dy0 = s_da_y0 - y - s_off_y;
  if (s_da_y1 - y - s_off_y + 1 < dy1) dy1 = s_da_y1 - y - s_off_y + 1;
  for (int dy = dy0; dy < dy1; dy++)
    for (int dx = dx0; dx < dx1; dx++) {
      if (textured) {
        uint16_t t = sample_tex(u0 + dx, v0 + dy);
        if (t == 0) continue;                     // transparent texel
        int px_semi = semi && (t & 0x8000);
        int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
        if (!(op & 1)) {                          // bit0=0 -> modulate texel by command color
          tr = tr * cr / 128; tg = tg * cg / 128; tb = tb * cb / 128;
          if (tr > 255) tr = 255; if (tg > 255) tg = 255; if (tb > 255) tb = 255;
        }
        put_px_b(x + dx + s_off_x, y + dy + s_off_y, tr, tg, tb, px_semi);
      } else put_px_b(x + dx + s_off_x, y + dy + s_off_y, cr, cg, cb, semi);
    }
}

// PC-native texture EXPORT (proves the texture DECODE is owned, not the PSX's). Decodes a w×h block of
// texels at the CURRENT texpage (s_tp_x/y, s_tp_mode) through the CURRENT CLUT (s_clut_x/y) with MY OWN
// decoder — the same CLUT/bit-depth logic as sample_tex but standalone — and writes an RGB PPM to
// scratch/export/. The texels come from VRAM that the PC-owned upload chain (lz_decompress → group unpack
// → ov_upload_image) filled, so neither the decompression nor the CLUT decode runs PSX code.
void GpuState::tex_export(const char* name, int u0, int v0, int w, int h) {
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
  char dir[] = "scratch/export"; { char cmd[64]; snprintf(cmd, sizeof cmd, "mkdir -p %s", dir); int r = system(cmd); (void)r; }
  char path[256]; snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  FILE* f = fopen(path, "wb"); if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int dy = 0; dy < h; dy++)
    for (int dx = 0; dx < w; dx++) {
      int u = u0 + dx, v = v0 + dy;
      uint16_t t;
      if (s_tp_mode == 2) t = *vram(s_tp_x + u, s_tp_y + v);                  // 15bpp direct
      else if (s_tp_mode == 1) {                                             // 8bpp -> CLUT
        uint16_t word = *vram(s_tp_x + (u >> 1), s_tp_y + v);
        int idx = (u & 1) ? (word >> 8) : (word & 0xFF);
        t = *vram(s_clut_x + idx, s_clut_y);
      } else {                                                               // 4bpp -> CLUT
        uint16_t word = *vram(s_tp_x + (u >> 2), s_tp_y + v);
        int idx = (word >> ((u & 3) * 4)) & 0xF;
        t = *vram(s_clut_x + idx, s_clut_y);
      }
      unsigned char rgb[3] = { (unsigned char)((t & 31) << 3),
                               (unsigned char)(((t >> 5) & 31) << 3),
                               (unsigned char)(((t >> 10) & 31) << 3) };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  fprintf(stderr, "[texexport] wrote %s (%dx%d, tp=(%d,%d) mode=%d clut=(%d,%d) uv0=(%d,%d))\n",
          path, w, h, s_tp_x, s_tp_y, s_tp_mode, s_clut_x, s_clut_y, u0, v0);
}

// Rasterize one flat line segment with the CURRENT draw state (s_off, clip). Shared by gp0_exec and
// the fps60 synthesizer so poly-lines are reproduced in the interpolated frame (else they flicker).
void GpuState::raster_line(int x0, int y0, int x1, int y1, uint8_t cr, uint8_t cg, uint8_t cb, int semi) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) { put_px_b(x0 + s_off_x, y0 + s_off_y, cr, cg, cb, semi); if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}

// Execute a complete GP0 primitive packet held in s_fifo[0..s_fcount).
void GpuState::gp0_exec(Core* core) {
  uint32_t c = s_fifo[0];
  uint8_t op = c >> 24;
  if (op >= 0x20 && op <= 0x3F) {            // polygon
    int gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = (op & 0x02) ? 1 : 0;
    int raw = textured && (op & 0x01);      // bit0 = texture-blend-disable (raw texel, no modulation)
    int nv = quad ? 4 : 3;
    Vtx v[4]; uint32_t vaddr[4]; int idx = 1;
    for (int i = 0; i < nv; i++) {
      uint8_t cr, cg, cb;
      if (gouraud) { uint32_t col = (i == 0) ? c : s_fifo[idx++]; cr = cmd_r(col); cg = cmd_g(col); cb = cmd_b(col); }
      else { cr = cmd_r(c); cg = cmd_g(c); cb = cmd_b(c); }
      vaddr[i] = s_fifo_addr[idx];          // guest addr of this vertex's XY word (Phase-1 attach)
      uint32_t xy = s_fifo[idx++];
      v[i].x = cx(xy); v[i].y = cy(xy); v[i].r = cr; v[i].g = cg; v[i].b = cb;
      if (textured) {
        uint32_t uv = s_fifo[idx++];
        v[i].u = uv & 0xFF; v[i].v = (uv >> 8) & 0xFF;
        if (i == 0) set_clut((uv >> 16) & 0xFFFF);
        if (i == 1) set_texpage((uv >> 16) & 0xFFFF);
      }
    }
    int shade = gouraud || !textured;       // flat-untextured uses the command color
    { int mr=0,mg=0,mb=0,xmn=99999,xmx=-99999,ymn=99999,ymx=-99999;
      for (int i=0;i<nv;i++){ if(v[i].r>mr)mr=v[i].r; if(v[i].g>mg)mg=v[i].g; if(v[i].b>mb)mb=v[i].b;
        if(v[i].x<xmn)xmn=v[i].x; if(v[i].x>xmx)xmx=v[i].x; if(v[i].y<ymn)ymn=v[i].y; if(v[i].y>ymx)ymx=v[i].y; }
      fade_note(mr, mg, mb, s_off_y, semi); fade_note_size(xmx-xmn, ymx-ymn, semi);
      if (semi) semi_dump("poly", s_tp_blend, mr, mg, mb, xmn, ymn, xmx, ymx, s_off_y); }
    core->game->fps60.join_poly(v[0].x, v[0].y);  // fps60: object join
    // VK backend (M5): tee polys to the GPU rasterizer in absolute VRAM coords. Opaque textured/
    // untextured -> opaque batch; semi -> semi batch (mode 3 = untextured flat). VK owns these now.
    if (vk_path()) {
      unsigned ord_idx = s_prim_order++;
      int xs[4], ys[4], us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
      for (int i = 0; i < nv; i++) { xs[i]=v[i].x+s_off_x; ys[i]=v[i].y+s_off_y; us[i]=v[i].u; vs[i]=v[i].v;
                                     rs[i]=v[i].r; gs[i]=v[i].g; bs[i]=v[i].b; }
      int mode = textured ? s_tp_mode : 3, rw = raw ? 1 : 0;
      // Classify 3D-vs-2D (and 2D backdrop-vs-HUD) once; this drives both the inline draw and the engine
      // render queue. Phase 2 (NATIVE_DEPTH): a poly whose every vertex resolves to a projected vertex is
      // 3D world geometry -> carries real per-vertex view-Z (D32 occlusion); otherwise it is a 2D element
      // -> a backdrop (FAR band) or HUD (near band) by screen coverage.
      int use_rq = rq_active();                  // engine render queue owns ordering (PSXPORT_RQ)
      float dep[4]; int is3d = 0, bg = 0, billboard = 0;   // billboard = a 2D prim that got obj_depth (fps60 anchor)
      int fade_full = 0;                         // full-screen SEMI overlay (fade/dim) -> stretch-to-fill, stay on top (#21)
      {
        float proj_pz_to_ord(float);
        is3d = 1;
        for (int i = 0; i < nv; i++) {
          float pz;
          if (vaddr[i] && core->mRender->projprim.lookupPz(vaddr[i], &pz)) dep[i] = proj_pz_to_ord(pz);
          else { is3d = 0; break; }
        }
        if (is3d) s_seen3d = 1;            // a projected world prim has now been drawn this frame
        int bx0=xs[0],by0=ys[0],bx1=xs[0],by1=ys[0];
        for (int i = 1; i < nv; i++) { if(xs[i]<bx0)bx0=xs[i]; if(xs[i]>bx1)bx1=xs[i]; if(ys[i]<by0)by0=ys[i]; if(ys[i]>by1)by1=ys[i]; }
        // 2D prim: backdrop vs HUD. PROVENANCE first — a node produced by the engine's owned background
        // drawer is the backdrop regardless of size (fixes the tiled background, blind to bg_2d's coverage
        // test); fall back to screen-coverage for scenes whose background drawer isn't owned yet.
        // A full-screen prim is a BACKDROP only if OPAQUE (sky/sea). A full-screen SEMI prim is a
        // fade/overlay -> must NOT be a backdrop (else it draws UNDER the world); leave it in the HUD
        // (topmost) band so fades composite on top. (Owned backdrops still match via node_is_bg.)
        if (!is3d) bg = node_is_bg(s_cur_node) || (!semi && bg_2d(bx0, by0, bx1, by1));
        // PSXPORT_BDTAG: attribute a DEFERRED PSX gp0 (is3d=0) prim to the build-time call that produced it
        // (ffspan_lookup maps the packet addr to a recorded pool span). Maps the remaining PSX render so the
        // next ownership target is picked by data. Dedups by (builder,tp) and only after the field settles
        // (s_frame>=120, so transition/load frames don't dominate). Logs each unique (builder,tp) once.
        if (!is3d && cfg_str("PSXPORT_BDTAG") && s_frame >= 120) {
          const char* t = core->game->ffspan.lookup(s_cur_node);
          static struct { const char* t; int tx, ty; } seen[64]; static int nseen = 0; int f = 0;
          for (int i = 0; i < nseen; i++) if (seen[i].t == t && seen[i].tx == s_tp_x && seen[i].ty == s_tp_y) { f = 1; break; }
          if (!f && nseen < 64) { seen[nseen].t = t; seen[nseen].tx = s_tp_x; seen[nseen].ty = s_tp_y; nseen++;
            fprintf(stderr, "[bdtag] PSX is3d=0 op=%02x tp=(%d,%d) built by '%s'\n", op, s_tp_x, s_tp_y, t); } }
        // FADE/DIM (#21): a full-screen SEMI prim is a fade/dim overlay, NOT a backdrop. It must cover the
        // WHOLE wide FB (else green field shows in the widescreen margins) but composite ON TOP (HUD band).
        // Tag it so the 2D-X mapping below stretches it to fill while the layer stays topmost.
        if (!is3d && !bg && semi && fade_full_2d(s_disp_w, s_disp_h, bx0, by0, bx1, by1)) fade_full = 1;
        // PC-native object depth: a 2D billboard prim (no projected verts) whose OT-node falls in an
        // object's packet-pool span inherits that object's world-position view-Z and occludes for real.
        if (!is3d && !bg) { float od; if (obj_depth_lookup(s_cur_node, &od)) {
          for (int i = 0; i < nv; i++) dep[i] = od; is3d = 1; s_seen3d = 1; billboard = 1; } }
        if (!is3d && cfg_dbg("ndepth")) {   // categorize what lands in the 2D band: op + gouraud/quad/tex
          extern long g_nd2d_hist[256]; g_nd2d_hist[op]++; }
        if (!is3d && !bg && cfg_dbg("objz") && s_frame == s_primdump_frame)
          fprintf(stderr, "[polynode] id=%u op=%02x bbox=(%d,%d)-(%d,%d) node=%08x\n",
                  ord_idx, op, bx0, by0, bx1, by1, s_cur_node);
        // PSXPORT_PRIMDUMP=<frame>: dump every prim (poly) of that frame as an individual PNG (named by its
        // OT-walk ID) so the backdrop can be identified by eye and its band corrected. id=ord_idx.
        { void prim_dump_poly(Core*, int frame, unsigned id, uint8_t op, int nv, int is3d, int bg,
                              const int* xs, const int* ys, uint8_t r, uint8_t g, uint8_t b, int tex, int semi);
          prim_dump_poly(core, s_frame, ord_idx, op, nv, is3d, is3d ? -1 : bg, xs, ys,
                         rs[0], gs[0], bs[0], textured ? 1 : 0, semi); }
        if (is3d) core->mRender->stats.nd3d++; else core->mRender->stats.nd2d++;
        // PSXPORT_PRIMAT="x,y" (DISPLAY coords): log EVERY poly whose triangle covers that display pixel,
        // with its 3D/2D classification + per-vertex depth (ord) + node + color. Unlike provat (blind to
        // VK polys), this is the gp0 tee, so it sees the actual occlusion contestants. Frontmost opaque =
        // max ord. Tagged f%d so a multi-frame run can be grepped for the shot frame. (diag, 2026-06-24)
        { static int qx=-2, qy=-1, qf0=0; if (qx==-2){ qx=-1; const char* pa=cfg_str("PSXPORT_PRIMAT"); if(pa) sscanf(pa,"%d,%d,%d",&qx,&qy,&qf0); }
          if (qx>=0 && (int)s_frame>=qf0) { int ax=s_disp_x+qx, ay=s_disp_y+qy;
            auto edge=[](int ax_,int ay_,int x0,int y0,int x1,int y1){ return (int64_t)(x1-x0)*(ay_-y0)-(int64_t)(y1-y0)*(ax_-x0); };
            auto intri=[&](int i0,int i1,int i2){ int64_t w0=edge(ax,ay,xs[i1],ys[i1],xs[i2],ys[i2]);
              int64_t w1=edge(ax,ay,xs[i2],ys[i2],xs[i0],ys[i0]); int64_t w2=edge(ax,ay,xs[i0],ys[i0],xs[i1],ys[i1]);
              return (w0>=0&&w1>=0&&w2>=0)||(w0<=0&&w1<=0&&w2<=0); };
            int cover = intri(0,1,2) || (nv==4 && intri(1,2,3));
            if (cover) { static int n=0; if (n++<6000)
              fprintf(stderr,"[primat] f%d objnode=%08X pktnode=%08X op=%02X is3d=%d bg=%d bb=%d semi=%d tex=%d mode=%d raw=%d tp=(%d,%d) clut=(%d,%d) uv0=(%d,%d) da=(%d,%d)-(%d,%d) off=(%d,%d) col=(%d,%d,%d) bbox=(%d,%d)-(%d,%d)\n",
                s_frame, core->mRender->diag.currentNode(), s_cur_node, op, is3d, bg, billboard, semi, textured?1:0, mode, rw, s_tp_x, s_tp_y, s_clut_x, s_clut_y,
                us[0], vs[0], s_da_x0,s_da_y0,s_da_x1,s_da_y1, s_off_x,s_off_y,
                rs[0],gs[0],bs[0], bx0,by0,bx1,by1); } } }
        // PSXPORT_PAINTFG=1 (diag): force every 2D-FG (HUD-band) poly to opaque solid magenta so we can SEE
        // whether these prims rasterize at all (vs being culled / texture-transparent).
        { static int pf=-2; if(pf==-2){ const char* e=cfg_str("PSXPORT_PAINTFG"); pf=e?atoi(e):0; }
          if (pf && !is3d && !bg) { textured=0; mode=3; for(int i=0;i<nv;i++){rs[i]=255;gs[i]=0;bs[i]=255;} } }
      }
      // Genuine engine-wide: a poly with is3d==0 is a SCREEN-SPACE 2D element (HUD banner, full-screen
      // overlay) drawn as polys rather than sprites. The 3D world widens via the projection (OFX); these
      // 2D polys would stay left-anchored at 320 (the banner gets cut). Widen them like the 2D sprites:
      // scale the 2D plane uniformly to the wide width about the framebuffer origin so they fill the frame.
      { int gpu_gpu_wide_engine(void), gpu_gpu_wide_engine_w(void);
        if (!is3d && gpu_gpu_wide_engine() && s_prev_had3d) {  // 2D widen only on gameplay frames
          // HUD: identity (shader centers it). Backdrop AND full-screen fade/dim: stretch-to-fill so the fade
          // covers the whole wide FB (no undimmed margins, #21). See ws_2d_local_x.
          int fill = bg || fade_full;
          for (int i = 0; i < nv; i++) xs[i] = ws_2d_local_x(xs[i], fill); } }   // engine-owned 2D layout
      // DIAG PSXPORT_PAINTER=1: force PURE PSX OT painter order (is3d=0 / no bg split) for EVERY prim, so the
      // frame composites exactly as the PSX ordering table would. Render the field with and without this and
      // diff: the differing pixels are precisely where native per-pixel depth changes the picture (the
      // object-occlusion bug — terrain/atlas not obeying world-depth). Diagnostic only.
      { static int pm=-2; if(pm==-2){ const char* e=cfg_str("PSXPORT_PAINTER"); pm=e?atoi(e):0; }
        if (pm) { is3d = 0; bg = 0; } }
      if (use_rq) {
        // Engine owns ordering: hand the prim to the render queue tagged with its layer + depth mode.
        int layer = is3d ? RQ_WORLD : (bg ? RQ_BACKGROUND : RQ_HUD);
        int om    = is3d ? RQ_OM_DEPTH : (bg ? RQ_OM_2D_BG : RQ_OM_2D_FG);
        // 2D-overlay-only field pass: drop ALL guest-OT POLYS. In the field the GTE-projected 3D world
        // arrives as polys and is OWNED by the native render (ov_scene_native draws it via VK) — these
        // guest polys are the redundant copy the field path never drew before. We CANNOT keep "2D" polys
        // via the is3d test here: projprim has no records on the native field path (the projection
        // provenance is built by the owned submit path, not this separate OT walk), so is3d==0 for EVERY
        // poly — keeping them would re-emit the whole 3D world as flat HUD prims (render-queue overflow +
        // double-draw, observed as the free-roam crash). 2D-poly overlays (gradient/fade panels) are a
        // known frontier; the cutscene narration + the common HUD are SPRITES, handled below. (engine
        // owns the field's 3D geometry; the guest OT supplies only the leftover 2D SPRITES.)
        if (s_ot_2d_only) { /* field 3D world is native-owned; guest polys are redundant — skip */ }
        else {
        core->game->rq.emitOrQueue(core, 1, layer, om, nv, semi, rw, xs, ys, 0, 0, us, vs, rs, gs, bs,
                         is3d ? dep : 0, mode, s_tp_x, s_tp_y, s_clut_x, s_clut_y,
                         s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
        // fps60: a 2D billboard prim (obj_depth-tagged) gets stamped here, at queue time, as an anchor-
        // reproject billboard keyed on its object's identity (node→span lookup) — no build_lerp pre-pass.
        if (billboard) core->game->fps60.stampBillboard(core, s_cur_node);
        }
      } else {
      gpu_gpu_set_order(core, ord_idx);           // OT submission order -> depth (preserve opaque/semi order)
      if (!is3d) {                               // 2D band select
        if (bg) gpu_gpu_set_order_2d_bg(core, ord_idx); else gpu_gpu_set_order_2d(core, ord_idx);
      }
      #define SBS_OR_ND_SETVD(p) do { if (is3d) gpu_gpu_set_vd(core, p); } while (0)
      if (semi) {
        {   // OT-order grouping (overlap -> fresh fb snapshot)
          int bx0=xs[0],by0=ys[0],bx1=xs[0],by1=ys[0];
          for (int i=1;i<nv;i++){ if(xs[i]<bx0)bx0=xs[i]; if(xs[i]>bx1)bx1=xs[i]; if(ys[i]<by0)by0=ys[i]; if(ys[i]>by1)by1=ys[i]; }
          gpu_gpu_semi_group(core, bx0, by0, bx1, by1); }
        SBS_OR_ND_SETVD(dep);
        gpu_gpu_draw_semi(core, xs, ys, us, vs, rs, gs, bs, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                         s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
        if (nv == 4) { SBS_OR_ND_SETVD(&dep[1]);
          gpu_gpu_draw_semi(core, &xs[1], &ys[1], &us[1], &vs[1], &rs[1], &gs[1], &bs[1], s_tp_x, s_tp_y, mode, rw,
                         s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend); }
      } else {
        SBS_OR_ND_SETVD(dep);
        gpu_gpu_draw_tritri(core, xs, ys, us, vs, rs, gs, bs, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                           s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
        if (nv == 4) { SBS_OR_ND_SETVD(&dep[1]);
          gpu_gpu_draw_tritri(core, &xs[1], &ys[1], &us[1], &vs[1], &rs[1], &gs[1], &bs[1], s_tp_x, s_tp_y, mode, rw,
                           s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1); }
      }
      #undef SBS_OR_ND_SETVD
      }
    }
    // PSXPORT_POLYDUMP=frame — log every poly at `frame` (our port side, to compare vs oracle
    // polywatch). Finds the garbage-block prims in the GAME level.
    { static int pd = -2, pax = -1, pay = -1;
      if (pd == -2) { const char* e = cfg_str("PSXPORT_POLYDUMP"); pd = e ? atoi(e) : -1;
        const char* pa = cfg_str("PSXPORT_POLYAT"); if (pa) sscanf(pa, "%d,%d", &pax, &pay); }
      if (pd >= 0 && s_frame == pd) {
        int hit = (pax < 0);   // no point filter -> log all
        if (pax >= 0) {        // log only prims whose screen bbox (incl offset) covers (pax,pay)
          int xmin=99999,xmax=-99999,ymin=99999,ymax=-99999;
          for (int i=0;i<nv;i++){ int X=v[i].x+s_off_x,Y=v[i].y+s_off_y;
            if(X<xmin)xmin=X; if(X>xmax)xmax=X; if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y; }
          hit = (pax>=xmin && pax<=xmax && pay>=ymin && pay<=ymax);
        }
        static int n = 0;
        if (hit && n++ < 2000)
          fprintf(stderr, "[polydump] f%d node=%08X op=%02X tex=%d gou=%d clut=(%d,%d) tp=(%d,%d) "
                  "cols[(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)] "
                  "V[(%d,%d)(%d,%d)(%d,%d)(%d,%d)] off=(%d,%d)\n",
                  s_frame, s_cur_node, op, textured?1:0, gouraud?1:0, s_clut_x, s_clut_y, s_tp_x, s_tp_y,
                  v[0].r,v[0].g,v[0].b, v[1].r,v[1].g,v[1].b, v[2].r,v[2].g,v[2].b, v[3].r,v[3].g,v[3].b,
                  v[0].x,v[0].y, v[1].x,v[1].y, v[2].x,v[2].y, v[3].x,v[3].y, s_off_x, s_off_y);
      } }
    if (s_reddbg && textured && s_cw_x >= 0 && s_clut_x == s_cw_x && s_clut_y == s_cw_y) {
      static int n = 0;
      if (n++ < 12)
        fprintf(stderr, "[redpkt] f%d stage=%08X node=0x%08X op=%02X nv=%d gou=%d semi=%d clut=(%d,%d) tp=(%d,%d) blend=%d mode=%d "
                "V[(%d,%d)uv(%d,%d) (%d,%d)uv(%d,%d) (%d,%d)uv(%d,%d)%s] off=(%d,%d)\n",
                s_frame, core->mem_r32(0x801fe00c), s_cur_node, op, nv, gouraud, semi, s_clut_x, s_clut_y, s_tp_x, s_tp_y, s_tp_blend, s_tp_mode,
                v[0].x,v[0].y,v[0].u,v[0].v, v[1].x,v[1].y,v[1].u,v[1].v, v[2].x,v[2].y,v[2].u,v[2].v,
                quad?" +q":"", s_off_x, s_off_y);
    }
    prov_begin(op, textured ? 1 : 0, semi, v[0].r, v[0].g, v[0].b,
               v[0].x + s_off_x, v[0].y + s_off_y, v[0].u, v[0].v);
    if (s_oracle_prim_log && soft_gpu) {
      int xmn=v[0].x,xmx=v[0].x,ymn=v[0].y,ymx=v[0].y;
      for (int i=1;i<nv;i++){ if(v[i].x<xmn)xmn=v[i].x; if(v[i].x>xmx)xmx=v[i].x; if(v[i].y<ymn)ymn=v[i].y; if(v[i].y>ymx)ymx=v[i].y; }
      fprintf(stderr, "[oraprim] POLY op=%02X nv=%d tex=%d semi=%d bbox=(%d,%d)-(%d,%d) col=(%d,%d,%d) tp=(%d,%d) clut=(%d,%d)\n",
              op, nv, textured?1:0, semi, xmn+s_off_x, ymn+s_off_y, xmx+s_off_x, ymx+s_off_y, v[0].r,v[0].g,v[0].b, s_tp_x, s_tp_y, s_clut_x, s_clut_y);
    }
    if (sw_path()) {                           // VK owns poly raster now (tee'd above); SW does the rest
      tri(v[0], v[1], v[2], textured, shade, semi, raw);
      if (quad) tri(v[1], v[2], v[3], textured, shade, semi, raw);
    }
    s_prims++;
  } else if (op >= 0x60 && op <= 0x7F) {     // rectangle / sprite
    int textured = op & 0x04, semi = (op & 0x02) ? 1 : 0, size = (op >> 3) & 3;
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    int idx = 1;
    uint32_t xy = s_fifo[idx++]; int x = cx(xy), y = cy(xy);
    int u0 = 0, v0 = 0;
    if (textured) { uint32_t uv = s_fifo[idx++]; u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF; set_clut((uv >> 16) & 0xFFFF); }
    int w, h;
    if (size == 0) { uint32_t wh = s_fifo[idx++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
    else { w = h = (size == 1) ? 1 : (size == 2) ? 8 : 16; }
    // PSXPORT_POLYDUMP (+POLYAT): also log sprites/rects, so the garbage-block source can be a sprite.
    { static int pd = -2, pax = -1, pay = -1;
      if (pd == -2) { const char* e = cfg_str("PSXPORT_POLYDUMP"); pd = e ? atoi(e) : -1;
        const char* pa = cfg_str("PSXPORT_POLYAT"); if (pa) sscanf(pa, "%d,%d", &pax, &pay); }
      if (pd >= 0 && s_frame == pd) {
        int X=x+s_off_x, Y=y+s_off_y;
        int hit = (pax < 0) || (pax>=X && pax<X+w && pay>=Y && pay<Y+h);
        static int n = 0;
        if (hit && n++ < 2000)
          fprintf(stderr, "[polydump] f%d node=%08X SPRITE op=%02X tex=%d semi=%d clut=(%d,%d) tp=(%d,%d) "
                  "col=(%d,%d,%d) at=(%d,%d) %dx%d uv0=(%d,%d) off=(%d,%d)\n",
                  s_frame, s_cur_node, op, textured?1:0, semi, s_clut_x, s_clut_y, s_tp_x, s_tp_y,
                  cr, cg, cb, x, y, w, h, u0, v0, s_off_x, s_off_y);
      } }
    // PSXPORT_TEXEXPORT=<frame> — export the texture of each large textured sprite (backgrounds) on that
    // frame via the PC-native decoder. The menu/sea backdrops are big sprites; this writes their decoded
    // pixels to scratch/export/*.ppm with no PSX code in the decode path.
    { static int tex_f = -2; if (tex_f == -2) { const char* e = cfg_str("PSXPORT_TEXEXPORT"); tex_f = e ? atoi(e) : -1; }
      if (tex_f >= 0 && s_frame == tex_f && textured) {
        // Backgrounds (menu/sea) are 16×16 TILEMAPS sampling a shared atlas texpage. Export the whole
        // atlas (256×256 texels at the texpage origin) ONCE per unique (texpage,clut,mode), not per tile.
        static int seen_tpx[64], seen_tpy[64], seen_clx[64], seen_cly[64], nseen = 0;
        int dup = 0; for (int k = 0; k < nseen; k++)
          if (seen_tpx[k]==s_tp_x && seen_tpy[k]==s_tp_y && seen_clx[k]==s_clut_x && seen_cly[k]==s_clut_y) { dup = 1; break; }
        if (!dup && nseen < 64) {
          seen_tpx[nseen]=s_tp_x; seen_tpy[nseen]=s_tp_y; seen_clx[nseen]=s_clut_x; seen_cly[nseen]=s_clut_y; nseen++;
          char nm[96]; snprintf(nm, sizeof nm, "atlas_tp%d_%d_clut%d_%d_m%d", s_tp_x, s_tp_y, s_clut_x, s_clut_y, s_tp_mode);
          tex_export(nm, 0, 0, 256, 256);
        }
      } }
    fade_note(cr, cg, cb, s_off_y, semi); fade_note_size(w, h, semi);
    if (semi) semi_dump("sprite", s_tp_blend, cr, cg, cb, x, y, x + w, y + h, s_off_y);
    prov_begin(op, textured ? 1 : 0, semi, cr, cg, cb, x + s_off_x, y + s_off_y, u0, v0);
    // bit0=1 -> raw texel; bit0=0 -> modulate by command color (beetle sprite decode table:
    // 0x64/0x66 = TM1 modulate, 0x65/0x67 = TM0 raw). Modulating unconditionally once wrongly
    // tinted raw 0x65 sprites (turned a blue item green).
    { if (s_oracle_prim_log && soft_gpu)
        fprintf(stderr, "[oraprim] SPR  op=%02X tex=%d semi=%d at=(%d,%d) %dx%d col=(%d,%d,%d) uv=(%d,%d) tp=(%d,%d) clut=(%d,%d)\n",
                op, textured?1:0, semi, x+s_off_x, y+s_off_y, w, h, cr, cg, cb, u0, v0, s_tp_x, s_tp_y, s_clut_x, s_clut_y); }
    if (sw_path()) raster_sprite(op, x, y, u0, v0, w, h, cr, cg, cb, textured, semi);  // VK owns it (tee'd below)
    // VK backend (M5): tee rects/sprites as two triangles (opaque or semi; mode 3 = untextured solid).
    if (vk_path()) {
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
      if (!semi && sprite_is_bg_texpage(core, s_tp_x, s_tp_y)) bg = 1;
      { static int pm=-2; if(pm==-2){ const char* e=cfg_str("PSXPORT_PAINTER"); pm=e?atoi(e):0; } if(pm) bg=0; }  // DIAG painter
      // FADE/DIM (#21): a full-screen SEMI sprite is a fade/dim overlay -> stretch-to-fill the wide FB so it
      // covers the margins too, while staying in the topmost (HUD) band (not a backdrop). See ws_2d_local_x.
      int fade_full = (!bg && semi && fade_full_2d(s_disp_w, s_disp_h, x, y, x + w, y + h));
      if (!bg && cfg_dbg("objz") && s_frame == s_primdump_frame)
        fprintf(stderr, "[sprnode] op=%02x at(%d,%d %dx%d) rgb=(%d,%d,%d) node=%08x\n",
                op, x, y, w, h, cr, cg, cb, s_cur_node);
      { void prim_dump_sprite(Core*, int, unsigned, uint8_t, int, int, int, int, int, uint8_t, uint8_t, uint8_t, int, int);
        prim_dump_sprite(core, s_frame, ord_idx, op, x, y, w, h, bg, cr, cg, cb, textured ? 1 : 0, semi); }
      int X = x + s_off_x, Y = y + s_off_y;
      int XL = X, XR = X + w;
      // Widescreen 2D handling. Genuine engine-wide widens the 3D world at the projection (OFX); 2D
      // sprites bypass the GTE, so they are mapped here. A backdrop (sky/water) STRETCHES to fill the wide
      // FB; HUD/UI is identity (the relocation shader's +margin already centers it). See ws_2d_local_x.
      { int gpu_gpu_wide_engine(void);
        if (gpu_gpu_wide_engine() && s_prev_had3d) {       // only widen 2D on gameplay frames (else pillarbox)
          int fill = bg || fade_full;                     // backdrop AND full-screen fade/dim stretch-to-fill (#21)
          XL = ws_2d_local_x(XL, fill);                   // engine-owned 2D layout (HUD centered, bg/fade fills)
          XR = ws_2d_local_x(XR, fill);
        } }
      int qx[4] = { XL, XR, XL, XR }, qy[4] = { Y, Y, Y+h, Y+h };
      int qu[4] = { u0, u0+w, u0, u0+w }, qv[4] = { v0, v0, v0+h, v0+h };
      unsigned char qr[4]={cr,cr,cr,cr}, qg[4]={cg,cg,cg,cg}, qb[4]={cb,cb,cb,cb};
      int mode = textured ? s_tp_mode : 3, rw = op & 1;
      if (use_rq) {
        // PC-native object depth: a world object's billboard sprite occludes by its world position.
        float dep[4], od; int objz = (!bg && obj_depth_lookup(s_cur_node, &od));
        if (objz) { dep[0] = dep[1] = dep[2] = dep[3] = od; s_seen3d = 1; }
        int layer = objz ? RQ_WORLD     : (bg ? RQ_BACKGROUND : RQ_HUD);
        int om    = objz ? RQ_OM_DEPTH  : (bg ? RQ_OM_2D_BG   : RQ_OM_2D_FG);
        // 2D-overlay-only field pass: drop the 3D-world / backdrop prims (owned natively); keep 2D HUD.
        if (s_ot_2d_only && layer != RQ_HUD) { /* world/bg owned by ov_scene_native — skip */ }
        else {
        core->game->rq.emitOrQueue(core, 1, layer, om, 4, semi, rw, qx, qy, 0, 0, qu, qv, qr, qg, qb, objz ? dep : 0, mode,
                         s_tp_x, s_tp_y, s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy,
                         s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
        // fps60: a 2D billboard sprite (obj_depth-tagged) gets stamped here, at queue time, as an anchor-
        // reproject billboard keyed on its object's identity (node→span lookup) — no build_lerp pre-pass.
        if (objz) core->game->fps60.stampBillboard(core, s_cur_node);
        }
      } else {
      gpu_gpu_set_order(core, ord_idx);          // OT submission order -> depth (preserve opaque/semi order)
      if (bg) gpu_gpu_set_order_2d_bg(core, ord_idx); else gpu_gpu_set_order_2d(core, ord_idx);
      if (semi) {
        { gpu_gpu_semi_group(core, X, Y, X+w, Y+h); }  // OT-order grouping
        gpu_gpu_draw_semi(core, qx, qy, qu, qv, qr, qg, qb, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                         s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
        gpu_gpu_draw_semi(core, &qx[1], &qy[1], &qu[1], &qv[1], &qr[1], &qg[1], &qb[1], s_tp_x, s_tp_y, mode, rw,
                         s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
      } else {
        gpu_gpu_draw_tritri(core, qx, qy, qu, qv, qr, qg, qb, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                           s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
        gpu_gpu_draw_tritri(core, &qx[1], &qy[1], &qu[1], &qv[1], &qr[1], &qg[1], &qb[1], s_tp_x, s_tp_y, mode, rw,
                           s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
      }
      }
    }
    s_prims++;
  } else if (op == 0x02) {                   // fill rectangle (in VRAM, ignores clip/offset)
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    uint32_t xy = s_fifo[1], wh = s_fifo[2];
    int x = xy & 0x3F0, y = (xy >> 16) & 0x1FF, w = ((wh & 0x3FF) + 0xF) & ~0xF, h = (wh >> 16) & 0x1FF;
    uint16_t col = to555(cr, cg, cb);
    for (int dy = 0; dy < h; dy++) for (int dx = 0; dx < w; dx++) *vram(x + dx, y + dy) = col;
    { if (s_oracle_prim_log && soft_gpu)
        fprintf(stderr, "[oraprim] FILL at=(%d,%d) %dx%d col=(%d,%d,%d)\n", x, y, w, h, cr, cg, cb); }
    if (vk_path()) gpu_gpu_dirty(core, x, y, w, h);   // mirror fill to VK
  } else if (op >= 0x40 && op <= 0x5F) {     // line / poly-line (flat or gouraud)
    int semi = (op & 0x02) ? 1 : 0, gouraud = (op & 0x10) ? 1 : 0;
    // Collect the vertex list from s_fifo (cmd carries v0's colour). Single lines have 2 verts;
    // poly-lines have N (gouraud: cmd,xy0,(c,xy)*; mono: cmd,xy0,xy*). Then draw each segment.
    uint8_t r0 = cmd_r(c), g0 = cmd_g(c), b0 = cmd_b(c);
    int vx[64], vy[64]; uint8_t vr[64], vg[64], vb[64]; int nv = 0, i = 1;
    vx[0] = cx(s_fifo[i]); vy[0] = cy(s_fifo[i]); vr[0] = r0; vg[0] = g0; vb[0] = b0; nv = 1; i++;
    while (i < s_fcount && nv < 64) {
      uint8_t r = r0, g = g0, b = b0;
      if (gouraud) { if (i >= s_fcount) break; uint32_t col = s_fifo[i++]; r = cmd_r(col); g = cmd_g(col); b = cmd_b(col); }
      if (i >= s_fcount) break;
      vx[nv] = cx(s_fifo[i]); vy[nv] = cy(s_fifo[i]); vr[nv] = r; vg[nv] = g; vb[nv] = b; nv++; i++;
    }
    for (int s = 0; s + 1 < nv; s++) {        // flat colour = start vertex
      if (sw_path())
        raster_line(vx[s], vy[s], vx[s+1], vy[s+1], vr[s], vg[s], vb[s], semi);
      else {                                   // VK: tee the segment as a 1px-thick quad (mode 3 flat)
        int x0=vx[s]+s_off_x, y0=vy[s]+s_off_y, x1=vx[s+1]+s_off_x, y1=vy[s+1]+s_off_y;
        int ox = (abs(x1-x0) >= abs(y1-y0)) ? 0 : 1, oy = ox ? 0 : 1;
        int xa[4]={x0,x1,x0+ox,x1+ox}, ya[4]={y0,y1,y0+oy,y1+oy}, zu[4]={0,0,0,0};
        unsigned char rr[4]={vr[s],vr[s+1],vr[s],vr[s+1]}, gg[4]={vg[s],vg[s+1],vg[s],vg[s+1]}, bb[4]={vb[s],vb[s+1],vb[s],vb[s+1]};
        int o1[3]={0,1,2}, o2[3]={1,2,3};      // tris (p0,p1,p0') and (p1,p0',p1')
        if (semi) {   // OT-order grouping for the segment quad
          int bx0=x0<x1?x0:x1, bx1=x0<x1?x1:x0, by0=y0<y1?y0:y1, by1=y0<y1?y1:y0;
          gpu_gpu_semi_group(core, bx0, by0, bx1+ox, by1+oy); }
        for (int t = 0; t < 2; t++) { int* o = t ? o2 : o1;
          int X[3]={xa[o[0]],xa[o[1]],xa[o[2]]}, Y[3]={ya[o[0]],ya[o[1]],ya[o[2]]};
          unsigned char R[3]={rr[o[0]],rr[o[1]],rr[o[2]]}, G[3]={gg[o[0]],gg[o[1]],gg[o[2]]}, B[3]={bb[o[0]],bb[o[1]],bb[o[2]]};
          if (semi) gpu_gpu_draw_semi(core, X,Y,zu,zu,R,G,B, 0,0,3,0,0,0, 0,0,0,0, s_da_x0,s_da_y0,s_da_x1,s_da_y1, s_tp_blend);
          else      gpu_gpu_draw_tritri(core, X,Y,zu,zu,R,G,B, 0,0,3,0,0,0, 0,0,0,0, s_da_x0,s_da_y0,s_da_x1,s_da_y1);
        }
      }
    }
    s_prims++;
  }
  // env commands (E1..E6) handled in gpu_gp0 directly (single-word).
}

// Words needed to complete the packet beginning with command word `c`.
static int gp0_len(uint32_t c) {
  uint8_t op = c >> 24;
  if (op >= 0x20 && op <= 0x3F) {
    int n = 1, nv = (op & 8) ? 4 : 3;
    n += nv * (1 + ((op & 4) ? 1 : 0));        // xy (+uv) per vertex
    if (op & 0x10) n += nv - 1;                 // extra colors for gouraud (first is cmd)
    return n;
  }
  if (op >= 0x60 && op <= 0x7F) { int n = 2; if (op & 4) n++; if (((op >> 3) & 3) == 0) n++; return n; }
  if (op >= 0x40 && op <= 0x5F) return (op & 0x10) ? 4 : 3;  // (poly-line term not modeled)
  if (op == 0x02) return 3;                      // fill
  if (op == 0x80) return 4;                       // VRAM->VRAM copy: cmd + src + dst + size
  if (op == 0xA0 || op == 0xC0) return 3;        // CPU<->VRAM xfer headers (pixels stream after)
  return 1;                                      // env / nop / single-word
}

// One word into the GP0 port (direct write or DMA).
void GpuState::gpu_gp0(Core* core, uint32_t w) {
  s_gp0_words++;
  if (s_xfer) {                                  // CPU->VRAM pixel stream (2 px/word)
    for (int k = 0; k < 2; k++) {
      int px = s_xfer_px % s_xfer_w, py = s_xfer_px / s_xfer_w;
      if (py < s_xfer_h) *vram(s_xfer_x + px, s_xfer_y + py) = (k ? (w >> 16) : w) & 0xFFFF;
      s_xfer_px++;
    }
    if (s_xfer_px >= s_xfer_w * s_xfer_h) {
      s_xfer = 0;
      if (s_cw_pending) { clutwatch_dump("A0 DONE", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h); s_cw_pending = 0; }
    }
    return;
  }
  if (s_fcount == 0) {
    uint8_t op = w >> 24;
    switch (op) {                                // single-word env / state commands
      case 0x00: return;                         // nop
      case 0x01: return;                         // clear cache
      case 0xE1: set_texpage(w & 0xFFFF); return;
      case 0xE2: s_tw_mx = w & 31; s_tw_my = (w >> 5) & 31; s_tw_ox = (w >> 10) & 31; s_tw_oy = (w >> 15) & 31; return;
      case 0xE3: s_da_x0 = w & 0x3FF; s_da_y0 = (w >> 10) & 0x1FF;
        if (cfg_dbg("env")) fprintf(stderr, "[env] E3 clip_tl=(%d,%d)\n", s_da_x0, s_da_y0); return;
      case 0xE4: s_da_x1 = w & 0x3FF; s_da_y1 = (w >> 10) & 0x1FF;
        if (cfg_dbg("env")) fprintf(stderr, "[env] E4 clip_br=(%d,%d)\n", s_da_x1, s_da_y1); return;
      case 0xE5: s_off_x = ((int)(w & 0x7FF) << 21) >> 21; s_off_y = ((int)((w >> 11) & 0x7FF) << 21) >> 21;
        if (cfg_dbg("env")) fprintf(stderr, "[env] E5 offset=(%d,%d)\n", s_off_x, s_off_y); return;
      case 0xE6: return;                         // mask settings (mask-test not modeled)
      default: break;
    }
    // Poly-lines (op 0x48-0x4F mono / 0x58-0x5F gouraud — line group 0x40-0x5F with bit 0x08) are
    // VARIABLE length: a vertex list terminated by a word with (w & 0xF000F000)==0x50005000
    // (0x55555555). gp0_len can't know the length from the first word, so accumulate until the
    // terminator. Mishandling this (treating it as a fixed 3/4-word single line) drifts the whole
    // GP0 parse and makes a later data word decode as a spurious VRAM copy (atlas corruption).
    s_pl = (op >= 0x40 && op <= 0x5F && (op & 0x08)) ? 1 : 0;
    s_pl_g = (op & 0x10) ? 1 : 0;
    s_fneed = gp0_len(w);
  }
  s_fifo_addr[s_fcount] = s_gp0_src;
  s_fifo[s_fcount++] = w;
  if (s_pl) {
    int idx = s_fcount - 1;                          // index of the word just stored
    // A terminator may appear at a vertex-START slot, only after the mandatory 2 vertices:
    //   gouraud: color slots = even indices >= 4 (cmd,xy0,c1,xy1, then c2/term,xy2,...)
    //   mono:    xy slots    = indices >= 3        (cmd,xy0,xy1, then xy2/term,...)
    int term_slot = s_pl_g ? (idx >= 4 && !(idx & 1)) : (idx >= 3);
    if (term_slot && (w & 0xF000F000u) == 0x50005000u) {
      s_fcount = idx;                                // drop the terminator; render cmd+vertices
      gp0_exec(core);
      s_fcount = 0; s_fneed = 0; s_pl = 0;
      return;
    }
    if (s_fcount >= 250) { s_fcount = 0; s_fneed = 0; s_pl = 0; }  // safety: never overflow s_fifo
    return;
  }
  if (s_fcount >= s_fneed) {
    uint8_t op = s_fifo[0] >> 24;
    if (op == 0xA0) {                            // CPU->VRAM: set up the pixel stream
      s_xfer_x = s_fifo[1] & 0x3FF; s_xfer_y = (s_fifo[1] >> 16) & 0x1FF;
      s_xfer_w = ((s_fifo[2] & 0x3FF) ? (s_fifo[2] & 0x3FF) : 1024);
      s_xfer_h = (((s_fifo[2] >> 16) & 0x1FF) ? ((s_fifo[2] >> 16) & 0x1FF) : 512);
      s_xfer_px = 0; s_xfer = 1;
      if (vk_path()) gpu_gpu_dirty(core, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);   // mirror upload to VK
      vram_guard_check(core, "A0", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, 0x80000000u | s_dma_src);
      clutwatch_xfer("A0", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);
      if (cfg_dbg("upload")) {
        fprintf(stderr, "[upload] f%d A0 dest=(%d,%d) %dx%d src=0x%08X\n",
                s_frame, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, 0x80000000u | s_dma_src);
      }
      if (texwatch_overlap(s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h)) {
        uint32_t src = 0x80000000u | s_dma_src;
        fprintf(stderr, "[texwatch] f%d A0 dest=(%d,%d) %dx%d src=0x%08X srcbytes:",
                s_frame, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, src);
        for (int k = 0; k < 12; k++) fprintf(stderr, " %02X", core->mem_r8(s_dma_src + k));
        fprintf(stderr, "\n");
      }
    } else if (op == 0x80) {                     // VRAM->VRAM copy
      int sx = s_fifo[1] & 0x3FF, sy = (s_fifo[1] >> 16) & 0x1FF;
      int dx = s_fifo[2] & 0x3FF, dy = (s_fifo[2] >> 16) & 0x1FF;
      int w2 = s_fifo[3] & 0x3FF, h2 = (s_fifo[3] >> 16) & 0x1FF;
      // Guard the DEST rect: a render-OT 0x80 copy whose dest lands on a live texpage is the classic
      // atlas-clobber (later-72 poly-line-desync family). Checked BEFORE the copy so the log names the
      // clobber even though the copy still proceeds (diagnostic, non-mutating; the catch is the point).
      vram_guard_check(core, "80copy", dx, dy, w2, h2, 0x80000000u | ((uint32_t)(sy * VRAM_W + sx) * 2));
      for (int y = 0; y < h2; y++) for (int x = 0; x < w2; x++) *vram(dx + x, dy + y) = *vram(sx + x, sy + y);
      if (vk_path()) gpu_gpu_dirty(core, dx, dy, w2, h2);   // mirror VRAM->VRAM copy to VK
      clutwatch_xfer("80copy", dx, dy, w2, h2);
      if (texwatch_overlap(dx, dy, w2, h2)) {
        fprintf(stderr, "[texwatch] f%d 80copy src=(%d,%d) dest=(%d,%d) %dx%d node=0x%08X words=%08X,%08X,%08X,%08X\n",
                s_frame, sx, sy, dx, dy, w2, h2, s_cur_node, s_fifo[0], s_fifo[1], s_fifo[2], s_fifo[3]);
        // Dump RAM + the OT node neighbourhood the first time the atlas-clobbering copy fires, so the
        // malformed node and the chain that reaches it can be examined offline.
        if (cfg_str("PSXPORT_CLOBBERDUMP")) { static int done = 0; if (!done++) {
          uint32_t na = s_cur_node & 0x1FFFFF;
          fprintf(stderr, "[clobber] OT root madr=0x%08X node@0x%08X neighbourhood:\n", 0x80000000u|s_ot_madr, s_cur_node);
          for (int k = -8; k <= 16; k++) fprintf(stderr, "  [%+d] 0x%08X: %08X\n", k,
                  0x80000000u | ((na + k*4) & 0x1FFFFF), core->mem_r32(0x80000000u | ((na + k*4) & 0x1FFFFF)));
          FILE* mf = fopen(cfg_str("PSXPORT_CLOBBERDUMP"), "wb");
          if (mf) { fwrite(core->ram, 1, 0x200000, mf); fclose(mf);
                    fprintf(stderr, "[clobber] RAM dumped -> %s\n", cfg_str("PSXPORT_CLOBBERDUMP")); } } }
      }
    } else if (op != 0xC0) {
      gp0_exec(core);
    }
    s_fcount = 0; s_fneed = 0;
  }
}

// GP1 display/control commands.
void GpuState::gpu_gp1(uint32_t w) {
  uint8_t op = w >> 24;
  if (cfg_dbg("gp1"))
    fprintf(stderr, "[gp1] f%d %02X %06X\n", s_frame, op, w & 0xFFFFFF);
  switch (op) {
    case 0x05: s_disp_x = w & 0x3FF; s_disp_y = (w >> 10) & 0x1FF; break;          // display area start
    case 0x07:  // vertical display range (scanlines). In 480i the field is shown twice (two VRAM
      // lines per scanline), so the displayed VRAM height is (y1-y0)*2 — without the doubling the
      // bottom of a 480-line framebuffer is clipped (the SCEA "Presents" line, journal later-46).
      s_disp_vy0 = w & 0x3FF; s_disp_vy1 = (w >> 10) & 0x3FF;
      { int n = s_disp_vy1 - s_disp_vy0; if (n <= 0) n = 240; s_disp_h = s_disp_480i ? n * 2 : n; }
      break;
    case 0x08:  // display mode: horizontal res (bits0-1, bit6=368), interlace (bit5), VRes 480 (bit2)
      s_disp_w = ((w & 3) == 0) ? 256 : ((w & 3) == 1) ? 320 : ((w & 3) == 2) ? 512 : 640;
      s_disp_480i = ((w & 0x20) && (w & 0x04)) ? 1 : 0;
      { int n = s_disp_vy1 - s_disp_vy0; if (n <= 0) n = 240; s_disp_h = s_disp_480i ? n * 2 : n; }
      break;
    default: break;
  }
}

// Optional live window (PSXPORT_GPU_WINDOW=1). Headless builds without SDL just no-op.
// The output is fit to the screen at a fixed 4:3 display aspect with letterbox/pillarbox bars —
// NEVER stretched. PSX always scans its framebuffer (whatever the horizontal res: 256/320/512/640)
// out to the same 4:3 screen area, so mapping disp_w×disp_h into a 4:3 rect reproduces the correct
// pixel aspect and keeps 2D art / FMVs un-stretched regardless of window size. (This is the display
// scaler, independent of the — currently blocked — widescreen GEOMETRY tier; we do not widen here.)
#ifdef PSXPORT_SDL
// The legacy SDL_Renderer software-present window is RETIRED: the SDL_GPU renderer (gpu_gpu.cpp) is THE
// present path (gpu_gpu_enabled() is always 1), windowed or headless. blit_src forwards the display region
// to it; ensure_window is a no-op (the GPU backend owns the window). This drops the SDL2 SDL_Renderer /
// SDL_Texture code that SDL3 doesn't carry verbatim.
int  gpu_gpu_enabled(void);                                   // gpu_gpu.cpp — SDL_GPU present backend
void GpuState::ensure_window() {}
void GpuState::blit_src(const uint16_t* src, int sx, int sy) {
  gpu_gpu_present(&game->core, src, sx, sy, s_disp_w, s_disp_h);   // SDL_GPU present (incl. headless upload)
}
void GpuState::present_window() { blit_src(s_vram, s_disp_x, s_disp_y); }   // the live front buffer
// Re-present the CURRENT frame without advancing game logic — used by the debug-server pause loop so
// the window stays live AND the VK readback reflects exactly what's on screen (vkshot reads the same
// region this presents). No s_frame++ / batch reset (those belong to gpu_present_ex).
void GpuState::gpu_repaint() { present_window(); }
#else
void GpuState::present_window() {}
void GpuState::gpu_repaint() {}
#endif

// Frame pacing: the native game loop (game_main) runs UNTHROTTLED — at thousands of fps.
// That's right for headless tests but unplayable windowed. When a window is up we throttle to
// the game's own pace: DAT_1f800235 is the engine's vblank quota (vblanks at 60 Hz per displayed
// frame; =2 => 30 fps, Tomba2's logic rate). PSXPORT_NOPACE disables (fast-forward); headless
// (no window) is never paced so tests stay fast. SDL timing keeps it portable (a window implies
// SDL is up). Called ONCE per native game-frame from ov_frame_update — NOT from gpu_present,
// which the boot stub also drives many times per frame (pacing those would stall the boot).
// Pace 1/`parts` of a logic frame: parts=1 → one full logic frame (30fps faithful path); parts=2 →
// half a logic frame (fps60 presents twice per logic frame for 60fps). The shared `next` accumulator
// advances by exactly one logic frame's worth per logic frame either way, so audio stays realtime.
extern "C" int gpu_has_window(void);
void gpu_pace_subframe(Core* core, int parts) {
  // PC-OWNED frame pacing (user 2026-06-22: the game loop paces itself). Gate on a RUNTIME check for an
  // on-screen window — NOT a compile-time macro and NOT gpu_gpu_enabled() — so it always paces a live
  // windowed run and never paces headless (tests stay fast). Portable monotonic clock + nanosleep (no
  // SDL dependency), so it works identically on Linux and macOS.
  if (!gpu_has_window() || cfg_on("PSXPORT_NOPACE")) return;
  if (parts < 1) parts = 1;
  int quota = core->mem_r8(0x1F800235u); if (quota < 1) quota = 2;   // vblanks per frame (default 30fps)
  double interval_ms = quota * 1000.0 / 60.0 / parts;               // target ms for this (sub)frame
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
  static double next = -1;
  if (next < 0) next = now;
  next += interval_ms;
  if (next > now) {
    double sleep_ms = next - now;
    struct timespec req = { (time_t)(sleep_ms / 1000.0), (long)((sleep_ms - (long)(sleep_ms/1000.0)*1000.0) * 1e6) };
    nanosleep(&req, 0);
  } else if (now - next > interval_ms) {
    next = now;                                                     // resync after a hitch (no debt)
  }
}
void gpu_pace_frame(Core* core) { gpu_pace_subframe(core, 1); }

// Present: copy the displayed VRAM region to an RGB buffer. PSXPORT_GPU_DUMP=dir dumps PPMs;
// PSXPORT_GPU_WINDOW=1 shows a live SDL window.
// REPL `shot <path>`: write the currently-displayed VRAM region to a PPM so I can SEE where the
// interactive driver is (title / menu / attract / gameplay) instead of guessing from stage numbers.
void GpuState::gpu_native_shot(Core* core, const char* path) {
  // VK render lives in the GPU image, not s_vram — read it back over the current display region.
  // (soft_gpu oracle: VK is off for this Core, so fall through to the s_vram PPM dump below.)
  if (vk_path()) {
    void gpu_gpu_shot_region(Core*, const char*, int, int, int, int);
    int dw = s_disp_w > 0 ? s_disp_w : 320, dh = s_disp_h > 0 ? s_disp_h : 240;
    gpu_gpu_shot_region(core, path, s_disp_x, s_disp_y, dw, dh);
    return;
  }
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "[shot] cannot open %s\n", path); return; }
  fprintf(f, "P6\n%d %d\n255\n", s_disp_w, s_disp_h);
  for (int y = 0; y < s_disp_h; y++)
    for (int x = 0; x < s_disp_w; x++) {
      uint16_t p = *vram(s_disp_x + x, s_disp_y + y);
      uint8_t rgb[3] = { (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3) };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  fprintf(stderr, "[shot] f%d -> %s (%dx%d disp@%d,%d)\n", s_frame, path, s_disp_w, s_disp_h, s_disp_x, s_disp_y);
}
// gpu_present_ex: the per-frame present + bookkeeping. `do_blit` blits the live front buffer to the
// window; fps60 passes 0 (it owns presentation: it blits the previous real frame + the interpolated
// frame itself) but still wants the bookkeeping (watchdog, s_frame++, diagnostics).
void GpuState::gpu_present_ex(Core* core, int do_blit) {
  watchdog_pet();             // frame-progress heartbeat (see watchdog.c)
  if (cfg_dbg("vramscan")) {
    int minx=99999,miny=99999,maxx=-1,maxy=-1; long nz=0;
    for (int y=0;y<512;y++) for (int x=0;x<1024;x++) if (*vram(x,y)&0x7FFF) {
      nz++; if(x<minx)minx=x; if(x>maxx)maxx=x; if(y<miny)miny=y; if(y>maxy)maxy=y; }
    fprintf(stderr, "[vramscan] f%d disp@(%d,%d) %dx%d  nonblack=%ld bbox=(%d,%d)-(%d,%d)\n",
            s_frame, s_disp_x, s_disp_y, s_disp_w, s_disp_h, nz, minx, miny, maxx, maxy);
  }
  if (do_blit) present_window();
  { void ws_sx_dump(const char*);   // widescreen RE (later-55): dump GTE screen-X histogram
    if (cfg_dbg("sxhist") && s_frame > 0 && (s_frame % 500) == 0) {
      char t[32]; snprintf(t, sizeof t, "f%d", s_frame); ws_sx_dump(t); } }
  { void proj_probe_dump(const char*);   // Phase-1: native-projection 0-diff verifier (PSXPORT_PROJPROBE)
    if (cfg_on("PSXPORT_PROJPROBE") && s_frame > 0 && (s_frame % 200) == 0) {
      char t[32]; snprintf(t, sizeof t, "f%d", s_frame); proj_probe_dump(t); } }
  { void rtpcaller_dump(Core*, const char*); void rtpcaller_reset(void);  // Phase-1: pin RTP caller sites
    // Window the histogram to the LAST 50 frames so a dump reflects only the CURRENT scene's submitters
    // (a cumulative-since-boot count is dominated by the title/menu phase before the field is reached).
    if (cfg_dbg("rtpcaller") && s_frame > 0 && (s_frame % 50) == 0) {
      char t[24]; snprintf(t, sizeof t, "f%d(last50)", s_frame); rtpcaller_dump(core, t); rtpcaller_reset(); } }
  // Reset the per-vertex depth table EVERY frame the native-depth path is live (NATIVE_DEPTH or SBS),
  // here — after this frame's DrawOTag/lookups, before next frame's projections record into it — so a
  // vertex word never reads an OLD frame's depth. The engine (engine_submit.c) repopulates it each frame.
  { int attach_enabled(void);
    if (attach_enabled()) core->mRender->projprim.reset(); }
  {
    {

      if (cfg_dbg("ndepth") && s_frame > 0 && (s_frame % 60) == 0)
        fprintf(stderr, "[ndepth f%d] real-depth(3D) prims=%ld  OT-band(2D) prims=%ld  3D%%=%.1f\n",
                s_frame, core->mRender->stats.nd3d, core->mRender->stats.nd2d, (core->mRender->stats.nd3d+core->mRender->stats.nd2d) ? 100.0*core->mRender->stats.nd3d/(core->mRender->stats.nd3d+core->mRender->stats.nd2d) : 0.0);
      { auto s = core->mRender->projprim.stats();
        if (cfg_dbg("ndepth") && s_frame > 0 && (s_frame % 60) == 0)
          fprintf(stderr, "    projprim(vtx) records=%ld  lookups hit=%ld miss=%ld\n", s.set, s.hit, s.miss);
        core->mRender->projprim.statsReset(); }
      { RenderStats& st = core->mRender->stats;
        if (cfg_dbg("ndepth") && s_frame > 0 && (s_frame % 60) == 0)
          fprintf(stderr, "    obj_depth spans=%ld  2D-prim lookups hit=%ld miss=%ld\n", st.odAdd, st.odHit, st.odMiss);
        st.odAdd = st.odHit = st.odMiss = 0; }
      extern long g_nd2d_hist[256];
      if (cfg_dbg("ndepth") && s_frame > 0 && (s_frame % 60) == 0) {
        for (int o = 0; o < 256; o++) if (g_nd2d_hist[o]) {
          int gour=o&0x10, quad=o&0x08, tex=o&0x04, semi=o&0x02;
          const char* k = (o>=0x60&&o<0x80) ? "SPRITE" : (o>=0x40&&o<0x60) ? "LINE" :
                          (o>=0x20&&o<0x40) ? "POLY" : (o>=0x80) ? "BLIT" : "misc";
          fprintf(stderr, "    2D-band op 0x%02X x%ld  [%s%s%s%s %s]\n", o, g_nd2d_hist[o],
                  gour?"G":"-", quad?"4":"3", tex?"T":"-", semi?"s":"-", k); }
        for (int o = 0; o < 256; o++) g_nd2d_hist[o] = 0;
      }
      core->mRender->stats.nd3d = core->mRender->stats.nd2d = 0;
    } }
  // PSXPORT_PROVAT="x,y[:frame]" — at present time, report (in DISPLAY space, so the double buffer
  // is irrelevant) which primitive last wrote each pixel in a 7x7 box around (x,y), with how many
  // frames ago it was drawn. A wrong pixel whose writer is the current frame = actively drawn (the
  // listed prim is the culprit); whose writer is many frames old = STALE, revealed through a gap.
  { const char* pa = cfg_str("PSXPORT_PROVAT");
    if (pa) {
      int qx = -1, qy = -1, qf = -1; sscanf(pa, "%d,%d", &qx, &qy);
      const char* col = strchr(pa, ':'); if (col) qf = atoi(col + 1);
      if (qx >= 0 && (qf < 0 ? (s_frame % 200 == 0) : s_frame == qf))
        gpu_provat_display(core, stderr, qx, qy);
    } }
  { const char* vd = cfg_str("PSXPORT_VRAMDUMP_AT");   // "frame:path" — dump our 1024x512x16 VRAM
    if (vd) { int fr = atoi(vd); const char* col = strchr(vd, ':');
      if (col && s_frame == fr) { FILE* vf = fopen(col + 1, "wb");
        if (vf) { fwrite(s_vram, 2, VRAM_W * VRAM_H, vf); fclose(vf);
                  fprintf(stderr, "[gpu] VRAM dump f%d -> %s\n", s_frame, col + 1); } } } }
  if (cfg_dbg("stage") && (s_frame % 200) == 0)
    fprintf(stderr, "[stagetl] gpu f%d task0entry=%08X\n", s_frame, core->mem_r32(0x801fe00c));
  const char* dir = cfg_str("PSXPORT_GPU_DUMP");
  if (s_log) fprintf(stderr, "[gpu] frame %d: %ld prims, %ld gp0words, %ld dma2, disp %dx%d @ (%d,%d)\n",
                     s_frame, s_prims, s_gp0_words, s_dma2, s_disp_w, s_disp_h, s_disp_x, s_disp_y);
  // PSXPORT_VRAMDUMP="frame:path" — dump our full 1024x512x16 VRAM at `frame` (raw u16, no header),
  // matching the oracle's PSXPORT_VRAMDUMP (main.cpp) so the texture/CLUT ATLAS can be diffed across
  // engines at a scene-aligned frame (the atlas is uploaded once at scene load = static per scene).
  { static int vf = -2; static char vp[256];
    if (vf == -2) { const char* e = cfg_str("PSXPORT_VRAMDUMP"); vf = -1;
      if (e) { const char* col = strchr(e, ':'); if (col) { vf = atoi(e); snprintf(vp, sizeof vp, "%s", col + 1); } } }
    if (vf >= 0 && s_frame == vf) { FILE* f = fopen(vp, "wb");
      if (f) { fwrite(s_vram, 2, (size_t)VRAM_W * VRAM_H, f); fclose(f);
               fprintf(stderr, "[vramdump] f%d -> %s (1024x512x16)\n", s_frame, vp); } } }
  if (dir) {
    if (s_frame == 0) { char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); int r = system(cmd); (void)r; }
    char path[512]; snprintf(path, sizeof path, "%s/f%05d.ppm", dir, s_frame);
    FILE* f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", s_disp_w, s_disp_h);
      for (int y = 0; y < s_disp_h; y++)
        for (int x = 0; x < s_disp_w; x++) {
          uint16_t p = *vram(s_disp_x + x, s_disp_y + y);
          uint8_t rgb[3] = { (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3) };
          fwrite(rgb, 1, 3, f);
        }
      fclose(f);
    }
  }
  { static int fa = -2, fb = -2;   // PSXPORT_FADEDBG="a:b": per-frame brightness/draw log over [a,b]
    if (fa == -2) { const char* e = cfg_str("PSXPORT_FADEDBG"); fa = fb = -1;
      if (e) { fa = atoi(e); const char* col = strchr(e, ':'); fb = col ? atoi(col + 1) : fa + 200; } }
    if (fa >= 0 && s_frame >= fa && s_frame <= fb)
      fprintf(stderr, "[fadedbg] f%d disp=(%d,%d) drawY=%d maxcol=%d nprim=%d nsemi=%d semi[%d..%d] bigsemi=%d\n",
              s_frame, s_disp_x, s_disp_y, s_fade_lasty, s_fade_maxc, s_fade_npoly, s_fade_nsemi,
              s_fade_semimin == 999 ? -1 : s_fade_semimin, s_fade_semimax, s_fade_bigsemi); }
  s_fade_maxc = 0; s_fade_npoly = 0; s_fade_nsemi = 0; s_fade_semimax = -1; s_fade_semimin = 999; s_fade_bigsemi = 0;
  { gpu_gpu_frame_end(core, s_vram, s_frame); }  // VK: diff + batch reset
  s_frame++; s_prims = 0; s_gp0_words = 0; s_dma2 = 0;
  s_prim_order = 0;   // restart the per-frame OT submission order (VK depth) for the next frame
  s_prev_had3d = s_seen3d;   // remember whether this frame was a gameplay (3D) frame (wide pillarbox gate)
  s_seen3d = 0;       // restart backdrop-vs-HUD discrimination (no 3D prim seen yet next frame)
  { void prim_dump_close_if_done(int); prim_dump_close_if_done(s_frame); }   // PSXPORT_PRIMDUMP: flush the file
}
void GpuState::gpu_present(Core* core) { gpu_present_ex(core, 1); }
// FMV / SCEA-splash teardown (issues #7/#11): black out the DISPLAYED framebuffer region of s_vram and
// present once, so no FMV last-frame or SCEA white-fill survives into the front-end. The resident
// off-display SCEA text page is left alone — the title overwrites that VRAM when it uploads its atlas;
// blacking the DISPLAY region is what removes the visible artifact. Wrap-safe per-pixel (any disp config).
void GpuState::gpu_blank_display() {            // zero the display FB rect (NO present) — caller presents later
  int dw = s_disp_w > 0 ? s_disp_w : 320, dh = s_disp_h > 0 ? s_disp_h : 240;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) *vram(s_disp_x + x, s_disp_y + y) = 0;   // opaque black (555, bit15=0)
}
void GpuState::gpu_clear_display(Core* core) { gpu_blank_display(); gpu_present(core); }
// VK 60fps in-between pass: present whatever draws are accumulated in the VK batch (over the current
// s_vram 2D background), then end the VK frame to reset the batch + restart the per-frame depth order.
// No s_frame++/diagnostics (that bookkeeping happens once per LOGIC frame in gpu_present_ex for the real
// pass). fps60 emits the interpolated RqItems, calls this to show them, then emits the real frame.
void GpuState::gpu_fps60_present_pass(Core* core) {
  present_window();                          // blit_src(s_vram) -> gpu_gpu_present renders the batch + shows
  // Plain per-present reset: this pass emitted the WHOLE queue (color prims AND the shadow tris carried on
  // each opaque world item, via RenderQueue::emitItem) and presented through the full pipeline (panel_render ->
  // shadow_pass + ssao_pass). frame_end resets BOTH the draw batch and the shadow stream; the REAL pass
  // that follows re-emits the same queue, rebuilding an identical shadow map. No keep_shadow side-channel —
  // both 60fps presents are the same full pipeline by construction, so the shadow/HUD/2D are correct on both.
  gpu_gpu_frame_end(core, s_vram, s_frame);   // submit/diff + reset the VK draw + shadow batch
  s_prim_order = 0;                          // restart per-frame OT submission order for the next pass
}

void gpu_native_init(void) {
  if (cfg_dbg("gpu") || cfg_on("PSXPORT_GPU_LOG")) s_log = 1;   // diagnostic: per-frame prim log via env
  if (cfg_dbg("red")) s_reddbg = 1;
  const char* cw = cfg_str("PSXPORT_CLUTWATCH");
  if (cw) { s_cw_x = 880; s_cw_y = 507; int x, y; if (sscanf(cw, "%d,%d", &x, &y) == 2) { s_cw_x = x; s_cw_y = y; } }
}

// Read-only VRAM inspection accessor (raw 16-bit 555+mask word). Used by the offline GPU-QA
// harness to assert exact rasterized/blended values; harmless in production (read-only).
uint16_t GpuState::gpu_vram_peek(int x, int y) { return *vram(x, y); }

// PC-native SCEA decode: turn the baked 4bpp+CLUT asset (scea_asset.h) into a flat RGBA8 buffer laid out
// at the 640x468 SCEA SCREEN positions (the same 3 rects / texpage / CLUT / UV the PSX boot stub used,
// mirroring scea_splash_composite). Text pixels = the CLUT color (a=255); everywhere else = (0,0,0,0).
// This is the PSX-FREE source for gpu_gpu_present_image (no s_vram poke, no VRAM mirror) — `out` must be
// SCEA_DISP_W * SCEA_DISP_H * 4 bytes. The 5-bit PSX color channels are expanded to 8-bit (<<3 | >>2).
void gpu_scea_decode_rgba(uint8_t* out) {
  memset(out, 0, (size_t)SCEA_DISP_W * SCEA_DISP_H * 4);   // transparent/black background
  struct { int sx, sy, w, h, u0, v0; } sp[3] = {           // screen rect + texture UV origin (from the SCEA GP0)
    { 536, 200,  64, 32, 0, 128 }, { 280, 200, 256, 64, 0, 64 }, { 24, 200, 256, 64, 0, 0 } };
  for (int s = 0; s < 3; s++)
    for (int r = 0; r < sp[s].h; r++)
      for (int c = 0; c < sp[s].w; c++) {
        int u = sp[s].u0 + c, v = sp[s].v0 + r;
        // 4bpp texel: the asset word for this (u,v) is scea_tex_words[v*W + (u>>2)] (the same word the
        // PSX boot read from VRAM at texpage (832,256)); the nibble is selected by (u&3). The baked
        // texture is 64 words x 160 rows — exactly covering all 3 rects' (u>>2) in [0,64), v in [0,160).
        int tu = u >> 2, tv = v;
        if (tu < 0 || tu >= SCEA_TEX_W || tv < 0 || tv >= SCEA_TEX_H) continue;
        uint16_t word = scea_tex_words[tv * SCEA_TEX_W + tu];
        int idx = (word >> ((u & 3) * 4)) & 0xF;
        uint16_t cl = scea_clut_words[idx];
        if (cl == 0) continue;                               // PSX textured: a 0x0000 CLUT entry is transparent
        int R = (cl & 31), G = (cl >> 5) & 31, B = (cl >> 10) & 31;
        int x = sp[s].sx + c, y = sp[s].sy + r;
        if (x < 0 || x >= SCEA_DISP_W || y < 0 || y >= SCEA_DISP_H) continue;
        uint8_t* px = out + ((size_t)y * SCEA_DISP_W + x) * 4;
        px[0] = (uint8_t)((R << 3) | (R >> 2)); px[1] = (uint8_t)((G << 3) | (G >> 2));
        px[2] = (uint8_t)((B << 3) | (B >> 2)); px[3] = 255;
      }
}

// (Was `int gpu_prims_since_present(void)` reading a file-scope s_prims — retired 2026-07-03; s_prims
// moved onto GpuState, and this accessor had no external callers. Read via `c->game->gpu.s_prims` now.)

// Bulk VRAM load/save (1024x512x16). Used by the offline GP0 differ harness (tools/gpu_differ):
// seed s_vram with a captured initial VRAM, replay a GP0 word stream via gpu_gp0(), then read back
// the rasterized result for a pixel-exact compare against Beetle on the identical input.
void GpuState::gpu_vram_load(const uint16_t* src) { memcpy(s_vram, src, sizeof(s_vram)); }
void GpuState::gpu_vram_save(uint16_t* dst)       { memcpy(dst, s_vram, sizeof(s_vram)); }

// Enable per-pixel provenance stamping unconditionally (the live debug server turns this on at
// startup so `provat` works at any time without PSXPORT_PROVAT). Cheap: one extra store per pixel.
void GpuState::gpu_provat_enable() { s_prov_on = 1; }

int GpuState::gpu_frame_no() { return s_frame; }

// Diagnostic dumps (gpu_prov_dump / gpu_provat_display / gpu_scene_dump[_now]) live in gpu_debug.c.
void gpu_scene_dump(Core*, FILE*, uint32_t);

// DMA channel 2 (GPU): walk an ordering-table linked list from `madr`, feeding each node's
// GP0 words to the parser. Header word: bits[24..31]=word count, bits[0..23]=next node addr
// (0xFFFFFF = end).
void GpuState::gpu_dma2_linked_list(Core* core, uint32_t madr, bool twoDOnly) {
  { static int sd = -2; if (sd == -2) { const char* e = cfg_str("PSXPORT_SCENEDUMP"); sd = e ? atoi(e) : -1; }
    if (sd >= 0 && s_frame == sd) gpu_scene_dump(core, stderr, madr); }
  s_dma2++;
  s_ot_madr = madr & 0x1FFFFC;
  s_ot_2d_only = twoDOnly;
  // PSXPORT_DEBUG=ot (diagnostic only — the driver no longer reads the OT): on a chain that fails to
  // terminate within an OT's worth of nodes (cyclic = malformed), dump its first 40 nodes once for diagnosis.
  // (Empty OTs are ~0x800 link-only nodes that DO terminate at the sentinel; a true cycle never terminates.)
  if (cfg_dbg("ot")) {
    static int dumped = 0;
    uint32_t a = madr & 0x1FFFFC; int term = 0;
    for (int k = 0; k < 4096; k++) {
      uint32_t next = core->mem_r32(a) & 0xFFFFFF;
      if (next == 0xFFFFFF || next == 0) { term = 1; break; }
      a = next & 0x1FFFFC;
    }
    if (!term && !dumped++) {
      a = madr & 0x1FFFFC;
      fprintf(stderr, "[otdbg] MALFORMED OT from madr=0x%08X:\n", 0x80000000u | (madr & 0x1FFFFC));
      for (int k = 0; k < 40; k++) {
        uint32_t hdr = core->mem_r32(a); uint32_t next = hdr & 0xFFFFFF; int n = hdr >> 24;
        fprintf(stderr, "  [%2d] @0x%08X hdr=0x%08X (n=%d) -> 0x%08X\n",
                k, 0x80000000u | a, hdr, n, 0x80000000u | (next & 0x1FFFFC));
        if (next == 0xFFFFFF || next == 0) break;
        a = next & 0x1FFFFC;
      }
    }
  }
  // Enumerate this DrawOTag's prims in OT LINK order (the guest draw order), feeding each prim's GP0 words
  // to gpu_gp0() which (a) APPLIES the GPU state commands (E1 texpage, E2 texwindow, …) and (b) classifies
  // drawables into the engine render queue (RQ_BACKGROUND/WORLD/HUD). This is NOT "honoring the PSX
  // visibility order": the engine still OWNS what ends up on top — 3D world prims carry real per-vertex
  // depth (RQ_OM_DEPTH → the depth buffer decides occlusion, order-independent). What link order DOES give
  // us is the only correct enumeration for replaying guest GP0: GP0 state commands are ORDER-DEPENDENT and
  // must be applied in DRAW order, because each 2D sprite/poly binds the texpage/texwindow set by the E1/E2
  // node that PRECEDES it in the OT. (later-172 replaced this with a LINEAR packet-pool scan on the premise
  // that "memory order ≡ draw order, the engine re-sorts anyway." That premise is FALSE for 2D: a 2D OT
  // links its nodes in REVERSE allocation order, so the linear scan decoupled every E1 DR_TPAGE from its
  // sprite — the title/menu's two full-screen background sprites then sampled a STALE texpage and rendered
  // BLACK. The 3D field was unaffected only because its prims carry their texpage inline and the depth
  // buffer owns order. Owning 2D order from engine-side SCENE data — instead of replaying guest packets at
  // all — is the remaining M4 work; until then the guest draw order is the correct enumeration to replay.)
  uint32_t addr = madr & 0x1FFFFC;
  int guard;
  for (guard = 0; guard < 0x10000; guard++) {
    uint32_t hdr = core->mem_r32(addr);
    unsigned n = hdr >> 24;                                         // primitive GP0-word count (tag high byte)
    s_cur_node = 0x80000000u | addr;
    for (unsigned i = 0; i < n; i++) { s_gp0_src = addr + 4 + i * 4;  // guest addr of this word (Phase-1 attach)
                                       gpu_gp0(core, core->mem_r32(addr + 4 + i * 4)); }
    uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) break;
    addr = next & 0x1FFFFC;
  }
  s_gp0_src = 0;   // non-OT gpu_gp0 callers (direct GP0 / FMV / block) carry no packet address
  // PSXPORT_DEBUG=pool: per-DrawOTag OT node count + the packet-pool high-water (write ptr 0x800BF544),
  // to inspect the widescreen fixed-buffer-overflow hypothesis (later-124). node count = OT entries the
  // walk traversed. (Pool write ptr is the field overlay's global; meaningless on non-field overlays.)
  if (cfg_dbg("pool")) {
    static int mx = 0; int nodes = guard + 1;
    uint32_t pool = core->mem_r32(0x800BF544u);
    if ((int)pool > mx) mx = (int)pool;
    fprintf(stderr, "[pool] f%d madr=0x%08X nodes=%d pool=0x%08X hi=0x%08X\n",
            s_frame, 0x80000000u | s_ot_madr, nodes, pool, (uint32_t)mx);
  }
  if (guard >= 0x10000) {
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[gpu] WARN: OT walk hit %d-node cap (madr=0x%08X) — "
                           "malformed/cyclic ordering table\n", guard, 0x80000000u | s_ot_madr);
  }
  s_ot_2d_only = false;   // walk done — the parameter is scoped to this call
}
// DMA channel 2 block mode: `count` words from `madr` (to/from GP0). to_gpu=1 -> GP0 writes.
void GpuState::gpu_dma2_block(Core* core, uint32_t madr, int count, int to_gpu) {
  s_dma2++;
  uint32_t addr = madr & 0x1FFFFC;
  s_dma_src = addr;
  for (int i = 0; i < count; i++) { if (to_gpu) gpu_gp0(core, core->mem_r32(addr)); addr += 4; }
}

// ---- Public GPU API: thin free-function wrappers over the per-instance GpuState methods. Keep the
// C-style call sites stable; each forwards to core->game->gpu (de-globalization, 2026-06-19). ----
void gpu_gp0(Core* core, uint32_t w) { core->game->gpu.gpu_gp0(core, w); }
void gpu_gp1(Core* core, uint32_t w) { core->game->gpu.gpu_gp1(w); }
// PC-NATIVE single display: set the displayed VRAM origin directly (what GP1(0x05) would set), so the
// present can scan a fixed page without going through the PSX disp-env / PutDispEnv struct dance. Used by
// native_step_frame to display the single buffer the engine draws into. The display W/H are unchanged
// (still driven by the mode/range GP1(0x07/0x08) the boot env sets once).
void gpu_set_disp_origin(Core* core, int x, int y) { core->game->gpu.s_disp_x = x; core->game->gpu.s_disp_y = y; }
void gpu_dma2_linked_list(Core* core, uint32_t madr, bool twoDOnly) { core->game->gpu.gpu_dma2_linked_list(core, madr, twoDOnly); }
void gpu_dma2_block(Core* core, uint32_t madr, int count, int to_gpu) { core->game->gpu.gpu_dma2_block(core, madr, count, to_gpu); }
void gpu_present(Core* core) { core->game->gpu.gpu_present(core); }
void gpu_present_ex(Core* core, int do_blit) { core->game->gpu.gpu_present_ex(core, do_blit); }
// PSXPORT_SBS accessors: each core's CPU front-buffer (s_vram) + its current display region, so the SBS
// composite can present each core's frame into its own pane (gpu_gpu_present_sbs). GpuState is a plain
// struct (all-public), so these reach the members directly.
const uint16_t* gpu_vram_ptr(Core* core) { return core->game->gpu.s_vram; }
void gpu_disp_region(Core* core, int* sx, int* sy, int* w, int* h) {
  GpuState& g = core->game->gpu;
  if (sx) *sx = g.s_disp_x; if (sy) *sy = g.s_disp_y; if (w) *w = g.s_disp_w; if (h) *h = g.s_disp_h;
}
void gpu_clear_display(Core* core) { core->game->gpu.gpu_clear_display(core); }
void gpu_fps60_present_pass(Core* core) { core->game->gpu.gpu_fps60_present_pass(core); }
void gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src) { core->game->gpu.gpu_native_load_image(core, x, y, w, h, src); }
int  gpu_native_load_vram(Core* core, const char* path) { return core->game->gpu.gpu_native_load_vram(path); }
void gpu_native_shot(Core* core, const char* path) { core->game->gpu.gpu_native_shot(core, path); }
int  gpu_frame_no(Core* core) { return core->game->gpu.gpu_frame_no(); }
uint16_t gpu_vram_peek(Core* core, int x, int y) { return core->game->gpu.gpu_vram_peek(x, y); }
void gpu_vram_load(Core* core, const uint16_t* src) { core->game->gpu.gpu_vram_load(src); }
void gpu_vram_save(Core* core, uint16_t* dst) { core->game->gpu.gpu_vram_save(dst); }
