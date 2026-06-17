// R3000A runtime model for the static-recompiled MAIN.EXE.
// The recompiler emits one C function per game function, all operating on a shared
// R3000 state + flat memory. This header is the ABI between generated code (sacrosanct)
// and the hand-written runtime (memory, dispatch, HLE BIOS, GTE).
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct R3000 {
  uint32_t r[32];   // GPRs; r[0] is hardwired 0 (generated code never writes it)
  uint32_t hi, lo;  // mult/div result registers
} R3000;

// ---- Memory (mem.c) -------------------------------------------------------
// PSX 2 MB main RAM, mirrored across KUSEG/KSEG0/KSEG1; 1 KB scratchpad.
extern uint8_t g_ram[0x200000];
extern uint8_t g_scratch[0x400];

uint8_t  mem_r8 (uint32_t a);
uint16_t mem_r16(uint32_t a);
uint32_t mem_r32(uint32_t a);
void     mem_w8 (uint32_t a, uint8_t  v);
void     mem_w16(uint32_t a, uint16_t v);
void     mem_w32(uint32_t a, uint32_t v);
// Unaligned word access (lwl/lwr/swl/swr): merge with the current register/memory.
uint32_t mem_lwl(uint32_t cur, uint32_t a);
uint32_t mem_lwr(uint32_t cur, uint32_t a);
void     mem_swl(uint32_t a, uint32_t v);
void     mem_swr(uint32_t a, uint32_t v);

// ---- Dispatch & traps -----------------------------------------------------
// Indirect call/jump target -> recompiled function (generated rec_dispatch),
// falling back to HLE/overlay routing for non-recompiled addresses.
void rec_dispatch(R3000* c, uint32_t addr);       // generated: address -> recompiled fn
void rec_dispatch_miss(R3000* c, uint32_t addr);  // runtime: BIOS/overlay/computed target

// Native overrides (recomp-overrides): register a hand-written native fn keyed by the
// original function's entry address. The generated wrapper func_<addr> runs the override
// if registered, else the recomp body gen_func_<addr> (extern that for super-calls).
typedef void (*OverrideFn)(R3000*);
void rec_set_override(uint32_t addr, OverrideFn fn);
// Override a NON-recompiled (interpreted) address — the boot stub / overlays. Keyed by raw
// address (rec_set_override is keyed by recompiled-function index, which stub code lacks).
void rec_set_interp_override(uint32_t addr, OverrideFn fn);
// Scan-on-load ownership of runtime-loaded overlay library fns whose addresses are reused per-scene:
// the loader calls rec_overlay_loaded(base,size) after copying an overlay in; that clears the prior
// scan's overrides and invokes the engine's load hook, which scans [base,base+size) for known
// library-fn signatures and registers each with rec_set_interp_override_auto(addr, fn).
void rec_set_interp_override_auto(uint32_t addr, OverrideFn fn);
void rec_set_overlay_load_hook(void (*fn)(uint32_t base, uint32_t size));
void rec_overlay_loaded(uint32_t base, uint32_t size);
int  rec_func_index(uint32_t addr);
void rec_syscall(R3000* c, uint32_t code);
void rec_break(R3000* c, uint32_t code);

// ---- COP0 (minimal) -------------------------------------------------------
uint32_t cop0_mfc(R3000* c, uint32_t reg);
void     cop0_mtc(R3000* c, uint32_t reg, uint32_t v);

// ---- COP2 / GTE -----------------------------------------------------------
uint32_t gte_read_data (uint32_t reg);
void     gte_write_data(uint32_t reg, uint32_t v);
uint32_t gte_read_ctrl (uint32_t reg);
void     gte_write_ctrl(uint32_t reg, uint32_t v);
void     gte_op(R3000* c, uint32_t insn);

// Signed-division helper with R3000 div-by-zero / overflow semantics (mem.c-adjacent
// in cpu_support.c) — kept as functions so the emitter stays a thin templater.
void cpu_div (R3000* c, uint32_t n, uint32_t d);
void cpu_divu(R3000* c, uint32_t n, uint32_t d);
