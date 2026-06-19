// Single-substrate dispatch (interpreter-only runtime).
//
// This replaces the generated dispatch infrastructure (generated/shard_disp.c + stub_disp.c). The
// runtime no longer links any statically-recompiled function bodies: MAIN.EXE and the boot stub both
// execute from g_ram via the interpreter (interp.c). The static recompiler (tools/recomp/emit.py ->
// generated/*.c) is retained ONLY as an offline analysis aid — readable, Ghidra-like pseudo-C used to
// reverse-engineer functions — and is not part of the build. See docs/journal.md "later-101".
//
// One execution model, one override table: rec_set_override / rec_set_interp_override both register
// into the address-keyed table owned by interp.c; rec_dispatch runs a native override if present, else
// interprets the original bytes. rec_super_call is the "run the real body" path an override uses for
// A/B oracle / super-call (the former gen_func_XXXX(c) pattern).
#include "core.h"

void rec_interp(Core* c, uint32_t pc);
void rec_dispatch_miss(Core* c, uint32_t addr);  // BIOS vectors -> HLE; else RAM code -> interp (hle.c)
void rec_set_interp_override(uint32_t addr, OverrideFn fn);
OverrideFn rec_interp_override_for(uint32_t a);   // unified address-keyed override lookup (interp.c)

// PSXPORT_SUBSTRATE (A/B build switch, top-down native port): when defined, the statically-recompiled
// function bodies (generated/shard_*.c + shard_disp.c) are LINKED and execute as compiled C — the
// no-interpreter substrate for the boot->cutscene path. In that build, shard_disp.c provides
// rec_func_index (real addr->index) and rec_dispatch (the addr->func_XXXX switch), and shard_set_override
// writes g_override[]. Here we own only the HYBRID rec_set_override (registers BOTH the interp address-
// keyed map AND g_override[], so an override is honored whether the caller is the interpreter OR a
// compiled gen body calling func_XXXX directly). Default build (undefined) = interpreter-only (every
// function runs via interp.c; rec_func_index always -1).
#ifndef PSXPORT_SUBSTRATE

// "Is this address a statically-recompiled function?" — interpreter-only build: never.
int rec_func_index(uint32_t addr) { (void)addr; return -1; }

// Register a fixed native override for a function address. One address-keyed table (interp.c).
void rec_set_override(uint32_t addr, OverrideFn fn) { rec_set_interp_override(addr, fn); }

// Run the function at `addr`: a native override wins; otherwise defer to rec_dispatch_miss, which
// routes BIOS vectors (A0/B0/C0) to the HLE BIOS and interprets RAM code (overlays / MAIN / stub).
// This mirrors the old generated rec_dispatch's `default: rec_dispatch_miss` path exactly — crucial
// for PSX BIOS calls (`li $t2,0xA0; jr $t2`), which must NOT be interpreted as code at 0xA0.
void rec_dispatch(Core* c, uint32_t addr) {
  OverrideFn ov = rec_interp_override_for(addr);
  if (ov) { ov(c); return; }
  rec_dispatch_miss(c, addr);
}

#else  // PSXPORT_SUBSTRATE: rec_func_index + rec_dispatch come from generated/shard_disp.c

void shard_set_override(uint32_t addr, OverrideFn fn);   // generated: g_override[rec_func_index(addr)] = fn

// Hybrid registration: keep both override tables in sync so a native override wins regardless of who
// calls the function — the interpreter (via rec_interp_override_for) OR a compiled body (via g_override[]
// inside func_XXXX). shard_set_override is a no-op for non-recompiled addrs (index < 0), so overlay/BIOS
// overrides still ride the interp map only.
void rec_set_override(uint32_t addr, OverrideFn fn) {
  rec_set_interp_override(addr, fn);
  shard_set_override(addr, fn);
}

#endif

// Super-call / A/B oracle: interpret the ORIGINAL function body, bypassing its own entry override (so
// a native override can invoke the real PSX code). rec_interp does not re-check the entry override, so
// this is just an interpret-from-addr. Replaces the old `gen_func_XXXX(c)` super-calls.
void rec_super_call(Core* c, uint32_t addr) { rec_interp(c, addr); }

// The boot stub shared MAIN.EXE's address space and used to have its own recompiled dispatcher; it now
// interprets from RAM like everything else. Its overrides are registered via rec_set_interp_override
// (native_stub.c), so stub_set_override is a no-op kept only for the existing call sites.
void stub_dispatch(Core* c, uint32_t addr) { rec_dispatch(c, addr); }
void stub_set_override(uint32_t addr, OverrideFn fn) { (void)addr; (void)fn; }
