// psxport hook & override layer (C API: included by the imported mednafen C
// sources). Hooks fire by PC inside the CPU interpreter; each hook carries an
// expected instruction word so overlay-swapped code at the same address can
// never misfire (same principle as the conditional RAM pokes).
//
// A hook returning PSXPORT_HOOK_REDIRECT acts as a native override: the
// interpreter skips the original code and resumes at *redirect_pc (typically
// the caller, GPR[31], with results placed in GPR by the hook).

#ifndef PSXPORT_HOOKS_H
#define PSXPORT_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSXPORT_HOOK_CONTINUE 0 /* run the original instruction normally */
#define PSXPORT_HOOK_REDIRECT 1 /* skip original code, jump to *redirect_pc */

/* gpr = s_cpu.GPR_full (32 GPRs + LO/HI at 32/33). */
typedef int (*psxport_hook_fn)(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc);

/* Nonzero while any hooks are registered; the interpreter's fast-path gate. */
extern uint32_t psxport_hook_count;

/* Called from the interpreter for every instruction while hooks exist.
   instr = fetched instruction word (signature check). */
int psxport_on_pc(uint32_t pc, uint32_t instr, uint32_t* gpr, uint32_t* redirect_pc);

/* PC coverage (RE aid): while enabled, every executed PC in RAM is marked in
   a 512Kbit bitmap (one bit per word address). */
extern uint8_t* psxport_cov_bitmap; /* NULL = disabled */

#ifdef __cplusplus
} /* extern "C" */

/* C++ registration side (frontend / per-game modules). expected_instr = 0
   skips the signature check (use only for non-overlay addresses). */
void psxport_add_hook(uint32_t pc, uint32_t expected_instr, psxport_hook_fn fn);
void psxport_clear_hooks();
#endif

#endif
