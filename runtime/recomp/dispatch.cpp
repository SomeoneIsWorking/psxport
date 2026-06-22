// Single-substrate dispatch (interpreter-only runtime).
//
// This replaces the generated dispatch infrastructure (generated/shard_disp.c + stub_disp.c). The
// runtime no longer links any statically-recompiled function bodies: MAIN.EXE and the boot stub both
// execute from g_ram via the interpreter (interp.c). The static recompiler (tools/recomp/emit.py ->
// generated/*.c) is retained ONLY as an offline analysis aid — readable, Ghidra-like pseudo-C used to
// reverse-engineer functions — and is not part of the build. See docs/journal.md "later-101".
//
// One execution model: rec_dispatch interprets the original bytes (BIOS vectors -> HLE). The override
// table is GONE (2026-06-22) — PC calls native code directly. rec_super_call is the "run the real body"
// path (the former gen_func_XXXX(c) pattern).
#include "core.h"

void rec_interp(Core* c, uint32_t pc);
void rec_dispatch_miss(Core* c, uint32_t addr);  // BIOS vectors -> HLE; else RAM code -> interp (hle.c)

// Interpreter-only build: there is no statically-recompiled function table; every function runs via
// interp.c, so rec_func_index is always -1.
// "Is this address a statically-recompiled function?" — interpreter-only build: never.
int rec_func_index(uint32_t addr) { (void)addr; return -1; }

// OVERRIDE SYSTEM REMOVED (2026-06-22) — top-down PC-driven model: PC calls PC directly; PSX never
// calls PC. There is no override table any more — native code is invoked by PC calling the native
// function directly, NOT by registering an address. rec_dispatch is the PC->PSX-leaf call: it runs the
// original recomp body (pure PSX) — BIOS vectors to HLE, RAM code interpreted — and never re-enters
// native code.
//
// Run the PSX function at `addr` as pure recomp: rec_dispatch_miss routes BIOS vectors (A0/B0/C0) to the
// HLE BIOS and interprets RAM code (overlays / MAIN / stub). Crucial for PSX BIOS calls (`li $t2,0xA0;
// jr $t2`), which must NOT be interpreted as code at 0xA0.
void rec_dispatch(Core* c, uint32_t addr) {
  rec_dispatch_miss(c, addr);
}

// Super-call / A/B oracle: interpret the ORIGINAL function body, bypassing its own entry override (so
// a native override can invoke the real PSX code). rec_interp does not re-check the entry override, so
// this is just an interpret-from-addr. Replaces the old `gen_func_XXXX(c)` super-calls.
void rec_super_call(Core* c, uint32_t addr) { rec_interp(c, addr); }

// The boot stub shared MAIN.EXE's address space and used to have its own recompiled dispatcher; it now
// interprets from RAM like everything else.
void stub_dispatch(Core* c, uint32_t addr) { rec_dispatch(c, addr); }
