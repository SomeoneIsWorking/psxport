// NATIVE render-list WALK subsystem (Tomba2Engine) — the engine's PC-native SCENE RENDERER.
//
// Driven from the game's WORLD DATA (entity lists -> per-object geomblk node+0xC0 cmds), NOT from PSX GP0
// packets: ov_scene_native orchestrates the per-frame field render, and the master / snapshot / aux list
// walks (native twins of gen_func_8003C048 / 8003BB50 / the BCF4/BF00/EEC0 aux walks) iterate the engine's
// render lists, dispatching each live node by its render TYPE and tagging every prim with the object's
// PC-native world-position depth so 2D billboards occlude for real at the deferred OT walk. The per-object
// flush composes the float camera x object transform and submits generic GT3/GT4 natively (no PSX per-mode
// dispatcher), and the native backdrop draws the sky/parallax tilemap as RQ_BACKGROUND behind the world.
//
// Split out of engine_submit.cpp (the geometry-SUBMIT subsystem) so the scene renderer is its own PC-game
// file. Shared helpers (PktSpanSession, obj_world_ord/cur_render_node, native_gt3gt4) live in render_internal.h.
#include "core.h"
#include "render.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include "render_queue.h"
#include "engine_project.h"   // EObjXform + eproj_* (per-object world-coord float projection)
#include "render_internal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);
int  rec_addr_has_entry(Core*, uint32_t);   // overlay_router.cpp — is fn a real entry in the resident module?
void ov_terrain(Core* c);
#define OTBASE_PTR   0x800ED8C8u             // *this = the active ordering-table base
#define SCR          0x1F800000u             // PSX scratchpad base (the engine's GTE-compose temp area)

// The per-object render path is FULLY native (no PSX fallback): submit_perobj_flush composes the float
// world transform and calls native_gt3gt4 directly. The old gen_func_8003F698 per-mode dispatcher (which
// ran interpreted per-scene submitter variants for non-generic modes) is no longer consulted — every
// per-object geomblk is submitted as generic GT3/GT4 through the native, world-coord projection.

static void submit_perobj_flush(Core* c) {
  uint32_t node = c->r[4];
  if (c->mem_r8(node + 8) == 0) return;
  if (c->mem_r8(node + 9) == 0) return;
  uint32_t otbase_ptr = c->mem_r32(OTBASE_PTR);              // *0x800ED8C8
  int i = 0;
  while (i < (int)c->mem_r8(node + 8)) {
    uint32_t cmd = c->mem_r32(node + 0xC0 + i * 4);
    uint32_t geomblk = c->mem_r32(cmd + 0x40);
    if (geomblk != 0) {
      // PC-NATIVE: compose the camera × object transform in FLOAT from the object's REAL WORLD coordinates
      // (its world matrix cmd+0x18 + world position cmd+0x2c, transformed by the scene camera) and make it
      // the ACTIVE projection. The submitters project every vertex through it — NO gte_op, NO CR0-7.
      EObjXform w; eproj_compose_object(c, cmd, &w);
      eproj_set_active(&w);
      // OT base: node[0xd]&0xf == 4 selects a per-command sub-bucket (cmd[0x3f]*4 offset), else the base.
      uint32_t otbase = otbase_ptr;
      if ((c->mem_r8(node + 0xD) & 0xF) == 4)
        otbase = otbase_ptr + (((int32_t)(int8_t)c->mem_r8(cmd + 0x3F)) << 2);
      c->game->fps60.fps_cur_key = cmd;                 // fps60: tag this actor's world quads for reprojection
      native_gt3gt4(c, geomblk, otbase);                // fully-native generic GT3/GT4 submit (no PSX fallback)
      c->game->fps60.fps_cur_key = 0;
      eproj_clear_active();
    }
    i++;
    if (i >= (int)c->mem_r8(node + 9)) break;
  }
}

void ov_perobj_flush(Core* c) {
  submit_perobj_flush(c);
}

// ===================================================================================================
// ONE NATIVE RENDER PATH — world-data-driven scene render (Phase 1, user 2026-06-24 architecture:
// [[one-native-render-path-decoupled]]). Driven from the GAME's WORLD DATA, NOT from PSX GP0 packets:
// walk the 3 active entity lists, and render each live object's 3D model (geomblk via node+0xC0 cmds)
// through the native float-projection submitters (eproj + D32 depth + engine lighting). This is the
// single mechanism depth/60fps/ires/lighting attach to. It runs as its OWN pass (not bolted onto the
// PSX OT-walk) so the draw state is the native pass's. Gated `debug scenenative` while standing it up.
extern "C" int g_scene_native_diag;  int g_scene_native_diag = 0;   // counters for the bring-up probe
extern "C" long g_sn_objs, g_sn_cmds; long g_sn_objs = 0, g_sn_cmds = 0;
void ov_render_walk(Core* c);
void ov_rwalk_aux_bf00(Core* c), ov_rwalk_aux_eec0(Core* c), ov_rwalk_b588(Core* c),
     ov_render_walk_snapshot(Core* c), ov_rwalk_aux_bcf4(Core* c);
// NATIVE BACKDROP tilemap drawer — overlay FUN_80115598 (the seaside field's state-0 background drawer,
// reached via 0x8003df04's 16-state jump table @0x80014fc0; state 0 → 0x8003df74 → 0x80115598). This is the
// sky + distant parallax hills (the only thing the decoupled native scene was missing — verified by SKIPPASS
// attribution, later-244). The PSX body reads the area's tile MAP (W×H grid of u16 tile entries) and a
// scroll position, then builds GP0 textured-sprite (cmd 0x7d) packets into the OT for the visible wraparound
// window of 16×16 tiles. We TRANSCRIBE the integer wrap/scroll/index math (that's scene data — what tile
// goes where), but emit NATIVE RQ_BACKGROUND 2D quads instead of GP0 packets / OT links (the engine owns the
// background layer; sky/hills sit behind the real-depth world). Struct @t4 (=0x800ed018 at the seaside):
//   +0x04 hword tpage  +0x06 hword clut-base  +0x10 byte W  +0x11 byte H
//   +0x14 word  tilemap ptr (u16[H][W])       +0x28 hword scrollX  +0x2a hword scrollY
// Tile entry bits: [0:3]=atlas col, [4:7]=atlas row, [8:11]=clut sub-index. U=(t&0xF)<<4, V=(t&0xF0)+8 (the
// half-tile V bias is in the source data layout — faithful), clut=clutbase+((t&0xF00)>>2). Texpage 0x0E =
// 4bpp @ VRAM(896,0) (set once via a GP0(0xE1) prim in the PSX body; applied per-quad here).
static void ov_bg_tilemap_native(Core* c, uint32_t t4) {
  int W = c->mem_r8(t4 + 0x10), H = c->mem_r8(t4 + 0x11);
  if (W == 0 || H == 0) return;
  int rowstride = W * 2;                          // s0 — bytes per map row
  int mapbytes  = rowstride * H;                  // s3 — total map bytes (wrap modulus)
  int scrollX = (int16_t)c->mem_r16(t4 + 0x28);
  int scrollY = (int16_t)c->mem_r16(t4 + 0x2a);
  uint32_t map      = c->mem_r32(t4 + 0x14);
  uint16_t tpage    = c->mem_r16(t4 + 0x04);
  uint16_t clutbase = c->mem_r16(t4 + 0x06);
  int tp_x = (tpage & 0xF) * 64, tp_y = ((tpage >> 4) & 1) * 256;
  int mode = (tpage >> 7) & 3; if (mode > 2) mode = 2;
  // Publish THIS frame's backdrop texpage so the OT walk can recognize the GUEST background drawer's
  // redundant sky/sea tiles (FUN_80115598) and drop them on the field: the native backdrop (this fn) already
  // owns the sky/sea as RQ_BACKGROUND. Replaces the dead ov_bg_tilemap packet-span provenance (orphaned by
  // the override-system removal 2026-06-22), which is why the redundant tiles regressed to RQ_HUD and
  // occluded the world (render.md OPEN #1). Data-derived (real backdrop tpage), not a magic constant.
  void gpu_bg_texpage_set(Core*, int, int); gpu_bg_texpage_set(c, tp_x, tp_y);
  // Starting tile row/col = (scroll - screen-center) >> 4, wrapped into [0,H) / [0,W).
  int rowtile = ((scrollY - 120) >> 4) % H; if (rowtile < 0) rowtile += H;
  int coltile = ((scrollX - 160) >> 4) % W; if (coltile < 0) coltile += W;
  int t2 = rowtile * rowstride;                   // current row byte offset (wraps mod mapbytes)
  int coloff0 = coltile * 2;                       // starting col byte offset (wraps mod rowstride)
  int xoff = (int16_t)(152 - scrollX);             // t9 — sub-tile X scroll remainder + screen offset
  int yoff = (int16_t)(112 - scrollY);             // s7 — sub-tile Y scroll remainder + screen offset
  unsigned char col[4] = { 0x80, 0x80, 0x80, 0x80 };  // 0x7d is raw-texture: color ignored
  int outer_bound = (int16_t)(scrollY - 120) + 0x100; // 16 rows
  int t5 = (int16_t)(scrollX - 160) + 0x160;          // ~22 cols
  for (int t8 = scrollY - 120;;) {
    int Y = (int16_t)((t8 & 0xFFF0) + yoff);
    int t6 = (int16_t)t2;                          // row byte offset (sign-extended)
    int t0 = coloff0;
    for (int t1 = scrollX - 160;;) {
      int X = (int16_t)((t1 & 0xFFF0) + xoff);
      uint16_t tile = c->mem_r16(map + (uint32_t)(t6 + t0));
      int u = (tile & 0xF) << 4, v = (tile & 0xF0) + 8;
      uint16_t clut = (uint16_t)(clutbase + ((tile & 0xF00) >> 2));
      int clut_x = (clut & 0x3F) * 16, clut_y = (clut >> 6) & 0x1FF;
      int xs[4] = { X, X + 16, X, X + 16 }, ys[4] = { Y, Y, Y + 16, Y + 16 };
      int us[4] = { u, u + 16, u, u + 16 }, vs[4] = { v, v, v + 16, v + 16 };
      sil_bbox_log_i("bg_tilemap", xs, ys, 4);
      rq_push_2d_quad(c, RQ_BACKGROUND, /*order_2d_fg=*/0, xs, ys, us, vs, col, col, col,
                      tp_x, tp_y, mode, /*raw=*/1, clut_x, clut_y, 0, 0, 0, 0, 0, 0, 1023, 511);
      g_sn_cmds++;
      t0 += 2; if (t0 >= rowstride) t0 = 0;        // column wrap
      t1 += 16;
      if (!((int16_t)t1 < t5)) break;
    }
    t2 += rowstride; if ((int16_t)t2 >= mapbytes) t2 -= mapbytes;  // row wrap
    t8 += 16;
    if (!((int16_t)t8 < outer_bound)) break;
  }
}

void Render::sceneNative() { Core* c = mCore;
  static const uint32_t HEADS[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
  uint32_t saved = c->r[4];
  g_sn_objs = g_sn_cmds = 0;
  // (0) BACKDROP (sky + distant parallax hills) — the field's background drawer, dispatched natively. The
  // PSX path is 0x8003df04's 16-state jump table @0x80014fc0 keyed on mem_r8(0x800bf870), gated by
  // mem_r8(0x800bf873)==0. Only state 0 (→ tilemap drawer 0x80115598) is owned natively so far; other
  // fields' drawers are still-PSX and stay black here until ported (frontier). RQ_BACKGROUND → behind world.
  if (c->mem_r8(0x800bf873u) == 0) {
    uint32_t bgstate = c->mem_r8(0x800bf870u);
    if (bgstate == 0) ov_bg_tilemap_native(c, 0x800ed018u);
  }
  if (cfg_dbg("bgonly")) { c->r[4] = saved; return; }  // PROBE: backdrop only (test if ov_render_frame already drew the world)
  // AREA-INIT SUPPRESSION: on the GAME field-area-machine OBJECT-PLACEMENT init frame (stage GAME, sm[0x48]==2
  // RUNNING, sm[0x4a]==1 field-area-machine, sm[0x4e]==0 = the ov_field_run case-0 init), the new area's
  // objects have just been spawned but their MODELS are NOT yet attached — attach runs from per-object
  // behaviours on the following running frames (sm[0x4e]>=1), so each object's render-command geomblk
  // (cmd+0x40) still holds an unrelocated area-data pointer (0x8018Axxx). The real game does not draw the
  // field during this init (the area transition holds the screen faded black); drawing it here feeds garbage
  // prim counts into native_gt3gt4 and overflows the render queue (later-275). Skip the field scene for that
  // frame; the backdrop above still draws. Read from task0's GAME state machine (persistent guest RAM, so it
  // is robust to the GAME loop's coroutine scheduling — a per-frame "did the field render run" latch is not,
  // because the field update and this display pass need not fall in the same native_step_frame).
  bool field_area_init = c->mem_r32(0x801fe00cu) == 0x8010637Cu   // GAME stage resident
                      && c->mem_r16(0x801fe048u) == 2             // sm[0x48] == RUNNING
                      && c->mem_r16(0x801fe04au) == 1             // sm[0x4a] == field area machine
                      && c->mem_r16(0x801fe04eu) == 0;            // sm[0x4e] == object-placement init (pre-attach)
  if (!field_area_init) {
    // (a) TERRAIN + per-object world geometry via the native render walks (self-route to ov_terrain etc.).
    ov_rwalk_aux_bf00(c); ov_rwalk_aux_eec0(c); ov_rwalk_b588(c); ov_render_walk_snapshot(c);
    ov_rwalk_aux_bcf4(c); ov_render_walk(c);
    // (b) SCENE TABLE (grass / props / sky-sea backdrop) — native world-coord render of 0x800F2418.
    fieldEntityRender(0x800F2418u);
    // (c) the field's OBJECTS — walk the 3 entity lists, render each object's geomblk natively (real depth).
    for (int h = 0; h < 3; h++) {
      uint32_t n = c->mem_r32(HEADS[h]);
      for (int g = 0; n && g < 400; g++, n = c->mem_r32(n + 0x24)) {
        if (c->mem_r8(n + 8) == 0 || c->mem_r8(n + 9) == 0) continue;   // no render commands
        g_sn_objs++; g_sn_cmds += c->mem_r8(n + 8);
        c->r[4] = n;
        submit_perobj_flush(c);
      }
    }
  }
  c->r[4] = saved;
  if (cfg_dbg("scenenative")) { int gpu_seen3d_this_frame(Core*); static int f = 0; if ((f++ % 60) == 0)
    fprintf(stderr, "[scenenative] objs=%ld cmds=%ld seen3d=%d\n", g_sn_objs, g_sn_cmds, gpu_seen3d_this_frame(c)); }
}

// NATIVE per-object render DISPATCH — gen_func_8003CCA4 (later-135). The phase-2 per-object render entry:
// stash the current render object (scratch 0x1F80028C), compute the flush flag (= node[0xb]==0xf, the
// "world" objects), select a case by idx = node[0xd]&0xb (idx>=9 → not rendered), and for the common
// flush-only case (jump-table target 0x8003CD00) run the native per-object flush — NO guest render code.
// The other cases add a secondary effect pass (gen_func_8003D584/8003F344/8003F3F4/8003F4C4/8003F594)
// over the just-emitted packet range; those are not owned yet, so for them we super-call the recomp body
// (which still calls the now-native func_8003CDD8 for the flush, then the secondary pass). At the field
// only idx0 (flush-only) fires (PSXPORT_DEBUG=ccase: 1 call/frame, target 0x8003cd00).
// Per-object render depth is now the EXACT object-origin projection (proj_obj_center_ord = the object's
// model origin (0,0,0) transformed through the LIVE composed camera×object transform the renderer just
// loaded → finer-than-integer view-Z), unifying these per-object tag sites with the universal render-cmd
// dispatcher chokepoint (ov_render_cmd) which already uses it. This replaced the old coarse
// `object_world_view_depth` (sign-extended u16 node position dotted with the camera-forward, >>12 with the
// 12-bit fraction discarded): that coarseness collapsed nearby objects to the same depth band, so which one
// won a shared pixel fell to draw ORDER — exactly why a flame billboard drew OVER nearer foreground decor
// (#4). The exact origin projection is "where the object actually is", so depth/occlusion is owned from the
// object's real placement, not a quantized approximation. (engine owns placement → engine owns depth.)

int g_perobj_psx = 0;   // BISECT: route the per-object render to the PSX body (compare which native piece diverges)
static void submit_perobj_render(Core* c) {
  if (g_perobj_psx) { rec_super_call(c, 0x8003CCA4u); return; }
  uint32_t node = c->r[4];
  g_dbg_render_node = node;                               // objid: tag this object's prims
  c->mem_w32(SCR + 0x28C, node);                          // current render object (read by downstream code)
  uint32_t idx = c->mem_r8(node + 0xD) & 0xB;
  if (idx >= 9) return;                                // not rendered
  uint32_t flag = (c->mem_r8(node + 0xB) == 0xF) ? 1u : 0u;
  uint32_t tgt = c->mem_r32(0x80014EC8u + idx * 4);
  // Tag the packet-pool span this object renders into with its PC-native WORLD-POSITION depth, so its 2D
  // billboard prims (apple quad, etc.) occlude by real depth at the deferred OT walk. (g_pkt_track records
  // the actual store range — the pool POINTER doesn't move for these renderers.)
  uint32_t slo, shi; PktSpanSession sess;
  if (tgt == 0x8003CD00u) { c->r[4] = node; c->r[5] = flag; submit_perobj_flush(c); }  // flush-only (native)
  else                    { rec_super_call(c, 0x8003CCA4u); }                          // secondary-effect case
  if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, node);   // PC-native depth from real world position
    gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, node); }   // fps60: this object's billboards reproject at midpoint
  g_dbg_render_node = 0;                                  // objid: end this object's render scope
}
void ov_perobj_render(Core* c) {
  submit_perobj_render(c);
}

// NATIVE seaside-area GROUND/BG node renderer — OVERLAY 0x8013E9D8 (the renderfn of the field's BG/world
// node 0x800FC5C0, dispatched by the master render-list walk's default case). The recomp wrapper: copy a
// position triple (*(node+0x14)[0/2/4]) + node[0x4e/50/52] onto the stack, call the GTE visibility/setup
// 0x8013DD34(a0=&pos, a1=&node-triple), then call the per-object render dispatch 0x8003CCA4(node). We own
// it so the dispatch goes to the NATIVE submit_perobj_render -> submit_perobj_flush (world-coord eproj
// projection) — this node carries 12 render commands = the main ground geometry, so this is what makes the
// visible seaside ground render PC-native from world coords. 0x8013DD34 stays PSX (rec_dispatch): it writes
// only the scratchpad cull/bound temps (0x1F8000C0/0x1F800080), NOT the per-command transform eproj reads,
// and the recomp calls 0x8003CCA4 UNCONDITIONALLY after it (it is a side-effect setup, not a gate).
void ov_bg_render(Core* c) {
  uint32_t node = c->r[4], saved_sp = c->r[29], ra = c->r[31];
  uint32_t sp = saved_sp - 0x28; c->r[29] = sp;          // mirror the recomp frame (addiu sp,-0x28)
  uint32_t pp = c->mem_r32(node + 0x14);                  // position-source ptr
  c->mem_w16(sp + 0x10, c->mem_r16(pp + 0));             // pos triple (3 u16) -> sp+0x10/0x12/0x14
  c->mem_w16(sp + 0x12, c->mem_r16(pp + 2));
  c->mem_w16(sp + 0x14, c->mem_r16(pp + 4));
  c->mem_w16(sp + 0x18, c->mem_r16(node + 0x4e));        // node triple (3 u16) -> sp+0x18/0x1a/0x1c
  c->mem_w16(sp + 0x1a, c->mem_r16(node + 0x50));
  c->mem_w16(sp + 0x1c, c->mem_r16(node + 0x52));
  c->r[4] = sp + 0x10; c->r[5] = sp + 0x18;
  rec_dispatch(c, 0x8013DD34u);                           // PSX GTE visibility/bound setup (faithful side-effect)
  c->r[29] = saved_sp; c->r[31] = ra;                    // pop the frame
  c->r[4] = node; submit_perobj_render(c);                // native world-coord per-object render
}

// NATIVE phase-2 render-list WALK — gen_func_8003C048 (later-135). The master draining of the per-frame
// render list: iterate the linked list (head *0x800F2624, next at node+36), skip non-live nodes
// (node+1==0), and dispatch each live node by its render type (node+0xb, <33) through the 33-entry jump
// table @0x80014DB8 to a per-type renderer. This is the engine's "entity-list iteration → render
// submission" — the native-engine layer named in the project goal.
//
// Faithful-first + own-when-fully-handleable: we PRE-SCAN the live nodes; if EVERY one resolves to a
// case we own natively, run the native walk; otherwise super-call the whole recomp body (so an unfamiliar
// scene is always correct, never a fragile partial). The two cases that fire at the field
// (PSXPORT_DEBUG=rlist):
//   - table target 0x8003C0B4 = the per-object render case → native submit_perobj_render(node).
//   - table target 0x8003C29C = the default case → rec_dispatch(node, *(node+24)) (the node's own render
//     fn ptr; its leaf submit is owned where owned). Identical to the recomp default block.
// Types ≥33 render nothing (the recomp skips them) → treated as handled no-ops.
#define RLIST_HEAD   0x800F2624u
#define RLIST_TABLE  0x80014DB8u
#define RCASE_PEROBJ 0x8003C0B4u             // jump-table target = the gen_func_8003CCA4 (per-object) case
#define RCASE_DEFAULT 0x8003C29Cu            // jump-table target = the default case: rec_dispatch(node+24)
// later-239: type-4 case (jump-table tgt 0x8003C0E8) = `FUN_80039f4c(node)` — a multi-element object
// renderer (GTE-transforms node+0xC0 elements). It is the ONLY case the steady field uses beyond
// PEROBJ/DEFAULT, and its presence forced the WHOLE walk to super-call PSX every steady frame (so the
// native walk never ran in gameplay). Own the case natively (dispatch the leaf, tag its packet span with
// the node's PC-native world depth) so the steady master walk runs native; the leaf stays guest content.
#define RCASE_TYPE4   0x8003C0E8u
#define RFN_TYPE4     0x80039F4Cu
// later-274: own the remaining simple master-walk cases so the walk NEVER falls back to the recomp body.
// The fallback (rec_super_call 0x8003C048) ran the recompiled per-object chain
// 0x8003CCA4 -> 0x8003CDD8 -> per-area dispatch 0x8003F698 -> the resident-area GT3/GT4 submitter (e.g. A00's
// 0x80146478). During the SOP intro NARRATION that submitter's CODE overlay is NOT resident (only the area
// DATA is), so the super-call recomp-MISSed and aborted the shipping game (later-273). The native walk
// avoids this: type-0 -> submit_perobj_flush -> native_gt3gt4 directly (no 0x8003F698 / no overlay dispatch).
// The only thing forcing the fallback was the NARRATION scene's type-16 node. Own it (and its structural
// siblings 17-19 + the render-nothing cases) so the field/cutscene render stays PC-native:
//   - skip cases (types 9-14,21-31): jump-table target 0x8003C2AC = the loop-continue no-op (render nothing).
//   - leaf cases (types 16-19): `jal <leaf>(node)` then loop-continue. The four leaves only call resident
//     MAIN library fns (matrix/GTE/libgpu 0x80051794/0x800517bc/0x80085050/0x800847f0/0x80084110) — never
//     0x8003F698 and never an overlay address — so rec_dispatching the leaf can NOT miss. Content stays PSX;
//     the prim span is tagged with the node's PC-native world depth like the other owned cases.
// type-4 (RCASE_TYPE4) and type-20 (0x8003C188, which conditionally jal's the field overlay 0x8011be5c)
// are intentionally NOT accepted here — they keep falling back (type-4 per the later-239 occlusion note;
// type-20 because its overlay leaf could itself be non-resident).
#define RCASE_NOOP    0x8003C2ACu
static uint32_t rw_leaf_for(uint32_t tgt) {
  switch (tgt) {
    case 0x8003C148u: return 0x8003C2D4u;   // type 16
    case 0x8003C158u: return 0x8003C464u;   // type 17
    case 0x8003C168u: return 0x8003C5F8u;   // type 18
    case 0x8003C178u: return 0x8003C788u;   // type 19
    default:          return 0u;
  }
}

// PSXPORT_BDTAG per-node attribution (later-239): name a node's render route + fn so the deferred gp0
// OT-walk classifier pins which node in the master walk builds the steady backdrop (sky/terrain quads).
extern "C" void ffspan_begin(void); extern "C" void ffspan_end(const char*);
static const char* rw_tag(const char* pfx, uint32_t fn) {
  static char buf[512][20]; static int bi = 0;       // ring (per-frame names; classify is deferred 1 frame)
  int i = bi; bi = (bi + 1) & 511;
  snprintf(buf[i], 20, "%s%05x", pfx, fn & 0xfffffu);
  return buf[i];
}

static void submit_render_walk(Core* c) {
  uint32_t head = c->mem_r32(RLIST_HEAD);
  if (head == 0) return;
  // pre-scan: bail to the recomp body if any live node uses a case we don't own natively.
  for (uint32_t n = head, g = 0; n && g < 256; n = c->mem_r32(n + 36), g++) {
    if (c->mem_r8(n + 1) == 0) continue;
    uint8_t t = c->mem_r8(n + 0xB);
    if (t >= 33) continue;                            // renders nothing (recomp skips) — handled
    uint32_t tgt = c->mem_r32(RLIST_TABLE + t * 4);
    if (cfg_dbg("rwtypes")) { static uint64_t seen=0; if(!(seen&(1ull<<t))){ seen|=(1ull<<t);
      fprintf(stderr,"[rwtypes] node type=%u tgt=%08X fn(node+24)=%08X\n", t, tgt, c->mem_r32(n+24)); } }
    // NOTE: type-4 (RCASE_TYPE4) intentionally NOT accepted here — accepting it activates the native
    // master walk in the steady field, which renders terrain at REAL depth and then OCCLUDES the flat
    // (is3d=0) objects (Tomba/entities/collectables). Until type-4 + the objects are owned with correct
    // depth, fall back to the all-PSX walk (flat OT order) so the steady field composites correctly.
    if (tgt != RCASE_PEROBJ && tgt != RCASE_DEFAULT && tgt != RCASE_NOOP && rw_leaf_for(tgt) == 0) {
      if (cfg_dbg("rwalk")) { static int w=0; if(!w++) fprintf(stderr,"[rwalk] FALLBACK: node type=%u tgt=%08X -> super-call PSX body\n", t, tgt); }
      rec_super_call(c, 0x8003C048u); return;
    }
  }
  if (cfg_dbg("rwalk")) { static int w=0; if(!w++) fprintf(stderr,"[rwalk] NATIVE walk active\n"); }
  // native walk: read `next` before dispatch (the recomp captures node+36 before the case runs).
  for (uint32_t n = head; n; ) {
    uint32_t next = c->mem_r32(n + 36);
    if (c->mem_r8(n + 1) != 0) {
      uint8_t t = c->mem_r8(n + 0xB);
      if (t < 33) {
        uint32_t tgt = c->mem_r32(RLIST_TABLE + t * 4);
        if (tgt == RCASE_PEROBJ) { ffspan_begin(); c->r[4] = n; submit_perobj_render(c); ffspan_end(rw_tag("rwP", c->mem_r32(n+24))); }  // self-tags its world depth
        else if (tgt == RCASE_NOOP) { /* types 9-14,21-31: render nothing (loop continue) */ }
        else if (uint32_t leaf = rw_leaf_for(tgt)) {   // types 16-19: special-effect leaf renderer (guest content, native depth)
          ffspan_begin();
          uint32_t slo, shi; PktSpanSession sess;
          c->r[4] = n; rec_dispatch(c, leaf);
          if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, n);
            gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, n); }
          ffspan_end(rw_tag("rwL", leaf));
        }
        else if (tgt == RCASE_TYPE4) {     // type-4 multi-element object renderer (FUN_80039f4c) — guest leaf, native depth
          ffspan_begin();
          uint32_t slo, shi; PktSpanSession sess;
          c->r[4] = n; rec_dispatch(c, RFN_TYPE4);
          if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, n);
            gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, n); }
          ffspan_end(rw_tag("rw4", RFN_TYPE4));
        }
        else {
          // default case: the node's own render fn (node+24) — e.g. a collectable's billboard-quad drawer,
          // or the field TERRAIN renderer (0x8002AB5C). Terrain is owned PC-native (ov_terrain → the float
          // terrain_render_pc, real per-pixel depth, NO GTE/packet) — route it there; everything else stays
          // PSX content (rec_dispatch), with its produced span tagged by the object's PC-native world depth.
          void ov_terrain(Core* c);
          uint32_t fn = c->mem_r32(n + 24);
          // DIAG probes (cfg_dbg): noterr skips the native terrain pass, nobg skips the native BG node —
          // to attribute the "stale village still drawn in the hut interior" bug to a specific native pass.
          if (fn == 0x8002AB5Cu && cfg_dbg("noterr")) { /* skip terrain */ }
          else if (fn == 0x8013E9D8u && cfg_dbg("nobg")) { /* skip bg */ }
          else if (fn == 0x8002AB5Cu) { ffspan_begin(); c->r[4] = n; ov_terrain(c); ffspan_end("rwT_terrain"); }   // PC-native world-coord terrain (self-draws)
          else if (fn == 0x8013E9D8u) { ffspan_begin(); c->r[4] = n; ov_bg_render(c); ffspan_end("rwB_bg"); }   // PC-native world-coord ground/BG node
          else if (!rec_addr_has_entry(c, fn)) { /* STALE node: its renderer is a dangling pointer into an
              evicted overlay (e.g. a SOP intro-narration node surviving into the A00 field — later-275).
              The engine owns its render visibility: skip it rather than dispatch into mid-overlay garbage. */ }
          else {
            ffspan_begin();
            uint32_t slo, shi; PktSpanSession sess;
            c->r[4] = n; rec_dispatch(c, fn);
            if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, n);   // PC-native depth from real world position
              gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, n); }
            ffspan_end(rw_tag("rwD", fn));
          }
        }
      }
    }
    n = next;
  }
}
void ov_render_walk(Core* c) {
  submit_render_walk(c);
}

// NATIVE depth for the collectable BILLBOARD-QUAD drawer — gen_func_8003C8F4. This is the single chokepoint
// for the op-2D textured quads the collectables (apple + score pickups) draw as 2D billboards: it GTE-projects
// the quad with the object's composed camera×object transform already live in CR0-7 (RTPT/RTPS @0x8003c98c/9dc).
// The recomp body emits the quad packet into the OT with NO depth, so the pickups fell to the flat 2D band and
// did not occlude. We compute the object's PC-native WORLD-POSITION view-Z from that live transform
// (proj_obj_center_ord = our float proj of the object origin = CR5-7 view translation) and tag the packet span
// the body writes, so the deferred OT walk gives each pickup its real world depth. Reached from multiple render
// walks (owned and un-owned) — owning it HERE covers them all. Super-calls the body (content/packet unchanged).
float proj_obj_center_ord(void);
void ov_collectable_quad(Core* c) {
  float ord = obj_world_ord(c, cur_render_node(c));  // PC-native depth from the object's real world position
  uint32_t slo, shi; PktSpanSession sess;
  rec_super_call(c, 0x8003C8F4u);
  if (sess.close(&slo, &shi)) { gpu_obj_depth_add(c, slo, shi, ord);   // slo/shi already KSEG
    // fps60: the collectable's billboard quad reprojects at the midpoint camera. Keyed by the SPAN [slo,shi)
    // (the OT walk matches the item's source node against it) + identity = the current render object
    // (scratch 0x1F80028C, set by submit_perobj_render); the composed camera×object transform is still live
    // in CR0-7 here (proj_obj_center_ord just read it), so fps60_record_billboard_span captures it.
    if (g_fps60_on || g_mods.debug_ids || cfg_dbg("objid")) fps60_record_billboard_span(c, slo, shi, cur_render_node(c)); }
}

// ===================================================================================================
// NATIVE WORLD-BUILDING render walk — gen_func_8003BB50 (the SNAPSHOT-QUEUE object render driver).
//
// This is the engine's per-frame object render phase for the FIELD: it drains the per-object render
// QUEUE (a snapshot of node pointers the entity walk enqueued — scratchpad cursor 0x1F800140 / count
// 0x1F800146), and for each live object dispatches its per-type renderer (by node+0xb through jump
// table 0x80014A70). The recomp body drove this loop but threw away each object's WORLD DEPTH — the 2D
// billboard prims the renderers emit (sprites / flat-textured quads with no projected vertices) then
// fell to a flat enumeration-ordered 2D band, so collectables/decals did NOT occlude by distance.
//
// We OWN the loop natively so the engine drives object rendering; the per-type renderers themselves stay
// guest content (rec_dispatch). Each object's PC-native WORLD-POSITION depth is attached downstream at the
// universal render-command dispatcher (ov_render_cmd on 0x8003F698), where the composed camera×object
// transform is live and the command's packet-pool span is captured — see ov_render_cmd above.
//
// Decoded byte-faithful from the recomp body (subagent RE, this session). Globals (PSX scratchpad):
//   swap_flag 0x1F800136, live list_ptr 0x1F80013C, read cursor 0x1F800140, live count 0x1F800144,
//   snap_count 0x1F800146; list base const 0x800F2410. Queue entries are raw entity-NODE pointers.
#define RQ_SWAP_FLAG  0x1F800136u
#define RQ_LIST_PTR   0x1F80013Cu
#define RQ_CURSOR     0x1F800140u
#define RQ_LIVE_CNT   0x1F800144u
#define RQ_SNAP_CNT   0x1F800146u
#define RQ_LIST_BASE  0x800F2410u
#define RQ_JUMPTABLE  0x80014A70u

// Replicate one jump-table case (node[0xb] → renderer), calling the existing guest renderers via
// rec_dispatch (content stays PSX). The depth for `node` is already published by the caller.
static void rq_dispatch_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003BC00u:   // per-object render dispatch, then the optional main renderer
    case 0x8003BC24u: { // alt scene/overlay submitter, then the optional main renderer
      c->r[4] = node;
      if (tgt == 0x8003BC00u) submit_perobj_render(c);    // native (world-coord eproj projection)
      else rec_dispatch(c, 0x80122974u);
      uint8_t b = c->mem_r8(node + 0xB);
      if (b & 0x40) { c->r[4] = node; c->r[5] = 80; c->r[6] = 0; rec_dispatch(c, 0x8002AE0Cu); }
      else if (b & 0x80) { c->r[4] = node; c->r[5] = (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 0x80); c->r[6] = 0;
                           rec_dispatch(c, 0x8002AE0Cu); }
      break; }
    case 0x8003BC6Cu: c->r[4] = node; rec_dispatch(c, 0x8003C2D4u); break;
    case 0x8003BC7Cu: c->r[4] = node; rec_dispatch(c, 0x8003C464u); break;
    case 0x8003BC8Cu: c->r[4] = node; rec_dispatch(c, 0x8003C5F8u); break;
    case 0x8003BC9Cu: c->r[4] = node; rec_dispatch(c, 0x8003C788u); break;
    case 0x8003BCACu: { c->r[4] = node; rec_dispatch(c, 0x8003C2D4u);                 // then indirect node[0x7c]
                        uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BCB4u: { uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BCC0u: { uint32_t fn = c->mem_r32(node + 0x18); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    default: break;   // 0x8003BCD0 = the skip/default case: render nothing
  }
}

static void submit_render_walk_snapshot(Core* c) {
  // Prologue: queue double-buffer SWAP (only when swap_flag==0) — capture the live count/cursor as the
  // read snapshot and reset the live write cursor to the list base for next frame's enqueues.
  if (c->mem_r8(RQ_SWAP_FLAG) == 0) {
    uint16_t cnt = c->mem_r16(RQ_LIVE_CNT);
    uint32_t lst = c->mem_r32(RQ_LIST_PTR);
    c->mem_w16(RQ_LIVE_CNT, 0);
    c->mem_w32(RQ_LIST_PTR, RQ_LIST_BASE);
    c->mem_w16(RQ_SNAP_CNT, cnt);
    c->mem_w32(RQ_CURSOR, lst);
  }
  int16_t count = (int16_t)c->mem_r16(RQ_SNAP_CNT);
  uint32_t cursor = c->mem_r32(RQ_CURSOR);
  while (count != 0) {
    uint32_t node = c->mem_r32(cursor);
    cursor += 4;
    count--;
    if (c->mem_r8(node + 1) == 0) continue;                  // not live
    uint8_t t = c->mem_r8(node + 0xB);
    if (t >= 144) continue;                                  // out of jump-table range -> render nothing
    uint32_t tgt = c->mem_r32(RQ_JUMPTABLE + t * 4);
    if (tgt == 0x8003BCD0u) continue;                        // default/skip case
    // Render the object, tagging the packet-pool span it produces with its PC-native world-position depth
    // so its 2D billboard prims (collectable quads, etc.) occlude for real at the deferred OT walk.
    uint32_t slo, shi; PktSpanSession sess;
    g_dbg_render_node = node;                                // objid: tag every prim this object emits
    rq_dispatch_case(c, node, tgt);                          // run the object's per-type renderer (guest content)
    g_dbg_render_node = 0;
    if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, node);   // PC-native depth from real world position
      gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, node); }   // fps60: object billboards reproject at midpoint
  }
}

void ov_render_walk_snapshot(Core* c) {
  submit_render_walk_snapshot(c);
}

// ===================================================================================================
// NATIVE AUXILIARY render walks — gen_func_8003BCF4 / 8003BF00 / 8003EEC0 (issue #4: flames/ropes
// drew OVER occluding foliage). These three are the secondary per-object render walks the field runs
// IN ADDITION to the owned snapshot walk 8003BB50 — they drain their own object queues/lists and
// dispatch each live object's per-type renderer through a jump table, exactly like 8003BB50. The
// recomp bodies drove the loop but threw away each object's WORLD DEPTH, so the 2D billboard prims the
// renderers emit (flame sprites, rope decals) landed in NO obj_depth span → fell to the flat 2D band →
// drew in enumeration order, in front of nearer foliage. We OWN the loops PC-native (faithful per-node
// lift of each recomp body), and after running each object's renderer we tag the packet-pool span it
// produced with the object's PC-native WORLD-POSITION depth via gpu_obj_depth_add — exactly as the
// owned snapshot walk does — so the deferred OT walk gives each effect its real world depth. The
// per-type renderers themselves stay guest CONTENT (rec_dispatch), unchanged. Depth tagging is
// PER-NODE (not a whole-walk merged span — a merged span mis-attributes depth in multi-effect scenes).
//
// Decoded byte-faithful from the recomp bodies (tools/disas.py, this session). For each: node is the
// only arg (a0); `next`/queue-advance is captured the same instant the recomp body captures it.

// --- 8003BCF4 -------------------------------------------------------------------------------------
// SNAPSHOT-QUEUE walk, same double-buffer-swap prologue as 8003BB50 but a DIFFERENT queue + table.
//   swap_flag 0x1F800136, live list_ptr 0x1F800148, live count 0x1F800150, snap_count 0x1F800152,
//   read cursor 0x1F80014C; list base const 0x800F26C8; jump table 0x80014CB0 (33 entries, type<33).
//   Liveness node+1!=0; type node+0xb. The s3=0x800C0000 base in two cases reads global 0x800BF870.
// Jump-table case bodies (a0=node), decoded from 0x8003BDAC..0x8003BED8 (0x8003BED8 = skip/continue):
//   idx 0,15  (0x8003BDAC): rec_dispatch 0x8003CCA4 (per-object render dispatch)
//   idx 1     (0x8003BDBC): g=*0x800BF870; ==0 -> 0x801341E8; ==6 -> 0x80123C14; else skip
//   idx 2     (0x8003BDF4): g; ==1->0x80129114, ==7->0x80120D2C, ==10->0x8011AD44, ==15->0x80115338,
//                           else -> 0x80117984
//   idx 3     (0x8003BE74): rec_dispatch 0x80136748
//   idx 16    (0x8003BE84): rec_dispatch 0x8003C2D4
//   idx 17    (0x8003BEA4): rec_dispatch 0x8003C464
//   idx 21    (0x8003BEB4): rec_dispatch 0x8003C2D4, then node[0x7c] indirect (if nonzero)
//   idx 22    (0x8003BEBC): node[0x7c] indirect (if nonzero)
//   idx 23    (0x8003BE94): node[0x7c] indirect (if nonzero), THEN rec_dispatch 0x8003C464
//   idx 32    (0x8003BEC8): node[0x18] indirect (if nonzero)
#define AUX_BCF4_SWAP    0x1F800136u
#define AUX_BCF4_LISTPTR 0x1F800148u
#define AUX_BCF4_CURSOR  0x1F80014Cu
#define AUX_BCF4_LIVECNT 0x1F800150u
#define AUX_BCF4_SNAPCNT 0x1F800152u
#define AUX_BCF4_BASE    0x800F26C8u
#define AUX_BCF4_TABLE   0x80014CB0u
#define AUX_BCF4_SKIP    0x8003BED8u
#define G_RENDER_MODE    0x800BF870u

static void aux_bcf4_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003BDACu: c->r[4] = node; submit_perobj_render(c); break;   // native (world-coord eproj projection)
    case 0x8003BDBCu: { uint8_t g = c->mem_r8(G_RENDER_MODE);
                        if (g == 0)      { c->r[4] = node; rec_dispatch(c, 0x801341E8u); }
                        else if (g == 6) { c->r[4] = node; rec_dispatch(c, 0x80123C14u); }
                        break; }
    case 0x8003BDF4u: { uint8_t g = c->mem_r8(G_RENDER_MODE); uint32_t fn;
                        if      (g == 1)  fn = 0x80129114u;
                        else if (g == 7)  fn = 0x80120D2Cu;
                        else if (g == 10) fn = 0x8011AD44u;
                        else if (g == 15) fn = 0x80115338u;
                        else              fn = 0x80117984u;
                        c->r[4] = node; rec_dispatch(c, fn); break; }
    case 0x8003BE74u: c->r[4] = node; rec_dispatch(c, 0x80136748u); break;
    case 0x8003BE84u: c->r[4] = node; rec_dispatch(c, 0x8003C2D4u); break;
    case 0x8003BEA4u: c->r[4] = node; rec_dispatch(c, 0x8003C464u); break;
    case 0x8003BEB4u: { c->r[4] = node; rec_dispatch(c, 0x8003C2D4u);
                        uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BEBCu: { uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BE94u: { uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); }
                        c->r[4] = node; rec_dispatch(c, 0x8003C464u); break; }
    case 0x8003BEC8u: { uint32_t fn = c->mem_r32(node + 0x18); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    default: break;   // 0x8003BED8 = skip/default: render nothing
  }
}

void ov_rwalk_aux_bcf4(Core* c) {
  if (c->mem_r8(AUX_BCF4_SWAP) == 0) {              // queue double-buffer swap (only when swap_flag==0)
    uint16_t cnt = c->mem_r16(AUX_BCF4_LIVECNT);
    uint32_t lst = c->mem_r32(AUX_BCF4_LISTPTR);
    c->mem_w16(AUX_BCF4_LIVECNT, 0);
    c->mem_w32(AUX_BCF4_LISTPTR, AUX_BCF4_BASE);
    c->mem_w16(AUX_BCF4_SNAPCNT, cnt);
    c->mem_w32(AUX_BCF4_CURSOR, lst);
  }
  int16_t count = (int16_t)c->mem_r16(AUX_BCF4_SNAPCNT);
  uint32_t cursor = c->mem_r32(AUX_BCF4_CURSOR);
  while (count != 0) {
    uint32_t node = c->mem_r32(cursor);
    cursor += 4;
    count--;
    if (c->mem_r8(node + 1) == 0) continue;                  // not live
    uint8_t t = c->mem_r8(node + 0xB);
    if (t >= 33) continue;                                   // out of jump-table range -> render nothing
    uint32_t tgt = c->mem_r32(AUX_BCF4_TABLE + t * 4);
    if (tgt == AUX_BCF4_SKIP) continue;                      // skip/default
    uint32_t slo, shi; PktSpanSession sess;
    aux_bcf4_case(c, node, tgt);                             // per-type renderer (guest content)
    if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, node);   // PC-native depth from real world position
      gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, node); }   // fps60: object billboards reproject at midpoint
  }
}

// --- 8003BF00 -------------------------------------------------------------------------------------
// SNAPSHOT-QUEUE walk (own queue), same swap prologue but type range <32.
//   swap_flag 0x1F800136, live list_ptr 0x1F800154, live count 0x1F80015C, snap_count 0x1F80015E,
//   read cursor 0x1F800158; list base const 0x800F2738; jump table 0x80014D38 (32 entries, type<32).
//   Liveness node+1!=0; type node+0xb.
// Jump-table case bodies (a0=node), decoded from 0x8003BFAC..0x8003C028 (0x8003C028 = skip/continue):
//   idx 0,15 (0x8003BFAC): rec_dispatch 0x8003CCA4
//   idx 16   (0x8003BFBC): rec_dispatch 0x8003C2D4
//   idx 17   (0x8003BFCC): rec_dispatch 0x8003C464
//   idx 18   (0x8003BFDC): rec_dispatch 0x8003C5F8
//   idx 19   (0x8003BFEC): rec_dispatch 0x8003C788
//   idx 31   (0x8003BFFC): g=*0x800BF870; ==20 -> 0x8010FC70; else -> 0x8004CC88
#define AUX_BF00_SWAP    0x1F800136u
#define AUX_BF00_LISTPTR 0x1F800154u
#define AUX_BF00_CURSOR  0x1F800158u
#define AUX_BF00_LIVECNT 0x1F80015Cu
#define AUX_BF00_SNAPCNT 0x1F80015Eu
#define AUX_BF00_BASE    0x800F2738u
#define AUX_BF00_TABLE   0x80014D38u
#define AUX_BF00_SKIP    0x8003C028u

static void aux_bf00_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003BFACu: c->r[4] = node; submit_perobj_render(c); break;   // native (world-coord eproj projection)
    case 0x8003BFBCu: c->r[4] = node; rec_dispatch(c, 0x8003C2D4u); break;
    case 0x8003BFCCu: c->r[4] = node; rec_dispatch(c, 0x8003C464u); break;
    case 0x8003BFDCu: c->r[4] = node; rec_dispatch(c, 0x8003C5F8u); break;
    case 0x8003BFECu: c->r[4] = node; rec_dispatch(c, 0x8003C788u); break;
    case 0x8003BFFCu: { uint8_t g = c->mem_r8(G_RENDER_MODE);
                        c->r[4] = node; rec_dispatch(c, g == 20 ? 0x8010FC70u : 0x8004CC88u); break; }
    default: break;   // 0x8003C028 = skip/default: render nothing
  }
}

void ov_rwalk_aux_bf00(Core* c) {
  if (c->mem_r8(AUX_BF00_SWAP) == 0) {             // queue double-buffer swap (only when swap_flag==0)
    uint16_t cnt = c->mem_r16(AUX_BF00_LIVECNT);
    uint32_t lst = c->mem_r32(AUX_BF00_LISTPTR);
    c->mem_w16(AUX_BF00_LIVECNT, 0);
    c->mem_w32(AUX_BF00_LISTPTR, AUX_BF00_BASE);
    c->mem_w16(AUX_BF00_SNAPCNT, cnt);
    c->mem_w32(AUX_BF00_CURSOR, lst);
  }
  int16_t count = (int16_t)c->mem_r16(AUX_BF00_SNAPCNT);
  uint32_t cursor = c->mem_r32(AUX_BF00_CURSOR);
  while (count != 0) {
    uint32_t node = c->mem_r32(cursor);
    cursor += 4;
    count--;
    if (c->mem_r8(node + 1) == 0) continue;                  // not live
    uint8_t t = c->mem_r8(node + 0xB);
    if (t >= 32) continue;                                   // out of jump-table range -> render nothing
    uint32_t tgt = c->mem_r32(AUX_BF00_TABLE + t * 4);
    if (tgt == AUX_BF00_SKIP) continue;                      // skip/default
    uint32_t slo, shi; PktSpanSession sess;
    aux_bf00_case(c, node, tgt);                             // per-type renderer (guest content)
    if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, node);   // PC-native depth from real world position
      gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, node); }   // fps60: object billboards reproject at midpoint
  }
}

// --- 8003EEC0 -------------------------------------------------------------------------------------
// LINKED-LIST walk (NOT a snapshot queue): head = *0x800F2738 (the same list base 8003BF00 enqueues
// into), next = node+36 (captured BEFORE dispatch), liveness node+1!=0, type node+0xb (<33), jump
// table 0x80015000 (33 entries). Has NO swap prologue.
// Jump-table case bodies (a0=node), decoded from 0x8003EF20..0x8003EF78 (0x8003EF78 = skip/advance):
//   idx 0,15 (0x8003EF20): rec_dispatch 0x8003CCA4
//   idx 1    (0x8003EF30): rec_dispatch 0x8003CCA4, then rec_dispatch 0x8003B704
//   idx 16   (0x8003EF40): rec_dispatch 0x8003C2D4, then IF node[2]==1 rec_dispatch 0x8003B704
//   idx 32   (0x8003EF68): node[0x18] indirect (if nonzero)
#define AUX_EEC0_HEAD    0x800F2738u
#define AUX_EEC0_TABLE   0x80015000u
#define AUX_EEC0_SKIP    0x8003EF78u

static void aux_eec0_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003EF20u: c->r[4] = node; submit_perobj_render(c); break;   // native (world-coord eproj projection)
    case 0x8003EF30u: c->r[4] = node; submit_perobj_render(c);          // native (world-coord eproj projection)
                      c->r[4] = node; rec_dispatch(c, 0x8003B704u); break;
    case 0x8003EF40u: { c->r[4] = node; rec_dispatch(c, 0x8003C2D4u);
                        if (c->mem_r8(node + 2) == 1) { c->r[4] = node; rec_dispatch(c, 0x8003B704u); } break; }
    case 0x8003EF68u: { uint32_t fn = c->mem_r32(node + 0x18); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    default: break;   // 0x8003EF78 = skip/default: render nothing
  }
}

void ov_rwalk_aux_eec0(Core* c) {
  uint32_t node = c->mem_r32(AUX_EEC0_HEAD);
  while (node) {
    uint32_t next = c->mem_r32(node + 36);                   // captured before dispatch (recomp s1)
    if (c->mem_r8(node + 1) != 0) {
      uint8_t t = c->mem_r8(node + 0xB);
      if (t < 33) {
        uint32_t tgt = c->mem_r32(AUX_EEC0_TABLE + t * 4);
        if (tgt != AUX_EEC0_SKIP) {
          uint32_t slo, shi; PktSpanSession sess;
          aux_eec0_case(c, node, tgt);                       // per-type renderer (guest content)
          if (sess.close(&slo, &shi)) { float od = obj_world_ord(c, node);   // PC-native depth from real world position
      gpu_obj_depth_add(c, slo, shi, od); fps60_bb_node(c, slo, shi, node); }   // fps60: object billboards reproject at midpoint
        }
      }
    }
    node = next;
  }
}

