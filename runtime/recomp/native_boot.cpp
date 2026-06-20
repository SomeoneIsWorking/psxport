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
#include "core.h"
#include "game.h"      // SchedulerState (per-instance cooperative-task state) reached via c->game->sched
#include "c_subsys.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>   // usleep (debug-server pause/step idle wait)

void rec_coro_run(Core* c, uint32_t pc); // flat coroutine interpreter (resumable, see interp.c)
OverrideFn rec_interp_override_for(uint32_t a);  // unified address-keyed override lookup (interp.c)

// Native XA voice/BGM clip player (xa_stream.c) owns task slot 2 — it replaced the FUN_8001cfc8
// streaming-reader coroutine. The scheduler skips slot 2 while owned and reflects clip completion
// into the task-2 state byte (the cutscene waits `while (DAT_801fe0e0 != 0)`).
void xa_dialog_coord(Core* c);          // dialog-vs-ingame-music coordination (cd_override.c)
void xa_audio_trace(Core* c, const char* tag);    // CD-vol fade + XA lifecycle trace (cd_override.c)

// Call recompiled/overridden game fn `fn` with up to 3 args; runs to its `jr ra` and returns.
static void rc0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static void rc1(Core* c, uint32_t fn, uint32_t a0) { c->r[4] = a0; rec_dispatch(c, fn); }
static void rc2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) {
  c->r[4] = a0; c->r[5] = a1; rec_dispatch(c, fn);
}
static void rc3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; rec_dispatch(c, fn);
}

// --- Native cooperative scheduler (replaces FUN_80051e60) without ucontext ------------------
// Tomba2 runs up to 3 cooperative tasks (objs @0x801fe000, stride 0x70): task0 = the stage
// sequencer (START/DEMO/GAME), task1/2 = sub-tasks it spawns (asset loaders etc.). Each is an
// infinite/looping routine that yields via FUN_80051f80 once per frame. We run each as a
// resumable coroutine WITHOUT ucontext: a yield SAVES the PSX register context into the task's
// slot (the PSX stack lives in c->ram per-task at obj+8, untouched) and longjmps out of the
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

// The cooperative-scheduler state (yield jmp_buf, per-task saved R3000 regs, run flags) is per-instance
// now: it lives on Game as SchedulerState (game.h), reached via c->game->sched. A task context is ONLY
// the CPU register file — guest RAM/scratchpad/DMA/peripherals are SHARED one memory across all tasks
// (saving a whole Core would give each task its own RAM snapshot — the OOP regression where the loader
// task read a pre-fill file-table snapshot and stalled boot; see oop-regression-hunt). So task_ctx
// slices to the R3000 base on save/restore.

// FUN_80080880 ChangeThread override = the universal task-switch primitive. Every cooperative
// switch funnels through it: FUN_80051f80 (yield, state=1), FUN_80051fb4 (task end, state=0) and
// FUN_80052078 (stage transition, state=3) all set the task state then call ChangeThread to stop
// running. While running a task we capture the full register context for the slot and longjmp
// back to the scheduler; the task resumes (state 2 after FUN_800506d0 re-arms a yield, or fresh
// at state 3) or ends (state 0). Outside a task run (init / pre-stage FUN_800499e8) it is a
// no-op returning the handle, exactly as the stubbed thread layer did. The caller (FUN_80051f80
// etc.) has already run its real body, so register side effects it needs on resume (e.g. it
// leaves v0=0x1f800000 for the stage loop head's `lw t0,0x138(v0)`) are captured.
static void ov_switch(Core* c) {
  if (!c->game->sched.in_stage) { c->r[2] = c->r[4]; return; }   // no-op: return the handle arg in v0
  c->game->sched.task_ctx[c->game->sched.cur_slot] = static_cast<R3000&>(*c);  // save REGISTERS only (r29=task SP, r31=resume ra)
  longjmp(c->game->sched.yield_jmp, 1);
}

// One scheduler pass over the 3 task slots (replaces FUN_80051e60).
static void native_scheduler_step(Core* c) {
  R3000 loop = *c;                           // frame-loop REGISTERS (gp etc. for fresh tasks); slices off RAM
  for (int i = 0; i < 3; i++) {
    uint32_t base = TASKBASE + (uint32_t)i * TASKSTRIDE;
    // Task slot 2 = XA voice/BGM. When the native clip player owns it, do NOT run the (now unused)
    // FUN_8001cfc8 recomp coroutine; instead reflect clip state into the task-2 state byte so the
    // cutscene's `while (DAT_801fe0e0 != 0)` wait advances exactly when the clip finishes.
    if (i == 2 && xa_stream_owns_slot2()) {
      if (xa_stream_voice_busy()) c->mem_w16(base, 2);     // still playing -> stay "running"
      else { c->mem_w16(base, 0); xa_stream_voice_release(); }  // clip done -> free -> cutscene advances
      c->game->sched.task_started[2] = 0;
      continue;
    }
    uint32_t st = c->mem_r16(base);
    if (st == 0) { c->game->sched.task_started[i] = 0; continue; }   // free
    uint32_t resume_pc;
    int fresh = 0;
    // state==3 (restart at new entry) or state==2 on a slot with no live context (freshly
    // registered by FUN_80051f14) => fresh entry. state==2 with a live context => resume.
    if (st == 3 || (st == 2 && !c->game->sched.task_started[i])) {
      resume_pc = c->mem_r32(base + 0xc);        // task entry
      fresh = 1;
      c->game->sched.task_ctx[i] = loop;                   // inherit gp; fresh sp/ra below
      c->game->sched.task_ctx[i].r[29] = c->mem_r32(base + 8);// per-task PSX stack top (obj+8)
      c->game->sched.task_ctx[i].r[31] = 0xDEAD0000u;      // sentinel return
      c->game->sched.task_started[i] = 1;
    } else if (st == 2) {                     // runnable: resume after the previous yield
      resume_pc = c->game->sched.task_ctx[i].r[31];
    } else {
      continue;                               // sleeping this frame (state==1)
    }
    c->mem_w16(base, 4);                         // running
    if (cfg_dbg("sched"))
      fprintf(stderr, "[sched] slot %d st_in=%u resume_pc=0x%08X ra=0x%08X sp=0x%08X\n",
              i, st, resume_pc, c->game->sched.task_ctx[i].r[31], c->game->sched.task_ctx[i].r[29]);
    c->mem_w32(CUR_TASK, base);
    c->game->sched.cur_slot = i;
    static_cast<R3000&>(*c) = c->game->sched.task_ctx[i];   // restore REGISTERS only (shared RAM untouched)
    c->game->sched.in_stage = 1;
    if (setjmp(c->game->sched.yield_jmp) == 0) {
      uint32_t start = resume_pc;
      // A task ENTRY may be a native override (e.g. the GAME stage prologue ov_game_stage_main).
      // interp_flat only fires overrides on a control transfer INTO an address, never at its start
      // pc, and nothing `jal`s a task entry — so fire the entry override here, as a native call,
      // exactly once on a FRESH entry (NOT on a resume, whose saved pc is deep guest code that must
      // be interpreted, not re-dispatched). The override does its prologue and either rec_coro_redirect's
      // to where the interp should continue (coro_redirect_pc) or returns to its ra; run flat from there.
      if (fresh) {
        OverrideFn ov = rec_interp_override_for(resume_pc);
        if (ov) { extern uint32_t g_override_tgt; g_override_tgt = resume_pc; ov(c);
                  start = c->coro_redirect_pc ? c->coro_redirect_pc : c->r[31];
                  c->coro_redirect_pc = 0; }
      }
      rec_coro_run(c, start);                  // runs until ov_yield longjmps back here
      c->mem_w16(base, 0);                        // returned (jr ra sentinel): task ended -> free
      c->game->sched.task_started[i] = 0;
    }
    c->game->sched.in_stage = 0;
  }
  static_cast<R3000&>(*c) = loop;             // restore the frame-loop REGISTERS (shared RAM untouched)
}

// ---- BGM start/stop diagnostic (PSXPORT_BGMDBG=1) --------------------------------------------
// FUN_80074BF8(idx) starts BGM #idx (writes the current-song index 0x800bed80, plays the seq);
// FUN_80074E48() stops the current BGM (sets 0x800bed80=0xFFFF). Tomba2's gameplay BGM is silent
// in this port because the start IS reached (idx written) but an immediate STOP clears it. Log
// each call with the caller (ra), arg, and the index before/after to find the spurious stopper.
volatile uint32_t g_bgm_frame = 0;   // current logic frame (extern: cd_override.c trace)
void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)
void xa_music_cut_if_dialog(Core*);   // cd_override.c: stop looping ingame music when a dialog tone starts
static int s_bgmdbg = -1;
static void ov_bgm_start(Core* c) {
  if (s_bgmdbg < 0) s_bgmdbg = (cfg_str("PSXPORT_BGMDBG") ? atoi(cfg_str("PSXPORT_BGMDBG")) : 0);
  if (s_bgmdbg) fprintf(stderr, "[bgmdbg] f%u BGM_START(idx=0x%02X) ra=%08X  idx@800bed80(before)=0x%04X\n",
                        g_bgm_frame, c->r[4] & 0xFF, c->r[31], c->mem_r16(0x800bed80));
  rec_super_call(c, 0x80074BF8u);
  // Dialog-tone songs (current-song 4..7) cut the looping ingame music SYNCHRONOUSLY here — at the
  // song-start, before this frame's audio mix — so it can't leak a frame past the dialog start
  // (the per-frame xa_dialog_coord stop is one frame late vs the audio mix). Instant-CD mod.
  xa_music_cut_if_dialog(c);
  if (s_bgmdbg) fprintf(stderr, "[bgmdbg]   -> idx@800bed80(after)=0x%04X\n", c->mem_r16(0x800bed80));
}
static void ov_bgm_stop(Core* c) {
  if (s_bgmdbg < 0) s_bgmdbg = (cfg_str("PSXPORT_BGMDBG") ? atoi(cfg_str("PSXPORT_BGMDBG")) : 0);
  if (s_bgmdbg) fprintf(stderr, "[bgmdbg] f%u BGM_STOP ra=%08X  idx@800bed80(before)=0x%04X\n",
                        g_bgm_frame, c->r[31], c->mem_r16(0x800bed80));
  rec_super_call(c, 0x80074E48u);
}

// ONE frame of deterministic guest work — the steppable core of the native frame loop, factored out so
// the in-process dual-core diff can step TWO cores in lockstep (it calls this on `a` then `b`). It is
// EXACTLY the guest-mutating body of the loop below (per-frame IRQ events, draw/display-env setup, the
// FUN_800788ac frame update + native scheduler pass + dialog-music coord + draw sync + buffer flip); the
// loop's driver scaffolding (REPL, auto-navigation/input, pause/step, diagnostics, dbg_server) stays in
// the loop and runs ONCE around this call. No input is injected here — drive pads before calling it.
static void native_step_frame(Core* c, uint32_t f) {
  void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
  void pad_service_frame(Core*);
  void gpu_set_disp_origin(Core* c, int x, int y);
  (void)f;
  // Per-frame IRQ-driven events the game's waits poll via TestEvent (VBlank classes + sound-DMA-complete).
  hle_deliver_event(c, 0xF2000003u, 0xFFFFFFFFu);
  hle_deliver_event(c, 0xF0000001u, 0xFFFFFFFFu);
  hle_deliver_event(c, 0xF0000009u, 0xFFFFFFFFu);
  // SINGLE-BUFFERED (PC-native) — the game's own PSX double-buffering is REMOVED (user: "remove the
  // game's own double buffering, it causes problems"). The PSX flips between two VRAM pages each frame:
  // OT region 0x800e80a8 + parity*0x2070 and packet pool 0x800bfe68 + parity*0x14000, with the display/
  // draw env following parity. That page-flip is pointless on PC — the VK renderer composites a COMPLETE
  // frame and the present provides the display buffering — and it actively caused aliasing: the native
  // display-list buckets are keyed by OT address (ndl_alloc on otaddr & 0x1FFFFC), so they alternated
  // between the two pages, and the present could sample the page being drawn. Pin the back-buffer parity
  // to 0: one OT region, one packet pool, the same env every frame, and NO flip below. 0x1f800135 stays
  // 0 (set by eng_init_framestate), so any guest code that reads it also sees a stable single buffer.
  const uint32_t parity = 0;                                 // was mem_r8(0x1f800135) — pinned single-buffer
  uint32_t envp = 0x800e80a8u + parity * 0x2070u;            // the one VRAM region we DRAW into every frame
  c->mem_w32(0x800ed8c8, envp);                               // PTR_DAT_800ed8c8 (OT base, now constant)
  rc2(c, 0x80081458, envp, 0x800);                            // ClearOTagR(ot, 0x800)
  c->mem_w16(0x800e809c, 0);                                  // DAT_800e809c = 0 (dwell counter)
  c->mem_w32(0x800bf4f4, c->mem_r32(0x800bf544));             // keep last pool ptr (read by some submitters)
  c->mem_w32(0x800bf544, (parity * 0x14000 + 0x800bfe68) & 0xffffff);   // packet pool (now constant)
  c->game->gpu.s_pool_drawn = 0x800bfe68u & 0x1FFFFC;        // M3: reset the per-frame "pool drawn" cursor with the pool
  pad_service_frame(c);                                       // host input -> game pad buffer (pre-read)
  xa_audio_trace(c, "pre");                                   // CD-vol fade state BEFORE tick+mix
  rc0(c, 0x800788ac);                                         // tick + present + audio (override)
  xa_audio_trace(c, "post");                                  // CD-vol fade state AFTER tick+mix
  native_scheduler_step(c);                                   // <- replaces FUN_80051e60
  xa_dialog_coord(c);                                         // dialogs stop/restore ingame music
  xa_audio_trace(c, "coord");                                 // CD-vol fade state AFTER coord
  rc1(c, 0x80080f6c, 0);                                      // draw sync
  rc0(c, 0x800506d0);                                         // task sleep-countdown (re-arm 1->2)
  // Display + OT submit (LAB_80050c6c, DAT_1f80019c==0 branch). Single-buffered, PC-native display.
  // The PSX disp-env dance is GONE: we draw page `parity` (env0 → VRAM (0,0)) and tell the PC present to
  // scan that very page DIRECTLY via gpu_set_disp_origin, instead of routing through PutDispEnv and the
  // opposite buffer's disp-env struct (the later-161 (1-parity) trick — a band-aid over the env pairing
  // that depended on the two structs lining up). One fixed page, drawn and displayed; nothing PSX in the
  // display path. (env0's draw area starts at VRAM (0,0); the display W/H stay as the boot env's mode set.)
  if (c->mem_r16(0x1f80019c) == 0) {
    rc1(c, 0x800815d0, envp + 0x2014);                        // PutDrawEnv (draw area/offset/clip for page 0)
    gpu_set_disp_origin(c, 0, 0);                             // PC-native: present scans the page we draw
    rc1(c, 0x80081560, envp + 0x1ffc);                        // DrawOTag (submit the OT head)
  }
}

// Native override of game-main FUN_80050b08: init prefix, then (later) native frame loop.
// ---- Interactive REPL (PSXPORT_REPL=1) — drive the native port from stdin --------------------
// Mirrors the oracle's (wide60rt -repl) command set so one driver can step BOTH cores and diff.
// Commands: run N | r addr [len] | rw addr [words] | w addr val | w8 addr val | watch lo hi |
//   unwatch | hits | press/release <btn> | tap <btn> [frames] | regs | seq | quit. Memory is the
//   game's address space (mem_r*/mem_w*); watchpoints via mem_set_watch (reported during `run`).
void pad_repl_hold(Core* c, uint16_t active_low_mask);
void pad_repl_tap(Core* c, uint16_t active_low_mask, int n);
static uint16_t repl_btn(const char* n) {     // name -> active-HIGH PSX pad bit
  if (!strcmp(n,"start"))    return 0x0008; if (!strcmp(n,"select")) return 0x0001;
  if (!strcmp(n,"x")||!strcmp(n,"cross"))  return 0x4000;
  if (!strcmp(n,"o")||!strcmp(n,"circle")) return 0x2000;
  if (!strcmp(n,"triangle")||!strcmp(n,"t")) return 0x1000;
  if (!strcmp(n,"square")||!strcmp(n,"sq"))  return 0x8000;
  if (!strcmp(n,"up"))    return 0x0010; if (!strcmp(n,"down"))  return 0x0040;
  if (!strcmp(n,"left"))  return 0x0080; if (!strcmp(n,"right")) return 0x0020;
  return (uint16_t)strtoul(n, 0, 16);
}
// ---- REPL music-dump helpers (build a labeled track library to identify each tune) -------
// Sequenced BGM is rendered through the live SPU (use `wav PATH` then `bgm N` then `run`);
// XA tracks (CD-streamed music/voice) are decoded straight off the disc here, since they
// never touch the sequencer. Both write standard 44100/native-rate stereo S16 WAVs.

static void repl_wav_write(const char* path, const int16_t* pcm, uint32_t frames, int rate) {
  FILE* fp = fopen(path, "wb");
  if (!fp) { fprintf(stderr, "[repl] wav write: cannot open %s\n", path); return; }
  uint32_t data = frames * 4u, riff = 36u + data, brate = (uint32_t)rate * 4u, fmtlen = 16;
  uint16_t pcm1 = 1, ch2 = 2, ba = 4, bits = 16; uint32_t r = (uint32_t)rate;
  fwrite("RIFF", 1, 4, fp); fwrite(&riff, 4, 1, fp); fwrite("WAVE", 1, 4, fp);
  fwrite("fmt ", 1, 4, fp); fwrite(&fmtlen, 4, 1, fp);
  fwrite(&pcm1, 2, 1, fp); fwrite(&ch2, 2, 1, fp); fwrite(&r, 4, 1, fp);
  fwrite(&brate, 4, 1, fp); fwrite(&ba, 2, 1, fp); fwrite(&bits, 2, 1, fp);
  fwrite("data", 1, 4, fp); fwrite(&data, 4, 1, fp);
  fwrite(pcm, 4, frames, fp); fclose(fp);
  fprintf(stderr, "[repl] xadump -> %s (%u frames @ %d Hz, %.2fs)\n", path, frames, rate, frames / (double)rate);
}

// Decode ~`secs` of the XA stream on subheader channel `chan` starting at CHD `start_lba`,
// write a WAV at the stream's native rate. Skips interleaved non-matching/non-audio sectors.
static void repl_xadump(uint8_t chan, uint32_t start_lba, const char* path, int secs) {
  static int16_t out[400000];                 // ~4.5s of 44100 stereo; XA max 37800*secs
  uint8_t raw[2352]; int16_t hist[2][2] = {{0,0},{0,0}}; int freq = 37800;
  uint32_t frames = 0, lba = start_lba, cap = 0;
  for (int guard = 0; guard < 20000; guard++) {
    if (!disc_read_raw(lba, raw, 2352)) break;
    if (raw[15] != 2) break;                   // ran off the Mode2 stream
    uint8_t fchan = raw[17], submode = raw[18];
    lba++;
    if (!(submode & 0x04) || fchan != chan) { if (submode & 0x80) break; continue; }  // not our audio
    int16_t pcm[4032 * 2]; int f2 = freq;
    int n = xa_decode_sector(raw, pcm, hist, &f2); freq = f2;
    if (!cap) cap = (uint32_t)freq * (uint32_t)secs;
    for (int i = 0; i < n && frames < cap && frames < 200000; i++) {
      out[2 * frames] = pcm[2 * i]; out[2 * frames + 1] = pcm[2 * i + 1]; frames++;
    }
    if (frames >= cap || (submode & 0x80)) break;
  }
  repl_wav_write(path, out, frames, freq);
}

// Read+execute REPL commands until a `run N` (returns N) or quit/EOF (returns -1).
static long native_repl_read(Core* c, uint32_t f) {
  static uint16_t held = 0xFFFF;              // active-low held mask (all released)
  char line[256];
  fprintf(stderr, "[repl] frame=%u ready\n", f); fflush(stderr);
  while (fgets(line, sizeof line, stdin)) {
    char cmd[24] = {0}, arg[32] = {0}; unsigned a = 0, b = 0;
    if (sscanf(line, "%23s", cmd) != 1) continue;
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) return -1;
    else if (!strcmp(cmd, "run") && sscanf(line, "%*s %u", &a) == 1) return (long)a;
    else if (!strcmp(cmd, "r") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
      if (!b) b = 16; fprintf(stderr, "[repl] %08X:", a);
      for (unsigned i = 0; i < b && i < 256; i++) fprintf(stderr, " %02X", c->mem_r8(a + i)); fprintf(stderr, "\n");
    } else if (!strcmp(cmd, "rw") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
      if (!b) b = 8; fprintf(stderr, "[repl] %08X:", a);
      for (unsigned i = 0; i < b && i < 64; i++) fprintf(stderr, " %08X", c->mem_r32(a + i * 4)); fprintf(stderr, "\n");
    } else if (!strcmp(cmd, "w") && sscanf(line, "%*s %x %x", &a, &b) == 2) { c->mem_w32(a, b); fprintf(stderr, "[repl] ok\n"); }
    else if (!strcmp(cmd, "w8") && sscanf(line, "%*s %x %x", &a, &b) == 2) { c->mem_w8(a, (uint8_t)b); fprintf(stderr, "[repl] ok\n"); }
    else if (!strcmp(cmd, "watch") && sscanf(line, "%*s %x %x", &a, &b) == 2) c->mem_set_watch(a, b);
    else if (!strcmp(cmd, "unwatch")) { c->mem_set_watch(0, 0); fprintf(stderr, "[repl] unwatch\n"); }
    else if (!strcmp(cmd, "hits")) fprintf(stderr, "[repl] watch hits=%d\n", c->mem_watch_hits());
    else if (!strcmp(cmd, "press") && sscanf(line, "%*s %31s", arg) == 1)   { held &= ~repl_btn(arg); pad_repl_hold(c, held); fprintf(stderr, "[repl] held=%04X\n", held); }
    else if (!strcmp(cmd, "release") && sscanf(line, "%*s %31s", arg) == 1) { held |= repl_btn(arg);  pad_repl_hold(c, held); fprintf(stderr, "[repl] held=%04X\n", held); }
    else if (!strcmp(cmd, "tap") && sscanf(line, "%*s %31s %u", arg, &a) >= 1) { if (!a) a = 4; pad_repl_tap(c, (uint16_t)(0xFFFF & ~repl_btn(arg)), (int)a); fprintf(stderr, "[repl] tap %s %u\n", arg, a); }
    else if (!strcmp(cmd, "shot")) { char path[200] = {0}; if (sscanf(line, "%*s %199s", path) == 1) { void gpu_native_shot(Core*, const char*); gpu_native_shot(c, path); } }
    else if (!strcmp(cmd, "dumpram")) {
      char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) {
        FILE* fp = fopen(path, "wb");
        if (fp) { fwrite(c->ram, 1, 0x200000, fp); fclose(fp); fprintf(stderr, "[repl] dumpram -> %s\n", path); }
        else fprintf(stderr, "[repl] dumpram: cannot open %s\n", path);
      }
    }
    else if (!strcmp(cmd, "wav")) { char path[200] = {0}; if (sscanf(line, "%*s %199s", path) == 1) spu_wav_reopen(path); }
    else if (!strcmp(cmd, "bgm") && sscanf(line, "%*s %u", &a) == 1) { rc1(c, 0x80074BF8u, a); fprintf(stderr, "[repl] bgm %u (song@800bed80=%04X)\n", a, c->mem_r16(0x800bed80)); }
    else if (!strcmp(cmd, "bgmstop")) { rc0(c, 0x80074E48u); fprintf(stderr, "[repl] bgmstop\n"); }
    else if (!strcmp(cmd, "xadump")) { unsigned ch = 0, lba = 0, secs = 3; char path[200] = {0};
      if (sscanf(line, "%*s %u %u %199s %u", &ch, &lba, path, &secs) >= 3) repl_xadump((uint8_t)ch, lba, path, secs ? (int)secs : 3); }
    else if (!strcmp(cmd, "stage")) fprintf(stderr, "[repl] stage=%08X sm48=%d\n", c->mem_r32(0x801fe00c), (int)c->mem_r16(0x801fe048));
    else if (!strcmp(cmd, "regs")) { for (int i = 0; i < 32; i++) { fprintf(stderr, " r%-2d=%08X", i, c->r[i]); if ((i & 3) == 3) fprintf(stderr, "\n"); } fprintf(stderr, " hi=%08X lo=%08X\n", c->hi, c->lo); }
    else if (!strcmp(cmd, "seq")) fprintf(stderr, "[repl] seq open=%d playmask=%04X tickmode=%d seqfn=%08X stage=%08X\n",
                                          (int16_t)c->mem_r16(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF, c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(0x801fe00c));
    else fprintf(stderr, "[repl] ? %s\n", cmd);
    fflush(stderr);
  }
  return -1;  // EOF
}

static void ov_game_main(Core* c) {
  fprintf(stderr, "[native_boot] FUN_80050b08 override: running init prefix\n");

  // --- init prefix, transcribed from FUN_80050b08 (no scheduler loop) ---
  rc0(c, 0x80089788);
  rc0(c, 0x80085b20);
  rc0(c, 0x800898a0);
  rc1(c, 0x80080bf0, 3);
  rc1(c, 0x80080d64, 0);
  rc1(c, 0x80080ed4, 1);
  rc1(c, 0x800865f0, 0);
  // Engine frame-state + camera init reimplemented PC-native (engine/engine_init.cpp), replacing the
  // 1:1 rec_dispatch transcription. FUN_800509b4 (display/GTE projection + PSX draw/disp double-buffer
  // env) stays dispatched for now — it sets DAT_801003f8 = H that eng_init_camera reads, and entangles
  // the PSX-GPU env, so it is the next target. (later-159, top-down engine port from main.)
  void eng_init_framestate(Core*), eng_init_display(Core*), eng_init_camera(Core*);
  eng_init_framestate(c);      // was rc0(c, 0x80050a0c)
  eng_init_display(c);         // was rc0(c, 0x800509b4) — GTE projection + display (sets H=DAT_801003f8)
  eng_init_camera(c);          // was rc0(c, 0x80050a80)
  rc0(c, 0x80096a70);
  rc1(c, 0x80099310, 0x1010);
  rc1(c, 0x800991b0, 0x20000);
  rc1(c, 0x800993a0, 1);
  // FUN_80089bac(0xe, &local_28, 0) with local_28[0] = 0x80 (a stack byte buffer).
  uint32_t buf = c->r[29] - 0x40;
  c->mem_w8(buf, 0x80);
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
  c->mem_w32(0x1f800138, 0x801fe000);
  rc0(c, 0x800499e8);
  // START.BIN loaded raw to 0x80106228: [0]=manifest count (6); entry word @0x8010649c.
  fprintf(stderr, "[native_boot] after FUN_800499e8: START.BIN count@0x80106228=%u "
                  "entry-word@0x8010649c=0x%08X (expect 0x27BDFE38); task0 state=%u entry=0x%08X\n",
          c->mem_r32(0x80106228), c->mem_r32(0x8010649c), c->mem_r16(0x801fe000), c->mem_r32(0x801fe00c));
  // --- native frame loop (replaces LAB_80050c6c). Per frame, faithful to the game-main loop
  // body but with the scheduler call FUN_80051e60 replaced by native stage stepping (added
  // incrementally). FUN_800788ac is overridden (ov_frame_update): real per-frame update +
  // gpu_present + audio + satisfies the vblank pacing dwell. PSXPORT_NATIVE_FRAMES caps the
  // run (headless). ---
  rec_set_override(0x80080880u, ov_switch);    // ChangeThread -> native task switch (capture+longjmp)
  // Always on: ov_bgm_start cuts the looping ingame music when a dialog tone starts (instant-CD mod);
  // both also trace BGM start/stop callers under PSXPORT_BGMDBG.
  rec_set_override(0x80074BF8u, ov_bgm_start);
  rec_set_override(0x80074E48u, ov_bgm_stop);

  // Frame budget: an explicit PSXPORT_NATIVE_FRAMES always wins (headless tests). Otherwise, when
  // a window is up this is the real interactive game loop — run until the user closes the window
  // (SDL_QUIT -> exit(0) in present_window); headless with no cap defaults to 120 (CI/smoke).
  uint32_t nframes = 0;   // 0 == run until window close / REPL quit
  const char* nf = cfg_str("PSXPORT_NATIVE_FRAMES");
  int repl_mode = cfg_on("PSXPORT_REPL") != 0;
  if (repl_mode) nframes = 0;                       // REPL drives frame count via `run N`
  else if (nf) nframes = (uint32_t)strtoul(nf, 0, 0);
  else if (!cfg_str("PSXPORT_GPU_WINDOW")) nframes = 120;
  fprintf(stderr, "[native_boot] entering native frame loop (%s)\n",
          nframes ? "capped" : "interactive (until window close)");
  void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
  void pad_service_frame(Core*);
  void dbg_server_start(Core* c); void dbg_server_service(Core* c);
  dbg_server_start(c);    // PSXPORT_DEBUG_SERVER: non-blocking live TCP debug server (dbg_server.c)
  long repl_budget = 0;   // frames remaining in the current REPL `run N`
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    g_bgm_frame = f;
    // REPL: when the run-budget is exhausted, block reading stdin commands until a `run N` refills
    // it (immediate commands — r/w/watch/input/regs/seq — execute between frames). Quit/EOF breaks.
    if (repl_mode) {
      while (repl_budget <= 0) { repl_budget = native_repl_read(c, f); if (repl_budget < 0) break; }
      if (repl_budget < 0) break;
      repl_budget--;
    }
    // TRANSPLANT harness (PSXPORT_TRANSPLANT="frame:ramfile:vramfile"): at logic frame `frame`,
    // overwrite our RAM (and VRAM if given) with the oracle's CLEAN green-field dump, then let the
    // frame loop CONTINUE. Tests the accumulation hypothesis directly: if our per-frame logic is
    // correct the next frames stay clean (match the oracle); if buggy they re-corrupt from a known
    // -good start, so we can watch the corruption begin instead of it being masked. RAM is dropped
    // at the TOP of the iteration (before FUN_800788AC), so our update runs on the oracle's state.
    { static int tf = -2; static char rf[256], vf[256];
      if (tf == -2) { tf = -1; rf[0] = vf[0] = 0; const char* e = cfg_str("PSXPORT_TRANSPLANT");
        if (e) { char buf[600]; snprintf(buf, sizeof buf, "%s", e);
          char* c1 = strchr(buf, ':'); if (c1) { *c1 = 0; tf = atoi(buf);
            char* c2 = strchr(c1 + 1, ':'); if (c2) { *c2 = 0; snprintf(vf, sizeof vf, "%s", c2 + 1); }
            snprintf(rf, sizeof rf, "%s", c1 + 1); } } }
      if (tf >= 0 && (int)f == tf) {
        int gpu_native_load_vram(Core*, const char*);
        FILE* mf = fopen(rf, "rb");
        if (mf) { size_t n = fread(c->ram, 1, 0x200000, mf); fclose(mf);
                  fprintf(stderr, "[transplant] loaded RAM %zu B from %s at lf%u\n", n, rf, f); }
        else fprintf(stderr, "[transplant] FAILED to open %s\n", rf);
        if (vf[0]) gpu_native_load_vram(c, vf);
      } }
    // PSXPORT_AUTO_NEWGAME: own the title->New Game navigation so we boot straight into the post-New
    // Game prologue cutscene (stage 0x8010637C) deterministically — no fighting the attract loop. The
    // menu confirm is Cross (0x4000); pulse it at the title (DEMO overlay) until task0 enters GAME.
    { static int ang = -1; if (ang < 0) ang = cfg_str("PSXPORT_AUTO_NEWGAME") ? 1 : 0;
      if (ang == 1) {
        if (c->mem_r32(0x801fe00c) != 0x8010637Cu) {
          if ((f % 12u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x4000), 6);   // tap Cross
        } else { void dbg_set_paused(int); fprintf(stderr, "[autonewgame] reached GAME (prologue) at frame %u — auto-paused\n", f);
                 ang = 2; if (cfg_str("PSXPORT_AUTO_NEWGAME") && atoi(cfg_str("PSXPORT_AUTO_NEWGAME")) >= 2) dbg_set_paused(1); }
      } }
    // PSXPORT_AUTO_GAMEPLAY=1: state-gated auto-navigator that OWNS the title->newgame->cutscene->field
    // path and then goes HANDS-OFF. The post-newgame fisherman dialog cutscene advances only on Start
    // EDGES (not Cross) and otherwise hard-stalls headless; once in the field, continued Start would
    // open the pause menu (which stops the BGM). So: pulse Start until the chan4 area music has been
    // looping continuously (= field reached, cutscene gate cleared), then RELEASE input. =2 also pauses.
    { static int gnav = -1, sustain = 0; static uint32_t field_f = 0;
      if (gnav < 0) gnav = cfg_str("PSXPORT_AUTO_GAMEPLAY") ? 1 : 0;
      if (gnav == 1) {
        void pad_repl_release(Core*);
        if (xa_stream_is_looping()) sustain++; else sustain = 0;   // resets whenever dialog stops chan4
        if (sustain >= 150) {                                      // ~5 s of uninterrupted area music = field
          pad_repl_release(c);
          fprintf(stderr, "[autogameplay] field reached (chan4 looping %d frames) at frame %u — input released\n", sustain, f);
          gnav = 2; field_f = f;
          if (atoi(cfg_str("PSXPORT_AUTO_GAMEPLAY")) >= 2) { void dbg_set_paused(int); dbg_set_paused(1); }
        } else if ((f % 24u) == 0) {
          pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x0008), 6);        // pulse Start (0x0008): title + dialog advance
        }
      }
      // PSXPORT_AUTO_SKIP=N: after field-reach, KEEP pulsing Start until native frame N to advance/skip
      // the post-arrival fisherman dialog CUTSCENE (the field "reached" = area music looping, but the
      // cutscene is still up and only ends on Start edges). Reaches actual Tomba gameplay on the cliff.
      if (gnav == 2) { const char* sk = cfg_str("PSXPORT_AUTO_SKIP");
        if (sk && f < (uint32_t)atoi(sk) && (f % 24u) == 0) {
          void pad_repl_tap(Core*, uint16_t, int); pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x0008), 6); } }
      // PSXPORT_AUTO_WALK=<script>: once the field is reached, drive a deterministic input SCRIPT so the
      // character walks/acts — a MOTION scene for fps60 interp AND for navigating to an area exit. Two forms:
      //   single button "r" / "l" / "u" / "d" / "x"(Cross/jump) / "o"(Circle) / "t"(Triangle) / "s"(Square)
      //     → HOLD it forever.  Multiple buttons in one phase combine: "rx" = right+jump.
      //   phase list "r:300,u:150,rx:120" → hold each phase for N frames, in order, then release.
      // Phases are counted from when the walk starts (= max(field-reached frame, AUTO_SKIP frame), so the
      // Start-pulsing skip finishes first). Active-low pad mask (a pressed button CLEARS its bit).
      if (gnav == 2) { const char* w = cfg_str("PSXPORT_AUTO_WALK");
        if (w) {
          const char* sk = cfg_str("PSXPORT_AUTO_SKIP");
          uint32_t walk0 = field_f; if (sk && (uint32_t)atoi(sk) > walk0) walk0 = (uint32_t)atoi(sk);
          uint32_t el = (f > walk0) ? f - walk0 : 0;
          auto btn_bit = [](char ch) -> uint16_t {
            switch (ch) { case 'l': return 0x0080; case 'u': return 0x0010; case 'd': return 0x0040;
              case 'x': return 0x4000; case 'o': return 0x2000; case 't': return 0x1000; case 's': return 0x8000;
              default:  return 0x0020; } };                          // 'r' / default = right
          // find the active phase by walking the comma list, accumulating durations.
          const char* p = w; uint32_t acc = 0; uint16_t held = 0; int done = 0;
          while (*p) {
            uint16_t phase = 0;
            while (*p && *p != ':' && *p != ',') phase |= btn_bit(*p++);
            uint32_t dur = 0;
            if (*p == ':') { p++; while (*p >= '0' && *p <= '9') dur = dur*10 + (uint32_t)(*p++ - '0'); }
            if (*p == ',') p++;
            if (dur == 0) { held = phase; done = 1; break; }         // no duration → hold this phase forever
            if (el < acc + dur) { held = phase; done = 1; break; }   // we're inside this phase
            acc += dur;
          }
          if (done) pad_repl_hold(c, (uint16_t)(0xFFFF & ~held));    // else script finished → leave input released
        } } }
    // PSXPORT_DEBUG_SERVER pause/step: when frozen, do NOT advance the game — just pump host input
    // (keeps the window alive) and service debug commands so `step`/`play` can arrive. A `step` runs
    // exactly one real frame then re-freezes, so transient bad frames can be inspected one at a time.
    { int dbg_is_paused(void), dbg_step_pending(void); void dbg_consume_step(void); void gpu_repaint(Core*);
      while (dbg_is_paused()) {
        if (dbg_step_pending()) { dbg_consume_step(); break; }   // run exactly one frame
        pad_service_frame(c);      // pump host input (keeps the window responsive)
        gpu_repaint(c);           // re-present current frame: window stays live + readback is accurate
        dbg_server_service(c);    // receive step/play/capture commands
        usleep(15000);
      } }
    native_step_frame(c, f);   // one frame of deterministic guest work (steppable core; see fn above)
    // PSXPORT_RAMHASH=1 — per-frame FNV-1a of guest RAM EXCLUDING the render pool [0xBE000,0xEC000)
    // (which legitimately differs native-vs-recomp: the recomp terrain submit writes packets there, the
    // native one writes host memory). Run twice — native terrain vs PSXPORT_NO_TERRAIN=1 (recomp) — and
    // diff the [ramhash] logs to find the FIRST frame the GAMEPLAY state diverges (the submit_terrain
    // leak out of the render pool). Then PSXPORT_RAMDUMP_FRAME=<that frame> both ways + cmp -l the dumps
    // for the byte addrs (~0x800FDxxx). The port is deterministic, so two sequential runs are comparable.
    { static int rh = -1; if (rh < 0) rh = cfg_on("PSXPORT_RAMHASH") ? 1 : 0;
      if (rh) { uint64_t h = 1469598103934665603ull;
        for (uint32_t o = 0; o < 0x200000u; o++) {
          if (o >= 0xBE000u && o < 0xEC000u) continue;            // skip the render pool
          h ^= c->ram[o]; h *= 1099511628211ull; }
        fprintf(stderr, "[ramhash] f%u %016llx\n", f, (unsigned long long)h); } }
    // PSXPORT_SEQDBG — libsnd sequencer STATE trace (from SsSeqCalled @0x80090BD0): is any BGM
    // sequence OPEN/PLAYING? 0x801054B0=open-seq count, 0x80104C28=playing bitmask, 0x800AC424=tick
    // mode, 0x800AC42C=SsSeqCalled ptr. If these never go nonzero, no song is ever started → the
    // missing-BGM root cause is upstream (song open/play not happening), not the SPU/tick.
    if (cfg_dbg("seq")) {
      static uint32_t ls = 0xFFFFFFFF;
      uint32_t st = (c->mem_r16(0x801054B0) << 16) | (c->mem_r32(0x80104C28) & 0xFFFF);
      if (st != ls) {
        fprintf(stderr, "[seqdbg] f%u open=%d playmask=0x%04X tickmode=%d seqfn=0x%08X stage=0x%08X\n",
                f, (int16_t)c->mem_r16(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF,
                c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(TASKBASE + 0xc));
        ls = st;
      }
    }
    // PSXPORT_DEBUG=state — RELIABLE game-state probe (replaces the old `nav` camera guess, which
    // disagreed with the screen). Dumps all 3 cooperative-task slots (state@+0x00, entry@+0x0c) so a
    // PAUSE MENU (a separate task spawned in the GAME overlay, entry 0x80108xxx, page byte that task+0x6B)
    // is DETECTABLE: when one of the 3 slots has a 0x80108xxx entry and is alive, a menu is open and
    // gameplay input is going to the menu, NOT the player. Also prints the GAME stage sm + camera pos.
    // Logged only on change of the (slot-state/entry + page) signature to stay quiet.
    if (cfg_dbg("state")) {
      uint64_t sig = 0; int menu_slot = -1; uint8_t menu_page = 0;
      for (int i = 0; i < 3; i++) {
        uint32_t base = 0x801fe000u + (uint32_t)i * 0x70u;
        uint16_t st = c->mem_r16(base);
        uint32_t ent = c->mem_r32(base + 0xc);
        sig = sig * 1099511628211ull + ((uint64_t)st << 32 | ent);
        if (st && (ent & 0xFFFFF000u) == 0x80108000u) { menu_slot = i; menu_page = c->mem_r8(base + 0x6bu); }
      }
      sig = sig * 31 + ((uint64_t)menu_slot << 8 | menu_page);
      static uint64_t last_sig = 0;
      if (sig != last_sig) {
        last_sig = sig;
        fprintf(stderr, "[state] f%u", f);
        for (int i = 0; i < 3; i++) {
          uint32_t base = 0x801fe000u + (uint32_t)i * 0x70u;
          fprintf(stderr, " | s%d st=%u ent=0x%08X", i, c->mem_r16(base), c->mem_r32(base + 0xc));
        }
        fprintf(stderr, "  MENU=%s", menu_slot >= 0 ? "OPEN" : "no");
        if (menu_slot >= 0) fprintf(stderr, "(slot%d page=%u)", menu_slot, menu_page);
        fprintf(stderr, "  cam=(%d,%d,%d) sm[0x4a]=%u\n",
                (int16_t)c->mem_r16(0x1f8000d2u), (int16_t)c->mem_r16(0x1f8000d6u),
                (int16_t)c->mem_r16(0x1f8000dau), c->mem_r16(0x801fe000u + 0x4au));
      }
    }
    // BGM-active probe (PSXPORT_BGMDBG): each frame scan the 14 libsnd sequence slots
    // (0x800be3d8 + i*0xB0) for the active/play flag (+0x98 bit0). For any active slot, log
    // its read pointer (+0x00) vs base (+0x04) — if the read ptr ADVANCES frame-to-frame the
    // sequence is genuinely ticking (audible); if it stays == base the SsSeqCalled tick isn't
    // advancing it (frozen, the handoff's hypothesis). Scene-independent: catches any window
    // where a BGM is active, without needing to reach a specific scene.
    if (cfg_str("PSXPORT_BGMDBG")) {
      static uint32_t s_rd[14];
      for (int i = 0; i < 14; i++) {
        uint32_t s = 0x800be3d8u + (uint32_t)i * 0xB0u;
        uint32_t flag = c->mem_r32(s + 0x98), rd = c->mem_r32(s);
        if ((flag & 1) && rd != s_rd[i]) {
          fprintf(stderr, "[bgmtick] f%u slot%d active rdptr=%08X base=%08X (%+d)\n",
                  f, i, rd, c->mem_r32(s + 4), (int)(rd - c->mem_r32(s + 4)));
          s_rd[i] = rd;
        }
        if (!(flag & 1)) s_rd[i] = 0;
      }
    }
    static uint32_t s_last_entry = 0; static uint32_t s_last_sm = 0xFFFFFFFF;
    uint32_t t0e = c->mem_r32(TASKBASE + 0xc), s48 = c->mem_r16(TASKBASE + 0x48);
    // GAME runs a 4-level nested state machine (task +0x48/4a/4c/4e). Track all of it so a
    // stuck leaf is visible, not just the outer s48.
    uint32_t sm = (c->mem_r16(TASKBASE+0x48)<<24)|(c->mem_r16(TASKBASE+0x4a)<<16)|
                  (c->mem_r16(TASKBASE+0x4c)<<8)|c->mem_r16(TASKBASE+0x4e)
                  ^ (c->mem_r16(TASKBASE+0x50)<<12)^(c->mem_r16(TASKBASE+0x52)<<4);
    if (t0e != s_last_entry || sm != s_last_sm) {
      const char* stg = t0e == 0x8010649Cu ? "START" : t0e == 0x801062E4u ? "DEMO" :
                        t0e == 0x8010637Cu ? "GAME" : "?";
      fprintf(stderr, "[native_boot]   frame %u: stage=%s(0x%08X) sm[48=%u 4a=%u 4c=%u 4e=%u 50=%u 52=%u]"
              " @0x80109450=%08X\n",
              f, stg, t0e, c->mem_r16(TASKBASE+0x48), c->mem_r16(TASKBASE+0x4a),
              c->mem_r16(TASKBASE+0x4c), c->mem_r16(TASKBASE+0x4e), c->mem_r16(TASKBASE+0x50),
              c->mem_r16(TASKBASE+0x52), c->mem_r32(0x80109450));
      s_last_entry = t0e; s_last_sm = sm;
    }
    // One-shot: when GAME has settled, dump the CD-streaming contract (FUN_8001cfc8, task
    // slot 2). task2 obj @0x801fe0e0; +0x54=start LBA, +0x58=end LBA (= globals
    // DAT_801fe134/138). DAT_801fe146=channel/type. _DAT_1f8001f8=dest, _DAT_1f8001f4=words.
    if (cfg_dbg("stream") && t0e == 0x8010637Cu && f == 75) {
      fprintf(stderr, "[streamdbg] task2 obj @0x801fe0e0 state=%u entry=0x%08X\n",
              c->mem_r16(0x801fe0e0), c->mem_r32(0x801fe0ec));
      fprintf(stderr, "[streamdbg] startLBA(+54/801fe134)=%u endLBA(+58/801fe138)=%u "
              "chan(801fe146)=%u be0e4=0x%02X\n",
              c->mem_r32(0x801fe134), c->mem_r32(0x801fe138), c->mem_r8(0x801fe146), c->mem_r8(0x800be0e4));
      fprintf(stderr, "[streamdbg] dest(_DAT_1f8001f8)=0x%08X words(_DAT_1f8001f4)=%u "
              "f0=%u f1f800224=0x%08X\n",
              c->mem_r32(0x1f8001f8), c->mem_r32(0x1f8001f4), c->mem_r32(0x1f8001f0), c->mem_r32(0x1f800224));
    }
    // PSXPORT_RAMDUMP_FRAME=N — dump RAM mid-run at native frame N (overlay state during gameplay
    // differs from end-of-run; needed to disasm the LIVE level/stage overlay at 0x8010/0x8011xxxx).
    { const char* rdf = cfg_str("PSXPORT_RAMDUMP_FRAME");
      if (rdf && f == (uint32_t)strtoul(rdf, 0, 0)) {
        
        const char* rd = cfg_str("PSXPORT_RAMDUMP"); if (!rd) rd = "scratch/bin/midrun_ram.bin";
        FILE* mf = fopen(rd, "wb");
        if (mf) { fwrite(c->ram, 1, 0x200000, mf); fclose(mf);
                  fprintf(stderr, "[native_boot] mid-run RAM dump @frame %u -> %s\n", f, rd); }
      } }
    if (cfg_dbg("schedf"))
      fprintf(stderr, "[schedf] f%u t0[st=%u e=%08X s48=%u s4a=%u s4c=%u s5c=%u] t1[st=%u] t2[st=%u]\n",
              f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48),
              c->mem_r16(TASKBASE + 0x4a), c->mem_r16(TASKBASE + 0x4c), c->mem_r16(TASKBASE + 0x5c),
              c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0));
    else if (f < 10 || (f % 30) == 0)
      fprintf(stderr, "[native_boot]   frame %u: t0[st=%u e=0x%08X s48=%u] t1[st=%u] t2[st=%u] "
                      "f135=%u\n", f, c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc),
              c->mem_r16(TASKBASE + 0x48), c->mem_r16(TASKBASE + 0x70), c->mem_r16(TASKBASE + 0xe0),
              c->mem_r8(0x1f800135));
    dbg_server_service(c);  // service one queued live-debug-server command (non-blocking)
  }
  fprintf(stderr, "[native_boot] frame loop done; task0 state=%u entry=0x%08X obj+0x48=%u\n",
          c->mem_r16(TASKBASE), c->mem_r32(TASKBASE + 0xc), c->mem_r16(TASKBASE + 0x48));
  const char* rd = cfg_str("PSXPORT_RAMDUMP");
  if (rd) {
    
    FILE* f = fopen(rd, "wb");
    if (f) { fwrite(c->ram, 1, 0x200000, f); fclose(f);
             fprintf(stderr, "[native_boot] dumped 2MB RAM -> %s\n", rd); }
  }
}

// Wired from boot.c when PSXPORT_NATIVE_BOOT is set. Registers the main override and enters
// crt0; crt0's call to FUN_80050b08 lands in ov_game_main.
void native_boot_run(Core* c) {
  { void cfg_dump(void); cfg_dump(); }   // log active PSXPORT_* config once (see docs/config.md)
  // Intro FMVs: the real boot is SCEA (stub) -> Whoopee logo -> opening movie -> title/menu. The
  // game's own STR streaming (strNext) TIMES OUT under our runtime (we don't feed CD-streamed FMV
  // sectors to its StrPlayer — see "time out in strNext()" in the DEMO stage), so the movies are
  // skipped to a black gap. Play them here with our self-contained native FMV player (native_fmv.c)
  // before booting MAIN, restoring SCEA->Woopee->OP->menu. PSXPORT_NO_FMV skips them (headless
  // gameplay tests that need to reach GAME fast / with stable frame numbers).
  int native_fmv_play(Core*, const char*);
  // Skip the intro FMVs when there's no viewer: PSXPORT_NO_FMV, OR any headless run (a headless probe
  // has nobody watching — playing/decoding the intro movies just burns wall-clock; a field probe went
  // from ~77s to ~1.4s). The in-game/cutscene FMVs that still play are also auto-uncapped in headless
  // (native_fmv.c) so they fast-forward. Set PSXPORT_NO_FMV=0 explicitly to force them on if ever needed.
  int skip_fmv = cfg_on("PSXPORT_NO_FMV") || cfg_on("PSXPORT_VK_HEADLESS");
  const char* nf_ov = cfg_str("PSXPORT_NO_FMV");
  if (nf_ov && atoi(nf_ov) == 0 && *nf_ov) skip_fmv = 0;     // explicit PSXPORT_NO_FMV=0 forces FMVs on
  if (!skip_fmv) {
    fprintf(stderr, "[native_boot] playing intro FMVs (Whoopee logo, opening)\n");
    native_fmv_play(c, "MOVIE/LOGO.STR");
    native_fmv_play(c, "MOVIE/OP.STR");
  } else {
    fprintf(stderr, "[native_boot] skipping intro FMVs (headless/NO_FMV)\n");
  }
  rec_set_override(0x80050b08u, ov_game_main);
  fprintf(stderr, "[native_boot] entering crt0 0x800896E0 (interpreted)\n");
  rec_dispatch(c, 0x800896E0u);
  fprintf(stderr, "[native_boot] returned from crt0\n");
}
