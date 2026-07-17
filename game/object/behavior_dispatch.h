// class BehaviorDispatch — the engine's per-object BEHAVIOR-HANDLER dispatcher.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::behaviors`. Back-pointer wired
// once by Core's constructor (same pattern as Collision / Bit / Spawn). Callers reach it as
// `eng(c).behaviors.method(args)`.
//
// SCOPE: the single registry that maps a per-object HANDLER address to its native implementation
// (each `beh_*.cpp` behavior file exposes a `beh_*(Core*)` entry that's registered in the
// table). Owns the dispatcher entry point every field walk / transition machine uses to hand off
// one object's per-frame tick — the walker sets the current-object bookkeeping and picks native-
// vs-substrate. Was the free functions `dispatch_obj_method` / `dispatch_native_behavior` /
// `behavior_native_name` in engine_tomba2.cpp.
//
// The 50-entry behavior table is a file-local `static const` array in behavior_dispatch.cpp — read
// by every method here. Class doesn't need per-instance storage for it (same registry for every
// Core), but the class itself is per-instance so its methods can reach guest RAM via `this->core`.
#pragma once
#include <cstdint>
struct Core;

class BehaviorDispatch {
public:
  Core* core = nullptr;

  // dispatchObj(obj, handler): route ONE object's per-frame tick — set the fps60 current-object
  //   bookkeeping, then run either the native behavior (if registered in the table) or the recomp
  //   substrate leaf via rec_dispatch. Used by the field entity-list walkers (ObjectList / Array8
  //   Dispatch / TransitionState3) and any other per-object dispatcher.
  //   On the pure-substrate leg (c->game->psx_fallback — SBS core B — or c->game->verify.inSubstrateLeg
  //   — MV_CHECK's strict-mirror replay, game/core/verify_harness.h) OR under pc_faithful itself
  //   (!c->game->pc_skip) the native table is skipped entirely and every handler routes through
  //   rec_dispatch to the literal gen body — the same suppression rec_dispatch itself applies to
  //   the override registry (runtime/recomp/overlay_router.cpp, overrides::dispatch), PLUS the
  //   pc_skip fork: the native beh_*
  //   table is a pc_skip=true REBUILD shortcut (matches the RESULT, not the PSX bytes), so
  //   pc_faithful (which must be byte-exact to recomp_path) can't take it either.
  //   Called directly by native *Faithful() C++ methods (bypassing rec_dispatch), so it must carry
  //   its own copy of that gate rather than inheriting it.
  void dispatchObj(uint32_t obj, uint32_t handler);

  // dispatchNative(handler): table lookup + call. Returns true if the handler was owned natively
  //   (and ran), false if there's no native entry (caller must fall through to rec_dispatch). The
  //   behaviors read the object from c->r[4] which caller set — this class doesn't marshal it.
  bool dispatchNative(uint32_t handler);

  // nativeName(handler): the short slug for the native behavior at `handler`, or nullptr if the
  //   object's logic still runs as the recomp substrate. Used by the `ents` REPL diagnostic to
  //   flag which objects are owned.
  const char* nativeName(uint32_t handler) const;
};
