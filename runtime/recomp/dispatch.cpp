// Dispatch entry points — the ONE place that routes a guest call to an execution engine.
//
// Which engine a Core uses is Core::engine (engine_select.h owns the enum and the routing policy).
// Substrate is the shipping native port: the static recompiler (tools/recomp/emit.py ->
// generated/shard_*.c) runs every ordinary game function as recompiled C. Interpreter is the SBS
// oracle Core's reference path. Jit is not implemented yet and refuses loudly rather than silently
// falling back — see route_guest_call().
//
// Explicit hardware/data owners are dispatched before the interpreter path; ordinary recompiler
// misses still fail fast through rec_dispatch_miss so missing coverage stays visible.
//
// I001: the four entry points below used to carry a copy each of `if (c->use_interp) … else …`, and
// guest_call.h carried five more. There is now one decision, here.
#include "core.h"
#include "engine_select.h"
#include "override_registry.h"
#include <lucent/log.h>
#include <stdlib.h>

void rec_dispatch(Core *c, uint32_t addr);      // global router (overlay_router.cpp): range-routes
                                                // to main_dispatch / the resident overlay's switch
void rec_dispatch_miss(Core *c, uint32_t addr); // BIOS vectors -> HLE; else FAIL FAST (hle.cpp)

// g_override_tgt retired 2026-07-03 — per-Core Core::override_tgt.

// ORACLE engine (later-278, docs/oracle.md): when c->use_interp is set this Core is the pure-PSX
// interpreter oracle — route these entry points to the interpreter (interp.cpp) instead of the recomp
// substrate. The native port Core (use_interp==0) keeps the substrate fast path unchanged.
void interp_run(Core *c, uint32_t addr);      // interp.cpp — run addr to completion (nested call)
void interp_coro_run(Core *c, uint32_t addr); // interp.cpp — cooperative-task entry

// THE routing owner. Every guest call arrives here. `interp_entry` is the interpreter flavour the
// caller wants (nested-call vs cooperative-task); the substrate has no such distinction because a
// recompiled body is a plain C call either way.
static void route_guest_call(Core *c, uint32_t addr, void (*interp_entry)(Core *, uint32_t)) {
  using namespace psx::exec;
  switch (route(c->engine)) {
  case Route::Substrate:
    rec_dispatch(c, addr);
    return;
  case Route::Interpreter:
    if (overrides::dispatchOracle(c, addr)) {
      return;
    }
    interp_entry(c, addr);
    return;
  case Route::Jit:
    // Not implemented (shared/jit-common). REFUSE rather than fall back to the substrate: a silent
    // fallback makes "the JIT ran" and "the JIT never ran" the same run, which is the exact defect
    // the engine enum replaced a boolean to avoid.
    lucent::error("engine", "Core::engine = {} but no JIT backend is built in (guest {:08X})", name(c->engine), addr);
    abort();
  case Route::Refuse:
  case Route::kCount:
    break;
  }
  lucent::error("engine",
                "Core::engine = {} ({}) is not an engine this build knows (guest {:08X})",
                (int)c->engine,
                name(c->engine),
                addr);
  abort();
}

// Entry points — under the substrate they are all the same thing: run the recompiled body at `addr`
// as a plain C call (rec_dispatch resolves the func_<addr> wrapper; a miss aborts in
// rec_dispatch_miss). rec_super_call was "interpret the original body" (the old gen_func_XXXX(c)
// super-call); rec_coro_run is the cooperative-task entry.
void rec_super_call(Core *c, uint32_t addr) {
  route_guest_call(c, addr, interp_run);
}
void rec_interp(Core *c, uint32_t addr) {
  route_guest_call(c, addr, interp_run);
}
void rec_coro_run(Core *c, uint32_t addr) {
  route_guest_call(c, addr, interp_coro_run);
}

// Cooperative-yield redirect handshake (later-169): an override stashed the PC the flat interpreter
// should resume at. Consumed by interp.cpp's flat loop (interpreter engine); on a substrate Core there is no
// resumable mid-function PSX PC, so it is inert there.
void rec_coro_redirect(Core *c, uint32_t target) {
  c->coro_redirect_pc = target;
}

// The boot stub shares MAIN.EXE's address space (decoded natively now); route any stray dispatch
// through the same engine as everything else.
void stub_dispatch(Core *c, uint32_t addr) {
  route_guest_call(c, addr, interp_run);
}

// NOTE: interp_trace_open / prof_start / prof_stop / prof_dump are now provided by interp.cpp (the
// interpreter engine is compiled back in for the oracle), so the old no-op stubs here are removed.
