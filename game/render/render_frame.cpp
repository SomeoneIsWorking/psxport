// render_frame.cpp — PC-native per-frame RENDER ORCHESTRATION.
//
// TOP-DOWN render ownership. The native field per-frame update (engine.cpp ov_field_frame) calls
// ov_render_frame DIRECTLY (a plain C call) instead of rec_dispatching MAIN.EXE 0x8003f9a8. The render-queue
// WALK cluster (0x8003bf00/eec0/bb50/bcf4/b588/c048 — ov_rwalk_aux_bf00, ov_rwalk_aux_eec0, ov_rwalk_b588,
// ov_render_walk_snapshot, ov_rwalk_aux_bcf4, ov_render_walk) used to also run from HERE, but that duplicated
// the SAME walk cluster ov_scene_native (game/render/render_walk.cpp) already runs every field-stage
// frame via ov_draw_otag (game_tomba2.cpp) — every terrain/scene-table/object prim was projected and
// submitted TWICE per frame, the root cause of the documented render-order/sliver bugs. The walk cluster is
// now owned SOLELY by ov_scene_native; ov_render_frame / ov_render_frame_x below perform only the remaining
// non-walk PSX passes (display atlas, ground-table fallback, backdrop state machine, and the 2D atlas/sprite
// bands) that ov_scene_native does not cover.
//
// RE (MAIN.EXE, tools/disas.py):
//   0x8003f9a8 render orchestrator: jal 0x8004fd30, 0x80025d98, 0x8003bf00, 0x8003eec0, 0x8003b588,
//     0x8003bb50, 0x8003bcf4, 0x8003d0bc(a0=0x800f2418), 0x8003f024, 0x8003df04, 0x8003c048.
//   0x8003fa44 (transition twin): jal 0x8004fd30, 0x80025d98, 0x8003bf00, 0x8003eec0, 0x8003b588,
//     0x8003bb50, 0x8003bcf4, 0x8003c048, 0x8003f024.

#include "render.h"    // class Render — methods live here
#include "core.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>

// DIAG skippass: PSXPORT_SKIPPASS=0xADDR skips that one rec_dispatch'd render pass, to attribute a prim to
// the pass producing it. NOTE: useless for PERSISTENT packets (built once at scene-load, re-walked from the
// OT every frame) — skipping the pass mid-run can't un-link an already-built packet; use PSXPORT_WWATCH on
// the packet address to find the real builder instead (later-237).
static inline void d0(Core* c, uint32_t fn) {
  static uint32_t skip = 0xFFFFFFFFu;
  if (skip == 0xFFFFFFFFu) { const char* s = cfg_str("PSXPORT_SKIPPASS"); skip = s ? (uint32_t)strtoul(s, 0, 0) : 0; }
  if (skip && fn == skip) return;
  rec_dispatch(c, fn);
}
static inline void d1(Core* c, uint32_t fn, uint32_t a0) { c->r[4] = a0; rec_dispatch(c, fn); }

// RENDER-PATH COMPARE SWITCH (diagnostic, user 2026-06-24). When set, the FIELD render runs entirely as
// the PSX recomp path (rec_dispatch the orchestrator) instead of the native world-coord path — WITHOUT
// touching the native frame loop / game state, so the SAME deterministic guest state can be rendered both
// ways and diffed (native must match PSX under 1x / 4:3 / 30fps). Set by PSXPORT_RENDER_PSX / REPL
// `renderpsx on|off`. This is a verification instrument, NOT a shipped behavior toggle.
// g_render_psx REMOVED (2026-07-02, deglobalize-game): now Render::mPsxRender / psxRender().
// g_dualview  REMOVED (2026-07-02, deglobalize-game): now Render::mDualview / dualview().

// 0x8003f9a8 — per-frame render orchestrator. The render-queue WALK passes (0x8003bf00/eec0/b588/bb50/bcf4/
// c048) are owned SOLELY by ov_scene_native (render_walk.cpp), which ov_draw_otag already runs every
// field-stage frame — running them again here would double-submit every terrain/object prim. This function
// now performs only the remaining non-walk PSX passes ov_scene_native does not cover.
#include "game.h"      // c->game->ffspan — PSXPORT_BDTAG builder-span attribution
// THE PSX RENDER PATH ALWAYS EXECUTES UNDERNEATH (USER 2026-07-07, issue #32): the substrate render
// orchestrator runs in BOTH render modes, on the faithful task's own guest call path, so every guest
// write it makes (packet pool, OT links, walk-queue cursors, node bookkeeping, stack scratch) is
// byte-identical to the recomp reference — rendering can never cause an SBS divergence. pc_render vs
// psx_render differ ONLY in where the PICTURE comes from (drawOTag): the native read-only passes +
// 2D-overlay OT walk vs the full OT walk. The old pc_render fork here ran a PARTIAL pass list and left
// the walk cluster to native re-implementations in the display phase (sceneNative) — a different call
// context than the recomp's, which is exactly what diverged (f26, guest-stack spills at a foreign sp).
void Render::frame() { Core* c = mCore;
  if (cfg_dbg("rfprobe")) { static int n=0; if ((n++ % 60)==0) cfg_logf("rfprobe", "ov_render_frame run #%d", n); }
  d0(c, 0x8003f9a8u);
}

// 0x8003fa44 — mid-transition render orchestrator twin (reduced pass set). Same rule: substrate always.
void Render::frameX() { Core* c = mCore;
  d0(c, 0x8003fa44u);
}
