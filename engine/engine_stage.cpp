// engine_stage.cpp — PC-native ownership of the GAME stage state machine (the per-area scene/update
// driver), the engine's top-level "run the game" sequencer. Boundary: the ENGINE owns the scene-state
// machine (which state runs, what fields reset on each transition); the actual per-state system work
// (asset load, fade, render, gameplay sub-machines) stays dispatched to the retained PSX content/system
// code. Realizes "the engine runs the game; PSX only drives content" for the stage driver — incrementally.
//
// The GAME stage is overlay \BIN\GAME.BIN (LBA 1882), loaded RAW to base 0x80106228; task-0 entry =
// 0x8010637C, which runs as a COOPERATIVE TASK (it loops forever, yielding once per frame via
// FUN_80051f80). The current task object ptr is *0x1f800138 (== task-0 obj 0x801fe000); its state fields:
//   sm[0x48] top state, sm[0x4a] running sub-mode, sm[0x4c] area machine, sm[0x5c] intro timer.
// Three-level nested machine (all handlers in GAME.BIN; full RE in docs/engine_re.md):
//   sm[0x48]: 0 -> 0x801086e0 (area INIT)   1 -> 0x80108720 (RESUME-INIT)   2 -> 0x80108784 (RUNNING)
//   sm[0x4a] (in 0x80108784, 6-way @0x8010631c): 0..5 -> sub-handler (mode 2 = the 9-state sm[0x4c] area
//            load/intro/play machine at 0x80106478, jump table @0x8010622c)
//
// OWNED HERE: all three sm[0x48] handlers. The area-INIT pair (==0 / ==1) are clean `jr ra` functions
// whose callees are SYNCHRONOUS, so a native override that mirrors their guest writes and dispatches their
// resident setup fns is faithful (verified RAM 0-diff @ a field frame, later-168). The RUNNING dispatcher
// (==2, 0x80108784) owns the 6-way running-sub-mode (sm[0x4a]) selection, but its sub-handlers YIELD DEEP
// (they call resident 0x8007xxxx fns that wait across frames). It can NOT be rec_dispatch'd (that nests a
// rec_interp with its own CORO_SENTINEL; the deep yield's longjmp destroys that C frame and the resume
// mis-reads the return as task-end, killing task 0 — later-168). It uses the cooperative-yield handshake
// (rec_coro_redirect, later-169): native dispatch, then hand to the handler IN-CONTEXT so the yield is the
// scheduler returning, not a nested sentinel. Same mechanism unlocks owning FUN_80052078/FUN_800499e8.

#include "core.h"
#include "cfg.h"
#include <stdio.h>

// sm[0x48] == 0 — area INIT: advance to running (sm[0x48]=2), reset the sub-machine state, run the per-area
// setup fns. (GAME.BIN 0x801086e0) Verified runtime-exercised + RAM 0-diff.
static void ov_game_s48_0(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // mirror prologue: addiu sp,-0x18; sw ra,0x10(sp)
  uint32_t sm = c->mem_r32(0x1f800138);
  c->mem_w16(sm + 0x48, 2);          // sm[0x48] = 2 (running)
  c->mem_w16(sm + 0x4a, 0);          // sm[0x4a] = 0
  c->mem_w16(sm + 0x4c, 0);          // sm[0x4c] = 0
  c->mem_w8 (sm + 0x69, 0);          // sm[0x69] = 0
  rec_dispatch(c, 0x8007a8e0u);      // per-area setup (resident system, synchronous)
  rec_dispatch(c, 0x8007b38cu);      // per-area setup (resident system, synchronous)
  c->r[29] = sp; c->r[31] = ra;      // mirror epilogue: addiu sp,+0x18; jr ra
}

// sm[0x48] == 1 — area RESUME-INIT (re-enter a running area, sub-mode 1): like init but sm[0x4a]=1 plus
// flag resets. (GAME.BIN 0x80108720) Faithful transcription of the disasm; the field-intro path used to
// verify (later-168) does not hit sm[0x48]==1, so this handler is not yet runtime-exercised — its callee
// FUN_8007b3f4 is synchronous like the init pair, so it is registered alongside ov_game_s48_0.
static void ov_game_s48_1(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // mirror prologue: addiu sp,-0x18; sw ra,0x10(sp)
  uint32_t sm = c->mem_r32(0x1f800138);
  c->mem_w16(sm + 0x48, 2);          // sm[0x48] = 2 (running)
  c->mem_w16(sm + 0x4a, 1);          // sm[0x4a] = 1
  c->mem_w16(sm + 0x4c, 0);          // sm[0x4c] = 0
  c->mem_w8 (sm + 0x69, 0);          // sm[0x69] = 0
  c->mem_w8 (0x1f8001ff, 0xff);      // DAT_1f8001ff = 0xff
  c->mem_w16(0x1f800278, 0);         // DAT_1f800278 = 0 (16-bit; delay-slot before the setup call)
  rec_dispatch(c, 0x8007b3f4u);      // per-area setup (resident system, synchronous)
  c->mem_w8 (0x1f800206, 0);         // display flags cleared after setup
  c->mem_w8 (0x1f800236, 0);
  c->mem_w8 (0x1f800234, 0);
  c->r[29] = sp; c->r[31] = ra;      // mirror epilogue: addiu sp,+0x18; jr ra
}

// sm[0x48] == 2 — RUNNING: dispatch the running sub-mode sm[0x4a] (0..5); >=6 is a no-op. (GAME.BIN
// 0x80108784) The engine OWNS this 6-way running-sub-mode selection. The selected sub-handler is retained
// PSX content/system code that can YIELD DEEP (it calls resident 0x8007xxxx fns that wait across frames),
// so it must NOT be rec_dispatch'd: that nests a rec_interp with its own CORO_SENTINEL, and the deep
// yield's longjmp destroys that C frame -> the resume mis-reads the return as task-end and kills task 0
// (later-168). Instead we use the cooperative-yield handshake (rec_coro_redirect, later-169): do the
// dispatch natively, then continue the flat interp at the handler IN-CONTEXT (same task run). A deep yield
// then longjmps to the scheduler and resumes correctly; the handler returns through the guest trampoline
// `j 0x8010881c` -> epilogue -> the task loop, exactly as the PSX path would.
//
// Faithful to 0x80108784: prologue `addiu sp,-0x18; sw ra,0x10(sp)`; read sm[0x4a]; if <6, set ra to the
// handler's guest return (the `jal handler`'s ra = jal_addr+8 = the trampoline's `j 0x8010881c`, byte-
// identical so the handler's saved-ra on its stack matches the reference) and redirect to the handler;
// else redirect to the guest epilogue 0x8010881c (which restores ra from 0x10(sp) and returns).
static void ov_game_s48_2(Core* c) {
  static const uint32_t handler[6] = {
    0x8010882cu, 0x801088d8u, 0x80106478u, 0x80106a24u, 0x801089c4u, 0x80108a60u,
  };
  uint32_t ra = c->r[31];
  c->r[29] -= 0x18; c->mem_w32(c->r[29] + 0x10, ra);   // prologue: addiu sp,-0x18; sw ra,0x10(sp)
  uint32_t sm = c->mem_r32(0x1f800138);
  uint16_t s4a = c->mem_r16(sm + 0x4a);
  if (s4a >= 6) { rec_coro_redirect(c, 0x8010881Cu); return; }   // out of range -> guest epilogue
  c->r[31] = 0x801087CCu + (uint32_t)s4a * 0x10u;       // handler's guest return (`j 0x8010881c` trampoline)
  rec_coro_redirect(c, handler[s4a]);                  // run the handler IN-CONTEXT (survives a deep yield)
}

// sm[0x4c] == the AREA machine (the 9-state load/intro/play scene state machine), GAME.BIN 0x80106478,
// reached as ov_game_s48_2's sub-mode 2 (the area LOAD/TRANSITION path). STAGED, NOT REGISTERED: it owns the
// 9-way sm[0x4c] state selection via the same coro-redirect handshake (the per-state bodies yield deep and
// run IN-CONTEXT), but the headless idle-field gate never reaches s4a==2, so it can't be A/B-verified yet —
// see the registration site. Faithful to 0x80106478:
//   prologue: addiu sp,-0x18; sw ra,0x14(sp); sw s0,0x10(sp)   (s0 saved in the jal's delay slot)
//   jal 0x80075a80  — a per-frame area update BEFORE the dispatch. It is SYNCHRONOUS (a transitive
//     jal-graph scan over its 57 reachable fns finds no yield FUN_80051f80; and the RAM 0-diff gate would
//     catastrophically collapse — task 0 dies — if it ever yield-crossed this nested rec_dispatch), so it
//     is dispatched synchronously here.
//   read sm[0x4c]; if <9 jump-table @0x8010622c -> state; else fall to the shared epilogue.
//   each state runs then `j 0x80106a14` (shared epilogue: lw ra,0x14(sp); lw s0,0x10(sp); jr ra).
// The states are reached by `jr v0` (computed), so they run with ra UNCHANGED (= this fn's caller ra, which
// rec_dispatch restores) and return only via `j 0x80106a14` — so we leave r[31] alone and just redirect.
static void ov_game_s4c(Core* c) {
  static const uint32_t state[9] = {
    0x801064c4u, 0x80106510u, 0x80106580u, 0x801065b8u, 0x801066b8u,
    0x80106830u, 0x80106930u, 0x8010694cu, 0x801069b4u,
  };
  uint32_t ra = c->r[31];
  c->r[29] -= 0x18;
  c->mem_w32(c->r[29] + 0x14, ra);              // sw ra,0x14(sp)
  c->mem_w32(c->r[29] + 0x10, c->r[16]);        // sw s0,0x10(sp)
  rec_dispatch(c, 0x80075a80u);                 // per-frame area update (synchronous — verified yield-free)
  uint32_t sm = c->mem_r32(0x1f800138);
  uint16_t s4c = c->mem_r16(sm + 0x4c);
  if (s4c >= 9) { rec_coro_redirect(c, 0x80106a14u); return; }   // out of range -> shared epilogue
  rec_coro_redirect(c, state[s4c]);             // run the state IN-CONTEXT (it `j`s to the epilogue itself)
}

// Register the GAME-stage area-init overrides when this just-loaded overlay is GAME.BIN at the stage base.
// Detect by the fixed entry + handler signatures (START.BIN/DEMO.BIN are smaller and hold stale bytes at
// these addresses, so they never match). Called from the overlay-load scan (engine_submit.cpp); registered
// AUTO so it is flushed when GAME.BIN unloads and another overlay reuses the base (mirrors the M3 scan).
void stage_scan_overlay(Core* c, uint32_t base, uint32_t size) {
  if (base != 0x80106228u || size < 0x2600u) return;   // only GAME.BIN (~11.6 KB) reaches the handlers
  if (c->mem_r32(0x8010637Cu) != 0x27bdffe0u) return;  // entry:   addiu sp,sp,-0x20
  if (c->mem_r32(0x801086e0u) != 0x27bdffe8u) return;  // s48==0:  addiu sp,sp,-0x18
  if (c->mem_r32(0x80108720u) != 0x27bdffe8u) return;  // s48==1:  addiu sp,sp,-0x18
  rec_set_interp_override_auto(0x801086e0u, ov_game_s48_0);
  rec_set_interp_override_auto(0x80108720u, ov_game_s48_1);
  rec_set_interp_override_auto(0x80108784u, ov_game_s48_2);   // RUNNING dispatcher (coro-redirect handshake)
  // ov_game_s4c (sm[0x4c] area machine, 0x80106478) is RE'd + implemented with the same coro-redirect
  // handshake, but it is reached ONLY via sm[0x4a]==2 — the area LOAD/TRANSITION path. The headless idle-
  // field gate runs entirely in sm[0x4a] 0/1 (steady play), so s4a==2 / 0x80106478 is NEVER exercised and
  // can't be A/B-verified here. NOT registered until there's a deterministic area-transition test path
  // (drive Tomba through an area boundary headless, or own FUN_80052078 to script a transition). See
  // docs/journal.md later-169 NEXT.
  (void)ov_game_s4c;
  if (cfg_dbg("stage"))
    fprintf(stderr, "[stage] own GAME area-init + RUNNING handlers (sm[0x48]==0 0x801086e0, ==1 0x80108720, "
                    "==2 0x80108784) in load 0x%08X+0x%X\n", base, size);
}
