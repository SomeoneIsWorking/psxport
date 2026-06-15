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
#include <setjmp.h>

void rec_coro_run(R3000* c, uint32_t pc); // flat coroutine interpreter (resumable, see interp.c)

// Call recompiled/overridden game fn `fn` with up to 3 args; runs to its `jr ra` and returns.
static void rc0(R3000* c, uint32_t fn) { rec_dispatch(c, fn); }
static void rc1(R3000* c, uint32_t fn, uint32_t a0) { c->r[4] = a0; rec_dispatch(c, fn); }
static void rc2(R3000* c, uint32_t fn, uint32_t a0, uint32_t a1) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static void rc3(R3000* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}

// --- Native cooperative scheduler (replaces FUN_80051e60) without ucontext ------------------
// Tomba2 runs up to 3 cooperative tasks (objs @0x801fe000, stride 0x70): task0 = the stage
// sequencer (START/DEMO/GAME), task1/2 = sub-tasks it spawns (asset loaders etc.). Each is an
// infinite/looping routine that yields via FUN_80051f80 once per frame. We run each as a
// resumable coroutine WITHOUT ucontext: a yield SAVES the PSX register context into the task's
// slot (the PSX stack lives in g_ram per-task at obj+8, untouched) and longjmps out of the
// interpreter call chain; we resume by restoring that context and continuing at the captured
// return address. This is the "later 29" design — a yield is a save/restore of a state struct,
// no native stack. native_scheduler_step mirrors FUN_80051e60: one pass over the 3 slots,
// running state==2 (runnable, resume) and state==3 (restart, fresh entry) tasks; the yield sets
// state=1 and FUN_800506d0 (called later in the frame) re-arms 1->2. This makes the cooperative
// handshakes work — e.g. task0's FUN_80044bd4 busy-waits (yielding) for DAT_1f80019b while the
// loader it spawned (task1) runs to completion across frames and sets the flag.
#define TASKBASE 0x801fe000u   // task obj table base (slot i at +i*0x70)
#define TASKSTRIDE 0x70u
#define CUR_TASK 0x1f800138u   // DAT_1f800138: scheduler "current task" ptr

static jmp_buf g_yield_jmp;    // longjmp target = the setjmp in native_scheduler_step
static R3000   g_task_ctx[3];  // saved register context per task slot
static int     g_in_stage;     // 1 while inside a task run (gates the yield override)
static int     g_cur_slot;     // task slot currently running (for the yield capture)
static int     g_task_started[3];  // slot has a live coroutine context (else state==2 == fresh)

// FUN_80080880 ChangeThread override = the universal task-switch primitive. Every cooperative
// switch funnels through it: FUN_80051f80 (yield, state=1), FUN_80051fb4 (task end, state=0) and
// FUN_80052078 (stage transition, state=3) all set the task state then call ChangeThread to stop
// running. While running a task we capture the full register context for the slot and longjmp
// back to the scheduler; the task resumes (state 2 after FUN_800506d0 re-arms a yield, or fresh
// at state 3) or ends (state 0). Outside a task run (init / pre-stage FUN_800499e8) it is a
// no-op returning the handle, exactly as the stubbed thread layer did. The caller (FUN_80051f80
// etc.) has already run its real body, so register side effects it needs on resume (e.g. it
// leaves v0=0x1f800000 for the stage loop head's `lw t0,0x138(v0)`) are captured.
static void ov_switch(R3000* c) {
  if (!g_in_stage) { c->r[2] = c->r[4]; return; }   // no-op: return the handle arg in v0
  g_task_ctx[g_cur_slot] = *c;      // r29=task SP, r31=return addr (resume point)
  longjmp(g_yield_jmp, 1);
}

// One scheduler pass over the 3 task slots (replaces FUN_80051e60).
static void native_scheduler_step(R3000* c) {
  R3000 loop = *c;                            // frame-loop context (gp etc. for fresh tasks)
  for (int i = 0; i < 3; i++) {
    uint32_t base = TASKBASE + (uint32_t)i * TASKSTRIDE;
    uint32_t st = mem_r16(base);
    if (st == 0) { g_task_started[i] = 0; continue; }   // free
    uint32_t resume_pc;
    // state==3 (restart at new entry) or state==2 on a slot with no live context (freshly
    // registered by FUN_80051f14) => fresh entry. state==2 with a live context => resume.
    if (st == 3 || (st == 2 && !g_task_started[i])) {
      resume_pc = mem_r32(base + 0xc);        // task entry
      g_task_ctx[i] = loop;                   // inherit gp; fresh sp/ra below
      g_task_ctx[i].r[29] = mem_r32(base + 8);// per-task PSX stack top (obj+8)
      g_task_ctx[i].r[31] = 0xDEAD0000u;      // sentinel return
      g_task_started[i] = 1;
    } else if (st == 2) {                     // runnable: resume after the previous yield
      resume_pc = g_task_ctx[i].r[31];
    } else {
      continue;                               // sleeping this frame (state==1)
    }
    mem_w16(base, 4);                         // running
    if (getenv("PSXPORT_SCHEDDBG"))
      fprintf(stderr, "[sched] slot %d st_in=%u resume_pc=0x%08X ra=0x%08X sp=0x%08X\n",
              i, st, resume_pc, g_task_ctx[i].r[31], g_task_ctx[i].r[29]);
    mem_w32(CUR_TASK, base);
    g_cur_slot = i;
    *c = g_task_ctx[i];
    g_in_stage = 1;
    if (setjmp(g_yield_jmp) == 0) {
      rec_coro_run(c, resume_pc);             // runs until ov_yield longjmps back here
      mem_w16(base, 0);                        // returned (jr ra sentinel): task ended -> free
      g_task_started[i] = 0;
    }
    g_in_stage = 0;
  }
  *c = loop;                                  // restore the frame-loop context
}

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
  rec_set_override(0x80080880u, ov_switch);    // ChangeThread -> native task switch (capture+longjmp)

  // Frame budget: an explicit PSXPORT_NATIVE_FRAMES always wins (headless tests). Otherwise, when
  // a window is up this is the real interactive game loop — run until the user closes the window
  // (SDL_QUIT -> exit(0) in present_window); headless with no cap defaults to 120 (CI/smoke).
  uint32_t nframes = 0;   // 0 == run until window close
  const char* nf = getenv("PSXPORT_NATIVE_FRAMES");
  if (nf) nframes = (uint32_t)strtoul(nf, 0, 0);
  else if (!getenv("PSXPORT_GPU_WINDOW")) nframes = 120;
  fprintf(stderr, "[native_boot] entering native frame loop (%s)\n",
          nframes ? "capped" : "interactive (until window close)");
  void hle_deliver_event(uint32_t ev_class, uint32_t spec);
  void pad_service_frame(void);
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    // Per-frame IRQ-driven events the game's waits poll via TestEvent (we deliver no preemptive
    // IRQs): VBlank classes + the sound-DMA-complete class 0xF0000009 (its callback FUN_80097030
    // would normally fire it; native SPU DMA is synchronous, so signal it ready each frame).
    hle_deliver_event(0xF2000003u, 0xFFFFFFFFu);
    hle_deliver_event(0xF0000001u, 0xFFFFFFFFu);
    hle_deliver_event(0xF0000009u, 0xFFFFFFFFu);
    // Per-frame draw/display-env setup (LAB_80050c6c top): the env struct pair for the current
    // back buffer is at 0x800e80a8 + DAT_1f800135*0x2070 (the Ghidra `+uVar1*0x81c` is word
    // arithmetic: 0x81c*4 = 0x2070 bytes); FUN_80081458 clears its ordering table.
    uint32_t envp = 0x800e80a8u + (uint32_t)mem_r8(0x1f800135) * 0x2070u;
    mem_w32(0x800ed8c8, envp);                               // PTR_DAT_800ed8c8
    rc2(c, 0x80081458, envp, 0x800);                         // ClearOTagR(ot, 0x800)
    mem_w16(0x800e809c, 0);                                  // DAT_800e809c = 0 (dwell counter)
    mem_w32(0x800bf4f4, mem_r32(0x800bf544));                // framebuffer ptr swap
    mem_w32(0x800bf544, (mem_r8(0x1f800135) * 0x14000 + 0x800bfe68) & 0xffffff);
    pad_service_frame();                                     // host input -> game pad buffer (pre-read)
    rc0(c, 0x800788ac);                                      // tick + present + audio (override)
    native_scheduler_step(c);                                // <- replaces FUN_80051e60
    rc1(c, 0x80080f6c, 0);                                   // draw sync
    rc0(c, 0x800506d0);                                      // task sleep-countdown (re-arm 1->2)
    // Buffer flip + display env (LAB_80050c6c, DAT_1f80019c==0 branch): submit this frame's
    // draw env / display env / ordering table to the GPU, then flip the double buffer.
    if (mem_r16(0x1f80019c) == 0) {
      rc1(c, 0x8008179c, envp + 0x2000);                    // PutDispEnv  (env+0x2000)
      rc1(c, 0x800815d0, envp + 0x2014);                    // PutDrawEnv  (env+0x2014)
      rc1(c, 0x80081560, envp + 0x1ffc);                    // DrawOTag (submit the OT head)
      mem_w8(0x1f800135, 1 - mem_r8(0x1f800135));           // flip back/front buffer
    }
    static uint32_t s_last_entry = 0; static uint32_t s_last_sm = 0xFFFFFFFF;
    uint32_t t0e = mem_r32(TASKBASE + 0xc), s48 = mem_r16(TASKBASE + 0x48);
    // GAME runs a 4-level nested state machine (task +0x48/4a/4c/4e). Track all of it so a
    // stuck leaf is visible, not just the outer s48.
    uint32_t sm = (mem_r16(TASKBASE+0x48)<<24)|(mem_r16(TASKBASE+0x4a)<<16)|
                  (mem_r16(TASKBASE+0x4c)<<8)|mem_r16(TASKBASE+0x4e)
                  ^ (mem_r16(TASKBASE+0x50)<<12)^(mem_r16(TASKBASE+0x52)<<4);
    if (t0e != s_last_entry || sm != s_last_sm) {
      const char* stg = t0e == 0x8010649Cu ? "START" : t0e == 0x801062E4u ? "DEMO" :
                        t0e == 0x8010637Cu ? "GAME" : "?";
      fprintf(stderr, "[native_boot]   frame %u: stage=%s(0x%08X) sm[48=%u 4a=%u 4c=%u 4e=%u 50=%u 52=%u]"
              " @0x80109450=%08X\n",
              f, stg, t0e, mem_r16(TASKBASE+0x48), mem_r16(TASKBASE+0x4a),
              mem_r16(TASKBASE+0x4c), mem_r16(TASKBASE+0x4e), mem_r16(TASKBASE+0x50),
              mem_r16(TASKBASE+0x52), mem_r32(0x80109450));
      s_last_entry = t0e; s_last_sm = sm;
    }
    // One-shot: when GAME has settled, dump the CD-streaming contract (FUN_8001cfc8, task
    // slot 2). task2 obj @0x801fe0e0; +0x54=start LBA, +0x58=end LBA (= globals
    // DAT_801fe134/138). DAT_801fe146=channel/type. _DAT_1f8001f8=dest, _DAT_1f8001f4=words.
    if (getenv("PSXPORT_STREAMDBG") && t0e == 0x8010637Cu && f == 75) {
      fprintf(stderr, "[streamdbg] task2 obj @0x801fe0e0 state=%u entry=0x%08X\n",
              mem_r16(0x801fe0e0), mem_r32(0x801fe0ec));
      fprintf(stderr, "[streamdbg] startLBA(+54/801fe134)=%u endLBA(+58/801fe138)=%u "
              "chan(801fe146)=%u be0e4=0x%02X\n",
              mem_r32(0x801fe134), mem_r32(0x801fe138), mem_r8(0x801fe146), mem_r8(0x800be0e4));
      fprintf(stderr, "[streamdbg] dest(_DAT_1f8001f8)=0x%08X words(_DAT_1f8001f4)=%u "
              "f0=%u f1f800224=0x%08X\n",
              mem_r32(0x1f8001f8), mem_r32(0x1f8001f4), mem_r32(0x1f8001f0), mem_r32(0x1f800224));
    }
    if (f < 10 || (f % 30) == 0)
      fprintf(stderr, "[native_boot]   frame %u: t0[st=%u e=0x%08X s48=%u] t1[st=%u] t2[st=%u] "
                      "f135=%u\n", f, mem_r16(TASKBASE), mem_r32(TASKBASE + 0xc),
              mem_r16(TASKBASE + 0x48), mem_r16(TASKBASE + 0x70), mem_r16(TASKBASE + 0xe0),
              mem_r8(0x1f800135));
  }
  fprintf(stderr, "[native_boot] frame loop done; task0 state=%u entry=0x%08X obj+0x48=%u\n",
          mem_r16(TASKBASE), mem_r32(TASKBASE + 0xc), mem_r16(TASKBASE + 0x48));
  const char* rd = getenv("PSXPORT_RAMDUMP");
  if (rd) {
    extern uint8_t g_ram[];
    FILE* f = fopen(rd, "wb");
    if (f) { fwrite(g_ram, 1, 0x200000, f); fclose(f);
             fprintf(stderr, "[native_boot] dumped 2MB RAM -> %s\n", rd); }
  }
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
