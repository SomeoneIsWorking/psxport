#ifndef SCHEDULER_H
#define SCHEDULER_H
#include <stdint.h>
struct Core;
struct R3000;
#define TASKBASE 0x801fe000u   // task obj table base (slot i at +i*0x70)
#define TASKSTRIDE 0x70u
#define CUR_TASK 0x1f800138u   // DAT_1f800138: scheduler current-task ptr
void scheduler_yield(Core* c);              // FUN_80080880 ChangeThread override — the universal task-switch/yield primitive

// Enter a recompiled/overridden cooperative-task body at its top and run it until it returns or
// parks in a yield (dispatch.cpp). The task-slot stanzas start their fibers on this; so does
// PcScheduler's inline spawned-task runner.
void rec_coro_run(Core* c, uint32_t pc);

// Substrate task-slot stanzas (scheduler.cpp), called from PcScheduler::step when no PC-native
// stanza claims the slot. Each returns 1 when it processed the tick, 0 to fall through.
int recomp_run_coro_fiber_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                 int native_content, const R3000& loop);
int recomp_run_generic_dispatch_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                       int native_content, const R3000& loop);

// Native port of FUN_80051F14 — the guest task-spawn primitive. Writes the same slot fields the
// substrate does: base+0x0C = entry_pc, base+0x10 = caller's gp (r28), base+0x00 = 2 (RUNNABLE),
// base+0x04 = kBiosTcbHandle placeholder, base+0x6F = 0. No BIOS OpenTh syscall — the port's
// scheduler doesn't use BIOS TCBs; the handle value is written for guest-state parity only.
// After this call the scheduler will pick up the slot as a fresh RUNNABLE task on the next tick.
void native_task_spawn(Core* c, int slot, uint32_t entry_pc);
#endif
