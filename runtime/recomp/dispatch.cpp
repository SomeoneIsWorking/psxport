// Dispatch entry points for the recomp-substrate runtime.
//
// The interpreter is GONE (2026-06-30). The static recompiler (tools/recomp/emit.py ->
// generated/shard_*.c) is the SUBSTRATE: every non-native guest function runs as recompiled C.
// shard_disp.c GENERATES rec_dispatch (an address->func_<addr> switch) and rec_func_index; this file
// only provides the remaining dispatch entry points the engine/runtime call (rec_super_call /
// rec_interp / rec_coro_run / stub_dispatch), routing them all to the generated rec_dispatch — a
// recompiled body is invoked as a plain C call. A MISS (overlay code, a non-recompiled address, a
// computed jump target) falls through rec_dispatch_miss, which FAILS FAST (abort + guest backtrace).
// There is NO interpreter fallback. (User directive 2026-06-30: drop the interpreter; every recomp
// miss must crash with a log so we can see what to port/recompile next.)
#include "core.h"

void rec_dispatch(Core* c, uint32_t addr);        // global router (overlay_router.cpp): range-routes
                                                  // to main_dispatch / the resident overlay's switch
void rec_dispatch_miss(Core* c, uint32_t addr);   // BIOS vectors -> HLE; else FAIL FAST (hle.cpp)

// g_override_tgt retired 2026-07-03 — per-Core Core::override_tgt.

// ORACLE engine (later-278, docs/oracle.md): when c->use_interp is set this Core is the pure-PSX
// interpreter oracle — route these entry points to the interpreter (interp.cpp) instead of the recomp
// substrate. The native port Core (use_interp==0) keeps the substrate fast path unchanged.
void interp_run(Core* c, uint32_t addr);        // interp.cpp — run addr to completion (nested call)
void interp_coro_run(Core* c, uint32_t addr);   // interp.cpp — cooperative-task entry

// Former interpreter entry points — under the substrate they are all the same thing: run the
// recompiled body at `addr` as a plain C call (rec_dispatch resolves the func_<addr> wrapper; a miss
// aborts in rec_dispatch_miss). rec_super_call was "interpret the original body" (the old
// gen_func_XXXX(c) super-call); rec_coro_run was a cooperative-task entry.
void rec_super_call(Core* c, uint32_t addr) { if (c->use_interp) { interp_run(c, addr); return; } rec_dispatch(c, addr); }
void rec_interp(Core* c, uint32_t addr)     { if (c->use_interp) { interp_run(c, addr); return; } rec_dispatch(c, addr); }
void rec_coro_run(Core* c, uint32_t addr)   { if (c->use_interp) { interp_coro_run(c, addr); return; } rec_dispatch(c, addr); }

// Cooperative-yield redirect handshake (later-169): an override stashed the PC the flat interpreter
// should resume at. Consumed by interp.cpp's flat loop (oracle Core); on a substrate Core there is no
// resumable mid-function PSX PC, so it is inert there.
void rec_coro_redirect(Core* c, uint32_t target) { c->coro_redirect_pc = target; }

// The boot stub shares MAIN.EXE's address space (decoded natively now); route any stray dispatch
// through the same substrate (interpreter when this is the oracle Core).
void stub_dispatch(Core* c, uint32_t addr) { if (c->use_interp) { interp_run(c, addr); return; } rec_dispatch(c, addr); }

// NOTE: interp_trace_open / prof_start / prof_stop / prof_dump are now provided by interp.cpp (the
// interpreter engine is compiled back in for the oracle), so the old no-op stubs here are removed.
