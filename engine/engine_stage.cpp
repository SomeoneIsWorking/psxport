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
// OWNED HERE: the two area-INIT handlers (sm[0x48]==0 / ==1). They are clean functions (`jr ra`) whose
// callees are SYNCHRONOUS (no cooperative yield), so a native override that mirrors their guest writes and
// dispatches their resident setup fns is faithful — verified RAM 0-diff @ a field frame (later-168).
//
// NOT OWNED (blocked): the RUNNING dispatcher 0x80108784 and the sm[0x4a]/sm[0x4c] area machine. Those
// sub-handlers YIELD cooperatively (they are the per-frame waits of task 0). Dispatching a yielding fn via
// rec_dispatch runs it in a NESTED rec_interp with its own CORO_SENTINEL; when the yield longjmps to the
// scheduler and the task resumes, the deep PSX return chain unwinds to that nested sentinel, which
// rec_coro_run reads as "the task returned" -> task 0 is marked dead (st=0) and the game limps on with no
// stage driver (later-168: caught as a divergence-explosion at f53). Owning the running loop natively
// requires the cooperative-yield handshake (a native override that can survive a deep yield/resume) FIRST
// — the same blocker engine_re.md flags for FUN_80052078/FUN_800499e8. ov_game_s48_2 below encodes the
// correct dispatch (kept as the next-step reference) but is intentionally NOT registered.

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
// 0x80108784) BLOCKED — see the file header: the sub-handlers yield, so this CANNOT be dispatched via
// rec_dispatch without the cooperative-yield handshake. Kept as the next-step reference; NOT registered.
// (The 6 targets are the real sub-handlers; the guest jump table @0x8010631c holds trampolines that just
// `jal <handler>; j <epilogue>`, so calling the handler directly is the faithful equivalent.)
static void ov_game_s48_2(Core* c) {
  static const uint32_t sub[6] = {
    0x8010882cu, 0x801088d8u, 0x80106478u, 0x80106a24u, 0x801089c4u, 0x80108a60u,
  };
  uint32_t ra = c->r[31], sp = c->r[29];
  c->r[29] = sp - 0x18; c->mem_w32(c->r[29] + 0x10, ra);
  uint32_t sm = c->mem_r32(0x1f800138);
  uint16_t s4a = c->mem_r16(sm + 0x4a);
  if (s4a < 6) rec_dispatch(c, sub[s4a]);
  c->r[29] = sp; c->r[31] = ra;
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
  (void)ov_game_s48_2;   // running dispatcher: blocked on the cooperative-yield handshake (see header)
  if (cfg_dbg("stage"))
    fprintf(stderr, "[stage] own GAME area-init handlers (sm[0x48]==0 0x801086e0, ==1 0x80108720) "
                    "in load 0x%08X+0x%X\n", base, size);
}
