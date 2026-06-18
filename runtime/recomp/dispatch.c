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
#include "r3000.h"

void rec_interp(R3000* c, uint32_t pc);
void rec_dispatch_miss(R3000* c, uint32_t addr);  // BIOS vectors -> HLE; else RAM code -> interp (hle.c)
void rec_set_interp_override(uint32_t addr, OverrideFn fn);
OverrideFn rec_interp_override_for(uint32_t a);   // unified address-keyed override lookup (interp.c)

// "Is this address a statically-recompiled function?" — used by interp.c's is_recompiled(). With no
// recompiled bodies the answer is always no; BIOS vectors (A0/B0/C0) are handled separately there.
int rec_func_index(uint32_t addr) { (void)addr; return -1; }

// Register a fixed native override for a function address (the resident-MAIN call sites). Same table
// as the overlay/auto overrides — there is no longer an index-keyed table to keep in sync.
void rec_set_override(uint32_t addr, OverrideFn fn) { rec_set_interp_override(addr, fn); }

// Run the function at `addr`: a native override wins; otherwise defer to rec_dispatch_miss, which
// routes BIOS vectors (A0/B0/C0) to the HLE BIOS and interprets RAM code (overlays / MAIN / stub).
// This mirrors the old generated rec_dispatch's `default: rec_dispatch_miss` path exactly — crucial
// for PSX BIOS calls (`li $t2,0xA0; jr $t2`), which must NOT be interpreted as code at 0xA0.
void rec_dispatch(R3000* c, uint32_t addr) {
  OverrideFn ov = rec_interp_override_for(addr);
  if (ov) { ov(c); return; }
  rec_dispatch_miss(c, addr);
}

// Super-call / A/B oracle: interpret the ORIGINAL function body, bypassing its own entry override (so
// a native override can invoke the real PSX code). rec_interp does not re-check the entry override, so
// this is just an interpret-from-addr. Replaces the old `gen_func_XXXX(c)` super-calls.
void rec_super_call(R3000* c, uint32_t addr) { rec_interp(c, addr); }

// The boot stub shared MAIN.EXE's address space and used to have its own recompiled dispatcher; it now
// interprets from RAM like everything else. Its overrides are registered via rec_set_interp_override
// (native_stub.c), so stub_set_override is a no-op kept only for the existing call sites.
void stub_dispatch(R3000* c, uint32_t addr) { rec_dispatch(c, addr); }
void stub_set_override(uint32_t addr, OverrideFn fn) { (void)addr; (void)fn; }
