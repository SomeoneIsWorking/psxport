// oracle_shim.h — the INDEPENDENT reference emulator's control surface.
//
// WHY: `docs/plans/oracle-against-beetle.md`. Today's differential harness compares our `pc_faithful`
// path against our `recomp_path`; both are ours, so a shared wrong assumption reads as SUCCESS. USER,
// 2026-08-12: *"oracle compare should be done against a verified emulator like beetle imo, not our
// unverified 'faithful' path"*. This is milestone 1 of that plan: the vendored Mednafen PSX CPU, hosted
// WITHOUT `vendor/beetle-psx/libretro.c` (which drags in the disc, BIOS, video and option system), able
// to execute a window of GAME instructions out of a RAM image we inject.
//
// SCOPE, stated so it cannot be overclaimed: this steps instructions and exposes registers and RAM. It
// performs NO comparison against our port. A working `oracle_step()` proves nothing about the port until
// milestone 2 puts a real window through both.
#ifndef PSXPORT_ORACLE_SHIM_H
#define PSXPORT_ORACLE_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Why the window ended. Every reason is REPORTED, never silently absorbed: "the window ran to the end
// of its cycle budget" and "the window hit hardware at instruction 3" must never look the same to a
// caller, or a comparison over 0 useful instructions reads as a comparison that agreed.
typedef enum OracleStop {
  ORACLE_STOP_NONE = 0,       // not run yet
  ORACLE_STOP_BUDGET,         // clean: the requested cycle budget was consumed, no event, no fault
  ORACLE_STOP_HARDWARE,       // the code touched a device register — the straight-line window is over
  ORACLE_STOP_EVENT,          // a scheduled event came due (milestone 1 asserts this cannot happen)
} OracleStop;

typedef struct OracleState {
  uint32_t   gpr[32];         // r0..r31
  uint32_t   lo, hi;
  uint32_t   pc;              // BACKED_PC
  uint32_t   next_pc;         // BACKED_new_PC (the branch-delay successor)
  int32_t    timestamp;       // the core's own cycle count
  OracleStop stop;
  uint32_t   stop_addr;       // the offending address when stop == ORACLE_STOP_HARDWARE
} OracleState;

// Bring up main RAM + scratchpad and the CPU. Returns 0 and explains itself on failure; a failed init
// never leaves a half-initialised core behind for a caller to step. Idempotent.
int  oracle_init(void);
void oracle_teardown(void);

// Inject a PS-X EXE image: copy `len` bytes to guest `t_addr`, then set pc/gp/sp/fp. This is the plan's
// option 1 — neither side executes a BIOS boot, so our HLE'd BIOS and Beetle's real one never have to
// agree about kernel memory. Callers pass exactly what `crt0_plan` computed, so the two sides start from
// one derivation rather than two. Returns 0 (naming why) if the image would not fit in main RAM.
int oracle_load_exe(const void *image, uint32_t len, uint32_t t_addr,
                    uint32_t pc, uint32_t gp, uint32_t sp);

// Run until `cycles` of the core's own timestamp have elapsed, or until the window ends for one of the
// OracleStop reasons. Returns the stop reason; `oracle_capture` reads the resulting state out.
OracleStop oracle_run(int32_t cycles);
void       oracle_capture(OracleState *out);

// Direct RAM access, for injecting fixtures and for the eventual byte-compare.
uint8_t *oracle_main_ram(void);
uint32_t oracle_ram_size(void);

// Human-readable stop reason, for reports that must state WHY a window ended rather than only how long
// it was.
const char *oracle_stop_name(OracleStop s);

#ifdef __cplusplus
}
#endif
#endif
