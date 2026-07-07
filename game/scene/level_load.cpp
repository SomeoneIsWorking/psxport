// PC-native engine LEVEL / STAGE LOADING (the engine system that reads a stage off the disc and brings
// the world into being). Per the CLAUDE.md boundary this is ENGINE → reimplemented PC-native; the bytes
// it loads (stage overlay code + level data) and the per-entity AI/physics that later run on it stay PSX.
//
// Entry chain from main: task0 = the stage sequencer FUN_800499e8 (resolves \BIN\START.BIN → its disc
// LBA/size into the stage table DAT_800be1e0/e4) → FUN_80052078(stage) (restart task at the stage) →
// FUN_800450bc(task, stage) = THIS loader. RE: scratch/decomp + tools/disas.py (later-162).
#include "core.h"
#include "cfg.h"
#include "scheduler.h"   // switch (FUN_80080880)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);
void rec_coro_redirect(Core*, uint32_t);   // hand the flat interp control IN-CONTEXT (later-169 handshake)

// FUN_800450bc — load a stage's overlay off the disc and set the task's stage entry point.
//   a0 (param_1) = the task's entry-pointer slot (2 words: [0]=entry fn, [1]=task stack/handle)
//   a1 (param_2) = stage index;  stage 3 = the resident default (no load, entry = *0x800a3ed8)
// Per-stage (LBA,size) pairs live at 0x800be1e0/0x800be1e4 (stride 8); the overlay loads to 0x80106228;
// the per-stage entry table is at 0x800a3ecc. The PSX yields a frame after the load to wait for the async
// CD (FUN_80051f80(1)); our overlay loader (cd_loadfile, the 0x8001db8c override) is SYNCHRONOUS — the
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

// FUN_80052078(stageIdx) — the cooperative STAGE TRANSITION: load the next stage's overlay and RESTART
// task 0 at its new entry. The engine OWNS the transition; the actual overlay read stays the retained PSX
// CD content (dispatched through eng_load_stage / FUN_800450bc). Reimplemented PC-native, NOT transcribed:
// the PSX body ends with thread plumbing (EnterCriticalSection / CloseThread(task[4]) / ExitCriticalSection /
// ChangeThread(0xff000000)) whose only purpose is to stop the current task and switch away. The native
// cooperative scheduler (PcScheduler::step) replaces all of that — setting the task state to 3 ("restart
// fresh at the new entry") IS the transition, and the terminal ChangeThread is the existing native yield
// primitive switch (0x80080880). The four thread/critical-section calls are RAM- and IRQ-flag-neutral in
// the native model (Enter+Exit cancel; Close/Change only set v0), so dropping them is faithful to the
// content interface (verified by the field RAM A/B gate). The yield here is TERMINAL — the task never
// resumes into this function, it restarts at state 3 — so a plain override that does the work then ends the
// run is correct; no coro-redirect handshake is needed (unlike the running dispatcher, later-169).
//
// switch behaves per context, matching the PSX: mid-game (in a task run, pcSched.in_stage==1) it longjmps
// to the scheduler and this function never returns (== ChangeThread suspending the task); at boot the
// START->DEMO->GAME init transitions run via rc0(FUN_800499e8) with in_stage==0, where switch is a no-op
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
  eng_load_stage(c);                                // native (was rec_dispatch(0x800450BCu))
  task = c->mem_r32(0x1f800138u);
  c->mem_w16(task, 3);                              // task state = 3 (RESTART fresh at the new entry)
  c->mem_w8(task + 0x6f, 0);                        // task[0x6f] = 0
  c->r[16] = s0; c->r[29] = sp; c->r[31] = ra;      // epilogue: restore s0/sp/ra (for the boot no-op-return path)
  c->r[4] = 0xff000000u;                            // ChangeThread handle arg
  scheduler_yield(c);                                     // yield (mid-game) / no-op return (boot) — native
}

// FUN_800499e8 — task-0 INITIAL ENTRY (the engine's first-level bootstrap, registered as task 0 by
// FUN_80051f14 in the init prefix). It resolves the first stage's overlay file off the disc, records its
// (LBA,size) in the stage table, and transitions to stage 0. Engine-owned PC-native: the engine bootstraps
// the first level; the disc CD-directory lookup (FUN_8008b8f0) and MSF->LBA decode (FUN_8008a110) stay the
// retained PSX/platform mechanism (called, not reimplemented — correct boundary: file/asset *loading* is
// engine, the CD/filesystem primitive is platform). Faithful to 0x800499e8:
//   sp-=0x30; DAT_1f80019a=0; s0="\BIN\START.BIN;1" (0x80015458); sw s0,0x28(sp); sw ra,0x2c(sp);
//   FUN_8008b8f0(buf=sp+0x10, name) -> v0; if v0!=0: stage[0].lba=FUN_8008a110(buf), stage[0].size=buf[+4];
//   else: FUN_8009a730("Not found file name ", name) [error]; then FUN_80052078(0) (native transition).
// Called once at boot via rc0(FUN_800499e8) (in_stage==0); FUN_80052078's terminal yield is a no-op return
// there, so this returns to its caller, exactly as the interpreted path did.
static void eng_task0_boot(Core* c) {
  const uint32_t name = 0x80015458u;                 // "\BIN\START.BIN;1"
  uint32_t ra = c->r[31], sp = c->r[29], s0_in = c->r[16];
  c->r[29] = sp - 0x30;
  uint32_t buf = c->r[29] + 0x10;                    // local CD dir-entry struct on the task stack
  c->mem_w8(0x1f80019Au, 0);                         // DAT_1f80019a = 0 (CD-load-done flag reset)
  c->mem_w32(c->r[29] + 0x28, s0_in);                // sw s0,0x28(sp)  — saves the INCOMING s0 (before s0=name)
  c->mem_w32(c->r[29] + 0x2c, ra);                   // sw ra,0x2c(sp)
  c->r[16] = name;                                   // s0 = name (AFTER the save; FUN_80052078 saves s0 on its stack)
  // Each callee saves its own ra into its stack frame; to keep that stack scratch byte-identical to the
  // interpreted body, set ra to the exact post-`jal` return address (jal_addr+8) each `jal` would link.
  c->r[4] = buf; c->r[5] = name;
  c->r[31] = 0x80049A10u;                            // jal 0x8008b8f0 @0x80049a08 -> ra
  rec_dispatch(c, 0x8008B8F0u);                      // resolve the file's CD directory entry into buf (platform CD)
  if (c->r[2] != 0) {
    c->r[4] = buf;
    c->r[31] = 0x80049A34u;                          // jal 0x8008a110 @0x80049a2c -> ra
    rec_dispatch(c, 0x8008A110u);                    // MSF (in buf) -> LBA
    c->mem_w32(0x800BE1E0u, c->r[2]);                // stage[0].lba
    c->mem_w32(0x800BE1E4u, c->mem_r32(buf + 4));    // stage[0].size = buf[+4]
  } else {
    c->r[4] = 0x8001546Cu; c->r[5] = name;           // "Not found file name "
    c->r[31] = 0x80049A24u;                          // jal 0x8009a730 @0x80049a1c -> ra
    rec_dispatch(c, 0x8009A730u);                    // error report
  }
  c->r[4] = 0;                                       // FUN_80052078(0): transition to stage 0
  c->r[31] = 0x80049A50u;                            // jal 0x80052078 @0x80049a48 -> ra
  eng_stage_transition(c);                           // native (was rec_dispatch(0x80052078u)) — frame still down for byte-faithful stack scratch
  c->r[16] = c->mem_r32(c->r[29] + 0x28);            // epilogue: lw s0,0x28(sp); lw ra,0x2c(sp)
  c->r[31] = c->mem_r32(c->r[29] + 0x2c);
  c->r[29] = sp;                                     // sp += 0x30
}

// =================================================================================================
// FUN_800753D4 — per-area CEL-GROUP LOAD-AND-WAIT (`ov_cel_load_wait`). Owned PC-native (prologue), with
// the cross-frame DMA-wait poll loop handed back IN-CONTEXT via the coro-redirect handshake (later-169).
//
// This is the area-asset bring-up primitive the area-init function FUN_800451d0 (via the per-area asset
// table walk FUN_800754f4) calls to pull one effect/animation CEL GROUP into VRAM and BLOCK until its
// upload finishes. The cel parse/VRAM layout/upload itself is the already-native BAV loader FUN_80096590
// (game/ui/bav_loader.cpp, ov_bav_load); this wrapper is the *load + completion-wait* around it.
// RE (tools/disas.py 0x800753D4):
//
//   0x800753d4 prologue: sp-=0x20; sw s1; s1=a0(out); a0=a1(desc); a1=-1; sw s0; sw ra; s0=a2(cbarg)
//   0x800753f0 v0_slot = FUN_80096480(a0=desc, a1=-1, a2=cbarg)  // cel-load wrapper (auto-slot -1; prefills
//                                                                //   upload callback 0x800964b4, a3=0;
//                                                                //   calls FUN_80096590 = ov_bav_load)
//   0x80075408 *(u16*)s1 = v0_slot                               // store the allocated SLOT at [out]
//   0x80075404 FUN_80096980(a0=cbarg, a1=sext16(v0_slot))        // kick the slot's UPLOAD state machine (->1)
//   0x80075410 LOOP: if (sext16(FUN_80096a40(0)) != 0) break;    // upload-DMA done? (FUN_800993a0 event poll)
//   0x80075424      else FUN_80051f80(1);                        // YIELD one frame (ChangeThread / DMA wait)
//   0x8007542c      goto LOOP;
//   0x80075434 epilogue: lw ra; lw s1; lw s0; jr ra; sp+=0x20
//
// ABI: a0 = u16* out-slot; a1 = BAV descriptor; a2 = callback arg4. v0 (ret) ignored by both callers.
//
// OWNERSHIP MODEL — why the prologue is native but the loop is redirected (NOT dropped). The poll loop is
// a *genuine cross-frame yield*: I measured it (`celloadverify` HITs) — the GPU-DMA upload is NOT complete
// on the first FUN_80096a40 check, so FUN_80051f80(1) runs and longjmps to the scheduler; the upload
// settles over the following frame(s). The earlier guess "synchronous upload ⇒ 0 iterations, drop the
// yield" was FALSE and would be a bandaid (it left the slot in state 1=allocating, never reaching
// 2=loaded). So the yield MUST be preserved. The two prologue callees DO run to completion without yielding
// (verified: after them the slot's state-byte is 1 as the recomp leaves it), so they are safe to dispatch
// natively here. We then `rec_coro_redirect` to the loop head 0x80075410 with the MIPS frame set up
// byte-faithfully (sp-=0x20; s0/s1/ra saved), so the recomp loop + its epilogue restore correctly and the
// deep yield resumes exactly as the PSX path would — same handshake engine.cpp uses for deep-yield
// handlers. The cel-system callees (FUN_80096480/80096980/80096a40 — bav_loader cel loader + upload state
// machine + DMA-done event poll) stay dispatched: they write the guest cel-system globals still-recomp
// content reads, and FUN_80096590 is itself an existing native override.
// =================================================================================================
static int32_t sext16(uint32_t v) { return (int32_t)(int16_t)(v & 0xffff); }
