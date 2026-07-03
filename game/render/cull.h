// engine/cull.h — PC-native visibility CULL / LOD subsystem.
//
// class Cull — instance subsystem owned by Engine. The engine OWNS the per-object visibility decision
// (per CLAUDE.md: render ordering / visibility is the engine's, with its own widescreen-aware margin;
// no PSX ±34° cone inheritance). Holds the per-object cull body (FUN_8007712C), its two camera-
// relative wrappers (FUN_8007778C / FUN_80077ACC), and the standalone view-cone cull (FUN_8002B278).
//
// Currently ORPHANED — the top-down PC path hasn't reached the cull site yet. The class shape is here
// so callers can hook in as `c->engine.cull.method()` once wired; the algorithmic body preserves the
// RE'd cull-decide + widescreen margin re-include logic.
#pragma once
#include <cstdint>
class Core;

class Cull {
public:
  Core* core = nullptr;

  // objectCull (FUN_8007712C): per-object cull body + widescreen margin re-include. Taxi-parameter
  // c->r[4] = object, c->r[5]/[6]/[7] = camera-relative dx/dz/dy (s16 each). Was ov_object_cull.
  void objectCull();

  // performBaseCull — the byte-exact PC-native reimplementation of FUN_8007712C's body ALONE (no
  // widescreen margin re-include). Called from Actor::boundsCull to replace the last rec_dispatch
  // in the bounds-cull chain — same taxi convention as objectCull (r[4]=obj, r[5]/[6]/[7]=dx/dy/dz),
  // same side effects (obj[+1] flag + per-class render-list push), same return in r[2]. Was the
  // file-scope `cull_native_body` helper — now the public entry point Actor::boundsCull dispatches to.
  void performBaseCull();

  // cullWrapper (FUN_8007778C): camera-relative delta + flag reset → cull body. Taxi c->r[4] = object.
  // Was ov_cull_wrapper.
  void cullWrapper();

  // cullWrap77acc (FUN_80077ACC): cull-wrapper variant, caller-supplied position in r5/r6/r7 (flags
  // 1/4 rather than 0/0). Was ov_cull_wrap_77acc.
  void cullWrap77acc();

  // coneCull2b278 (FUN_8002B278): standalone view-cone cull, sets node visible flag on keep. Taxi
  // c->r[4] = node. Was ov_cone_cull_2b278.
  void coneCull2b278();

  // enqueueQueueA (FUN_80077E7C): MANUAL push of `obj` onto queue A (0x1F80013C/0x1F800144, cap 24) —
  // the render queue for object types 2/9 that cull_decide would auto-push. Six callers across
  // game/world/entity.cpp + game/ai/beh_* use this as an unconditional queue insert (early bail if
  // count >= 24; no visibility check). Body from disas 0x80077E7C.
  //
  // Returns v0 as the recomp does — on cap-hit v0=0 (the slti's false result); on success v0 =
  // old_counter + 1 (i.e. the NEW 1-based count). beh_jumptable_release_trigger uses this return.
  uint32_t enqueueQueueA(uint32_t obj);

  // enqueueVisibleClass4 (FUN_80077EBC): MANUAL push of `obj` onto render class 4's list — the same
  // list-add tail performBaseCull runs when the base cull KEEPS a class-4 object, but callable
  // directly by beh_ handlers whose scene-specific logic decides an object should render this frame
  // (bypassing the base cull test). Respects the cap-40 limit at *(0x1F800150). Callers set obj[+1]=1
  // themselves; this method only manipulates the queue. Was rec_dispatch(0x80077EBCu) in 5+ handlers.
  void enqueueVisibleClass4(uint32_t obj);
};
