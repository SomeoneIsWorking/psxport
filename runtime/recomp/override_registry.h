// override_registry.h — the ONE registry for native engine/game overrides.
//
// A guest function at address X is reachable by TWO code paths the recompiler emits, and BOTH funnel
// through the same generated wrapper `func_X` (or `ov_<mod>_func_X`):
//
//   direct call:   recompiled body emits `func_X(c)`      -> func_X consults g_<mod>_override[]
//   rec_dispatch:  `rec_dispatch(c, X)` -> <mod>_dispatch  -> func_X(c) -> same table
//
//   void func_X(Core* c){ c->pc = X; if (g_override[i]) { g_override[i](c); return; } gen_func_X(c); }
//
// So installing ONE shared thunk into g_<mod>_override[] intercepts EVERY caller of X — there is no
// need to register an address in two places, and no need to own X's callers before X (a leaf engine
// is ownable on its own). This is the single registry that realises that: one entry per address
// carrying { native, gen }, one dispatch decision, two interception points that share it.
//
//   native = the PC-native handler (guest ABI: args in c->r[4..7], return in c->r[2]).
//   gen    = the recompiled substrate body (gen_func_X / ov_a00_gen_X / ov_game_gen_X).
//
// ORACLE PURITY (the crown-jewel invariant). The g_<mod>_override[] tables are PROCESS-GLOBAL, shared
// by every Core including SBS core B — the pure-substrate reference the native side is byte-compared
// against. The one dispatch decision therefore runs `gen` on the oracle leg (c->game->psx_fallback,
// or verify.inSubstrateLeg during a MIRROR_VERIFY substrate replay) and `native` everywhere else.
// Because that gate lives in ONE place it cannot be forgotten per-cluster — the class of bug where a
// trampoline that omitted its psx_fallback guard silently ran native on core B and turned SBS into a
// native-vs-native fake 0-diff.
//
//   Oracle-allowed primitives (the scheduler / platform-sync leaves that MUST run on both cores, or
//   the no-IRQ runtime hangs) are expressed the same way, with gen == native — there is no separate
//   mechanism and no special case.
//
// PlatformHle (async->sync + HLE BIOS vectors) is the ONE thing that legitimately installs into the
// raw g_override[] tables directly (via shard_set_override / ov_*_set_override) and is outside this
// registry — it is BIOS/hardware-sync HLE, not a guest-function override.
#pragma once
#include <cstdint>

struct Core;
typedef void (*OverrideFn)(Core*);

namespace overrides {

// A recompiler module's raw override installer — one of the generated shard_set_override /
// ov_<tag>_set_override functions (main, a00..a0l, crd, demo, opn, sop, start, game). Passed as a
// function pointer so the registry supports every overlay module without a hand-maintained enum.
typedef void (*Setter)(uint32_t, OverrideFn);

// Wire addr -> { native, gen }. `name` is the trace/ovhit label (may be nullptr -> the address is
// shown). Re-installing the same address with the SAME handlers is idempotent and silent
// (register_overrides() runs once per Game, and several sites self-guard). Re-installing it with
// DIFFERENT handlers ABORTS: two native owners of one address is never intended, the second silently
// wins, and the loser's work just disappears from the picture — it costs days because nothing else
// catches it (no build error, no warning, and SBS stays green since both write the same guest state).
// Give the address one owner that dispatches to both instead; game/render/mesh_emit_tap.cpp is the
// worked example. (USER 2026-07-23; the old wording said overwriting was fine — it was not.) Legacy:
// Game, twice under SBS full, into the one global table).
//
// `setter` selects how DIRECT `func_X(c)` callers are handled:
//   - a module set_override (shard_set_override / ov_<tag>_set_override): install the shared thunk into
//     that module's table, so direct callers hit the override too (not just rec_dispatch).
//   - nullptr: rec_dispatch interception ONLY. Direct `func_X(c)` callers fall through to the gen body
//     (substrate) unchanged — required where a direct call must stay on the substrate (e.g. an
//     intra-shard call the port deliberately leaves unhooked).
void install(uint32_t addr, const char* name, OverrideFn native, OverrideFn gen, Setter setter = nullptr);

// rec_dispatch interception point. Runs the entry (native or gen, per the oracle leg) and returns true
// if `addr` is registered; false lets rec_dispatch route normally. Intercepting here — rather than only
// at the g_<mod>_override[] wrapper — preserves the "an override fires even when its overlay image is
// not resident in guest RAM" property that the old EngineOverrides::run() provided (gen bodies are
// always linked, independent of overlay residency).
bool dispatch(Core* c, uint32_t addr);

// COVERAGE — how much of what we OWN did a run actually execute? Fills `total` with the number of
// registered addresses and `unreached` with how many were never dispatched on either core.
//
// This exists because a byte-compare gate reports only on code the run REACHES, and that is invisible
// in the result: a green SBS run over a boot window says nothing whatsoever about a native the window
// never enters. Tomba2Engine kanban #60 was a GUARANTEED core-A/core-B divergence (a native wrote a
// shared table 0x20 below where the gen body writes it) that sat behind a green 41,280-frame gate for
// as long as it existed, purely because those frames never executed that opcode. The per-address
// counts were already here, but only under PSXPORT_DEBUG=ovhit — a flag you have to already suspect
// something to set. Callers that report a clean compare should print this ALONGSIDE the verdict so
// "green" is self-qualifying, not a claim about the whole port.
void coverage(int* total, int* unreached);

}  // namespace overrides

// Thin module-named forwarders for the direct-install call sites (render emitters etc.) that already
// pass { native, gen }. Equivalent to overrides::install(addr, nullptr, native, gen, <mod>).
void engine_set_override_main(uint32_t addr, OverrideFn native, OverrideFn gen);
void engine_set_override_a00 (uint32_t addr, OverrideFn native, OverrideFn gen);
void engine_set_override_game(uint32_t addr, OverrideFn native, OverrideFn gen);
