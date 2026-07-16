// PcScheduler — PC-native cooperative-task stanzas + the per-frame slot dispatch (replaces the
// PSX BIOS scheduler FUN_80051e60 for tasks the port owns natively). The stanzas run under BOTH
// pc_skip modes: pc_skip=true uses shortcut branches, pc_skip=false uses faithful byte-exact
// branches. Un-ported tasks fall through to the substrate coro-fiber / generic-dispatch stanzas
// (runtime/recomp/scheduler.cpp), which also owns the yield/spawn primitives.
#include "pc_scheduler.h"
#include "core.h"
#include "game.h"
#include "cfg.h"
#include "scheduler.h"  // TASKBASE/TASKSTRIDE/CUR_TASK + the substrate stanzas + yield primitive
#include "c_subsys.h"   // xa_stream_owns_slot2/xa_stream_voice_busy/xa_stream_voice_release
#include "coro.h"       // native task bodies park on the same Coro fiber the substrate bodies use
#include "guest_call.h" // rec_dispatch — BIOS leaves + still-substrate leaves from the primitives
#include "override_registry.h" // overrides::install — the one native-override registry
#include "rng.h"        // Rng (c->rng) — FUN_8009A450, shared guest seed 0x80105EE8
#include <setjmp.h>
#include <stdio.h>

// ---- Ported guest scheduler primitives (docs/faithful-execution.md) --------------------------
// Byte-shape source: generated recomp bodies gen_func_80051F80 / 80051F14 / 80044BD4 / 80052010 /
// 80051FB4 (shard_2/3/5/6.c) + Ghidra decomps (scratch/decomp/task_spawn.c, ram_f1000_all.c
// 31833-31990). Every frame descent, spill offset and task-slot write below is that RE; the spill
// VALUES are the live guest registers, so nested callee spills land organically.

// Scratchpad/task-table anchors shared by the primitives (RE: FUN_80051E60 scheduler pass).
static constexpr uint32_t kCurTaskPtr   = 0x1F800138u;  // scratchpad: current-task object ptr
static constexpr uint32_t kDoneFlag     = 0x1F80019Bu;  // spawn-and-wait completion flag
static constexpr uint32_t kWaitFrameCtr = 0x1F800198u;  // flag==2 wait-loop frame counter
static constexpr uint32_t kSpawnParam3  = 0x801FE0DDu;  // FUN_80044BD4 param_3 latch
static constexpr uint32_t kSpawnParam2  = 0x801FE0DEu;  // FUN_80044BD4 param_2 latch
static constexpr uint32_t kTask1State   = 0x801FE070u;  // task-1 slot state hword

// FUN_80051F80 — cooperative yield. Frame: sp-=24, ra spill at +16. Sets task[+0x02]=mode,
// task[+0x00]=1 (YIELDED; FUN_800506D0 re-arms 1->2 next frame), then ChangeThread
// (scheduler_yield: fiber-park on a Coro task, longjmp on a flat task, no-op outside a task).
// The resume path is FUN_80051FA4 (ra reload + frame ascent) — also what a flat task's saved
// r31=0x80051FA4 re-enters through the generic dispatch stanza.
void PcScheduler::yieldPrim(uint16_t mode) {
  Core* c = &game->core;
  c->r[29] -= 24;
  const uint32_t task = c->mem_r32(kCurTaskPtr);
  c->mem_w32(c->r[29] + 16, c->r[31]);
  c->mem_w16(task + 0x02, mode);
  c->mem_w16(task + 0x00, 1);
  c->r[2] = 1; c->r[3] = task; c->r[4] = 0xFF000000u;
  c->r[31] = 0x80051FA4u;              // substrate resume PC (frame-ascent epilogue)
  scheduler_yield(c);
  // resumed (or the outside-a-task no-op): FUN_80051FA4 epilogue
  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80051F14 — arm a task slot. Frame: sp-=24, s0 spill at +16, ra at +20. Slot writes:
// +0x0C=entry, +0x10=caller gp (FUN_80080930), +0x00=2 (RUNNABLE), +0x6F=0, +0x04=BIOS OpenTh
// handle (0xFF000000 placeholder — threads.cpp thread_open). The BIOS-thread create maps to
// arming the slot: the scheduler stanzas pick the RUNNABLE slot up on this frame's pass.
void PcScheduler::spawnPrim(uint32_t slot, uint32_t entry_pc) {
  Core* c = &game->core;
  c->r[29] -= 24;
  const uint32_t base = TASKBASE + slot * TASKSTRIDE;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = base;
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->mem_w32(base + 0x0C, entry_pc);
  c->r[31] = 0x80051F40u;
  c->mem_w32(base + 0x10, c->r[28]);   // FUN_80080930 returns the caller's gp
  c->mem_w16(base + 0x00, 2);          // RUNNABLE
  c->r[31] = 0x80051F54u;
  c->mem_w8(base + 0x6F, 0);
  rec_dispatch(c, 0x80080890u);        // EnterCriticalSection
  c->r[4] = c->mem_r32(base + 0x0C);
  c->r[5] = c->mem_r32(base + 0x08);
  c->r[6] = c->mem_r32(base + 0x10);
  c->r[31] = 0x80051F68u;
  rec_dispatch(c, 0x80080860u);        // BIOS OpenTh -> 0xFF000000 placeholder handle in v0
  c->r[31] = 0x80051F70u;
  c->mem_w32(base + 0x04, c->r[2]);
  rec_dispatch(c, 0x800808A0u);        // ExitCriticalSection
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80052010 — force-close a slot. Frame: sp-=24, s0 spill at +16, ra at +20 (written even on
// the already-closed early-out, per the recomp prologue). Body: state=0, +0x6C=0, +0x6F=0,
// EnterCS, CloseTh(task[+0x04]), ExitCS.
void PcScheduler::forceClose(uint32_t slot) {
  Core* c = &game->core;
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
    rec_dispatch(c, 0x80080890u);      // EnterCriticalSection
    c->r[4] = c->mem_r32(base + 0x04);
    c->r[31] = 0x80052060u;
    rec_dispatch(c, 0x80080870u);      // BIOS CloseTh
    c->r[31] = 0x80052068u;
    rec_dispatch(c, 0x800808A0u);      // ExitCriticalSection
  }
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80051FB4 — current task ends itself. Frame: sp-=24, s0 spill at +16, ra at +20. Sets
// state=0 then ChangeThread: on a fiber task scheduler_yield sees state==0 and unwinds the
// fiber (Coro::exit_now); on a flat task it longjmps to the stanza; outside a task it returns
// and the epilogue runs.
void PcScheduler::selfClose() {
  Core* c = &game->core;
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = 0x1F800000u;
  const uint32_t task = c->mem_r32(kCurTaskPtr);
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->mem_w8(task + 0x6C, 0);
  c->mem_w16(task + 0x00, 0);          // ENDED
  c->r[31] = 0x80051FDCu;
  c->mem_w8(task + 0x6F, 0);
  rec_dispatch(c, 0x80080890u);        // EnterCriticalSection
  c->r[4] = c->mem_r32(task + 0x04);
  c->r[31] = 0x80051FF0u;
  rec_dispatch(c, 0x80080870u);        // BIOS CloseTh
  c->r[31] = 0x80051FF8u;
  rec_dispatch(c, 0x800808A0u);        // ExitCriticalSection
  c->r[4] = 0xFF000000u;
  c->r[31] = 0x80052000u;
  scheduler_yield(c);                  // ChangeThread — never returns on a task
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// bd4Tail — shared FUN_80044BD4 a3!=1 tail: RNG stamp store at taskBase+0x56, and (flag==2 only)
// one wait-frame-counter bump + FUN_8007FD54 dispatch. See pc_scheduler.h for the full rationale
// (docs/findings/scene.md "pc_skip FUN_80044BD4-collapse INCOMPLETENESS class"). Every pc_skip
// collapse site that reproduces FUN_80044BD4's a3!=1 branch AS A SINGLE SYNCHRONOUS TICK (demo.cpp,
// engine.cpp Engine::submode1Case0Skip, sop.cpp Sop::fieldMode case 0) calls this instead of
// re-deriving the tail per site. spawnAndWait below keeps its own inline tail: its flag==2 bump+
// dispatch is gated by done==0 and repeats once per yielded wait-loop iteration (not a single shot),
// a shape this single-shot helper doesn't model — factoring it in would risk the wait loop's
// byte-exactness for no benefit (the ONE call site, the reference this helper was extracted from).
void PcScheduler::bd4Tail(uint32_t taskBase, uint32_t flag) {
  Core* c = &game->core;
  const uint16_t stamp = (uint16_t)c->rng.next();          // FUN_8009A450 (guest seed 0x80105EE8)
  c->mem_w16(taskBase + 0x56, stamp);                       // RNG stamp on the waiting task
  if (flag == 2) {
    c->mem_w16(kWaitFrameCtr, (uint16_t)(c->mem_r16(kWaitFrameCtr) + 1));
    rec_dispatch(c, 0x8007FD54u);                           // flag==2 per-wait-frame service
  }
}

// FUN_80044BD4 — spawn a slot-1 task and wait for its done_flag. Frame: sp-=40, spills at
// +16..+32 hold the caller's LIVE s0..s3 + ra; the body then keeps fn/flag/p2/p3 in s-regs
// (r18/r19/r17/r16) so nested callee spills (spawnPrim's s0 etc.) hold live values too.
void PcScheduler::spawnAndWait(uint32_t fn, uint32_t p2, uint32_t p3, uint32_t flag) {
  Core* c = &game->core;
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 24, c->r[18]); c->r[18] = fn;
  c->mem_w32(c->r[29] + 28, c->r[19]); c->r[19] = flag;
  c->mem_w32(c->r[29] + 20, c->r[17]); c->r[17] = p2;
  c->mem_w32(c->r[29] + 16, c->r[16]); c->r[16] = p3;
  c->mem_w32(c->r[29] + 32, c->r[31]);
  while (c->mem_r16(kTask1State) != 0) {           // drain: a live task-1 finishes first
    c->r[4] = 1; c->r[31] = 0x80044C10u;
    yieldPrim(1);
  }
  c->r[4] = 2; c->r[31] = 0x80044C2Cu;
  forceClose(2);
  c->mem_w8(kSpawnParam2, (uint8_t)c->r[17]);
  c->r[17] = 0x1F800000u;
  c->mem_w8(kSpawnParam3, (uint8_t)c->r[16]);
  c->mem_w8(kDoneFlag, 0);
  c->r[4] = 1; c->r[5] = c->r[18]; c->r[31] = 0x80044C50u;
  spawnPrim(1, c->r[18]);
  if (c->r[19] != 1) {
    c->r[31] = 0x80044C64u;
    const uint16_t stamp = (uint16_t)c->rng.next();        // FUN_8009A450 (guest seed 0x80105EE8)
    const uint32_t task = c->mem_r32(kCurTaskPtr);
    const uint8_t done = c->mem_r8(kDoneFlag);
    c->mem_w16(task + 0x56, stamp);                        // RNG stamp on the waiting task
    if (done == 0) {
      c->r[18] = 2; c->r[16] = 0x1F800000u;                // wait-loop register state (RE)
      do {
        if (c->r[19] == 2) {
          c->mem_w16(kWaitFrameCtr, (uint16_t)(c->mem_r16(kWaitFrameCtr) + 1));
          c->r[31] = 0x80044CA0u;
          rec_dispatch(c, 0x8007FD54u);                    // flag==2 per-wait-frame service
        }
        c->r[4] = 1; c->r[31] = 0x80044CA8u;
        yieldPrim(1);                                      // parks the fiber one frame
      } while (c->mem_r8(kDoneFlag) == 0);
    }
  }
  c->r[31] = c->mem_r32(c->r[29] + 32);
  c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 40;
}

// Guest-ABI trampolines: substrate callers reach the ported primitives through rec_dispatch
// (EngineOverrides), args in r4..r7 exactly like the recomp bodies read them.
static void eov_yield(Core* c)      { c->game->pcSched.yieldPrim((uint16_t)c->r[4]); }
static void eov_spawn(Core* c)      { c->game->pcSched.spawnPrim(c->r[4], c->r[5]); }
static void eov_spawnwait(Core* c)  { c->game->pcSched.spawnAndWait(c->r[4], c->r[5], c->r[6], c->r[7]); }
static void eov_forceclose(Core* c) { c->game->pcSched.forceClose(c->r[4]); }
static void eov_selfclose(Core* c)  { c->game->pcSched.selfClose(); }

// shard_set_override wires the RECOMPILER's OWN g_override[] table (generated/shard_disp.c).
// FOUND (2026-07-08, dispatch-blind-spot verification): all five of these guest addresses are
// reached from MANY still-substrate call sites as a DIRECT C call `func_<addr>(c)` (grep across
// generated/shard_*.c: 15/7/2/7/8 direct call sites for yield/spawn/spawnAndWait/forceClose/
// selfClose respectively) — a MIPS jal the recompiler emits as an intra-module call, which NEVER
// goes through rec_dispatch. EngineOverrides::register_ (above) only wires the EngineOverrides
// table, which rec_dispatch consults; it does NOT populate g_override[]. So every one of those
// direct-call sites fell through the wrapper's `if (g_override[i]) {...}` to `gen_func_<addr>(c)`
// — running the OLD SUBSTRATE body — even though a byte-exact native port existed and was
// "registered". A live run confirmed the gap empirically: PSXPORT_DEBUG=dispatch,recdep over a
// real autonav session showed exactly ONE `[dispatch]` hit total for these five addresses
// (PcScheduler::spawnPrim, called once from the native boot driver at ra=DEAD0000) and ZERO for
// yieldPrim/spawnAndWait/forceClose/selfClose, despite the game visibly scheduling tasks every
// frame — i.e. every REAL in-game yield/close ran the substrate body, not this port. See
// docs/findings/tooling.md "EngineOverrides::register_ is BLIND to a direct substrate call" (the
// gap was documented but left unfixed pending a concrete victim; this is that victim) and
// game/object/actor_sm_reward.cpp (the existing precedent this copies).
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80051F80(Core*);   // yieldPrim substrate body
extern void gen_func_80051F14(Core*);   // spawnPrim substrate body
extern void gen_func_80044BD4(Core*);   // spawnAndWait substrate body
extern void gen_func_80052010(Core*);   // forceClose substrate body
extern void gen_func_80051FB4(Core*);   // selfClose substrate body

void PcScheduler::registerOverrides() {
  using overrides::install;
  install(0x80051F80u, "PcScheduler::yieldPrim",    eov_yield,      gen_func_80051F80, shard_set_override);
  install(0x80051F14u, "PcScheduler::spawnPrim",    eov_spawn,      gen_func_80051F14, shard_set_override);
  install(0x80044BD4u, "PcScheduler::spawnAndWait", eov_spawnwait,  gen_func_80044BD4, shard_set_override);
  install(0x80052010u, "PcScheduler::forceClose",   eov_forceclose, gen_func_80052010, shard_set_override);
  install(0x80051FB4u, "PcScheduler::selfClose",    eov_selfclose,  gen_func_80051FB4, shard_set_override);
}


// Entry PCs the native stanzas handle. Under BOTH pc_skip modes native handlers run — that's the
// actual test surface. The two modes differ in what the handlers DO:
//   pc_skip=true  → shortcut branches (collapsed multi-step init, scratch drift OK)
//   pc_skip=false → FAITHFUL native branches — byte-exact reproduction of substrate cadence
//                   (Slip #3 case-0 split, task-1 spawn cadence, RNG stamps, etc.).
// Routing pc_skip=false to substrate/fiber = trivial "match" that tests nothing. Reverted
// 2026-07-04 (and again after a second attempt) per user directive: "PC_SKIP=0 uses native
// calls but is BYTE exact to recomp path" — the ported path RUNS, and its byte-exactness is
// the gate, not the routing.
bool PcScheduler::hasNativeHandlerForEntry(uint32_t entry_pc) const {
  return entry_pc == 0x801062E4u   // DEMO
      || entry_pc == 0x80109164u   // SOP area-load
      || entry_pc == 0x8010637Cu   // GAME
      || entry_pc == 0x8010649Cu;  // STAGE-0 START.BIN
}

// DEMO 0x801062E4 — native per-frame dispatcher (both pc_skip modes; see the note on
// hasNativeHandlerForEntry). Fresh: stageMain (prologue) then frame() dispatches
// sm[0x48]==0 substate. Resume: frame(). Slip #1 leave-defer delays the sm[0x48]==5 leave-to-
// GAME dispatch by one tick to match the substrate coro's FUN_80051F80 yield cost. Entry
// rewrite (LEAVE-DEMO -> GAME): drop demo_native, task_started=0, state=3 stands so the next
// tick enters the generic path with the new entry.
void PcScheduler::runDemoBody(Core* c, int i, bool demo_fresh) {
  if (demo_fresh) c->engine.demo.stageMain();
  uint16_t sm48v = c->mem_r16(c->mem_r32(0x1f800138u) + 0x48);
  bool defer_leave = (sm48v == 5 && !demo_fresh
                      && demo_leave_step[i] == 0);
  if (defer_leave) { demo_leave_step[i] = 1; return; }
  c->engine.demo.frame();
  if (sm48v == 5) demo_leave_step[i] = 0;
}

PcScheduler::StanzaResult PcScheduler::runDemoStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                     int native_content, const R3000& loop) {
  if (!game->pc_skip) return STANZA_NOT_MINE;    // pc_faithful DEMO runs on the stage fiber
  int demo_fresh = native_content && (st == 3 || (st == 2 && !task_started[i]))
                   && c->mem_r32(base + 0xc) == 0x801062E4u;
  if (!demo_fresh && !(demo_native[i] && st == 2 && task_started[i]))
    return STANZA_NOT_MINE;
  if (demo_fresh) {
    task_ctx[i] = loop;                          // inherit gp
    task_ctx[i].r[29] = c->mem_r32(base + 8);    // per-task PSX stack top
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    demo_native[i] = 1;
    demo_s0_step[i] = 0;
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000&>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {
    runDemoBody(c, i, demo_fresh);
  } else if (cfg_dbg("demo")) {
    if (!warned_demo_yield++) cfg_logf("demo", "caught a substate yield (async CD not yet "
                                              "owned native+sync) — frontier");
  }
  in_stage = 0;
  if (c->mem_r32(base + 0xc) != 0x801062E4u) {                  // s5 -> GAME rewrote entry
    demo_native[i] = 0;
    task_started[i] = 0;
    demo_s0_step[i] = 0;
    return STANZA_HANDLED;
  }
  task_ctx[i] = static_cast<R3000&>(*c);
  c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// SOP area-load 0x80109164 — SOP.BIN's cooperative slot-1 loader run synchronously (all leaves are
// sync CD reads). Fresh-only: run areaLoad, mark task done. With psx_fallback on the recomp body
// runs as a normal cooperative task via the fiber stanza (its FUN_80051fb4 yield is serviced).
PcScheduler::StanzaResult PcScheduler::runSopAreaLoadStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                            int native_content, const R3000& loop) {
  int sop_fresh = (st == 3 || (st == 2 && !task_started[i]))
                  && c->mem_r32(base + 0xc) == 0x80109164u;
  if (!(sop_fresh && native_content)) return STANZA_NOT_MINE;
  if (!game->pc_skip) return STANZA_NOT_MINE;    // pc_faithful: the task-1 fiber runs areaLoadFaithful
  task_ctx[i] = loop;
  task_ctx[i].r[29] = c->mem_r32(base + 8);
  task_ctx[i].r[31] = 0xDEAD0000u;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000&>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {
    c->engine.sop.areaLoad();
  } else {
    cfg_logf("sched", "SOP area-load yielded unexpectedly — a leaf isn't sync yet");
  }
  in_stage = 0;
  c->mem_w16(base, 0);
  task_started[i] = 0;
  return STANZA_HANDLED;
}

// GAME 0x8010637C — native per-frame dispatcher. Fresh: stagePrologue + frame(). Resume: frame().
// frame() returns 0 when its current sm[0x48] state isn't owned natively — fall back to game_coop:
// hand the task to the guest cooperative loop 0x801063F4 with the loop's callee-saved regs reset,
// so the generic path drives it next tick. Entry rewrite (area transition): drop game_native.
PcScheduler::StanzaResult PcScheduler::runGameStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                     int native_content, const R3000& loop) {
  int game_fresh = (st == 3 || (st == 2 && !task_started[i]))
                   && c->mem_r32(base + 0xc) == 0x8010637Cu;
  if (!native_content && game_native[i]) {       // psx_fallback toggled -> clear stale
    game_native[i] = 0;
    task_started[i] = 0;
  }
  if (!native_content) return STANZA_NOT_MINE;
  if (!game->pc_skip) return STANZA_NOT_MINE;    // pc_faithful GAME runs on the stage fiber
  if (!game_fresh && !(game_native[i] && st == 2 && task_started[i]))
    return STANZA_NOT_MINE;
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
  static_cast<R3000&>(*c) = task_ctx[i];
  in_stage = 1;
  int handled = 1;
  if (setjmp(yield_jmp) == 0) {
    if (game_fresh) c->engine.stagePrologue();
    c->game->ffspan.begin(); handled = c->engine.frame(); c->game->ffspan.end("gameframe");
  } else if (cfg_dbg("sched")) {
    if (!warned_game_yield++) cfg_logf("sched", "caught a GAME substate yield (a leaf not "
                                              "yet sync) — frontier");
  }
  in_stage = 0;
  if (c->mem_r32(base + 0xc) != 0x8010637Cu) {                  // area transition rewrote entry
    game_native[i] = 0;
    task_started[i] = 0;
    return STANZA_HANDLED;
  }
  if (!handled) {                                                // fall back to guest cooperative loop
    c->r[16] = 0x1f800000u; c->r[17] = 0x1f800000u; c->r[18] = 1;
    c->r[31] = 0x801063F4u;
    task_ctx[i] = static_cast<R3000&>(*c);
    game_native[i] = 0;
    game_coop[i] = 1;
    c->mem_w16(base, 2);
    cfg_logf("sched", "GAME -> cooperative guest loop (state not yet "
                                           "owned native; field reachable)");
    return STANZA_HANDLED;
  }
  task_ctx[i] = static_cast<R3000&>(*c);
  c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// TASK-1 BODY under pc_faithful — dispatched by fresh-entry PC:
//   0x80044F58 → Asset::loadTexgroup   (per-set texgroup loader)
//   0x8004514C → Asset::preloadStage1  (SWDATA+DAT+relocation+VRAM build)
// Both set done_flag and rec_dispatch 0x80051FB4 (task-end) so the caller of FUN_80044BD4's
// wait-loop sees the completion in the same tick. pc_skip=true never enters this stanza —
// Engine::startBinStage closes task-1 preemptively there.
PcScheduler::StanzaResult PcScheduler::runTask1PreloadStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                             int native_content, const R3000& loop) {
  if (!native_content) return STANZA_NOT_MINE;
  Coro*& co = coro[i];
  const int fresh = (st == 3 || (st == 2 && !task_started[i]));
  if (fresh) {
    const uint32_t entry_pc = c->mem_r32(base + 0xc);
    const bool is_preload_body    = entry_pc == 0x80044F58u;
    const bool is_stage1_callback = entry_pc == 0x8004514Cu;
    const bool is_sop_area_load   = entry_pc == 0x80109164u;
    const bool is_area_data_load  = entry_pc == 0x800452C0u;   // walkable-field area-DATA loader
    if (!is_preload_body && !is_stage1_callback && !is_sop_area_load && !is_area_data_load)
      return STANZA_NOT_MINE;
    if (co) { delete co; co = nullptr; }        // ~Coro cancels a blocked fiber
    task_ctx[i] = loop;
    task_ctx[i].r[29] = c->mem_r32(base + 8);
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    native_fiber[i] = 1;
    Core* cc = c;
    co = new Coro();
    if (is_preload_body)         co->start([cc] { cc->engine.asset.loadTexgroup(); });
    else if (is_stage1_callback) co->start([cc] { cc->engine.asset.preloadStage1AsTask(); });
    else if (is_area_data_load)  co->start([cc] { cc->engine.asset.areaDataLoadAsTask(); });
    else                         co->start([cc] { cc->engine.sop.areaLoadFaithful(); });
  } else if (!native_fiber[i]) {
    return STANZA_NOT_MINE;
  } else if (st != 2 || !co || co->done()) {
    if (st == 2) { task_started[i] = 0; native_fiber[i] = 0; }
    return STANZA_HANDLED;                      // sleeping (st==1) or dead fiber
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  in_stage = 1;
  cur_is_coro = 1;
  static_cast<R3000&>(*c) = task_ctx[i];
  cfg_logf("sched", "slot %d native-fiber %s st=%u sp=0x%08X", i,
            fresh ? "start" : "resume", st, task_ctx[i].r[29]);
  co->resume();
  cur_is_coro = 0;
  in_stage = 0;
  if (co->done() || c->mem_r16(base) == 0) {
    c->mem_w16(base, 0);
    task_started[i] = 0;
    native_fiber[i] = 0;
    delete co; co = nullptr;
  }
  return STANZA_HANDLED;
}

// pc_faithful STAGE-0 — the whole ov_start arc as a NATIVE task body on a Coro fiber (faithful-
// execution model). Fresh at entry 0x8010649C: fiber start with the task's guest sp and the
// frame-loop registers, exactly like the substrate coro stanza. Resume: one co->resume() per
// runnable frame; the body parks inside PcScheduler::yieldPrim with guest regs saved. Teardown:
// when the body's sm==3 arm dispatches FUN_80052078 the entry rewrites to DEMO and state=3 —
// cancel the fiber (its abandoned frames are plain data; the body holds no destructibles across
// yields) so the DEMO stanza takes the slot fresh next tick.
PcScheduler::StanzaResult PcScheduler::runStage0FiberStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                            int native_content, const R3000& loop) {
  if (!native_content || game->pc_skip) return STANZA_NOT_MINE;
  Coro*& co = coro[i];
  const int fresh = (st == 3 || (st == 2 && !task_started[i]));
  if (fresh) {
    const uint32_t entry = c->mem_r32(base + 0xc);
    if (entry != 0x8010649Cu && entry != 0x801062E4u && entry != 0x8010637Cu) return STANZA_NOT_MINE;
    if (co) { delete co; co = nullptr; }        // ~Coro cancels a blocked fiber
    task_ctx[i] = loop;
    task_ctx[i].r[29] = c->mem_r32(base + 8);
    task_ctx[i].r[31] = 0xDEAD0000u;
    task_started[i] = 1;
    native_fiber[i] = 1;
    fiber_entry[i] = entry;
    demo_native[i] = 0; game_native[i] = 0; game_coop[i] = 0;
    Core* cc = c;
    co = new Coro();
    if (entry == 0x8010649Cu)      co->start([cc] { cc->engine.startBinStageFaithful(); });
    else if (entry == 0x801062E4u) co->start([cc] { cc->engine.demo.stageBodyFaithful(); });
    else                           co->start([cc] { cc->engine.stageBodyFaithful(); });
  } else if (!native_fiber[i]) {
    return STANZA_NOT_MINE;
  } else if (st != 2 || !co || co->done()) {
    if (st == 2) { task_started[i] = 0; native_fiber[i] = 0; }
    return STANZA_HANDLED;                      // sleeping (st==1) or dead fiber
  }
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  in_stage = 1;
  cur_is_coro = 1;
  static_cast<R3000&>(*c) = task_ctx[i];
  cfg_logf("sched", "slot %d native-fiber %s st=%u sp=0x%08X", i,
            fresh ? "start" : "resume", st, task_ctx[i].r[29]);
  co->resume();
  cur_is_coro = 0;
  in_stage = 0;
  if (co->done() || c->mem_r16(base) == 0) {
    c->mem_w16(base, 0);
    task_started[i] = 0;
    native_fiber[i] = 0;
    delete co; co = nullptr;
  } else if (c->mem_r32(base + 0xc) != fiber_entry[i]) {
    // FUN_80052078 swapped the stage (entry rewritten, state=3): the parked body will never be
    // resumed — tear the fiber down so the new stage's stanza starts fresh.
    task_started[i] = 0;
    native_fiber[i] = 0;
    delete co; co = nullptr;
  }
  return STANZA_HANDLED;
}

// STAGE-0 START.BIN step-spread — resume path. Between the fresh startBinStage tick (handled in
// the generic dispatch stanza) and the final swap-to-DEMO tick, Engine::stage0Advance steps the
// preload SM one step per scheduler tick to match the recomp coro yield cadence (docs/findings/
// sbs.md Slip #1). Entry stays at 0x8010649C until the last step's native_start_stage swap.
PcScheduler::StanzaResult PcScheduler::runStage0StepStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                           int native_content) {
  if (!(native_content && game->pc_skip && st == 2 && task_started[i]
        && c->mem_r32(base + 0xc) == 0x8010649Cu && stage0_step[i] < 8))
    return STANZA_NOT_MINE;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000&>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {                   // final step yields via longjmp
    c->engine.stage0AdvanceSkip(stage0_step[i]);
  }
  in_stage = 0;
  task_ctx[i] = static_cast<R3000&>(*c);
  // Last step's native_start_stage rewrites entry to DEMO + sets state=3 (fresh) — leave both
  // alone. Otherwise keep state=2 (runnable) for the next step.
  if (c->mem_r32(base + 0xc) == 0x8010649Cu) c->mem_w16(base, 2);
  return STANZA_HANDLED;
}

// One scheduler pass over the 3 task slots (replaces FUN_80051e60). The main loop is purely
// slot-iteration + stanza dispatch — PC-native stanzas first, then the substrate coro-fiber /
// generic-dispatch stanzas for un-ported tasks.
void PcScheduler::step() {
  Core* c = &game->core;
  R3000 loop = *c;                           // frame-loop REGISTERS (gp etc. for fresh tasks); slices off RAM
  for (int i = 0; i < 3; i++) {
    uint32_t base = TASKBASE + (uint32_t)i * TASKSTRIDE;
    // Task slot 2 = XA voice/BGM. When the native clip player owns it, do NOT run the (now unused)
    // FUN_8001cfc8 recomp coroutine; reflect clip state into task-2's state byte so the cutscene's
    // `while (DAT_801fe0e0 != 0)` wait advances exactly when the clip finishes.
    if (i == 2 && xa_stream_owns_slot2(&c->game->xa)) {
      if (xa_stream_voice_busy(&c->game->xa)) c->mem_w16(base, 2);
      else { c->mem_w16(base, 0); xa_stream_voice_release(&c->game->xa); }
      task_started[2] = 0;
      continue;
    }
    uint32_t st = c->mem_r16(base);
    if (st == 0) { task_started[i] = 0; demo_native[i] = 0; continue; }
    // GATE: on `gate` toggle to psx_fallback, clear any stale native-dispatcher mode so the
    // generic path re-enters cleanly (the native DEMO/GAME dispatchers are OFF under psx_fallback).
    int native_content = !game->psx_fallback;
    if (!native_content && (demo_native[i] || game_native[i])) {
      demo_native[i] = 0; game_native[i] = 0; task_started[i] = 0;
    }
    if (runDemoStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)          continue;
    if (runSopAreaLoadStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)   continue;
    if (runGameStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)          continue;
    if (!game->pc_skip && i == 1                                                                  // pc_faithful task-1 body
        && runTask1PreloadStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED) continue;
    if (runStage0FiberStanza(c, i, base, st, native_content, loop) == STANZA_HANDLED)   continue;
    if (recomp_run_coro_fiber_stanza(c, i, base, st, native_content, loop))             continue;
    if (runStage0StepStanza(c, i, base, st, native_content) == STANZA_HANDLED)          continue;
    recomp_run_generic_dispatch_stanza(c, i, base, st, native_content, loop);
  }
  static_cast<R3000&>(*c) = loop;             // restore the frame-loop REGISTERS (shared RAM untouched)
}

// FUN_800506D0 — VERIFIED + WIRED (frontier tier, 2026-07-10). See pc_scheduler.h for the RE summary. 1:1
// with generated/shard_5.c:7522: no frame descent (leaf, no sp/ra touched), 3-slot sweep over
// TASKBASE+i*TASKSTRIDE for i=0..2 (verified against the guest loop bound 0x801FE000..0x801FE14F
// inclusive-by-112 == exactly 3 iterations), state==1 -> decrement countdown at +2 -> re-arm to 2
// on underflow-to-exactly-zero. The still-substrate call site is
// runtime/recomp/native_boot.cpp:129 (`rc0(c, 0x800506d0)`); wiring is a frontier-tier follow-up
// (swap that call for `c->game->pcSched.tickSleepCountdown()` once SBS-gated).
void PcScheduler::tickSleepCountdown() {
  Core* c = &game->core;
  for (uint32_t base = TASKBASE; base <= TASKBASE + 2u * TASKSTRIDE; base += TASKSTRIDE) {
    if (c->mem_r16(base + 0x00u) != 1u) continue;               // only YIELDED (state==1) slots tick
    uint16_t countdown = (uint16_t)(c->mem_r16(base + 0x02u) - 1u);
    c->mem_w16(base + 0x02u, countdown);
    if (countdown == 0u) c->mem_w16(base + 0x00u, 2u);           // re-arm 1 -> 2 (RUNNABLE)
  }
}
