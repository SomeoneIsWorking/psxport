// PC-native engine LEVEL / STAGE LOADING (the engine system that reads a stage off the disc and brings
// the world into being). Per the CLAUDE.md boundary this is ENGINE → reimplemented PC-native; the bytes
// it loads (stage overlay code + level data) and the per-entity AI/physics that later run on it stay PSX.
//
// Entry chain from main: task0 = the stage sequencer FUN_800499e8 (resolves \BIN\START.BIN → its disc
// LBA/size into the stage table DAT_800be1e0/e4) → FUN_80052078(stage) (restart task at the stage) →
// FUN_800450bc(task, stage) = THIS loader. RE: scratch/decomp + tools/disas.py (later-162).
#include "core.h"
#include "cfg.h"
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);

// FUN_800450bc — load a stage's overlay off the disc and set the task's stage entry point.
//   a0 (param_1) = the task's entry-pointer slot (2 words: [0]=entry fn, [1]=task stack/handle)
//   a1 (param_2) = stage index;  stage 3 = the resident default (no load, entry = *0x800a3ed8)
// Per-stage (LBA,size) pairs live at 0x800be1e0/0x800be1e4 (stride 8); the overlay loads to 0x80106228;
// the per-stage entry table is at 0x800a3ecc. The PSX yields a frame after the load to wait for the async
// CD (FUN_80051f80(1)); our overlay loader (ov_cd_loadfile, the 0x8001db8c override) is SYNCHRONOUS — the
// data is present when it returns — so the yield is DROPPED. That also removes the only coroutine yield in
// this function, which is what makes a native reimplementation safe (no longjmp out of this C frame).
static void eng_load_stage(Core* c) {
  uint32_t param_1 = c->r[4];
  int32_t  stage   = (int32_t)c->r[5];
  uint32_t entry;
  if (stage == 3) {
    entry = c->mem_r32(0x800A3ED8u);                                   // resident default entry
  } else {
    uint32_t lba  = c->mem_r32(0x800BE1E0u + (uint32_t)stage * 8u);    // stage's disc LBA
    uint32_t size = c->mem_r32(0x800BE1E4u + (uint32_t)stage * 8u);    // stage's byte size
    c->r[4] = 0x80106228u; c->r[5] = lba; c->r[6] = size;
    rec_dispatch(c, 0x8001DB8Cu);                                     // synchronous overlay load (CD platform)
    entry = c->mem_r32(0x800A3ECCu + (uint32_t)stage * 4u);            // the stage's entry point
  }
  c->mem_w32(param_1 + 0, entry);                                      // *param_1 = stage entry fn
  rec_dispatch(c, 0x80080930u);                                        // task stack/handle alloc
  c->mem_w32(param_1 + 4, c->r[2]);                                    // param_1[1] = its result
  c->r[2] = (int32_t)stage;                                            // (recomp returns param_2; harmless)
}

void ov_load_stage(Core* c) {
  if (cfg_on("PSXPORT_LOADSTAGE_RECOMP")) { rec_super_call(c, 0x800450BCu); return; }   // A/B oracle
  eng_load_stage(c);
}

// FUN_80052078(stageIdx) — the cooperative STAGE TRANSITION: load the next stage's overlay and RESTART
// task 0 at its new entry. The engine OWNS the transition; the actual overlay read stays the retained PSX
// CD content (dispatched through eng_load_stage / FUN_800450bc). Reimplemented PC-native, NOT transcribed:
// the PSX body ends with thread plumbing (EnterCriticalSection / CloseThread(task[4]) / ExitCriticalSection /
// ChangeThread(0xff000000)) whose only purpose is to stop the current task and switch away. The native
// cooperative scheduler (native_scheduler_step) replaces all of that — setting the task state to 3 ("restart
// fresh at the new entry") IS the transition, and the terminal ChangeThread is the existing native yield
// primitive ov_switch (0x80080880). The four thread/critical-section calls are RAM- and IRQ-flag-neutral in
// the native model (Enter+Exit cancel; Close/Change only set v0), so dropping them is faithful to the
// content interface (verified by the field RAM A/B gate). The yield here is TERMINAL — the task never
// resumes into this function, it restarts at state 3 — so a plain override that does the work then ends the
// run is correct; no coro-redirect handshake is needed (unlike the running dispatcher, later-169).
//
// ov_switch behaves per context, matching the PSX: mid-game (in a task run, sched.in_stage==1) it longjmps
// to the scheduler and this function never returns (== ChangeThread suspending the task); at boot the
// START->DEMO->GAME init transitions run via rc0(FUN_800499e8) with in_stage==0, where ov_switch is a no-op
// return and this function returns to FUN_800499e8, which continues — exactly as the stubbed thread layer
// did. Either way the scheduler then restarts task 0 (state 3) at the new stage entry.
// Faithful to 0x80052078: prologue sp-=0x18, sw s0,0x10(sp), sw ra,0x14(sp); eng_load_stage(task+0xc,stage);
// task[0]=3; task[0x6f]=0; (thread plumbing -> native scheduler); ChangeThread(0xff000000).
static void eng_stage_transition(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29], s0 = c->r[16];
  uint32_t stage = c->r[4];
  c->r[29] = sp - 0x18;
  c->mem_w32(c->r[29] + 0x10, s0);                  // sw s0,16(sp)  (mirror prologue stack writes byte-faithful)
  c->mem_w32(c->r[29] + 0x14, ra);                  // sw ra,20(sp)
  uint32_t task = c->mem_r32(0x1f800138u);
  c->r[4] = task + 0xc; c->r[5] = stage;
  rec_dispatch(c, 0x800450BCu);                     // eng_load_stage: load the overlay + set the task's new entry
  task = c->mem_r32(0x1f800138u);
  c->mem_w16(task, 3);                              // task state = 3 (RESTART fresh at the new entry)
  c->mem_w8(task + 0x6f, 0);                        // task[0x6f] = 0
  c->r[16] = s0; c->r[29] = sp; c->r[31] = ra;      // epilogue: restore s0/sp/ra (for the boot no-op-return path)
  c->r[4] = 0xff000000u;                            // ChangeThread handle arg
  rec_dispatch(c, 0x80080880u);                     // ov_switch: yield (mid-game) / no-op return (boot)
}

void ov_stage_transition(Core* c) {
  eng_stage_transition(c);
}
