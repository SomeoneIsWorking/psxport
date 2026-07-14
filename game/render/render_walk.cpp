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
// Split out of submit.cpp (the geometry-SUBMIT subsystem) so the scene renderer is its own PC-game
// file. Shared helpers (PktSpanSession, obj_world_ord/cur_render_node) live in render_internal.h.
#include "core.h"
#include "render.h"
#include "game.h"
#include "cfg.h"
#include "mods.h"
#include "render_queue.h"
#include "projection.h"   // EObjXform (per-object world-coord float projection; ops on Render)
#include "render_internal.h"
#include "player/actor_tomba.h"   // ActorTomba::G_ADDR — Tomba's node, outside the 3 generic entity lists
#include <stdio.h>
#include <string.h>
#include <math.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);
int  rec_addr_has_entry(Core*, uint32_t);   // overlay_router.cpp — is fn a real entry in the resident module?
#define OTBASE_PTR   0x800ED8C8u             // *this = the active ordering-table base
#define SCR          0x1F800000u             // PSX scratchpad base (the engine's GTE-compose temp area)

// The per-object render path is FULLY native (no PSX fallback): submit_perobj_flush composes the float
// world transform and calls Render::gt3gt4 directly. The old gen_func_8003F698 per-mode dispatcher (which
// ran interpreted per-scene submitter variants for non-generic modes) is no longer consulted — every
// per-object geomblk is submitted as generic GT3/GT4 through the native, world-coord projection.

void Render::perObjFlush() {
  Core* c = mCore;
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
      EObjXform w; c->mRender->projComposeObject(cmd, &w);
      c->mRender->projSetActive(&w);
      // OT base: node[0xd]&0xf == 4 selects a per-command sub-bucket (cmd[0x3f]*4 offset), else the base.
      uint32_t otbase = otbase_ptr;
      if ((c->mem_r8(node + 0xD) & 0xF) == 4)
        otbase = otbase_ptr + ((c->mem_r8s(cmd + 0x3F)) << 2);
      // fps60 TRUE per-object tier: the object's world transform was captured (keyed by cmd) inside
      // projComposeObject above; the GT3/GT4 submit projects it. No per-prim key needed anymore.
      c->mRender->gt3gt4(geomblk, otbase);              // fully-native generic GT3/GT4 submit (no PSX fallback)
      c->mRender->projClearActive();
    }
    i++;
    if (i >= (int)c->mem_r8(node + 9)) break;
  }
}

// ===================================================================================================
// ONE NATIVE RENDER PATH — world-data-driven scene render (Phase 1, user 2026-06-24 architecture:
// [[one-native-render-path-decoupled]]). Driven from the GAME's WORLD DATA, NOT from PSX GP0 packets:
// walk the 3 active entity lists, and render each live object's 3D model (geomblk via node+0xC0 cmds)
// through the native float-projection submitters (eproj + D32 depth + engine lighting). This is the
// single mechanism depth/60fps/ires/lighting attach to. It runs as its OWN pass (not bolted onto the
// PSX OT-walk) so the draw state is the native pass's. Gated `debug scenenative` while standing it up.
// (g_scene_native_diag was defined here but never read; dead — removed 2026-07-02)
// g_sn_objs/g_sn_cmds retired 2026-07-03 — Render::stats.snObjs/snCmds (RenderStats).
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
static void render_bg_tilemap_native(Core* c, uint32_t t4) {
  int W = c->mem_r8(t4 + 0x10), H = c->mem_r8(t4 + 0x11);
  if (W == 0 || H == 0) return;
  int rowstride = W * 2;                          // s0 — bytes per map row
  int mapbytes  = rowstride * H;                  // s3 — total map bytes (wrap modulus)
  int scrollX = c->mem_r16s(t4 + 0x28);
  int scrollY = c->mem_r16s(t4 + 0x2a);
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
  // WIDESCREEN backdrop coverage (root-cause fix for the [320,nw) atlas-garbage band): the PSX body tiles
  // a 320-wide window centred at screen-x 160 (t5 = ...+0x160 = 352 = 320+32 slack). At a wide aspect the
  // engine projects the world with OFX=nw/2, so the visible field spans [0,nw) — but the sky/parallax
  // backdrop, drawn only across ~[0,344), left the right margin uncovered, exposing the raw VRAM texture
  // atlas that lives past the 320-wide FB (later-55 VRAM packing). Re-centre the backdrop on the wide
  // centre (cx=nw/2) and widen the tiled window to nw+32 so it fills the full wide FB, matching the
  // world's OFX shift. cx/winw reduce to the exact 4:3 values (160 / 0x160) when not wide, so the 4:3
  // path stays byte-identical. Gated on gpu_gpu_wide_engine() (false at 4:3 / oracle / SBS legs).
  int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_w(Core*);
  int cx = 160, winw = 0x160;                       // screen-centre X / tiled window width (4:3 defaults)
  if (gpu_gpu_wide_engine(c)) { int nw = gpu_gpu_wide_engine_w(c); cx = nw / 2; winw = nw + 0x20; }
  // Starting tile row/col = (scroll - screen-center) >> 4, wrapped into [0,H) / [0,W).
  int rowtile = ((scrollY - 120) >> 4) % H; if (rowtile < 0) rowtile += H;
  int coltile = ((scrollX - cx) >> 4) % W; if (coltile < 0) coltile += W;
  int t2 = rowtile * rowstride;                   // current row byte offset (wraps mod mapbytes)
  int coloff0 = coltile * 2;                       // starting col byte offset (wraps mod rowstride)
  int xoff = (int16_t)(cx - 8 - scrollX);          // t9 — sub-tile X scroll remainder + screen offset
  int yoff = (int16_t)(112 - scrollY);             // s7 — sub-tile Y scroll remainder + screen offset
  unsigned char col[4] = { 0x80, 0x80, 0x80, 0x80 };  // 0x7d is raw-texture: color ignored
  int outer_bound = (int16_t)(scrollY - 120) + 0x100; // 16 rows
  int t5 = (int16_t)(scrollX - cx) + winw;            // wide-covering column window (4:3: ~22 cols)
  for (int t8 = scrollY - 120;;) {
    int Y = (int16_t)((t8 & 0xFFF0) + yoff);
    int t6 = (int16_t)t2;                          // row byte offset (sign-extended)
    int t0 = coloff0;
    for (int t1 = scrollX - cx;;) {
      int X = (int16_t)((t1 & 0xFFF0) + xoff);
      uint16_t tile = c->mem_r16(map + (uint32_t)(t6 + t0));
      int u = (tile & 0xF) << 4, v = (tile & 0xF0) + 8;
      uint16_t clut = (uint16_t)(clutbase + ((tile & 0xF00) >> 2));
      int clut_x = (clut & 0x3F) * 16, clut_y = (clut >> 6) & 0x1FF;
      int xs[4] = { X, X + 16, X, X + 16 }, ys[4] = { Y, Y, Y + 16, Y + 16 };
      int us[4] = { u, u + 16, u, u + 16 }, vs[4] = { v, v, v + 16, v + 16 };
      sil_bbox_log_i("bg_tilemap", xs, ys, 4);
      c->game->rq.push2dQuad(RQ_BACKGROUND, /*order_2d_fg=*/0, xs, ys, us, vs, col, col, col,
                             tp_x, tp_y, mode, /*raw=*/1, clut_x, clut_y, 0, 0, 0, 0, 0, 0, 1023, 511);
      c->mRender->stats.snCmds++;
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
  c->mRender->stats.snObjs = c->mRender->stats.snCmds = 0;
  // AREA-SCOPED CACHE trust latches (see render.h mSceneTableTrusted/mBackdropTrusted) — shared edge
  // detector, computed once per frame. Both SCENE_ENT_TABLE (0x800F2418) and PARALLAX_BG_SM
  // (0x800ED018, the backdrop tilemap struct below) go stale for a few ticks right when SOP narration
  // hands off to the field-area load; each latches back to trusted once ITS OWN structure shows the
  // natural re-zero its setup step performs before repopulating (see render.h for the full writeup).
  //
  // The handoff edge itself is read from the GAME submode dispatcher's own ownership switch
  // (sm[0x4a]==0: SOP field-mode machine owns the per-tick prepass/scroller; sm[0x4a]==1: the field-
  // area machine does, once it takes over) rather than the SOP overlay signature (0x80109450): the
  // area-load that repurposes PARALLAX_BG_SM's referenced memory (an in-progress CD stream landing
  // AS the same-tick RESET that also increments sm[0x4c]/sm[0x4a] — Sop::fieldMode case 4) clobbers it
  // 1-2 ticks BEFORE the overlay's own first-instruction word is overwritten by the incoming stream
  // (verified: sm[0x4c] leaves 0 the exact tick the garbage first appears, one tick ahead of sm[0x4a]
  // and two ahead of the overlay signature). sm[0x4c]!=0 while sm[0x4a]==0 is SOP's own one-shot
  // "narration ending, RESET case has fired" tail (it only ever increments once, at the single RESET
  // that ends the whole cutscene — not per-beat), so `sm4a==0 && sm4c==0` is "SOP genuinely still
  // owns this tick" without false-negatives across the narration's own beats.
  // The trust-latch EDGE DETECTOR is a once-per-frame state machine (mAreaCacheWasNarration is toggled by
  // the narration→field handoff edge). sceneNative runs exactly once per real logic frame (fps60 no longer
  // re-runs it for the in-between — docs/fps60-rework.md), so this always mutates the REAL frame's latches.
  {
    uint16_t sm4a = c->mem_r16(0x801fe04au), sm4c = c->mem_r16(0x801fe04cu);
    bool sop_narration_now = (sm4a == 0) && (sm4c == 0);
    if (sop_narration_now) {
      mSceneTableTrusted = true; mBackdropTrusted = true;      // narration's own prepass/scroller ticks both every tick
      mAreaCacheWasNarration = true;
    } else {
      if (mAreaCacheWasNarration) { mSceneTableTrusted = false; mBackdropTrusted = false; mAreaCacheWasNarration = false; }  // handoff edge
      if (!mSceneTableTrusted && c->mem_r8(0x800F2418u + 6u) == 0) mSceneTableTrusted = true;      // SCENE_ENT_TABLE owner reset seen
      if (!mBackdropTrusted   && c->mem_r8(0x800ed018u + 0x10u) == 0) mBackdropTrusted = true;     // PARALLAX_BG_SM owner reset seen (W==0)
    }
  }
  // (0) BACKDROP (sky + distant parallax hills) — the field's background drawer, dispatched natively. The
  // PSX path is 0x8003df04's 16-state jump table @0x80014fc0 keyed on mem_r8(0x800bf870), gated by
  // mem_r8(0x800bf873)==0. Only state 0 (→ tilemap drawer 0x80115598) is owned natively so far; other
  // fields' drawers are still-PSX and stay black here until ported (frontier). RQ_BACKGROUND → behind world.
  // Gated on mBackdropTrusted (see above): while untrusted, PARALLAX_BG_SM's tilemap pointer is the
  // ended narration's leftover, now aliasing the just-loaded field overlay's raw bytes — drawing it
  // produced a tiled noise/atlas-grid garbage frame (the narration-end -> fisherman-cutscene loading-
  // screen bug) where the oracle (pure PSX render) shows the expected black load-hold instead.
  if (mBackdropTrusted && c->mem_r8(0x800bf873u) == 0) {
    uint32_t bgstate = c->mem_r8(0x800bf870u);
    if (bgstate == 0) render_bg_tilemap_native(c, 0x800ed018u);
  }
  if (cfg_dbg("bgonly")) { c->r[4] = saved; return; }  // PROBE: backdrop only (test if ov_render_frame already drew the world)
  // AREA-INIT SUPPRESSION: on the GAME field-area-machine OBJECT-PLACEMENT init frame (stage GAME, sm[0x48]==2
  // RUNNING, sm[0x4a]==1 field-area-machine, sm[0x4e]==0 = the ov_field_run case-0 init), the new area's
  // objects have just been spawned but their MODELS are NOT yet attached — attach runs from per-object
  // behaviours on the following running frames (sm[0x4e]>=1), so each object's render-command geomblk
  // (cmd+0x40) still holds an unrelocated area-data pointer (0x8018Axxx). The real game does not draw the
  // field during this init (the area transition holds the screen faded black); drawing it here feeds garbage
  // prim counts into Render::gt3gt4 and overflows the render queue (later-275). Skip the field scene for that
  // frame; the backdrop above still draws. Read from task0's GAME state machine (persistent guest RAM, so it
  // is robust to the GAME loop's coroutine scheduling — a per-frame "did the field render run" latch is not,
  // because the field update and this display pass need not fall in the same native_step_frame).
  bool field_area_init = c->mem_r32(0x801fe00cu) == 0x8010637Cu   // GAME stage resident
                      && c->mem_r16(0x801fe048u) == 2             // sm[0x48] == RUNNING
                      && c->mem_r16(0x801fe04au) == 1             // sm[0x4a] == field area machine
                      && c->mem_r16(0x801fe04eu) == 0;            // sm[0x4e] == object-placement init (pre-attach)
  if (!field_area_init) {
    // (a) The render WALK CLUSTER (0x8003bf00/eec0/b588/bb50/bcf4/c048) is NO LONGER run from here
    // (USER 2026-07-07, issue #32): the substrate orchestrator executes it underneath every frame
    // (Render::frame — both render modes), so all its guest writes (walk-queue swaps, node bookkeeping,
    // per-node renderer dispatches, packet emission) happen on the faithful task's own call path,
    // byte-identical to the recomp reference. Re-running native lifts of those walks HERE was the
    // f26 divergence class: same writes from a foreign (display-phase) call context. The native lifts
    // (renderWalk/renderWalkSnapshot/rwalkAux*/rwalkB588/perObjRender/bgRender) are retired; this
    // display pass is READ-ONLY — it may not write guest memory or dispatch guest code. Per-object
    // depth tags for the guest-emitted billboard prims are lost until a read-only observer (e.g. an
    // EngineOverrides wrap teeing span info) is built — a KNOWN deferred render regression.
    // (a) TERRAIN — the field's render-list node whose render fn (node+24) is 0x8002AB5C, drawn by the
    // READ-ONLY float pass (real per-pixel depth). Pure reads: the node scan is the same enumeration the
    // substrate walk performs; the draw computes its matrices in host memory (native_terrain.cpp).
    // Enumeration+call moved to Render::terrainRenderAll() (submit.cpp) so Fps60's Tier-1 present-time
    // camera-lerp re-render (fps60.cpp) runs the identical sequence, not a hand-duplicated copy.
    terrainRenderAll();
    // (b) SCENE TABLE (grass / props / sky-sea backdrop) — native world-coord render of 0x800F2418.
    // Gated on mSceneTableTrusted (computed once, top of this function — see render.h for the writeup).
    if (mSceneTableTrusted) fieldEntityRender(0x800F2418u);
    // (c) the field's OBJECTS — walk the 3 entity lists, render each object's geomblk natively (real depth).
    for (int h = 0; h < 3; h++) {
      uint32_t n = c->mem_r32(HEADS[h]);
      for (int g = 0; n && g < 400; g++, n = c->mem_r32(n + 0x24)) {
        if (c->mem_r8(n + 1) == 0) continue;   // per-frame visibility marker (guest cull chain, cull.cpp)
        if (c->mem_r8(n + 8) == 0 || c->mem_r8(n + 9) == 0) continue;   // no render commands
        c->mRender->stats.snObjs++; c->mRender->stats.snCmds += c->mem_r8(n + 8);
        c->r[4] = n;
        perObjFlush();
      }
    }
    // (d) TOMBA (the master "G" block, ActorTomba::G_ADDR = 0x800E7E80) — verified RE (docs/port-
    // progress.md "KEY FINDING", game/player/actor_tomba.h): the player is NOT a member of any of the
    // 3 doubly-linked entity lists above (he is excluded from walkAll/walkList2/walkAux by design — his
    // per-frame tick runs off the per-area callback table instead, see area_seaside_perframe.cpp). The
    // generic entity-list walk above therefore never visits him and his geomblk render commands (the
    // SAME node+0xC0 array / node+8 count shape every other object uses — live-verified at the seaside
    // free-roam checkpoint: G+8=G+9=0x11, cmd ptrs 0x800F2740.. all valid) were never flushed — this was
    // the pc_render "Tomba invisible" bug. Submit him the same way as any HEADS-list node: read-only,
    // same generic perObjFlush path, no guest writes.
    {
      uint32_t g = ActorTomba::G_ADDR;
      if (c->mem_r8(g + 8) != 0 && c->mem_r8(g + 9) != 0) {
        c->mRender->stats.snObjs++; c->mRender->stats.snCmds += c->mem_r8(g + 8);
        c->r[4] = g;
        perObjFlush();
      }
    }
  }
  c->r[4] = saved;
  if (cfg_dbg("scenenative")) { int gpu_seen3d_this_frame(Core*); static int f = 0; if ((f++ % 60) == 0)
    fprintf(stderr, "[scenenative] objs=%ld cmds=%ld seen3d=%d\n", c->mRender->stats.snObjs, c->mRender->stats.snCmds, gpu_seen3d_this_frame(c)); }
}
// ===================================================================================================
// RETIRED 2026-07-07 (issue #32, USER: "PSX render path always active underneath; PC renderer
// shouldn't write to guest memory"): the native lifts of the walk cluster — perObjRender, bgRender,
// renderWalk (gen_func_8003C048), renderWalkSnapshot (8003BB50), rwalkAuxBcf4/Bf00/Eec0
// (8003BCF4/BF00/EEC0) and their per-type case tables. They re-ran the substrate walks' guest writes
// (queue swaps, node bookkeeping, guest renderer dispatches) from the display phase — a foreign call
// context whose guest-stack spills diverged from the recomp reference (SBS f26). The substrate
// orchestrator now executes the real walks underneath (Render::frame); this display pass is read-only.
// The RE'd case tables live in git history (this file @ commit 7989159) and docs/findings/render.md.
