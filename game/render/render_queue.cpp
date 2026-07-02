// Engine-owned render queue — see render_queue.h. Per-instance state lives on Game (game.h);
// the free rq_* API forwards to core->game->rq.
#include "render_queue.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include "gpu_gpu.h"
#include <algorithm>
#include <unordered_map>
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>

// Debug object-ID overlay: split into QUAD (billboard) and 3D-OBJECT (mesh) highlighting so the user can
// box only one class. On when its RmlUi/cfg toggle is set. `objid` channel + legacy debug_ids = both.
static inline int objid_quads_on(void)   { return g_mods.debug_quads   || g_mods.debug_ids || cfg_dbg("objid"); }
static inline int objid_objects_on(void) { return g_mods.debug_objects || g_mods.debug_ids || cfg_dbg("objid"); }
static inline int objid_on(void) { return objid_quads_on() || objid_objects_on(); }

void fps60_bb_frame_reset(void);   // clear the per-frame billboard-identity registry (engine/fps60.cpp)
int  gpu_gpu_enabled(void);        // gpu_gpu.cpp — Core*-less device-singleton query (declared at use)

// ---- Debug OBJECT-ID overlay (REPL `debug objid`) -------------------------------------------------
// Draw each rendered object's engine identity ON the object, in the live game, so the user can point at
// any object ("the flame at A3F2 flickers") and we share a stable name for it. The ID is the engine's own
// per-object key (RqItem::fps_key) — for a mesh the persistent render-command ptr (stable across frames),
// for a billboard the render-object node. This is a pure HOST overlay: it appends extra HUD quads to the
// render queue (so it flows through WHICHEVER present path is active — the inline emit OR the fps60
// double-emit — with no separate draw path), and touches no guest RAM. Gated by `debug objid`; zero cost
// otherwise. It is injected at the TOP of flush(), before the sort + before the fps60 capture, so the
// labels sort into the HUD layer (drawn on top) and ride along on both 60fps present passes.
//
// Readable PC-native 5x7 ASCII font (digits, hex A-F, sign/punct) for the objid labels — bigger + cleaner
// than the old cramped 3x5 hex glyphs. Bit b4..b0 = leftmost..rightmost of each 5-wide row; 7 rows top->
// bottom. Indexed by font_idx() over the small char set the labels use. Built-in so the overlay never
// depends on the game's font atlas / CLUT.
static const unsigned char FONT5x7[][7] = {
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
  {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
  {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
  {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
  {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A (10)
  {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
  {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
  {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F (15)
  {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // - (16)
  {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, // , (17)
  {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, // : (18)
  {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, // # (19)
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space (20)
};
static int font_idx(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  switch (ch) { case '-': return 16; case ',': return 17; case ':': return 18; case '#': return 19; }
  return 20; // space / unknown
}

// Push one solid (untextured, mode-3) HUD quad [x0,y0]-[x1,y1] of colour (r,g,b). Clip/texpage state is
// copied from a real reference prim so the quad is never clipped away by a stale draw-area. Returns false
// on queue overflow.
static bool objid_solid(Core* core, const RqItem* ref, int x0, int y0, int x1, int y1,
                        unsigned char r, unsigned char g, unsigned char b) {
  RqItem* it = core->game->rq.push();
  if (!it) return false;
  uint32_t seq = it->seq;                      // push() stamped the submission seq — preserve it
  *it = RqItem{};                              // zero every field (fps_*, sh_cast, has_xyf, depth, ...)
  it->seq = seq;
  it->layer = RQ_HUD; it->order_mode = RQ_OM_2D_FG; it->nv = 4; it->mode = 3; it->raw = 0;
  it->xs[0]=x0; it->ys[0]=y0; it->xs[1]=x1; it->ys[1]=y0; it->xs[2]=x0; it->ys[2]=y1; it->xs[3]=x1; it->ys[3]=y1;
  for (int k = 0; k < 4; k++) { it->rs[k]=r; it->gs[k]=g; it->bs[k]=b; it->us[k]=0; it->vs[k]=0; }
  it->tp_x=ref->tp_x; it->tp_y=ref->tp_y; it->clut_x=ref->clut_x; it->clut_y=ref->clut_y;
  it->tw_mx=ref->tw_mx; it->tw_my=ref->tw_my; it->tw_ox=ref->tw_ox; it->tw_oy=ref->tw_oy;
  it->da_x0=ref->da_x0; it->da_y0=ref->da_y0; it->da_x1=ref->da_x1; it->da_y1=ref->da_y1;
  return true;
}

// Draw one 5x7 glyph at (x,y), pixel scale s, colour (r,g,b).
static void objid_char(Core* core, const RqItem* ref, char ch, int x, int y, int s,
                       unsigned char r, unsigned char g, unsigned char b) {
  const unsigned char* gph = FONT5x7[font_idx(ch)];
  for (int row = 0; row < 7; row++)
    for (int col = 0; col < 5; col++)
      if (gph[row] & (1 << (4 - col))) {
        int px = x + col * s, py = y + row * s;
        objid_solid(core, ref, px, py, px + s, py + s, r, g, b);
      }
}
// Draw a string at (x,y) scale s with a dark backing box; glyphs in (r,g,b). Advance 6*s px per char.
static void objid_str(Core* core, const RqItem* ref, int x, int y, int s, const char* str,
                      unsigned char r, unsigned char g, unsigned char b) {
  int n = 0; for (const char* p = str; *p; p++) n++;
  if (!n) return;
  objid_solid(core, ref, x - s, y - s, x + n * 6 * s, y + 7 * s + s, 0, 0, 0);   // dark backing box
  int cx = x;
  for (const char* p = str; *p; p++) { objid_char(core, ref, *p, cx, y, s, r, g, b); cx += 6 * s; }
}

// Draw a 1px-scaled hollow rectangle outline (4 thin solid quads) in colour (r,g,b).
static void objid_box(Core* core, const RqItem* ref, int x0, int y0, int x1, int y1,
                      unsigned char r, unsigned char g, unsigned char b, int t) {
  objid_solid(core, ref, x0, y0, x1, y0 + t, r, g, b);   // top
  objid_solid(core, ref, x0, y1 - t, x1, y1, r, g, b);   // bottom
  objid_solid(core, ref, x0, y0, x0 + t, y1, r, g, b);   // left
  objid_solid(core, ref, x1 - t, y0, x1, y1, r, g, b);   // right
}

// Box + label every live GAME OBJECT, identified by ENUMERATING the render-node list (head 0x800F2624,
// next at node+0x24) — NOT by grouping emitted quads (which collapsed all objects into ONE box because
// quad->object attribution went through fragile packet-span correlation, and the user has no per-quad
// ownership). Each per-object node carries its real WORLD position at node+0x2E/0x32/0x36; we project that
// through the stable scene camera (proj_camview_world_screen) and draw a box + the object's id + WORLD
// coordinates in readable PC-native text. A node is classified a QUAD (2D sprite at a 3D position =
// billboard) if any prim it emitted this frame is an fps_anchor billboard; else a 3D-MESH object. The two
// classes have independent toggles (debug_quads / debug_objects) so the user can highlight ONLY quads.
// Pure host overlay (reads guest RAM, writes only the queue).
static void objid_overlay(RenderQueue* q, Core* core) {
  int  proj_camview_world_screen(float, float, float, float*, float*);
  void camview_publish(const float R[3][3], const float T[3]);
  // Capture the STABLE scene camera from the scratchpad NOW (at flush = frame end): 0x1F8000F8 holds the
  // camera rotation (CR-packed, /4096) and 0x1F80010C the translation (int32 view units). The per-object
  // render uses a SEPARATE scratchpad matrix area (SCR+0), so the camera here is the frame's real scene
  // camera (verified: projects the field objects in front). proj_camview_world_screen then maps each
  // object's world position to screen. (Same data native_terrain published; terrain is orphaned now.)
  {
    uint32_t S = 0x1F800000u;
    uint32_t k0=core->mem_r32(S+0xF8),k1=core->mem_r32(S+0xFC),k2=core->mem_r32(S+0x100),
             k3=core->mem_r32(S+0x104),k4=core->mem_r32(S+0x108);
    float R[3][3] = {
      {(int16_t)k0/4096.0f,        (int16_t)(k0>>16)/4096.0f, (int16_t)k1/4096.0f},
      {(int16_t)(k1>>16)/4096.0f,  (int16_t)k2/4096.0f,       (int16_t)(k2>>16)/4096.0f},
      {(int16_t)k3/4096.0f,        (int16_t)(k3>>16)/4096.0f, (int16_t)k4/4096.0f} };
    float T[3] = {(float)(int32_t)core->mem_r32(S+0x10C),(float)(int32_t)core->mem_r32(S+0x110),(float)(int32_t)core->mem_r32(S+0x114)};
    camview_publish(R, T);
  }
  int n0 = q->n;                               // freeze the count: only scan real prims, not our own labels
  // Reference prim for clip/texpage state (so the HUD quads aren't clipped by a stale draw-area).
  const RqItem* ref = 0;
  for (int i = 0; i < n0; i++) if (q->items[i].layer == RQ_WORLD) { ref = &q->items[i]; break; }
  if (!ref && n0 > 0) ref = &q->items[0];
  if (!ref) return;
  // The game objects live in the engine's active entity lists (doubly-linked, next @ node+0x24, end =
  // next==0). There are three (heads @ 0x800FB168 / 0x800F2624 / 0x800F2738; the object walk uses the
  // first two, cull touches all three). Walk all three so EVERY live object is enumerated individually.
  static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
  static int s_logframe = 0; int dolog = objid_on() && ((s_logframe++ % 120) == 0);
  int nquad = 0, nobj = 0, nlive = 0;
  for (int li = 0; li < 3; li++) {
    for (uint32_t n = core->mem_r32(HEADS[li]), g = 0; n >= 0x80000000u && n < 0x80200000u && g < 512;
         n = core->mem_r32(n + 0x24), g++) {
      if (core->mem_r8(n + 1) == 0) continue;                        // not live
      nlive++;
      int16_t wx = (int16_t)core->mem_r16(n + 0x2E);
      int16_t wy = (int16_t)core->mem_r16(n + 0x32);
      int16_t wz = (int16_t)core->mem_r16(n + 0x36);
      // QUAD (billboard) vs 3D-MESH classification by INTRINSIC render type (node+0xb): the per-object
      // render dispatcher (gen_func_8003C048) routes render types 0x10..0x14 to the SPRITE/BILLBOARD
      // submitters (single object-center RTPS -> screen quad: e.g. the AP-crystal pickup), while 0/0xf are
      // mesh. (The old fps_anchor signal is dead — its feeder ov_render_cmd is orphaned post override-removal.)
      uint8_t rtype = core->mem_r8(n + 0xB);
      int quad = (rtype >= 0x10 && rtype <= 0x14) ? 1 : 0;
      if (quad ? !objid_quads_on() : !objid_objects_on()) continue;  // class toggled off
      float sx = 0, sy = 0;
      if (!proj_camview_world_screen((float)wx, (float)wy, (float)wz, &sx, &sy)) continue;  // behind camera
      int cx = (int)(sx + 0.5f), cy = (int)(sy + 0.5f);
      if (cx < -60 || cx > 420 || cy < -40 || cy > 280) continue;   // off-screen
      if (quad) nquad++; else nobj++;
      if (dolog && quad) fprintf(stderr, "[objid-q] node=%08X rtype=0x%02X scr=(%d,%d) world=(%d,%d,%d) +0xC=%02X +0xD=%02X\n",
                                 n, rtype, (int)(sx+0.5f),(int)(sy+0.5f), wx,wy,wz, core->mem_r8(n+0xC), core->mem_r8(n+0xD));
      unsigned char br = quad ? 255 : 0, bg = 255, bb = quad ? 0 : 255;   // quads yellow, 3D objects cyan
      objid_box(core, ref, cx - 6, cy - 6, cx + 6, cy + 6, br, bg, bb, 1);
      char l1[16], l2[40];
      snprintf(l1, sizeof l1, "#%04X", (unsigned)(n & 0xFFFF));      // per-instance id (node handle)
      snprintf(l2, sizeof l2, "%d,%d,%d", wx, wy, wz);               // WORLD coordinates
      objid_str(core, ref, cx + 9, cy - 7, 1, l1, br, bg, bb);
      objid_str(core, ref, cx + 9, cy + 6, 1, l2, br, bg, bb);
    }
  }
  if (dolog) fprintf(stderr, "[objid] === %d live; %d quads + %d 3D boxed ===\n", nlive, nquad, nobj);
}

// The render queue is THE render path — one behavior, the PC game. No env gate (user directive
// 2026-06-20: "have only one behavior that is PC game"). The lone exception is the PSXPORT_SBS dual-channel
// debug COMPARE tool, which keeps its own inline path; callers check gpu_sbs_get() for that, not this.
int rq_active(void) { return 1; }

void RenderQueue::reset() { n = 0; seq = 0; consumed = 0; }

RqItem* RenderQueue::push() {
  if (consumed) { reset(); fps60_bb_frame_reset(); }   // first push after a flush -> new frame (clear bb registry too)
  if (n >= RQ_MAX) {
    // FAIL-FAST (user 2026-06-30): never silently drop prims. RQ_MAX already covers the real worst-case
    // scene (the area-transition spike, ~43k — see render_queue.h); exceeding it means a submit path is
    // running away (e.g. a stuck render walk re-submitting the same scene every frame — the bug-1 / later-273
    // symptom). Abort with a C backtrace so that submit path is visible rather than hidden behind a drop.
    fprintf(stderr, "\n[rq] FATAL: render queue full (%d items) — refusing to drop prims (fail-fast).\n"
                    "  A submit path produced > %d prims this frame (runaway re-submission?). Backtrace:\n",
            RQ_MAX, RQ_MAX);
    void* bt[32]; int nbt = backtrace(bt, 32); backtrace_symbols_fd(bt, nbt, 2);
    fflush(stderr);
    abort();
  }
  RqItem* it = &items[n++];
  it->seq = seq++;
  return it;
}

void RenderQueue::mark_consumed() { if (n) consumed = 1; }

void RenderQueue::flush(Core* core) {
  if (n && objid_on()) objid_overlay(this, core);   // debug: label each object with its engine ID
  // Engine-decided order: layer low->high, submission order within a layer. stable_sort keeps the
  // within-layer submission order exactly (matters for semi-transparent blending). The D32 depth buffer
  // does fine-grained occlusion inside RQ_WORLD regardless of this order.
  if (n) std::stable_sort(items, items + n, [](const RqItem& a, const RqItem& b) {
    return a.layer != b.layer ? a.layer < b.layer : a.seq < b.seq;
  });
  // fps60: the interpolated-60fps tier OWNS presentation — it needs to emit this frame TWICE (the lerped
  // in-between, then the real frame), so it must hold the items rather than have flush emit them now.
  // Snapshot the sorted queue to it and skip the inline emit; fps60_present_vk emits + presents both.
  // BUT only when this core actually presents per-frame: under diff_mode (the SBS dual-core compare) the
  // per-core present is suppressed (ov_frame_update early-returns on diff_mode), so fps60_present_vk NEVER
  // runs — capturing here would leave the captured queue undrawn and the geometry batch EMPTY, which is
  // exactly why the SBS panes rendered black (worldquads queued, batch tex=0). In diff_mode the SBS
  // composite reads the geometry batch directly via gpu_gpu_render_readback, so the flush MUST do the
  // inline emit to fill that batch. Gate the fps60 capture on !diff_mode.
  extern int g_fps60_on;
  if (g_fps60_on && !core->game->diff_mode) { core->game->fps60.rq_capture(items, n); mark_consumed(); return; }
  if (!n) { mark_consumed(); return; }
  // `debug rqhist` (diag): per-frame histogram of what the queue actually emits, by layer × opaque/semi.
  // Answers "the native field shows only sky/sea — is the LAND geometry even being queued as opaque world
  // prims?" without depending on shader paint behavior (PAINTWORLD's mode=3 is unreliable through the
  // textured pipe). bg=RQ_BACKGROUND world=RQ_WORLD ovl=RQ_OVERLAY hud=RQ_HUD. (diag, 2026-06-26; render.md OPEN #1)
  if (cfg_dbg("rqhist")) {
    int c[4][2] = {{0,0},{0,0},{0,0},{0,0}};
    for (int i = 0; i < n; i++) { int L = items[i].layer & 3, sm = items[i].semi ? 1 : 0; c[L][sm]++; }
    static int lf = 0; if ((lf++ % 30) == 0)
      fprintf(stderr, "[rqhist] n=%d  bg(op/semi)=%d/%d  WORLD=%d/%d  ovl=%d/%d  hud=%d/%d\n",
              n, c[0][0],c[0][1], c[1][0],c[1][1], c[2][0],c[2][1], c[3][0],c[3][1]);
  }
  for (int i = 0; i < n; i++) gpu_emit_rq_item(core, &items[i]);
  mark_consumed();
}

// ---- Native render-queue EMISSION (moved from gpu_native.cpp, 2026-07 restructure): the engine's OWN
// render-queue API (RqItem-based world/2D quad submission with real per-vertex depth + order_mode + shadow-
// cast tagging), as distinct from gpu_native.cpp's PSX GP0-packet interpreter/rasterizer. gpu_draw_world_quad
// / rq_push_2d_quad are the entry points game/render (engine_submit.cpp, native_terrain.cpp, mesh_draw.cpp)
// call to submit engine-owned geometry.

// PC-NATIVE world-quad draw (the render-PC-native path — NOT a PSX-packet transcription). Takes a quad
// already projected to FLOAT screen coords + normalized per-vertex depth (proj_pz_to_ord) + decoded
// UV/RGB/texpage/clut, and tees two triangles straight to the VK rasterizer with real per-pixel depth —
// no GP0 packet, no OT, no guest write. The renderer's D32 buffer does true occlusion from the depth.
// Used by engine/native_terrain.cpp. Free function (reaches the per-instance GPU state via core->game->gpu),
// mirroring the geometry tee in gp0_exec (this file ~522-595) but fed float scene data instead of a packet.
// Emit one resolved RqItem to the VK rasterizer. The emission logic (set_order/semi_group/set_vd/draw)
// lives ONLY here; both the inline draw and the engine render-queue flush funnel through it. set_order
// uses the live GpuState counter so the order value reflects actual emit sequence (the 2D-fallback/
// faithful-depth band); real per-vertex depth (set_vd) drives true occlusion for world prims.
int gpu_gpu_enabled(void);   // gpu_gpu.cpp — Core*-less device-singleton query (declared at use; see gpu_gpu.h)
void gpu_emit_rq_item(Core* core, const RqItem* it) {
  if (!gpu_gpu_enabled()) return;
  GpuState& s = core->game->gpu;
  // PSXPORT_PAINTWORLD=1 (diag): force every opaque RQ_WORLD prim to untextured solid magenta so we can SEE
  // exactly where the native 3D world geometry rasterizes (vs the backdrop). Answers the recurring "the
  // native field shows only sky/sea — where did the world go?" question: if magenta covers the land area,
  // the world IS built+drawn (occlusion/blend bug); if magenta is absent/sparse, ov_scene_native isn't
  // producing that geometry. (diag, 2026-06-26; render.md OPEN #1)
  RqItem pw;
  { static int p=-2; if(p==-2){ const char* e=cfg_str("PSXPORT_PAINTWORLD"); p=e?atoi(e):0; }
    if (p && it->layer == RQ_WORLD && !it->semi) { pw = *it; pw.mode = 3; pw.raw = 0;
      for (int i=0;i<4;i++){ pw.rs[i]=255; pw.gs[i]=0; pw.bs[i]=255; } it = &pw; } }
  // PSXPORT_ONLYWORLD=1 (diag): emit ONLY RQ_WORLD prims — drop backdrop/overlay/HUD — so the readback
  // shows EXACTLY the native 3D world geometry on a black field, with NO shader-paint dependency. Reliable
  // answer to "is the world built but occluded, or is it not landing on-screen?" (diag, 2026-06-26; OPEN #1)
  { static int o=-2; if(o==-2){ const char* e=cfg_str("PSXPORT_ONLYWORLD"); o=e?atoi(e):0; }
    if (o && it->layer != RQ_WORLD) return; }
  // PSXPORT_NOBG=1 (diag): drop ONLY the RQ_BACKGROUND (sky/sea tilemap) — keep world+overlay+HUD. If the
  // world becomes visible, the backdrop is the occluder (despite its far 2D-BG ord). (diag, 2026-06-26)
  { static int nb=-2; if(nb==-2){ const char* e=cfg_str("PSXPORT_NOBG"); nb=e?atoi(e):0; }
    if (nb && it->layer == RQ_BACKGROUND) return; }
  // PSXPORT_NOHUD=1 (diag): drop ONLY the RQ_HUD prims — if the world becomes visible, the sky/sea backdrop
  // is being MIS-CLASSIFIED as HUD (nearest band) and occluding the world. (diag, 2026-06-26; OPEN #1)
  { static int nh=-2; if(nh==-2){ const char* e=cfg_str("PSXPORT_NOHUD"); nh=e?atoi(e):0; }
    if (nh && it->layer == RQ_HUD) return; }
  // Shadow geometry is part of the frame: re-push this prim's view-space verts to the shadow VBO on EVERY
  // emit, so the shadow map rebuilds identically on each 60fps present pass (no keep_shadow side-channel).
  // gpu_gpu_shadow_push_tri no-ops when shadows are off; verts are the B (un-interpolated) positions.
  if (it->sh_cast) {
    float v[4][3]; for (int k = 0; k < 4; k++) { v[k][0]=it->sh_vx[k]; v[k][1]=it->sh_vy[k]; v[k][2]=it->sh_vz[k]; }
    gpu_gpu_shadow_push_tri(core, v[0], v[1], v[2]);
    if ((it->nv ? it->nv : 4) == 4) gpu_gpu_shadow_push_tri(core, v[1], v[2], v[3]);
  }
  const int* xs = it->xs; const int* ys = it->ys; const int* us = it->us; const int* vs = it->vs;
  const unsigned char* rs = it->rs; const unsigned char* gs = it->gs; const unsigned char* bs = it->bs;
  const float* depth = it->depth; int mode = it->mode, raw = it->raw, nv = it->nv ? it->nv : 4;
  // PSXPORT_PRIMAT="x,y" (DISPLAY coords): also log WORLD/queue prims (gpu_draw_world_quad etc.) that cover
  // that pixel — primat in gp0_exec is blind to these (they bypass the OT walk). Shows the real-depth
  // occluders. (diag, 2026-06-24)
  { static int qx=-2, qy=-1, qf0=0; if (qx==-2){ qx=-1; const char* pa=cfg_str("PSXPORT_PRIMAT"); if(pa) sscanf(pa,"%d,%d,%d",&qx,&qy,&qf0); }
    if (qx>=0 && (int)s.s_frame>=qf0) { int ax=s.s_disp_x+qx, ay=s.s_disp_y+qy;
      auto edge=[](int ax_,int ay_,int x0,int y0,int x1,int y1){ return (int64_t)(x1-x0)*(ay_-y0)-(int64_t)(y1-y0)*(ax_-x0); };
      auto intri=[&](int i0,int i1,int i2){ int64_t w0=edge(ax,ay,xs[i1],ys[i1],xs[i2],ys[i2]);
        int64_t w1=edge(ax,ay,xs[i2],ys[i2],xs[i0],ys[i0]); int64_t w2=edge(ax,ay,xs[i0],ys[i0],xs[i1],ys[i1]);
        return (w0>=0&&w1>=0&&w2>=0)||(w0<=0&&w1<=0&&w2<=0); };
      if (intri(0,1,2) || (nv==4 && intri(1,2,3))) { static int n=0; if(n++<6000)
        fprintf(stderr,"[primat-rq] f%d dbgnode=%08X layer=%d om=%d semi=%d depth=[%.4f %.4f %.4f %.4f] col=(%d,%d,%d) xy0=(%d,%d) xy2=(%d,%d)\n",
          s.s_frame, it->dbg_node, it->layer, it->order_mode, it->semi,
          depth?depth[0]:-1.f, depth?depth[1]:-1.f, depth?depth[2]:-1.f, (depth&&nv==4)?depth[3]:-1.f,
          rs[0],gs[0],bs[0], xs[0],ys[0], xs[2],ys[2]); } } }
  unsigned ord = s.s_prim_order++;
  gpu_gpu_set_order(core, ord);
  // Depth: 3D world prims carry real per-vertex view-Z (set_vd); 2D prims select the renderer's far/near
  // screen-space band (preserving the existing 2D depth semantics — only the ORDER is now engine-decided).
  int om = it->order_mode;
  if      (om == RQ_OM_2D_BG) gpu_gpu_set_order_2d_bg(core, ord);
  else if (om == RQ_OM_2D_FG) gpu_gpu_set_order_2d(core, ord);
  #define RQ_SETVD(p) do { if (om == RQ_OM_DEPTH) gpu_gpu_set_vd(core, (p)); } while (0)
  // Vertex smoothing (#15): for the world path, hand the rasterizer the sub-pixel float screen XY. The base
  // pointer maps to vertex [0]; the second triangle of a quad is emitted from &xs[1], so it gets &xsf[1].
  // gpu_gpu_set_order (inside set_order, fired per draw via the *_set_vd/order path) clears s_xf, so a NULL
  // here for non-world prims leaves them snapping to the integer xs/ys. set after set_order, before draw.
  const float* xsf = it->has_xyf ? it->xsf : nullptr;
  const float* ysf = it->has_xyf ? it->ysf : nullptr;
  #define RQ_SETXYF(o) do { gpu_gpu_set_xyf(core, xsf ? xsf+(o) : nullptr, ysf ? ysf+(o) : nullptr); } while (0)
  if (it->semi) {
    int bx0=xs[0],by0=ys[0],bx1=xs[0],by1=ys[0];
    for (int i=1;i<nv;i++){ if(xs[i]<bx0)bx0=xs[i]; if(xs[i]>bx1)bx1=xs[i]; if(ys[i]<by0)by0=ys[i]; if(ys[i]>by1)by1=ys[i]; }
    gpu_gpu_semi_group(core, bx0, by0, bx1, by1);
    RQ_SETVD(depth); RQ_SETXYF(0);
    gpu_gpu_draw_semi(core, (int*)xs, (int*)ys, (int*)us, (int*)vs, (unsigned char*)rs, (unsigned char*)gs, (unsigned char*)bs,
                     it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                     it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1, it->tp_blend);
    if (nv == 4) { RQ_SETVD(&depth[1]); RQ_SETXYF(1);
      gpu_gpu_draw_semi(core, (int*)&xs[1], (int*)&ys[1], (int*)&us[1], (int*)&vs[1], (unsigned char*)&rs[1], (unsigned char*)&gs[1], (unsigned char*)&bs[1],
                       it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                       it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1, it->tp_blend); }
  } else {
    RQ_SETVD(depth); RQ_SETXYF(0);
    gpu_gpu_draw_tritri(core, (int*)xs, (int*)ys, (int*)us, (int*)vs, (unsigned char*)rs, (unsigned char*)gs, (unsigned char*)bs,
                       it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                       it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1);
    if (nv == 4) { RQ_SETVD(&depth[1]); RQ_SETXYF(1);
      gpu_gpu_draw_tritri(core, (int*)&xs[1], (int*)&ys[1], (int*)&us[1], (int*)&vs[1], (unsigned char*)&rs[1], (unsigned char*)&gs[1], (unsigned char*)&bs[1],
                         it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                         it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1); }
  }
  gpu_gpu_set_xyf(core, nullptr, nullptr);   // clear so the next prim (if not world) snaps to integer xs/ys
  #undef RQ_SETVD
  #undef RQ_SETXYF
}

// Build an RqItem from already-resolved quad/tri data + material snapshot, then either queue it (engine
// owns the order, flushed at the draw kick) or emit it now. The ONE place the three submit paths (world
// quad, guest poly, guest sprite) funnel through. `capture` routes to the queue (set during the OT walk
// under PSXPORT_RQ); otherwise it draws inline immediately (default — identical to pre-queue behavior).
// Not static: gpu_native.cpp's guest GP0/OT-walk poly and sprite submit paths (gp0_exec) also funnel their
// queued items through this same one place via their own local extern forward declaration.
void rq_emit_or_queue(Core* core, int capture, int layer, int order_mode, int nv, int semi, int raw,
                             const int* xs, const int* ys, const float* xsf, const float* ysf,
                             const int* us, const int* vs,
                             const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                             const float* depth, int mode, int tp_x, int tp_y, int clut_x, int clut_y,
                             int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1,
                             int tp_blend, const float (*sv)[3] = nullptr) {
  RqItem it;
  it.layer = (uint8_t)layer; it.semi = semi ? 1 : 0; it.nv = (uint8_t)nv; it.raw = raw ? 1 : 0;
  it.order_mode = (uint8_t)order_mode;
  it.fps_world = 0;   // fps60 capture: cleared here, set only by fps60_stamp_world on GTE-composed world prims
  // objid overlay: stamp the entity node the native render walk is currently rendering (engine_submit.cpp).
  // Every world prim an object emits gets its node, so the overlay labels ALL rendered objects. Terrain/
  // static/background prims render with no per-object scope (g_dbg_render_node==0) → correctly unlabeled.
  { extern uint32_t g_dbg_render_node; it.dbg_node = (layer == RQ_WORLD) ? g_dbg_render_node : 0; }
  // Shadow capture: an opaque world prim with view-space verts casts into the shadow map. Carried on the
  // item so gpu_emit_rq_item re-pushes it to the shadow VBO on EVERY emit (= on both 60fps present passes).
  it.sh_cast = sv ? 1 : 0;
  if (sv) for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
            it.sh_vx[k] = sv[s][0]; it.sh_vy[k] = sv[s][1]; it.sh_vz[k] = sv[s][2]; }
  it.has_xyf = (xsf && ysf) ? 1 : 0;   // sub-pixel float XY (vertex smoothing) supplied by the world path
  for (int i = 0; i < nv; i++) { it.xs[i]=xs[i]; it.ys[i]=ys[i]; it.us[i]=us[i]; it.vs[i]=vs[i];
                                 it.xsf[i]= it.has_xyf ? xsf[i] : (float)xs[i];
                                 it.ysf[i]= it.has_xyf ? ysf[i] : (float)ys[i];
                                 it.rs[i]=rs[i]; it.gs[i]=gs[i]; it.bs[i]=bs[i];
                                 it.depth[i] = depth ? depth[i] : 0.0f; }
  it.mode = mode; it.tp_x = tp_x; it.tp_y = tp_y; it.clut_x = clut_x; it.clut_y = clut_y;
  it.tw_mx = tw_mx; it.tw_my = tw_my; it.tw_ox = tw_ox; it.tw_oy = tw_oy;
  it.da_x0 = da_x0; it.da_y0 = da_y0; it.da_x1 = da_x1; it.da_y1 = da_y1; it.tp_blend = tp_blend;
  if (capture) { RqItem* slot = core->game->rq.push(); if (slot) { uint32_t sq = slot->seq; *slot = it; slot->seq = sq; } }
  else         gpu_emit_rq_item(core, &it);
}

// sv (optional, NULL = no shadow): the prim's 4 VIEW-SPACE verts (x=vx, y=vy, z=pz) for the shadow map.
// When non-NULL and opaque, the queued item carries them and gpu_emit_rq_item re-pushes them as two tris
// to the shadow VBO on every emit (= on both 60fps present passes — see render_queue.h sh_cast).
long g_dbg_world_quads = 0;   // PSXPORT_GPU_TRACE: world quads emitted (SBS black-pane diag)
void gpu_draw_world_quad(Core* core, const float* px, const float* py, const float* depth,
                         const int* u, const int* v, const uint8_t* r, const uint8_t* g,
                         const uint8_t* b, uint16_t tp, uint16_t clut, int semi,
                         const float (*sv)[3]) {
  if (!gpu_gpu_enabled()) return;
  g_dbg_world_quads++;   // PSXPORT_GPU_TRACE: world quads this frame (SBS diag)
  if (cfg_dbg("silbbox")) { static int once=0; if (!once++) fprintf(stderr, "[silbbox] s_off=(%d,%d)\n", core->game->gpu.s_off_x, core->game->gpu.s_off_y); }
  GpuState& s = core->game->gpu;
  s.set_texpage(tp);
  s.set_clut(clut);
  s.s_seen3d = 1;                              // a projected world prim has now been drawn this frame
  int xs[4], ys[4], us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
  float xsf[4], ysf[4];
  for (int i = 0; i < 4; i++) {
    // Vertex smoothing (#15): keep the engine's SUB-PIXEL float screen XY (draw offset applied in float)
    // for the rasterizer, and round only for the integer xs/ys still used by the 2D bbox/semi-group path.
    xsf[i] = px[i] + (float)s.s_off_x;
    ysf[i] = py[i] + (float)s.s_off_y;
    xs[i] = (int)(px[i] < 0 ? px[i] - 0.5f : px[i] + 0.5f) + s.s_off_x;  // round, then draw offset
    ys[i] = (int)(py[i] < 0 ? py[i] - 0.5f : py[i] + 0.5f) + s.s_off_y;
    us[i] = u[i]; vs[i] = v[i]; rs[i] = r[i]; gs[i] = g[i]; bs[i] = b[i];
  }
  // World geometry: engine layer WORLD with real per-vertex depth. The queue is the render path.
  // Only opaque prims cast a shadow (semi water etc. must not occlude the light); drop the cast if semi.
  const float (*cast)[3] = (sv && !semi) ? sv : nullptr;
  rq_emit_or_queue(core, 1, RQ_WORLD, RQ_OM_DEPTH, 4, semi ? 1 : 0, 0,
                   xs, ys, xsf, ysf, us, vs, rs, gs, bs, depth, s.s_tp_mode,
                   s.s_tp_x, s.s_tp_y, s.s_clut_x, s.s_clut_y, s.s_tw_mx, s.s_tw_my, s.s_tw_ox, s.s_tw_oy,
                   s.s_da_x0, s.s_da_y0, s.s_da_x1, s.s_da_y1, s.s_tp_blend, cast);
}

// 2D quad enqueue (HUD / overlay / background) — funnels through rq_emit_or_queue so a 2D drawable is a
// queued RqItem (part of THE FRAME), not a direct gpu_gpu_draw_tritri that lands on only one 60fps pass.
void RenderQueue::push2dQuad(int layer, int order_2d_fg,
                             const int* xs, const int* ys, const int* us, const int* vs,
                             const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                             int tp_x, int tp_y, int mode, int raw, int clut_x, int clut_y,
                             int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1) {
  if (!gpu_gpu_enabled()) return;
  Core* core = &game->core;
  int om = order_2d_fg ? RQ_OM_2D_FG : RQ_OM_2D_BG;
  rq_emit_or_queue(core, 1, layer, om, 4, 0, raw,
                   xs, ys, nullptr, nullptr, us, vs, rs, gs, bs, nullptr, mode,
                   tp_x, tp_y, clut_x, clut_y, tw_mx, tw_my, tw_ox, tw_oy,
                   da_x0, da_y0, da_x1, da_y1, 0, nullptr);
}
