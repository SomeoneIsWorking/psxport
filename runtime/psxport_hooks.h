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

/* Instant-CD: data reads/seeks at PC speed (CDDA/XA streaming stays native).
   Consumed by the imported cdc.c. */
extern int psxport_cd_instant;

/* CDC command/IRQ logging to stderr (PSXPORT_CDC_LOG=1). */
extern int psxport_cdc_log;

/* GTE transform tap: when psxport_gte_capture != 0, GTE_WriteCR forwards
   control-register writes 0..7 (rotation matrix CR0-4, translation CR5-7) to
   psxport_on_gte_cr. This is the per-object/camera transform used to project
   geometry — the basis for object-state capture and the reprojecting renderer. */
extern int psxport_gte_capture;
void psxport_on_gte_cr(unsigned which, uint32_t value);

/* Last emulated PC seen by the hook layer (watchdog diagnostics). */
extern uint32_t psxport_last_pc;

/* Host frame counter (set by the frontend; stamps debug logs). */
extern unsigned psxport_frame;

/* Live GPR file (32 GPRs + LO/HI), from the imported cpu.c. */
uint32_t* psxport_cpu_gpr(void);

/* Diagnostics: registers + heuristic MIPS stack trace (scans the emulated
   stack for plausible return addresses: RAM text address preceded by a
   jal/jalr). ram = 2MB main RAM. */
void psxport_dump_cpu_state(const uint8_t* ram);

#ifdef __cplusplus
} /* extern "C" */

/* C++ registration side (frontend / per-game modules). expected_instr = 0
   skips the signature check (use only for non-overlay addresses). */
void psxport_add_hook(uint32_t pc, uint32_t expected_instr, psxport_hook_fn fn);
void psxport_clear_hooks();

/* Register a consumer for GTE control-register writes (0..7). Setting a non-null
   fn enables capture; null disables. */
typedef void (*psxport_gte_cr_fn)(unsigned which, uint32_t value);
void psxport_set_gte_cr_hook(psxport_gte_cr_fn fn);
#endif

#endif
