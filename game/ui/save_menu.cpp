// game/ui/save_menu.cpp — Tomba!2 SAVE / LOAD FLOW (the game's save-system orchestration), PC-native.
//
// SCOPE (per CLAUDE.md: the engine owns save/load FLOW; the memory-card hardware + libmcrd file I/O
// are the PLATFORM leaf, already native in runtime/recomp/memcard.cpp and kept dispatched here):
// this module owns the SAVE/LOAD-MENU STATE MACHINE — the head dispatcher that drives the
// "Load data" / "Save" / format / delete pages reached from the title's Load-Game screen and the
// in-game pause menu's "Load data" entry. It is the game's save/load FLOW logic.
//
// ---------------------------------------------------------------------------------------------------
// RE — the save/load flow (disas.py over scratch/bin/tomba2/MAIN.EXE)
// ---------------------------------------------------------------------------------------------------
// The save system is a 6-state machine whose body is the active save-menu task's handler. The HEAD
// dispatcher is FUN_80036DFC (entry 0x80036DFC; an earlier scan labelled it 0x80036E00, which is the
// first store AFTER the `addiu sp,-0x30` prologue):
//
//   80036dfc  addiu sp, sp, -0x30          ; frame
//   80036e00  sw s1, 0x24(sp)              ; save s1
//   80036e04  addu s1, a0, zero            ; s1 = a0 = the save-menu TASK STRUCT
//   80036e08  lui  v0, 0x800d
//   80036e0c  sw ra, 0x2c(sp)              ; save ra
//   80036e10  sw s2, 0x28(sp)              ; save s2
//   80036e14  sw s0, 0x20(sp)              ; save s0
//   80036e18  lbu  v1, 1(s1)               ; SUBSTATE = task[1]  (the page/op selector)
//   80036e1c  addiu s0, v0, 0x1e68         ; s0 = 0x800d1e68 = the save-system CONTEXT struct base
//   80036e20  sltiu v0, v1, 6              ; bounds: substate < 6 ?
//   80036e24  beq  v0, zero, 0x800376d4    ; >= 6 -> epilogue (no-op return)
//   80036e28  lui  v0, 0x8001
//   80036e2c  addiu v0, v0, 0x668          ; table = 0x80010668
//   80036e30  sll  v1, v1, 2
//   80036e34  addu v1, v1, v0
//   80036e38  lw   v0, 0(v1)               ; handler = table[substate]
//   80036e40  jr   v0                      ; TAIL-CALL the handler (a0/s0/s1/s2/ra + frame all live)
//
//   80036e (out-of-range / epilogue) 0x800376d4:
//   8fbf002c lw ra,0x2c(sp); 8fb20028 lw s2; 8fb10024 lw s1; 8fb00020 lw s0;
//   03e00008 jr ra; 27bd0030 addiu sp,+0x30      ; plain restore-and-return.
//
// Substate -> handler (table @0x80010668):
//   [0] 0x80036E48  LOAD-SELECT      (page setup for the slot list; clears ctx[32], sets task[1])
//   [1] 0x80036E58  LOAD-RUN         (slot navigation Up/Down via pad edges 0x800E7E68, confirm/cancel)
//   [2] 0x800371D4  SAVE-CONFIRM     (confirmation prompt)
//   [3] 0x80037360  SAVE-EXECUTE     (commit: sets save flags 0x800BF809/0A/0B/3A, kicks the writers)
//   [4] 0x800375E0  FORMAT           (format-card page)
//   [5] 0x80037638  DELETE           (delete-save page)
//
// The dispatcher is the FLOW HEAD: it does NO work of its own beyond bounds-checking the substate and
// tail-calling the page handler (the handlers do all the cursor/page state + call the libmcrd file I/O,
// which the platform HLE in memcard.cpp services — those LEAVES stay PSX). A native tail-call to the
// handler is exactly equivalent: same args (a0 untouched), same callee-saved register setup (s1=task,
// s0=ctx), same stack frame (the handler unwinds the dispatcher's 0x30 frame itself via its own
// epilogue), same return target (ra still holds the dispatcher's caller). Only the dispatch GLUE goes
// native; the save/load behaviour is unchanged. This is the same shape as the render-command dispatcher
// (ov_render_cmd / render_cmd_dispatch, submit.cpp) and the object dispatcher (ov_disp_26c88).
//
// THE SERIALIZE/DESERIALIZE BUFFER: in this title there is NO discrete game-side "build the save buffer
// from RAM" function to own — the save data IS the game's live progress block, and persistence is done
// by libmcrd writing/reading the card frames directly through the BIOS file API (open/read/write via the
// 0xB0/0xA0 BIOS-call trampolines). That whole path is library/PLATFORM, reached only through BIOS-call
// trampolines (no direct game-code jal into it), and is already owned by memcard.cpp's HLE (B0:0x4E/0x4F
// frame read/write + the SwCARD completion). So the ENGINE-ownable save/load logic here is precisely this
// FLOW state machine, which this module owns; the card I/O leaf stays dispatched. (Evidence: the file-API
// stubs 0x800808B8/C8/D8/E8/F8 have zero direct jal callers; the libmcrd internals FUN_8009Bxxx/8009Cxxx
// are reached via 0xB0 trampolines; the save-execute handler 0x80037360 contains only flag writes +
// sub-calls, no bulk game-RAM->buffer copy.)
//
// VERIFY: the `saveverify` REPL gate (PSXPORT_DEBUG / `debug saveverify`) runs the native dispatch, then
// snapshots+rolls back full RAM + scratchpad + regs, runs the recomp body via rec_super_call, and diffs.
// Because the dispatcher tail-calls (and the handlers it dispatches run identically in both passes,
// leaving transient residue below the entry sp where this fn's own 0x30 frame is also dead on return),
// the gate excludes the top-of-RAM stack window [sp-0x800, sp) — far above all game data — exactly as the
// dispatcher/state-machine family gates do (disp26c88 / sm40558). See docs/port-progress.md §SAVE.

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "save_menu.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (super-call / A/B oracle)
void rec_dispatch(Core*, uint32_t);     // hybrid call: recomp body if emitted, else interpret

namespace {

// Save-system constants RE'd above.
constexpr uint32_t SAVE_DISPATCH_FN = 0x80036DFCu;  // FUN_80036DFC — the save/load-flow head dispatcher
constexpr uint32_t SAVE_HANDLER_TBL = 0x80010668u;  // 6-entry handler table (stride 4)
constexpr uint32_t SAVE_CTX_BASE    = 0x800D1E68u;  // save-system context struct (s0 in the prologue)
constexpr int      SAVE_NUM_STATES  = 6;            // load-select/run, save-confirm/execute, format, delete

// Register file indices (raw MIPS abi slots, matching the rest of the engine overrides).
enum { R_A0 = 4, R_S0 = 16, R_S1 = 17, R_S2 = 18, R_SP = 29, R_RA = 31 };

}  // namespace

// ------------------------------------------------------------------------------------------------
// SaveMenu::runHandler(task) — resolve and run ONE save-menu page handler for the given
// substate (0..5), faithfully reproducing the dispatcher's prologue so the handler can unwind the
// frame and tail-return through it. The page handlers themselves (cursor/page logic + libmcrd file I/O)
// stay PSX via rec_dispatch. Returns nothing; the handler sets v0/the task fields just as the PSX body
// would. Out-of-range substate (>=6) is the dispatcher's no-op return path.
// ------------------------------------------------------------------------------------------------
void SaveMenu::runHandler(uint32_t task) {
  Core* c = core;
  // ---- prologue (FUN_80036DFC 0x80036dfc..0x80036e1c) ----
  uint32_t sp = c->r[R_SP] - 0x30u;            // addiu sp,-0x30
  c->mem_w32(sp + 0x24, c->r[R_S1]);           // sw s1, 0x24(sp)
  c->mem_w32(sp + 0x2C, c->r[R_RA]);           // sw ra, 0x2c(sp)
  c->mem_w32(sp + 0x28, c->r[R_S2]);           // sw s2, 0x28(sp)
  c->mem_w32(sp + 0x20, c->r[R_S0]);           // sw s0, 0x20(sp)
  c->r[R_SP] = sp;
  c->r[R_S1] = task;                           // addu s1, a0, zero  (s1 = task struct)
  c->r[R_S0] = SAVE_CTX_BASE;                  // addiu s0, v0, 0x1e68 (s0 = save-system context)

  uint32_t substate = c->mem_r8(task + 1);     // lbu v1, 1(s1)

  if (substate >= (uint32_t)SAVE_NUM_STATES) {
    // ---- out-of-range / epilogue (0x800376d4): restore + return (no-op) ----
    c->r[R_RA] = c->mem_r32(sp + 0x2C);
    c->r[R_S2] = c->mem_r32(sp + 0x28);
    c->r[R_S1] = c->mem_r32(sp + 0x24);
    c->r[R_S0] = c->mem_r32(sp + 0x20);
    c->r[R_SP] = sp + 0x30;
    return;
  }

  // ---- dispatch: handler = table[substate]; jr handler (tail-call, frame + regs live) ----
  uint32_t handler = c->mem_r32(SAVE_HANDLER_TBL + substate * 4u);
  rec_dispatch(c, handler);  // the page handler runs the load/save page logic + unwinds the frame itself
}

// ------------------------------------------------------------------------------------------------
// SaveMenu::dispatchBody(c) — native entry replacing FUN_80036DFC. a0 = the save-menu task struct.
// ------------------------------------------------------------------------------------------------
void SaveMenu::dispatchBody(Core* c) {
  saveMenuOf(c).runHandler(c->r[R_A0]);
}

