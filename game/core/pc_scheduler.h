// PcScheduler — the PC-native cooperative task scheduler (replaces the PSX BIOS scheduler
// FUN_80051e60 and its thread layer). One instance per Game (c->game->pcSched). Owns the
// per-task bookkeeping (saved register contexts, run flags, step-spread counters) and the
// per-frame dispatch over the 3 task slots: PC-native stanzas (DEMO/SOP/GAME/task-1/STAGE-0)
// first, then the substrate coro-fiber / generic-dispatch stanzas in runtime/recomp/scheduler.cpp
// for un-ported tasks. The yield/spawn primitives (scheduler_yield, native_task_spawn) also live
// in scheduler.cpp and reach this state via c->game->pcSched.
//
// A task context is ONLY the CPU register file (R3000) — guest RAM/scratchpad/DMA/peripherals are
// SHARED one memory across all tasks (saving a whole Core would give each task its own RAM
// snapshot — the OOP regression where the loader task read a pre-fill file-table snapshot and
// stalled boot; see oop-regression-hunt). So task_ctx slices to the R3000 base on save/restore.
#pragma once
#include <stdint.h>
#include <setjmp.h>
#include "r3000.h"

class Game;
class Coro;          // runtime/recomp/coro.h — thread-fiber for full-PSX mid-function resume (later-264)
struct Core;
struct RecOverlay;   // generated overlay_table.h — descriptor of one recompiled overlay image

class PcScheduler {
public:
  Game* game = nullptr;

  jmp_buf yield_jmp;          // longjmp target = the setjmp in the running stanza (was g_yield_jmp)
  R3000   task_ctx[3] = {};   // saved CPU register context per task slot, registers only (was g_task_ctx)
  int     in_stage = 0;       // 1 while inside a task run (gates the yield override) (was g_in_stage)
  int     cur_slot = 0;       // task slot currently running (for the yield capture) (was g_cur_slot)
  int     task_started[3] = {};  // slot has a live coroutine context (else fresh) (was g_task_started)
  int     demo_native[3] = {};   // slot runs the DEMO/front-end as a NATIVE per-frame dispatcher (no guest
                                 // coroutine): demo.frame() is called once per frame, state in guest RAM.
  int     game_native[3] = {};   // slot runs the GAME stage as a NATIVE per-frame dispatcher (engine.frame()
                                 // once per frame; state in guest RAM). Mirrors demo_native.
  int     game_coop[3] = {};     // slot runs the GAME COOPERATIVE task loop (a GAME state not yet owned
                                 // natively). As a PC game the per-frame cooperative yield is just a frame
                                 // boundary: RE-ENTER the recompiled loop at its TOP (0x801063F4) every
                                 // frame with the loop's callee-saved regs, instead of resuming at the saved
                                 // mid-yield PC (which the substrate can't continue — the loop's C frame was
                                 // longjmp'd away at the yield). All loop state lives in guest RAM, so
                                 // re-entry == continue. (The loop body is the seeded ov_game_func_801063F4.)

  // ---- FULL-PSX (psx_fallback) thread-fiber coroutines (later-264) -----------------------------
  // The native path above re-enters at a loop top / runs synchronous dispatchers, so it never needs a
  // true mid-function resume. The FULL-PSX path (PSXPORT_SBS_MODE=gameplay/full core B) runs pure
  // recompiled task bodies that yield mid-function (switch); the substrate can't re-enter mid-body,
  // so each such task runs on its OWN Coro thread that BLOCKS at a yield (preserving its C stack) and
  // CONTINUES on resume — recompiler-only, no interpreter (USER 2026-06-30). Active ONLY when
  // psx_fallback is on; the native path is untouched. cur_is_coro tells switch to coro-yield (or
  // Coro::exit_now on task-end) vs longjmp; Coro owns its own unwind jmp_buf for end/cancel.
  Coro*   coro[3] = {};          // per-slot fiber (heap; nullptr = no live full-PSX task on this slot)
  int     cur_is_coro = 0;       // 1 while a Coro task is running -> switch yields via the fiber

  // ---- Faithful-execution model (docs/faithful-execution.md, 2026-07-07) -----------------------
  // A NATIVE ported task body can run on a Coro fiber (same suspension mechanism core B's substrate
  // bodies use): one resume per runnable task per frame, suspension inside the ported yield
  // primitive with guest registers saved to task_ctx. Every instruction of the body is ported
  // native C++ — the fiber is only the parking mechanism, so this does not route pc_skip=0 to the
  // substrate. native_fiber[i] marks the slot's Coro as a native body (vs a recomp substrate body).
  int     native_fiber[3] = {};

  // Ported guest scheduler primitives. Each reproduces its substrate body's guest-stack discipline
  // exactly (frame descent, ra/s-reg spills of the LIVE register values, task-slot writes) so a
  // strict SBS byte-compare holds. Callers must have c->r[31] set to the guest call-site constant
  // of THEIR RE'd body before calling (the primitives spill it), exactly as a jal would.
  void yieldPrim(uint16_t mode);                 // FUN_80051F80: task yield (state=1, fiber-park)
  void spawnPrim(uint32_t slot, uint32_t entry_pc); // FUN_80051F14: arm slot (entry/gp/state=2/tcb)
  void spawnAndWait(uint32_t fn, uint32_t p2, uint32_t p3, uint32_t flag); // FUN_80044BD4
  void forceClose(uint32_t slot);                // FUN_80052010: close another slot
  void selfClose();                              // FUN_80051FB4: current task ends itself
  // Wire the five primitives into game->engine_overrides at their guest addresses so substrate
  // callers on core A (via rec_dispatch) reach the same implementations. Core B (psx_fallback)
  // never consults the table.
  void registerOverrides();

  // ---- Native START.BIN (0x8010649C) step-spread (attack (a), docs/findings/sbs.md Slip #1) ----
  // The recomp body of 0x8010649C is a per-iteration yield loop over 4 sm[0x48] states; each state's
  // FUN_80044BD4 wait costs 1..2 ticks on B's coro. Native ran them all inline in ONE tick, collapsing
  // ~7 recomp ticks into 1 → the +1-frame residual that reasserts as Slip #1. Fix: split native work
  // over the same tick count via this per-task step counter; the scheduler re-enters an "advance"
  // branch each tick with stage0_step incremented, matching the recomp loop's cadence.
  //   step 0-1: preloadTexgroup(0,0) — matches recomp sm[0x48]==0 FUN_80044BD4(0x800CE858)
  //   step 2-3: preloadStage1()      — matches recomp sm[0x48]==1 FUN_80044BD4(0x800CD54C)
  //   step 4  : (no-op)              — matches recomp sm[0x48]==2 (advance-only)
  //   step 5  : native_start_stage(1)— matches recomp sm[0x48]==3 FUN_80052078(1) (swap to DEMO)
  // Steps 0/1 and 2/3 pair up to match the ~2-tick coro cost per FUN_80044BD4 wait; step 5 is where
  // the entry finally rewrites to DEMO (and iter ends via scheduler_yield on the swap).
  uint8_t stage0_step[3] = {};   // per-task step counter for the native START.BIN step-spread

  // ---- Native SOP fieldMode case-0 step-spread (attack (a), docs/findings/sbs.md Slip #2) ----
  // The recomp body of 0x80109450's case 0 calls FUN_80044BD4 (scratch/decomp/bd4.c) which spawns a
  // slot task and then `while (DAT_1f80019b == 0) FUN_80051f80(1)` — YIELDS AT LEAST ONCE waiting
  // for the callback. Native Sop::fieldMode case 0 runs `native_sop_area_load` INLINE = no yield,
  // completing in 1 tick vs coro's ~2. This flag defers native completion by one tick to match.
  //   0 = fresh entry into case 0; set to 1 and RETURN without touching sm[0x50] (defers case 0)
  //   1 = second tick; run the actual case 0 work, then reset to 0 for the next area load
  // Not per-task since only task 0 runs SOP; using [3] for consistency with the other arrays.
  uint8_t sop_field_step[3] = {};

  // ---- Native DEMO leave-substate step-spread (Slip #1 residual, docs/findings/sbs.md) ----
  // demo.frame() with sm[0x48]==5 dispatches the LEAVE-DEMO substate which calls native_start_stage
  // synchronously: entry rewrites from 0x801062E4 → 0x8010637C in the same scheduler tick. The coro
  // body of 0x801062E4's substate 5 does the same work but yields at least once through FUN_80051f80
  // before completing, so B's task-entry rewrite lands one tick LATER than A's. Defer A by one tick:
  //   0 = fresh sm[0x48]==5 tick; set to 1 and RETURN without calling demo.frame() (entry stays 801062E4)
  //   1 = second tick; actually dispatch substate 5 (native_start_stage rewrites entry), reset to 0
  // Per-task since demo runs only on task 0 in practice; array indexed for consistency with siblings.
  uint8_t demo_leave_step[3] = {};

  // ---- Native DEMO s0 preload-wait step (Slip #4, docs/findings/sbs.md) --------------------
  // The recomp body of 0x801062E4 state s0 dispatches FUN_80044BD4 which spawns task-1 with entry
  // FUN_80044F58 (preload) then YIELDS AT LEAST ONCE waiting for the callback. Native demo_frame_s0
  // used to run preloadTexgroup inline and complete s0 in ONE tick — putting A ~5 ticks ahead of B's
  // fiber (task-1 preload spans several ticks). Fix: split native s0 across ticks matching the
  // substrate cadence:
  //   0 = fresh s0 tick; run pre-yield (sm setup + loader + native_task_spawn(1, FUN_80044F58) +
  //       FUN_80044BD4 latches). Set step=1, suppress frame() dispatch of sm[0x48]==1 (s1), yield.
  //   1 = wait tick; if done_flag(0x1F80019B) == 0, yield (task-1 fiber still running); else run
  //       s0 post-yield tail (FUN_8007982C + reset75240 + FUN_8001CF00), reset step to 0.
  uint8_t demo_s0_step[3] = {};

  // Resident overlay per OVERLAP SLOT (0x80106228 stage / 0x80108F9C mode / 0x8018A000 area), recorded
  // by overlay_note_load() at LOAD time — when the freshly-written image still matches its raw .BIN
  // signature, BEFORE the game mutates its header pointer table at runtime. The router routes by this
  // IDENTITY (robust), falling back to a content-signature scan only when unset. Fixes the overlay-router
  // miss when GAME's header pointer @+0x08 is swapped at runtime so the raw-bytes signature no longer
  // matches (later-264). Per-instance for SBS (each core has its own resident set).
  const RecOverlay* resident_ov[3] = {};

  // step(): one scheduler pass over the 3 task slots (replaces FUN_80051e60).
  void step();

  // True when entry_pc is one of the stage entries the PC port handles natively.
  bool hasNativeHandlerForEntry(uint32_t entry_pc) const;

private:
  enum StanzaResult { STANZA_NOT_MINE = 0, STANZA_HANDLED = 1 };
  void runDemoBody(Core* c, int i, bool demo_fresh);
  StanzaResult runDemoStanza(Core* c, int i, uint32_t base, uint32_t st,
                             int native_content, const R3000& loop);
  StanzaResult runSopAreaLoadStanza(Core* c, int i, uint32_t base, uint32_t st,
                                    int native_content, const R3000& loop);
  StanzaResult runGameStanza(Core* c, int i, uint32_t base, uint32_t st,
                             int native_content, const R3000& loop);
  StanzaResult runTask1PreloadStanza(Core* c, int i, uint32_t base, uint32_t st,
                                     int native_content, const R3000& loop);
  StanzaResult runStage0StepStanza(Core* c, int i, uint32_t base, uint32_t st,
                                   int native_content);
  // pc_faithful STAGE-0: the whole ov_start arc (Engine::startBinStageFaithful) as a native task
  // body on a fiber — fresh at entry 0x8010649C, resumed while suspended in the ported yield,
  // torn down when FUN_80052078 swaps the entry to DEMO (state=3).
  StanzaResult runStage0FiberStanza(Core* c, int i, uint32_t base, uint32_t st,
                                    int native_content, const R3000& loop);
  int warned_demo_yield = 0;   // warn-once latches for the frontier diagnostics
  int warned_game_yield = 0;
};
