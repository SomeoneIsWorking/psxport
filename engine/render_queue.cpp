// Engine-owned render queue — see render_queue.h. Per-instance state lives on Game (game.h);
// the free rq_* API forwards to core->game->rq.
#include "render_queue.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include <algorithm>
#include <unordered_set>
#include <stdio.h>

// Debug object-ID overlay is on when the RmlUi Debug-tab toggle is set OR the `objid` debug channel is on.
static inline int objid_on(void) { return g_mods.debug_ids || cfg_dbg("objid"); }

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
// 3x5 bitmap font, hex 0..F (bit2 = leftmost column of each 3-wide row). Built-in so the overlay never
// depends on the game's font atlas / CLUT (which is the tangle issue #6 lives in).
static const unsigned char OBJID_FONT[16][5] = {
  {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},
  {7,5,7,5,7},{7,5,7,1,7},{7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4} };

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

// Draw a single 3x5 glyph (index into OBJID_FONT) at (x,y), pixel scale s.
static void objid_glyph(Core* core, const RqItem* ref, int g, int x, int y, int s) {
  const unsigned char* gph = OBJID_FONT[g & 0xF];
  for (int row = 0; row < 5; row++)
    for (int col = 0; col < 3; col++)
      if (gph[row] & (1 << (2 - col))) {
        int px = x + col * s, py = y + row * s;
        objid_solid(core, ref, px, py, px + s, py + s, 255, 255, 0);      // yellow pixel
      }
}

// Draw `val` as `ndig` HEX digits at (x,y), scale s, on a dark backing box.
static void objid_hex(Core* core, const RqItem* ref, uint32_t val, int ndig, int x, int y, int s) {
  int w = ndig * 4 * s, h = 5 * s;
  objid_solid(core, ref, x - s, y - s, x + w, y + h, 0, 0, 0);            // backing box
  for (int d = 0; d < ndig; d++)
    objid_glyph(core, ref, (val >> ((ndig - 1 - d) * 4)) & 0xF, x + d * 4 * s, y, s);
}

// Scan the world prims (captured before we append). One label per unique ENTITY INSTANCE (dbg_node), at
// its first vertex, showing the GAME object id = the entity model id (node+0xe & 0x3fff) in decimal.
static void objid_overlay(RenderQueue* q, Core* core) {
  int n0 = q->n;                               // freeze the count: only label real prims, not our own labels
  std::unordered_set<uint32_t> seen;
  int nworld = 0, nnoded = 0, nlabel = 0;
  static int s_logframe = 0;                    // throttle the stderr table to once every ~60 frames
  int dolog = objid_on() && ((s_logframe++ % 60) == 0);   // log whether enabled via channel OR the menu toggle
  for (int i = 0; i < n0; i++) {
    const RqItem* it = &q->items[i];
    if (it->layer != RQ_WORLD) continue;
    nworld++;
    uint32_t node = it->dbg_node;
    if (node == 0) continue;                                   // no entity node resolved for this prim
    nnoded++;
    if (!seen.insert(node).second) continue;                   // one label per entity INSTANCE
    // The model-id field (node+0xe) reads 0 for the captured entities, so display the per-instance node
    // handle (low 16 bits, hex) — stable + unique per object. The stderr scan below hunts for the real
    // game-id field; once found we switch the label to it.
    objid_hex(core, it, node & 0xFFFF, 4, it->xs[0], it->ys[0], 2);
    nlabel++;
    if (dolog) {
      // Hunt for where the game id (352/353/354 = 0x160/0x161/0x162) actually lives: scan the node struct
      // (0..0xD0) at every byte offset, reporting any u16 in [0x150,0x180]. Also dump node[0..0x3F] words.
      char hits[256]; int hp = 0; hits[0] = 0;
      for (int off = 0; off + 1 < 0xD0; off++) {
        uint32_t v = core->mem_r16(node + off);
        if (v >= 0x150 && v <= 0x180 && hp < (int)sizeof(hits) - 24)
          hp += snprintf(hits + hp, sizeof(hits) - hp, " +%X=%u", off, v);
      }
      fprintf(stderr, "[objid] %s node=%08x type=%02x pos=(%d,%d,%d) idhits[%s ] w0..3=%08x %08x %08x %08x\n",
              it->fps_anchor ? "BILLBOARD" : "MESH", node, core->mem_r8(node + 0xC),
              (int)(int16_t)core->mem_r16(node + 0x2E), (int)(int16_t)core->mem_r16(node + 0x32), (int)(int16_t)core->mem_r16(node + 0x36),
              hits, core->mem_r32(node), core->mem_r32(node + 4), core->mem_r32(node + 8), core->mem_r32(node + 0xC));
    }
  }
  if (dolog)
    fprintf(stderr, "[objid] --- world prims=%d noded=%d labels=%d (n=%d) ---\n", nworld, nnoded, nlabel, n0);
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
  extern int g_fps60_on;
  if (g_fps60_on) { core->game->fps60.rq_capture(items, n); mark_consumed(); return; }
  if (!n) { mark_consumed(); return; }
  for (int i = 0; i < n; i++) gpu_emit_rq_item(core, &items[i]);
  mark_consumed();
}

RqItem* rq_push(Core* core)   { return core->game->rq.push(); }
void    rq_flush(Core* core)  { core->game->rq.flush(core); }
