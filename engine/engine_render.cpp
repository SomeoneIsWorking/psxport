// engine_render.cpp — PC-native per-frame RENDER ORCHESTRATION. (See engine_render.h.)
//
// TOP-DOWN render ownership. The native field per-frame update (engine_stage.cpp ov_field_frame) now calls
// ov_render_frame DIRECTLY (a plain C call) instead of rec_dispatching MAIN.EXE 0x8003f9a8. ov_render_frame
// mirrors that orchestrator's render passes; the per-object render-queue WALKS it drives already have
// PC-native bodies in engine_submit.cpp (ov_rwalk_aux_bf00, ov_rwalk_aux_eec0, ov_render_walk_snapshot,
// ov_rwalk_aux_bcf4) — written under the old override model but ORPHANED since the override table was
// removed. Owning the orchestrator natively is the contiguous parent that lets us WIRE those orphan
// walkers back into the LIVE field render (they attach each object's PC-native WORLD-POSITION depth via
// gpu_obj_depth_add, so render ordering is engine-owned from real world coords, not the PSX OT).
//
// The render-queue walks run through their native bodies; the non-walk passes (0x8004fd30, 0x80025d98,
// 0x8003d0bc, 0x8003f024, 0x8003df04, 0x8003c048) and the diagnostic-only 0x8003b588 (no real native)
// stay PSX (rec_dispatch). VERIFIED (later-225): the seaside walkable field renders correctly in
// widescreen — the world-coord depth path drives the engine-owned render extent.
//
// RE (MAIN.EXE, tools/disas.py):
//   0x8003f9a8 render orchestrator: jal 0x8004fd30, 0x80025d98, 0x8003bf00, 0x8003eec0, 0x8003b588,
//     0x8003bb50, 0x8003bcf4, 0x8003d0bc(a0=0x800f2418), 0x8003f024, 0x8003df04, 0x8003c048.
//   0x8003fa44 (transition twin): jal 0x8004fd30, 0x80025d98, 0x8003bf00, 0x8003eec0, 0x8003b588,
//     0x8003bb50, 0x8003bcf4, 0x8003c048, 0x8003f024.

#include "engine_render.h"
#include "core.h"
#include "cfg.h"
#include <stdlib.h>
void ov_field_entity_render(Core* c);  // engine_submit.cpp — native world-space GT3/GT4 scene-table render

// Native render-queue walkers (engine_submit.cpp). void(Core*); each drains its queue/list, dispatches
// every live object's per-type renderer (still-PSX content), and tags the produced packet span with the
// object's PC-native world depth.
void ov_rwalk_aux_bf00(Core* c);       // 0x8003bf00
void ov_rwalk_aux_eec0(Core* c);       // 0x8003eec0
void ov_render_walk_snapshot(Core* c); // 0x8003bb50
void ov_rwalk_aux_bcf4(Core* c);       // 0x8003bcf4
void ov_render_walk(Core* c);          // 0x8003c048 (master phase-2 list walk; routes terrain to ov_terrain)
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

// 0x8003f9a8 — per-frame render orchestrator (11 passes). The render-queue walks (0x8003bf00/eec0/bb50/
// bcf4) run through their PC-native bodies (engine_submit.cpp), which attach each object's PC-native
// world-position depth — engine-owned render ordering from real world coords. 0x8003b588 has no real
// native (only a diagnostic counter), and the non-walk passes stay PSX, so both rec_dispatch.
void ov_render_frame(Core* c) {
  if (g_render_psx) { d0(c, 0x8003f9a8u); return; }   // COMPARE: render the field via the PSX recomp path
  d0(c, 0x8004fd30u);
  d0(c, 0x80025d98u);                // STILL-PSX: the 2D ATLAS backdrop (sky/sea sprites, op-0x65, builder
                                     // 0x8007e6dc; node 0x800BFEB8 is the visible sea — confirmed by WWATCH, later-237)
  ov_rwalk_aux_bf00(c);              // 0x8003bf00
  ov_rwalk_aux_eec0(c);             // 0x8003eec0
  d0(c, 0x8003b588u);                // STILL-PSX: REAL 3D WATER (GTE per-object path 0x8003cdd8, node 800C0B04; later-236)
  ov_render_walk_snapshot(c);        // 0x8003bb50
  ov_rwalk_aux_bcf4(c);              // 0x8003bcf4
  ov_ground_probe(c);                // DIAG groundprobe: decode the ground scene table (no draw; later-235)
  // DIAG groundnative: route the ground table real-depth via ov_field_entity_render. Decode is CORRECT, but
  // the 2D sea/water backdrop then composites OVER it (later-235 render-ordering blocker) — OFF by default.
  if (cfg_dbg("groundnative")) { c->r[4] = 0x800f2418u; ov_field_entity_render(c); }
  else d1(c, 0x8003d0bcu, 0x800f2418u); // STILL-PSX: emits ~220 GP0 field prims = the GROUND (later-229)
  d0(c, 0x8003f024u);
  d0(c, 0x8003df04u);
  ov_render_walk(c);                  // 0x8003c048 (native — terrain renders world-coord via ov_terrain)
}

// 0x8003fa44 — mid-transition render orchestrator twin (reduced pass set, same native walks).
void ov_render_frame_x(Core* c) {
  if (g_render_psx) { d0(c, 0x8003fa44u); return; }   // COMPARE: render the field via the PSX recomp path
  d0(c, 0x8004fd30u);
  d0(c, 0x80025d98u);
  ov_rwalk_aux_bf00(c);              // 0x8003bf00
  ov_rwalk_aux_eec0(c);             // 0x8003eec0
  d0(c, 0x8003b588u);
  ov_render_walk_snapshot(c);        // 0x8003bb50
  ov_rwalk_aux_bcf4(c);              // 0x8003bcf4
  ov_render_walk(c);                  // 0x8003c048 (native — terrain world-coord)
  d0(c, 0x8003f024u);
}
