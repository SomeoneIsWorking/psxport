// engine_select.h — WHICH ENGINE EXECUTES GUEST CODE, decided in exactly one place.
//
// A Core runs guest MIPS through one of several engines. This header owns the enum and the routing
// policy; nothing else may re-derive "which engine is this" from a flag.
//
// NAMESPACED, and that is not cosmetic. psxport is a library games link against, so the framework
// does not get to claim a bare global `Engine` — Tomba2Engine already has `class Engine`
// (game/core/engine.h), and an unnamespaced version of this header broke that build on contact.
// `psxport_smoke` proves the framework exports no GAME symbols; it cannot prove the reverse, so
// framework types belong in a namespace rather than relying on a name no game happens to want.
// Namespace follows the house convention (psx::config, psx::ui).
//
// HISTORY (I001): this replaced `int Core::use_interp`, a boolean branched on in dispatch.cpp (four
// entry points), guest_call.h (five call sites) and overlay_router.cpp. Two things were wrong with
// it. It could not name a third engine, which a JIT needs. And "any non-zero means interpreter"
// silently routes an engine it does not know into the shipping substrate — so "the new engine is
// selected" and "the new engine never ran" produce the same run, which is the failure mode a
// diagnostic must never have. route() is therefore TOTAL and REFUSES an unknown value.
#pragma once

#include <stdint.h>
#include <strings.h> // strcasecmp — engine_parse

namespace psx::exec {

// The engines a Core can execute guest code with. Substrate is 0 so the value matches the old
// `use_interp == 0` default byte-for-byte; nothing depends on that, but it makes the migration
// diffable.
enum class Engine : uint8_t {
  Substrate = 0,   // recompiled C (generated/shard_*.c) — the shipping native port
  Interpreter = 1, // the flat MIPS interpreter (interp.cpp) — the divergence-diagnosis engine
  Jit = 2,         // runtime translation; not implemented (see shared/jit-common)
  kCount           // MUST stay last: the denominator every exhaustive check counts against
};

// The shipping default. A Core nobody configured runs the substrate, exactly as before.
inline constexpr Engine kDefault = Engine::Substrate;

// Where a guest call goes. Distinct from Engine so a future engine that shares an existing
// execution arm does not have to lie about its identity.
enum class Route : uint8_t {
  Substrate,
  Interpreter,
  Jit,
  Refuse, // not an engine this build knows — fail fast, never guess
  kCount
};

// The one routing decision. Pure and constexpr so it is unit-testable without a Core, memory, or a
// disc, and so there is nowhere for a second copy of the policy to live.
inline constexpr Route route(Engine e) {
  switch (e) {
  case Engine::Substrate:
    return Route::Substrate;
  case Engine::Interpreter:
    return Route::Interpreter;
  case Engine::Jit:
    return Route::Jit;
  case Engine::kCount:
    break;
  }
  return Route::Refuse;
}

// Name for diagnostics and refusal messages. Never returns null, including for an unknown value — a
// null here would turn a refusal report into a crash, in the code whose whole job is to report.
inline constexpr const char *name(Engine e) {
  switch (e) {
  case Engine::Substrate:
    return "substrate";
  case Engine::Interpreter:
    return "interpreter";
  case Engine::Jit:
    return "jit";
  case Engine::kCount:
    break;
  }
  return "unknown";
}

// The valid spellings, in enum order, as ONE list. It is what a refusal prints and what the
// round-trip test counts against, so a new engine that forgets its name fails a test rather than
// becoming an unspellable engine nobody can select.
inline constexpr const char *kNames[] = {"substrate", "interpreter", "jit"};
inline constexpr int kNameCount = (int)(sizeof(kNames) / sizeof(kNames[0]));
static_assert(kNameCount == (int)Engine::kCount, "every Engine needs a spelling in kNames");

// Text -> Engine. Returns false and leaves *out untouched for anything it does not recognise,
// INCLUDING null and empty: an unrecognised engine name must never resolve to a plausible engine.
// Case-insensitive, matching the other text knobs (render_path_parse).
inline bool engine_parse(const char *s, Engine *out) {
  if (!s || !*s || !out) {
    return false;
  }
  for (int i = 0; i < kNameCount; ++i) {
    if (strcasecmp(s, kNames[i]) == 0) {
      *out = (Engine)i;
      return true;
    }
  }
  return false;
}

} // namespace psx::exec
