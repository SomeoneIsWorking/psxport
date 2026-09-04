// PcScheduler — the PC-native cooperative task scheduler (replaces the PSX BIOS
// scheduler FUN_80051e60 and its thread layer). One instance per Game
// (c->game->pcSched). Owns the per-task bookkeeping (saved register contexts,
// run flags, step-spread counters) and the per-frame dispatch over the 3 task
// slots: PC-native stanzas (DEMO/SOP/GAME/task-1/STAGE-0) first, then the
// substrate coro-fiber / generic-dispatch stanzas in
// runtime/psx/scheduler.cpp for un-ported tasks. The yield/spawn primitives
// (scheduler_yield, native_task_spawn) also live in scheduler.cpp and reach
// this state via c->game->pcSched.
//
// A task context is ONLY the CPU register file (R3000) — guest
// RAM/scratchpad/DMA/peripherals are SHARED one memory across all tasks (saving
// a whole Core would give each task its own RAM snapshot — the OOP regression
// where the loader task read a pre-fill file-table snapshot and stalled boot;
// see oop-regression-hunt). So task_ctx slices to the R3000 base on
// save/restore.
#pragma once
#include "r3000.h"
#include "synchronous_task_wait.h"
#include <setjmp.h>
#include <stdint.h>

class Game;
class Coro; // runtime/psx/coro.h — thread-fiber for full-PSX mid-function
            // resume (later-264)
struct Core;

class PcScheduler {
public:
  Game *game = nullptr;

  jmp_buf yield_jmp;        // longjmp target = the setjmp in the running stanza (was
                            // g_yield_jmp)
  R3000 task_ctx[3] = {};   // saved CPU register context per task slot, registers
                            // only (was g_task_ctx)
  int in_stage = 0;         // 1 while inside a task run (gates the yield override) (was
                            // g_in_stage)
  int cur_slot = 0;         // task slot currently running (for the yield capture) (was g_cur_slot)
  int task_started[3] = {}; // slot has a live coroutine context (else fresh) (was g_task_started)
  int demo_native[3] = {};  // slot runs the DEMO/front-end as a NATIVE per-frame
                            // dispatcher (no guest coroutine): demo.frame() is
                            // called once per frame, state in guest RAM.
  int game_native[3] = {};  // slot runs the GAME stage as a NATIVE per-frame
                            // dispatcher (engine.frame() once per frame; state
                            // in guest RAM). Mirrors demo_native.
  int game_coop[3] = {};    // slot runs the GAME COOPERATIVE task loop (a GAME state not yet
                            // owned natively). As a PC game the per-frame cooperative yield is
                            // just a frame boundary: RE-ENTER the guest loop at its TOP
                            // (0x801063F4) every frame with the loop's callee-saved regs, instead
                            // of resuming at the saved mid-yield PC (which the substrate can't
                            // continue — the loop's C frame was longjmp'd away at the yield). All
                            // loop state lives in guest RAM, so re-entry == continue. (The loop
                            // body is the seeded ov_game_func_801063F4.)

  // ---- FULL-PSX (guest execution) thread-fiber coroutines (later-264)
  // ----------------------------- The native path above re-enters at a loop top
  // / runs synchronous dispatchers, so it never needs a true mid-function
  // resume. The FULL-PSX path (test configuration core B) runs pure
  // guest task bodies that yield mid-function (switch); the substrate
  // can't re-enter mid-body, so each such task runs on its OWN Coro thread that
  // BLOCKS at a yield (preserving its C stack) and CONTINUES on resume —
  // through the executor boundary. Active ONLY when
  // guest execution is on; the native path is untouched. cur_is_coro tells switch
  // to coro-yield (or Coro::exit_now on task-end) vs longjmp; Coro owns its own
  // unwind jmp_buf for end/cancel.
  Coro *coro[3] = {};  // per-slot fiber (heap; nullptr = no live full-PSX task on this slot)
  int cur_is_coro = 0; // 1 while a Coro task is running -> switch yields via the fiber

  // ---- Faithful-execution model (docs/faithful-execution.md, 2026-07-07)
  // ----------------------- A NATIVE ported task body can run on a Coro fiber
  // (same suspension mechanism core B's substrate bodies use): one resume per
  // runnable task per frame, suspension inside the ported yield primitive with
  // guest registers saved to task_ctx. Every instruction of the body is ported
  // native C++ — the fiber is only the parking mechanism, so this does not
  // route native execution=0 to the substrate. native_fiber[i] marks the slot's Coro as
  // a native body (rather than the runtime guest executor).
  int native_fiber[3] = {};
  uint32_t fiber_entry[3] = {}; // guest entry the native fiber was started for (teardown detect)

  // Ported guest scheduler primitives. Each reproduces its substrate body's
  // guest-stack discipline exactly (frame descent, ra/s-reg spills of the LIVE
  // register values, task-slot writes) so a strict differential test byte-compare holds.
  // Callers must have c->r[31] set to the guest call-site constant of THEIR
  // RE'd body before calling (the primitives spill it), exactly as a jal would.
  // ---- Synchronous native waits --------------------------------------------------------------
  // Native FUN_80044BD4 has one policy in every caller context: drain the spawned task before
  // returning. This removes both flat-task stack truncation and coroutine-only loading frames. The
  // explicit generated/oracle path retains the original multi-frame body for differential work.

  void yieldPrim(uint16_t mode); // FUN_80051F80: task yield (state=1, fiber-park)
  void spawnPrim(uint32_t slot,
                 uint32_t entry_pc); // FUN_80051F14: arm slot (entry/gp/state=2/tcb)
  void spawnAndWait(uint32_t fn, uint32_t p2, uint32_t p3,
                    uint32_t flag); // FUN_80044BD4
  void forceClose(uint32_t slot);   // FUN_80052010: close another slot
  void selfClose();                 // FUN_80051FB4: current task ends itself

  // Complete an already-owned synchronous wait. This is the same completion
  // seam used by spawnAndWait: flag!=1 retains the authored RNG stamp; no
  // native path synthesizes a wait tick or loading-screen service after the
  // host work has completed.
  SyncWaitCompletion completeSyncWait(uint32_t taskBase, uint32_t flag);
  // step(): one scheduler pass over the 3 task slots (replaces FUN_80051e60).
  void step();

  // tickSleepCountdown(): FUN_800506D0 — VERIFIED + WIRED (frontier tier,
  // 2026-07-10). The "sleep countdown / re-arm" sweep called once per frame
  // from native_boot.cpp's guest-call site. Binary analysis identifies 0x800506D0 as a leaf with no stack
  // frame (no sp descent, no ra use). Walks task slots 0..2 (TASKBASE +
  // i*TASKSTRIDE, matches "Tomba2 runs up to 3 cooperative tasks" in
  // scheduler.cpp): for each slot whose state (base+0x00, u16) == 1 (YIELDED,
  // per PcScheduler::yieldPrim's doc comment), decrement the countdown at
  // base+0x02 (u16); when it reaches exactly 0, re-arm the slot's state to 2
  // (RUNNABLE) — this is the "FUN_800506D0 re-arms 1->2" mechanism referenced
  // throughout scheduler.cpp/pc_scheduler.cpp comments as already-known
  // behavior but never previously drafted as a method body.
  void tickSleepCountdown();

  // True when entry_pc is one of the stage entries the PC port handles
  // natively.
  bool hasNativeHandlerForEntry(uint32_t entry_pc) const;

  // Counts consultations of the game's entry-PC seam, so "was this reached" is
  // answerable. Tomba!2's boot gate passes IDENTICALLY with an empty table, and
  // neither a 400-frame boot nor a 400-frame attract run consults this at all
  // (measured 2026-08-13) — it is the SUBSTRATE FALLBACK guard at
  // scheduler.cpp:157, reached only for a task the native path did not claim.
  // So a green gate is not evidence about this table, and the one-shot line the
  // first lookup prints is how a run says whether it touched it. `mutable`
  // because the accessor is const and this is a pure diagnostic touching no
  // guest state.
  mutable unsigned long schedLookupsSeen = 0;

private:
  enum StanzaResult { STANZA_NOT_MINE = 0, STANZA_HANDLED = 1 };
  void runDemoBody(Core *c, int i, bool demo_fresh);
  StanzaResult runDemoStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop);
  StanzaResult runSopAreaLoadStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop);
  StanzaResult runGameStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop);
  StanzaResult runTask1PreloadStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop);
  // pc_faithful STAGE-0: the whole ov_start arc (Engine::startBinStageFaithful)
  // as a native task body on a fiber — fresh at entry 0x8010649C, resumed while
  // suspended in the ported yield, torn down when FUN_80052078 swaps the entry
  // to DEMO (state=3).
  StanzaResult runStage0FiberStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop);
  int warned_demo_yield = 0; // warn-once latches for the frontier diagnostics
  int warned_game_yield = 0;
};
