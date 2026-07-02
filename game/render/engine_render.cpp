// engine_render.cpp — PC-native per-frame RENDER ORCHESTRATION. (See engine_render.h.)
//
// TOP-DOWN render ownership. The native field per-frame update (engine_stage.cpp ov_field_frame) calls
// ov_render_frame DIRECTLY (a plain C call) instead of rec_dispatching MAIN.EXE 0x8003f9a8. The render-queue
// WALK cluster (0x8003bf00/eec0/bb50/bcf4/b588/c048 — ov_rwalk_aux_bf00, ov_rwalk_aux_eec0, ov_rwalk_b588,
// ov_render_walk_snapshot, ov_rwalk_aux_bcf4, ov_render_walk) used to also run from HERE, but that duplicated
// the SAME walk cluster ov_scene_native (game/render/engine_render_walk.cpp) already runs every field-stage
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

#include "engine_render.h"
#include "render.h"    // class Render — methods live here
#include "core.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
void ov_field_entity_render(Core* c);  // engine_submit.cpp — native world-space GT3/GT4 scene-table render
                                        // (used by the groundnative diagnostic branch below)

void ov_ground_probe(Core* c);         // DIAG: decode ground scene table 0x800F2418 (later-234 blocker)

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
extern "C" { int g_render_psx = 0; }
// DUAL-VIEW: render ONE game state two ways side by side (engine-native left | PSX-recomp right). Set at
// launch (PSXPORT_DUALVIEW). The second (PSX) render pass is driven by native_step_frame; this flag gates
// the GPU's two-batch allocation + side-by-side present. (REPL `dualview on|off` mirrors PSXPORT_DUALVIEW.)
extern "C" { int g_dualview = 0; }

// 0x8003f9a8 — per-frame render orchestrator. The render-queue WALK passes (0x8003bf00/eec0/b588/bb50/bcf4/
// c048) are owned SOLELY by ov_scene_native (engine_render_walk.cpp), which ov_draw_otag already runs every
// field-stage frame — running them again here would double-submit every terrain/object prim. This function
// now performs only the remaining non-walk PSX passes ov_scene_native does not cover.
extern "C" void ffspan_begin(void), ffspan_end(const char*);   // PSXPORT_BDTAG attribution (engine_stage.cpp)
void Render::frame() { Core* c = mCore;
  if (cfg_dbg("rfprobe")) { static int n=0; if ((n++ % 60)==0) fprintf(::stderr,"[rfprobe] ov_render_frame run #%d\n", n); }
  if (g_render_psx) { d0(c, 0x8003f9a8u); return; }   // COMPARE: render the field via the PSX recomp path
  ffspan_begin(); d0(c, 0x8004fd30u); ffspan_end("rf_4fd30");
  ffspan_begin(); d0(c, 0x80025d98u); ffspan_end("rf_25d98");   // 2D atlas SPRITE band (op-0x65)
  ov_ground_probe(c);                // DIAG groundprobe: decode the ground scene table (no draw; later-235)
  // DIAG groundnative: route the ground table real-depth via ov_field_entity_render. Decode is CORRECT, but
  // the 2D sea/water backdrop then composites OVER it (later-235 render-ordering blocker) — OFF by default.
  if (cfg_dbg("groundnative")) { c->r[4] = 0x800f2418u; ov_field_entity_render(c); }
  else { ffspan_begin(); d1(c, 0x8003d0bcu, 0x800f2418u); ffspan_end("rf_ground"); } // STILL-PSX GROUND (later-229)
  ffspan_begin(); d0(c, 0x8003f024u); ffspan_end("rf_3f024");
  ffspan_begin(); d0(c, 0x8003df04u); ffspan_end("rf_3df04");
}

// 0x8003fa44 — mid-transition render orchestrator twin (reduced pass set). The walk cluster is owned by
// ov_scene_native (see ov_render_frame above); only the non-walk passes remain here.
void Render::frameX() { Core* c = mCore;
  if (g_render_psx) { d0(c, 0x8003fa44u); return; }   // COMPARE: render the field via the PSX recomp path
  d0(c, 0x8004fd30u);
  d0(c, 0x80025d98u);
  d0(c, 0x8003f024u);
}
