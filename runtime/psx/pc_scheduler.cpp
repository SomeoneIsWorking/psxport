// PcScheduler — native cooperative-task handlers plus per-frame slot dispatch. Title-owned native
// handlers run first; unowned tasks resume through the guest scheduler in runtime/psx/scheduler.cpp.
#include "pc_scheduler.h"
#include "c_subsys.h" // xa_stream_owns_slot2/xa_stream_voice_busy/xa_stream_voice_release
#include "core.h"
#include "coro.h" // native task bodies park on a Coro fiber across frame boundaries
#include "game.h"
#include "guest_call.h"
#include "scheduler.h" // TASKBASE/TASKSTRIDE/CUR_TASK + guest stanzas + yield primitive
#include "synchronous_task_wait.h"
#include <lucent/log.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h> // abort — the fail-fast on a wait that can never complete

// Native scheduler primitives recovered from the original guest binary. Every frame descent, spill
// offset, and task-slot write preserves the recovered guest ABI; spill values remain live guest
// registers so nested callee spills land at their original locations.

// Scratchpad/task-table anchors shared by the primitives (RE: FUN_80051E60
// scheduler pass).
static constexpr uint32_t kCurTaskPtr = 0x1F800138u; // scratchpad: current-task object ptr

// FUN_80051F80 — cooperative yield. Frame: sp-=24, ra spill at +16. Sets
// task[+0x02]=mode, task[+0x00]=1 (YIELDED; FUN_800506D0 re-arms 1->2 next
// frame), then ChangeThread (scheduler_yield: fiber-park on a Coro task,
// longjmp on a flat task, no-op outside a task). The resume path is
// FUN_80051FA4 (ra reload + frame ascent) — also what a flat task's saved
// r31=0x80051FA4 re-enters through the generic dispatch stanza.
void PcScheduler::yieldPrim(uint16_t mode) {
  Core *c = &game->core;
  c->r[29] -= 24;
  const uint32_t task = c->mem_r32(kCurTaskPtr);
  c->mem_w32(c->r[29] + 16, c->r[31]);
  c->mem_w16(task + 0x02, mode);
  c->mem_w16(task + 0x00, 1);
  c->r[2] = 1;
  c->r[3] = task;
  c->r[4] = 0xFF000000u;
  c->r[31] = 0x80051FA4u; // guest resume PC (frame-ascent epilogue)
  scheduler_yield(c);
  // resumed (or the outside-a-task no-op): FUN_80051FA4 epilogue
  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80051F14 — arm a task slot. Frame: sp-=24, s0 spill at +16, ra at +20.
// Slot writes: +0x0C=entry, +0x10=caller gp (FUN_80080930), +0x00=2 (RUNNABLE),
// +0x6F=0, +0x04=BIOS OpenTh handle (0xFF000000 placeholder — threads.cpp
// thread_open). The BIOS-thread create maps to arming the slot: the scheduler
// stanzas pick the RUNNABLE slot up on this frame's pass.
void PcScheduler::spawnPrim(uint32_t slot, uint32_t entry_pc) {
  Core *c = &game->core;
  c->r[29] -= 24;
  const uint32_t base = TASKBASE + slot * TASKSTRIDE;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = base;
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->mem_w32(base + 0x0C, entry_pc);
  c->r[31] = 0x80051F40u;
  c->mem_w32(base + 0x10, c->r[28]); // FUN_80080930 returns the caller's gp
  c->mem_w16(base + 0x00, 2);        // RUNNABLE
  c->r[31] = 0x80051F54u;
  c->mem_w8(base + 0x6F, 0);
  psx::cpu::dispatchGuestToReturn0(
      *c, 0x80080890u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::spawnPrim EnterCriticalSection");
  c->r[4] = c->mem_r32(base + 0x0C);
  c->r[5] = c->mem_r32(base + 0x08);
  c->r[6] = c->mem_r32(base + 0x10);
  c->r[31] = 0x80051F68u;
  psx::cpu::dispatchGuestToReturn0(
      *c, 0x80080860u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::spawnPrim OpenTh");
  c->r[31] = 0x80051F70u;
  c->mem_w32(base + 0x04, c->r[2]);
  psx::cpu::dispatchGuestToReturn0(
      *c, 0x800808A0u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::spawnPrim ExitCriticalSection");
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80052010 — force-close a slot. Frame: sp-=24, s0 spill at +16, ra at +20
// (written even on the already-closed early-out, per the guest prologue).
// Body: state=0, +0x6C=0, +0x6F=0, EnterCS, CloseTh(task[+0x04]), ExitCS.
void PcScheduler::forceClose(uint32_t slot) {
  Core *c = &game->core;
  c->r[29] -= 24;
  const uint32_t base = TASKBASE + slot * TASKSTRIDE;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = base;
  c->mem_w32(c->r[29] + 20, c->r[31]);
  if (c->mem_r16(base + 0x00) != 0) {
    c->mem_w16(base + 0x00, 0);
    c->mem_w8(base + 0x6C, 0);
    c->r[31] = 0x80052054u;
    c->mem_w8(base + 0x6F, 0);
    psx::cpu::dispatchGuestToReturn0(
        *c, 0x80080890u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::forceClose EnterCriticalSection");
    c->r[4] = c->mem_r32(base + 0x04);
    c->r[31] = 0x80052060u;
    psx::cpu::dispatchGuestToReturn0(
        *c, 0x80080870u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::forceClose CloseTh");
    c->r[31] = 0x80052068u;
    psx::cpu::dispatchGuestToReturn0(
        *c, 0x800808A0u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::forceClose ExitCriticalSection");
  }
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80051FB4 — current task ends itself. Frame: sp-=24, s0 spill at +16, ra
// at +20. Sets state=0 then ChangeThread: on a fiber task scheduler_yield sees
// state==0 and unwinds the fiber (Coro::exit_now); on a flat task it longjmps
// to the stanza; outside a task it returns and the epilogue runs.
void PcScheduler::selfClose() {
  Core *c = &game->core;
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = 0x1F800000u;
  const uint32_t task = c->mem_r32(kCurTaskPtr);
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->mem_w8(task + 0x6C, 0);
  c->mem_w16(task + 0x00, 0); // ENDED
  c->r[31] = 0x80051FDCu;
  c->mem_w8(task + 0x6F, 0);
  psx::cpu::dispatchGuestToReturn0(
      *c, 0x80080890u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::selfClose EnterCriticalSection");
  c->r[4] = c->mem_r32(task + 0x04);
  c->r[31] = 0x80051FF0u;
  psx::cpu::dispatchGuestToReturn0(
      *c, 0x80080870u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::selfClose CloseTh");
  c->r[31] = 0x80051FF8u;
  psx::cpu::dispatchGuestToReturn0(
      *c, 0x800808A0u, psx::cpu::ExecutionBudget::currentTurn(*c), "PcScheduler::selfClose ExitCriticalSection");
  c->r[4] = 0xFF000000u;
  c->r[31] = 0x80052000u;
  scheduler_yield(c); // ChangeThread — never returns on a task
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

SyncWaitCompletion PcScheduler::completeSyncWait(uint32_t taskBase, uint32_t flag) {
  return SynchronousTaskWait::finish(*this, taskBase, flag);
}

// FUN_80044BD4 — spawn a slot-1 task and wait for its done_flag. Frame: sp-=40,
// spills at +16..+32 hold the caller's LIVE s0..s3 + ra; the body then keeps
// fn/flag/p2/p3 in s-regs (r18/r19/r17/r16) so nested callee spills
// (spawnPrim's s0 etc.) hold live values too. The native owner completes the task synchronously;
// ordinary guest execution retains the original multi-frame wait.
void PcScheduler::spawnAndWait(uint32_t fn, uint32_t p2, uint32_t p3, uint32_t flag) {
  SynchronousTaskWait::run(*this, fn, p2, p3, flag);
}

// Native handlers are selected only by the title-declared entry table. The framework never embeds a
// title's scheduler-entry mapping; a miss continues through ordinary guest execution.
//
// THE COUNTERS EXIST BECAUSE THE BOOT GATE CANNOT SEE THIS CODE. Measured
// 2026-08-13: Tomba!2's `gate.py boot --frames 400` passes IDENTICALLY with
// `schedEntryCount = 0` — same 133 lines, same stage=8010637C, same sm48=2. So
// a green boot gate is NOT evidence that this table is right, and without a
// reached-counter there is no way to tell "the declaration is correct" from
// "nothing ever asked". `schedLookupsSeen` is the DENOMINATOR: a run reporting
// 0 hits out of 0 lookups never exercised the seam, and must not be read as
// agreement.
bool PcScheduler::hasNativeHandlerForEntry(uint32_t entry_pc) const {
  const GameConfig::SchedEntry *e = sched_entry_for(game->core.cfg, entry_pc);
  if (schedLookupsSeen++ == 0) {
    const GameConfig *cfg = game->core.cfg;
    lucent::info("sched",
                 "entry-PC seam FIRST CONSULTED: entry 0x{:08X} {} one of {} "
                 "declared entry/entries. "
                 "A run whose log lacks this line never reached the seam at "
                 "all, and says nothing "
                 "about whether the declared table is correct.",
                 entry_pc,
                 e ? "MATCHED" : "did NOT match",
                 cfg ? cfg->schedEntryCount : 0u);
  }
  return e && e->nativeHandler;
}

// Reported at exit by whoever owns shutdown, so a run states whether the
// scheduler seam was reached at all. Not behind a debug channel: a silent 0 is
// exactly the failure this is here to make visible.

// DEMO 0x801062E4 — native per-frame dispatcher. Fresh entry runs stageMain then frame(); resume runs
// frame(). Leave-defer preserves the guest coroutine's FUN_80051F80 yield cost. Rewriting the entry
// drops native ownership so the replacement entry is selected on the next tick.
void PcScheduler::runDemoBody(Core *c, int i, bool demo_fresh) {
  if (demo_fresh) {
    c->hooks->schedStageBody(c, SCHED_DEMO_STAGEMAIN, nullptr);
  }
  c->hooks->schedStageBody(c, SCHED_DEMO_FRAME, nullptr);
}

PcScheduler::StanzaResult
PcScheduler::runDemoStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop) {
  int demo_fresh =
      native_content && (st == 3 || (st == 2 && !task_started[i])) && c->mem_r32(base + 0xc) == 0x801062E4u;
  if (!demo_fresh && !(demo_native[i] && st == 2 && task_started[i])) {
    return STANZA_NOT_MINE;
  }
  if (demo_fresh) {
    task_ctx[i] = loop;                       // inherit gp
    task_ctx[i].r[29] = c->mem_r32(base + 8); // per-task PSX stack top
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    demo_native[i] = 1;
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000 &>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {
    runDemoBody(c, i, demo_fresh);
  } else {
    if (!warned_demo_yield++) {
      lucent::debug("demo",
                    "caught a substate yield (async CD not yet "
                    "owned native+sync) — frontier");
    }
  }
  in_stage = 0;
  if (c->mem_r32(base + 0xc) != 0x801062E4u) { // s5 -> GAME rewrote entry
    demo_native[i] = 0;
    task_started[i] = 0;
    return STANZA_HANDLED;
  }
  task_ctx[i] = static_cast<R3000 &>(*c);
  c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// SOP area-load 0x80109164 — SOP.BIN's cooperative slot-1 loader run
// synchronously (all leaves are sync CD reads). Fresh-only: run areaLoad, mark
// task done. With guest execution on the guest body runs as a normal cooperative
// task via the fiber stanza (its FUN_80051fb4 yield is serviced).
PcScheduler::StanzaResult
PcScheduler::runSopAreaLoadStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop) {
  int sop_fresh = (st == 3 || (st == 2 && !task_started[i])) && c->mem_r32(base + 0xc) == 0x80109164u;
  if (!(sop_fresh && native_content)) {
    return STANZA_NOT_MINE;
  }
  task_ctx[i] = loop;
  task_ctx[i].r[29] = c->mem_r32(base + 8);
  task_ctx[i].r[31] = 0xDEAD0000u;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000 &>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {
    c->hooks->schedStageBody(c, SCHED_SOP_AREALOAD, nullptr);
  } else {
    lucent::debug("sched", "SOP area-load yielded unexpectedly — a leaf isn't sync yet");
  }
  in_stage = 0;
  c->mem_w16(base, 0);
  task_started[i] = 0;
  return STANZA_HANDLED;
}

// GAME 0x8010637C — native per-frame dispatcher. Fresh: stagePrologue +
// frame(). Resume: frame(). frame() returns 0 when its current sm[0x48] state
// isn't owned natively — fall back to game_coop: hand the task to the guest
// cooperative loop 0x801063F4 with the loop's callee-saved regs reset, so the
// generic path drives it next tick. Entry rewrite (area transition): drop
// game_native.
PcScheduler::StanzaResult
PcScheduler::runGameStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop) {
  int game_fresh = (st == 3 || (st == 2 && !task_started[i])) && c->mem_r32(base + 0xc) == 0x8010637Cu;
  if (!native_content) {
    return STANZA_NOT_MINE;
  }
  if (!game_fresh && !(game_native[i] && st == 2 && task_started[i])) {
    return STANZA_NOT_MINE;
  }
  if (game_fresh) {
    task_ctx[i] = loop;
    task_ctx[i].r[29] = c->mem_r32(base + 8);
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    game_native[i] = 1;
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000 &>(*c) = task_ctx[i];
  in_stage = 1;
  int handled = 1;
  if (setjmp(yield_jmp) == 0) {
    if (game_fresh) {
      c->hooks->schedStageBody(c, SCHED_GAME_PROLOGUE, nullptr);
    }
    handled = c->hooks->schedStageBody(c, SCHED_GAME_FRAME, nullptr);
  } else {
    if (!warned_game_yield++) {
      lucent::debug("sched",
                    "caught a GAME substate yield (a leaf not "
                    "yet sync) — frontier");
    }
  }
  in_stage = 0;
  if (c->mem_r32(base + 0xc) != 0x8010637Cu) { // area transition rewrote entry
    game_native[i] = 0;
    task_started[i] = 0;
    return STANZA_HANDLED;
  }
  if (!handled) { // fall back to guest cooperative loop
    c->r[16] = 0x1f800000u;
    c->r[17] = 0x1f800000u;
    c->r[18] = 1;
    c->r[31] = 0x801063F4u;
    task_ctx[i] = static_cast<R3000 &>(*c);
    game_native[i] = 0;
    game_coop[i] = 1;
    c->mem_w16(base, 2);
    lucent::debug("sched",
                  "GAME -> cooperative guest loop (state not yet "
                  "owned native; field reachable)");
    return STANZA_HANDLED;
  }
  task_ctx[i] = static_cast<R3000 &>(*c);
  c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// Native task-1 preload body, selected by its fresh-entry PC:
//   0x80044F58 → Asset::loadTexgroup   (per-set texgroup loader)
//   0x8004514C → Asset::preloadStage1  (SWDATA+DAT+relocation+VRAM build)
// Both set done_flag and guest dispatch 0x80051FB4 (task-end) so the caller of
// FUN_80044BD4's wait-loop sees completion in the same tick.
PcScheduler::StanzaResult
PcScheduler::runTask1PreloadStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop) {
  if (!native_content) {
    return STANZA_NOT_MINE;
  }
  Coro *&co = coro[i];
  const int fresh = (st == 3 || (st == 2 && !task_started[i]));
  if (fresh) {
    const uint32_t entry_pc = c->mem_r32(base + 0xc);
    // Which coro body a fresh task at this entry starts is the GAME's
    // declaration, not a literal here.
    const GameConfig::SchedEntry *se = sched_entry_for(c->cfg, entry_pc);
    if (!se || !se->hasFiberBody) {
      return STANZA_NOT_MINE;
    }
    if (co) {
      delete co;
      co = nullptr;
    } // ~Coro cancels a blocked fiber
    task_ctx[i] = loop;
    task_ctx[i].r[29] = c->mem_r32(base + 8);
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    native_fiber[i] = 1;
    Core *cc = c;
    co = new Coro();
    // The body kind is the game's declaration; the framework supplies no default for an unknown
    // entry.
    const SchedBody kind = se->fiberBody;
    co->start([cc, kind] {
      cc->hooks->schedStageBody(cc, kind, nullptr);
    });
  } else if (!native_fiber[i]) {
    return STANZA_NOT_MINE;
  } else if (st != 2 || !co || co->done()) {
    if (st == 2) {
      task_started[i] = 0;
      native_fiber[i] = 0;
    }
    return STANZA_HANDLED; // sleeping (st==1) or dead fiber
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  in_stage = 1;
  cur_is_coro = 1;
  static_cast<R3000 &>(*c) = task_ctx[i];
  lucent::debug(
      "sched", "slot {} native-fiber {} st={} sp=0x{:08X}", i, fresh ? "start" : "resume", st, task_ctx[i].r[29]);
  co->resume();
  cur_is_coro = 0;
  in_stage = 0;
  if (co->done() || c->mem_r16(base) == 0) {
    c->mem_w16(base, 0);
    task_started[i] = 0;
    native_fiber[i] = 0;
    delete co;
    co = nullptr;
  }
  return STANZA_HANDLED;
}

// STAGE-0 — the ov_start arc as a native task body on a Coro fiber. Fresh entry 0x8010649C starts
// with the task's guest stack and frame-loop registers. Resume calls co->resume() once per runnable
// frame; the body
// parks inside PcScheduler::yieldPrim with guest regs saved. Teardown: when the
// body's sm==3 arm dispatches FUN_80052078 the entry rewrites to DEMO and
// state=3 — cancel the fiber (its abandoned frames are plain data; the body
// holds no destructibles across yields) so the DEMO stanza takes the slot fresh
// next tick.
PcScheduler::StanzaResult
PcScheduler::runStage0FiberStanza(Core *c, int i, uint32_t base, uint32_t st, int native_content, const R3000 &loop) {
  if (!native_content) {
    return STANZA_NOT_MINE;
  }
  Coro *&co = coro[i];
  const int fresh = (st == 3 || (st == 2 && !task_started[i]));
  if (fresh) {
    const uint32_t entry = c->mem_r32(base + 0xc);
    if (entry != 0x8010649Cu && entry != 0x801062E4u && entry != 0x8010637Cu) {
      return STANZA_NOT_MINE;
    }
    if (co) {
      delete co;
      co = nullptr;
    } // ~Coro cancels a blocked fiber
    task_ctx[i] = loop;
    task_ctx[i].r[29] = c->mem_r32(base + 8);
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    native_fiber[i] = 1;
    fiber_entry[i] = entry;
    demo_native[i] = 0;
    game_native[i] = 0;
    game_coop[i] = 0;
    Core *cc = c;
    co = new Coro();
    if (entry == 0x8010649Cu) {
      co->start([cc] {
        cc->hooks->schedStageBody(cc, SCHED_FIBER_STARTBIN, nullptr);
      });
    } else if (entry == 0x801062E4u) {
      co->start([cc] {
        cc->hooks->schedStageBody(cc, SCHED_FIBER_DEMO_BODY, nullptr);
      });
    } else {
      co->start([cc] {
        cc->hooks->schedStageBody(cc, SCHED_FIBER_STAGE_BODY, nullptr);
      });
    }
  } else if (!native_fiber[i]) {
    return STANZA_NOT_MINE;
  } else if (st != 2 || !co || co->done()) {
    if (st == 2) {
      task_started[i] = 0;
      native_fiber[i] = 0;
    }
    return STANZA_HANDLED; // sleeping (st==1) or dead fiber
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  in_stage = 1;
  cur_is_coro = 1;
  static_cast<R3000 &>(*c) = task_ctx[i];
  lucent::debug(
      "sched", "slot {} native-fiber {} st={} sp=0x{:08X}", i, fresh ? "start" : "resume", st, task_ctx[i].r[29]);
  co->resume();
  cur_is_coro = 0;
  in_stage = 0;
  if (co->done() || c->mem_r16(base) == 0) {
    c->mem_w16(base, 0);
    task_started[i] = 0;
    native_fiber[i] = 0;
    delete co;
    co = nullptr;
  } else if (c->mem_r32(base + 0xc) != fiber_entry[i]) {
    // FUN_80052078 swapped the stage (entry rewritten, state=3): the parked
    // body will never be resumed — tear the fiber down so the new stage's
    // stanza starts fresh.
    task_started[i] = 0;
    native_fiber[i] = 0;
    delete co;
    co = nullptr;
  }
  return STANZA_HANDLED;
}

// One scheduler pass over the 3 task slots (replaces FUN_80051e60). The main
// loop is purely slot iteration and dispatch: native handlers first, then guest coroutine and
// generic-dispatch handlers for unowned tasks.
void PcScheduler::step() {
  Core *c = &game->core;
  R3000 loop = *c; // frame-loop REGISTERS (gp etc. for fresh tasks); slices off RAM
  for (int i = 0; i < 3; i++) {
    uint32_t base = TASKBASE + (uint32_t)i * TASKSTRIDE;
    // Task slot 2 = XA voice/BGM. When the native clip player owns it, do NOT
    // run the (now unused) FUN_8001cfc8 guest coroutine; reflect clip state
    // into task-2's state byte so the cutscene's `while (DAT_801fe0e0 != 0)`
    // wait advances exactly when the clip finishes.
    if (i == 2 && xa_stream_owns_slot2(&c->game->xa)) {
      if (xa_stream_voice_busy(&c->game->xa)) {
        c->mem_w16(base, 2);
      } else {
        c->mem_w16(base, 0);
        xa_stream_voice_release(&c->game->xa);
      }
      task_started[2] = 0;
      continue;
    }
    uint32_t st = c->mem_r16(base);
    if (st == 0) {
      task_started[i] = 0;
      demo_native[i] = 0;
      continue;
    }
    constexpr int native_content = 1;
    if (runDemoStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED) {
      continue;
    }
    if (runSopAreaLoadStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED) {
      continue;
    }
    if (runGameStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED) {
      continue;
    }
    if (guest_run_coro_fiber_stanza(c, i, base, st, native_content != 0, loop)) {
      continue;
    }
    guest_run_dispatch_stanza(c, i, base, st, native_content != 0, loop);
  }
  static_cast<R3000 &>(*c) = loop; // restore the frame-loop REGISTERS (shared RAM untouched)
}

// FUN_800506D0 — VERIFIED + WIRED (frontier tier, 2026-07-10). See
// pc_scheduler.h for the RE summary. Retail address 0x800506D0 has no frame descent
// (leaf, no sp/ra touched) and performs a 3-slot sweep over
// TASKBASE+i*TASKSTRIDE for i=0..2 (verified against the guest loop bound
// 0x801FE000..0x801FE14F inclusive-by-112 == exactly 3 iterations), state==1 ->
// decrement countdown at +2 -> re-arm to 2 on underflow-to-exactly-zero. The
// Callers may replace the guest leaf with this native owner only after differential verification.
void PcScheduler::tickSleepCountdown() {
  Core *c = &game->core;
  for (uint32_t base = TASKBASE; base <= TASKBASE + 2u * TASKSTRIDE; base += TASKSTRIDE) {
    if (c->mem_r16(base + 0x00u) != 1u) {
      continue; // only YIELDED (state==1) slots tick
    }
    uint16_t countdown = (uint16_t)(c->mem_r16(base + 0x02u) - 1u);
    c->mem_w16(base + 0x02u, countdown);
    if (countdown == 0u) {
      c->mem_w16(base + 0x00u, 2u); // re-arm 1 -> 2 (RUNNABLE)
    }
  }
}
