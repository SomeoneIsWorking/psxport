// scheduler.cpp — the native cooperative-task-switch harness (replaces the PSX BIOS scheduler
// FUN_80051e60 and its ChangeThread primitive FUN_80080880). Generic platform mechanism: task-slot
// bookkeeping + the setjmp/longjmp (native path) and Coro-fiber (full-PSX path) task-switch
// primitives. The per-stage DISPATCH this steps (DEMO/SOP/GAME) is game logic reached through plain
// forward-declared calls, not hardcoded here beyond the entry-address literals used to detect which
// stage a slot is running (native_scheduler_step is still address-keyed — see the file's own
// comments — but the switch/yield primitive itself is pure platform).
#include "core.h"
#include "game.h"      // SchedulerState (per-instance cooperative-task state) reached via c->game->sched
#include "scheduler.h"
#include "cfg.h"
#include "coro.h"      // thread-fiber for full-PSX mid-function resume (later-264)
#include "c_subsys.h"  // xa_stream_owns_slot2/xa_stream_voice_busy/xa_stream_voice_release
#include <setjmp.h>
#include <stdio.h>
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
// no native stack. native_scheduler_step mirrors FUN_80051e60: one pass over the 3 slots,
// running state==2 (runnable, resume) and state==3 (restart, fresh entry) tasks; the yield sets
// state=1 and FUN_800506d0 (called later in the frame) re-arms 1->2. This makes the cooperative
// handshakes work — e.g. task0's FUN_80044bd4 busy-waits (yielding) for DAT_1f80019b while the
// loader it spawned (task1) runs to completion across frames and sets the flag.
// (TASKBASE/TASKSTRIDE/CUR_TASK live in scheduler.h — shared with native_boot.cpp's REPL/debug probes.)

// The cooperative-scheduler state (yield jmp_buf, per-task saved R3000 regs, run flags) is per-instance
// now: it lives on Game as SchedulerState (game.h), reached via c->game->sched. A task context is ONLY
// the CPU register file — guest RAM/scratchpad/DMA/peripherals are SHARED one memory across all tasks
// (saving a whole Core would give each task its own RAM snapshot — the OOP regression where the loader
// task read a pre-fill file-table snapshot and stalled boot; see oop-regression-hunt). So task_ctx
// slices to the R3000 base on save/restore.

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

// Native port of FUN_80051F14 (see scheduler.h). Verified via ram_menu Ghidra decomp: the substrate
// writes entry, gp, state=2, tcb, +0x6F=0 to the slot base at 0x801FE000 + slot*0x70. BIOS OpenTh
// (syscall B0:0x0E) is a no-op in our port — the port scheduler runs guest tasks via its own
// setjmp/fiber machinery, not BIOS TCBs — but the observed placeholder handle 0xFF000000 is written
// so any guest code inspecting task+0x04 sees the same value.
static constexpr uint32_t kTaskTableBaseGuest = 0x801FE000u;
static constexpr uint32_t kTaskSlotStrideGuest = 0x70u;
static constexpr uint32_t kBiosTcbHandlePlaceholder = 0xFF000000u;
void native_task_spawn(Core* c, int slot, uint32_t entry_pc) {
  const uint32_t base = kTaskTableBaseGuest + (uint32_t)slot * kTaskSlotStrideGuest;
  c->mem_w32(base + 0x0C, entry_pc);
  c->mem_w32(base + 0x10, c->r[28]);                 // caller's gp — same as FUN_80080930() returns
  c->mem_w16(base + 0x00, 2);                         // RUNNABLE — scheduler picks up next tick
  c->mem_w32(base + 0x04, kBiosTcbHandlePlaceholder);
  c->mem_w8 (base + 0x6F, 0);
}

void scheduler_yield(Core* c) {
  if (!c->game->sched.in_stage) { c->r[2] = c->r[4]; return; }   // no-op: return the handle arg in v0
  if (cfg_dbg("yieldpc")) fprintf(stderr, "[yieldpc] switch yield ra=0x%08X waitloop=0x%08X r16=0x%08X r29=0x%08X 801fe0e0=0x%X\n",
                                  c->r[31], c->mem_r32(c->r[29] + 16), c->r[16], c->r[29], c->mem_r32(0x801fe0e0u));
  int slot = c->game->sched.cur_slot;
  c->game->sched.task_ctx[slot] = static_cast<R3000&>(*c);  // save REGISTERS only (r29=task SP, r31=resume ra)
  if (c->game->sched.cur_is_coro) {
    // FULL-PSX thread-fiber task. If the guest just ENDED this task (FUN_80051fb4 set state=0 then funneled
    // here), the body will never be resumed — unwind the fiber thread so its body returns and the Coro
    // finishes (else the thread blocks forever). Otherwise BLOCK the fiber (its whole C stack preserved);
    // the scheduler's co->resume() returns and we continue here on the next resume — mid-function.
    if (c->mem_r16(TASKBASE + (uint32_t)slot * TASKSTRIDE) == 0) {
      if (cfg_dbg("sched")) fprintf(stderr, "[sched]   switch EXIT slot %d (base.state==0) ra=0x%08X\n",
                                    slot, c->r[31]);
      c->game->sched.coro[slot]->exit_now();
    }
    if (cfg_dbg("yieldpc")) fprintf(stderr, "[sched]   switch YIELD slot %d ra=0x%08X\n", slot, c->r[31]);
    // btyield: dump the coro's guest call chain at the yield point (the live regs ARE the yielding
    // field-mode frame). Diagnoses WHERE a full-PSX task is parked (e.g. the deep field-mode sub-wait).
    if (cfg_dbg("btyield")) {
      fprintf(stderr, "[btyield] slot %d ra=0x%08X r16=0x%08X r17=0x%08X r18=0x%08X r19=0x%08X r29=0x%08X\n",
              slot, c->r[31], c->r[16], c->r[17], c->r[18], c->r[19], c->r[29]);
      guest_backtrace_to(c, stderr);
      { void* bt[40]; int n = backtrace(bt, 40); fprintf(stderr, "[btyield] C-stack (recomp func chain):\n");
        backtrace_symbols_fd(bt, n, 2); }
    }
    c->game->sched.coro[slot]->yield();
    return;
  }
  longjmp(c->game->sched.yield_jmp, 1);   // native path: unwind to the scheduler's setjmp
}

// ---- Per-stanza task-slot processors -------------------------------------------------------------
// native_scheduler_step's main loop is a dispatch over per-entry-PC stanzas. Each stanza returns
// STANZA_HANDLED when it processed the tick (caller does `continue` to the next slot), or
// STANZA_NOT_MINE to fall through to the next stanza. Extracted from the historically-scattered
// scheduler main loop so the entry-PC dispatch surface is O(N-stanzas) rather than O(11-checks).

enum StanzaResult { STANZA_NOT_MINE = 0, STANZA_HANDLED = 1 };
extern "C" void ffspan_reset_frame(void), ffspan_begin(Core*), ffspan_end(Core*, const char*);  // PSXPORT_BDTAG (engine_stage.cpp)

// DEMO 0x801062E4 — native per-frame dispatcher (no guest coroutine). Fresh: stageMain (prologue)
// then frame() dispatches sm[0x48]==0 substate. Resume: frame(). Slip #1 leave-defer delays the
// sm[0x48]==5 leave-to-GAME dispatch by one tick to match the substrate coro's FUN_80051f80
// yield cost. Entry rewrite (LEAVE-DEMO -> GAME): drop demo_native, task_started=0, state=3
// stands so the next tick enters the generic path with the new entry.
static StanzaResult run_demo_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                    int native_content, const R3000& loop) {
  int demo_fresh = native_content && (st == 3 || (st == 2 && !c->game->sched.task_started[i]))
                   && c->mem_r32(base + 0xc) == 0x801062E4u;
  if (!demo_fresh && !(c->game->sched.demo_native[i] && st == 2 && c->game->sched.task_started[i]))
    return STANZA_NOT_MINE;
  if (demo_fresh) {
    c->game->sched.task_ctx[i] = loop;                          // inherit gp
    c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);    // per-task PSX stack top
    c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
    c->game->sched.task_started[i] = 1;
    c->game->sched.demo_native[i] = 1;
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  c->game->sched.cur_slot = i;
  static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];
  c->game->sched.in_stage = 1;
  if (setjmp(c->game->sched.yield_jmp) == 0) {
    if (demo_fresh) c->engine.demo.stageMain();                 // prologue only (sm[0x48]=0)
    uint16_t sm48v = c->mem_r16(c->mem_r32(0x1f800138u) + 0x48);
    bool defer_leave = (sm48v == 5 && !demo_fresh
                        && c->game->sched.demo_leave_step[i] == 0);
    if (defer_leave) c->game->sched.demo_leave_step[i] = 1;     // consumed on the next tick
    else {
      c->engine.demo.frame();                                   // dispatches sm[0x48] substate
      if (sm48v == 5) c->game->sched.demo_leave_step[i] = 0;    // re-arm after LEAVE completes
    }
  } else if (cfg_dbg("demo")) {
    static int w = 0; if (!w++) fprintf(stderr, "[demo] caught a substate yield (async CD not yet "
                                                "owned native+sync) — frontier\n");
  }
  c->game->sched.in_stage = 0;
  if (c->mem_r32(base + 0xc) != 0x801062E4u) {                  // s5 -> GAME rewrote entry
    c->game->sched.demo_native[i] = 0;
    c->game->sched.task_started[i] = 0;
    return STANZA_HANDLED;
  }
  c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);
  c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// SOP area-load 0x80109164 — SOP.BIN's cooperative slot-1 loader run synchronously (all leaves are
// sync CD reads). Fresh-only: run areaLoad, mark task done. With psx_fallback on the recomp body
// runs as a normal cooperative task via the fiber stanza below (its FUN_80051fb4 yield is serviced).
static StanzaResult run_sop_area_load_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                             int native_content, const R3000& loop) {
  int sop_fresh = (st == 3 || (st == 2 && !c->game->sched.task_started[i]))
                  && c->mem_r32(base + 0xc) == 0x80109164u;
  if (!(sop_fresh && native_content)) return STANZA_NOT_MINE;
  c->game->sched.task_ctx[i] = loop;
  c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);
  c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  c->game->sched.cur_slot = i;
  static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];
  c->game->sched.in_stage = 1;
  if (setjmp(c->game->sched.yield_jmp) == 0) {
    c->engine.sop.areaLoad();
  } else if (cfg_dbg("sched")) {
    fprintf(stderr, "[sched] SOP area-load yielded unexpectedly — a leaf isn't sync yet\n");
  }
  c->game->sched.in_stage = 0;
  c->mem_w16(base, 0);
  c->game->sched.task_started[i] = 0;
  return STANZA_HANDLED;
}

// GAME 0x8010637C — native per-frame dispatcher. Fresh: stagePrologue + frame(). Resume: frame().
// frame() returns 0 when its current sm[0x48] state isn't owned natively — fall back to game_coop:
// hand the task to the guest cooperative loop 0x801063F4 with the loop's callee-saved regs reset,
// so the generic path drives it next tick. Entry rewrite (area transition): drop game_native.
static StanzaResult run_game_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                    int native_content, const R3000& loop) {
  int game_fresh = (st == 3 || (st == 2 && !c->game->sched.task_started[i]))
                   && c->mem_r32(base + 0xc) == 0x8010637Cu;
  if (!native_content && c->game->sched.game_native[i]) {       // psx_fallback toggled -> clear stale
    c->game->sched.game_native[i] = 0;
    c->game->sched.task_started[i] = 0;
  }
  if (!native_content) return STANZA_NOT_MINE;
  if (!game_fresh && !(c->game->sched.game_native[i] && st == 2 && c->game->sched.task_started[i]))
    return STANZA_NOT_MINE;
  if (game_fresh) {
    c->game->sched.task_ctx[i] = loop;
    c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);
    c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
    c->game->sched.task_started[i] = 1;
    c->game->sched.game_native[i] = 1;
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  c->game->sched.cur_slot = i;
  static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];
  c->game->sched.in_stage = 1;
  int handled = 1;
  if (setjmp(c->game->sched.yield_jmp) == 0) {
    if (game_fresh) c->engine.stagePrologue();
    ffspan_begin(c); handled = c->engine.frame(); ffspan_end(c, "gameframe");
  } else if (cfg_dbg("sched")) {
    static int w = 0; if (!w++) fprintf(stderr, "[sched] caught a GAME substate yield (a leaf not "
                                                "yet sync) — frontier\n");
  }
  c->game->sched.in_stage = 0;
  if (c->mem_r32(base + 0xc) != 0x8010637Cu) {                  // area transition rewrote entry
    c->game->sched.game_native[i] = 0;
    c->game->sched.task_started[i] = 0;
    return STANZA_HANDLED;
  }
  if (!handled) {                                                // fall back to guest cooperative loop
    c->r[16] = 0x1f800000u; c->r[17] = 0x1f800000u; c->r[18] = 1;
    c->r[31] = 0x801063F4u;
    c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);
    c->game->sched.game_native[i] = 0;
    c->game->sched.game_coop[i] = 1;
    c->mem_w16(base, 2);
    if (cfg_dbg("sched")) fprintf(stderr, "[sched] GAME -> cooperative guest loop (state not yet "
                                           "owned native; field reachable)\n");
    return STANZA_HANDLED;
  }
  c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);
  c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// Entry PCs the native stanzas handle. task-1's substrate wake (0x80044F58) etc. are NOT in this
// set, so under mIsFaithful they route through the Coro fiber path so their yielding substrate
// bodies run on core-A too. Keep in sync with the run_*_stanza guards above.
//
// STAGE-0 (0x8010649C) is deliberately excluded under mIsFaithful: the substrate body pushes ~115
// B of stack scratch via libgs LoadImage/DrawSync callee frames that the native startBinStage
// can't reproduce. Under mIsFaithful, letting STAGE-0 route to fiber makes A run the exact same
// substrate code B does — byte-identical stack scratch by construction, at the cost of skipping
// A's native fresh entry (which reappears once mIsFaithful is off / SBS is not gameplay mode).
static bool has_native_handler_for_entry(Core* c, uint32_t entry_pc) {
  if (entry_pc == 0x801062E4u) return true;   // DEMO
  if (entry_pc == 0x80109164u) return true;   // SOP area-load
  if (entry_pc == 0x8010637Cu) return true;   // GAME
  if (entry_pc == 0x8010649Cu) return !c->game->mIsFaithful;   // STAGE-0 → fiber under faithful
  return false;
}

// FULL-PSX (psx_fallback) task — thread-fiber coroutine. The substrate can't re-enter mid-fn, so
// each task runs on its own Coro thread that BLOCKS at a yield (preserving its C stack) and
// CONTINUES on resume — recompiler-only, no interpreter. cur_is_coro tells scheduler_yield to
// coro-yield vs longjmp. Active when psx_fallback is on (native_content==0) — OR, under
// mIsFaithful (SBS gameplay/full mode on core A), when the task's entry PC has no native handler,
// so substrate-only wakes like task-1 preload (0x80044F58) execute on A same as core B and the
// FUN_80044BD4 spawn-and-wait cycle actually runs (dropping the completion-shim override in
// engine_stage.cpp).
static StanzaResult run_coro_fiber_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                          int native_content, const R3000& loop) {
  if (native_content) {
    if (!c->game->mIsFaithful) return STANZA_NOT_MINE;
    if (has_native_handler_for_entry(c, c->mem_r32(base + 0xc))) return STANZA_NOT_MINE;
  }
  Coro*& co = c->game->sched.coro[i];
  int co_fresh = (st == 3 || (st == 2 && !c->game->sched.task_started[i]));
  if (co_fresh) {
    if (co) { delete co; co = nullptr; }
    uint32_t entry = c->mem_r32(base + 0xc);
    c->game->sched.task_ctx[i] = loop;
    c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);
    c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
    c->game->sched.task_started[i] = 1;
    c->game->sched.demo_native[i] = 0; c->game->sched.game_native[i] = 0; c->game->sched.game_coop[i] = 0;
    Core* cc = c;
    co = new Coro();
    co->start([cc, entry] { rec_coro_run(cc, entry); });
  } else if (st == 2 && co && !co->done()) {
    /* resume the suspended fiber (regs restored below) */
  } else if (st == 2) {
    c->game->sched.task_started[i] = 0;
    return STANZA_HANDLED;
  } else {
    return STANZA_HANDLED;                                       // sleeping this frame (state==1)
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  c->game->sched.cur_slot = i;
  c->game->sched.in_stage = 1;
  c->game->sched.cur_is_coro = 1;
  static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];
  if (cfg_dbg("sched"))
    fprintf(stderr, "[sched] slot %d coro %s st=%u entry=0x%08X sp=0x%08X\n", i,
            co_fresh ? "start" : "resume", st, c->mem_r32(base + 0xc), c->game->sched.task_ctx[i].r[29]);
  co->resume();
  c->game->sched.cur_is_coro = 0;
  c->game->sched.in_stage = 0;
  if (cfg_dbg("sched"))
    fprintf(stderr, "[sched]   slot %d coro out: done=%d base.state=%u entry=0x%08X\n",
            i, co->done() ? 1 : 0, c->mem_r16(base), c->mem_r32(base + 0xc));
  if (co->done() || c->mem_r16(base) == 0) {
    c->mem_w16(base, 0);
    c->game->sched.task_started[i] = 0;
    delete co; co = nullptr;
  }
  return STANZA_HANDLED;
}

// STAGE-0 START.BIN step-spread — resume path. Between the fresh startBinStage tick (handled in
// the generic stanza below) and the final swap-to-DEMO tick, Engine::stage0Advance steps the
// preload SM one step per scheduler tick to match the recomp coro yield cadence (docs/findings/
// sbs.md Slip #1). Entry stays at 0x8010649C until the last step's native_start_stage swap.
static StanzaResult run_stage0_step_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                           int native_content) {
  if (!(native_content && st == 2 && c->game->sched.task_started[i]
        && c->mem_r32(base + 0xc) == 0x8010649Cu && c->game->sched.stage0_step[i] < 7))
    return STANZA_NOT_MINE;
  // Under mIsFaithful, STAGE-0 routes through the fiber stanza above (byte-clean stack scratch).
  if (c->game->mIsFaithful) return STANZA_NOT_MINE;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  c->game->sched.cur_slot = i;
  static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];
  c->game->sched.in_stage = 1;
  if (setjmp(c->game->sched.yield_jmp) == 0) {                   // final step yields via longjmp
    c->engine.stage0Advance(c->game->sched.stage0_step[i]);
  }
  c->game->sched.in_stage = 0;
  c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);
  // Last step's native_start_stage rewrites entry to DEMO + sets state=3 (fresh) — leave both
  // alone. Otherwise keep state=2 (runnable) for the next step.
  if (c->mem_r32(base + 0xc) == 0x8010649Cu) c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// Generic dispatch — the "everything else" path. Handles: fresh entry setup (state==3 or state==2
// with no live ctx); game_coop resume at 0x801063F4 (guest-loop re-entry with loop's callee-saved
// regs pinned); state==2 resume from saved r31. Inside the setjmp block, fresh entries for the
// two remaining native task entries (GAME stagePrologue and STAGE-0 startBinStage) fire before
// rec_coro_run. DEMO/SOP/GAME-per-frame entries were consumed by the stanzas above.
static StanzaResult run_generic_dispatch_stanza(Core* c, int i, uint32_t base, uint32_t st,
                                                int native_content, const R3000& loop) {
  uint32_t resume_pc;
  int fresh = 0;
  if (st == 3 || (st == 2 && !c->game->sched.task_started[i])) {
    resume_pc = c->mem_r32(base + 0xc);
    fresh = 1;
    c->game->sched.task_ctx[i] = loop;
    c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);
    c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
    c->game->sched.task_started[i] = 1;
    c->game->sched.demo_native[i] = 0;
  } else if (st == 2 && c->game->sched.game_coop[i]
             && c->mem_r32(base + 0xc) == 0x8010637Cu) {
    // GAME cooperative re-entry at loop top 0x801063F4 with the callee-saved regs the loop
    // expects; sp pinned to task-top - 0x20 (verified 0x8010637C prologue frame size, later-284c
    // stack-leak fix) so the pre-prologue r29 leak doesn't ratchet task0's guest sp into the task
    // table across 100+ field frames.
    resume_pc = 0x801063F4u;
    c->game->sched.task_ctx[i].r[16] = 0x1f800000u;
    c->game->sched.task_ctx[i].r[17] = 0x1f800000u;
    c->game->sched.task_ctx[i].r[18] = 1;
    c->game->sched.task_ctx[i].r[31] = 0x801063F4u;
    c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8) - 0x20u;
  } else if (st == 2) {
    c->game->sched.game_coop[i] = 0;                             // left GAME cooperative loop
    resume_pc = c->game->sched.task_ctx[i].r[31];
  } else {
    return STANZA_HANDLED;                                       // sleeping this frame (state==1)
  }
  c->mem_w16(base, 4);
  if (cfg_dbg("sched"))
    fprintf(stderr, "[sched] slot %d st_in=%u resume_pc=0x%08X ra=0x%08X sp=0x%08X\n",
            i, st, resume_pc, c->game->sched.task_ctx[i].r[31], c->game->sched.task_ctx[i].r[29]);
  c->mem_w32(CUR_TASK, base);
  c->game->sched.cur_slot = i;
  static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];
  c->game->sched.in_stage = 1;
  if (setjmp(c->game->sched.yield_jmp) == 0) {
    uint32_t start = resume_pc;
    // Remaining fresh-entry native dispatches (DEMO's 0x801062E4 was handled by run_demo_stanza).
    if (native_content && fresh && resume_pc == 0x8010637Cu) {
      c->override_tgt = resume_pc;                               // GAME stageMain: coro-redirect
      c->engine.stageMain();
      start = c->coro_redirect_pc ? c->coro_redirect_pc : c->r[31];
      c->coro_redirect_pc = 0;
    } else if (native_content && fresh && resume_pc == 0x8010649Cu && !c->game->mIsFaithful) {
      c->engine.startBinStage();                                 // STAGE-0 fresh; skip rec_coro_run
      // Under mIsFaithful the fiber stanza above handles STAGE-0 (byte-clean substrate scratch);
      // we won't reach this branch, but the `!mIsFaithful` gate is defensive.
      c->game->sched.stage0_step[i] = 0;
      c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);
      c->mem_w16(base, 2);
      c->game->sched.in_stage = 0;
      return STANZA_HANDLED;
    }
    ffspan_begin(c); rec_coro_run(c, start); ffspan_end(c, "coro");
    c->mem_w16(base, 0);                                          // task returned (jr ra sentinel)
    c->game->sched.task_started[i] = 0;
  }
  c->game->sched.in_stage = 0;
  return STANZA_HANDLED;
}

// One scheduler pass over the 3 task slots (replaces FUN_80051e60). The main loop is now purely
// slot-iteration + stanza dispatch — the historical scattered address-check chain lives in the
// per-stanza processors above.
void native_scheduler_step(Core* c) {
  R3000 loop = *c;                           // frame-loop REGISTERS (gp etc. for fresh tasks); slices off RAM
  for (int i = 0; i < 3; i++) {
    uint32_t base = TASKBASE + (uint32_t)i * TASKSTRIDE;
    // Task slot 2 = XA voice/BGM. When the native clip player owns it, do NOT run the (now unused)
    // FUN_8001cfc8 recomp coroutine; reflect clip state into task-2's state byte so the cutscene's
    // `while (DAT_801fe0e0 != 0)` wait advances exactly when the clip finishes.
    if (i == 2 && xa_stream_owns_slot2()) {
      if (xa_stream_voice_busy()) c->mem_w16(base, 2);
      else { c->mem_w16(base, 0); xa_stream_voice_release(); }
      c->game->sched.task_started[2] = 0;
      continue;
    }
    uint32_t st = c->mem_r16(base);
    if (st == 0) { c->game->sched.task_started[i] = 0; c->game->sched.demo_native[i] = 0; continue; }
    // GATE: on `gate` toggle to psx_fallback, clear any stale native-dispatcher mode so the
    // generic path re-enters cleanly (the native DEMO/GAME dispatchers are OFF under psx_fallback).
    int native_content = !c->game->psx_fallback;
    if (!native_content && (c->game->sched.demo_native[i] || c->game->sched.game_native[i])) {
      c->game->sched.demo_native[i] = 0; c->game->sched.game_native[i] = 0; c->game->sched.task_started[i] = 0;
    }
    if (run_demo_stanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)          continue;
    if (run_sop_area_load_stanza(c, i, base, st, native_content, loop) == STANZA_HANDLED) continue;
    if (run_game_stanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)          continue;
    if (run_coro_fiber_stanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)    continue;
    if (run_stage0_step_stanza(c, i, base, st, native_content) == STANZA_HANDLED)         continue;
    run_generic_dispatch_stanza(c, i, base, st, native_content, loop);
  }
  static_cast<R3000&>(*c) = loop;             // restore the frame-loop REGISTERS (shared RAM untouched)
}
