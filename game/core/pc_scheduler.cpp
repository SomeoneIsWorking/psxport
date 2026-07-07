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
#include <setjmp.h>
#include <stdio.h>


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
    if (!warned_demo_yield++) fprintf(stderr, "[demo] caught a substate yield (async CD not yet "
                                              "owned native+sync) — frontier\n");
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
  } else if (cfg_dbg("sched")) {
    fprintf(stderr, "[sched] SOP area-load yielded unexpectedly — a leaf isn't sync yet\n");
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
  // Native GAME stanza runs under BOTH pc_skip modes.
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
    if (!warned_game_yield++) fprintf(stderr, "[sched] caught a GAME substate yield (a leaf not "
                                              "yet sync) — frontier\n");
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
    if (cfg_dbg("sched")) fprintf(stderr, "[sched] GAME -> cooperative guest loop (state not yet "
                                           "owned native; field reachable)\n");
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
  int t1_fresh = st == 3 || (st == 2 && !task_started[i]);
  if (!t1_fresh) return STANZA_NOT_MINE;
  const uint32_t entry_pc = c->mem_r32(base + 0xc);
  const bool is_preload_body    = entry_pc == 0x80044F58u;
  const bool is_stage1_callback = entry_pc == 0x8004514Cu;
  if (!is_preload_body && !is_stage1_callback) return STANZA_NOT_MINE;
  task_ctx[i] = loop;
  task_ctx[i].r[29] = c->mem_r32(base + 8);
  task_ctx[i].r[31] = 0xDEAD0000u;
  task_started[i] = 1;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000&>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {
    if (is_preload_body) c->engine.asset.loadTexgroup();
    else                 c->engine.asset.preloadStage1AsTask();
  }
  in_stage = 0;
  task_started[i] = 0;
  return STANZA_HANDLED;
}

// STAGE-0 START.BIN step-spread — resume path. Between the fresh startBinStage tick (handled in
// the generic dispatch stanza) and the final swap-to-DEMO tick, Engine::stage0Advance steps the
// preload SM one step per scheduler tick to match the recomp coro yield cadence (docs/findings/
// sbs.md Slip #1). Entry stays at 0x8010649C until the last step's native_start_stage swap.
PcScheduler::StanzaResult PcScheduler::runStage0StepStanza(Core* c, int i, uint32_t base, uint32_t st,
                                                           int native_content) {
  if (!(native_content && st == 2 && task_started[i]
        && c->mem_r32(base + 0xc) == 0x8010649Cu && stage0_step[i] < 8))
    return STANZA_NOT_MINE;
  c->mem_w16(base, 4);
  c->mem_w32(CUR_TASK, base);
  cur_slot = i;
  static_cast<R3000&>(*c) = task_ctx[i];
  in_stage = 1;
  if (setjmp(yield_jmp) == 0) {                   // final step yields via longjmp
    c->engine.stage0Advance(stage0_step[i]);
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
    if (i == 2 && xa_stream_owns_slot2()) {
      if (xa_stream_voice_busy()) c->mem_w16(base, 2);
      else { c->mem_w16(base, 0); xa_stream_voice_release(); }
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
    if (recomp_run_coro_fiber_stanza(c, i, base, st, native_content, loop))             continue;
    if (runStage0StepStanza(c, i, base, st, native_content) == STANZA_HANDLED)          continue;
    recomp_run_generic_dispatch_stanza(c, i, base, st, native_content, loop);
  }
  static_cast<R3000&>(*c) = loop;             // restore the frame-loop REGISTERS (shared RAM untouched)
}
