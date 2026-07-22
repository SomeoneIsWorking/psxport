// Engine-owned render queue — see render_queue.h. Per-instance state lives on Game (game.h);
// the free rq_* API forwards to core->game->rq.
#include "render_queue.h"
#include "proj_params.h"   // class ProjParams — proj_camview_world_screen / camview_publish bridges
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include "gpu_vk.h"
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>

// Debug object-ID overlay: split into QUAD (billboard) and 3D-OBJECT (mesh) highlighting so the user can
// box only one class. On when its RmlUi/cfg toggle is set. `objid` channel + legacy debug_ids = both.
static inline int objid_quads_on(Core* c)   { const Mods& m = c->game->mods; return m.debug_quads   || m.debug_ids || cfg_dbg("objid"); }
static inline int objid_objects_on(Core* c) { const Mods& m = c->game->mods; return m.debug_objects || m.debug_ids || cfg_dbg("objid"); }
static inline int objid_on(Core* c) { return objid_quads_on(c) || objid_objects_on(c); }

int  gpu_vk_enabled(void);        // gpu_vk.cpp — Core*-less device-singleton query (declared at use)

// ---- Debug OBJECT-ID overlay (REPL `debug objid`) -------------------------------------------------
// Draw each rendered object's engine identity ON the object, in the live game, so the user can point at
// any object ("the flame at A3F2 flickers") and we share a stable name for it. The ID is the engine's own
// per-object key (RqItem::dbg_node) — the render-object node, for both meshes and billboards. This is a
// pure HOST overlay: it appends extra HUD quads to the render queue (so it flows through WHICHEVER present
// path is active — the inline emit OR the fps60 double-emit — with no separate draw path), and touches no
// guest RAM. Gated by `debug objid`; zero cost
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
bool RenderQueue::objidSolid(Core* core, const RqItem* ref, int x0, int y0, int x1, int y1,
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
void RenderQueue::objidChar(Core* core, const RqItem* ref, char ch, int x, int y, int s,
                            unsigned char r, unsigned char g, unsigned char b) {
  const unsigned char* gph = FONT5x7[font_idx(ch)];
  for (int row = 0; row < 7; row++)
    for (int col = 0; col < 5; col++)
      if (gph[row] & (1 << (4 - col))) {
        int px = x + col * s, py = y + row * s;
        objidSolid(core, ref, px, py, px + s, py + s, r, g, b);
      }
}
// Draw a string at (x,y) scale s with a dark backing box; glyphs in (r,g,b). Advance 6*s px per char.
void RenderQueue::objidStr(Core* core, const RqItem* ref, int x, int y, int s, const char* str,
                           unsigned char r, unsigned char g, unsigned char b) {
  int n = 0; for (const char* p = str; *p; p++) n++;
  if (!n) return;
  objidSolid(core, ref, x - s, y - s, x + n * 6 * s, y + 7 * s + s, 0, 0, 0);   // dark backing box
  int cx = x;
  for (const char* p = str; *p; p++) { objidChar(core, ref, *p, cx, y, s, r, g, b); cx += 6 * s; }
}

// Draw a 1px-scaled hollow rectangle outline (4 thin solid quads) in colour (r,g,b).
void RenderQueue::objidBox(Core* core, const RqItem* ref, int x0, int y0, int x1, int y1,
                           unsigned char r, unsigned char g, unsigned char b, int t) {
  objidSolid(core, ref, x0, y0, x1, y0 + t, r, g, b);   // top
  objidSolid(core, ref, x0, y1 - t, x1, y1, r, g, b);   // bottom
  objidSolid(core, ref, x0, y0, x0 + t, y1, r, g, b);   // left
  objidSolid(core, ref, x1 - t, y0, x1, y1, r, g, b);   // right
}

// Box + label every live GAME OBJECT, identified by ENUMERATING the render-node list (head 0x800F2624,
// next at node+0x24) — NOT by grouping emitted quads (which collapsed all objects into ONE box because
// quad->object attribution went through fragile packet-span correlation, and the user has no per-quad
// ownership). Each per-object node carries its real WORLD position at node+0x2E/0x32/0x36; we project that
// through the stable scene camera (proj_camview_world_screen) and draw a box + the object's id + WORLD
// coordinates in readable PC-native text. A node is classified a QUAD (2D sprite at a 3D position =
// billboard) vs a 3D-MESH object by its intrinsic render type (node+0xB, see below). The two classes have
// independent toggles (debug_quads / debug_objects) so the user can highlight ONLY quads.
// Pure host overlay (reads guest RAM, writes only the queue).
void RenderQueue::objidOverlay(Core* core) {
  RenderQueue* q = this;
  // class ProjParams free-function bridges (proj_camview_world_screen, camview_publish) — see proj_params.h.
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
  static int s_logframe = 0; int dolog = objid_on(core) && ((s_logframe++ % 120) == 0);
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
      // mesh.
      uint8_t rtype = core->mem_r8(n + 0xB);
      int quad = (rtype >= 0x10 && rtype <= 0x14) ? 1 : 0;
      if (quad ? !objid_quads_on(core) : !objid_objects_on(core)) continue;  // class toggled off
      float sx = 0, sy = 0;
      if (!proj_camview_world_screen((float)wx, (float)wy, (float)wz, &sx, &sy)) continue;  // behind camera
      int cx = (int)(sx + 0.5f), cy = (int)(sy + 0.5f);
      if (cx < -60 || cx > 420 || cy < -40 || cy > 280) continue;   // off-screen
      if (quad) nquad++; else nobj++;
      if (dolog && quad) cfg_logi("objid-q", "node=%08X rtype=0x%02X scr=(%d,%d) world=(%d,%d,%d) +0xC=%02X +0xD=%02X", n, rtype, (int)(sx+0.5f),(int)(sy+0.5f), wx,wy,wz, core->mem_r8(n+0xC), core->mem_r8(n+0xD));
      unsigned char br = quad ? 255 : 0, bg = 255, bb = quad ? 0 : 255;   // quads yellow, 3D objects cyan
      objidBox(core, ref, cx - 6, cy - 6, cx + 6, cy + 6, br, bg, bb, 1);
      char l1[16], l2[40];
      snprintf(l1, sizeof l1, "#%04X", (unsigned)(n & 0xFFFF));      // per-instance id (node handle)
      snprintf(l2, sizeof l2, "%d,%d,%d", wx, wy, wz);               // WORLD coordinates
      objidStr(core, ref, cx + 9, cy - 7, 1, l1, br, bg, bb);
      objidStr(core, ref, cx + 9, cy + 6, 1, l2, br, bg, bb);
    }
  }
  if (dolog) cfg_logi("objid", "=== %d live; %d quads + %d 3D boxed ===", nlive, nquad, nobj);
}

// The render queue is THE render path — one behavior, the PC game. No env gate (user directive
// 2026-06-20: "have only one behavior that is PC game"). The lone exception is the PSXPORT_SBS dual-channel
// debug COMPARE tool, which keeps its own inline path; callers check gpu_sbs_get() for that, not this.
int rq_active(void) { return 1; }

void RenderQueue::reset() { n = 0; seq = 0; consumed = 0; }

RqItem* RenderQueue::push() {
  if (consumed) reset();
  if (n >= RQ_MAX) {
    // FAIL-FAST (user 2026-06-30): never silently drop prims. RQ_MAX already covers the real worst-case
    // scene (the area-transition spike, ~43k — see render_queue.h); exceeding it means a submit path is
    // running away (e.g. a stuck render walk re-submitting the same scene every frame — the bug-1 / later-273
    // symptom). Abort with a C backtrace so that submit path is visible rather than hidden behind a drop.
    cfg_loge("rq", "\nFATAL: render queue full (%d items) — refusing to drop prims (fail-fast).\n  A submit path produced > %d prims this frame (runaway re-submission?). Backtrace:", RQ_MAX, RQ_MAX);
    void* bt[32]; int nbt = backtrace(bt, 32); backtrace_symbols_fd(bt, nbt, 2);
    fflush(stderr);
    abort();
  }
  RqItem* it = &items[n++];
  it->seq = seq++;
  return it;
}

void RenderQueue::mark_consumed() { if (n) consumed = 1; }

// Engine-decided order: layer low->high, submission order within a layer. stable_sort keeps the within-
// layer submission order exactly (matters for semi-transparent blending). The D32 depth buffer does fine-
// grained occlusion inside RQ_WORLD regardless. Kept as its own method so the fps60 mid-present (which
// builds a fresh queue by re-running sceneNative + re-appending non-scene prims) sorts identically.
// NOTE (coplanar z-fight follow-up): a secondary tie-break key can layer on top of the (layer,seq) sort
// here — keep this comparator the single sort authority so it has one place to extend.
void RenderQueue::sortQueue() {
  if (n) std::stable_sort(items, items + n, [](const RqItem& a, const RqItem& b) {
    return a.layer != b.layer ? a.layer < b.layer : a.seq < b.seq;
  });
}

void RenderQueue::emitQueue(Core* core) {
  if (!n) { mark_consumed(); return; }
  // `debug rqhist` (diag): per-frame histogram of what the queue actually emits, by layer × opaque/semi.
  if (cfg_dbg("rqhist")) {
    int c[4][2] = {{0,0},{0,0},{0,0},{0,0}};
    for (int i = 0; i < n; i++) { int L = items[i].layer & 3, sm = items[i].semi ? 1 : 0; c[L][sm]++; }
    static int lf = 0; if ((lf++ % 30) == 0)
      cfg_logf("rqhist", "n=%d  bg(op/semi)=%d/%d  WORLD=%d/%d  ovl=%d/%d  hud=%d/%d",
              n, c[0][0],c[0][1], c[1][0],c[1][1], c[2][0],c[2][1], c[3][0],c[3][1]);
  }
  for (int i = 0; i < n; i++) emitItem(core, &items[i]);
  mark_consumed();
}

// PSXPORT_ZFIGHT[=eps]: automatic z-fight FINDER. Software-rasterize every opaque RQ_OM_DEPTH world prim
// into a per-pixel top-2 interpolated-D32-depth buffer (exactly what the VK D32 buffer receives —
// ord3d(barycentric depth)). A z-fight pixel = the two frontmost coverers come from DIFFERENT prims yet
// their interpolated D32 values differ by < eps (default 6e-5, ~ the D32 ULP band near 1.0). Reports the
// count + the worst contesting prim pairs (dbg_node / color / D32 gap) and writes a heatmap PPM. Pure
// host diagnostic (no guest write, no effect on the real render). One shot per s_zfight_frame window.
void RenderQueue::zfightScan(Core* core) {
  static float eps = -1.f; static int scan_from = -1;
  if (eps < 0.f) { const char* e = cfg_str("PSXPORT_ZFIGHT"); if (!e) { eps = -2.f; }
                   else { // PSXPORT_ZFIGHT=1 (and the bare/empty form) is the repo-wide "enable" idiom, NOT
                          // an explicit eps=1.0 request — atof("1") is a legitimate (if absurd) eps value,
                          // so "1"/"" must be special-cased to the default rather than inferred from atof<=0.
                          eps = (!*e || !strcmp(e, "1")) ? 6e-5f : (float)atof(e);
                          if (eps <= 0.f) eps = 6e-5f;
                          const char* f = cfg_str("PSXPORT_ZFIGHT_FRAME"); scan_from = f ? atoi(f) : 0; } }
  if (eps < 0.f) return;   // -2 = disabled
  float gpu_zbias_unit();   // gpu_vk.cpp — the shipped paint-order bias unit (PSXPORT_ZBIAS), modeled here
  GpuState& s = core->game->gpu;
  if ((int)s.s_frame < scan_from) return;
  const int W = s.s_disp_w, H = s.s_disp_h, DX = s.s_disp_x, DY = s.s_disp_y;
  if (W <= 0 || H <= 0) return;
  // Optional PSXPORT_ZFIGHT_BOX="x0,y0,x1,y1" (display coords): restrict the contest report to a region.
  static int bx0=-1,by0=-1,bx1=1<<20,by1=1<<20,boxset=-1;
  if (boxset<0){ boxset=0; const char* bb=cfg_str("PSXPORT_ZFIGHT_BOX"); if(bb&&sscanf(bb,"%d,%d,%d,%d",&bx0,&by0,&bx1,&by1)==4) boxset=1; }
  // top-2 D32 per pixel + owning prim index
  std::vector<float> d1(W*H, -1.f), d2(W*H, -1.f);
  std::vector<int>   p1(W*H, -1),   p2(W*H, -1);
  auto edge=[](float ax,float ay,float x0,float y0,float x1,float y1){ return (x1-x0)*(ay-y0)-(y1-y0)*(ax-x0); };
  for (int idx = 0; idx < n; idx++) {
    const RqItem* it = &items[idx];
    if (it->semi || it->order_mode != RQ_OM_DEPTH || !it->depth) continue;
    int nv = it->nv ? it->nv : 4;
    const float* fx = it->has_xyf ? it->xsf : nullptr; const float* fy = it->has_xyf ? it->ysf : nullptr;
    for (int t = 0; t < (nv==4?2:1); t++) {   // tris: (0,1,2) and for a quad also (1,2,3)
      int i0=t, i1=t+1, i2=t+2;
      float X0=(fx?fx[i0]:(float)it->xs[i0])-DX, Y0=(fy?fy[i0]:(float)it->ys[i0])-DY;
      float X1=(fx?fx[i1]:(float)it->xs[i1])-DX, Y1=(fy?fy[i1]:(float)it->ys[i1])-DY;
      float X2=(fx?fx[i2]:(float)it->xs[i2])-DX, Y2=(fy?fy[i2]:(float)it->ys[i2])-DY;
      float den = (Y1-Y2)*(X0-X2) + (X2-X1)*(Y0-Y2);
      if (den == 0.f) continue;
      int bx0=(int)floorf(fminf(fminf(X0,X1),X2)), bx1=(int)ceilf(fmaxf(fmaxf(X0,X1),X2));
      int by0=(int)floorf(fminf(fminf(Y0,Y1),Y2)), by1=(int)ceilf(fmaxf(fmaxf(Y0,Y1),Y2));
      if (bx0<0)bx0=0; if(by0<0)by0=0; if(bx1>=W)bx1=W-1; if(by1>=H)by1=H-1;
      for (int y=by0; y<=by1; y++) for (int x=bx0; x<=bx1; x++) {
        float px=x+0.5f, py=y+0.5f;
        float l0=((Y1-Y2)*(px-X2)+(X2-X1)*(py-Y2))/den;
        float l1=((Y2-Y0)*(px-X2)+(X0-X2)*(py-Y2))/den;
        float l2=1.f-l0-l1;
        if (l0<-0.001f||l1<-0.001f||l2<-0.001f) continue;
        float ord=l0*it->depth[i0]+l1*it->depth[i1]+l2*it->depth[i2];
        float d32=0.0625f+ord*(0.9375f-0.0625f);
        int k=y*W+x;
        // >= not >: the rasterizer's depth test is GREATER_OR_EQUAL and prims are visited in paint order,
        // so on an exact tie the LATER prim owns the pixel. Modelling that with a strict > made the scan
        // report every exactly-tied pair as painted against submission order (the earlier prim held d1),
        // which is the opposite of what actually gets drawn.
        if (d32>=d1[k]) { d2[k]=d1[k]; p2[k]=p1[k]; d1[k]=d32; p1[k]=idx; }
        else if (d32>=d2[k]) { d2[k]=d32; p2[k]=idx; }
      }
    }
  }
  // Scan for contests: top-2 from different prims within eps.
  // Model the SHIPPED fix: with the paint-order bias (idx*U added to each prim's d32), the LATER-emitted prim
  // (max array idx = paint order) should win the GREATER_OR_EQUAL test uniformly. "paint-stable" = the winner
  // is the later prim => motion-invariant (no z-fight pop). Count raw (U=0) vs biased so one run shows the
  // fix converting depth-driven (unstable) contests into paint-order (stable) ones.
  const float Usw[4] = { 4e-7f, 1e-6f, 4e-6f, 1e-5f };   // U sweep for the stability report
  std::vector<unsigned char> heat(W*H*3, 0);
  int nfight=0, paint_stable_raw=0, ps_b[4]={0,0,0,0};
  int ntie=0, ptie_raw=0, ptie_b[4]={0,0,0,0};          // gap<1e-5 subset = the true (flickery) ties
  struct Pair{int a,b,cnt,inv; float gap;}; std::vector<Pair> pairs;
  for (int k=0;k<W*H;k++){
    if (p1[k]<0||p2[k]<0||p1[k]==p2[k]) continue;
    if (boxset==1){ int x=k%W, y=k/W; if(x<bx0||x>bx1||y<by0||y>by1) continue; }
    float gap=fabsf(d1[k]-d2[k]);
    if (gap<eps) {
      // paint-order stability of this contest, raw vs biased (later-emitted prim should win => motion-stable)
      int ia=p1[k], ib=p2[k]; float da=d1[k], db=d2[k];
      float d_later = ia>ib?da:db, d_earlier = ia>ib?db:da;
      int later_idx = ia>ib?ia:ib, earlier_idx = ia>ib?ib:ia;
      bool tie = gap < 1e-5f; if (tie) ntie++;
      if (d_later >= d_earlier) { paint_stable_raw++; if (tie) ptie_raw++; }
      for (int u=0;u<4;u++) if (d_later + later_idx*Usw[u] >= d_earlier + earlier_idx*Usw[u]) { ps_b[u]++; if(tie) ptie_b[u]++; }
      nfight++; heat[k*3]=255; heat[k*3+1]= (unsigned char)fminf(255.f, gap/eps*255.f);
      // BLUE = the contest the depth buffer resolves AGAINST submission order (the earlier-submitted prim
      // wins the pixel). Submission order IS the game's intra-bucket paint order, so these pixels are the
      // ones where the picture contradicts the guest's own ordering — the visible z-fight, as opposed to
      // the (far more numerous) harmless coincident-surface contests the paint order already settles.
      heat[k*3+2] = (d_later >= d_earlier) ? 0 : 255;
      int a=p1[k], b=p2[k]; if(a>b){int t2=a;a=b;b=t2;}
      int found=-1; for(size_t i=0;i<pairs.size();i++) if(pairs[i].a==a&&pairs[i].b==b){found=(int)i;break;}
      int inv = (d_later >= d_earlier) ? 0 : 1;
      if(found<0){ pairs.push_back({a,b,1,inv,gap}); } else { pairs[found].cnt++; pairs[found].inv+=inv; pairs[found].gap=fminf(pairs[found].gap,gap); }
    }
  }
  std::sort(pairs.begin(),pairs.end(),[](const Pair&a,const Pair&b){return a.inv!=b.inv ? a.inv>b.inv : a.cnt>b.cnt;});
  auto pc=[](int a,int b){ return b?100.f*a/b:0.f; };
  cfg_logi("zfight", "f%d eps=%.6g fight=%d ties(<1e-5)=%d | ALL paint-stable raw=%.0f%% U4e7=%.0f%% U1e6=%.0f%% U4e6=%.0f%% U1e5=%.0f%% | TIES raw=%.0f%% U4e7=%.0f%% U1e6=%.0f%% U4e6=%.0f%% U1e5=%.0f%%", s.s_frame, eps, nfight, ntie,
    pc(paint_stable_raw,nfight), pc(ps_b[0],nfight), pc(ps_b[1],nfight), pc(ps_b[2],nfight), pc(ps_b[3],nfight),
    pc(ptie_raw,ntie), pc(ptie_b[0],ntie), pc(ptie_b[1],ntie), pc(ptie_b[2],ntie), pc(ptie_b[3],ntie));
  auto vd=[](const RqItem&P,int i){ return P.depth?P.depth[i]:-1.f; };
  for (size_t i=0;i<pairs.size()&&i<10;i++){
    const RqItem&A=items[pairs[i].a]; const RqItem&B=items[pairs[i].b];
    int an=A.nv?A.nv:4, bn=B.nv?B.nv:4;
    cfg_logi("zfight", "  pair px=%d order-inverted=%d gap>=%.7f", pairs[i].cnt, pairs[i].inv, pairs[i].gap);
    cfg_logi("zfight", "    A node=%08X key=%d key_ord=%.6f col=(%d,%d,%d) seq=%u nv=%d xyf=%d vdepth=[%.6f %.6f %.6f %.6f] xy=[(%d,%d)(%d,%d)(%d,%d)(%d,%d)]", A.dbg_node,A.sort_key,(double)A.key_ord,A.rs[0],A.gs[0],A.bs[0],A.seq,an,A.has_xyf, vd(A,0),vd(A,1),vd(A,2),an==4?vd(A,3):-1.f,
      A.xs[0],A.ys[0],A.xs[1],A.ys[1],A.xs[2],A.ys[2],an==4?A.xs[3]:0,an==4?A.ys[3]:0);
    cfg_logi("zfight", "    B node=%08X key=%d key_ord=%.6f col=(%d,%d,%d) seq=%u nv=%d xyf=%d vdepth=[%.6f %.6f %.6f %.6f] xy=[(%d,%d)(%d,%d)(%d,%d)(%d,%d)]", B.dbg_node,B.sort_key,(double)B.key_ord,B.rs[0],B.gs[0],B.bs[0],B.seq,bn,B.has_xyf, vd(B,0),vd(B,1),vd(B,2),bn==4?vd(B,3):-1.f,
      B.xs[0],B.ys[0],B.xs[1],B.ys[1],B.xs[2],B.ys[2],bn==4?B.xs[3]:0,bn==4?B.ys[3]:0);
  }
  if (nfight>0) { char path[128]; snprintf(path,sizeof path,"scratch/screenshots/zfight/heat_f%d.ppm",s.s_frame);
    FILE* fp=fopen(path,"wb"); if(fp){ fprintf(fp,"P6\n%d %d\n255\n",W,H); fwrite(heat.data(),3,W*H,fp); fclose(fp);
      cfg_logi("zfight", "  heatmap -> %s", path); } }
}

// finalize — see the header. Game-sort-key order resolution (kanban #11) runs FIRST, on the complete
// queue, so the snapped depths reach every consumer of that queue identically; the sort follows.
void RenderQueue::finalize(Core* core) {
  resolveKeyOrder(core);
  sortQueue();
}

void RenderQueue::flush(Core* core) {
  // debug: label each object with its engine ID. Appended BEFORE finalize so the overlay's quads take
  // part in the same sort as everything else; resolveKeyOrder ignores them (HUD, no game sort key).
  if (n && objid_on(core)) objidOverlay(core);
  finalize(core);
  // zfightScan reads only the sorted item array (depth/xy/order_mode set at submission time) — it does not
  // depend on emitItem having run — so it belongs HERE, right after sortQueue, not inside emitQueue. This is
  // the one placement that scans the exact same (sorted, real-frame) item set under BOTH the fps60 capture
  // branch below and the plain emitQueue() path: fps60 double-buffers this REAL queue and later re-derives an
  // INTERPOLATED queue from it (Fps60::rq_capture / present_vk) — the interp frame is a lerp of two already-
  // scanned real frames, so it doesn't need (or want) its own scan. Previously this call lived at the tail of
  // emitQueue(), which the fps60 branch below skips entirely (it returns before emitQueue) — under the user's
  // real mods.fps60=1 config the instrument never ran at all.
  zfightScan(core);
  // fps60: the interpolated-60fps tier OWNS presentation — it double-buffers this sorted queue (Q[N]) and
  // presents ONE FRAME BEHIND (slot A = lerp(Q[N-1],Q[N]), slot B = Q[N] verbatim; Fps60::present_vk). So
  // it must HOLD the sorted queue rather than have flush emit it now. Only when this core actually presents
  // per-frame: under diff_mode (SBS dual-core compare) per-core present is suppressed, so present_vk never
  // runs — capturing would leave the geometry batch empty (black SBS panes). In diff_mode the SBS composite
  // reads the geometry batch directly, so flush MUST inline-emit. Gate the fps60 capture on !diff_mode.
  if (core->game->mods.fps60 && !core->game->diff_mode) { core->game->fps60.rq_capture(items, n); mark_consumed(); return; }
  emitQueue(core);
}

// ---- Native render-queue EMISSION (moved from gpu_native.cpp, 2026-07 restructure): the engine's OWN
// render-queue API (RqItem-based world/2D quad submission with real per-vertex depth + order_mode + shadow-
// cast tagging), as distinct from gpu_native.cpp's PSX GP0-packet interpreter/rasterizer. drawWorldQuad
// / push2dQuad are the entry points game/render (submit.cpp, native_terrain.cpp, mesh_draw.cpp)
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
int gpu_vk_enabled(void);   // gpu_vk.cpp — Core*-less device-singleton query (declared at use; see gpu_vk.h)
void RenderQueue::emitItem(Core* core, const RqItem* it) {
  if (!gpu_vk_enabled()) return;
  // preseqobj (per-object motion tracker, tools/preseqobj_check.py): when a `preseq` capture is armed AND
  // the `preseqobj` channel is on, log one line per emitted RqItem keyed to the present index this pass
  // will dump. present index >= 0 only while armed, so the cfg_dbg scan is skipped entirely otherwise —
  // zero cost in a normal run. The tracker groups by `key` (dbg_node = object identity; 0 = an
  // un-keyed 2D/HUD prim) and follows each object's screen x/y across consecutive presents to flag
  // sign-alternating (oscillation) or stall-step (snapping) motion. Both 60fps present passes
  // (interp + real) emit through here, so their prims are logged under their own present index —
  // this instrument doubles as the interp pass's quality debugger (docs/fps60-rework.md).
  { int pi = gpu_vk_preseq_present_index(core);
    if (pi >= 0 && cfg_dbg("preseqobj"))
      // scene=1 (== has_xyf): a float-projected display-pass world prim — rebuilt at the interp present
      // by tier1Render, correct-by-construction; the tracker counts but does not judge these (its NN/
      // emit-index identity is meaningless over dense mesh triangles). scene=0 = a guest-time drawable
      // (OT-walk billboard / #65 dual-emit / 2D / HUD) — the class the per-object gate actually verifies.
      cfg_logf("preseqobj", "p%04d key=%08X layer=%d x=%d y=%d scene=%d",
              pi, it->dbg_node, it->layer, it->xs[0], it->ys[0], it->has_xyf ? 1 : 0); }
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
  // gpu_vk_shadow_push_tri no-ops when shadows are off; verts are the B (un-interpolated) positions.
  if (it->sh_cast) {
    float v[4][3]; for (int k = 0; k < 4; k++) { v[k][0]=it->sh_vx[k]; v[k][1]=it->sh_vy[k]; v[k][2]=it->sh_vz[k]; }
    gpu_vk_shadow_push_tri(core, v[0], v[1], v[2]);
    if ((it->nv ? it->nv : 4) == 4) gpu_vk_shadow_push_tri(core, v[1], v[2], v[3]);
  }
  const int* xs = it->xs; const int* ys = it->ys; const int* us = it->us; const int* vs = it->vs;
  const unsigned char* rs = it->rs; const unsigned char* gs = it->gs; const unsigned char* bs = it->bs;
  const float* depth = it->depth; int mode = it->mode, raw = it->raw, nv = it->nv ? it->nv : 4;
  // PSXPORT_PRIMAT="x,y" (DISPLAY coords): also log WORLD/queue prims (drawWorldQuad etc.) that cover
  // that pixel — primat in gp0_exec is blind to these (they bypass the OT walk). Shows the real-depth
  // occluders. (diag, 2026-06-24)
  { static int qx=-2, qy=-1, qf0=0; if (qx==-2){ qx=-1; const char* pa=cfg_str("PSXPORT_PRIMAT"); if(pa) sscanf(pa,"%d,%d,%d",&qx,&qy,&qf0); }
    if (qx>=0 && (int)s.s_frame>=qf0) { int ax=s.s_disp_x+qx, ay=s.s_disp_y+qy;
      auto edge=[](int ax_,int ay_,int x0,int y0,int x1,int y1){ return (int64_t)(x1-x0)*(ay_-y0)-(int64_t)(y1-y0)*(ax_-x0); };
      auto intri=[&](int i0,int i1,int i2){ int64_t w0=edge(ax,ay,xs[i1],ys[i1],xs[i2],ys[i2]);
        int64_t w1=edge(ax,ay,xs[i2],ys[i2],xs[i0],ys[i0]); int64_t w2=edge(ax,ay,xs[i0],ys[i0],xs[i1],ys[i1]);
        return (w0>=0&&w1>=0&&w2>=0)||(w0<=0&&w1<=0&&w2<=0); };
      int t0 = intri(0,1,2) ? 0 : ((nv==4 && intri(1,2,3)) ? 1 : -1);
      if (t0 >= 0) { static int n=0; if(n++<6000) {
        // Interpolated depth at (ax,ay) = the value the D32 buffer actually receives, so a z-fight shows as
        // two prims with (near-)equal INTERPOLATED ord3d here. Barycentric on the float verts the rasterizer
        // uses (has_xyf) else the rounded xs/ys. ord3d(d)=NATIVE_3D_MIN+d*(NATIVE_3D_MAX-NATIVE_3D_MIN) for
        // RQ_OM_DEPTH; 2D-band prims store a screen-space band value (not depth) so print raw.
        const float* fx = it->has_xyf ? it->xsf : nullptr; const float* fy = it->has_xyf ? it->ysf : nullptr;
        int i0=t0, i1=t0+1, i2=t0+2;
        float ax0 = fx?fx[i0]:(float)xs[i0], ay0 = fy?fy[i0]:(float)ys[i0];
        float ax1 = fx?fx[i1]:(float)xs[i1], ay1 = fy?fy[i1]:(float)ys[i1];
        float ax2 = fx?fx[i2]:(float)xs[i2], ay2 = fy?fy[i2]:(float)ys[i2];
        float d0 = depth?depth[i0]:-1.f, d1 = depth?depth[i1]:-1.f, d2 = depth?depth[i2]:-1.f;
        float den = (ay1-ay2)*(ax0-ax2) + (ax2-ax1)*(ay0-ay2);
        float interp_ord = -1.f, d32 = -1.f;
        if (depth && den != 0.f) {
          float l0 = ((ay1-ay2)*(ax-ax2) + (ax2-ax1)*(ay-ay2)) / den;
          float l1 = ((ay2-ay0)*(ax-ax2) + (ax0-ax2)*(ay-ay2)) / den;
          float l2 = 1.f - l0 - l1;
          interp_ord = l0*d0 + l1*d1 + l2*d2;
          d32 = 0.0625f + interp_ord * (0.9375f - 0.0625f);   // ord3d
        }
        // Texture identity is part of "what am I looking at": a face that draws BLACK is either losing a
        // depth contest or sampling the wrong page/CLUT, and the two are indistinguishable without it.
        cfg_logi("primat-rq", "f%d seq=%u dbgnode=%08X layer=%d om=%d semi=%d tri=%d nv=%d vdepth=[%.6f %.6f %.6f] interp_ord=%.6f D32=%.6f col=(%d,%d,%d) raw=%d mode=%d tp=(%d,%d) clut=(%d,%d) uv=[(%d,%d) (%d,%d) (%d,%d) (%d,%d)] xy=[(%d,%d) (%d,%d) (%d,%d) (%d,%d)]",
          s.s_frame, it->seq, it->dbg_node, it->layer, it->order_mode, it->semi, t0, nv,
          d0, d1, d2, interp_ord, d32,
          rs[0],gs[0],bs[0], raw, mode, it->tp_x, it->tp_y, it->clut_x, it->clut_y,
          us[0],vs[0], us[1],vs[1], us[2],vs[2], us[3],vs[3],
          xs[0],ys[0], xs[1],ys[1], xs[2],ys[2], xs[3],ys[3]); } } } }
  unsigned ord = s.s_prim_order++;
  gpu_vk_set_order(core, ord);
  // Depth: 3D world prims carry real per-vertex view-Z (set_vd); 2D prims select the renderer's far/near
  // screen-space band (preserving the existing 2D depth semantics — only the ORDER is now engine-decided).
  int om = it->order_mode;
  if      (om == RQ_OM_2D_BG) gpu_vk_set_order_2d_bg(core, ord);
  else if (om == RQ_OM_2D_FG) gpu_vk_set_order_2d(core, ord);
  #define RQ_SETVD(p) do { if (om == RQ_OM_DEPTH) gpu_vk_set_vd(core, (p)); } while (0)
  // Vertex smoothing (#15): for the world path, hand the rasterizer the sub-pixel float screen XY. The base
  // pointer maps to vertex [0]; the second triangle of a quad is emitted from &xs[1], so it gets &xsf[1].
  // gpu_vk_set_order (inside set_order, fired per draw via the *_set_vd/order path) clears s_xf, so a NULL
  // here for non-world prims leaves them snapping to the integer xs/ys. set after set_order, before draw.
  const float* xsf = it->has_xyf ? it->xsf : nullptr;
  const float* ysf = it->has_xyf ? it->ysf : nullptr;
  #define RQ_SETXYF(o) do { gpu_vk_set_xyf(core, xsf ? xsf+(o) : nullptr, ysf ? ysf+(o) : nullptr); } while (0)
  if (it->semi) {
    int bx0=xs[0],by0=ys[0],bx1=xs[0],by1=ys[0];
    for (int i=1;i<nv;i++){ if(xs[i]<bx0)bx0=xs[i]; if(xs[i]>bx1)bx1=xs[i]; if(ys[i]<by0)by0=ys[i]; if(ys[i]>by1)by1=ys[i]; }
    gpu_vk_semi_group(core, bx0, by0, bx1, by1);
    RQ_SETVD(depth); RQ_SETXYF(0);
    gpu_vk_draw_semi(core, (int*)xs, (int*)ys, (int*)us, (int*)vs, (unsigned char*)rs, (unsigned char*)gs, (unsigned char*)bs,
                     it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                     it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1, it->tp_blend);
    if (nv == 4) { RQ_SETVD(&depth[1]); RQ_SETXYF(1);
      gpu_vk_draw_semi(core, (int*)&xs[1], (int*)&ys[1], (int*)&us[1], (int*)&vs[1], (unsigned char*)&rs[1], (unsigned char*)&gs[1], (unsigned char*)&bs[1],
                       it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                       it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1, it->tp_blend); }
  } else {
    RQ_SETVD(depth); RQ_SETXYF(0);
    gpu_vk_draw_tritri(core, (int*)xs, (int*)ys, (int*)us, (int*)vs, (unsigned char*)rs, (unsigned char*)gs, (unsigned char*)bs,
                       it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                       it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1);
    if (nv == 4) { RQ_SETVD(&depth[1]); RQ_SETXYF(1);
      gpu_vk_draw_tritri(core, (int*)&xs[1], (int*)&ys[1], (int*)&us[1], (int*)&vs[1], (unsigned char*)&rs[1], (unsigned char*)&gs[1], (unsigned char*)&bs[1],
                         it->tp_x, it->tp_y, mode, raw, it->clut_x, it->clut_y,
                         it->tw_mx, it->tw_my, it->tw_ox, it->tw_oy, it->da_x0, it->da_y0, it->da_x1, it->da_y1); }
  }
  gpu_vk_set_xyf(core, nullptr, nullptr);   // clear so the next prim (if not world) snaps to integer xs/ys
  #undef RQ_SETVD
  #undef RQ_SETXYF
}

// Build an RqItem from already-resolved quad/tri data + material snapshot, then either queue it (engine
// owns the order, flushed at the draw kick) or emit it now. The ONE place the three submit paths (world
// quad, guest poly, guest sprite) funnel through. `capture` routes to the queue (set during the OT walk
// under PSXPORT_RQ); otherwise it draws inline immediately (default — identical to pre-queue behavior).
// Not static: gpu_native.cpp's guest GP0/OT-walk poly and sprite submit paths (gp0_exec) also funnel their
// queued items through this same one place via their own local extern forward declaration.
void RenderQueue::emitOrQueue(Core* core, int capture, int layer, int order_mode, int nv, int semi, int raw,
                              const int* xs, const int* ys, const float* xsf, const float* ysf,
                              const int* us, const int* vs,
                              const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                              const float* depth, int mode, int tp_x, int tp_y, int clut_x, int clut_y,
                              int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1,
                              int tp_blend, const float (*sv)[3], int sort_key, float key_ord) {
  // ---- WIDESCREEN 2D layout — the ONE layout authority for NATIVE screen-space producers (USER
  // 2026-07-16: dialog/prompt panels sat left-anchored in wide). Native UI producers (Panel, Font/
  // dialog text, gauges, menus) push 4:3 coords (x∈[0,320)); the wide FB spans [0,ww) with the world
  // centered at ww/2, so un-shifted 2D hugs the left edge. Rule (mirrors gpu_native's ws_2d_local_x):
  //   HUD/overlay -> +margin (centered, native size — matches 4:3 exactly, just registered with the
  //                  centered world);
  //   RQ_BACKGROUND (dbg_node==0 full-screen art/fades) -> STRETCH to fill [0,ww).
  // EXEMPT (already wide-final coords): backdropRender's REAL wide tiles (kBackdropDbgNode via the
  // diag scope). 3D (RQ_OM_DEPTH) never enters. 4:3: margin==0, no-op.
  int wxs[4]; float wxsf[4];
  { int gpu_vk_wide_engine(Core*), gpu_vk_wide_engine_w(Core*);
    if (order_mode != RQ_OM_DEPTH && gpu_vk_wide_engine(core) &&
        core->rsub.diag.currentNode() != kBackdropDbgNode) {
      const int ww = gpu_vk_wide_engine_w(core), margin = (ww - 320) / 2;
      if (margin > 0) {
        // RQ_BACKGROUND art on widescreen: only a UNIFORM SOLID FILL — flat vertex colour AND
        // untextured (all-zero UVs) — STRETCHES to fill the wide FB (stretching a flat colour is
        // uniform, and this is what backs the pillarbox bars). ANYTHING with visible content — a
        // GRADIENT (non-flat colour) or a TEXTURED backdrop (title art, menu image; flat colour but
        // real UVs) — must NOT stretch (that spreads/squishes it), so it gets the same centering
        // +margin as every other 2D layer, pillarboxed on the flat wide fill.
        const bool flat = rs && gs && bs &&
                          rs[0]==rs[1] && rs[1]==rs[2] && rs[2]==rs[3] &&
                          gs[0]==gs[1] && gs[1]==gs[2] && gs[2]==gs[3] &&
                          bs[0]==bs[1] && bs[1]==bs[2] && bs[2]==bs[3];
        const bool untextured = (!us || (us[0]==0 && us[1]==0 && us[2]==0 && us[3]==0)) &&
                                (!vs || (vs[0]==0 && vs[1]==0 && vs[2]==0 && vs[3]==0));
        const bool stretch = (layer == RQ_BACKGROUND) && flat && untextured;
        for (int i = 0; i < nv; i++) {
          wxs[i] = stretch ? xs[i] * ww / 320 : xs[i] + margin;
          if (xsf) wxsf[i] = stretch ? xsf[i] * (float)ww / 320.0f : xsf[i] + (float)margin;
        }
        xs = wxs;
        if (xsf) xsf = wxsf;
      }
    } }
  RqItem it;
  it.layer = (uint8_t)layer; it.semi = semi ? 1 : 0; it.nv = (uint8_t)nv; it.raw = raw ? 1 : 0;
  it.order_mode = (uint8_t)order_mode;
  // objid overlay: stamp the entity node the native render walk is currently rendering (submit.cpp).
  // Every world prim an object emits gets its node, so the overlay labels ALL rendered objects. Terrain/
  // static/background prims render with no per-object scope (mDbgRenderNode==0) → correctly unlabeled.
  // RQ_BACKGROUND also carries currentNode() (#54): Render::backdropRender scopes itself with
  // kBackdropDbgNode (render_queue.h) the same way world producers do, so Fps60::isTier1Owned can key on
  // ITS prims specifically. Any RQ_BACKGROUND item from OUTSIDE that scope (the generic guest-OT-walk bg
  // classification in gpu_native.cpp — no beginObject wraps it) still gets dbg_node==0, unchanged.
  it.dbg_node = (layer == RQ_WORLD || layer == RQ_BACKGROUND) ? core->rsub.diag.currentNode() : 0;
  it.sort_key = sort_key; it.key_ord = key_ord;   // game's own OT sort key (kanban #11) — -1 = none
  // Shadow capture: an opaque world prim with view-space verts casts into the shadow map. Carried on the
  // item so emitItem re-pushes it to the shadow VBO on EVERY emit (= on both 60fps present passes).
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
  if (capture) { RqItem* slot = push(); if (slot) { uint32_t sq = slot->seq; *slot = it; slot->seq = sq; } }
  else         emitItem(core, &it);
}

// GAME-SORT-KEY ORDER RESOLUTION (kanban #11) — the waterpump barrel's black top face, done from the
// game's own data instead of a bias ramp.
//
// The barrel draws its top opening as a dark interior cap and, over it, the water surface. Measured on
// the psx_render leg (REPL `otwhere`, 2026-07-22): the game's own submitter files the cap in OT bucket
// 460 and the water in bucket 457, and the OT is walked descending — the game DECLARES the water in
// front, categorically. pc_render's interpolated per-vertex depth says the opposite at the contested
// pixels (the two surfaces genuinely cross), so the depth buffer paints the cap. Neither depth precision
// nor any epsilon can recover this: it is authored ORDER, not geometry.
//
// The rule: every keyed face carries the sort key the GAME computed for it (RqItem::sort_key, recomputed
// natively in submit.cpp from the RE'd emitter bodies). Within one object, if a face pair's interpolated
// depth could CONTRADICT the game's key order — the farther-keyed face able to win a pixel both cover —
// both faces' test depth is snapped to their key's shared ord (RqItem::key_ord, a pure function of the
// key). Snapped faces then resolve exactly as the game ordered them; equal keys snap to the SAME value
// and GREATER_OR_EQUAL + submission order resolves the tie. Everything else — faces whose real depth
// already agrees with the key order, terrain, other objects — keeps untouched per-vertex depth. Zero
// bias, zero span budget, zero tuned constant.
//
// The contradiction test is per-PAIR and interior-only: a sample point must lie strictly inside BOTH
// faces' polygons with the farther-keyed face interpolating nearer. Mesh-adjacent faces (shared edge,
// keys 1 apart) never trigger — their interiors are disjoint, they only touch along the edge — so an
// ordinary mesh is left entirely on real per-vertex depth. This is what makes it a discriminator rather
// than the reverted rank ramp, which re-ordered every face of every object by view-independent storage
// order (measured net-negative: 95/6 in the barrel but 173/311 of unintended winner flips elsewhere).

// Interpolated ord of item `it` at screen point (x,y), using the same triangle split + barycentric the
// rasterizer applies (tri 0 = verts 0,1,2; tri 1 = verts 1,2,3). Returns false when outside both tris.
static bool rq_ord_at(const RqItem* it, float x, float y, float* out) {
  int nv = it->nv ? it->nv : 4;
  const float* fx = it->has_xyf ? it->xsf : nullptr;
  const float* fy = it->has_xyf ? it->ysf : nullptr;
  for (int t = 0; t < (nv == 4 ? 2 : 1); t++) {
    int i0 = t, i1 = t + 1, i2 = t + 2;
    float x0 = fx ? fx[i0] : (float)it->xs[i0], y0 = fy ? fy[i0] : (float)it->ys[i0];
    float x1 = fx ? fx[i1] : (float)it->xs[i1], y1 = fy ? fy[i1] : (float)it->ys[i1];
    float x2 = fx ? fx[i2] : (float)it->xs[i2], y2 = fy ? fy[i2] : (float)it->ys[i2];
    float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (den == 0.f) continue;
    float l0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / den;
    float l1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / den;
    float l2 = 1.f - l0 - l1;
    if (l0 < 0.f || l1 < 0.f || l2 < 0.f) continue;   // outside this triangle (strict interior sampling)
    *out = l0 * it->depth[i0] + l1 * it->depth[i1] + l2 * it->depth[i2];
    return true;
  }
  return false;
}

// Do two faces occupy the IDENTICAL polygon — same vertex count, and every vertex of one matching a
// vertex of the other in screen position AND depth? Exact comparison on the values the rasterizer
// actually consumes (float XY when the producer supplied sub-pixel XY, integer XY otherwise), so this
// is a structural test, not a proximity test: a decal filed on the face it decorates matches, ordinary
// neighbouring or overlapping geometry does not. Vertex ORDER is deliberately ignored — a rotated
// listing of the same corners is precisely the case that defeats the depth buffer (see the caller).
static bool rq_faces_coincident(const RqItem& A, const RqItem& B) {
  const int nv = A.nv ? A.nv : 4;
  if ((B.nv ? B.nv : 4) != nv) return false;
  auto vx = [](const RqItem& P, int i) { return P.has_xyf ? P.xsf[i] : (float)P.xs[i]; };
  auto vy = [](const RqItem& P, int i) { return P.has_xyf ? P.ysf[i] : (float)P.ys[i]; };
  if (A.has_xyf != B.has_xyf) return false;
  bool used[4] = { false, false, false, false };
  for (int i = 0; i < nv; i++) {
    int m = -1;
    for (int j = 0; j < nv && m < 0; j++)
      if (!used[j] && vx(A, i) == vx(B, j) && vy(A, i) == vy(B, j) && A.depth[i] == B.depth[j]) m = j;
    if (m < 0) return false;
    used[m] = true;
  }
  return true;
}

void RenderQueue::resolveKeyOrder(Core* core) {
  (void)core;
  // Gather this frame's keyed world faces, grouped by object. Real guest nodes only — the reserved
  // sentinels (terrain/scene-table/backdrop) and unscoped prims carry no game sort key anyway.
  struct KF { int idx; uint32_t node; float bx0, by0, bx1, by1, dmin, dmax; };
  static thread_local std::vector<KF> kf;   // scratch, reused across frames
  kf.clear();
  for (int i = 0; i < n; i++) {
    const RqItem& it = items[i];
    if (it.layer != RQ_WORLD || it.order_mode != RQ_OM_DEPTH || it.sort_key < 0) continue;
    if (it.dbg_node < 0x80000000u || it.dbg_node >= 0x80200000u) continue;
    int nv = it.nv ? it.nv : 4;
    KF f; f.idx = i; f.node = it.dbg_node;
    f.bx0 = f.bx1 = it.has_xyf ? it.xsf[0] : (float)it.xs[0];
    f.by0 = f.by1 = it.has_xyf ? it.ysf[0] : (float)it.ys[0];
    f.dmin = f.dmax = it.depth[0];
    for (int k = 1; k < nv; k++) {
      float x = it.has_xyf ? it.xsf[k] : (float)it.xs[k], y = it.has_xyf ? it.ysf[k] : (float)it.ys[k];
      if (x < f.bx0) f.bx0 = x; if (x > f.bx1) f.bx1 = x;
      if (y < f.by0) f.by0 = y; if (y > f.by1) f.by1 = y;
      if (it.depth[k] < f.dmin) f.dmin = it.depth[k];
      if (it.depth[k] > f.dmax) f.dmax = it.depth[k];
    }
    kf.push_back(f);
  }
  if (kf.size() < 2) return;
  std::stable_sort(kf.begin(), kf.end(), [](const KF& a, const KF& b) { return a.node < b.node; });
  static thread_local std::vector<uint8_t> snap;   // parallel to kf: face must snap to its key_ord
  snap.assign(kf.size(), 0);
  for (size_t g0 = 0; g0 < kf.size(); ) {
    size_t g1 = g0 + 1;
    while (g1 < kf.size() && kf[g1].node == kf[g0].node) g1++;
    for (size_t a = g0; a < g1; a++) {
      for (size_t b = a + 1; b < g1; b++) {
        const RqItem& A = items[kf[a].idx];
        const RqItem& B = items[kf[b].idx];
        if (A.sort_key == B.sort_key) {
          // SAME OT BUCKET (kanban #29 — the hut-interior wall decals). The key says nothing about which
          // face is in front: the guest resolves a bucket internally by LINK order, which our submission
          // order (RqItem::seq, the array order after sortQueue) reproduces, and GREATER_OR_EQUAL already
          // hands the pixel to the later face wherever the two interpolate equal. Real depth is therefore
          // the status quo here — EXCEPT for one case the depth buffer cannot get right at all: a face
          // pair that is EXACTLY COINCIDENT (a decal quad filed on the wall quad it decorates, same four
          // projected corners, same four depths) but listed with ROTATED vertex order. The quad
          // triangulation is fixed at (0,1,2)+(1,2,3), so a rotation splits the two faces on OPPOSITE
          // diagonals; their interiors then interpolate differently and the EARLIER face wins the half
          // where its diagonal runs nearer. Measured in the hut interior (f1200, node 800FD850): the top
          // contesting pair is key 408 vs key 408, identical corners, 115 of 541 pixels painted against
          // submission order. Coincident faces carry no depth information to preserve, so both snap to
          // their key's shared ord and submission order — the guest's own intra-bucket order — decides
          // every pixel. Coincidence is tested EXACTLY (identical vertex multiset, position AND depth):
          // no epsilon, and ordinary geometry can never satisfy it.
          if (rq_faces_coincident(A, B)) { snap[a] = 1; snap[b] = 1; }
          continue;
        }
        // near = the face the game files NEARER (smaller OT index); far = the other.
        const KF& nf = A.sort_key < B.sort_key ? kf[a] : kf[b];
        const KF& ff = A.sort_key < B.sort_key ? kf[b] : kf[a];
        const RqItem& NI = items[nf.idx];
        const RqItem& FI = items[ff.idx];
        // Cheap rejects: no screen overlap, or the far face can never out-depth the near one (ord:
        // larger = nearer, so contradiction requires far.dmax > near.dmin).
        float ox0 = nf.bx0 > ff.bx0 ? nf.bx0 : ff.bx0, ox1 = nf.bx1 < ff.bx1 ? nf.bx1 : ff.bx1;
        float oy0 = nf.by0 > ff.by0 ? nf.by0 : ff.by0, oy1 = nf.by1 < ff.by1 ? nf.by1 : ff.by1;
        if (ox0 >= ox1 || oy0 >= oy1) continue;
        if (ff.dmax <= nf.dmin) continue;
        // Interior contest: sample a grid over the bbox intersection; a point strictly inside BOTH
        // polygons where the farther-keyed face interpolates nearer = the depth buffer would invert
        // the game's order there. Grid density: pixel-ish steps, capped — misses only sub-sample
        // slivers (documented residual), and mesh-adjacent faces (interiors disjoint) never hit.
        const int GN = 8;
        float sx = (ox1 - ox0) / (GN + 1), sy = (oy1 - oy0) / (GN + 1);
        bool contradict = false;
        for (int iy = 1; iy <= GN && !contradict; iy++) {
          for (int ix = 1; ix <= GN && !contradict; ix++) {
            float px = ox0 + sx * ix, py = oy0 + sy * iy, dn, df;
            if (!rq_ord_at(&NI, px, py, &dn)) continue;
            if (!rq_ord_at(&FI, px, py, &df)) continue;
            if (df > dn) contradict = true;
          }
        }
        if (contradict) { snap[a] = 1; snap[b] = 1; }
      }
    }
    g0 = g1;
  }
  int nsnap = 0;
  for (size_t i = 0; i < kf.size(); i++) {
    if (!snap[i]) continue;
    RqItem& it = items[kf[i].idx];
    for (int k = 0; k < 4; k++) it.depth[k] = it.key_ord;
    nsnap++;
    if (cfg_dbg("keyord"))
      cfg_logf("keyord", "f%u snap seq=%u node=%08X key=%d key_ord=%.6f bbox=(%.0f,%.0f)-(%.0f,%.0f)",
               core->game->gpu.s_frame, it.seq, it.dbg_node, it.sort_key, (double)it.key_ord,
               (double)kf[i].bx0, (double)kf[i].by0, (double)kf[i].bx1, (double)kf[i].by1);
  }
  if (nsnap && cfg_dbg("keyord"))
    cfg_logf("keyord", "f%u resolveKeyOrder: %d/%zu keyed faces snapped", core->game->gpu.s_frame, nsnap, kf.size());
}

// sv (optional, NULL = no shadow): the prim's 4 VIEW-SPACE verts (x=vx, y=vy, z=pz) for the shadow map.
// When non-NULL and opaque, the queued item carries them and emitItem re-pushes them as two tris
// to the shadow VBO on every emit (= on both 60fps present passes — see render_queue.h sh_cast).
// g_dbg_world_quads retired 2026-07-03 — Render::stats.dbgWorldQuads (RenderStats).
void RenderQueue::drawWorldQuad(Core* core, const float* px, const float* py, const float* depth,
                                const int* u, const int* v, const unsigned char* r, const unsigned char* g,
                                const unsigned char* b, uint16_t tp, uint16_t clut, int semi,
                                const float (*sv)[3], int sort_key, float key_ord) {
  if (!gpu_vk_enabled()) return;
  core->rsub.stats.dbgWorldQuads++;   // PSXPORT_GPU_TRACE: world quads this frame (SBS diag)
  if (cfg_dbg("silbbox")) { static int once=0; if (!once++) cfg_logf("silbbox", "s_off=(%d,%d)", core->game->gpu.s_off_x, core->game->gpu.s_off_y); }
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
  // sort_key/key_ord ride on the item; resolveKeyOrder (flush time) is the ONE consumer.
  const float (*cast)[3] = (sv && !semi) ? sv : nullptr;
  emitOrQueue(core, 1, RQ_WORLD, RQ_OM_DEPTH, 4, semi ? 1 : 0, 0,
                   xs, ys, xsf, ysf, us, vs, rs, gs, bs, depth, s.s_tp_mode,
                   s.s_tp_x, s.s_tp_y, s.s_clut_x, s.s_clut_y, s.s_tw_mx, s.s_tw_my, s.s_tw_ox, s.s_tw_oy,
                   s.s_da_x0, s.s_da_y0, s.s_da_x1, s.s_da_y1, s.s_tp_blend, cast, sort_key, key_ord);
}

// 2D quad enqueue (HUD / overlay / background) — funnels through emitOrQueue so a 2D drawable is a
// queued RqItem (part of THE FRAME), not a direct gpu_vk_draw_tritri that lands on only one 60fps pass.
void RenderQueue::push2dQuad(int layer, int order_2d_fg,
                             const int* xs, const int* ys, const int* us, const int* vs,
                             const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                             int tp_x, int tp_y, int mode, int raw, int clut_x, int clut_y,
                             int tw_mx, int tw_my, int tw_ox, int tw_oy, int da_x0, int da_y0, int da_x1, int da_y1,
                             int semi) {
  if (!gpu_vk_enabled()) return;
  Core* core = &game->core;
  int om = order_2d_fg ? RQ_OM_2D_FG : RQ_OM_2D_BG;
  emitOrQueue(core, 1, layer, om, 4, semi, raw,
                   xs, ys, nullptr, nullptr, us, vs, rs, gs, bs, nullptr, mode,
                   tp_x, tp_y, clut_x, clut_y, tw_mx, tw_my, tw_ox, tw_oy,
                   da_x0, da_y0, da_x1, da_y1, 0, nullptr);
}
