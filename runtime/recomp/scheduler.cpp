// scheduler.cpp — the substrate half of the native cooperative-task-switch harness (replaces the
// PSX BIOS scheduler FUN_80051e60 and its ChangeThread primitive FUN_80080880). Generic platform
// mechanism: task-slot bookkeeping + the setjmp/longjmp (native path) and Coro-fiber (full-PSX
// path) task-switch primitives, plus the two substrate stanzas (coro-fiber + generic dispatch)
// for tasks the port does not own natively. The per-frame slot loop and the PC-native stanzas
// (DEMO/SOP/GAME/task-1/STAGE-0) live on PcScheduler (game/core/pc_scheduler.cpp), which calls
// the two stanzas exported here.
#include "core.h"
#include "game.h"      // PcScheduler (per-instance cooperative-task state) reached via c->game->pcSched
#include "scheduler.h"
#include "cfg.h"
#include "coro.h"      // thread-fiber for full-PSX mid-function resume (later-264)
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>

// Generic cooperative-task entry. POST-INTERPRETER (later-254): this is now just rec_dispatch
// (dispatch.cpp) — it can only ENTER a recompiled function at its top, NOT resume a yielded task at a
// saved mid-function PC. So it only works for a FRESH task entry; resuming a full-PSX (psx_fallback)
// task whose saved r31 is mid-body fail-fasts (no recompiled entry there). That is why the full-PSX
// reference modes (PSXPORT_SBS_MODE=gameplay/full core B) abort at the first scheduler yield-return
// (e.g. 0x80051FA4). The native path avoids this entirely: each stage runs as a synchronous per-frame
// native dispatcher (DEMO/GAME/SOP) and the GAME field re-enters at its loop top (game_coop), never a
// mid-function resume. See docs/findings/recomp.md "full-PSX coroutine resume".
void rec_coro_run(Core* c, uint32_t pc);

// --- Native cooperative scheduler (replaces FUN_80051e60) without ucontext ------------------
// Tomba2 runs up to 3 cooperative tasks (objs @0x801fe000, stride 0x70): task0 = the stage
// sequencer (START/DEMO/GAME), task1/2 = sub-tasks it spawns (asset loaders etc.). Each is an
// infinite/looping routine that yields via FUN_80051f80 once per frame. We run each as a
// resumable coroutine WITHOUT ucontext: a yield SAVES the PSX register context into the task's
// slot (the PSX stack lives in c->ram per-task at obj+8, untouched) and longjmps out of the
// interpreter call chain; we resume by restoring that context and continuing at the captured
// return address. This is the "later 29" design — a yield is a save/restore of a state struct,
// no native stack. PcScheduler::step mirrors FUN_80051e60: one pass over the 3 slots,
// running state==2 (runnable, resume) and state==3 (restart, fresh entry) tasks; the yield sets
// state=1 and FUN_800506d0 (called later in the frame) re-arms 1->2. This makes the cooperative
// handshakes work — e.g. task0's FUN_80044bd4 busy-waits (yielding) for DAT_1f80019b while the
// loader it spawned (task1) runs to completion across frames and sets the flag.
// (TASKBASE/TASKSTRIDE/CUR_TASK live in scheduler.h — shared with native_boot.cpp's REPL/debug probes.)

// The cooperative-scheduler state (yield jmp_buf, per-task saved R3000 regs, run flags) is per-instance:
// it lives on Game as class PcScheduler (game/core/pc_scheduler.h), reached via c->game->pcSched. A task
// context is ONLY the CPU register file — guest RAM/scratchpad/DMA/peripherals are SHARED one memory
// across all tasks (saving a whole Core would give each task its own RAM snapshot — the OOP regression
// where the loader task read a pre-fill file-table snapshot and stalled boot; see oop-regression-hunt).
// So task_ctx slices to the R3000 base on save/restore.

// FUN_80080880 ChangeThread override = the universal task-switch primitive. Every cooperative
// switch funnels through it: FUN_80051f80 (yield, state=1), FUN_80051fb4 (task end, state=0) and
// FUN_80052078 (stage transition, state=3) all set the task state then call ChangeThread to stop
// running. While running a task we capture the full register context for the slot and longjmp
// back to the scheduler; the task resumes (state 2 after FUN_800506d0 re-arms a yield, or fresh
// at state 3) or ends (state 0). Outside a task run (init / pre-stage FUN_800499e8) it is a
// no-op returning the handle, exactly as the stubbed thread layer did. The caller (FUN_80051f80
// etc.) has already run its real body, so register side effects it needs on resume (e.g. it
// leaves v0=0x1f800000 for the stage loop head's `lw t0,0x138(v0)`) are captured.
extern "C" void guest_backtrace_to(Core*, FILE*);   // sync_overrides.cpp — guest-stack backtrace (btyield)

// Native port of FUN_80051F14 (see scheduler.h). Verified via ram_menu Ghidra decomp + MAIN.EXE
// disas. Substrate body:
//   sp -= 24
//   s0 = task[slot] = 0x801FE000 + slot*0x70
//   sw s0, 16(sp)                     ← callee-save spill
//   sw ra, 20(sp)                     ← return-address spill
//   task[+0x0C] = entry_pc
//   gp = FUN_80080930()               (returns caller's gp)
//   task[+0x10] = gp
//   task[+0x00] = 2                   (RUNNABLE)
//   FUN_80080890()                    (EnterCriticalSection — syscall(0))
//   task[+0x6F] = 0
//   tcb = FUN_80080860(entry_pc, task[+0x08], gp)   (BIOS OpenTh — syscall B0:0x0E)
//   FUN_800808A0()                    (ExitCriticalSection — syscall(0))
//   task[+0x04] = tcb
//   lw ra, 20(sp); lw s0, 16(sp); sp += 24; jr ra
//
// Native port reproduces the observable guest writes:
//   - 24 B guest-stack allocation + sw s0/ra prologue writes (byte-faithful stack scratch)
//   - All task-slot writes at their offsets
//   - BIOS TCB placeholder 0xFF000000 (our port has no BIOS TCB table; the recomp shard's
//     FUN_80080860 override returns this constant).
// Syscalls EnterCriticalSection / ExitCriticalSection are no-ops in our port — the fiber scheduler
// doesn't use BIOS critical sections.
static constexpr uint32_t kBiosTcbHandlePlaceholder = 0xFF000000u;
void native_task_spawn(Core* c, int slot, uint32_t entry_pc) {
  const uint32_t base = c->cfg->taskTableBase + (uint32_t)slot * c->cfg->taskSlotStride;

  // NOTE: substrate FUN_80051F14 also does `sp -= 24; sw s0, 16(sp); sw ra, 20(sp)` — a prologue
  // that produces 12 bytes of stack scratch. Reproducing it in the port ONLY matches core B
  // byte-for-byte if the caller's sp equals the substrate caller's sp at this point (FUN_80044BD4's
  // frame). Since sync_preload_spawn doesn't descend the FUN_80044BD4 40-byte frame, adding the
  // FUN_80051F14 24-byte prologue here would land the writes at wrong absolute addresses. Match
  // is deferred until the full call-chain ports (FUN_80044BD4 + its callees) land — then each fn
  // in the chain writes at the correct sp. For now, only the task-slot writes fire (which are at
  // fixed absolute addresses — 0x801FE000+slot*0x70 — so they match unconditionally).
  c->mem_w32(base + 0x0C, entry_pc);
  c->mem_w32(base + 0x10, c->r[28]);          // FUN_80080930() returns caller's gp
  c->mem_w16(base + 0x00, 2);                  // RUNNABLE
  c->mem_w8 (base + 0x6F, 0);
  c->mem_w32(base + 0x04, kBiosTcbHandlePlaceholder);
}

void scheduler_yield(Core* c) {
  if (c->game && c->game->verify.inCheck) {   // MV_CHECK legs must be yield-free (verify_harness.h)
    fprintf(stderr, "[mirror-verify] FATAL: yield inside a strict-check leg — this mirror is not "
                    "yield-free; gate it with SBS lockstep instead of MV_CHECK.\n");
    abort();
  }
  if (!c->game->pcSched.in_stage) { c->r[2] = c->r[4]; return; }   // no-op: return the handle arg in v0
  cfg_logf("yieldpc", "switch yield ra=0x%08X waitloop=0x%08X r16=0x%08X r29=0x%08X 801fe0e0=0x%X",
           c->r[31], c->mem_r32(c->r[29] + 16), c->r[16], c->r[29], c->mem_r32(0x801fe0e0u));
  int slot = c->game->pcSched.cur_slot;
  c->game->pcSched.task_ctx[slot] = static_cast<R3000&>(*c);  // save REGISTERS only (r29=task SP, r31=resume ra)
  if (c->game->pcSched.cur_is_coro) {
    // FULL-PSX thread-fiber task. If the guest just ENDED this task (FUN_80051fb4 set state=0 then funneled
    // here), the body will never be resumed — unwind the fiber thread so its body returns and the Coro
    // finishes (else the thread blocks forever). Otherwise BLOCK the fiber (its whole C stack preserved);
    // the scheduler's co->resume() returns and we continue here on the next resume — mid-function.
    if (c->mem_r16(c->cfg->taskTableBase + (uint32_t)slot * c->cfg->taskSlotStride) == 0) {
      cfg_logf("sched", "   switch EXIT slot %d (base.state==0) ra=0x%08X",
               slot, c->r[31]);
      c->game->pcSched.coro[slot]->exit_now();
    }
    cfg_logf("yieldpc", "[sched]   switch YIELD slot %d ra=0x%08X", slot, c->r[31]);
    // btyield: dump the coro's guest call chain at the yield point (the live regs ARE the yielding
    // field-mode frame). Diagnoses WHERE a full-PSX task is parked (e.g. the deep field-mode sub-wait).
    if (cfg_dbg("btyield")) {
      cfg_logf("btyield", "slot %d ra=0x%08X r16=0x%08X r17=0x%08X r18=0x%08X r19=0x%08X r29=0x%08X",
               slot, c->r[31], c->r[16], c->r[17], c->r[18], c->r[19], c->r[29]);
      guest_backtrace_to(c, stderr);
      { void* bt[40]; int n = backtrace(bt, 40); cfg_logf("btyield", "C-stack (recomp func chain):");
        backtrace_symbols_fd(bt, n, 2); }
    }
    c->game->pcSched.coro[slot]->yield();
    return;
  }
  longjmp(c->game->pcSched.yield_jmp, 1);   // native path: unwind to the scheduler's setjmp
}

// ---- Substrate task-slot stanzas ------------------------------------------------------------------
// PcScheduler::step's main loop is a dispatch over per-entry-PC stanzas. Each stanza returns
// 1 (handled) when it processed the tick (caller does `continue` to the next slot), or 0
// (not mine) to fall through to the next stanza. The PC-native stanzas live on PcScheduler
// (game/core/pc_scheduler.cpp); the two below are the substrate fallbacks it calls.


// FULL-PSX (psx_fallback) task — thread-fiber coroutine. The substrate can't re-enter mid-fn, so
// each task runs on its own Coro thread that BLOCKS at a yield (preserving its C stack) and
// CONTINUES on resume — recompiler-only, no interpreter. cur_is_coro tells scheduler_yield to
// coro-yield vs longjmp. Active when psx_fallback is on (native_content==0) — OR, under
// pc_faithful (pc_skip=false, SBS gameplay/full mode on core A), when the task's entry PC has no
// native handler, so substrate-only wakes like task-1 preload (0x80044F58) execute on A same as
// core B and the FUN_80044BD4 spawn-and-wait cycle actually runs (dropping the completion-shim
// override in engine.cpp).
int recomp_run_coro_fiber_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                 int native_content, const R3000& loop) {
  if (native_content) {
    if (c->game->pc_skip) return 0;
    if (c->hooks->hasNativeHandlerForEntry(c, c->mem_r32(base + 0xc))) return 0;
  }
  Coro*& co = c->game->pcSched.coro[i];
  int co_fresh = (st == 3 || (st == 2 && !c->game->pcSched.task_started[i]));
  if (co_fresh) {
    if (co) { delete co; co = nullptr; }
    uint32_t entry = c->mem_r32(base + 0xc);
    c->game->pcSched.task_ctx[i] = loop;
    c->game->pcSched.task_ctx[i].r[29] = c->mem_r32(base + 8);
    c->game->pcSched.task_ctx[i].r[31] = 0xDEAD0000u;
    c->game->pcSched.task_started[i] = 1;
    c->game->pcSched.demo_native[i] = 0; c->game->pcSched.game_native[i] = 0; c->game->pcSched.game_coop[i] = 0;
    Core* cc = c;
    co = new Coro();
    co->start([cc, entry] { rec_coro_run(cc, entry); });
  } else if (st == 2 && co && !co->done()) {
    /* resume the suspended fiber (regs restored below) */
  } else if (st == 2) {
    c->game->pcSched.task_started[i] = 0;
    return 1;
  } else {
    return 1;                                                    // sleeping this frame (state==1)
  }
  c->mem_w16(base, 4);
  c->mem_w32(c->cfg->curTaskPtr, base);
  c->game->pcSched.cur_slot = i;
  c->game->pcSched.in_stage = 1;
  c->game->pcSched.cur_is_coro = 1;
  static_cast<R3000&>(*c) = c->game->pcSched.task_ctx[i];
  cfg_logf("sched", "slot %d coro %s st=%u entry=0x%08X sp=0x%08X", i,
           co_fresh ? "start" : "resume", st, c->mem_r32(base + 0xc), c->game->pcSched.task_ctx[i].r[29]);
  co->resume();
  c->game->pcSched.cur_is_coro = 0;
  c->game->pcSched.in_stage = 0;
  cfg_logf("sched", "   slot %d coro out: done=%d base.state=%u entry=0x%08X",
           i, co->done() ? 1 : 0, c->mem_r16(base), c->mem_r32(base + 0xc));
  if (co->done() || c->mem_r16(base) == 0) {
    c->mem_w16(base, 0);
    c->game->pcSched.task_started[i] = 0;
    delete co; co = nullptr;
  }
  return 1;
}

// Generic dispatch — the "everything else" path. Handles: fresh entry setup (state==3 or state==2
// with no live ctx); game_coop resume at 0x801063F4 (guest-loop re-entry with loop's callee-saved
// regs pinned); state==2 resume from saved r31. Inside the setjmp block, fresh entries for the
// two remaining native task entries (GAME stagePrologue and STAGE-0 startBinStage) fire before
// rec_coro_run. DEMO/SOP/GAME-per-frame entries were consumed by the PcScheduler stanzas.
int recomp_run_generic_dispatch_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                       int native_content, const R3000& loop) {
  uint32_t resume_pc;
  int fresh = 0;
  if (st == 3 || (st == 2 && !c->game->pcSched.task_started[i])) {
    resume_pc = c->mem_r32(base + 0xc);
    fresh = 1;
    c->game->pcSched.task_ctx[i] = loop;
    c->game->pcSched.task_ctx[i].r[29] = c->mem_r32(base + 8);
    c->game->pcSched.task_ctx[i].r[31] = 0xDEAD0000u;
    c->game->pcSched.task_started[i] = 1;
    c->game->pcSched.demo_native[i] = 0;
  } else if (st == 2 && c->game->pcSched.game_coop[i]
             && c->mem_r32(base + 0xc) == 0x8010637Cu) {
    // GAME cooperative re-entry at loop top 0x801063F4 with the callee-saved regs the loop
    // expects; sp pinned to task-top - 0x20 (verified 0x8010637C prologue frame size, later-284c
    // stack-leak fix) so the pre-prologue r29 leak doesn't ratchet task0's guest sp into the task
    // table across 100+ field frames.
    resume_pc = 0x801063F4u;
    c->game->pcSched.task_ctx[i].r[16] = 0x1f800000u;
    c->game->pcSched.task_ctx[i].r[17] = 0x1f800000u;
    c->game->pcSched.task_ctx[i].r[18] = 1;
    c->game->pcSched.task_ctx[i].r[31] = 0x801063F4u;
    c->game->pcSched.task_ctx[i].r[29] = c->mem_r32(base + 8) - 0x20u;
  } else if (st == 2) {
    c->game->pcSched.game_coop[i] = 0;                           // left GAME cooperative loop
    resume_pc = c->game->pcSched.task_ctx[i].r[31];
  } else {
    return 1;                                                    // sleeping this frame (state==1)
  }
  c->mem_w16(base, 4);
  cfg_logf("sched", "slot %d st_in=%u resume_pc=0x%08X ra=0x%08X sp=0x%08X",
           i, st, resume_pc, c->game->pcSched.task_ctx[i].r[31], c->game->pcSched.task_ctx[i].r[29]);
  c->mem_w32(c->cfg->curTaskPtr, base);
  c->game->pcSched.cur_slot = i;
  static_cast<R3000&>(*c) = c->game->pcSched.task_ctx[i];
  c->game->pcSched.in_stage = 1;
  if (setjmp(c->game->pcSched.yield_jmp) == 0) {
    uint32_t start = resume_pc;
    // Remaining fresh-entry native stage dispatches (DEMO's 0x801062E4 was handled by the DEMO stanza)
    // route through the game's schedFreshEntry hook (game_hooks.cpp): it runs the GAME stagePrologue
    // (stageMain, which leaves c->coro_redirect_pc = 0x801063F4) or the TERMINAL STAGE-0 startBinStage
    // by entryPc. The start/in_stage dance + the stage-0 finalize stay HERE (scheduler bookkeeping); only
    // the engine stage BODY and its entry-PC constants moved into the hook.
    if (native_content && fresh) {
      c->coro_redirect_pc = 0;   // clear before: only the hook's stageMain path sets it (safety vs a stale value)
      if (c->hooks->schedFreshEntry(c, i, base, resume_pc)) {
        // TERMINAL startBinStage: finalize the fresh stage-0 slot and end the tick (skip rec_coro_run).
        c->game->pcSched.stage0_step[i] = 0;
        c->game->pcSched.task_ctx[i] = static_cast<R3000&>(*c);
        c->mem_w16(base, 2);
        c->game->pcSched.in_stage = 0;
        return 1;
      }
      // stageMain left the redirect start in c->coro_redirect_pc; a non-stage fresh entry left it 0 (start
      // stays resume_pc). Reproduces `start = coro_redirect_pc ? coro_redirect_pc : r[31]` — stageMain
      // ALWAYS sets coro_redirect_pc (= 0x801063F4), so the original r[31] fallback branch was dead.
      if (c->coro_redirect_pc) { start = c->coro_redirect_pc; c->coro_redirect_pc = 0; }
    }
    c->game->ffspan.begin(); rec_coro_run(c, start); c->game->ffspan.end("coro");
    c->mem_w16(base, 0);                                          // task returned (jr ra sentinel)
    c->game->pcSched.task_started[i] = 0;
  }
  c->game->pcSched.in_stage = 0;
  return 1;
}
