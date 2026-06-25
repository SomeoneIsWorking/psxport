// Engine-owned render queue — see render_queue.h. Per-instance state lives on Game (game.h);
// the free rq_* API forwards to core->game->rq.
#include "render_queue.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include <algorithm>
#include <unordered_map>
#include <stdio.h>

// Debug object-ID overlay: split into QUAD (billboard) and 3D-OBJECT (mesh) highlighting so the user can
// box only one class. On when its RmlUi/cfg toggle is set. `objid` channel + legacy debug_ids = both.
static inline int objid_quads_on(void)   { return g_mods.debug_quads   || g_mods.debug_ids || cfg_dbg("objid"); }
static inline int objid_objects_on(void) { return g_mods.debug_objects || g_mods.debug_ids || cfg_dbg("objid"); }
static inline int objid_on(void) { return objid_quads_on() || objid_objects_on(); }

void fps60_bb_frame_reset(void);   // clear the per-frame billboard-identity registry (engine/fps60.cpp)

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
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[rq] WARN: render queue full (%d) — dropping prims\n", RQ_MAX);
    return 0;
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
  for (int i = 0; i < n; i++) gpu_emit_rq_item(core, &items[i]);
  mark_consumed();
}

RqItem* rq_push(Core* core)   { return core->game->rq.push(); }
void    rq_flush(Core* core)  { core->game->rq.flush(core); }
