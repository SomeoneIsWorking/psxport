// RenderObserver — READ-ONLY per-object depth tagging at the substrate override choke point.
//
// Since the render-underneath architecture (issue #32) the substrate walk cluster emits every
// object's GP0 packets itself; the retired native walk lifts were also the place that tagged each
// object's packet-pool span with its PC-native world depth (gpu_obj_depth_add), so their removal
// lost real-depth occlusion for guest-emitted 2D billboards (flames/ropes/collectables — the old
// issue-#4 class). This restores the tags WITHOUT touching guest state: a TRANSPARENT wrapper in
// the recomp override table (g_override — the same shared table both SBS cores consult) that
//   1. reads the object node from a0,
//   2. opens a nesting-safe PktSpanSession,
//   3. runs the LITERAL gen body (guest-byte behavior identical by construction),
//   4. tags the captured span with obj_world_ord(node) — HOST memory only.
// Observed fns: originally 0x8003CCA4 (the per-object render dispatch), 0x8003C2D4/0x8003C464/
// 0x8003C8F4 (billboard/particle-quad leaves) and the special-effect leaf renderers the master/aux
// walks call with a0=node. 2026-07-08: CCA4/C2D4/C464/C8F4 are now OWNED natively
// (game/render/perobj_billboard.cpp, Render::perObjRenderDispatch/billboardCompose1/2/billboardEmit) —
// each folds this SAME depth-tag wrap in directly, so they are no longer installed here. This file
// still wraps the remaining unowned siblings (C5F8, C788, 80039F4C).
// Extend the table below when a new billboard source shows up untagged (PSXPORT_DEBUG=objid).
#include "core.h"
#include "game.h"
#include "render.h"
#include "cfg.h"                // oracle_mode() — the observer is inert in the pure PSX oracle
#include "pkt_span.h"
#include "render_internal.h"   // obj_world_ord / gpu_obj_depth_add / fps60_bb_node

// OverrideFn is typedef'd in core.h. shard_set_override (generated/shard_disp.c) has C++ linkage.
void shard_set_override(uint32_t addr, OverrideFn fn);

extern void gen_func_8003C5F8(Core*);   // special-effect leaf renderers (walk types 16..19) — still substrate
extern void gen_func_8003C788(Core*);
extern void gen_func_80039F4C(Core*);   // type-4 multi-element object renderer
extern void gen_func_8007D594(Core*);   // generated/shard_4.c — dialog text-box state machine (its shared
                                        // tail unconditionally emits the PANEL: border tiles via
                                        // FUN_8007CC00, corner sprites + FT4 fills via FUN_8005019C/
                                        // FUN_8004FFB4 — docs/findings/ui.md "Dialog text-box PANEL
                                        // emitter chain", bug #34)

static void obs_body(Core* c, void (*gen)(Core*)) {
  // PSXPORT_ORACLE: the oracle is PURE PSX — run the literal substrate body untouched, add NO host depth
  // tag (obj_depth would only feed the native compositor, which oracle mode bypasses via painter order).
  if (c->game->oracle) { gen(c); return; }
  const uint32_t node = c->r[4];
  c->mRender->diag.beginObject(node);
  uint32_t slo, shi;
  PktSpanSession sess(c);
  gen(c);                                             // the substrate body, untouched
  if (sess.close(&slo, &shi)) {
    float od = obj_world_ord(c, node);
    gpu_obj_depth_add(c, slo, shi, od);
    fps60_bb_node(c, slo, shi, node);
  }
  c->mRender->diag.endObject();
}

#define OBS_WRAP(genfn) static void obs_##genfn(Core* c) { obs_body(c, genfn); }
OBS_WRAP(gen_func_8003C5F8)
OBS_WRAP(gen_func_8003C788)
OBS_WRAP(gen_func_80039F4C)
#undef OBS_WRAP

// UI-span observer wrap (bug #34): guest-transparent, SAME shape as obs_body above, but tags the
// captured span in the UI-span registry (gpu_ui_span_add) instead of obj_depth — the dialog panel is
// screen-space UI with no world depth and no fps60 billboard identity, so it needs its own
// presence-only provenance channel (gpu_native_internal.h ui_span_add/lookup) rather than
// obj_depth_add's world-position tag.
//
// DEVIATION from the original spec (which asked for engine_set_override_main, "like
// render_walk_dispatch.cpp:252"): engine_override_thunk.cpp gates its oracle branch on
// `c->game->psx_fallback || c->game->verify.inSubstrateLeg` — and per game.h ("recomp_path
// (psx_fallback=1, pc_skip ignored)"), a STANDALONE `PSXPORT_GATE=1` run (no SBS peer) also sets
// psx_fallback=1. Wiring through engine_set_override_main would therefore skip span registration on
// EVERY GATE run, not just true oracle/SBS-core-B — verified empirically: with that install, the
// panel stayed missing under `PSXPORT_GATE=1` (scratch/screenshots/bug34fix_gate_pc.png before this
// fix, identical to the broken bug44_ab_pc.png baseline; `PSXPORT_DEBUG=ovhit` showed native=0
// oracle=34 on 0x8007D594 in that standalone run). That breaks verification target A, which requires
// the panel to render under GATE+pc_render. obs_body's own gate — `c->game->oracle` — is narrower: it
// is 0 on a standalone GATE run and only set to 1 by Game::setOracle() (true oracle boots / SBS-full
// core B, sbs.cpp:1921), which is exactly the purity boundary this wrap needs. This is the SAME gate
// the 3 sibling wraps above already use in production for pc_render depth-tagging under GATE, so this
// wrap follows that proven, already-installed pattern instead.
static void obs_8007D594(Core* c) {
  if (c->game->oracle) { gen_func_8007D594(c); return; }   // true oracle / SBS core B: pure gen, no tag
  PktSpanSession sess(c);
  gen_func_8007D594(c);                               // the substrate body, untouched
  uint32_t slo, shi;
  if (sess.close(&slo, &shi)) gpu_ui_span_add(c, slo, shi);
}

// Install once per process (the override table is shared by both cores; the wrapper is guest-
// transparent, so SBS strictness is unaffected — MV_CHECK substrate legs run through it too).
void render_observer_install() {
  static bool done = false;
  if (done) return;
  done = true;
  shard_set_override(0x8003C5F8u, obs_gen_func_8003C5F8);
  shard_set_override(0x8003C788u, obs_gen_func_8003C788);
  shard_set_override(0x80039F4Cu, obs_gen_func_80039F4C);
  shard_set_override(0x8007D594u, obs_8007D594);
}
