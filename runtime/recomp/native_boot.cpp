// PC-PSX hybrid native boot driver.
//
// Architecture (locked): the PC engine is the driver. It runs the game's INIT calls (the
// prefix of game-main FUN_80050b08) via the recompiler, then OWNS the frame loop, replacing
// the PSX cooperative-task scheduler (FUN_80051e60) with native per-frame stepping of the
// current stage's state machine. No BIOS threads, no ucontext: leaf logic fns are normal
// recompiled fns that RETURN; the stage sequencers' infinite yield-loops are reimplemented
// natively (one state-machine iteration per frame == one original FUN_80051f80 yield).
//
// We do NOT call FUN_80050b08 directly (it ends in the infinite scheduler loop). Instead we
// override it: crt0 (func_800896E0) does BSS-zero + SP/gp/heap setup and calls main, which is
// now this native driver. The init prefix below is transcribed 1:1 from FUN_80050b08
// (ram_f1000_all.c:31275-31299); each call is dispatched into the recompiled/overridden body,
// whose CD/sync/vsync/pad/thread dependencies are already native overrides.
//
// MILESTONE 1 (this file, current): run the init prefix and confirm it executes cleanly via
// PC/RAM probes. The native frame loop + per-stage stepping land next.
#include "core.h"
#include "game.h"      // SchedulerState (per-instance cooperative-task state) reached via c->game->sched
#include "c_subsys.h"
#include "cfg.h"
#include "asset.h"     // ov_unpack_group / ov_upload_image — existing native asset leaves (call direct)
#include "audio/music_list.h"   // native sound-test: music_list_play/stop (engine/audio/)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>   // usleep (debug-server pause/step idle wait)
#include "coro.h"      // thread-fiber for full-PSX mid-function resume (later-264)

// Generic cooperative-task entry. POST-INTERPRETER (later-254): this is now just rec_dispatch
// (dispatch.cpp) — it can only ENTER a recompiled function at its top, NOT resume a yielded task at a
// saved mid-function PC. So it only works for a FRESH task entry; resuming a full-PSX (psx_fallback)
// task whose saved r31 is mid-body fail-fasts (no recompiled entry there). That is why the full-PSX
// reference modes (PSXPORT_SBS_MODE=gameplay/both core B) abort at the first scheduler yield-return
// (e.g. 0x80051FA4). The native path avoids this entirely: each stage runs as a synchronous per-frame
// native dispatcher (DEMO/GAME/SOP) and the GAME field re-enters at its loop top (game_coop), never a
// mid-function resume. See docs/findings/recomp.md "full-PSX coroutine resume".
void rec_coro_run(Core* c, uint32_t pc);

// Native XA voice/BGM clip player (xa_stream.c) owns task slot 2 — it replaced the FUN_8001cfc8
// streaming-reader coroutine. The scheduler skips slot 2 while owned and reflects clip completion
// into the task-2 state byte (the cutscene waits `while (DAT_801fe0e0 != 0)`).
void xa_dialog_coord(Core* c);          // dialog-vs-ingame-music coordination (cd_override.c)
void xa_audio_trace(Core* c, const char* tag);    // CD-vol fade + XA lifecycle trace (cd_override.c)

// ---- Native-layer GATE registry (user directive 2026-06-23) -------------------------------------
// Each PC-native layer that REPLACES game behavior is gated behind a named flag (DEFAULT ENABLED).
// `native <name> off` at the REPL drops that layer so the recompiled game code (the in-game ORACLE)
// runs in its place — letting us A/B a native layer against the faithful recomp to isolate breakage.
// Names auto-register on first query. Diagnostic-only mechanism (not a shipped behavior toggle).
struct NativeGate { const char* name; int on; };
static NativeGate s_gates[32];
static int s_ngates = 0;
extern "C" int native_gate(const char* name) {
  for (int i = 0; i < s_ngates; i++) if (!strcmp(s_gates[i].name, name)) return s_gates[i].on;
  if (s_ngates < 32) { s_gates[s_ngates] = { strdup(name), 1 }; return s_gates[s_ngates++].on; }  // copy name (REPL buf dangles); default ON
  return 1;
}
static void native_gate_set(const char* name, int on) {
  (void)native_gate(name);   // ensure registered
  for (int i = 0; i < s_ngates; i++) if (!strcmp(s_gates[i].name, name)) { s_gates[i].on = on; return; }
}
static void native_gate_list() {
  fprintf(stderr, "[native] gates (%d):\n", s_ngates);
  for (int i = 0; i < s_ngates; i++)
    fprintf(stderr, "  %-16s %s\n", s_gates[i].name, s_gates[i].on ? "on" : "off");
}

// Call recompiled/overridden game fn `fn` with up to 3 args; runs to its `jr ra` and returns.
static void rc0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static void rc1(Core* c, uint32_t fn, uint32_t a0) { c->r[4] = a0; rec_dispatch(c, fn); }
static void rc2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static void rc3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}
static void rc4(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3; rec_dispatch(c, fn);
}

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
#define TASKBASE 0x801fe000u   // task obj table base (slot i at +i*0x70)
#define TASKSTRIDE 0x70u
#define CUR_TASK 0x1f800138u   // DAT_1f800138: scheduler "current task" ptr

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
void ov_switch(Core* c) {
  if (!c->game->sched.in_stage) { c->r[2] = c->r[4]; return; }   // no-op: return the handle arg in v0
  if (cfg_dbg("yieldpc")) fprintf(stderr, "[yieldpc] ov_switch yield from ra=0x%08X 801fe0e0=0x%X\n",
                                  c->r[31], c->mem_r32(0x801fe0e0u));
  int slot = c->game->sched.cur_slot;
  c->game->sched.task_ctx[slot] = static_cast<R3000&>(*c);  // save REGISTERS only (r29=task SP, r31=resume ra)
  if (c->game->sched.cur_is_coro) {
    // FULL-PSX thread-fiber task. If the guest just ENDED this task (FUN_80051fb4 set state=0 then funneled
    // here), the body will never be resumed — unwind the fiber thread so its body returns and the Coro
    // finishes (else the thread blocks forever). Otherwise BLOCK the fiber (its whole C stack preserved);
    // the scheduler's co->resume() returns and we continue here on the next resume — mid-function.
    if (c->mem_r16(TASKBASE + (uint32_t)slot * TASKSTRIDE) == 0)
      c->game->sched.coro[slot]->exit_now();
    c->game->sched.coro[slot]->yield();
    return;
  }
  longjmp(c->game->sched.yield_jmp, 1);   // native path: unwind to the scheduler's setjmp
}

// One scheduler pass over the 3 task slots (replaces FUN_80051e60).
extern "C" void ffspan_reset_frame(void), ffspan_begin(void), ffspan_end(const char*);  // PSXPORT_BDTAG (engine_stage.cpp)
static void native_scheduler_step(Core* c) {
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
        void ov_demo_stage_main(Core*); void ov_demo_frame(Core*);
        // Contain any cooperative yield from a rec_dispatch'd guest substate (e.g. a menu state that
        // spawns an async CD read and FUN_80051f80-yields): without this setjmp the yield's longjmp
        // would hit a stale jmp_buf and corrupt the scheduler. A yield = the substate isn't fully
        // synchronous yet (a CD load to own native+sync — the frontier); treat it as frame-done.
        if (setjmp(c->game->sched.yield_jmp) == 0) {
          if (demo_fresh) ov_demo_stage_main(c);                     // prologue + s0 (sets sm[0x48]=1)
          ov_demo_frame(c);                                          // one frame: substate dispatch + tail
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
        void ov_game_stage_prologue(Core*); int ov_game_frame(Core*);
        int handled = 1;
        if (setjmp(c->game->sched.yield_jmp) == 0) {
          if (game_fresh) ov_game_stage_prologue(c);                // prologue (sets sm[0x48] from 0x134)
          ffspan_begin(); handled = ov_game_frame(c); ffspan_end("gameframe");  // one frame: sm[0x48] dispatch + tail
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
    // recompiled PSX bodies that yield mid-function via ov_switch. The substrate can't re-enter a fn
    // mid-body, so run each task on its OWN Coro thread: a yield BLOCKS the thread (its whole nested C
    // call stack is preserved) and a resume CONTINUES it exactly there — recompiler-only, no interpreter
    // (USER 2026-06-30: "the PSX path needs to work recompiler-only; condvars with pause-resume"). The
    // shared register file Core::r[] is save/restored around the handoff just like the longjmp path:
    // ov_switch saves task_ctx[i] before blocking; we restore it before resuming. The NATIVE path
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
          rec_coro_run(cc, entry);   // pure recompiled PSX body; ov_switch yields/exits back to the Coro
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
      co->resume();                                               // run until ov_switch yields / body returns
      c->game->sched.cur_is_coro = 0;
      c->game->sched.in_stage = 0;
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
        void ov_game_stage_main(Core*);
        extern uint32_t g_override_tgt; g_override_tgt = resume_pc;
        ov_game_stage_main(c);
        start = c->coro_redirect_pc ? c->coro_redirect_pc : c->r[31];
        c->coro_redirect_pc = 0;
      } else if (native_content && fresh && resume_pc == 0x8010649Cu) {
        // Stage-0 START.BIN entry: own the file-table builder native (disc_find_file), then continue
        // the PSX stage SM in-task. Same top-down direct-call pattern as the GAME stage above.
        void ov_start_bin_stage(Core*);
        ov_start_bin_stage(c);
        start = c->coro_redirect_pc ? c->coro_redirect_pc : c->r[31];
        c->coro_redirect_pc = 0;
      }
      // (The DEMO/front-end entry 0x801062E4 is handled by the native per-frame dispatcher above,
      //  never here — it `continue`s before reaching this generic coroutine path.)
      ffspan_begin(); rec_coro_run(c, start); ffspan_end("coro");   // runs until ov_yield longjmps back here
      c->mem_w16(base, 0);                        // returned (jr ra sentinel): task ended -> free
      c->game->sched.task_started[i] = 0;
    }
    c->game->sched.in_stage = 0;
  }
  static_cast<R3000&>(*c) = loop;             // restore the frame-loop REGISTERS (shared RAM untouched)
}

// ---- BGM frame counter (PSXPORT_BGMDBG trace shared with cd_override.cpp) --------------------
// FUN_80074BF8(idx) starts BGM #idx; FUN_80074E48() stops it. These are now OWNED PC-native by
// engine/sound.cpp (sound_register), which also carries the instant-CD dialog-music cut hook. Only
// the shared frame counter remains here (cd_override.cpp externs it for its own BGM trace).
volatile uint32_t g_bgm_frame = 0;   // current logic frame (extern: cd_override.cpp trace)
void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

// ONE frame of deterministic guest work — the steppable core of the native frame loop, factored out so
// the in-process dual-core diff can step TWO cores in lockstep (it calls this on `a` then `b`). It is
// EXACTLY the guest-mutating body of the loop below (per-frame IRQ events, draw/display-env setup, the
// FUN_800788ac frame update + native scheduler pass + dialog-music coord + draw sync + buffer flip); the
// loop's driver scaffolding (REPL, auto-navigation/input, pause/step, diagnostics, dbg_server) stays in
// the loop and runs ONCE around this call. No input is injected here — drive pads before calling it.
// gpu_perf.cpp — per-frame CPU phase / frame-time profiler (REPL `debug perf`), default off.
extern "C" void perf_frame_begin(void), perf_mark_pre(void), perf_frame_end(void),
                perf_phase_begin(int), perf_phase_end(int);

// ---- DUAL-VIEW guest-state snapshots (native | PSX side-by-side render of ONE game state) -------------
// Two snapshots of guest state (main RAM + scratchpad + GTE regs): "pre" = post-gameplay/pre-render
// (captured in ov_field_frame by dv_snapshot, before the native render consumes per-frame queues) — the
// PSX render pass runs from this; "post" = the real post-frame canonical state, restored after the PSX
// pass so the running game is unaffected by the extra render. See native_step_frame's dual-view block.
extern "C" int g_dualview;       // defined in engine_render.cpp
extern "C" int g_sbs;            // defined in sbs.cpp (PSXPORT_SBS two-core side-by-side harness)
extern "C" { int g_dv_have_pre = 0; }
static uint8_t  s_dv_pre_ram[0x200000],  s_dv_post_ram[0x200000];
static uint8_t  s_dv_pre_spad[0x400],    s_dv_post_spad[0x400];
static uint32_t s_dv_pre_gc[32], s_dv_pre_gd[32], s_dv_post_gc[32], s_dv_post_gd[32];
static void dv_save(Core* c, uint8_t* ram, uint8_t* spad, uint32_t* gc, uint32_t* gd) {
  uint32_t gte_read_ctrl(uint32_t), gte_read_data(uint32_t);
  memcpy(ram, c->ram, 0x200000); memcpy(spad, c->scratch, 0x400);
  for (int i = 0; i < 32; i++) { gc[i] = gte_read_ctrl(i); gd[i] = gte_read_data(i); }
}
static void dv_load(Core* c, const uint8_t* ram, const uint8_t* spad, const uint32_t* gc, const uint32_t* gd) {
  void gte_write_ctrl(uint32_t,uint32_t), gte_write_data(uint32_t,uint32_t);
  memcpy(c->ram, ram, 0x200000); memcpy(c->scratch, spad, 0x400);
  for (int i = 0; i < 32; i++) { gte_write_ctrl(i, gc[i]); gte_write_data(i, gd[i]); }
}
// called from ov_field_frame (engine_stage.cpp) right before the native render. Captures the
// post-gameplay / pre-render guest state UNCONDITIONALLY: it feeds both the dual-view PSX pass AND the
// always-on "PSX render underneath" (user 2026-06-24: the native renderer must leave no guest-memory side
// effects; the PSX render runs from this snapshot to keep guest memory correct). RAM+scratchpad+GTE.
extern "C" void dv_snapshot(Core* c) { dv_save(c, s_dv_pre_ram, s_dv_pre_spad, s_dv_pre_gc, s_dv_pre_gd); g_dv_have_pre = 1; }
extern "C" void dv_capture_post(Core* c) { dv_save(c, s_dv_post_ram, s_dv_post_spad, s_dv_post_gc, s_dv_post_gd); }
extern "C" void dv_restore_pre (Core* c) { dv_load(c, s_dv_pre_ram,  s_dv_pre_spad,  s_dv_pre_gc,  s_dv_pre_gd ); }
extern "C" void dv_restore_post(Core* c) { dv_load(c, s_dv_post_ram, s_dv_post_spad, s_dv_post_gc, s_dv_post_gd); }

// Bind THIS core's per-instance SPU state (Beetle spu.c), lazily powering it on first use. Like gte_bind,
// called per core frame-step + at boot, from the explicit Core — two cores keep SEPARATE SPU state.
static void spu_bind(Core* c) {
  SPU_BindState(c->game->spu_state);
  if (!c->game->spu_powered) { SPU_Power(); c->game->spu_powered = 1; }
}
// Same for MDEC (per-instance; lazy power on first bind — MDEC has no separate global init).
static void mdec_bind(Core* c) {
  MDEC_BindState(c->game->mdec_state);
  if (!c->game->mdec_powered) { MDEC_Power(); c->game->mdec_powered = 1; }
}
// Bind THIS core's per-instance CD-controller (cdc_native.c) and XA streamer (xa_stream.c) state, so two
// cores (native vs PSX-recomp) keep SEPARATE CD state — the recomp core busy-polls the CD registers /
// streams XA, the native core mostly bypasses them via cd_override. Plain-C BindState (cdc_state.h /
// xa_state.h), same per-frame-step contract as gte/spu/mdec.
static void cdc_bind(Core* c) { cdc_bind_state(&c->game->cdc); }   // decls in cdc_state.h (via game.h, extern "C")
static void xa_bind(Core* c)  { xa_bind_state(&c->game->xa); }     // decls in xa_state.h  (via game.h, extern "C")

static void native_step_frame(Core* c, uint32_t f) {
  void gte_bind(Core*); gte_bind(c);   // bind THIS core's GTE register file (per-instance — no shared GTE)
  spu_bind(c);                          // bind THIS core's SPU state (per-instance — no shared SPU)
  mdec_bind(c);                         // bind THIS core's MDEC state (per-instance — no shared MDEC)
  cdc_bind(c);                          // bind THIS core's CD-controller registers (per-instance — no shared CD)
  xa_bind(c);                           // bind THIS core's XA streamer state (per-instance — no shared XA)
  void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
  ffspan_reset_frame();   // backdrop-attribution: reset the per-frame builder span table
  void pad_service_frame(Core*);
  void gpu_set_disp_origin(Core* c, int x, int y);
  (void)f;
  perf_frame_begin();   // perf: start the frame clock (top of the deterministic per-frame work)
  // Per-frame IRQ-driven events the game's waits poll via TestEvent (VBlank classes + sound-DMA-complete).
  hle_deliver_event(c, 0xF2000003u, 0xFFFFFFFFu);
  hle_deliver_event(c, 0xF0000001u, 0xFFFFFFFFu);
  hle_deliver_event(c, 0xF0000009u, 0xFFFFFFFFu);
  // SINGLE-BUFFERED (PC-native) — the game's own PSX double-buffering is REMOVED (user: "remove the
  // game's own double buffering, it causes problems"). The PSX flips between two VRAM pages each frame:
  // OT region 0x800e80a8 + parity*0x2070 and packet pool 0x800bfe68 + parity*0x14000, with the display/
  // draw env following parity. That page-flip is pointless on PC — the VK renderer composites a COMPLETE
  // frame and the present provides the display buffering — and it actively caused aliasing: the native
  // display-list buckets are keyed by OT address (ndl_alloc on otaddr & 0x1FFFFC), so they alternated
  // between the two pages, and the present could sample the page being drawn. Pin the back-buffer parity
  // to 0: one OT region, one packet pool, the same env every frame, and NO flip below. 0x1f800135 stays
  // 0 (set by eng_init_framestate), so any guest code that reads it also sees a stable single buffer.
  const uint32_t parity = 0;                                 // was mem_r8(0x1f800135) — pinned single-buffer
  uint32_t envp = 0x800e80a8u + parity * 0x2070u;            // the one VRAM region we DRAW into every frame
  c->mem_w32(0x800ed8c8, envp);                               // PTR_DAT_800ed8c8 (OT base, now constant)
  rc2(c, 0x80081458, envp, 0x800);                            // ClearOTagR(ot, 0x800)
  c->mem_w16(0x800e809c, 0);                                  // DAT_800e809c = 0 (dwell counter)
  c->mem_w32(0x800bf4f4, c->mem_r32(0x800bf544));             // keep last pool ptr (read by some submitters)
  c->mem_w32(0x800bf544, (parity * 0x14000 + 0x800bfe68) & 0xffffff);   // packet pool (now constant)
  pad_service_frame(c);                                       // host input -> game pad buffer (pre-read)
  xa_audio_trace(c, "pre");                                   // CD-vol fade state BEFORE tick+mix
  perf_mark_pre();   // perf: charge the pre-tick host work (input/IRQ/OT-clear) to `pre`
  // PC-driven frame body: per-frame state update (still-PSX leaf) + per-vblank audio + fps60 commit +
  // present + pace. Called as a plain C call (top-down PC-driven model) — NOT an override. This is where
  // gpu_present / gpu_pace_frame / the per-vblank sequencer+SPU tick are reached every live frame; before
  // this was wired they were orphaned with the override table (a live window ran uncapped + stale). The
  // present happens here (before the OT submit below) so the VK batch shown is the one DrawOTag built last
  // frame, exactly as the override-era ordering did.
  ffspan_begin();
  { void ov_frame_update(Core*); ov_frame_update(c); }        // tick + per-vblank audio + present + pace
  ffspan_end("frameupd");
  xa_audio_trace(c, "post");                                  // CD-vol fade state AFTER tick+mix
  perf_phase_begin(3);   // perf: SCHED-LOGIC = the cooperative scheduler step (the real per-frame GAME logic)
  // The native scheduler is the frame-loop's task-stepping HARNESS (no BIOS threads — yields are setjmp/
  // longjmp coroutines, CD loads are synchronous). It stays native at every gate level. What the gate
  // controls is whether the TASK BODIES it steps run as native stage dispatchers + content (full native) or
  // as pure PSX recomp coroutines (psx_fallback on) — see the gate checks inside native_scheduler_step.
  ffspan_begin();
  native_scheduler_step(c);                                   // <- replaces FUN_80051e60 (BIOS scheduler)
  ffspan_end("scheduler");
  perf_phase_end(3);
  xa_dialog_coord(c);                                         // dialogs stop/restore ingame music
  xa_audio_trace(c, "coord");                                 // CD-vol fade state AFTER coord
  rc1(c, 0x80080f6c, 0);                                      // draw sync
  rc0(c, 0x800506d0);                                         // task sleep-countdown (re-arm 1->2)
  // Display + OT submit (LAB_80050c6c, DAT_1f80019c==0 branch). Single-buffered, PC-native display.
  // The PSX disp-env dance is GONE: we draw page `parity` (env0 → VRAM (0,0)) and tell the PC present to
  // scan that very page DIRECTLY via gpu_set_disp_origin, instead of routing through PutDispEnv and the
  // opposite buffer's disp-env struct (the later-161 (1-parity) trick — a band-aid over the env pairing
  // that depended on the two structs lining up). One fixed page, drawn and displayed; nothing PSX in the
  // display path. (env0's draw area starts at VRAM (0,0); the display W/H stay as the boot env's mode set.)
  // Dual-core diff mode skips the whole display/OT-submit block (host render output, shared VK singleton).
  // SBS: both cores set diff_mode=1 (suppress per-core present at the engine frame tail) but ALSO
  // sbs_render=1 to re-enable THIS render-submit block, so each core EMITS its geometry into its own VK
  // batch (the SBS composite then draws each into its pane). So gate on "not-diff-mode OR sbs_render".
  if ((!c->game->diff_mode || c->game->sbs_render) && c->mem_r16(0x1f80019c) == 0) {
    rc1(c, 0x800815d0, envp + 0x2014);                        // PutDrawEnv (draw area/offset/clip for page 0)
    gpu_set_disp_origin(c, 0, 0);                             // PC-native: present scans the page we draw
    // DrawOTag, PC-native: call ov_draw_otag DIRECTLY (top-down) instead of interpreting the PSX FUN_80081560.
    // ov_draw_otag walks the OT to ENUMERATE the leftover guest prims (its draw ORDER is discarded), QUEUES
    // them into the engine render queue (rq_active() is always on), then rq_flush()es the queue in ENGINE
    // order. The interpreted PSX DrawOTag did the walk+queue but NOT the flush (rq_flush only lives in
    // ov_draw_otag, orphaned by the override-table removal) — so the queue filled every frame and never
    // drained, and NOTHING 2D reached the VK renderer (the whole front-end rendered black). a0 = OT head.
    { void ov_draw_otag(Core*); c->r[4] = envp + 0x1ffcu; ov_draw_otag(c); }
    // ---- DUAL-VIEW second render pass: render the SAME game state via the PSX recomp path into render
    // target 1 (right panel). The engine render is NOT idempotent (its per-frame queues/OT get consumed),
    // so the PSX pass must run from the PRE-render state captured in ov_field_frame (dv_snapshot, before
    // the native render ran), not from the post-native-render state. We then restore the POST-FRAME state
    // so the canonical game (which includes the post-render per-frame area update) is undisturbed.
    extern int g_dualview, g_dv_have_pre; extern void gpu_gpu_select_target(int);
    extern void dv_capture_post(Core*), dv_restore_pre(Core*), dv_restore_post(Core*);
    // SBS owns BOTH panes (core A | core B); its target-1 batch is core B's render, NOT a PSX re-render of
    // THIS core — so skip the in-engine dualview second pass. g_sbs declared at file scope below.
    if (g_dualview && g_dv_have_pre && !g_sbs) {
      void ov_draw_otag(Core*);
      dv_capture_post(c);            // save the real post-frame canonical state
      dv_restore_pre(c);             // rewind to the pre-render (post-gameplay) state the PSX pass needs
      rc2(c, 0x80081458, envp, 0x800);                          // ClearOTagR(ot, 0x800)
      c->mem_w32(0x800ed8c8, envp);                             // OT base
      c->mem_w16(0x800e809c, 0);                                // dwell counter
      c->mem_w32(0x800bf4f4, c->mem_r32(0x800bf544));
      c->mem_w32(0x800bf544, (0x800bfe68u) & 0xffffff);         // reset packet pool ptr
      gpu_gpu_select_target(1);
      rec_dispatch(c, 0x8003f9a8u);                             // PSX field render orchestrator (full OT build)
      rec_dispatch(c, 0x8010810cu);                             // render submit (faithful to ov_field_frame)
      c->r[4] = envp + 0x1ffcu; ov_draw_otag(c);                // walk PSX OT -> target-1 batch
      gpu_gpu_select_target(0);
      dv_restore_post(c);            // restore the real canonical state (PSX pass fully undone)
      g_dv_have_pre = 0;
    }
  }
  perf_frame_end();   // perf: close the frame (post-tick remainder + full wall time) + emit rolling avg
}

// Native override of game-main FUN_80050b08: init prefix, then (later) native frame loop.
// ---- Interactive REPL (PSXPORT_REPL=1) — drive the native port from stdin --------------------
// Mirrors the oracle's (wide60rt -repl) command set so one driver can step BOTH cores and diff.
// Commands: run N | r addr [len] | rw addr [words] | w addr val | w8 addr val | watch lo hi |
//   unwatch | hits | press/release <btn> | tap <btn> [frames] | regs | seq | quit. Memory is the
//   game's address space (mem_r*/mem_w*); watchpoints via mem_set_watch (reported during `run`).
void pad_repl_hold(Core* c, uint16_t active_low_mask);
void cam_teleport(int x, int y, int z); void cam_teleport_off(void);   // engine_camera.cpp — REPL `tp`
void pad_repl_tap(Core* c, uint16_t active_low_mask, int n);
static uint16_t repl_btn(const char* n) {     // name -> active-HIGH PSX pad bit
  if (!strcmp(n,"start"))    return 0x0008; if (!strcmp(n,"select")) return 0x0001;
  if (!strcmp(n,"x")||!strcmp(n,"cross"))  return 0x4000;
  if (!strcmp(n,"o")||!strcmp(n,"circle")) return 0x2000;
  if (!strcmp(n,"triangle")||!strcmp(n,"t")) return 0x1000;
  if (!strcmp(n,"square")||!strcmp(n,"sq"))  return 0x8000;
  if (!strcmp(n,"up"))    return 0x0010; if (!strcmp(n,"down"))  return 0x0040;
  if (!strcmp(n,"left"))  return 0x0080; if (!strcmp(n,"right")) return 0x0020;
  return (uint16_t)strtoul(n, 0, 16);
}
// ---- REPL music-dump helpers (build a labeled track library to identify each tune) -------
// Sequenced BGM is rendered through the live SPU (use `wav PATH` then `bgm N` then `run`);
// XA tracks (CD-streamed music/voice) are decoded straight off the disc here, since they
// never touch the sequencer. Both write standard 44100/native-rate stereo S16 WAVs.

static void repl_wav_write(const char* path, const int16_t* pcm, uint32_t frames, int rate) {
  FILE* fp = fopen(path, "wb");
  if (!fp) { fprintf(stderr, "[repl] wav write: cannot open %s\n", path); return; }
  uint32_t data = frames * 4u, riff = 36u + data, brate = (uint32_t)rate * 4u, fmtlen = 16;
  uint16_t pcm1 = 1, ch2 = 2, ba = 4, bits = 16; uint32_t r = (uint32_t)rate;
  fwrite("RIFF", 1, 4, fp); fwrite(&riff, 4, 1, fp); fwrite("WAVE", 1, 4, fp);
  fwrite("fmt ", 1, 4, fp); fwrite(&fmtlen, 4, 1, fp);
  fwrite(&pcm1, 2, 1, fp); fwrite(&ch2, 2, 1, fp); fwrite(&r, 4, 1, fp);
  fwrite(&brate, 4, 1, fp); fwrite(&ba, 2, 1, fp); fwrite(&bits, 2, 1, fp);
  fwrite("data", 1, 4, fp); fwrite(&data, 4, 1, fp);
  fwrite(pcm, 4, frames, fp); fclose(fp);
  fprintf(stderr, "[repl] xadump -> %s (%u frames @ %d Hz, %.2fs)\n", path, frames, rate, frames / (double)rate);
}

// Decode ~`secs` of the XA stream on subheader channel `chan` starting at CHD `start_lba`,
// write a WAV at the stream's native rate. Skips interleaved non-matching/non-audio sectors.
static void repl_xadump(uint8_t chan, uint32_t start_lba, const char* path, int secs) {
  static int16_t out[400000];                 // ~4.5s of 44100 stereo; XA max 37800*secs
  uint8_t raw[2352]; int16_t hist[2][2] = {{0,0},{0,0}}; int freq = 37800;
  uint32_t frames = 0, lba = start_lba, cap = 0;
  for (int guard = 0; guard < 20000; guard++) {
    if (!disc_read_raw(lba, raw, 2352)) break;
    if (raw[15] != 2) break;                   // ran off the Mode2 stream
    uint8_t fchan = raw[17], submode = raw[18];
    lba++;
    if (!(submode & 0x04) || fchan != chan) { if (submode & 0x80) break; continue; }  // not our audio
    int16_t pcm[4032 * 2]; int f2 = freq;
    int n = xa_decode_sector(raw, pcm, hist, &f2); freq = f2;
    if (!cap) cap = (uint32_t)freq * (uint32_t)secs;
    for (int i = 0; i < n && frames < cap && frames < 200000; i++) {
      out[2 * frames] = pcm[2 * i]; out[2 * frames + 1] = pcm[2 * i + 1]; frames++;
    }
    if (frames >= cap || (submode & 0x80)) break;
  }
  repl_wav_write(path, out, frames, freq);
}

// REPL-armed auto-drive state, consumed by the frame loop (see the loop body). `newgame` pulses Cross
// to the GAME prologue; `skip N` pulses Start for N frames to advance the intro cutscene into the field.
static int  g_nav_newgame = 0;
static long g_skip_frames  = 0;
// `warp <id>` (dev/diagnostic): arm an AREA WARP. The GAME-stage area machine loads the area whose id is
// in the current-area global 0x800bf870; an area CHANGE is driven by FUN_80044bd4(area_task_entry=0x800452c0,
// dest_area_id, mode, phase) from inside the GAME stage SM (the steady handler 0x801088d8 case0 calls it with
// a1 = the current/destination area id). That registers/restarts the AREA-LOAD TASK (0x800452c0) which, via
// FUN_8004514c, commits 0x800bf870 = translate(dest), pulls the area overlay (disc LBA/size from the area
// table at 0x800be118, stride 8, indexed by id+3) to 0x80182000, and walks the per-area asset table at
// area_base+0x51000. We arm the dest id here and fire FUN_80044bd4 from the frame loop (scheduler context
// active, like `newgame`). See docs/engine_re.md "Area WARP / destination mechanism".
static int      g_warp_armed = 0;
static uint32_t g_warp_dest   = 0;

// Read+execute REPL commands until a `run N` (returns N) or quit/EOF (returns -1).
static long native_repl_read(Core* c, uint32_t f) {
  static uint16_t held = 0xFFFF;              // active-low held mask (all released)
  char line[256];
  fprintf(stderr, "[repl] frame=%u ready\n", f); fflush(stderr);
  while (fgets(line, sizeof line, stdin)) {
    char cmd[24] = {0}, arg[32] = {0}; unsigned a = 0, b = 0;
    if (sscanf(line, "%23s", cmd) != 1) continue;
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) return -1;
    else if (!strcmp(cmd, "run") && sscanf(line, "%*s %u", &a) == 1) return (long)a;
    else if (!strcmp(cmd, "r") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
      if (!b) b = 16; fprintf(stderr, "[repl] %08X:", a);
      for (unsigned i = 0; i < b && i < 256; i++) fprintf(stderr, " %02X", c->mem_r8(a + i)); fprintf(stderr, "\n");
    } else if (!strcmp(cmd, "rw") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
      if (!b) b = 8; fprintf(stderr, "[repl] %08X:", a);
      for (unsigned i = 0; i < b && i < 64; i++) fprintf(stderr, " %08X", c->mem_r32(a + i * 4)); fprintf(stderr, "\n");
    } else if (!strcmp(cmd, "w") && sscanf(line, "%*s %x %x", &a, &b) == 2) { c->mem_w32(a, b); fprintf(stderr, "[repl] ok\n"); }
    else if (!strcmp(cmd, "w8") && sscanf(line, "%*s %x %x", &a, &b) == 2) { c->mem_w8(a, (uint8_t)b); fprintf(stderr, "[repl] ok\n"); }
    else if (!strcmp(cmd, "watch") && sscanf(line, "%*s %x %x", &a, &b) == 2) c->mem_set_watch(a, b);
    else if (!strcmp(cmd, "unwatch")) { c->mem_set_watch(0, 0); fprintf(stderr, "[repl] unwatch\n"); }
    else if (!strcmp(cmd, "hits")) fprintf(stderr, "[repl] watch hits=%d\n", c->mem_watch_hits());
    else if (!strcmp(cmd, "press") && sscanf(line, "%*s %31s", arg) == 1)   { held &= ~repl_btn(arg); pad_repl_hold(c, held); fprintf(stderr, "[repl] held=%04X\n", held); }
    else if (!strcmp(cmd, "release") && sscanf(line, "%*s %31s", arg) == 1) { held |= repl_btn(arg);  pad_repl_hold(c, held); fprintf(stderr, "[repl] held=%04X\n", held); }
    else if (!strcmp(cmd, "tap") && sscanf(line, "%*s %31s %u", arg, &a) >= 1) { if (!a) a = 4; pad_repl_tap(c, (uint16_t)(0xFFFF & ~repl_btn(arg)), (int)a); fprintf(stderr, "[repl] tap %s %u\n", arg, a); }
    else if (!strcmp(cmd, "debug")) { char ch[200] = {0}; sscanf(line, "%*s %199[^\n]", ch); void cfg_dbg_set(const char*); cfg_dbg_set(ch); fprintf(stderr, "[repl] debug channels = %s\n", ch[0] ? ch : "(none)"); }
    else if (!strcmp(cmd, "ents")) {   // enumerate live GAME OBJECTS across the 3 entity lists, with identity
      // Each object is a node in a doubly-linked list (next @ +0x24). Identity fields: type @+0xc, render
      // intrinsic @+0xb (0x10..0x14 = sprite/billboard, 0/0xf = mesh), behavior handler @+0x1c (the object's
      // "what is it" — different per Tomba / enemy / prop), model id @+0xe & 0x3fff, world pos @+0x2e/32/36,
      // and the 3D MODEL = geomblk of render cmd[0] (cmd @+0xc0, geomblk @ cmd+0x40). later-241.
      const uint32_t heads[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
      int total = 0;
      for (int h = 0; h < 3; h++) {
        uint32_t n = c->mem_r32(heads[h]);
        fprintf(stderr, "[ents] -- list %d head=%08X --\n", h, n);
        for (int g = 0; n && g < 400; g++, n = c->mem_r32(n + 0x24)) {
          uint32_t cmd0 = c->mem_r8(n + 8) ? c->mem_r32(n + 0xC0) : 0;
          fprintf(stderr, "[ents]  %08X t=%02X ri=%02X model=%04X h=%08X pos=(%6d,%6d,%6d) rf=%u cmds=%u gb0=%08X\n",
                  n, c->mem_r8(n + 0xc), c->mem_r8(n + 0xb), c->mem_r16(n + 0xe) & 0x3fff, c->mem_r32(n + 0x1c),
                  (int16_t)c->mem_r16(n + 0x2e), (int16_t)c->mem_r16(n + 0x32), (int16_t)c->mem_r16(n + 0x36),
                  c->mem_r8(n + 1), c->mem_r8(n + 8), cmd0 ? c->mem_r32(cmd0 + 0x40) : 0);
          total++;
        }
      }
      fprintf(stderr, "[ents] (%d nodes)\n", total);
    }
    else if (!strcmp(cmd, "tp")) { int x=0,y=0,z=0;
      if (sscanf(line, "%*s %d %d %d", &x, &y, &z) == 3) { cam_teleport(x, y, z); fprintf(stderr, "[repl] tp camera -> (%d,%d,%d)\n", x, y, z); }
      else { cam_teleport_off(); fprintf(stderr, "[repl] tp off (camera follows player)\n"); } }
    else if (!strcmp(cmd, "invtest")) {   // diagnostic: exercise the inventory subsystem with a test vector
      // invtest [type] [amt] — fire FUN_8004D338/D4C4/D4F4(type,amt) through the override path (with the
      // `invverify` gate enabled this runs the full RAM+scratchpad A/B vs the recomp body). With no args,
      // sweep a spread of item types/amounts covering both quest-ref variants + the 23..28 ring + the cap.
      int ty = -1, am = -1; sscanf(line, "%*s %d %d", &ty, &am);
      static const int vt[] = { 1, 2, 5, 10, 23, 25, 28, 40, 60, 99 };
      static const int va[] = { 1, 3, 1, 50, 1, 99, 2, 7, 1, 5 };
      int n = (ty >= 0) ? 1 : (int)(sizeof vt / sizeof vt[0]);
      for (int i = 0; i < n; i++) {
        uint32_t t = (ty >= 0) ? (uint32_t)ty : (uint32_t)vt[i];
        uint32_t m = (am >= 0) ? (uint32_t)am : (ty >= 0 ? 1u : (uint32_t)va[i]);
        rc2(c, 0x8004D338u, t, m);    // inventory_add core
        rc2(c, 0x8004D4F4u, t, m);    // give_only
        rc2(c, 0x8004D4C4u, t, m);    // give_and_flag
      }
      fprintf(stderr, "[repl] invtest: fired %d vector(s) through inventory overrides\n", n * 3); }
    else if (!strcmp(cmd, "newgame")) { g_nav_newgame = 1; fprintf(stderr, "[repl] newgame: pulsing to GAME prologue\n"); return 100000; }
    else if (!strcmp(cmd, "skip")) { a = 0; sscanf(line, "%*s %u", &a); if (!a) a = 500; g_skip_frames = (long)a; fprintf(stderr, "[repl] skip %u frames\n", a); return (long)a; }
    else if (!strcmp(cmd, "warp")) {
      // warp <area_id> — load a different area on demand (foundation for a level/boss selector). Only valid
      // from the field (GAME stage 0x8010637C, sm[0x48]==2). Arms the dest; the frame loop fires it.
      if (sscanf(line, "%*s %u", &a) == 1) {
        if (c->mem_r32(0x801fe00c) != 0x8010637Cu)
          fprintf(stderr, "[repl] warp: not in GAME stage (stage=%08X) — reach the field first (newgame/skip)\n",
                  c->mem_r32(0x801fe00c));
        else {
          g_warp_dest = a; g_warp_armed = 1;
          fprintf(stderr, "[repl] warp: armed dest area id=%u (cur=%u) — run frames to load\n",
                  a, c->mem_r8(0x800bf870u));
        }
      } else fprintf(stderr, "[repl] warp <area_id>  (area table @0x800be118, ids 0..23)\n");
    }
    else if (!strcmp(cmd, "shot")) { char path[200] = {0}; if (sscanf(line, "%*s %199s", path) == 1) { void gpu_native_shot(Core*, const char*); gpu_native_shot(c, path); } }
    else if (!strcmp(cmd, "vram")) { char path[200] = {0}; unsigned x=0,y=0,w=1024,h=512;
      if (sscanf(line, "%*s %199s %u %u %u %u", path, &x,&y,&w,&h) >= 1) {
        void gpu_gpu_vram_region(const char*, int, int, int, int); gpu_gpu_vram_region(path, (int)x,(int)y,(int)w,(int)h); } }
    else if (!strcmp(cmd, "vramraw")) { char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { void gpu_gpu_vram_raw(const char*); gpu_gpu_vram_raw(path); } }
    else if (!strcmp(cmd, "dumpram")) {
      char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) {
        FILE* fp = fopen(path, "wb");
        if (fp) { fwrite(c->ram, 1, 0x200000, fp); fclose(fp); fprintf(stderr, "[repl] dumpram -> %s\n", path); }
        else fprintf(stderr, "[repl] dumpram: cannot open %s\n", path);
        // Also dump the 1 KB scratchpad (0x1F800000) to a sidecar .spad — the main-RAM A/B diff is
        // BLIND to scratchpad, where several engine flags live (DEMO: 0x1f80019a/19d/134/198 etc.).
        char spath[208]; snprintf(spath, sizeof spath, "%s.spad", path);
        FILE* sp = fopen(spath, "wb");
        if (sp) { fwrite(c->scratch, 1, sizeof c->scratch, sp); fclose(sp); fprintf(stderr, "[repl] dumpram scratchpad -> %s\n", spath); }
      }
    }
    else if (!strcmp(cmd, "wav")) { char path[200] = {0}; if (sscanf(line, "%*s %199s", path) == 1) spu_wav_reopen(path); }
    else if (!strcmp(cmd, "bgm") && sscanf(line, "%*s %u", &a) == 1) { rc1(c, 0x80074BF8u, a); fprintf(stderr, "[repl] bgm %u (song@800bed80=%04X)\n", a, c->mem_r16(0x800bed80)); }
    else if (!strcmp(cmd, "bgmstop")) { rc0(c, 0x80074E48u); fprintf(stderr, "[repl] bgmstop\n"); }
    // native <name> on|off  /  native list — gate PC-native layers (default ON) so the recomp oracle
    // runs in their place. e.g. `native music off` drops the native field-BGM engine.
    else if (!strcmp(cmd, "native")) {
      char nm[64] = {0}, st[16] = {0}; int k = sscanf(line, "%*s %63s %15s", nm, st);
      if (k <= 0 || !strcmp(nm, "list")) native_gate_list();
      else native_gate_set(nm, strcmp(st, "off") != 0),
           fprintf(stderr, "[repl] native %s = %s\n", nm, strcmp(st, "off") ? "on" : "off");
    }
    // gate on|off (or 1|0) — toggle PSX-fallback live: everything the frame loop calls runs as PSX recomp
    // (sync CD) instead of the native owners. Applies to tasks freshly (re)entered after the toggle; an
    // already-running native dispatcher is reset on its next scheduler step so it re-enters as PSX.
    else if (!strcmp(cmd, "gate")) {
      char st[16] = {0};
      if (sscanf(line, "%*s %15s", st) == 1)
        c->game->psx_fallback = (!strcmp(st, "off") || !strcmp(st, "0")) ? 0 : 1;
      fprintf(stderr, "[repl] psx_fallback = %d\n", c->game->psx_fallback);
    }
    // renderpsx on|off — render the FIELD via the PSX recomp path (vs the native world-coord path) with the
    // SAME native game state, for a native-vs-PSX RENDER diff (must match at 1x/4:3/30fps). Diagnostic.
    else if (!strcmp(cmd, "renderpsx")) {
      extern int g_render_psx; char st[16] = {0};
      if (sscanf(line, "%*s %15s", st) == 1)
        g_render_psx = (!strcmp(st, "off") || !strcmp(st, "0")) ? 0 : 1;
      fprintf(stderr, "[repl] g_render_psx = %d\n", g_render_psx);
    }
    // seqsolo <i> — stop ALL open libsnd sequences then SsSeqPlay just sequence <i> at full vol, via the
    // GAME'S OWN sequencer. Lets each area SEP sequence be rendered in isolation (the area's field theme
    // otherwise plays continuously). SsSeqStop=0x80091AF0, SsSeqPlay(h,mode,loop)=0x80090560, SsSeqSetVol
    // (h,volL,volR)=0x80091F50. handle == the seq access index (0..13).
    else if (!strcmp(cmd, "seqsolo") && sscanf(line, "%*s %u", &a) == 1) {
      for (uint32_t i = 0; i < 14; i++) rc1(c, 0x80091AF0u, i);   // SsSeqStop(i) — silence all
      rc3(c, 0x80090560u, a, 1, 0);                                // SsSeqPlay(a, mode=1, loop=0)
      rc3(c, 0x80091F50u, a, 127, 127);                           // SsSeqSetVol(a, 127, 127)
      fprintf(stderr, "[repl] seqsolo %u\n", a);
    }
    // musictest <n> — play catalogued music track <n> through the NATIVE audio engine (sound test).
    // 'musictest stop' (or n<0) stops. Bypasses the broken libsnd path entirely (engine/audio/).
    else if (!strcmp(cmd, "musictest")) {
      char sub[32] = {0}; int n = -1;
      if (sscanf(line, "%*s %31s", sub) == 1 && !strcmp(sub, "stop")) { music_list_stop(); fprintf(stderr, "[repl] musictest stop\n"); }
      else if (sscanf(line, "%*s %d", &n) == 1 && n >= 0) {
        int rc = music_list_play(n);
        fprintf(stderr, "[repl] musictest %d (%s) -> %s\n", n, music_list_name(n) ? music_list_name(n) : "?", rc ? "FAIL" : "ok");
      } else {
        fprintf(stderr, "[repl] musictest: tracks 0..%d, or 'stop'\n", music_list_count()-1);
        for (int i = 0; i < music_list_count(); i++) fprintf(stderr, "   %d: %s\n", i, music_list_name(i));
      }
    }
    else if (!strcmp(cmd, "xadump")) { unsigned ch = 0, lba = 0, secs = 3; char path[200] = {0};
      if (sscanf(line, "%*s %u %u %199s %u", &ch, &lba, path, &secs) >= 3) repl_xadump((uint8_t)ch, lba, path, secs ? (int)secs : 3); }
    else if (!strcmp(cmd, "prof")) {
      void prof_start(void); void prof_stop(void); void prof_dump(const char*);
      char sub[32] = {0}, path[200] = {0}; sscanf(line, "%*s %31s %199s", sub, path);
      if (!strcmp(sub, "start")) prof_start();
      else if (!strcmp(sub, "stop") || !strcmp(sub, "off")) prof_stop();
      else if (!strcmp(sub, "dump")) prof_dump(path[0] ? path : 0);
      else fprintf(stderr, "[repl] prof: start | stop | dump <path>\n");
    }
    else if (!strcmp(cmd, "trace")) {   // trace <path> : open the interp call tracer; `trace` alone closes
      void interp_trace_open(const char*); char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { interp_trace_open(path); fprintf(stderr, "[repl] trace -> %s\n", path); }
      else { interp_trace_open(0); fprintf(stderr, "[repl] trace closed\n"); }
    }
    else if (!strcmp(cmd, "stage")) fprintf(stderr, "[repl] stage=%08X sm48=%d\n", c->mem_r32(0x801fe00c), (int)c->mem_r16(0x801fe048));
    else if (!strcmp(cmd, "regs")) { for (int i = 0; i < 32; i++) { fprintf(stderr, " r%-2d=%08X", i, c->r[i]); if ((i & 3) == 3) fprintf(stderr, "\n"); } fprintf(stderr, " hi=%08X lo=%08X\n", c->hi, c->lo); }
    else if (!strcmp(cmd, "seq")) fprintf(stderr, "[repl] seq open=%d playmask=%04X tickmode=%d seqfn=%08X stage=%08X\n",
                                          (int16_t)c->mem_r16(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF, c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(0x801fe00c));
    else fprintf(stderr, "[repl] ? %s\n", cmd);
    fflush(stderr);
  }
  return -1;  // EOF
}

static void ov_game_main(Core* c);

// PC-native crt0 (faithful reimplementation of FUN_800896E0): BSS-zero, heap setup, then a DIRECT
// call to ov_game_main — the top of the top-down PC-driven spine. Replaces the old bootstrap flip
// (crt0 jal main -> override). The libc/heap init at 0x80089860 stays a dispatched PSX leaf.
// crt0 register/heap setup only (no main call) — shared by native_crt0 and the dual-core harness.
static void crt0_setup(Core* c) {
  for (uint32_t a = 0x800be0d8; a < 0x80106228; a += 4) c->mem_w32(a, 0);   // BSS zero
  uint32_t v0 = c->mem_r32(0x800a3f88) - 8;          // stack top base
  uint32_t sp = v0 | 0x80000000u;
  uint32_t a0 = 0x80106228u & 0x1FFFFFFFu;           // 0x00106228 (heap base, masked)
  uint32_t v1 = c->mem_r32(0x800a3f8c);
  uint32_t heapsz = (v0 - v1) - a0;
  c->mem_w32(0x800abef8, heapsz);                    // heap size
  a0 |= 0x80000000u;                                 // 0x80106228
  c->mem_w32(0x800abef4, a0);                        // heap base
  c->r[28] = 0x800be0d4u;                            // gp
  c->r[29] = sp; c->r[30] = sp;                      // sp, fp
  c->r[4]  = a0 + 4;                                 // a0 for the init call
  rec_dispatch(c, 0x80089860u);                      // libc/heap init (PSX leaf) — keep dispatched
}

static void native_crt0(Core* c) {
  crt0_setup(c);
  ov_game_main(c);                                   // DIRECT PC call (was: crt0 jal main -> flip)
}

// --- PC-native task-0 bootstrap: own the START.BIN resolve + stage-0 overlay load top-down ----------
// Replaces the FUN_800499e8 -> FUN_80052078 -> FUN_800450bc CD subtree, which (run as a pure-PSX leaf
// now that the CD overrides are unregistered) busy-waits forever on the libcd Init/Read handshake.
// Behavioural reference (read via tools/disas.py):
//   FUN_800499e8 : CdSearchFile("\BIN\START.BIN") -> {MSF,size}; store {LBA,size} at 0x800be1e0/e4;
//                  FUN_80052078(0).
//   FUN_80052078 : FUN_800450bc(task+0xc, 0); task.state=3; task[0x6f]=0; a few libgpu/BIOS resets.
//   FUN_800450bc : FUN_8001db8c(0x80106228, LBA, size) [= ov_cd_loadfile]; entry = STAGE_ENTRY[0]
//                  (0x8010649c); task+0xc = task+0x10 = entry.
// The per-stage {LBA,size} table lives at 0x800be1e0 (stride 8); the stage-entry table at 0x800a3ecc.
static const uint32_t STAGE_ENTRY_TBL = 0x800a3ecc;  // [0]=0x8010649c [1]=0x801062e4 [2]=0x8010637c
static const uint32_t STAGE_FILE_TBL  = 0x800be1e0;  // {LBA,size} per stage, stride 8

void cd_loadfile_native(Core* c, uint32_t dest, uint32_t lba, uint32_t size);  // cd_override.cpp

// FUN_800450bc: load the stage overlay (if any) and point the task's restart entry at the stage code.
static void native_load_overlay(Core* c, uint32_t taskfields, uint32_t stage) {
  uint32_t entry;
  if (stage == 3) {
    entry = c->mem_r32(STAGE_ENTRY_TBL + 3 * 4);     // stage 3 is already resident: no overlay load
  } else {
    uint32_t lba  = c->mem_r32(STAGE_FILE_TBL + stage * 8);
    uint32_t size = c->mem_r32(STAGE_FILE_TBL + stage * 8 + 4);
    cd_loadfile_native(c, 0x80106228, lba, size);    // = FUN_8001db8c / ov_cd_loadfile
    // FUN_80051f80(1) cooperative yield is a no-op with the native scheduler — skipped.
    entry = c->mem_r32(STAGE_ENTRY_TBL + stage * 4);
  }
  c->mem_w32(taskfields, entry);                     // task+0xc = restart PC
  c->mem_w32(taskfields + 4, entry);                 // task+0x10
}

// FUN_80052078: switch task 0 to the given stage (load overlay + reset the display/BIOS bits).
static void native_start_stage(Core* c, uint32_t stage) {
  uint32_t task = c->mem_r32(0x1f800138);            // current task (= task 0, 0x801fe000)
  native_load_overlay(c, task + 0xc, stage);
  c->mem_w16(task, 3);                               // task state = 3 (active)
  c->mem_w8(task + 0x6f, 0);
  rec_dispatch(c, 0x80080890u);                      // EnterCriticalSection (BIOS leaf)
  c->r[4] = c->mem_r32(task + 4);
  rec_dispatch(c, 0x80080870u);                      // B(0Fh) reset (BIOS leaf)
  rec_dispatch(c, 0x800808a0u);                      // ExitCriticalSection (BIOS leaf)
  c->r[4] = 0xff000000u;
  rec_dispatch(c, 0x80080880u);                      // B(10h) reset (BIOS leaf)
}

// Public entry for the front-end (engine_demo.cpp s5 = LEAVE DEMO -> GAME). FUN_80052078(2): the DEMO
// substate s5's whole body is `jal 0x80052078(2)` — switch task 0 to stage 2 (GAME). The scheduler's
// DEMO branch detects the entry change and hands off to GAME (see native_scheduler_step).
void demo_start_stage(Core* c, uint32_t stage) { native_start_stage(c, stage); }

// FUN_800499e8: resolve \BIN\START.BIN natively, record its {LBA,size}, switch task 0 to stage 0.
static void native_task0_bootstrap(Core* c) {
  uint32_t lba = 0, size = 0;
  if (!disc_find_file("\\BIN\\START.BIN", &lba, &size)) {
    fprintf(stderr, "[native_boot] FATAL: cannot resolve \\BIN\\START.BIN on disc\n");
    return;
  }
  c->mem_w32(STAGE_FILE_TBL, lba);                   // 0x800be1e0 = START.BIN LBA
  c->mem_w32(STAGE_FILE_TBL + 4, size);              // 0x800be1e4 = START.BIN size
  fprintf(stderr, "[native_boot] START.BIN resolved: LBA %u, %u bytes\n", lba, size);
  native_start_stage(c, 0);
}

// Read a NUL-terminated guest string into `out` (bounded). Used to pull the START.BIN filename
// tables (which live in the loaded overlay) for native resolution.
static void read_guest_str(Core* c, uint32_t addr, char* out, int cap) {
  int k = 0;
  for (; k < cap - 1; k++) { char ch = (char)c->mem_r8(addr + k); out[k] = ch; if (!ch) break; }
  out[k] = 0;
}

void cd_loadfile_native(Core* c, uint32_t dest, uint32_t lba, uint32_t size);  // cd_override.cpp (sync 0x8001DB8C/DC40)

// ===== Stage-0 area/asset PRELOAD — PC-native + SYNCHRONOUS (user 2026-06-22: no async loads) =====
// The PSX stage-0 SM (overlay 0x80106728) runs a 4-state preload; each state calls
// FUN_80044bd4(callback,a1,a2) which spawns the callback as task-1 and yield-waits across frames for
// the async streaming reader FUN_8001d940 (never completes in our no-IRQ runtime → the boot hang).
// We own the chain top-down + synchronous, REUSING the existing leaf natives the code map surfaced
// (docs/code-map.md): cd_loadfile_native (the sync replacement for the async CD reads 0x8001DB8C/DC40),
// ov_unpack_group (FUN_80044E84 decompress+VRAM upload, itself now calling ov_upload_image direct).
// Only the orchestration (SM + area-load) and the async-carrying cel upload are reimplemented here.

// FUN_80044F58 texture-group load, synchronous. (Mirrors engine/asset.cpp ov_load_texgroup but driven
// by explicit (mode,set) — no task-1 spawn, no terminal yield.) Header sector -> archive -> unpack ->
// copy the 42-word per-set metadata table the still-recomp content reads back.
void preload_texgroup(Core* c, uint32_t mode, uint32_t set) {
  uint32_t hdr_sector = c->mem_r32(0x800BE0F0u) + set;             // filebase0 + set
  if (mode == 2) {                                                 // mode-2 per-set 4/26-sector bias
    uint16_t mask = (uint16_t)c->mem_r16(0x800BFE56u);
    hdr_sector += ((mask >> (set & 31)) & 1) ? 26u : 4u;
  }
  cd_loadfile_native(c, 0x800EF478u, hdr_sector, 2048);           // 1. 2KB header
  uint32_t h0 = c->mem_r32(0x800EF478u), h1 = c->mem_r32(0x800EF47Cu);
  cd_loadfile_native(c, 0x8018A000u, c->mem_r32(0x800BE0F8u) + (h0 >> 11), h1 - h0);  // 2. compressed archive
  c->r[4] = 0x8018A000u; c->r[5] = 0x1FD000u; ov_unpack_group(c); // 3. decompress + VRAM upload (native)
  for (uint32_t i = 0; i < 42; i++)                                // 4. per-set metadata table
    c->mem_w32(0x800FB170u + i * 4, c->mem_r32(0x800EF478u + 0x100u + i * 4));
}

// FUN_800753D4 cel-load, SYNCHRONOUS. Original: FUN_80096480 (slot alloc + BAV cel load) -> store slot
// at `out` -> FUN_80096980 (kick the upload state machine) -> cross-frame poll FUN_80096a40 until the
// GPU-DMA upload completes. The alloc + kick carry no async wait (they leave the slot in state 1), so
// run them as the recomp REFERENCE; our native GPU upload is synchronous, so we DROP the cross-frame
// poll (the "no async" directive) instead of yielding for a DMA that already happened.
static void preload_cel(Core* c, uint32_t out, uint32_t desc, uint32_t cbarg) {
  int16_t slot = (int16_t)(rc3(c, 0x80096480u, desc, (uint32_t)-1, cbarg), c->r[2]);  // FUN_80096480(desc,-1,cbarg)
  c->mem_w16(out, (uint16_t)slot);                               // *(u16*)out = allocated slot
  rc2(c, 0x80096980u, cbarg, (uint32_t)slot);                    // FUN_80096980(cbarg, slot): kick upload
  // Drain the BAV upload queue SYNCHRONOUSLY so this cel's VAB bank reaches SPU (and its slot frees so the
  // NEXT cel can allocate). The real FUN_800753d4 polls 0x80096a40, whose 0x800993a0 sync busy-waits on the
  // upload's DMA-complete IRQ event — which never fires in this no-IRQ preload, so the original code dropped
  // the poll and silently skipped a whole VAB bank (proved by the dual-core SPU-DMA diff: 1 transfer vs PSX's
  // 2). Deliver the sound-DMA-complete event first so 0x800993a0 returns immediately (no busy-wait, no yield),
  // then run the sync once. (later: in-game music VAB.)
  void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
  hle_deliver_event(c, 0xF0000009u, 0xFFFFFFFFu);                // sound/DMA-complete event
  rc1(c, 0x80096a40u, 0);                                        // FUN_80096a40(0): upload sync (now non-blocking)
}

// FUN_800754F4 cel/sprite VRAM build, synchronous. FUN_800753ac is itself an async CD read -> use the
// sync loadfile; the two FUN_800753d4 cel-loads go through preload_cel; the ten FUN_80075448
// sprite-cell registrations carry no CD/async wait, so run as recomp. `base` = work base 0x80182000.
static void preload_build_vram(Core* c, uint32_t base) {
  uint32_t s0 = base + 0x51000u;                                   // descriptor table (filled by the read)
  cd_loadfile_native(c, base, c->mem_r32(0x800BE108u), 0x51800u);  // FUN_800753ac: read SND file (idx3)
  preload_cel(c, 0x800BED84u, base + c->mem_r32(s0 + 0x28), base + c->mem_r32(s0 + 0x30));
  preload_cel(c, 0x800BED82u, base + c->mem_r32(s0 + 0x2c), base + c->mem_r32(s0 + 0x34));
  static const struct { uint32_t off, sz; } cells[10] = {
    {0x0c,14},{0x08,14},{0x04,14},{0x00,14},{0x10,8},{0x14,8},{0x18,8},{0x1c,14},{0x20,14},{0x24,14},
  };
  uint32_t cell_h = (uint32_t)(int32_t)(int16_t)c->mem_r16(0x800BED82u);
  for (int i = 0; i < 10; i++)
    rc4(c, 0x80075448u, (uint32_t)i, base + c->mem_r32(s0 + cells[i].off), cells[i].sz, cell_h);
  c->r[2] = base + 26356;                                          // v0 = base + 0x66f4
}

// FUN_8004514C — the stage-1 callback. SWDATA + DAT load, shared texgroup sub-load, relocation table,
// then the cel/sprite VRAM build.
static void preload_stage1(Core* c) {
  cd_loadfile_native(c, 0x80157000u, c->mem_r32(0x800BE110u), c->mem_r32(0x800BE114u));  // SWDATA.BIN
  preload_texgroup(c, 1, 1);                                       // shared texgroup sub-load
  uint32_t lo = c->mem_r32(0x800EF480u), hi = c->mem_r32(0x800EF484u);
  cd_loadfile_native(c, 0x80158000u, c->mem_r32(0x800BE100u) + (lo >> 11), hi - lo);     // DAT payload
  uint32_t dat_end = (hi - lo) + 0x80158000u;
  c->mem_w32(0x1F800228u, dat_end);
  c->mem_w32(0x800ED014u, dat_end);
  int32_t n = (int32_t)c->mem_r32(0x800EF488u);                    // relocation table (blez -> skip)
  for (int32_t i = 0; i < n; i++) {
    uint32_t word = c->mem_r32(0x800EF48Cu + i * 4);
    c->mem_w32(0x800ECF58u + (word >> 24) * 4, (word & 0x00FFFFFFu) + 0x80158000u);
  }
  preload_build_vram(c, 0x80182000u);
  c->mem_w32(0x1F80022Cu, c->r[2]);
}

// Stage-0 START.BIN state machine (overlay 0x80106728), PC-native + synchronous. Original loop yields
// one frame per state; synchronous, we run them in order then transition task0 to DEMO and yield (so
// the slot keeps its state=3 restart, exactly like the GAME stage transition — a plain return would
// hit the scheduler's task-ended path and free the slot before the restart).
static void native_stage0_sm(Core* c) {
  uint32_t task = c->mem_r32(CUR_TASK);
  c->mem_w16(task + 0x48, 0);
  c->mem_w16(task + 0x4a, 0);
  preload_texgroup(c, 0, 0);          // state 0: index/asset preload
  preload_stage1(c);                  // state 1: SWDATA + DAT + cel/sprite VRAM build
  native_start_stage(c, 1);           // state 3: switch task0 -> stage 1 (DEMO 0x801062e4), state=3
  ov_switch(c);                       // yield (longjmp to the scheduler); never returns
}

// Stage-0 START.BIN entry (0x8010649c): own the file-table BUILDER PC-native, then hand the small
// stage state-machine back to the PSX body in-task. The builder is a set of CdSearchFile loops that
// resolve ~36 disc filenames and record each {LBA,size}; run as pure PSX (overrides gone) every
// CdSearchFile busy-waits on libcd forever. Replace the resolution with the native ISO9660 resolver
// (disc_find_file) — the filename tables and destination layout are read from the loaded overlay, so
// nothing is hard-coded. Reference: dumped overlay disas of 0x8010649c..0x80106728 (later-211).
//   Loop A: 25 names @0x80106808 -> {LBA,size} table 0x800be118 (stride 8)  [\BIN\OPN/CRD/SOP/A00..A0L]
//   Loop B:  3 names @0x8010686c -> 0x800be1e0  [START/DEMO/GAME.BIN — fills the per-stage file table]
//   Loop C:  5 names @0x801067f4 -> 0x800be0f0  [\CD\TOMBA2.IDX/IMG/DAT/SND, SWDATA.BIN]
//   3 inline singletons -> scratchpad LBA: \CD\VOICE.XA->0x1f80021c, DEMO.XA->0x1f800220, BGM.XA->0x1f800224
// Then s2(reg18)=1 (the SM's "1" constant) and continue into the PSX state machine at 0x80106728,
// whose FUN_80044bd4 cooperative loads run correctly in-task (via rec_coro_run).
void ov_start_bin_stage(Core* c) {
  struct { uint32_t names, dest, n; } loops[] = {
    { 0x80106808u, 0x800be118u, 25 },
    { 0x8010686cu, 0x800be1e0u, 3  },
    { 0x801067f4u, 0x800be0f0u, 5  },
  };
  char name[80];
  for (auto& L : loops) {
    for (uint32_t i = 0; i < L.n; i++) {
      read_guest_str(c, c->mem_r32(L.names + i * 4), name, sizeof name);
      uint32_t lba = 0, size = 0;
      if (disc_find_file(name, &lba, &size)) {
        c->mem_w32(L.dest + i * 8, lba);
        c->mem_w32(L.dest + i * 8 + 4, size);
      } else {
        fprintf(stderr, "[start.bin] Not found file name %s\n", name);   // matches the PSX error path
      }
    }
  }
  struct { uint32_t str, dest; } sing[] = {
    { 0x8010646cu, 0x1f80021cu }, { 0x8010647cu, 0x1f800220u }, { 0x8010648cu, 0x1f800224u },
  };
  for (auto& S : sing) {
    read_guest_str(c, S.str, name, sizeof name);
    uint32_t lba = 0, size = 0;
    if (disc_find_file(name, &lba, &size)) c->mem_w32(S.dest, lba);   // XA stream LBA (only LBA stored)
    else fprintf(stderr, "[start.bin] Not found file name %s\n", name);
  }
  fprintf(stderr, "[start.bin] file table built (native); running stage-0 preload SM (native + sync)\n");
  native_stage0_sm(c);                // own the 4-state preload + transition to DEMO; yields (no return)
}

// Init prefix + task-0 bootstrap (everything FUN_80050b08 does before its scheduler loop). Factored out
// of ov_game_main so the dual-core harness can init two cores then drive the frame loop itself.
static void ov_game_init(Core* c) {
  fprintf(stderr, "[native_boot] FUN_80050b08 override: running init prefix\n");

  // --- init prefix, transcribed from FUN_80050b08 (no scheduler loop) ---
  rc0(c, 0x80089788);
  rc0(c, 0x80085b20);
  // CD init: native HLE, NOT the recomp libcd (FUN_800898a0). The recomp CdInit busy-waits on the
  // CD-controller reset handshake (no IRQ ever acks → 5 retries → "CD timeout" → "Init failed").
  // We model no CD controller; all CD ops are native synchronous (cd_override.cpp). (was rc0 0x800898a0)
  { void cd_hle_init(Core*); cd_hle_init(c); }
  rc1(c, 0x80080bf0, 3);
  rc1(c, 0x80080d64, 0);
  rc1(c, 0x80080ed4, 1);
  rc1(c, 0x800865f0, 0);
  // Engine frame-state + camera init reimplemented PC-native (engine/engine_init.cpp), replacing the
  // 1:1 rec_dispatch transcription. FUN_800509b4 (display/GTE projection + PSX draw/disp double-buffer
  // env) stays dispatched for now — it sets DAT_801003f8 = H that eng_init_camera reads, and entangles
  // the PSX-GPU env, so it is the next target. (later-159, top-down engine port from main.)
  void eng_init_framestate(Core*), eng_init_display(Core*), eng_init_camera(Core*);
  eng_init_framestate(c);      // was rc0(c, 0x80050a0c)
  eng_init_display(c);         // was rc0(c, 0x800509b4) — GTE projection + display (sets H=DAT_801003f8)
  eng_init_camera(c);          // was rc0(c, 0x80050a80)
  rc0(c, 0x80096a70);
  rc1(c, 0x80099310, 0x1010);
  rc1(c, 0x800991b0, 0x20000);
  rc1(c, 0x800993a0, 1);
  // FUN_80089bac(cmd=0xe Setmode, &mode=0x80, 0) — CdControlB. The recomp body busy-waits in CD_cw
  // on the controller ack (no IRQ -> "CdlSetmode timeout"). We model no CD drive mode (every read is
  // by LBA, served natively), so Setmode is a native no-op. (was rc3 0x80089bac)
  // (removed: VSync(3) display-settle wait — the PC-native frame loop owns ALL timing; boot does not
  // call libetc VSync. Any code that reaches VSync now TRAPS (sync_overrides.cpp ov_vsync_trap).)
  // FUN_80075130 font/text init reimplemented PC-native (engine/engine_font.cpp): owns the orchestration +
  // direct writes + the 3 engine-state callees (FUN_800963a0/80096370/800752b4); the 8 libgpu/sound callees
  // stay rec_dispatched in-context (later-182b nested-dispatch risk). Replaces the rc0 transcription.
  { void ov_font_init(Core*); ov_font_init(c); }   // was rc0(c, 0x80075130)
  rc1(c, 0x8009c620, 0);
  rc0(c, 0x8001cc00);
  { void eng_init_subsystems(Core*); eng_init_subsystems(c); }  // was rc0(c, 0x800520e0) — own orchestration native
  // (removed: VSync(1) — see above; PC owns timing, boot never calls VSync.)
  rc0(c, 0x80051e00);                       // scheduler-table init (task objs @0x801fe000)
  rc2(c, 0x80051f14, 0, 0x800499e8);        // register task 0, entry FUN_800499e8
  // VSyncCallback(LAB_800506b4): native no-op — we deliver no preemptive VBlank IRQ (the per-vblank
  // callback's unmodeled interrupt-vector deref is skipped). (was rc1 0x80085bb0)
  { void ov_vsync_callback(Core*); c->r[4] = 0x800506b4; ov_vsync_callback(c); }

  fprintf(stderr, "[native_boot] init prefix complete\n");

  // --- task 0 initial entry: FUN_800499e8 resolves \BIN\START.BIN and FUN_80052078(0) loads
  // the stage-0 overlay to 0x80106228 + restarts task 0 at stage 0 (0x8010649c). It yields once
  // (FUN_80051f80, a no-op with threads stubbed) so it runs straight to completion here. The
  // scheduler's "current task" ptr DAT_1f800138 is normally set by FUN_80051e60; set it to task0
  // so FUN_80052078/FUN_800450bc operate on task 0. ---
  c->mem_w32(0x1f800138, 0x801fe000);
  native_task0_bootstrap(c);   // PC-native: was rc0(c, 0x800499e8) — CD subtree owned top-down
  // START.BIN loaded raw to 0x80106228: [0]=manifest count (6); entry word @0x8010649c.
  fprintf(stderr, "[native_boot] after FUN_800499e8: START.BIN count@0x80106228=%u "
                  "entry-word@0x8010649c=0x%08X (expect 0x27BDFE38); task0 state=%u entry=0x%08X\n",
          c->mem_r32(0x80106228), c->mem_r32(0x8010649c), c->mem_r16(0x801fe000), c->mem_r32(0x801fe00c));
}

// Dual-core harness hooks (dualcore.cpp): boot a core to the start of the frame loop, then step it one
// frame at a time. dc_boot_init = crt0 setup + the init prefix/bootstrap; dc_step_frame = one frame.
void dc_boot_init(Core* c) { void gte_bind(Core*); gte_bind(c); spu_bind(c); mdec_bind(c); cdc_bind(c); xa_bind(c); crt0_setup(c); ov_game_init(c); }
void dc_step_frame(Core* c, uint32_t f) { native_step_frame(c, f); }

static void ov_game_main(Core* c) {
  void gte_bind(Core*); gte_bind(c);   // bind this core's GTE before the init prefix / frame loop
  spu_bind(c);                          // and this core's SPU
  mdec_bind(c);                         // and this core's MDEC
  cdc_bind(c);                          // and this core's CD-controller registers
  xa_bind(c);                           // and this core's XA streamer
  ov_game_init(c);
  // --- native frame loop (replaces LAB_80050c6c). Per frame, faithful to the game-main loop
  // body but with the scheduler call FUN_80051e60 replaced by native stage stepping (added
  // incrementally). native_step_frame calls ov_frame_update DIRECTLY (PC-driven, top-down): real
  // per-frame update (still-PSX leaf FUN_800788ac) + per-vblank audio + fps60 commit + gpu_present +
  // gpu_pace_frame + satisfies the vblank pacing dwell. PSXPORT_NATIVE_FRAMES caps the run (headless). ---
  // ov_switch (the cooperative task-switch) is wired via the platform-HLE table — see
  // sync_overrides_init: FUN_80080880 (ChangeThread, the universal yield/task-end primitive that
  // FUN_80051f80/FUN_80051fb4 funnel through) -> ov_switch, so a yield from an interpreted task
  // coroutine longjmps back to the native scheduler. (Was the removed address-keyed override table.)
  // BGM start/stop (FUN_80074BF8 / FUN_80074E48) are now OWNED PC-native by engine/sound.cpp
  // (sound_register, called from games_tomba2_init). The instant-CD "cut looping ingame music when a
  // dialog tone starts" hook (xa_music_cut_if_dialog) moved into ov_sound_play_bgm there. The REPL
  // `bgm`/`bgmstop` commands still rc1/rc0 those addresses directly (now routed through the overrides).

  // Frame budget: an explicit PSXPORT_NATIVE_FRAMES always wins (headless tests). Otherwise, when
  // a window is up this is the real interactive game loop — run until the user closes the window
  // (SDL_QUIT -> exit(0) in present_window); headless with no cap defaults to 120 (CI/smoke).
  uint32_t nframes = 0;   // 0 == run until window close / REPL quit
  int repl_mode = cfg_on("PSXPORT_REPL") != 0;
  if (repl_mode) nframes = 0;                       // REPL drives frame count via `run N`
  else { if (!gpu_windowed()) nframes = 120; }  // headless smoke default
  // PSXPORT_AUTO_SKIP needs time to tap through title -> GAME -> field: raise the headless smoke cap so a
  // no-REPL run actually reaches free-roam before the loop ends (REPL runs gate frames via `run N` instead).
  if (!repl_mode && !gpu_windowed() && cfg_str("PSXPORT_AUTO_SKIP")) nframes = 1500;
  // When the debug server is up (headless, no REPL), the run is INTERACTIVELY DRIVEN over the socket
  // (rw/w16/press/shot/dumpram, step/play) — do NOT cap it, or it exits before we can drive. The
  // server's `quit` command (or SIGINT) ends it. AUTO_SKIP still auto-drives to free-roam first.
  if (!repl_mode && !gpu_windowed() && cfg_on("PSXPORT_DEBUG_SERVER")) nframes = 0;
  fprintf(stderr, "[native_boot] entering native frame loop (%s)\n",
          nframes ? "capped" : "interactive (until window close)");
  void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
  void pad_service_frame(Core*);
  void dbg_server_start(Core* c); void dbg_server_service(Core* c);
  dbg_server_start(c);    // PSXPORT_DEBUG_SERVER: non-blocking live TCP debug server (dbg_server.c)
  long repl_budget = 0;   // frames remaining in the current REPL `run N`
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    g_bgm_frame = f;
    // REPL: when the run-budget is exhausted, block reading stdin commands until a `run N` refills
    // it (immediate commands — r/w/watch/input/regs/seq — execute between frames). Quit/EOF breaks.
    if (repl_mode) {
      // Blocking on stdin for the next command is an intentional idle, not a hang — suspend the
      // frame-progress watchdog while waiting so it doesn't fire at a paused REPL prompt.
      if (repl_budget <= 0) watchdog_suspend();
      while (repl_budget <= 0) { repl_budget = native_repl_read(c, f); if (repl_budget < 0) break; }
      if (repl_budget < 0) break;
      repl_budget--;
    }
    // PSXPORT_AUTO_SKIP=1 — headless AUTO-DRIVE into the real GAME free-roam FIELD (NOT the attract DEMO, NOT
    // the intro cutscene). The dead PSXPORT_AUTO_GAMEPLAY/AUTO_SKIP env vars previously did nothing, so a
    // no-input run only ever reached the attract demo (stage 0x801062E4, fully interpreted), never player
    // control. Self-contained state machine driven by the CUTSCENE-ACTIVE flag *(0x1F800137) — verified 1
    // throughout the post-NewGame intro cutscene (scripted camera pan + dialog) and 0 in free-roam:
    //   (0) REACH GAME: tap Cross until task0 enters the GAME stage (0x8010637C).
    //   (1) WAIT FOR CUTSCENE: hold until the flag first reads 1 (skips the early loading 0-window before the
    //       cutscene starts), so a flag==0 later is unambiguously free-roam, not "not started yet".
    //   (2) SKIP: the cutscene does NOT end on its own — pulse START (every ~40 frames) while the flag is 1.
    //       Start ends it (it can take a few taps until the cutscene reaches a skippable point). Pressing
    //       Start ONLY while the flag is 1 is what keeps us OUT of the pause menu (Start in free-roam opens
    //       it). Done once the flag has been 0 for a short persistence window (survives any brief beat gap).
    // Works with or without the REPL (the REPL's `run N` still gates frame budget).
    static int  s_as_phase = -1;     // -1 uninit, 0 reach-GAME, 1 await-cutscene, 2 skip-cutscene, 3 done
    static int  s_as_idle  = 0;      // consecutive flag==0 frames in phase 2
    if (s_as_phase == -1) {
      const char* s = cfg_str("PSXPORT_AUTO_SKIP");
      s_as_phase = (s && strcmp(s, "0")) ? 0 : 3;
      if (s_as_phase == 0) fprintf(stderr, "[autoskip] armed: drive into GAME free-roam\n");
    }
    if (s_as_phase < 3) {
      void pad_repl_release(Core*);
      uint32_t stg = c->mem_r32(TASKBASE + 0xc);
      uint8_t  cut = c->mem_r8(0x1F800137u);             // cutscene-active flag
      if (s_as_phase == 0) {                             // tap Cross until the GAME stage
        if (stg != 0x8010637Cu) { if ((f % 12u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x4000), 6); }
        else { s_as_phase = 1; fprintf(stderr, "[autoskip] reached GAME at frame %u\n", f); }
      } else if (s_as_phase == 1) {                      // wait for the cutscene to actually start (flag -> 1)
        if (cut) { s_as_phase = 2; fprintf(stderr, "[autoskip] intro cutscene up at frame %u; skipping (Start)\n", f); }
      } else {                                           // phase 2: pulse Start while the cutscene is active
        if (cut) { s_as_idle = 0; if ((f % 40u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x0008), 6); }
        else if (++s_as_idle >= 60) {   // ~2s after the flag clears: lets the cutscene-END FADE finish before
          pad_repl_release(c); s_as_phase = 3;   // hand-off, so a Start right after skip opens the pause menu
          fprintf(stderr, "[autoskip] free-roam reached at frame %u (cutscene ended)\n", f);   // (not mid-fade)
        }
      }
    }
    // REPL-armed auto-drive (the `newgame` / `skip` commands). One way to drive the game: pipe REPL
    // commands. `newgame` pulses Cross at the title until task0 enters the GAME prologue (0x8010637C),
    // then returns to the REPL prompt. `skip N` then pulses Start each frame for N frames to advance the
    // post-newgame fisherman dialog cutscene into the field. Manual walking = `press`/`run`/`release`.
    if (g_nav_newgame) {
      if (c->mem_r32(0x801fe00c) != 0x8010637Cu) {
        if ((f % 12u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x4000), 6);   // tap Cross
      } else {
        fprintf(stderr, "[repl] newgame: reached GAME prologue at frame %u\n", f);
        g_nav_newgame = 0; repl_budget = 0;                                     // back to the REPL prompt
        // Do NOT run this frame's native_step_frame: it would advance into the GAME loop body (area
        // INIT -> running sub-mode) and, with the area-code overlay not yet loaded, derail before the
        // REPL prompt regains control. `continue` freezes task0 right after the GAME prologue (before
        // INIT runs) so immediate `r`/`rw`/`dumpram` reads see a clean GAME-entry state.
        continue;
      }
    }
    if (g_skip_frames > 0) {
      if ((f % 24u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x0008), 6);     // pulse Start
      if (--g_skip_frames == 0) { void pad_repl_release(Core*); pad_repl_release(c);
        fprintf(stderr, "[repl] skip done at frame %u\n", f); }
    }
    // `warp` — fire the armed area change. We replicate the NON-yielding essential body of the GAME-stage
    // area-change call FUN_80044bd4(0x800452c0, dest, mode=0, phase=2). FUN_80044bd4 cannot be rec_dispatched
    // directly here: it has cooperative FUN_80051f80(1) yields (waiting for in-flight CD-DMA on 0x801fe070)
    // that deadlock when invoked outside a real task run (verified: a direct dispatch hangs). Its meaningful
    // work in a steady field (DMA idle) is just: (1) FUN_80052010(2) kill sub-task slot 2; (2) write the
    // dest area id into task0[0x6e] and the load mode into task0[0x6d]; (3) FUN_80051f14(1, 0x800452c0)
    // restart AREA-LOAD TASK slot 1 at its entry. The cooperative area-load task then runs naturally over
    // the following frames (commits 0x800bf870=translate(dest), pulls the area overlay from the table at
    // 0x800be118[id+3], walks the asset table) — no nested-yield hazard. Both callees are non-yielding.
    if (g_warp_armed) {
      g_warp_armed = 0;
      // The IN-GAME area reload (no DEMO/title bounce) is driven by the GAME-stage steady handler
      // 0x801088d8 case sm[0x4c]==0 (running sub-mode sm[0x4a]==1): it calls
      // FUN_80044bd4(0x800452c0, a1=lbu@0x800bf870, a2=0, a3=2) IN-CONTEXT (task0), so FUN_80044bd4's
      // cooperative FUN_80051f80 yields work correctly — task0 yields, the area-load task (slot 1) pulls the
      // new overlay, task0 resumes. (Restarting the load task ourselves, or rec_dispatching FUN_80044bd4
      // from the frame-loop top, both fail: the former corrupts the live area -> bad opcodes, the latter
      // deadlocks on the yield outside a task run — both verified.) So we DON'T do the load here: we seed the
      // destination into the area id global 0x800bf870 (case0's a1) and set sm[0x4a]=1, sm[0x4c]=0 so the
      // GAME stage runs case0 itself next frame and loads the dest area. (sm[0x4c]==4 -> FUN_80052078(1) is
      // the title/DEMO teardown, NOT an area-to-area warp — verified it bounces to stage DEMO.)
      const uint32_t TASK0 = 0x801fe000u;
      fprintf(stderr, "[repl] warp: dest=%u at f%u (cur area=%u) -> seed area id + drive GAME case0\n",
              g_warp_dest, f, c->mem_r8(0x800bf870u));
      c->mem_w8(0x800bf870u, (uint8_t)g_warp_dest);    // area id global = dest (case0 passes it to FUN_80044bd4)
      c->mem_w16(TASK0 + 0x4au, 1);                    // sm[0x4a] = 1 (running sub-mode -> handler 0x801088d8)
      c->mem_w16(TASK0 + 0x4cu, 0);                    // sm[0x4c] = 0 -> case0 = FUN_80044bd4 area-load
      fprintf(stderr, "[repl] warp: drove GAME case0; sm[0x4a]=%u sm[0x4c]=%u — run frames to load\n",
              c->mem_r16(TASK0 + 0x4au), c->mem_r16(TASK0 + 0x4cu));
    }
    // PSXPORT_DEBUG_SERVER pause/step: when frozen, do NOT advance the game — just pump host input
    // (keeps the window alive) and service debug commands so `step`/`play` can arrive. A `step` runs
    // exactly one real frame then re-freezes, so transient bad frames can be inspected one at a time.
    { int dbg_is_paused(void), dbg_step_pending(void); void dbg_consume_step(void); void gpu_repaint(Core*);
      if (dbg_is_paused()) watchdog_suspend();   // a debug pause is intentional idle, not a hang
      while (dbg_is_paused()) {
        if (dbg_step_pending()) { dbg_consume_step(); break; }   // run exactly one frame
        pad_service_frame(c);      // pump host input (keeps the window responsive)
        gpu_repaint(c);           // re-present current frame: window stays live + readback is accurate
        dbg_server_service(c);    // receive step/play/capture commands
        usleep(15000);
      } }
    watchdog_pet();   // re-arm the timer for THIS step (covers a step that hangs before it presents,
                      // and re-arms after an idle suspend); c_subsys.h C-linkage decl
    native_step_frame(c, f);   // one frame of deterministic guest work (steppable core; see fn above).
    // native_step_frame -> ov_frame_update OWNS present + pace + per-vblank audio (PC-driven frame body),
    // so the loop no longer needs a separate pacer here (the earlier orphaned-override stopgap is gone).
    // PSXPORT_SEQDBG — libsnd sequencer STATE trace (from SsSeqCalled @0x80090BD0): is any BGM
    // sequence OPEN/PLAYING? 0x801054B0=open-seq count, 0x80104C28=playing bitmask, 0x800AC424=tick
    // mode, 0x800AC42C=SsSeqCalled ptr. If these never go nonzero, no song is ever started → the
    // missing-BGM root cause is upstream (song open/play not happening), not the SPU/tick.
    if (cfg_dbg("seq")) {
      static uint32_t ls = 0xFFFFFFFF;
      uint32_t st = (c->mem_r16(0x801054B0) << 16) | (c->mem_r32(0x80104C28) & 0xFFFF);
      if (st != ls) {
        fprintf(stderr, "[seqdbg] f%u open=%d playmask=0x%04X tickmode=%d seqfn=0x%08X stage=0x%08X\n",
                f, (int16_t)c->mem_r16(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF,
                c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(TASKBASE + 0xc));
        ls = st;
      }
    }
    // PSXPORT_DEBUG=state — RELIABLE game-state probe (replaces the old `nav` camera guess, which
    // disagreed with the screen). Dumps all 3 cooperative-task slots (state@+0x00, entry@+0x0c) so a
    // PAUSE MENU (a separate task spawned in the GAME overlay, entry 0x80108xxx, page byte that task+0x6B)
    // is DETECTABLE: when one of the 3 slots has a 0x80108xxx entry and is alive, a menu is open and
    // gameplay input is going to the menu, NOT the player. Also prints the GAME stage sm + camera pos.
    // Logged only on change of the (slot-state/entry + page) signature to stay quiet.
    // PSXPORT_DEBUG=cam — per-frame camera pos (tracks Tomba). Used to determine controls empirically:
    // hold one button and watch for a vertical (Y) excursion = a JUMP, vs a planar (X/Z) shift = walking.
    if (cfg_dbg("cam"))
      fprintf(stderr, "[cam] f%u (%d,%d,%d)\n", f, (int16_t)c->mem_r16(0x1f8000d2u),
              (int16_t)c->mem_r16(0x1f8000d6u), (int16_t)c->mem_r16(0x1f8000dau));
    if (cfg_dbg("state")) {
      uint64_t sig = 0; int menu_slot = -1; uint8_t menu_page = 0;
      for (int i = 0; i < 3; i++) {
        uint32_t base = 0x801fe000u + (uint32_t)i * 0x70u;
        uint16_t st = c->mem_r16(base);
        uint32_t ent = c->mem_r32(base + 0xc);
        sig = sig * 1099511628211ull + ((uint64_t)st << 32 | ent);
        if (st && (ent & 0xFFFFF000u) == 0x80108000u) { menu_slot = i; menu_page = c->mem_r8(base + 0x6bu); }
      }
      sig = sig * 31 + ((uint64_t)menu_slot << 8 | menu_page);
      static uint64_t last_sig = 0;
      if (sig != last_sig) {
        last_sig = sig;
        fprintf(stderr, "[state] f%u", f);
        for (int i = 0; i < 3; i++) {
          uint32_t base = 0x801fe000u + (uint32_t)i * 0x70u;
          fprintf(stderr, " | s%d st=%u ent=0x%08X", i, c->mem_r16(base), c->mem_r32(base + 0xc));
        }
        fprintf(stderr, "  MENU=%s", menu_slot >= 0 ? "OPEN" : "no");
        if (menu_slot >= 0) fprintf(stderr, "(slot%d page=%u)", menu_slot, menu_page);
        fprintf(stderr, "  cam=(%d,%d,%d) sm[0x4a]=%u\n",
                (int16_t)c->mem_r16(0x1f8000d2u), (int16_t)c->mem_r16(0x1f8000d6u),
                (int16_t)c->mem_r16(0x1f8000dau), c->mem_r16(0x801fe000u + 0x4au));
      }
    }
    // BGM-active probe (PSXPORT_BGMDBG): each frame scan the 14 libsnd sequence slots
    // (0x800be3d8 + i*0xB0) for the active/play flag (+0x98 bit0). For any active slot, log
    // its read pointer (+0x00) vs base (+0x04) — if the read ptr ADVANCES frame-to-frame the
    // sequence is genuinely ticking (audible); if it stays == base the SsSeqCalled tick isn't
    // advancing it (frozen, the handoff's hypothesis). Scene-independent: catches any window
    // where a BGM is active, without needing to reach a specific scene.
    if (cfg_str("PSXPORT_BGMDBG")) {
      static uint32_t s_rd[14];
      for (int i = 0; i < 14; i++) {
        uint32_t s = 0x800be3d8u + (uint32_t)i * 0xB0u;
        uint32_t flag = c->mem_r32(s + 0x98), rd = c->mem_r32(s);
        if ((flag & 1) && rd != s_rd[i]) {
          fprintf(stderr, "[bgmtick] f%u slot%d active rdptr=%08X base=%08X (%+d)\n",
                  f, i, rd, c->mem_r32(s + 4), (int)(rd - c->mem_r32(s + 4)));
          s_rd[i] = rd;
        }
        if (!(flag & 1)) s_rd[i] = 0;
      }
    }
    static uint32_t s_last_entry = 0; static uint32_t s_last_sm = 0xFFFFFFFF;
    uint32_t t0e = c->mem_r32(TASKBASE + 0xc), s48 = c->mem_r16(TASKBASE + 0x48);
    // GAME runs a 4-level nested state machine (task +0x48/4a/4c/4e). Track all of it so a
    // stuck leaf is visible, not just the outer s48.
    uint32_t sm = (c->mem_r16(TASKBASE+0x48)<<24)|(c->mem_r16(TASKBASE+0x4a)<<16)|
                  (c->mem_r16(TASKBASE+0x4c)<<8)|c->mem_r16(TASKBASE+0x4e)
                  ^ (c->mem_r16(TASKBASE+0x50)<<12)^(c->mem_r16(TASKBASE+0x52)<<4);
    if (t0e != s_last_entry || sm != s_last_sm) {
      const char* stg = t0e == 0x8010649Cu ? "START" : t0e == 0x801062E4u ? "DEMO" :
                        t0e == 0x8010637Cu ? "GAME" : "?";
      fprintf(stderr, "[native_boot]   frame %u: stage=%s(0x%08X) sm[48=%u 4a=%u 4c=%u 4e=%u 50=%u 52=%u]"
              " @0x80109450=%08X\n",
              f, stg, t0e, c->mem_r16(TASKBASE+0x48), c->mem_r16(TASKBASE+0x4a),
              c->mem_r16(TASKBASE+0x4c), c->mem_r16(TASKBASE+0x4e), c->mem_r16(TASKBASE+0x50),
              c->mem_r16(TASKBASE+0x52), c->mem_r32(0x80109450));
      s_last_entry = t0e; s_last_sm = sm;
    }
    // One-shot: when GAME has settled, dump the CD-streaming contract (FUN_8001cfc8, task
    // slot 2). task2 obj @0x801fe0e0; +0x54=start LBA, +0x58=end LBA (= globals
    // DAT_801fe134/138). DAT_801fe146=channel/type. _DAT_1f8001f8=dest, _DAT_1f8001f4=words.
    if (cfg_dbg("stream") && t0e == 0x8010637Cu && f == 75) {
      fprintf(stderr, "[streamdbg] task2 obj @0x801fe0e0 state=%u entry=0x%08X\n",
              c->mem_r16(0x801fe0e0), c->mem_r32(0x801fe0ec));
      fprintf(stderr, "[streamdbg] startLBA(+54/801fe134)=%u endLBA(+58/801fe138)=%u "
              "chan(801fe146)=%u be0e4=0x%02X\n",
              c->mem_r32(0x801fe134), c->mem_r32(0x801fe138), c->mem_r8(0x801fe146), c->mem_r8(0x800be0e4));
      fprintf(stderr, "[streamdbg] dest(_DAT_1f8001f8)=0x%08X words(_DAT_1f8001f4)=%u "
              "f0=%u f1f800224=0x%08X\n",
              c->mem_r32(0x1f8001f8), c->mem_r32(0x1f8001f4), c->mem_r32(0x1f8001f0), c->mem_r32(0x1f800224));
    }
    // PSXPORT_RAMDUMP_FRAME=N — dump RAM mid-run at native frame N (overlay state during gameplay
    // differs from end-of-run; needed to disasm the LIVE level/stage overlay at 0x8010/0x8011xxxx).
    { const char* rdf = cfg_str("PSXPORT_RAMDUMP_FRAME");
      if (rdf && f == (uint32_t)strtoul(rdf, 0, 0)) {
        
        const char* rd = cfg_str("PSXPORT_RAMDUMP"); if (!rd) rd = "scratch/bin/midrun_ram.bin";
        FILE* mf = fopen(rd, "wb");
        if (mf) { fwrite(c->ram, 1, 0x200000, mf); fclose(mf);
                  fprintf(stderr, "[native_boot] mid-run RAM dump @frame %u -> %s\n", f, rd); }
      } }
    if (cfg_dbg("schedf"))
      fprintf(stderr, "[schedf] f%u t0[st=%u e=%08X s48=%u s4a=%u s4c=%u s5c=%u] t1[st=%u] t2[st=%u]\n",
              f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48),
              c->mem_r16(TASKBASE + 0x4a), c->mem_r16(TASKBASE + 0x4c), c->mem_r16(TASKBASE + 0x5c),
              c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0));
    else if (f < 10 || (f % 30) == 0)
      fprintf(stderr, "[native_boot]   frame %u: t0[st=%u e=0x%08X s48=%u] t1[st=%u] t2[st=%u] "
                      "f135=%u\n", f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc),
              c->mem_r16(TASKBASE + 0x48), c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0),
              c->mem_r8(0x1f800135));
    dbg_server_service(c);  // service one queued live-debug-server command (non-blocking)
  }
  fprintf(stderr, "[native_boot] frame loop done; task0 state=%u entry=0x%08X obj+0x48=%u\n",
          c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48));
  const char* rd = cfg_str("PSXPORT_RAMDUMP");
  if (rd) {
    
    FILE* f = fopen(rd, "wb");
    if (f) { fwrite(c->ram, 1, 0x200000, f); fclose(f);
             fprintf(stderr, "[native_boot] dumped 2MB RAM -> %s\n", rd); }
  }
}

// Wired from boot.c when PSXPORT_NATIVE_BOOT is set. Registers the main override and enters
// crt0; crt0's call to FUN_80050b08 lands in ov_game_main.
void native_boot_run(Core* c) {
  { void cfg_dump(void); cfg_dump(); }   // log active PSXPORT_* config once (see docs/config.md)
  // PSX-fallback gate (diagnostic): boot + frame-loop stay native; everything the loop calls runs as PSX
  // recomp (sync CD). PSXPORT_GATE nonzero turns it on; the REPL `gate on|off` toggles it live.
  { const char* g = cfg_str("PSXPORT_GATE");
    if (g && *g) c->game->psx_fallback = (atoi(g) != 0);
    fprintf(stderr, "[native_boot] psx_fallback=%d (%s)\n", c->game->psx_fallback,
            c->game->psx_fallback ? "native boot+frameloop, PSX everything else (sync)" : "full native"); }
  // RENDER-path compare switch: PSXPORT_RENDER_PSX renders the field via the PSX recomp path (native state).
  { extern int g_render_psx; const char* r = cfg_str("PSXPORT_RENDER_PSX");
    if (r && *r) g_render_psx = (atoi(r) != 0);
    if (g_render_psx) fprintf(stderr, "[native_boot] g_render_psx=1 (field render via PSX recomp path)\n"); }
  // DUAL-VIEW: render the SAME game state TWICE per frame — engine-native (left) + PSX-recomp (right) — and
  // composite side by side. Set at launch (PSXPORT_DUALVIEW=1) so the GPU allocates two geometry batches.
  { extern int g_dualview; const char* r = cfg_str("PSXPORT_DUALVIEW");
    if (r && *r) g_dualview = (atoi(r) != 0);
    if (g_dualview) fprintf(stderr, "[native_boot] g_dualview=1 (side-by-side native | PSX render)\n"); }
  { extern int g_perobj_psx; const char* r = cfg_str("PSXPORT_PEROBJ_PSX");   // BISECT: per-object render -> PSX
    if (r && *r) g_perobj_psx = (atoi(r) != 0); }
  // Intro FMVs: the real boot is SCEA (stub) -> Whoopee logo (LOGO.STR) -> opening movie (OP.STR) ->
  // title/menu. The game's own STR streaming (strNext) TIMES OUT under our runtime (we don't feed
  // CD-streamed FMV sectors to its StrPlayer — see "time out in strNext()" in the DEMO stage), so the
  // movies are played here with our self-contained native FMV player (native_fmv.c).
  // SPLIT OF OWNERSHIP: only LOGO.STR (the Whoopee logo, which plays BEFORE the front-end overlay is
  // even loaded) is played at boot. OP.STR (the opening movie) is OWNED BY THE FRONT-END — the DEMO
  // menu machine's states 4..7 ARE the OP.STR sequence (engine_demo.cpp demo_menu_machine), which now
  // plays it via native_fmv_play. Playing OP here too made it play TWICE (boot + front-end) — the
  // "FMV repeats" bug. Boot plays LOGO; the front-end plays OP -> SCEA->LOGO->OP->title, no repeat.
  int native_fmv_play(Core*, const char*);
  // Skip the intro FMVs when there's no viewer: PSXPORT_NO_FMV, OR any headless run (a headless probe
  // has nobody watching — playing/decoding the intro movies just burns wall-clock; a field probe went
  // from ~77s to ~1.4s). The in-game/cutscene FMVs that still play are also auto-uncapped in headless
  // (native_fmv.c) so they fast-forward. Set PSXPORT_NO_FMV=0 explicitly to force them on if ever needed.
  int skip_fmv = cfg_on("PSXPORT_NO_FMV") || cfg_on("PSXPORT_VK_HEADLESS");
  const char* nf_ov = cfg_str("PSXPORT_NO_FMV");
  if (nf_ov && atoi(nf_ov) == 0 && *nf_ov) skip_fmv = 0;     // explicit PSXPORT_NO_FMV=0 forces FMVs on
  if (!skip_fmv) {
    fprintf(stderr, "[native_boot] playing boot FMV (Whoopee logo); OP.STR is the front-end's\n");
    native_fmv_play(c, "MOVIE/LOGO.STR");
  } else {
    fprintf(stderr, "[native_boot] skipping intro FMVs (headless/NO_FMV)\n");
  }
  // Clean hand-off to the front-end (issues #7/#11): black the display FB before the title builds, so the
  // title's first frames (drawn over several frames while its background/font/CLUT upload) never composite
  // over the stale SCEA white-fill or an FMV last-frame. Covers the no-FMV-ran case too (the stub splash
  // fill is still resident in s_vram even when both intros are skipped). Deterministic, no timer.
  void gpu_clear_display(Core*);
  gpu_clear_display(c);
  fprintf(stderr, "[native_boot] entering native crt0 (PC-driven)\n");
  native_crt0(c);
  fprintf(stderr, "[native_boot] returned from native crt0\n");
}
