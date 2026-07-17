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
// still wraps the remaining unowned siblings (C5F8, C788); 0x80039F4C is owned by Render::textLabelEmit (text_label.cpp).
// Extend the table below when a new billboard source shows up untagged (PSXPORT_DEBUG=objid).
#include "core.h"
#include "game.h"
#include "render.h"
#include "cfg.h"                // oracle_mode() — the observer is inert in the pure PSX oracle
#include "pkt_span.h"
#include "render_internal.h"   // obj_world_ord / gpu_obj_depth_add

// OverrideFn is typedef'd in core.h. shard_set_override (generated/shard_disp.c) has C++ linkage.
void shard_set_override(uint32_t addr, OverrideFn fn);

extern void gen_func_8003C5F8(Core*);   // special-effect leaf renderers (walk types 16..19) — still substrate
extern void gen_func_8003C788(Core*);

static void obs_body(Core* c, void (*gen)(Core*)) {
  // PSXPORT_ORACLE: the oracle is PURE PSX — run the literal substrate body untouched, add NO host depth
  // tag (obj_depth would only feed the native compositor, which oracle mode bypasses via painter order).
  if (c->game->oracle) { gen(c); return; }
  const uint32_t node = c->r[4];
  c->rsub.diag.beginObject(node);
  uint32_t slo, shi;
  PktSpanSession sess(c);
  gen(c);                                             // the substrate body, untouched
  if (sess.close(&slo, &shi)) {
    float od = obj_world_ord(c, node);
    gpu_obj_depth_add(c, slo, shi, od);
  }
  c->rsub.diag.endObject();
}

#define OBS_WRAP(genfn) static void obs_##genfn(Core* c) { obs_body(c, genfn); }
OBS_WRAP(gen_func_8003C5F8)
OBS_WRAP(gen_func_8003C788)
#undef OBS_WRAP

// (The ui_span observer wrap that used to sit here on 0x8007D594 was removed 2026-07-16,
// break-first-then-rebuild: the dialog panel's pc_render picture is now the native
// Panel::pushFill/pushCorners/pushDialogGlyphs taps, so the span-provenance shim had no consumer.)

// Install once per process (the override table is shared by both cores; the wrapper is guest-
// transparent, so SBS strictness is unaffected — MV_CHECK substrate legs run through it too).
void render_observer_install() {
  static bool done = false;
  if (done) return;
  done = true;
  shard_set_override(0x8003C5F8u, obs_gen_func_8003C5F8);
  shard_set_override(0x8003C788u, obs_gen_func_8003C788);
}
