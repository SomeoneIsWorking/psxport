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
// reference modes (PSXPORT_SBS_MODE=gameplay/both core B) abort at the first scheduler yield-return
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

// One scheduler pass over the 3 task slots (replaces FUN_80051e60).
extern "C" void ffspan_reset_frame(void), ffspan_begin(Core*), ffspan_end(Core*, const char*);  // PSXPORT_BDTAG (engine_stage.cpp)
void native_scheduler_step(Core* c) {
  R3000 loop = *c;                           // frame-loop REGISTERS (gp etc. for fresh tasks); slices off RAM
  for (int i = 0; i < 3; i++) {
    uint32_t base = TASKBASE + (uint32_t)i * TASKSTRIDE;
    // Task slot 2 = XA voice/BGM. When the native clip player owns it, do NOT run the (now unused)
    // FUN_8001cfc8 recomp coroutine; instead reflect clip state into the task-2 state byte so the
    // cutscene's `while (DAT_801fe0e0 != 0)` wait advances exactly when the clip finishes.
    if (i == 2 && xa_stream_owns_slot2()) {
      if (xa_stream_voice_busy()) c->mem_w16(base, 2);     // still playing -> stay "running"
      else { c->mem_w16(base, 0); xa_stream_voice_release(); }  // clip done -> free -> cutscene advances
      c->game->sched.task_started[2] = 0;
      continue;
    }
    uint32_t st = c->mem_r16(base);
    if (st == 0) { c->game->sched.task_started[i] = 0; c->game->sched.demo_native[i] = 0; continue; }  // free
    // GATE: with psx_fallback on, the native stage dispatchers + loaders (DEMO/GAME per-frame, START.BIN
    // file-table builder, SOP area load) are OFF — the stage state machines and asset/area LOADING run as
    // pure PSX recomp coroutines (the generic path; their CD reads stay sync via cd_override). The scheduler
    // harness stays native always. Clear any stale native-dispatcher mode so a `gate` toggle re-enters the
    // generic path cleanly.
    int native_content = !c->game->psx_fallback;
    if (!native_content && (c->game->sched.demo_native[i] || c->game->sched.game_native[i])) {
      c->game->sched.demo_native[i] = 0; c->game->sched.game_native[i] = 0; c->game->sched.task_started[i] = 0;
    }
    // ---- DEMO / front-end NATIVE per-frame task (no guest coroutine) ----------------------------
    // The DEMO stage runs as a native dispatcher: ov_demo_frame is called once per frame with ALL state
    // in guest RAM (sm[0x48] substate, sm[0x4a] sub-mode). On the FRESH entry we run the prologue + s0
    // (ov_demo_stage_main) then the first frame; on resume, just another frame. The substates are all
    // SYNCHRONOUS (no cross-frame yield), so no coroutine/longjmp is needed — "yield" is just returning.
    // This replaces coro-redirecting into the guest loop (whose deep pure-PSX substate calls hit libcd/
    // VSync busy-waits our no-IRQ runtime can't satisfy, and which the removed override table can no
    // longer intercept). Leaving DEMO (-> GAME) re-registers the slot with a non-DEMO entry, clearing
    // demo_native via the generic fresh path below.
    {
      int demo_fresh = native_content && (st == 3 || (st == 2 && !c->game->sched.task_started[i]))
                       && c->mem_r32(base + 0xc) == 0x801062E4u;
      if (demo_fresh || (c->game->sched.demo_native[i] && st == 2 && c->game->sched.task_started[i])) {
        if (demo_fresh) {
          c->game->sched.task_ctx[i] = loop;                         // inherit gp
          c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);   // per-task PSX stack top
          c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
          c->game->sched.task_started[i] = 1;
          c->game->sched.demo_native[i] = 1;
        }
        c->mem_w16(base, 4);                                         // running
        c->mem_w32(CUR_TASK, base);
        c->game->sched.cur_slot = i;
        static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];        // restore REGISTERS
        c->game->sched.in_stage = 1;
        // Contain any cooperative yield from a rec_dispatch'd guest substate (e.g. a menu state that
        // spawns an async CD read and FUN_80051f80-yields): without this setjmp the yield's longjmp
        // would hit a stale jmp_buf and corrupt the scheduler. A yield = the substate isn't fully
        // synchronous yet (a CD load to own native+sync — the frontier); treat it as frame-done.
        if (setjmp(c->game->sched.yield_jmp) == 0) {
          if (demo_fresh) c->engine.demo.stageMain();                // prologue + s0 (sets sm[0x48]=1)
          c->engine.demo.frame();                                    // one frame: substate dispatch + tail
        } else if (cfg_dbg("demo")) {
          static int w = 0; if (!w++) fprintf(stderr, "[demo] caught a substate yield (async CD not yet "
                                                      "owned native+sync) — frontier\n");
        }
        c->game->sched.in_stage = 0;
        // A substate may LEAVE the DEMO stage this frame (s5 New-Game -> GAME calls native_start_stage,
        // which rewrites this task's entry (base+0xc) to the GAME overlay + sets state=3). Detect that by
        // the entry no longer being the DEMO root: drop native-DEMO mode and let the state=3 fresh entry
        // stand so the next step enters GAME via the generic path (do NOT clobber state back to 2).
        if (c->mem_r32(base + 0xc) != 0x801062E4u) {
          c->game->sched.demo_native[i] = 0;
          c->game->sched.task_started[i] = 0;
          continue;
        }
        c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);        // save REGISTERS
        c->mem_w16(base, 2);                                         // runnable next frame (resume the
                                                                    // native dispatcher; state==2 + the
                                                                    // demo_native flag re-enters here)
        continue;
      }
    }
    // ---- SOP area-DATA load task: own the load body SYNCHRONOUSLY (engine/sop.cpp) ---------------
    // SOP state-0 spawns LAB_80109164 (SOP.BIN 0x80109164) as a cooperative slot-1 task and blocks on
    // *0x1f80019b. The task body is all-synchronous (sync CD reads + unpack + reloc patch), so we run
    // it native+sync in one scheduler step and mark the slot done — no FUN_80051fb4 yield. Result is
    // identical (1f80019b=1, ecf58 patched); this removes the load's cross-frame yield, the prereq for
    // owning the SOP machine native per-frame. Keyed on the exact entry (SOP.BIN-specific address).
    {
      int sop_fresh = (st == 3 || (st == 2 && !c->game->sched.task_started[i]))
                      && c->mem_r32(base + 0xc) == 0x80109164u;
      // With psx_fallback on this native SOP load is dropped: fall through so the recomp load body 0x80109164
      // runs as a normal cooperative task (its FUN_80051fb4 yield serviced by the harness). Its CD reads
      // still go through the platform CD sync layer, so the load stays synchronous; only the orchestration
      // (unpack/reloc/libsnd setup) runs as PSX — the recomp baseline.
      if (sop_fresh && native_content) {
        c->game->sched.task_ctx[i] = loop;                          // inherit gp
        c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);    // per-task PSX stack top
        c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
        c->mem_w16(base, 4);                                        // running
        c->mem_w32(CUR_TASK, base);
        c->game->sched.cur_slot = i;
        static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];       // restore REGISTERS
        c->game->sched.in_stage = 1;
        void native_sop_area_load(Core*);
        if (setjmp(c->game->sched.yield_jmp) == 0) {
          native_sop_area_load(c);                                  // synchronous (all leaves are sync)
        } else if (cfg_dbg("sched")) {
          fprintf(stderr, "[sched] SOP area-load yielded unexpectedly — a leaf isn't sync yet\n");
        }
        c->game->sched.in_stage = 0;
        c->mem_w16(base, 0);                                        // task done -> slot free
        c->game->sched.task_started[i] = 0;
        continue;
      }
    }
    // ---- GAME stage NATIVE per-frame dispatcher (mirrors demo_native) ---------------------------
    // Run the GAME stage as a native per-frame task: ov_game_frame each frame (sm[0x48] dispatch +
    // frame counter), all state in guest RAM. Fresh entry -> prologue + first frame; resume -> a frame.
    // SOP state-0's area load is native+sync (later-217b), so no cross-frame yield; a not-yet-sync deep
    // leaf yield is contained by setjmp = frame-done. Replaces coro-redirecting into the guest loop.
    {
      int game_fresh = (st == 3 || (st == 2 && !c->game->sched.task_started[i]))
                       && c->mem_r32(base + 0xc) == 0x8010637Cu;
      // With psx_fallback on the GAME native per-frame dispatcher is off: route the WHOLE GAME stage through the
      // cooperative guest loop instead. The native transition area-load (native_transition_area_load, reached
      // only inside the native dispatcher via ov_game_submode1) is bypassed, so the recomp's cooperative
      // area-load (FUN_80044bd4 spawn-and-wait) runs with its yields serviced — the GAME stage runs as PSX
      // recomp. Drop any stale native mode so a `gate` toggle re-enters the generic path cleanly.
      int areaload = native_content;
      if (!areaload && c->game->sched.game_native[i]) {
        c->game->sched.game_native[i] = 0;
        c->game->sched.task_started[i] = 0;
      }
      if (areaload &&
          (game_fresh || (c->game->sched.game_native[i] && st == 2 && c->game->sched.task_started[i]))) {
        if (game_fresh) {
          c->game->sched.task_ctx[i] = loop;                        // inherit gp
          c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);  // per-task PSX stack top
          c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;
          c->game->sched.task_started[i] = 1;
          c->game->sched.game_native[i] = 1;
        }
        c->mem_w16(base, 4);                                        // running
        c->mem_w32(CUR_TASK, base);
        c->game->sched.cur_slot = i;
        static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];       // restore REGISTERS
        c->game->sched.in_stage = 1;
        int handled = 1;
        if (setjmp(c->game->sched.yield_jmp) == 0) {
          if (game_fresh) c->engine.stagePrologue();                // prologue (sets sm[0x48] from 0x134)
          ffspan_begin(c); handled = c->engine.frame(); ffspan_end(c, "gameframe");  // one frame: sm[0x48] dispatch + tail
        } else if (cfg_dbg("sched")) {
          static int w = 0; if (!w++) fprintf(stderr, "[sched] caught a GAME substate yield (a leaf not "
                                                      "yet sync) — frontier\n");
        }
        c->game->sched.in_stage = 0;
        // A handler may LEAVE the GAME stage (area transition rewrites base+0xc). Drop native mode and
        // let the fresh entry stand if so.
        if (c->mem_r32(base + 0xc) != 0x8010637Cu) {
          c->game->sched.game_native[i] = 0;
          c->game->sched.task_started[i] = 0;
          continue;
        }
        // ov_game_frame returned 0: the current GAME state isn't owned natively yet (transition sub-mode /
        // area machine / non-SOP overlay — all yield deep). Hand the task back to the COOPERATIVE guest
        // loop: resume it at the loop top 0x801063F4 with the regs the loop expects (s0=s1=0x1f800000,
        // s2=1). The generic coroutine path then drives it (the coro-redirect handshake survives the deep
        // yields). Own the area machine + field overlays to shrink this fallback (gameplay_start_flow_re.md).
        if (!handled) {
          c->r[16] = 0x1f800000u; c->r[17] = 0x1f800000u; c->r[18] = 1;   // guest-loop callee-saved regs
          c->r[31] = 0x801063F4u;                                          // resume PC = guest loop top
          c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);
          c->game->sched.game_native[i] = 0;                               // -> generic cooperative path
          c->game->sched.game_coop[i] = 1;       // PC-game: re-enter at the loop top every frame (below)
          c->mem_w16(base, 2);
          if (cfg_dbg("sched")) fprintf(stderr, "[sched] GAME -> cooperative guest loop (state not yet "
                                                 "owned native; field reachable)\n");
          continue;
        }
        c->game->sched.task_ctx[i] = static_cast<R3000&>(*c);       // save REGISTERS
        c->mem_w16(base, 2);                                        // runnable next frame
        continue;
      }
    }
    // ---- FULL-PSX (psx_fallback) task: thread-fiber coroutine — TRUE mid-function resume ----------
    // The native dispatchers above are OFF in psx_fallback, so the stage/loader tasks run as pure
    // recompiled PSX bodies that yield mid-function via switch. The substrate can't re-enter a fn
    // mid-body, so run each task on its OWN Coro thread: a yield BLOCKS the thread (its whole nested C
    // call stack is preserved) and a resume CONTINUES it exactly there — recompiler-only, no interpreter
    // (USER 2026-06-30: "the PSX path needs to work recompiler-only; condvars with pause-resume"). The
    // shared register file Core::r[] is save/restored around the handoff just like the longjmp path:
    // switch saves task_ctx[i] before blocking; we restore it before resuming. The NATIVE path
    // (native_content) is untouched — it falls through to the existing setjmp scheduler below.
    if (!native_content) {
      Coro*& co = c->game->sched.coro[i];
      int co_fresh = (st == 3 || (st == 2 && !c->game->sched.task_started[i]));
      if (co_fresh) {
        if (co) { delete co; co = nullptr; }                       // discard an abandoned task on this slot
        uint32_t entry = c->mem_r32(base + 0xc);                   // task entry pc
        c->game->sched.task_ctx[i] = loop;                         // inherit gp; fresh sp/ra below
        c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);   // per-task PSX stack top (obj+8)
        c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;            // sentinel return (task fn `jr ra` => end)
        c->game->sched.task_started[i] = 1;
        c->game->sched.demo_native[i] = 0; c->game->sched.game_native[i] = 0; c->game->sched.game_coop[i] = 0;
        Core* cc = c;
        co = new Coro();
        co->start([cc, entry] {
          rec_coro_run(cc, entry);   // pure recompiled PSX body; switch yields/exits back to the Coro
        });
      } else if (st == 2 && co && !co->done()) {
        /* resume the suspended fiber (regs restored below) */
      } else if (st == 2) {
        c->game->sched.task_started[i] = 0;      // state==2 but no live fiber (stale) -> drop, re-arm later
        continue;
      } else {
        continue;                                // sleeping this frame (state==1)
      }
      c->mem_w16(base, 4);                                         // running
      c->mem_w32(CUR_TASK, base);
      c->game->sched.cur_slot = i;
      c->game->sched.in_stage = 1;
      c->game->sched.cur_is_coro = 1;
      static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];        // load regs (fresh: gp/sp/ra; resume: saved)
      if (cfg_dbg("sched"))
        fprintf(stderr, "[sched] slot %d coro %s st=%u entry=0x%08X sp=0x%08X\n", i,
                co_fresh ? "start" : "resume", st, c->mem_r32(base + 0xc), c->game->sched.task_ctx[i].r[29]);
      co->resume();                                               // run until switch yields / body returns
      c->game->sched.cur_is_coro = 0;
      c->game->sched.in_stage = 0;
      if (cfg_dbg("sched"))
        fprintf(stderr, "[sched]   slot %d coro out: done=%d base.state=%u entry=0x%08X\n",
                i, co->done() ? 1 : 0, c->mem_r16(base), c->mem_r32(base + 0xc));
      // A handler may LEAVE the stage (rewrite base+0xc / set state) — guest owns base state. If the body
      // finished, or the guest ended the task (state 0), free the slot + reap the fiber.
      if (co->done() || c->mem_r16(base) == 0) {
        c->mem_w16(base, 0);
        c->game->sched.task_started[i] = 0;
        delete co; co = nullptr;
      }
      // else: yielded mid-body. The guest set base state (FUN_80051f80 -> 1); FUN_800506d0 re-arms 1->2.
      continue;
    }

    uint32_t resume_pc;
    int fresh = 0;
    // state==3 (restart at new entry) or state==2 on a slot with no live context (freshly
    // registered by FUN_80051f14) => fresh entry. state==2 with a live context => resume.
    if (st == 3 || (st == 2 && !c->game->sched.task_started[i])) {
      resume_pc = c->mem_r32(base + 0xc);        // task entry
      fresh = 1;
      c->game->sched.task_ctx[i] = loop;                   // inherit gp; fresh sp/ra below
      c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);// per-task PSX stack top (obj+8)
      c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;      // sentinel return
      c->game->sched.task_started[i] = 1;
      c->game->sched.demo_native[i] = 0;       // a non-DEMO fresh entry (e.g. GAME): leave native-DEMO mode
    } else if (st == 2 && c->game->sched.game_coop[i]
               && c->mem_r32(base + 0xc) == 0x8010637Cu) {
      // GAME cooperative loop: as a PC game, re-enter at the loop TOP every frame (not the saved mid-yield
      // PC, which the substrate can't continue). gen_func_801063F4 runs one frame from the top — dispatch
      // the SM, then yield — with the loop's callee-saved regs; all SM state persists in guest RAM.
      resume_pc = 0x801063F4u;
      c->game->sched.task_ctx[i].r[16] = 0x1f800000u;
      c->game->sched.task_ctx[i].r[17] = 0x1f800000u;
      c->game->sched.task_ctx[i].r[18] = 1;
      c->game->sched.task_ctx[i].r[31] = 0x801063F4u;
      // RESTORE the loop's STACK POINTER too (later-284c). We re-enter at 0x801063F4, which is PAST the
      // 0x8010637C prologue `addiu sp,-0x20`, so the loop body always runs with sp = task-stack-top - 0x20.
      // The saved r29 from the previous yield is DEEPER: the per-frame yield FUN_80051f80 does `addiu sp,-0x18`
      // and we re-enter at the loop top BEFORE its `addiu sp,+0x18` epilogue runs, so that 0x18 is never
      // unwound — leaking 0x18/frame off task0's ~2.5KB guest stack. Left unreset, after ~100 field frames sp
      // reaches the task table at 0x801FE000 and a leaf's `sw`-spill clobbers sm[0x48]/sm[0x4a] -> ov_game_frame
      // sees s4a=0 with A00 (not SOP) resident -> jal 0x80109450 into A00's jump-table data -> recomp-MISS.
      // Pinning sp to the loop frame base every re-entry kills the leak (matches the fresh-entry sp after the
      // prologue). 0x20 = the verified 0x8010637C prologue frame size (GAME.BIN disasm), not a fudge factor.
      c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8) - 0x20u;
    } else if (st == 2) {                     // runnable: resume after the previous yield
      c->game->sched.game_coop[i] = 0;        // left the GAME cooperative loop (e.g. area transition)
      resume_pc = c->game->sched.task_ctx[i].r[31];
    } else {
      continue;                               // sleeping this frame (state==1)
    }
    c->mem_w16(base, 4);                         // running
    if (cfg_dbg("sched"))
      fprintf(stderr, "[sched] slot %d st_in=%u resume_pc=0x%08X ra=0x%08X sp=0x%08X\n",
              i, st, resume_pc, c->game->sched.task_ctx[i].r[31], c->game->sched.task_ctx[i].r[29]);
    c->mem_w32(CUR_TASK, base);
    c->game->sched.cur_slot = i;
    static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];   // restore REGISTERS only (shared RAM untouched)
    c->game->sched.in_stage = 1;
    if (setjmp(c->game->sched.yield_jmp) == 0) {
      uint32_t start = resume_pc;
      // A task ENTRY may be a native override (e.g. the GAME stage prologue ov_game_stage_main).
      // interp_flat only fires overrides on a control transfer INTO an address, never at its start
      // pc, and nothing `jal`s a task entry — so fire the entry override here, as a native call,
      // exactly once on a FRESH entry (NOT on a resume, whose saved pc is deep guest code that must
      // be interpreted, not re-dispatched). The override does its prologue and either rec_coro_redirect's
      // to where the interp should continue (coro_redirect_pc) or returns to its ra; run flat from there.
      // With psx_fallback on, these native stage entries are OFF: the stage runs as pure PSX recomp from
      // its entry (start == resume_pc), so the GAME stage SM and the START.BIN file-table builder + stage-0
      // asset preload all run as PSX (sync CD via cd_override). Only fire the native owners at full native.
      if (native_content && fresh && resume_pc == 0x8010637Cu) {
        // The ONE native task entry: the GAME stage dispatcher (engine_stage.cpp). Called directly
        // (top-down PC-driven) instead of via the removed address-keyed override table.
        c->override_tgt = resume_pc;
        c->engine.stageMain();
        start = c->coro_redirect_pc ? c->coro_redirect_pc : c->r[31];
        c->coro_redirect_pc = 0;
      } else if (native_content && fresh && resume_pc == 0x8010649Cu) {
        // Stage-0 START.BIN entry: own the file-table builder native (disc_find_file), then continue
        // the PSX stage SM in-task. Same top-down direct-call pattern as the GAME stage above.
        c->engine.startBinStage();
        start = c->coro_redirect_pc ? c->coro_redirect_pc : c->r[31];
        c->coro_redirect_pc = 0;
      }
      // (The DEMO/front-end entry 0x801062E4 is handled by the native per-frame dispatcher above,
      //  never here — it `continue`s before reaching this generic coroutine path.)
      ffspan_begin(c); rec_coro_run(c, start); ffspan_end(c, "coro");   // runs until ov_yield longjmps back here
      c->mem_w16(base, 0);                        // returned (jr ra sentinel): task ended -> free
      c->game->sched.task_started[i] = 0;
    }
    c->game->sched.in_stage = 0;
  }
  static_cast<R3000&>(*c) = loop;             // restore the frame-loop REGISTERS (shared RAM untouched)
}
