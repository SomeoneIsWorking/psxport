// PC-PSX hybrid native boot driver.
//
// Architecture (locked): the PC engine is the driver. It runs the game's INIT calls (the
// prefix of game-main FUN_80050b08) via the recompiler, then OWNS the frame loop, replacing
// the PSX cooperative-task scheduler (FUN_80051e60) with native per-frame stepping of the
// current stage's state machine. No BIOS threads, no ucontext: leaf logic fns are normal
// recompiled fns that RETURN; the stage sequencers' infinite yield-loops are reimplemented
// natively (one state-machine iteration per frame == one original FUN_80051f80 yield).
//
// We do NOT call FUN_80050b08 directly (it ends in the infinite scheduler loop). Instead we
// override it: crt0 (func_800896E0) does BSS-zero + SP/gp/heap setup and calls main, which is
// now this native driver. The init prefix below is transcribed 1:1 from FUN_80050b08
// (ram_f1000_all.c:31275-31299); each call is dispatched into the recompiled/overridden body,
// whose CD/sync/vsync/pad/thread dependencies are already native overrides.
//
// MILESTONE 1 (this file, current): run the init prefix and confirm it executes cleanly via
// PC/RAM probes. The native frame loop + per-stage stepping land next.
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>

// Call recompiled/overridden game fn `fn` with up to 3 args; runs to its `jr ra` and returns.
static void rc0(R3000* c, uint32_t fn) { rec_dispatch(c, fn); }
static void rc1(R3000* c, uint32_t fn, uint32_t a0) { c->r[4] = a0; rec_dispatch(c, fn); }
static void rc2(R3000* c, uint32_t fn, uint32_t a0, uint32_t a1) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static void rc3(R3000* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}

// Native per-frame stage step (replaces the scheduler call FUN_80051e60). Runs the current
// stage's state-machine one iteration per frame. Phase A: stub (loop-mechanics bring-up).
static void native_stage_step(R3000* c) { (void)c; }

// Native override of game-main FUN_80050b08: init prefix, then (later) native frame loop.
static void ov_game_main(R3000* c) {
  fprintf(stderr, "[native_boot] FUN_80050b08 override: running init prefix\n");

  // --- init prefix, transcribed from FUN_80050b08 (no scheduler loop) ---
  rc0(c, 0x80089788);
  rc0(c, 0x80085b20);
  rc0(c, 0x800898a0);
  rc1(c, 0x80080bf0, 3);
  rc1(c, 0x80080d64, 0);
  rc1(c, 0x80080ed4, 1);
  rc1(c, 0x800865f0, 0);
  rc0(c, 0x80050a0c);
  rc0(c, 0x800509b4);
  rc0(c, 0x80050a80);
  rc0(c, 0x80096a70);
  rc1(c, 0x80099310, 0x1010);
  rc1(c, 0x800991b0, 0x20000);
  rc1(c, 0x800993a0, 1);
  // FUN_80089bac(0xe, &local_28, 0) with local_28[0] = 0x80 (a stack byte buffer).
  uint32_t buf = c->r[29] - 0x40;
  mem_w8(buf, 0x80);
  rc3(c, 0x80089bac, 0xe, buf, 0);
  rc1(c, 0x80085900, 3);
  rc0(c, 0x80075130);
  rc1(c, 0x8009c620, 0);
  rc0(c, 0x8001cc00);
  rc0(c, 0x800520e0);
  rc1(c, 0x80085900, 1);
  rc0(c, 0x80051e00);                       // scheduler-table init (task objs @0x801fe000)
  rc2(c, 0x80051f14, 0, 0x800499e8);        // register task 0, entry FUN_800499e8
  rc1(c, 0x80085bb0, 0x800506b4);           // register the vsync callback LAB_800506b4

  fprintf(stderr, "[native_boot] init prefix complete\n");

  // --- task 0 initial entry: FUN_800499e8 resolves \BIN\START.BIN and FUN_80052078(0) loads
  // the stage-0 overlay to 0x80106228 + restarts task 0 at stage 0 (0x8010649c). It yields once
  // (FUN_80051f80, a no-op with threads stubbed) so it runs straight to completion here. The
  // scheduler's "current task" ptr DAT_1f800138 is normally set by FUN_80051e60; set it to task0
  // so FUN_80052078/FUN_800450bc operate on task 0. ---
  mem_w32(0x1f800138, 0x801fe000);
  rc0(c, 0x800499e8);
  // START.BIN loaded raw to 0x80106228: [0]=manifest count (6); entry word @0x8010649c.
  fprintf(stderr, "[native_boot] after FUN_800499e8: START.BIN count@0x80106228=%u "
                  "entry-word@0x8010649c=0x%08X (expect 0x27BDFE38); task0 state=%u entry=0x%08X\n",
          mem_r32(0x80106228), mem_r32(0x8010649c), mem_r16(0x801fe000), mem_r32(0x801fe00c));
  // --- native frame loop (replaces LAB_80050c6c). Per frame, faithful to the game-main loop
  // body but with the scheduler call FUN_80051e60 replaced by native stage stepping (added
  // incrementally). FUN_800788ac is overridden (ov_frame_update): real per-frame update +
  // gpu_present + audio + satisfies the vblank pacing dwell. PSXPORT_NATIVE_FRAMES caps the
  // run (headless). ---
  uint32_t nframes = 120;
  const char* nf = getenv("PSXPORT_NATIVE_FRAMES");
  if (nf) nframes = (uint32_t)strtoul(nf, 0, 0);
  fprintf(stderr, "[native_boot] entering native frame loop (%u frames)\n", nframes);
  for (uint32_t f = 0; f < nframes; f++) {
    mem_w16(0x800e809c, 0);                                  // DAT_800e809c = 0 (dwell counter)
    mem_w32(0x800bf4f4, mem_r32(0x800bf544));                // framebuffer ptr swap
    mem_w32(0x800bf544, (mem_r8(0x1f800135) * 0x14000 + 0x800bfe68) & 0xffffff);
    rc0(c, 0x800788ac);                                      // tick + present + audio (override)
    native_stage_step(c);                                    // <- replaces FUN_80051e60
    rc1(c, 0x80080f6c, 0);                                   // draw sync
    rc0(c, 0x800506d0);                                      // task sleep-countdown
  }
  fprintf(stderr, "[native_boot] frame loop done; task0 state=%u entry=0x%08X obj+0x48=%u\n",
          mem_r16(0x801fe000), mem_r32(0x801fe00c), mem_r16(mem_r32(0x1f800138) + 0x48));
}

// Wired from boot.c when PSXPORT_NATIVE_BOOT is set. Registers the main override and enters
// crt0; crt0's call to FUN_80050b08 lands in ov_game_main.
void native_boot_run(R3000* c) {
  void func_800896E0(R3000*);
  rec_set_override(0x80050b08u, ov_game_main);
  fprintf(stderr, "[native_boot] entering crt0 func_800896E0\n");
  func_800896E0(c);
  fprintf(stderr, "[native_boot] returned from crt0\n");
}
