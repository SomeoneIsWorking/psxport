// class Sbs — LIVE side-by-side two-core divergence debugger (PSXPORT_SBS=1). See sbs.h for the public
// API surface. This file is the whole harness — no file-scope free functions, no anonymous-namespace
// statics; everything is a `class Sbs` static field + static method (there is only ever ONE harness per
// process, so the "singleton" is expressed as class-scope statics rather than an instance pointer).
//
// Runs TWO native-boot cores in ONE process, in lockstep, with IDENTICAL input, differing ONLY by MODE:
//   render   (default): A = native gameplay + NATIVE render,  B = native gameplay + PSX render
//   gameplay:           A = native gameplay,  B = PSX gameplay (psx_fallback); render IDENTICAL (PSX) on both
//   full:               A = full native (native gp + native render),  B = full PSX (PSX gp + PSX render)
//   oracle:             A = full native,  B = PURE ORACLE — the interpreter engine (use_interp) + the
//                       software rasterizer (soft_gpu), NOT the recomp substrate (psx_fallback alone). B's
//                       render|gameplay|full psx_fallback pane shares A's native rasterizer so it can't
//                       isolate a native-render-only bug (docs/oracle.md).
// Select with PSXPORT_SBS_MODE=render|gameplay|full|oracle.
// Legacy alias: `both` is still accepted as a synonym of `full` (renamed 2026-07-03 — "both" implied
// "both cores use PSX", but the mode is actually A-full-native vs B-full-PSX).
//
// Concurrent from boot: both cores step ONE frame each per lockstep iteration and present as two panes
// every frame. Safe because every per-machine subsystem is per-instance (GTE/SPU/MDEC bind, CD-controller
// registers, XA streamer, native depth-cache); the read-only CHD/disc image stays shared by design.
//
// Diff/inspection over the debug server (PSXPORT_DEBUG_SERVER, tools/dbgclient.py):
//   sbs                 status: mode, frame, selected core, divergence summary, watch state
//   sbs diff            first diverging addr/range, A bytes vs B bytes
//   sbs bt              guest stack backtrace of BOTH cores at the divergence (frame-boundary)
//   sbs watch           arm a write-watchpoint on the diverging address; the WRITE pauses mid-frame with
//                       the EXACT guest backtrace of each writing core
//   sbs show a|b        which core r/rw/ents/node/scene/etc target
//   sbs resume          unpause;   sbs step [n]   advance n lockstep frames then re-pause
//   sbs dump [path]     write side-by-side PPM composite of the two current panes
//   sbs ramdiff [N]     ON-DEMAND full-region diff (cap N spans, default 24)
//
// Diagnostic, not behavior (one PC-native game ships; this is a debugger). Owns its own Game instances
// and never returns (the process exits when the window closes).

#include "sbs.h"
#include "game.h"
#include "cfg.h"
#include "render/render.h"    // Render::setPsxRender (per-Core render-path switch)
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <tuple>
#include <map>
#include <execinfo.h>
#include <set>
#include <algorithm>
#include <csignal>
#include <string>

// --- runtime entry points reused from the normal boot path / dual-core harness ---
void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
extern "C" int cfg_on(const char*);
extern "C" void watchdog_disable(void);
extern "C" void guest_backtrace_to(Core* c, FILE* out);
void gpu_gpu_render_readback(Core* core, const uint16_t* vram, int sx, int sy, int w, int h, uint8_t* rgba);
void gpu_gpu_select_target(int t);
void gpu_gpu_frame_end(Core* core, const uint16_t* svram, int frame);
void gpu_present_finalize(Core* core);   // per-frame reset/bookkeeping standalone does in gpu_present (SBS skips present)
const uint16_t* gpu_vram_ptr(Core* core);
void gpu_disp_region(Core* core, int* sx, int* sy, int* w, int* h);

// SBS presenter (sbs_present_sdl.cpp): draws the two CPU panes + polls the window keyboard/quit.
extern "C" {
  void sbs_rl_init(void);
  int  sbs_rl_should_close(void);
  unsigned short sbs_rl_poll_input(void);
  void sbs_rl_shutdown(void);
}
void sbs_rl_present(Game* game, const unsigned char* rgbaA, int wA, int hA, const unsigned char* rgbaB, int wB, int hB);

// ============================================================================
// Sbs::Impl — the pimpl body of class Sbs. All state + dispatch lives here; Sbs's public methods
// forward through mImpl. Exactly one instance per process (stack-allocated inside Sbs::run() and
// destroyed on process exit).
// ============================================================================

enum Mode { M_RENDER, M_GAMEPLAY, M_FULL, M_ORACLE, M_SKIP };
constexpr uint32_t GAME_ENTRY  = 0x8010637Cu;  // task0 entry while the GAME stage runs (in the field)
constexpr uint32_t TASK0_ENTRY = 0x801fe00cu;  // task0 obj +0xc = current stage entry
constexpr uint32_t CUT_FLAG    = 0x1F800137u;  // cutscene-active byte (1 = intro cutscene, 0 = free-roam)
// BTN_RIGHT FIX (2026-07-10): was 0x2000 — that bit is CIRCLE, not Right (see the pad-button table in
// docs/driving-the-game.md: Right=0x0020, Circle=0x2000). This mislabeling meant PSXPORT_SBS_POSTDRIVE=1's
// "walk into things" script (Nav::DONE below) pressed Circle the whole time, never actually walking —
// found while building the PSXPORT_SBS_AUTONAV=combat leg (docs/findings/ai.md), which used the same
// constant and went nowhere until this was traced back. Fixing the constant fixes BOTH scripts.
constexpr uint16_t BTN_CROSS = 0x4000, BTN_START = 0x0008, BTN_RIGHT = 0x0020, BTN_NONE = 0xFFFF;

enum Phase { REACH_GAME, AWAIT_CUT, SKIP_CUT, AWAIT_CONTROL, DONE };
struct Nav { Phase phase = REACH_GAME; int idle = 0; int postFrame = 0; };

// PSXPORT_SBS_POSTDRIVE=1 — keep exercising REAL interactive input (walk/jump) once player control
// is reached (see Nav::DONE below), instead of handing off to feedInput()'s host-keyboard poll. One
// evaluation, shared between navStep's DONE case and Sbs::Impl::run()'s per-frame dispatch (below).
static bool sbsPostdriveOn() {
  static const int on = []{ const char* e = getenv("PSXPORT_SBS_POSTDRIVE"); return e && *e && e[0] != '0' ? 1 : 0; }();
  return on != 0;
}

// PSXPORT_SBS_AUTONAV=combat — closes the "combat-cluster overrides are RE-verified only, never
// SBS-exercised" coverage gap (docs/fleet-workflow.md §9, docs/findings/ai.md). Standard autonav
// (plain `=1`) never leaves the immediate spawn area, so ActorMeleeEngage (0x80112188),
// MeleeProximity (0x8001F9DC), and beh_actor_tomba_proximity_combat (0x800527C8) sat at "verified by
// RE only" — `ovhit` reads 0 for all three under the standard gate command. This leg is a
// DETERMINISTIC input script (same requirement as the rest of Nav — SBS runs two cores in lockstep
// off identical input, so no wall-clock/RNG-driven navigation) empirically found 2026-07-10 via a
// live single-core REPL/debug-server session (`tools/dbgclient.py`, `PSXPORT_DEBUG=dispatch`): hold
// Right from the seaside spawn (~Z=3940) until it collides with the first `ActorZonedAttacker`
// encounter (`id_compare_motion_dispatch`, node handler 0x80145230, physical wall at Z≈6190), fire
// ONE Cross-jump edge to clear that obstacle (lands at Z≈7922), then keep holding Right. From there
// `ActorMeleeEngage::doIt` (0x80112188) fires EVERY FRAME for the nearby `cull_substate_orchestrator`
// object (0x800F1008, handler 0x8013259C) even though a second, taller ledge (~399 Y units) still
// blocks physically reaching that object — confirmed via `PSXPORT_DEBUG=dispatch` showing continuous
// `[dispatch] ... 0x80112188 ActorMeleeEngage::doIt` hits while stuck at the ledge.
// STILL A REAL GAP (not fixed by this leg): MeleeProximity (0x8001F9DC, called only from two
// currently-unowned engine leaves 0x80020868/0x80023618) and beh_actor_tomba_proximity_combat
// (0x800527C8, reached only via an indirect per-object "think" pointer) require an object whose
// think-slot/call-site is actually LIVE — no such object is spawned anywhere in the ~150-entity
// reachable seaside/intro area in this playthrough (confirmed by a full `ents` walk). Reaching them
// needs either solving the second ledge (deeper platforming/grow-shrink RE) or a wider-area
// playthrough with a real melee encounter — tracked OPEN in docs/findings/ai.md, not solved here.
// A red gate from this leg is real coverage working as intended (CLAUDE.md "no residual diverges") —
// this knob stays OFF by default so the standard gate command (`docs/fleet-workflow.md` §2, no
// `=combat`) is unaffected; it must be opted into explicitly.
static bool sbsCombatOn() {
  static const int on = []{ const char* e = getenv("PSXPORT_SBS_AUTONAV");
    return (e && !strcmp(e, "combat")) ? 1 : 0; }();
  return on != 0;
}

// FIELD-RUNNING sub-machine offsets off task0 (0x801fe000) — see game/core/engine.cpp
// Engine::fieldRunFaithful / Engine::fieldRun (the 12-way switch on sm[0x4e], RE'd from
// 0x80106b98). sm[0x4e]==9 ("L_80106FC4 — field frame + gate on pad bit 3", engine.cpp:1095-1104)
// is the scripted "caught on the fishing-line" HOLD that the cutscene-active flag (CUT_FLAG,
// above) does NOT cover: CUT_FLAG belongs to the separate SOP intro-narration machine
// (game/scene/sop.cpp Sop::fieldMode, sm[0x50]) and clears once THAT machine's state 4 (RESET)
// runs — at which point fieldRun's OWN sub-machine has already been steered into s4e=9 by
// fieldRun's case 0 (engine.cpp:954-957: "if (0x800BF89C==2) sm[0x4e]=9"). s4e==9 only advances
// on a Cross-edge (0x800E7E68 & 8, engine.cpp:1098) while the scripted-camera gate 0x800BF89C==2
// is still set — exactly the state docs/findings/sbs.md "oraclediff: interactive-play SCAN
// added" (later-283) found: CUT_FLAG==0 (our old DONE trigger) reaches this frozen pose, NOT
// real player control. s4e==1 ("L_80106D00 — the RUNNING field frame", engine.cpp:994-1032) is
// the genuine free-roam per-frame state — it reads the scene trigger byte 0x800BF839 for
// interactive movement/menu/area-exit dispatch. VERIFIED (this session, headless single-core
// REPL `newgame`+`skip`+manual Cross taps + `ents`/`node`): Tomba's node position (obj+0x2e/32/36)
// is flat while s4e==9 under held Right, and starts changing once s4e settles at 1 under the
// same held Right — see docs/findings/sbs.md "AUTONAV: reaching real player control (s4e==9 -> 1)".
constexpr uint32_t TASK0_BASE  = 0x801fe000u;
constexpr uint32_t SM_S4A      = TASK0_BASE + 0x4au;   // running sub-mode (0=intro-cutscene machine, 1=field)
constexpr uint32_t SM_S4E      = TASK0_BASE + 0x4eu;   // fieldRun's own running state (1 = steady playable frame, 9 = scripted caught hold)
constexpr uint32_t FISH_GATE   = 0x800BF89Cu;          // scripted-camera/cull gate; ==2 while s4e==9's caught pose is armed
struct SbsKey { uint32_t from, to; uint16_t btn; };

// One-frame rewind snapshot of the PcScheduler fields the harness must roll back alongside
// guest RAM. Excludes jmp_buf yield_jmp (not trivially copyable — but only meaningful during a
// task run; snapshot is taken outside one) and Coro* coro[] (fiber C-stack can't be rolled back;
// rewind deletes fibers instead so the re-step re-spawns them).
struct SbsSchedSnap {
  R3000    task_ctx[3]{};
  int      in_stage = 0;
  int      cur_slot = 0;
  int      task_started[3]{};
  int      demo_native[3]{};
  int      game_native[3]{};
  int      game_coop[3]{};
  int      cur_is_coro = 0;
  uint8_t  stage0_step[3]{};
  uint8_t  sop_field_step[3]{};
  uint8_t  demo_leave_step[3]{};
  uint8_t  demo_s0_step[3]{};
  const RecOverlay* resident_ov[3]{};
};

// Pimpl body — all Sbs state and dispatch lives here. Accessed from Sbs's public methods (below)
// through `mImpl`; the header stays light.
class Sbs::Impl {
public:
  void run(const char* exePath, Sbs* facade);
  int  dbgCmd(FILE* out, const char* line);
  void dumpAllocRa(FILE* out);
  void dumpByteTrace(FILE* out);
  void storeCb(Core* c, uint32_t addr, uint32_t val, uint32_t width);
  bool active() const { return mSbs; }
  int  coreId(Core* c) const {
    if (!mA) return -1;
    return (mB && c == &mB->core) ? 1 : 0;
  }
  uint32_t frameNum() const { return mFrame; }
  Core* coreByLetter(char which) const {
    if (which == 'a' || which == 'A') return mA ? &mA->core : nullptr;
    if (which == 'b' || which == 'B') return mB ? &mB->core : nullptr;
    return nullptr;
  }
  Core* shownCore() const {
    return mSel ? (mB ? &mB->core : nullptr) : (mA ? &mA->core : nullptr);
  }

  // ---- mode + core handles ----
  Mode  mMode = M_RENDER;
  Game* mA = nullptr;
  Game* mB = nullptr;
  bool  mSbs = false;   // "harness running" flag (native_fmv/native_boot gate off Sbs::active())

  // ---- last-writer map (rewind-free write-site attribution) ----
  struct LastW { uint32_t pc = 0, ra = 0, sp = 0, frame = 0xFFFFFFFFu; };
  static constexpr uint32_t LW_RAM = 0x200000u, LW_SPAD = 0x400u, LW_N = LW_RAM + LW_SPAD;
  LastW* mLwA = nullptr;
  LastW* mLwB = nullptr;
  bool   mLwOn = false;
  static int lwIndex(uint32_t ka) {          // ka = kernelized store addr
    uint32_t off = ka & 0x1FFFFFFFu;
    if (off < LW_RAM) return (int)off;
    if (off >= 0x1F800000u && off < 0x1F800000u + LW_SPAD) return (int)(LW_RAM + (off - 0x1F800000u));
    return -1;
  }
  void lwReport(uint32_t addr, FILE* out = stderr) {   // print both cores' last writer for a divergent byte
    int idx = lwIndex(addr | 0x80000000u);
    if (idx < 0 || !mLwOn) { if (out != stderr) fprintf(out, "sbs lw: last-writer map off or addr out of range\n"); return; }
    const LastW& a = mLwA[idx];
    const LastW& b = mLwB[idx];
    fprintf(out, "[sbs] last-writer 0x%08X:  A pc=%08X ra=%08X sp=%08X f%u  |  B pc=%08X ra=%08X sp=%08X f%u\n",
            addr, a.pc, a.ra, a.sp, a.frame, b.pc, b.ra, b.sp, b.frame);
  }

  // ---- lockstep state ----
  uint32_t mFrame = 0;
  int      mSel   = 0;   // 0 = A, 1 = B (window + debug-server target)
  uint32_t mLo = 0x80010000u;
  uint32_t mHi = 0x80200000u;

  // ---- per-pane RGBA readback ----
  uint8_t  mRgbaA[1024 * 512 * 4];
  uint8_t  mRgbaB[1024 * 512 * 4];
  int      mWa = 0, mHa = 0, mWb = 0, mHb = 0;

  // ---- divergence record (frame-boundary RAM/scratchpad diff) ----
  bool     mDivFound = false;
  bool     mDivArmed = false;
  bool     mSeenCutA = false, mSeenCutB = false, mFrA = false, mFrB = false;
  uint32_t mDivFrame = 0, mDivAddr = 0, mDivEnd = 0;
  // Detection-time byte snapshot of the divergent range. `sbs diff` must show THESE, not live
  // memory: after rewind-and-arm both cores are restored to the pre-frame state, so a live read
  // shows identical bytes and hides what actually differed.
  uint8_t  mDivBytesA[64] = {0}, mDivBytesB[64] = {0};
  uint32_t mDivBytesN = 0;
  char     mBtA[4096] = {0}, mBtB[4096] = {0};
  bool     mHaveDbgsrv = false;

  // ---- M_SKIP observable-output compare (USER 2026-07-08, tightened 2026-07-10) ----
  // Originally: pc_skip vs recomp is NOT byte-comparable (cadence legitimately collapses); compare
  // a POSITIVE LIST of observable state and report only SETTLED divergences (a region must differ
  // for kObsPersist consecutive frames — transient lead/lag during loads was "by design").
  //
  // 2026-07-10 FRAME ALIGNMENT: that 60-frame tolerance existed to paper over exactly the cadence
  // drift the skipRendezvousReached() barrier (above) now closes at each wired fork site — once a
  // fork rendezvous-waits for the oracle to catch up, "differ for a while during the load, then
  // converge" is no longer an expected shape for content downstream of that fork; a divergence that
  // shows up and STAYS is either a genuine behavioral bug or a fork this pass didn't wire yet (see
  // docs/findings/sbs.md "SKIP-mode frame alignment" for the wired/unwired fork inventory). Dropped
  // to 1 (first differing frame reports immediately, no persistence window) so this compare is now
  // as strict as checkDivergence()'s byte-exact path elsewhere in this file. Any address that
  // legitimately still lags at f1 because ITS fork isn't rendezvous-gated yet will show up here
  // honestly as a divergence — that's the harness doing its job, not a false positive to mask.
  static constexpr int kObsPersist = 1;
  static constexpr int kNObs = 6;               // fixed regions + area-deref + SPU RAM (below)
  int      mObsCnt[8]  = {0};                   // consecutive differing frames per observable
  bool     mObsDone[8] = {false};               // reported already (report once, stay running)
  uint8_t* mObsSpuA = nullptr;                  // 512 KB SPU RAM peek buffers
  uint8_t* mObsSpuB = nullptr;
  void checkObservables();

  // ---- SKIP-mode rendezvous (frame alignment, USER 2026-07-10, see sbs.h skipRendezvousReached) ----
  // Per-label wait bookkeeping: when did we start waiting on this label, is a wait currently active,
  // and (for the end-of-run audit report) how many times did we check it / how many frames total did
  // we spend stalled on it. A label that's checked but never once stalls is a clean pass-through; a
  // label whose wait count only ever grows and times out is the deadlock case skipRendezvousReached
  // aborts on directly (loud, not a hang).
  struct RvSite { uint32_t firstWaitFrame = 0; bool waiting = false; uint32_t checks = 0;
                  uint32_t stalls = 0; uint32_t maxWaitFrames = 0; };
  std::map<std::string, RvSite> mRvSites;
  static constexpr uint32_t kRvTimeoutFrames = 3600;   // 60s @ 60fps — deadlock diagnostic, not a hang
  bool mRvDumped = false;
  void dumpRendezvousSites(FILE* out);
  bool skipCompareMode() const { return mMode == M_SKIP; }
  bool skipRendezvousReached(Core* c, uint32_t addr, uint32_t minVal, const char* label);

  // ---- write-watchpoint record (exact corrupting-write site) ----
  bool     mWwArmed = false;
  bool     mWwPersist = false;      // PREWATCH: stay armed until first DIVERGENT write (not first write)
  uint32_t mWwAddr  = 0;
  int      mWwHit   = 0;            // bit0 = A wrote, bit1 = B wrote
  uint32_t mWwVa = 0, mWwVb = 0;
  char     mWwBtA[4096] = {0}, mWwBtB[4096] = {0};
  // Per-core call-site metadata captured on each armed store during the rewind. Used to auto-diagnose
  // the divergent call PATH (differing pc/ra names the split site) without hand-eyeballing the log.
  uint32_t mWwPcA = 0,  mWwPcB = 0;
  uint32_t mWwRaA = 0,  mWwRaB = 0;
  uint32_t mWwSpA = 0,  mWwSpB = 0;
  uint32_t mWwCountA = 0, mWwCountB = 0;   // #stores per core landing on mWwAddr in the rewind frame
  // Host-side C-stack backtrace at write time. c->pc is often STALE (reflects the last recomp fn
  // wrapper's set, not the actual writer), so the guest-side pc/ra alone can lie about who wrote.
  // The host backtrace names the ACTUAL C function running when mem_w8/w16/w32 fires — that's the
  // uncontested writer. Only the LAST fire per core is retained (cheap): for a COUNT-MISMATCH it's
  // the write whose value survives to the frame boundary; for a single fire it IS that fire.
  static constexpr int WW_HOST_BT_DEPTH = 24;
  void*    mWwHostBtA[WW_HOST_BT_DEPTH] = {}, *mWwHostBtB[WW_HOST_BT_DEPTH] = {};
  int      mWwHostBtNA = 0, mWwHostBtNB = 0;

  // ---- pre-step snapshot (for one-frame rewind on divergence) ----
  // Fixes the "wwatch arms AFTER the divergent frame already ran" defect: we snapshot both cores'
  // RAM+scratchpad+regs+pc BEFORE each stepCore(). When checkDivergence at frame N finds a diff,
  // we restore both cores, arm wwatch on the divergent addr, and RE-STEP frame N. The wwatch then
  // catches the exact divergent write(s) in-frame with both cores' stacks, in ONE pass — no manual
  // PREWATCH re-run. SPU/GTE/MDEC state isn't rewound (they advance twice), but the RAM-diff
  // divergence-write pin-point works because those subsystems don't write into the diff region.
  uint8_t* mPreRamA = nullptr;
  uint8_t* mPreRamB = nullptr;
  uint8_t  mPreSpadA[0x400] = {0};
  uint8_t  mPreSpadB[0x400] = {0};
  uint32_t mPreRegsA[32]    = {0};
  uint32_t mPreRegsB[32]    = {0};
  uint32_t mPrePcA = 0, mPrePcB = 0;
  // Per-core scheduler bookkeeping snapshot. On rewind, guest RAM + regs get restored but the
  // scheduler's per-slot state (task_started[], stage0_step[], native-dispatcher flags, saved
  // task-context registers) lives on the Game host object — the re-stepped frame would inherit
  // stale bookkeeping from the pre-rewind execution and take the resume path with a mid-body
  // task_ctx.r[31] that misses. Snap the trivially-copyable fields alongside RAM and restore in
  // rewindAndArm. Fibers (coro[3]) are torn down separately — their C-stack can't be rolled back.
  // (SchedSnap type lives at file scope below so the free-function helpers can name it.)
  SbsSchedSnap mPreSchedA{}, mPreSchedB{};
  bool     mPreSnapValid = false;
  bool     mRewindActive = false;   // in the rewind re-step: don't snapshot, don't re-check divergence
  int      mRewindDone   = 0;       // 0=not-rewound, 1=rewound-and-restepped (headless exit gate)

  // ---- ALLOCTRACE: per-frame count of writes to 0x800ED098 (the free-slot count) per core ----
  // Attack (a) instrumentation: names the frame(s) where A allocates more than B. If A > B on a
  // specific frame, that's where the 3-slot lead grows. Enabled with PSXPORT_SBS_ALLOCTRACE=1.
  int      mAllocTraceOn = 0;
  int      mAllocA = 0, mAllocB = 0;   // per-frame decrement count (any write value < current)
  int      mAllocCumA = 0, mAllocCumB = 0;
  uint32_t mStageTraceSigA = 0, mStageTraceSigB = 0;   // stagetrace change-detector signatures
  // Per-ra bucket: {alloc, release} counts by guest r[31] at store time, split A vs B. Landed as the
  // durable workflow-first invariant for +N-alloc attribution — ordinal-point-in-time comparison is
  // misleading (a timing-shifted alloc reads as "A-only") and the correct compare is at SETTLED STATE
  // (per-ra totals over the whole run). Asymmetric buckets = real caller divergence; symmetric = a
  // timing shift. Dumped at the end of every run when ALLOCTRACE is on; opt-in REPL `sbs allocra`.
  struct RaBucket { int allocA=0, allocB=0, relA=0, relB=0; };
  std::map<uint32_t, RaBucket> mAllocRa;
  int      mAllocRaDumped = 0;

  // ---- BYTETRACE: per-byte-value + ra bucketing over a range, with auto-classification.
  // The generalization of ALLOCRA to arbitrary byte ranges. Every 1/2/4-byte store landing in
  // [mByteTraceLo, mByteTraceHi) is decomposed into its constituent BYTES, and for each byte address
  // we tally (value → count) per core plus (ra → count) per core. At end-of-run we classify each
  // divergent byte:
  //   PHASE-NOISE: value_counts_A == value_counts_B (both cores wrote the same value set with the
  //                same counts; the byte just happens to be at a different phase at snapshot time).
  //   REAL      : some value has A_count != B_count OR one core wrote a value the other never did.
  // Enables PSXPORT_SBS_BYTETRACE=<lo>,<hi>. Dumps at atexit / SIGTERM (shares the ALLOCRA hook).
  // Contiguous runs of PHASE-NOISE bytes are emitted as suggested SBS noise-filter ranges so future
  // PREWATCH hunts aren't misled by phase flicker (recurring-blocker fix: name once, filter forever).
  int      mByteTraceOn = 0;
  uint32_t mByteTraceLo = 0, mByteTraceHi = 0;
  struct BytePerCore {
    std::map<uint8_t, uint32_t>  vals;      // value → count
    std::map<uint32_t, uint32_t> ras;       // guest r[31] → count
  };
  struct ByteRow { BytePerCore a, b; };
  std::map<uint32_t, ByteRow> mByteTrace;

  // ---- scripted headless input (PSXPORT_SBS_KEYS) ----
  std::vector<SbsKey> mKeys;
  bool                mKeysParsed = false;

  // ---- navigation state (concurrent boot AUTO-NAV to free-roam) ----
  Nav mNavA, mNavB;

  // ---- helpers / stages ----
  const char* modeName() const;
  // Noise-filter methods (isRenderRegion / isCdCacheNoise / isAudioNoise / isObjectPoolNoise /
  // isRenderSpad / isDiffNoise) were REMOVED 2026-07-03 per the standing rule: no RAM diverge may
  // be waved off as "residual/known/expected" (memory: no_residual_ram_diverges). Every diff is
  // fatal and gets root-caused, so filter ranges have no place here. If a diff is a PSX-quirk
  // native deliberately skips, the fix is to gate the quirk on !c->game->pc_skip at the write
  // site (see cull.cpp / engine.cpp) — i.e. do the faithful thing when pc_skip is off,
  // NOT to blacklist the address.
  //
  // EXCEPTION 2026-07-04 (user directive [[sbs-two-compare-modes]]): pc_skip=ON is ALLOWED to
  // diverge in TRUE SCRATCH — stack-frame leftovers from substrate boot chains that native's
  // collapsed shortcut never enters, transient scratchpad state that has no consumer on pc_skip.
  // Shared/consumable state (libcd dir cache, task-slot fields, done_flag when a native path
  // reads it, ...) still must match. The mask below applies ONLY when core A is pc_skip=true;
  // under pc_skip=false (PSXPORT_SBS_PCFAITHFUL=1) the compare stays strict byte-exact.
  // Per-frame `mMaskedBytes` counts hits so overreach is visible in the summary line.
  bool mPcSkipMask = false;                    // set from Core A's pc_skip at init
  uint32_t mMaskedBytes = 0;                    // per-frame counter, reset each summary
  bool isPcSkipScratch(uint32_t addr) const {
    if (!mPcSkipMask) return false;
    // Task-stack window under the substrate boot chain. Task-slot control blocks live at
    // 0x801FE000..0x801FE14F (3 × 0x70; those fields ARE state and must match — reproduced by
    // native_task_spawn). Above that up to 0x801FF200 (0xDEAD sentinel at task-0 stack top) is
    // task-0's stack — the substrate's boot chain (FUN_800499E8 → CdSearchFile ×30 → memcpy
    // ×N → OpenTh initial-frame setup) writes into every byte of it. Under pc_skip, native's
    // Engine::startBinStage skips those substrate calls entirely, so their stack scratch is
    // absent by design.
    if (addr >= 0x801FE150u && addr < 0x801FF200u) return true;
    // libgs boot RECT scratch that pc_skip's Engine::startBinStage writes at 0x1F800008..F for
    // asset.uploadImage (VRAM RECT header); substrate uses a guest-RAM RECT elsewhere so this
    // scratchpad window differs by direction (A has extra ephemeral write, B has none).
    if (addr >= 0x1F800008u && addr < 0x1F800010u) return true;
    // libgs graphics context / DMA state (base 0x800AC5F8, stride 0x100). Populated by the
    // substrate FUN_80081218 (LoadImage) → FUN_80097194 chain each time a LoadImage fires; the
    // substrate does dozens across boot with a specific ordering. pc_skip's Engine::startBinStage
    // uses native asset.uploadImage (gpu_native_load_image, direct VRAM write) which BYPASSES
    // the whole libgs DMA state machine — so the values at 0x800AC5F8+ never match. Marked
    // scratch because native's DrawSync is a no-op (asset.cpp:146: "meaningless for our SYNCHRONOUS
    // native VRAM upload — there is no async DMA to wait on"), so no native pc_skip code path
    // reads this state to gate behavior. Any substrate GPU/DMA path later dispatched from
    // native does re-enter FUN_80081218 which rewrites 0x800AC61C for its own use.
    if (addr >= 0x800AC5F8u && addr < 0x800AC700u) return true;
    // Boot-preload TRANSIENT regions — under pc_skip these get populated by native's
    // startBinStage collapsing many substrate ticks into ~5 ticks (via stage0Advance), while
    // substrate B spreads the same work across ~10+ ticks in its own task-0 body + task-1
    // fiber. During the collapse window the values naturally differ (one core farther along
    // than the other). They CONVERGE once both cores complete their boot chain — this mask
    // hides only the transient window, not any post-gameplay-start divergence. If a byte in
    // one of these regions stays diverged AFTER gameplay-start, that's a real bug (drop from
    // mask + fix).
    if (addr >= 0x800BE0E0u && addr < 0x800BE0E4u) return true;    // CD position tracker (cd_override.cpp:151)
    if (addr >= 0x800BED80u && addr < 0x800BED88u) return true;    // preload cel_h etc (asset.cpp:222/226)
    if (addr >= 0x800ECF54u && addr < 0x800ECF80u) return true;    // preload task-state u16s (engine.cpp:1523)
    if (addr >= 0x800ED000u && addr < 0x800ED020u) return true;    // preload metadata
    if (addr >= 0x800EF478u && addr < 0x800EF500u) return true;    // texgroup header buffer (asset.cpp:188)
    if (addr >= 0x80105C10u && addr < 0x80105CA0u) return true;    // BAV slot descriptor table (bav_loader.cpp:67)
    if (addr >= 0x80105D00u && addr < 0x80105EE8u) return true;    // preload metadata (excludes the RNG seed — RNG must be faithful, user 2026-07-04)
    if (addr >= 0x80105EECu && addr < 0x80105F00u) return true;    // preload metadata after RNG seed
    if (addr >= 0x80157000u && addr < 0x8017D000u) return true;    // preload allocation + AI-code regions (boot-transient)
    // Boot-time state whose ONLY consumer is the substrate loader chain pc_skip skips: async CD
    // read descriptor (0x1F8001F0..F4 = lba/words/dst), scheduler current-task pointer at boot
    // (0x1F800138, populated once the fiber scheduler starts stepping), loader done_flag
    // (0x1F80019B, native's synchronous preload has nothing to signal).
    if (addr == 0x1F800138u) return true;
    if (addr == 0x1F80019Bu) return true;
    if (addr >= 0x1F8001F0u && addr < 0x1F8001FCu) return true;
    return false;
  }
  // isDeadStackScratch REMOVED 2026-07-08 (docs/findings/animation.md): Animation::attach
  // (FUN_80077C40) now mirrors its real 32-byte guest-stack frame (game/object/animation.cpp) —
  // a probe (PSXPORT_DEBUG=animstack) proved c->r[29] is IDENTICAL between SBS core A and core B at
  // every reach of attach, disproving the earlier "no canonical frame" assumption that justified
  // this exclusion. No residual RAM diverges (CLAUDE.md) — this was the last such exception.
  static bool isSpad(uint32_t a) { return a >= 0x1F800000u && a < 0x1F800400u; }
  void  capBt(Core* c, char* buf, size_t n);
  bool  navStep(Core* c, Nav& nv, uint32_t f, const char* tag);
  void  applyMode(Game* g, int which);
  void  recordDivergence(uint32_t addr);
  void  takePreStepSnap();
  void  rewindAndArm(uint32_t addr);
  void  checkDivergence();
  // Per-frame divergence SUMMARY: count differing bytes (excluding render noise) across main RAM
  // + scratchpad and log a one-line report each `every` frames. Doesn't pause on the first byte
  // (that's `checkDivergence()`); it surfaces the running "how far apart are the two cores" number
  // so a divergence trend is visible even before checkDivergence trips.
  void  summarizeDivergence(uint32_t every);
  void  stepCore(Game* g, int which);
  void  grabPane(Game* g, uint8_t* rgba, int* ow, int* oh);
  void  presentPanes();
  uint16_t btnBit(const char* n) const;
  void  parseKeys();
  void  feedInput();
  void  dumpPpm(const char* path);
  // PICTURE compare (USER 2026-07-08): pixel-diff the port pane (A = pc_render) against the oracle
  // pane (B = psx_render / recomp+psx_render in full mode). SBS's RAM compare is BLIND to render bugs
  // (pc_render is a read-only overlay — it never writes guest RAM), so a wrong picture can coexist
  // with byte-identical guest state. This names the frames + screen region where the port renders
  // wrong. PSXPORT_SBS_RENDERDIFF=<pct> arms it (default threshold 2.0% of pixels); worst frames are
  // dumped to scratch/screenshots/renderdiff/ for the user to eyeball.
  void  checkPaneDiff();
  bool     mRdiffOn = false; int mRdiffChecked = 0;
  double   mRdiffThreshPct = 2.0;
  uint32_t mRdiffWorstFrame = 0; double mRdiffWorstPct = 0.0;
};

const char* Sbs::Impl::modeName() const {
  return mMode == M_RENDER ? "render" : mMode == M_GAMEPLAY ? "gameplay" :
         mMode == M_ORACLE ? "oracle" : mMode == M_SKIP ? "skip" : "full";
}

// Noise-filter method DEFINITIONS were removed with their declarations above (2026-07-03).

void Sbs::Impl::capBt(Core* c, char* buf, size_t n) {
  buf[0] = 0;
  FILE* f = fmemopen(buf, n, "w");
  if (f) { guest_backtrace_to(c, f); fclose(f); }
}

// Navigation to REAL PLAYER CONTROL (concurrent per-core AUTO-NAV — identical shape to AUTO_SKIP /
// dualcore, extended 2026-07-08 per docs/findings/sbs.md "AUTONAV: reaching real player control"):
// (0) tap Cross until the GAME stage, (1) wait for the intro cutscene to begin, (2) pulse Start
// while the cutscene flag is up, until it has read 0 for 60 consecutive frames (fade settled — this
// is FIELD RENDERING, not player control: Tomba is still scripted-caught), (3) AWAIT_CONTROL: tap
// Cross to release the fishing-line hold (fieldRun sm[0x4e]==9) and wait for sm[0x4e] to settle at
// 1 (the genuine running-field frame) — THIS is when Tomba responds to pad input.
bool Sbs::Impl::navStep(Core* c, Nav& nv, uint32_t f, const char* tag) {
  if ((f % 400u) == 0) fprintf(stderr, "[sbs-nav] %s f%u phase=%d stage=%08X cut=%u\n",
                               tag, f, (int)nv.phase, c->mem_r32(TASK0_ENTRY), c->mem_r8(CUT_FLAG));
  uint8_t cut = c->mem_r8(CUT_FLAG);
  switch (nv.phase) {
    case REACH_GAME:
      if (c->mem_r32(TASK0_ENTRY) == GAME_ENTRY) { fprintf(stderr, "[sbs] %s GAME @f%u\n", tag, f); nv.phase = AWAIT_CUT; }
      else if ((f % 12u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
      break;
    case AWAIT_CUT:
      if (cut) { fprintf(stderr, "[sbs] %s cutscene up @f%u\n", tag, f); nv.phase = SKIP_CUT; nv.idle = 0; }
      break;
    case SKIP_CUT: {
      // PSXPORT_SBS_WATCH_CUT=1 — DON'T press Start during the intro cutscene; let it play out
      // naturally so its scripted SFX (fisherman scene, etc.) actually fire on both cores. That's
      // what makes the SFX bug (#29) surface via divergence-check. Default (=0) keeps the fast-skip
      // behavior for rapid gameplay-start reach.
      // PSXPORT_SBS_CUT_PRESSES=<N> — press Start exactly N times during the cutscene, then stop
      // (let the rest play out with its SFX). Use 3-5 to skip the intro narration text but let the
      // fisherman scene animate + play its footstep / splash SFX so #29 surfaces (user 2026-07-04).
      static const int watch_cut = []{ const char* e = getenv("PSXPORT_SBS_WATCH_CUT"); return e && *e && e[0] != '0' ? 1 : 0; }();
      static const int cut_presses = []{ const char* e = getenv("PSXPORT_SBS_CUT_PRESSES"); return e && *e ? atoi(e) : -1; }();
      if (cut) {
        nv.idle = 0;
        bool press_ok = !watch_cut && (cut_presses < 0 || nv.postFrame < cut_presses);
        if (press_ok && (f % 40u) == 0) { c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_START), 6); nv.postFrame++; }
      }
      else if (++nv.idle >= 60) { fprintf(stderr, "[sbs] %s field-rendering @f%u (still scripted-caught — awaiting real control)\n", tag, f); nv.phase = AWAIT_CONTROL; nv.idle = 0; nv.postFrame = 0; }
      break;
    }
    case AWAIT_CONTROL: {
      // The SOP narration ending (CUT_FLAG->0, previous phase) is NOT player control — Tomba is
      // still caught on the fishing line (fieldRun sm[0x4e]==9, see the block comment above Nav).
      // That state only advances on a Cross-press EDGE while the scripted-camera gate 0x800BF89C
      // is armed (==2); tap Cross periodically (edges only — mashing every frame would still just
      // be one edge on press, but a hold produces exactly one edge then nothing, so TAP not HOLD)
      // until fieldRun settles at s4e==1 (the real "RUNNING field frame" state) for 30 consecutive
      // frames — long enough to rule out a transient pass-through (s4e visits 6/7/8/10/11 on the
      // way out of 9, and case 5 also sets s4e=1 transiently for an area-7 re-arm).
      uint16_t s4e = c->mem_r16(SM_S4E);
      if ((f % 20u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
      if (s4e == 1) {
        if (++nv.idle >= 30) {
          fprintf(stderr, "[sbs] %s player-controllable @f%u (s4e settled at 1, s4a=%u, gate=%u)\n",
                  tag, f, c->mem_r16(SM_S4A), c->mem_r8(FISH_GATE));
          nv.phase = DONE; nv.idle = 0; return true;
        }
      } else {
        nv.idle = 0;
        if ((f % 200u) == 0) fprintf(stderr, "[sbs-nav] %s f%u awaiting control: s4e=%u s4a=%u gate=%u\n",
                                      tag, f, s4e, c->mem_r16(SM_S4A), c->mem_r8(FISH_GATE));
      }
      break;
    }
    case DONE: {
      // PSXPORT_SBS_AUTONAV=combat — the combat-coverage leg (see the sbsCombatOn() banner above).
      // Deterministic: hold Right the whole time (driveHold's auto-resume after each tap keeps it
      // held with no re-scripting needed), and fire a JUMP EDGE every COMBAT_JUMP_PERIOD frames
      // starting at COMBAT_JUMP_FRAME — a single well-timed jump cleared the first ledge in an
      // interactive repro, but a fixed single frame offset in the free-running SBS timeline isn't
      // guaranteed to land on the same collision-window edge every ledge (Tomba can end up pinned
      // against an obstacle for a stretch, same shape as the live REPL session's "stuck" case) —
      // repeated periodic jump attempts make the leg robust to that without needing to poll game
      // state (edges only, same "TAP not HOLD" rule as the rest of Nav). ActorMeleeEngage already
      // fires once merely adjacent to the first obstacle (confirmed 2026-07-10), so this also keeps
      // giving that address fresh coverage every run even if a further ledge is never cleared.
      if (sbsCombatOn()) {
        nv.postFrame++;
        constexpr int COMBAT_JUMP_FRAME = 300, COMBAT_JUMP_PERIOD = 60;
        if (nv.postFrame == 1) c->game->pad.driveHold((uint16_t)(BTN_NONE & ~BTN_RIGHT));
        else if (nv.postFrame >= COMBAT_JUMP_FRAME && (nv.postFrame - COMBAT_JUMP_FRAME) % COMBAT_JUMP_PERIOD == 0)
          c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
        // PSXPORT_DEBUG=combatnav — periodic progress print (Tomba G-block position + pad drive
        // state), the tool used to trace this leg's navigation live (2026-07-10) and to confirm it
        // reaches the ActorMeleeEngage/MeleeProximity encounter zone in future sessions.
        if (cfg_dbg("combatnav") && (nv.postFrame % 100) == 1) {
          constexpr uint32_t G_ADDR = 0x800E7E80u;
          fprintf(stderr, "[combatnav] %s pf=%d f=%u pos(Z,Y,X)=(%d,%d,%d) repl_on=%d hold=%04X tap_n=%d\n",
                  tag, nv.postFrame, f,
                  (int16_t)c->mem_r16(G_ADDR+46), (int16_t)c->mem_r16(G_ADDR+50), (int16_t)c->mem_r16(G_ADDR+54),
                  c->game->pad.repl_on, c->game->pad.repl_hold, c->game->pad.repl_tap_n);
        }
      } else if (sbsPostdriveOn()) {
        nv.postFrame++;
        // Cycle: 30 frames walk Right, 6-frame Cross tap, repeat. Each Cross tap = jump = SFX fire.
        int cycle = nv.postFrame % 36;
        if (cycle == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_CROSS), 6);
        else if (cycle == 6) c->game->pad.driveHold((uint16_t)(BTN_NONE & ~BTN_RIGHT));
      }
      return true;
    }
  }
  return false;
}

// Per-core render-path config. Sets THIS core's Render::mPsxRender — no shared global. psx_fallback is
// per-Game, set once at boot; render mode is set per core, per step.
void Sbs::Impl::applyMode(Game* g, int which) {
  Render* r = g->core.mRender;
  // PSXPORT_SBS_FORCE_PSX_RENDER=1 — bisect: is a divergence coming from the native-render path?
  // Force PSX render on BOTH cores regardless of mode. If a divergence that showed in RENDER mode
  // (A native render vs B PSX render) DISAPPEARS with this on, the writer lives on the native
  // render side. If it PERSISTS, native render is not the culprit. Cheap A/B one-liner.
  static const int forcePsxRender = []{ const char* e = getenv("PSXPORT_SBS_FORCE_PSX_RENDER"); return e && *e && strcmp(e,"0")!=0 ? 1 : 0; }();
  if (forcePsxRender) { r->mode.setPsxRender(true); return; }
  switch (mMode) {
    case M_RENDER:   r->mode.setPsxRender(which != 0);    break;   // A native render (0), B PSX render (1)
    case M_GAMEPLAY: r->mode.setPsxRender(true);          break;   // PSX render on BOTH (isolate gameplay)
    case M_FULL:     r->mode.setPsxRender(which != 0);    break;   // A native render, B PSX render
    case M_ORACLE:   r->mode.setPsxRender(false);         break;   // A native; B goes through use_interp+soft_gpu
    case M_SKIP:     r->mode.setPsxRender(which != 0);    break;   // A = real ./run.sh config, B PSX
  }
}

// Take the pre-step snapshot: RAM + scratchpad + regs + pc for both cores. Called right BEFORE
// stepCore() on both A and B each frame. If a divergence is detected at frame boundary, this
// snapshot is the "just before frame N started" state that we rewind to.
static void sbs_snap_core(Core& c, uint8_t* ram, uint8_t* spad, uint32_t* regs, uint32_t& pc) {
  memcpy(ram, c.ram, 0x200000);
  memcpy(spad, c.scratch, 0x400);
  memcpy(regs, c.r, sizeof c.r);
  pc = c.pc;
}
static void sbs_restore_core(Core& c, const uint8_t* ram, const uint8_t* spad, const uint32_t* regs, uint32_t pc) {
  memcpy(c.ram, ram, 0x200000);
  memcpy(c.scratch, spad, 0x400);
  memcpy(c.r, regs, sizeof c.r);
  c.pc = pc;
}
// Snapshot the trivially-copyable PcScheduler fields (skip jmp_buf and Coro* — see SchedSnap
// docstring on the harness class).
static void sbs_snap_sched(const PcScheduler& s, SbsSchedSnap& out) {
  memcpy(out.task_ctx,        s.task_ctx,        sizeof out.task_ctx);
  out.in_stage    = s.in_stage;
  out.cur_slot    = s.cur_slot;
  memcpy(out.task_started,    s.task_started,    sizeof out.task_started);
  memcpy(out.demo_native,     s.demo_native,     sizeof out.demo_native);
  memcpy(out.game_native,     s.game_native,     sizeof out.game_native);
  memcpy(out.game_coop,       s.game_coop,       sizeof out.game_coop);
  out.cur_is_coro = s.cur_is_coro;
  memcpy(out.stage0_step,     s.stage0_step,     sizeof out.stage0_step);
  memcpy(out.sop_field_step,  s.sop_field_step,  sizeof out.sop_field_step);
  memcpy(out.demo_leave_step, s.demo_leave_step, sizeof out.demo_leave_step);
  memcpy(out.demo_s0_step,    s.demo_s0_step,    sizeof out.demo_s0_step);
  memcpy(out.resident_ov,     s.resident_ov,     sizeof out.resident_ov);
}
// Restore scheduler bookkeeping AND tear down any live Coro fibers — a fiber's C-stack reflects
// the pre-rewind PSX execution and cannot be rolled back, so we delete + null it. The re-stepped
// frame will re-enter the fresh-coro branch (task_started[] just got zeroed for started slots),
// which spawns a new fiber from a clean stack.
static void sbs_restore_sched(PcScheduler& s, const SbsSchedSnap& in) {
  memcpy(s.task_ctx,        in.task_ctx,        sizeof s.task_ctx);
  s.in_stage    = in.in_stage;
  s.cur_slot    = in.cur_slot;
  memcpy(s.task_started,    in.task_started,    sizeof s.task_started);
  memcpy(s.demo_native,     in.demo_native,     sizeof s.demo_native);
  memcpy(s.game_native,     in.game_native,     sizeof s.game_native);
  memcpy(s.game_coop,       in.game_coop,       sizeof s.game_coop);
  s.cur_is_coro = in.cur_is_coro;
  memcpy(s.stage0_step,     in.stage0_step,     sizeof s.stage0_step);
  memcpy(s.sop_field_step,  in.sop_field_step,  sizeof s.sop_field_step);
  memcpy(s.demo_leave_step, in.demo_leave_step, sizeof s.demo_leave_step);
  memcpy(s.demo_s0_step,    in.demo_s0_step,    sizeof s.demo_s0_step);
  memcpy(s.resident_ov,     in.resident_ov,     sizeof s.resident_ov);
  for (int i = 0; i < 3; i++) {
    if (s.coro[i]) { delete s.coro[i]; s.coro[i] = nullptr; }
  }
}
void Sbs::Impl::takePreStepSnap() {
  if (!mPreRamA) { mPreRamA = (uint8_t*)malloc(0x200000); mPreRamB = (uint8_t*)malloc(0x200000); }
  sbs_snap_core(mA->core, mPreRamA, mPreSpadA, mPreRegsA, mPrePcA);
  sbs_snap_core(mB->core, mPreRamB, mPreSpadB, mPreRegsB, mPrePcB);
  sbs_snap_sched(mA->pcSched, mPreSchedA);
  sbs_snap_sched(mB->pcSched, mPreSchedB);
  mPreSnapValid = true;
}
void Sbs::Impl::rewindAndArm(uint32_t addr) {
  if (!mPreSnapValid) { fprintf(stderr, "[sbs] rewind: no snapshot — divergence surfaced pre-nav.\n"); return; }
  fprintf(stderr, "[sbs] rewinding one frame to catch the divergent write on 0x%08X on BOTH cores.\n", addr);
  sbs_restore_core(mA->core, mPreRamA, mPreSpadA, mPreRegsA, mPrePcA);
  sbs_restore_core(mB->core, mPreRamB, mPreSpadB, mPreRegsB, mPrePcB);
  sbs_restore_sched(mA->pcSched, mPreSchedA);
  sbs_restore_sched(mB->pcSched, mPreSchedB);
  mWwAddr = (addr & ~3u) | 0x80000000u; mWwArmed = true; mWwPersist = true;
  mWwHit = 0; mWwVa = mWwVb = 0; mWwBtA[0] = mWwBtB[0] = 0;
  mWwPcA = mWwPcB = mWwRaA = mWwRaB = mWwSpA = mWwSpB = 0;
  mWwCountA = mWwCountB = 0;
  mWwHostBtNA = mWwHostBtNB = 0;
  mA->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
  mB->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
  mRewindActive = true;
}

void Sbs::Impl::recordDivergence(uint32_t addr) {
  bool spad = isSpad(addr);
  uint32_t end_addr = spad ? 0x1F800400u : mHi;
  uint32_t last = addr, gap = 0;
  for (uint32_t x = addr + 1; x < end_addr && gap < 64; x++) {
    if (mA->core.mem_r8(x) != mB->core.mem_r8(x)) { last = x; gap = 0; } else gap++;
  }
  mDivFound = true; mDivFrame = mFrame; mDivAddr = addr; mDivEnd = last + 1;
  capBt(&mA->core, mBtA, sizeof mBtA);
  capBt(&mB->core, mBtB, sizeof mBtB);
  fprintf(stderr, "\n[sbs] *** DIVERGENCE at lockstep frame %u: 0x%08X..0x%08X (mode=%s) ***\n",
          mFrame, mDivAddr, mDivEnd, modeName());
  // Print the diverging bytes side-by-side so it's clear WHAT differs, without needing debug server.
  {
    uint32_t n = mDivEnd - mDivAddr;
    if (n > 64) n = 64;
    mDivBytesN = n;
    for (uint32_t i = 0; i < n; i++) {
      mDivBytesA[i] = isSpad(mDivAddr+i) ? mA->core.scratch[(mDivAddr+i)-0x1F800000u] : mA->core.mem_r8(mDivAddr+i);
      mDivBytesB[i] = isSpad(mDivAddr+i) ? mB->core.scratch[(mDivAddr+i)-0x1F800000u] : mB->core.mem_r8(mDivAddr+i);
    }
    fprintf(stderr, "[sbs] diff bytes (up to 64):\n");
    fprintf(stderr, "  A @0x%08X:", mDivAddr);
    for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %02X", mDivBytesA[i]);
    fprintf(stderr, "\n  B @0x%08X:", mDivAddr);
    for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %02X", mDivBytesB[i]);
    fprintf(stderr, "\n           ");
    for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %s", mDivBytesA[i] != mDivBytesB[i] ? "^^" : "  ");
    fprintf(stderr, "\n");
  }
  // Print BOTH guest-stack backtraces — captured at the frame boundary AFTER the diverging write, but
  // still pinpoints the region of code that just ran on each core.
  fprintf(stderr, "[sbs] === FRAME-BOUNDARY BACKTRACE — core A ===\n%s", mBtA[0] ? mBtA : "(empty)\n");
  fprintf(stderr, "[sbs] === FRAME-BOUNDARY BACKTRACE — core B ===\n%s", mBtB[0] ? mBtB : "(empty)\n");
  // REWIND-AND-ARM: the diff was detected at the END of frame N. Any writes in frame N have already
  // happened — a wwatch armed NOW would only catch frame N+1 onwards, and if only one core wrote in
  // frame N, that write is lost forever (previously required a manual PREWATCH re-run). Instead:
  // restore both cores to their pre-frame-N snapshot, arm wwatch, and re-step frame N. The re-step's
  // stores fire wwatch — we get BOTH cores' write-site backtraces + values in one pass. If a core
  // doesn't write to the addr in frame N, mask reflects that (single-side write => "the other core
  // took a different branch"), and we still get one exact backtrace. Skip in PREWATCH mode.
  // Last-writer map first: names both cores' writers with zero replay (works with native fibers).
  if (mLwOn) {
    lwReport(mDivAddr);
    for (uint32_t a = mDivAddr + 1; a < mDivEnd && a < mDivAddr + 8; a++) lwReport(a);
    // Optional extra probe: the lowest divergent byte is often a cascade (e.g. the packet-pool tail
    // pointer, written last by substrate propagating an earlier native divergence). Point this at a
    // divergent DATA byte to name the real writer. PSXPORT_SBS_LW_ADDR=0xADDR[,0xADDR2,...]
    const char* extra = getenv("PSXPORT_SBS_LW_ADDR");
    if (extra && *extra) {
      fprintf(stderr, "[sbs] (PSXPORT_SBS_LW_ADDR probes)\n");
      for (const char* p = extra; p && *p; ) {
        uint32_t a = (uint32_t)strtoul(p, (char**)&p, 0);
        if (a) { lwReport(a); for (uint32_t d = a + 1; d < a + 6; d++) lwReport(d); }
        while (*p == ',' || *p == ' ') p++;
      }
    }
  }
  // Fibers cannot be rewound: a native fiber's C stack does not restore with the RAM snapshot,
  // and a coro fiber parked MID-BODY cannot be replayed by respawning from the task entry
  // (sbs_restore_sched deletes it; the fresh fiber re-runs the body from the start — different
  // writes, post-rewind state is an ARTIFACT). With any fiber live on EITHER core, skip the
  // rewind and rely on the last-writer map instead.
  bool nativeFiberLive = false;
  for (int i = 0; i < 3; i++) {
    if (mA->pcSched.native_fiber[i] || mA->pcSched.coro[i]) nativeFiberLive = true;
    if (mB && (mB->pcSched.native_fiber[i] || mB->pcSched.coro[i])) nativeFiberLive = true;
  }
  if (!mWwArmed && !nativeFiberLive) rewindAndArm(mDivAddr);
  else if (nativeFiberLive) {
    fprintf(stderr, "[sbs] rewind skipped (fiber live — coro replay is unsound) — last-writer map above is the write-site source.\n");
    if (!mHaveDbgsrv) { fprintf(stderr, "[sbs] headless: exiting after last-writer report.\n"); sbs_rl_shutdown(); exit(0); }
  }
  if (mHaveDbgsrv) {
    fprintf(stderr, "[sbs] paused. Inspect over the debug server: `sbs diff`, `sbs bt`, `sbs watch`.\n");
    mA->dbg_server.setPaused(true);
  }
}

// Human-readable label for a divergent address so the log names *what* diverged, not just where.
// Audio-relevant hits (fx_table, spu-related scratchpad, area-audio-table) get a distinctive tag so
// they stand out when scanning a flood of divergences under PSXPORT_SBS_NOPAUSE=1.
static const char* addrLabel(uint32_t a) {
  if (a >= 0x800A4D18u && a <  0x800A5000u) return "AUDIO fx_table[0..111]";
  if (a >= 0x800A4EF8u && a <  0x800A4F80u) return "AUDIO fx_area_table_ptrs";
  if (a == 0x800FB165u)                    return "AUDIO global_scale";
  if (a >= 0x800AC000u && a <  0x800AC800u) return "libgs.gfx_ctx";
  if (a >= 0x800BE000u && a <  0x800BF000u) return "libcd/file-table";
  if (a >= 0x800BF4F0u && a <  0x800BF54Cu) return "packet_pool_ptrs";
  if (a >= 0x800BF800u && a <  0x800BF900u) return "area_state";
  if (a >= 0x800BFE68u && a <= 0x800E7E68u) return "packet_pool";
  if (a >= 0x800E7E68u && a <  0x800E8000u) return "OT_ring";
  if (a >= 0x800E8000u && a <  0x800E8100u) return "OT_head";
  if (a >= 0x801FE000u && a <  0x801FE200u) return "task_slots";
  if (a == 0x1F80019Bu)                    return "done_flag";
  if (a == 0x1F800137u)                    return "AUDIO paused_flag";
  if (a >= 0x1F800100u && a <  0x1F800200u) return "scratchpad_game_state";
  return "?";
}

// SKIP-mode frame alignment (USER 2026-07-10) — see sbs.h for the full design rationale. Reads the
// CALLING core's sibling's memory at `addr`; true once sibling's value >= minVal. A pass-through
// (always true) outside MODE=skip, so calling this from a fork site is safe in every context —
// callers still gate on skipCompareMode() first purely to avoid a per-frame map lookup + fprintf
// path in the hot default-play case.
//
// HALFWORD, not word: every known fork-completion field in this codebase (the stage-0/stage-1
// preload SM's task+0x48 etc.) is written with mem_w16, packed adjacent to a SIBLING sm byte at
// +2 (e.g. +0x4a) that's independently live. A mem_r32 read here would silently OR that sibling's
// garbage into the compared value and report "reached" the moment EITHER half went nonzero — found
// live during the first MODE=skip smoke run (start_bin_load "reached" after a single check, 0
// stalls, when the substrate oracle should still have been many frames from done). mem_r16 is the
// correct width for every current caller; widen this (and its callers' minVal range) the day a
// fork's shared completion field is genuinely a full word.
bool Sbs::Impl::skipRendezvousReached(Core* c, uint32_t addr, uint32_t minVal, const char* label) {
  RvSite& site = mRvSites[label];
  site.checks++;
  if (mMode != M_SKIP) return true;
  Core* other = (coreId(c) == 0) ? &mB->core : &mA->core;
  uint32_t v = other->mem_r16(addr);
  bool ok = v >= minVal;
  if (ok) {
    if (site.waiting) {
      uint32_t waited = mFrame - site.firstWaitFrame;
      if (cfg_dbg("skiprv"))
        fprintf(stderr, "[sbs][rendezvous] f%u '%s' SETTLED after %u frame(s) — 0x%08X now %u (wanted >=%u)\n",
                mFrame, label, waited, addr, v, minVal);
      site.waiting = false;
    }
    return true;
  }
  site.stalls++;
  if (!site.waiting) { site.waiting = true; site.firstWaitFrame = mFrame; }
  uint32_t waited = mFrame - site.firstWaitFrame;
  if (waited > site.maxWaitFrames) site.maxWaitFrames = waited;
  if (cfg_dbg("skiprv") && (waited % 60) == 0)
    fprintf(stderr, "[sbs][rendezvous] f%u waiting on '%s': 0x%08X=%u want>=%u (%u frame(s) so far)\n",
            mFrame, label, addr, v, minVal, waited);
  if (waited >= kRvTimeoutFrames) {
    // Deadlock diagnostic, not a hang (CLAUDE.md fail-fast): the shortcut side has been idling on
    // this milestone for a full minute of frames with no sign the oracle side is ever going to
    // write it — that's either a genuinely broken rendezvous predicate (wrong addr/minVal for this
    // fork) or the oracle side is itself stuck. Either way, printing both sides' relevant state and
    // aborting beats a silent 10-minute-gate hang that just looks like "the harness is slow".
    fprintf(stderr, "\n[sbs][rendezvous] *** DEADLOCK f%u: fork '%s' waited %u frames (>= timeout %u) ***\n"
                    "  addr 0x%08X: A=%u B=%u (want >= %u)\n"
                    "  A last id=%s\n",
            mFrame, label, waited, kRvTimeoutFrames, addr,
            mA->core.mem_r16(addr), mB->core.mem_r16(addr), minVal,
            coreId(c) == 0 ? "A(skip side)" : "B(oracle side)");
    dumpRendezvousSites(stderr);
    fflush(stderr);
    abort();
  }
  return false;
}

void Sbs::Impl::dumpRendezvousSites(FILE* out) {
  fprintf(out, "[sbs][rendezvous] fork-site audit (%zu label(s) checked this run):\n", mRvSites.size());
  if (mRvSites.empty()) {
    fprintf(out, "  (none — no rendezvous-gated fork site was reached, or this run wasn't MODE=skip)\n");
    return;
  }
  for (const auto& [label, s] : mRvSites) {
    fprintf(out, "  %-24s checks=%-8u stalls=%-8u maxWait=%-6u %s\n",
            label.c_str(), s.checks, s.stalls, s.maxWaitFrames,
            s.stalls == 0 ? "(never stalled — A and B were already aligned every time it was checked)"
                          : (s.waiting ? "(STILL WAITING AT EXIT — one-sided passage, harness error)" : "(stalled then settled)"));
  }
}

void Sbs::Impl::checkObservables() {
  // Positive list of observable regions (label, lo, hi) — guest RAM unless noted. Grow this list
  // as observable-output bugs surface; it is the OPPOSITE of an allowlist (what MUST match).
  struct Obs { const char* label; uint32_t lo, hi; };
  static const Obs kObs[kNObs] = {
    { "AUDIO fx_table",        0x800A4D18u, 0x800A4EF8u },
    { "AUDIO fx_area_ptrs",    0x800A4EF8u, 0x800A4F80u },
    { "AUDIO seq_slots",       0x800BE3B8u, 0x800BE3F8u },
    { "AUDIO global_scale",    0x800FB165u, 0x800FB166u },
    { "libcd file-table",      0x800BE0F0u, 0x800BE110u },
    { "SCENE_BEAT",            0x800BF9B4u, 0x800BF9B5u },  // SOP scene/backdrop identity byte —
                                                             // docs/findings/scene.md "prologue
                                                             // vortex backdrop missing" (2026-07-10)
  };
  auto ramA = [this](uint32_t a){ return mA->core.ram[(a & 0x1FFFFFFFu)]; };
  auto ramB = [this](uint32_t a){ return mB->core.ram[(a & 0x1FFFFFFFu)]; };
  // PSXPORT_SBS_SKIP_CONTINUE=1 — log-and-continue instead of abort() on a strict observable
  // divergence (same escape-hatch shape as MIRROR_VERIFY_CONTINUE, docs/config.md) — useful to
  // survey ALL observable regions in one run while triaging the first post-alignment frontier
  // instead of dying at the very first hit. Default is fail-fast abort, matching every other
  // strict SBS compare in this file (no residual RAM diverges, CLAUDE.md).
  static const bool skip_continue = []{ const char* e = getenv("PSXPORT_SBS_SKIP_CONTINUE");
    return e && *e && e[0] != '0'; }();
  auto report = [this](int idx, const char* label, uint32_t addr, uint8_t va, uint8_t vb) {
    fprintf(stderr, "\n[sbs-obs] *** OBSERVABLE DIVERGENCE f%u [%s] @0x%08X A=%02X B=%02X ***\n",
            mFrame, label, addr, va, vb);
    mObsDone[idx] = true;
    if (mHaveDbgsrv) { fprintf(stderr, "[sbs-obs] paused for inspection.\n"); mA->dbg_server.setPaused(true); }
    if (!skip_continue) { dumpRendezvousSites(stderr); fflush(stderr); abort(); }
  };
  for (int i = 0; i < kNObs; i++) {
    if (mObsDone[i]) continue;
    uint32_t bad = 0; uint8_t va = 0, vb = 0;
    for (uint32_t a = kObs[i].lo; a < kObs[i].hi; a++)
      if (ramA(a) != ramB(a)) { bad = a; va = ramA(a); vb = ramB(a); break; }
    if (!bad) { mObsCnt[i] = 0; continue; }
    if (++mObsCnt[i] >= kObsPersist) report(i, kObs[i].label, bad, va, vb);
  }
  { // area fx table deref: 0x800A4EF8[area] -> per-area SFX table content (in loaded area data)
    const int idx = kNObs;
    if (!mObsDone[idx]) {
      uint8_t area = mA->core.mem_r8(0x800BF870u);
      uint32_t pA = mA->core.mem_r32(0x800A4EF8u + area * 4u);
      uint32_t pB = mB->core.mem_r32(0x800A4EF8u + area * 4u);
      uint32_t bad = 0; uint8_t va = 0, vb = 0;
      if (pA != pB) { bad = 0x800A4EF8u + area * 4u; va = (uint8_t)pA; vb = (uint8_t)pB; }
      else if ((pA & 0x1FFFFFFFu) < 0x1FFE00u && pA)
        for (uint32_t o = 0; o < 0x200; o++)
          if (ramA(pA + o) != ramB(pA + o)) { bad = pA + o; va = ramA(pA + o); vb = ramB(pA + o); break; }
      if (!bad) mObsCnt[idx] = 0;
      else if (++mObsCnt[idx] >= kObsPersist) report(idx, "AUDIO area_fx_deref", bad, va, vb);
    }
  }
  { // SPU RAM: the loaded VAB sample banks — THE observable for issue #29 (wrong sample selected).
    // Peek both instances via bind-swap (each core's frame-step rebinds its own SPU afterwards).
    const int idx = kNObs + 1;
    if (!mObsDone[idx]) {
      if (!mObsSpuA) { mObsSpuA = (uint8_t*)malloc(524288); mObsSpuB = (uint8_t*)malloc(524288); }
      mA->spu.bind(); SPU_PeekRAM(mObsSpuA);
      mB->spu.bind(); SPU_PeekRAM(mObsSpuB);
      uint32_t bad = 0; bool diff = false;
      if (memcmp(mObsSpuA, mObsSpuB, 524288) != 0) {
        for (uint32_t o = 0; o < 524288; o++)
          if (mObsSpuA[o] != mObsSpuB[o]) { bad = o; diff = true; break; }
      }
      if (!diff) mObsCnt[idx] = 0;
      else if (++mObsCnt[idx] >= kObsPersist) {
        fprintf(stderr, "\n[sbs-obs] *** OBSERVABLE DIVERGENCE f%u [SPU RAM / VAB banks] "
                        "@0x%05X A=%02X B=%02X ***\n", mFrame, bad, mObsSpuA[bad], mObsSpuB[bad]);
        for (int k = 0; k < 3; k++) {   // a few following diff runs for shape
          while (bad < 524288 && mObsSpuA[bad] == mObsSpuB[bad]) bad++;
          if (bad >= 524288) break;
          uint32_t run = bad; while (run < 524288 && mObsSpuA[run] != mObsSpuB[run] && run - bad < 16) run++;
          fprintf(stderr, "[sbs-obs]   spu 0x%05X..0x%05X A:", bad, run);
          for (uint32_t o = bad; o < run; o++) fprintf(stderr, " %02X", mObsSpuA[o]);
          fprintf(stderr, "  B:");
          for (uint32_t o = bad; o < run; o++) fprintf(stderr, " %02X", mObsSpuB[o]);
          fprintf(stderr, "\n");
          bad = run + 1;
        }
        mObsDone[idx] = true;
        if (mHaveDbgsrv) { fprintf(stderr, "[sbs-obs] paused for inspection.\n"); mA->dbg_server.setPaused(true); }
        if (!skip_continue) { dumpRendezvousSites(stderr); fflush(stderr); abort(); }
      }
    }
  }
}

void Sbs::Impl::checkDivergence() {
  if (mMode == M_SKIP) { checkObservables(); return; }
  // PSXPORT_SBS_NOPAUSE=1 keeps SBS running past a divergence: each frame we log EVERY diverging
  // BYTE-RUN (contiguous run of differing bytes) with a category label and per-core values, then
  // continue. Purpose: let a native-code bug (e.g. #29 wrong SFX) surface as an AUDIO-labelled
  // divergence even when boot cadence has already produced dozens of pre-existing diffs. Under this
  // mode we DO NOT set mDivFound (so we re-check next frame) and we DO NOT rewind (rewind trashes
  // fiber C-stacks). The trade-off is verbosity — the log can be long — but grep by label narrows it.
  static const int nopause = []{ const char* e = getenv("PSXPORT_SBS_NOPAUSE"); return e && *e && e[0] != '0' ? 1 : 0; }();
  // PSXPORT_SBS_ONLY_LABEL=<prefix> — when set, only log divergences whose category label starts
  // with <prefix>. E.g. PSXPORT_SBS_ONLY_LABEL=AUDIO narrows the flood to audio-relevant hits so
  // Issue #29 (wrong SFX) can surface without wading through boot-cadence noise.
  static const char* only_label = []{ const char* e = getenv("PSXPORT_SBS_ONLY_LABEL"); return (e && *e) ? e : nullptr; }();
  if (mDivFound && !nopause) return;   // first-hit only in default mode

  auto scan = [this](uint32_t base, uint32_t sz, auto readA, auto readB) -> int {
    int hits = 0;
    uint32_t i = 0;
    while (i < sz) {
      if (readA(i) == readB(i) || isPcSkipScratch(base + i)) { i++; continue; }
      uint32_t run_start = i;
      while (i < sz && readA(i) != readB(i) && !isPcSkipScratch(base + i)) i++;
      uint32_t run_end = i;
      uint32_t addr = base + run_start;
      const char* label = addrLabel(addr);
      if (only_label && strncmp(label, only_label, strlen(only_label)) != 0) continue;
      uint32_t rlen = run_end - run_start;
      if (rlen > 32) rlen = 32;
      char va_hex[128] = {0}, vb_hex[128] = {0};
      for (uint32_t j = 0; j < rlen; j++) {
        snprintf(va_hex + j*3, 4, "%02X ", readA(run_start + j));
        snprintf(vb_hex + j*3, 4, "%02X ", readB(run_start + j));
      }
      fprintf(stderr, "[sbs-div] f%u [%s] 0x%08X..0x%08X (%u B)  A=%s B=%s\n",
              mFrame, label, addr, base + run_end, run_end - run_start, va_hex, vb_hex);
      hits++;
      if (!only_label && hits >= 16) { fprintf(stderr, "[sbs-div] f%u (more suppressed this frame)\n", mFrame); break; }
    }
    return hits;
  };

  int hits = 0;
  hits += scan(mLo, mHi - mLo,
               [this](uint32_t i){ return mA->core.ram[(mLo - 0x80000000u) + i]; },
               [this](uint32_t i){ return mB->core.ram[(mLo - 0x80000000u) + i]; });
  hits += scan(0x1F800000u, 0x400,
               [this](uint32_t i){ return mA->core.scratch[i]; },
               [this](uint32_t i){ return mB->core.scratch[i]; });

  if (hits > 0 && !nopause) {
    // Default (auto-pause) mode: mimic the old first-hit behavior by finding the true first address
    // and calling recordDivergence for the interactive rewind/pause flow. Skip pc_skip-masked bytes.
    const uint8_t* a = mA->core.ram + (mLo - 0x80000000u);
    const uint8_t* b = mB->core.ram + (mLo - 0x80000000u);
    uint32_t n = mHi - mLo;
    for (uint32_t i = 0; i < n; i++)
      if (a[i] != b[i] && !isPcSkipScratch(mLo + i)) { recordDivergence(mLo + i); return; }
    for (uint32_t i = 0; i < 0x400; i++)
      if (mA->core.scratch[i] != mB->core.scratch[i] && !isPcSkipScratch(0x1F800000u + i))
        { recordDivergence(0x1F800000u + i); return; }
  }
}

void Sbs::Impl::summarizeDivergence(uint32_t every) {
  if (!every || (mFrame % every) != 0) return;
  const uint8_t* a = mA->core.ram + (mLo - 0x80000000u);
  const uint8_t* b = mB->core.ram + (mLo - 0x80000000u);
  uint32_t n = mHi - mLo;
  // Per-64 KB page cluster histogram: counts bytes-differing per page so the drift's ACTUAL hot
  // regions surface (not just min/max bounds). The top-3 pages get reported so a large drift is
  // named by where it lives, not just how big it is.
  constexpr uint32_t PAGE_SHIFT = 16;   // 64 KB
  constexpr uint32_t N_PAGES    = ((0x200000u) >> PAGE_SHIFT) + 1;
  uint32_t pageCount[N_PAGES] = {0};
  uint32_t nDiff = 0, firstAddr = 0, lastAddr = 0, nMaskedRam = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (a[i] == b[i]) continue;
    if (isPcSkipScratch(mLo + i)) { nMaskedRam++; continue; }
    if (!nDiff) firstAddr = mLo + i;
    lastAddr = mLo + i;
    pageCount[(mLo + i - 0x80000000u) >> PAGE_SHIFT]++;
    nDiff++;
  }
  uint32_t nSpad = 0, nMaskedSpad = 0;
  for (uint32_t i = 0; i < 0x400; i++) {
    if (mA->core.scratch[i] == mB->core.scratch[i]) continue;
    if (isPcSkipScratch(0x1F800000u + i)) { nMaskedSpad++; continue; }
    nSpad++;
  }
  if (nDiff == 0 && nSpad == 0) {
    if (mPcSkipMask && (nMaskedRam || nMaskedSpad))
      fprintf(stderr, "[sbs] f%u: A/B identical modulo scratch mask (%u ram + %u spad masked) (mode=%s pc_skip=on)\n",
              mFrame, nMaskedRam, nMaskedSpad, modeName());
    else
      fprintf(stderr, "[sbs] f%u: A/B identical (mode=%s)\n", mFrame, modeName());
    return;
  }
  // Pick top-3 pages by count for the compact per-frame report.
  uint32_t topIdx[3] = {0, 0, 0}, topCnt[3] = {0, 0, 0};
  for (uint32_t p = 0; p < N_PAGES; p++) {
    uint32_t c = pageCount[p];
    if (c > topCnt[0])      { topCnt[2]=topCnt[1]; topIdx[2]=topIdx[1]; topCnt[1]=topCnt[0]; topIdx[1]=topIdx[0]; topCnt[0]=c; topIdx[0]=p; }
    else if (c > topCnt[1]) { topCnt[2]=topCnt[1]; topIdx[2]=topIdx[1]; topCnt[1]=c; topIdx[1]=p; }
    else if (c > topCnt[2]) { topCnt[2]=c; topIdx[2]=p; }
  }
  fprintf(stderr, "[sbs] f%u: A/B differ %u RAM bytes [0x%08X..0x%08X] + %u spad (mode=%s) | top pages:",
          mFrame, nDiff, firstAddr, lastAddr, nSpad, modeName());
  for (int k = 0; k < 3 && topCnt[k]; k++)
    fprintf(stderr, " 0x%08X:%u", 0x80000000u + (topIdx[k] << PAGE_SHIFT), topCnt[k]);
  if (mPcSkipMask && (nMaskedRam || nMaskedSpad))
    fprintf(stderr, " | scratch-masked: %u ram + %u spad", nMaskedRam, nMaskedSpad);
  fprintf(stderr, "\n");
}

// Step ONE core's frame for the SBS composite: diff_mode=1 suppresses its OWN per-core present/pace/audio
// (the SBS loop owns the single window present); sbs_render=1 re-enables the render-submit so it EMITS its
// geometry into VK batch `which` (gpu_gpu_select_target). Neither core presents; the SBS composite does.
void Sbs::Impl::stepCore(Game* g, int which) {
  g->core.game->diff_mode  = 1;
  g->core.game->sbs_render = 1;
  // (The old DEMO intro-FMV guest poke — mem_w8(0x1f80019d,1) while DEMO SM[0x48]==1 — is REMOVED,
  //  2026-07-10: it force-tore-down the demo machine's FMV sub-state in ~1 frame while a standalone
  //  run spends the guest's own strNext timeout there, so the SBS lockstep timeline ran AHEAD of both
  //  standalone configs and no pane could ever match a standalone picture at the same frame index.
  //  The guest's own timeout path (the same one standalone PSXPORT_ORACLE=1 takes — "skips OP.FMV")
  //  is slower in wall-clock but keeps frame-for-frame parity with the standalone timeline, which is
  //  the whole point of the panes.)
  applyMode(g, which);
  gpu_gpu_select_target(which);
  dc_step_frame(&g->core, mFrame);
}

// Render ONE core's just-emitted frame into the shared VK target HEADLESS and read it back to its CPU RGBA
// pane buffer (SDL_GPU window then draws it). Resets the VK geometry batch. Records the live display-region
// size for the SDL_GPU window upload.
// PSXPORT_SBS_NOPRESENT=1 — headless byte-compare runs skip the per-frame pane readback +
// present entirely (the VK readback dominates wall time once real 3D renders; the RAM compare
// needs none of it). Lazy latch so REPL-less runs pay one getenv.
static int sbs_nopresent() {
  static int v = -1;
  if (v < 0) { const char* e = getenv("PSXPORT_SBS_NOPRESENT"); v = (e && *e && e[0] != '0') ? 1 : 0; }
  return v;
}

void Sbs::Impl::presentPanes() {
  if (sbs_nopresent()) return;
  sbs_rl_present(mA, mRgbaA, mWa, mHa, mRgbaB, mWb, mHb);
}

// Pixel-diff the port pane (A) vs the oracle pane (B). Tolerance-gated so PSX-fixed vs native-float
// color/dither noise (a few LSBs everywhere) doesn't count — only STRUCTURAL differences (missing or
// misplaced geometry, wrong fills, wrong colors) register. Reports per-frame diff% + the bounding box
// of the differing region, tracks the worst frame, and dumps frames over threshold as side-by-side
// PPMs (A=port left | B=oracle right) so the actual bug is visible.
void Sbs::Impl::checkPaneDiff() {
  static int inited = 0;
  if (!inited) {
    inited = 1;
    const char* e = getenv("PSXPORT_SBS_RENDERDIFF");
    if (e && *e) { mRdiffOn = (e[0] != '0'); double v = atof(e); if (v > 0) mRdiffThreshPct = v; }
    // MODE=skip auto-arms the per-frame PICTURE compare (USER 2026-07-10 "per-frame VISUAL compare
    // hook"): this pixel-diff of A's pc_render pane against B's psx_render/oracle pane, tolerance-
    // gated (±40/channel) to absorb PSX-fixed-vs-float dither noise, IS the visual-compare hook —
    // no new mechanism needed, just wiring it on by default for this mode instead of requiring the
    // env var. Covers: STRUCTURAL rendered-picture differences (missing/misplaced geometry, wrong
    // fills/colors) frame over frame, worst-frame tracked, over-threshold frames dumped as side-by-
    // side PPMs to scratch/screenshots/renderdiff/. Does NOT cover: audio (see the SPU-write-log +
    // checkObservables SPU-RAM compare below/above), or any non-visual guest state (the RAM/scratch
    // divergence + rendezvous checks cover that). PSXPORT_SBS_RENDERDIFF=0 explicitly disables even
    // under skip mode if a run needs to ignore known-deferred render bugs while triaging content.
    else if (!e && mMode == M_SKIP) mRdiffOn = true;
  }
  if (!mRdiffOn) return;
  int W = mWa < mWb ? mWa : mWb, H = mHa < mHb ? mHa : mHb;
  if (W < 8 || H < 8) return;                       // no real picture yet
  const int TOL = 40;                               // per-channel tolerance (absorbs dither/rounding)
  long ndiff = 0; int minx = W, miny = H, maxx = -1, maxy = -1;
  for (int y = 0; y < H; y++) {
    const uint8_t* ra = mRgbaA + (size_t)y * mWa * 4;
    const uint8_t* rb = mRgbaB + (size_t)y * mWb * 4;
    for (int x = 0; x < W; x++) {
      int dr = ra[x*4+0]-rb[x*4+0], dg = ra[x*4+1]-rb[x*4+1], db = ra[x*4+2]-rb[x*4+2];
      if (dr<0)dr=-dr; if (dg<0)dg=-dg; if (db<0)db=-db;
      if (dr > TOL || dg > TOL || db > TOL) {
        ndiff++;
        if (x<minx)minx=x; if (x>maxx)maxx=x; if (y<miny)miny=y; if (y>maxy)maxy=y;
      }
    }
  }
  double pct = 100.0 * (double)ndiff / ((double)W * H);
  mRdiffChecked++;
  if (pct > mRdiffWorstPct) { mRdiffWorstPct = pct; mRdiffWorstFrame = mFrame; }
  if (pct >= mRdiffThreshPct) {
    fprintf(stderr, "[renderdiff] f%u %.2f%% pixels differ (port vs oracle) bbox x[%d..%d] y[%d..%d] of %dx%d\n",
            mFrame, pct, minx, maxx, miny, maxy, W, H);
    static int dumped = 0;
    if (dumped < 40) {                              // cap the dump flood
      char path[256];
      snprintf(path, sizeof path, "scratch/screenshots/renderdiff/f%05u_%02d.ppm", mFrame, (int)pct);
      dumpPpm(path);
      dumped++;
    }
  }
}

void Sbs::Impl::grabPane(Game* g, uint8_t* rgba, int* ow, int* oh) {
  if (sbs_nopresent()) { *ow = 4; *oh = 4; return; }
  int sx, sy, w, h; gpu_disp_region(&g->core, &sx, &sy, &w, &h);
  // Widescreen: the engine renders a wider FOV into VRAM columns [sx, sx+nw) — sample the wide
  // width like the standalone present does (else the wide pane is cropped back to 4:3).
  { int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_w(Core*);
    if (gpu_gpu_wide_engine(&g->core)) w = gpu_gpu_wide_engine_w(&g->core); }
  if (w < 1) w = 1; if (h < 1) h = 1;
  if (w > 1024) w = 1024; if (h > 512) h = 512;
  gpu_gpu_render_readback(&g->core, gpu_vram_ptr(&g->core), sx, sy, w, h, rgba);
  // Same per-frame finalize standalone runs in gpu_present (which SBS skips via diff_mode): resets the
  // batch AND this core's s_prim_order / s_seen3d / native depth table / s_frame. Without it those
  // accumulate across frames and corrupt cross-frame draw ordering (semi sea over the fisherman sprite).
  gpu_present_finalize(&g->core);
  *ow = w; *oh = h;
}

// PSXPORT_SBS_KEYS — scripted timed input for HEADLESS repro: "FROM-TO:BTN,FROM-TO:BTN,…" (btn = libpad
// bit name: start/select/cross/circle/square/triangle/up/down/left/right).
uint16_t Sbs::Impl::btnBit(const char* n) const {
  if (!strcmp(n, "start"))    return 0x0008;
  if (!strcmp(n, "select"))   return 0x0001;
  if (!strcmp(n, "cross"))    return 0x4000;
  if (!strcmp(n, "circle"))   return 0x2000;
  if (!strcmp(n, "square"))   return 0x8000;
  if (!strcmp(n, "triangle")) return 0x1000;
  // Real PSX digital-pad bit layout (matches dbg_server.cpp's dbg_btn())
  if (!strcmp(n, "up"))       return 0x0010;
  if (!strcmp(n, "down"))     return 0x0040;
  if (!strcmp(n, "left"))     return 0x0080;
  if (!strcmp(n, "right"))    return 0x0020;
  return 0;
}

void Sbs::Impl::parseKeys() {
  mKeysParsed = true;
  const char* e = getenv("PSXPORT_SBS_KEYS"); if (!e || !*e) return;
  char buf[2048]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
  for (char* p = strtok(buf, ","); p; p = strtok(0, ",")) {
    uint32_t from = 0, to = 0; char name[32] = {0};
    if (sscanf(p, "%u-%u:%31s", &from, &to, name) == 3) {
      uint16_t b = btnBit(name);
      if (b) mKeys.push_back({from, to, b});
    }
  }
  fprintf(stderr, "[sbs] PSXPORT_SBS_KEYS: %zu scripted input ranges\n", mKeys.size());
}

// Feed the SAME host pad mask to BOTH cores (mirrored input). PSXPORT_SBS_KEYS injects timed input.
void Sbs::Impl::feedInput() {
  if (!mKeysParsed) parseKeys();
  uint16_t mask = (uint16_t)sbs_rl_poll_input();
  for (const SbsKey& k : mKeys)
    if (mFrame >= k.from && mFrame <= k.to) mask &= ~k.btn;   // active-low: pressed = bit cleared
  mA->pad.setButtons(mask);
  mB->pad.setButtons(mask);
}

// PSXPORT_SBS_DUMP=path: write the two panes (A left | B right) as ONE side-by-side PPM.
void Sbs::Impl::dumpPpm(const char* path) {
  int H = mHa > mHb ? mHa : mHb; int W = mWa + mWb;
  if (W < 1 || H < 1) return;
  FILE* f = fopen(path, "wb"); if (!f) { fprintf(stderr, "[sbs] dump: cannot open %s\n", path); return; }
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const uint8_t* rgba; int px, pw, ph;
      if (x < mWa) { rgba = mRgbaA; px = x;       pw = mWa; ph = mHa; }
      else         { rgba = mRgbaB; px = x - mWa; pw = mWb; ph = mHb; }
      uint8_t rgb[3] = {0,0,0};
      if (px < pw && y < ph) { const uint8_t* p = rgba + ((size_t)y * pw + px) * 4; rgb[0]=p[0]; rgb[1]=p[1]; rgb[2]=p[2]; }
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
  fprintf(stderr, "[sbs] dumped side-by-side panes (A %dx%d | B %dx%d) -> %s\n", mWa, mHa, mWb, mHb, path);
}

// Write-watch callback. Fired mid-frame by whichever core writes the armed address; capture that core's
// EXACT guest backtrace + value. We DON'T pause here (mid-frame is unsafe) — the lockstep loop pauses
// after both cores finish the frame, with both write sites captured.
// LAST-WRITER map — per-core, per-byte {pc, ra, frame} of the most recent store, recorded on
// EVERY store while SBS runs (the wwatch range is armed over all of RAM+scratchpad at init).
// This is the rewind-free write-site mechanism: native fibers cannot be rewound (their C stacks
// do not restore with the RAM snapshot), so on divergence the map names both cores' writers
// directly. PSXPORT_SBS_LASTWRITER=0 disables (and restores the narrow-range wwatch arming).
void Sbs::Impl::storeCb(Core* c, uint32_t a, uint32_t v, uint32_t w) {
  if (mLwOn) {
    int idx = lwIndex(a);
    if (idx >= 0) {
      LastW* m = (mB && c == &mB->core) ? mLwB : mLwA;
      const LastW rec{c->pc, c->r[31], c->r[29], mFrame};
      for (uint32_t k = 0; k < w && idx + (int)k < (int)LW_N; k++) m[idx + k] = rec;
    }
  }
  // UPPROBE (target-#4 upstream): when a write lands on the configured address (typically the
  // divergent rec+0x0C address, e.g. 0x800F0036), dump c->r[16] (which holds the owning obj address
  // in ov_a00_gen_801337E4) plus obj[+0x42] (arg to FUN_80083F50) and obj[+0x46] (branch gate) on
  // this core. Compare across A and B in the log to name the upstream write cadence.
  static const uint32_t upprobe = []{
    const char* e = getenv("PSXPORT_SBS_UPPROBE"); return e ? (uint32_t)strtoul(e, nullptr, 0) : 0u;
  }();
  if (upprobe && a == upprobe) {
    int which_a = (mB && c == &mB->core) ? 1 : 0;
    uint32_t obj = c->r[16];
    uint16_t f42 = obj ? c->mem_r16(obj + 0x42) : 0;
    uint8_t  f46 = obj ? c->mem_r8 (obj + 0x46) : 0;
    fprintf(stderr, "[upprobe] f%u %c write [%08X]=%08X  obj=%08X obj[+42]=%04X obj[+46]=%02X  r[4]=%08X r[2]=%08X r[3]=%08X ra=%08X\n",
            mFrame, which_a ? 'B' : 'A', a, v, obj, f42, f46, c->r[4], c->r[2], c->r[3], c->r[31]);
  }
  // ALLOCTRACE: sniff writes to 0x800ED098 (free-slot count) — count per-frame decrements per core.
  // Fires INDEPENDENTLY of mWwArmed so it stays live across the whole run without arming a watch.
  // Exact-address check (a == 0x800ED098): word-aligned would count neighboring-byte writes too.
  // BYTETRACE: bucket each store's constituent BYTES over the armed range.
  if (mByteTraceOn && a < mByteTraceHi && a >= mByteTraceLo) {
    int which_a = (mB && c == &mB->core) ? 1 : 0;
    uint32_t ra = c->r[31];
    // The write width is not carried here — the wwatch_check fires per byte/half/word store, and the
    // value has already been widened to uint32 by the caller. Reconstruct the byte-value by peeking
    // pre-store from RAM would be wrong (we don't know if this is the low/high byte). Instead: assume
    // the store is a mem_w8 semantic (record `v & 0xFF` at `a`) — this is correct for byte stores and
    // is a close approximation for wider stores when they only diverge on one byte (the common case
    // here — spawn.cpp stamps use mem_w8 for node[+0/+10/+12], node[+1..+3] are set by beh handlers
    // via mem_w8/w16 which land here byte-at-a-time). Sufficient for the phase-vs-real classification.
    ByteRow& r = mByteTrace[a];
    BytePerCore& pc = which_a ? r.b : r.a;
    pc.vals[(uint8_t)(v & 0xFFu)]++;
    pc.ras[ra]++;
  }
  if (mAllocTraceOn && a == 0x800ED098u) {
    int which_a = (mB && c == &mB->core) ? 1 : 0;
    uint32_t cur = c->mem_r16(0x800ED098u);
    uint32_t next = v & 0xFFFFu;
    // Bucket alloc AND release by guest r[31] so settled-state per-caller compares surface. Recomp
    // preserves r[31] across jal; native record_gate leaves the previous r[31] intact (typically
    // 0xDEAD0000 or a stale value), so its bucket is a mixed-caller lump — expected asymmetry vs B.
    uint32_t ra = c->r[31];
    RaBucket& b = mAllocRa[ra];
    if (next < cur) {   // decrement = allocation
      if (which_a) { mAllocB++; mAllocCumB++; b.allocB++; }
      else         { mAllocA++; mAllocCumA++; b.allocA++; }
    } else if (next > cur) {  // increment = release
      if (which_a) b.relB++; else b.relA++;
    }
  }
  if (!mWwArmed || (a & ~3u) != (mWwAddr & ~3u)) return;
  int which = (mB && c == &mB->core) ? 1 : 0;
  if (which) { mWwVb = v; capBt(c, mWwBtB, sizeof mWwBtB);
               mWwPcB = c->pc; mWwRaB = c->r[31]; mWwSpB = c->r[29]; mWwCountB++;
               mWwHostBtNB = backtrace(mWwHostBtB, WW_HOST_BT_DEPTH); }
  else       { mWwVa = v; capBt(c, mWwBtA, sizeof mWwBtA);
               mWwPcA = c->pc; mWwRaA = c->r[31]; mWwSpA = c->r[29]; mWwCountA++;
               mWwHostBtNA = backtrace(mWwHostBtA, WW_HOST_BT_DEPTH); }
  // Guest a/s registers at the diverging store — the fastest way to compare the two cores'
  // argument chains when the store site is identical but its inputs differ.
  {
    char* bt = which ? mWwBtB : mWwBtA;
    size_t used = strlen(bt), cap = which ? sizeof mWwBtB : sizeof mWwBtA;
    if (used + 220 < cap)
      snprintf(bt + used, cap - used,
               "  [ww-regs] a0=%08X a1=%08X a2=%08X a3=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X\n",
               c->r[4], c->r[5], c->r[6], c->r[7], c->r[16], c->r[17], c->r[18], c->r[19], c->r[20], c->r[21]);
  }
  mWwHit |= (1 << which);
  // PSXPORT_SBS_WW_ONVALUEDIVERGE=1 — instead of pausing on the first PREWATCH fire (which normally
  // treats an asymmetric-but-same-value store as a divergence), pause on the first store that leaves
  // the two cores' post-write byte values DIFFERENT. Ideal for cadence probes where the address
  // (e.g. the RNG state at 0x80105EE8) is written many times a second on both cores with matching
  // values — the interesting moment is when the values first diverge, not the first raw fire.
  static const int only_on_value_diverge = []{ const char* e = getenv("PSXPORT_SBS_WW_ONVALUEDIVERGE"); return e && *e && e[0] != '0' ? 1 : 0; }();
  if (only_on_value_diverge && mWwPersist) {
    // Track per-core: LAST write's host stack, LAST written value, and total-count-this-frame.
    // Frame-boundary code (post-presentPanes) compares counts (& optionally end-of-frame seed
    // values) to detect the FIRST frame where the two cores' cadence diverges. Not per-store
    // comparison — inter-store the seeds would mismatch every advance (naturally), only the
    // end-of-frame state matters.
    // Counts + last-value + backtrace already updated by the block ABOVE (lines 673-678); just
    // return here so the OLD PREWATCH pause-on-first-fire logic is skipped.
    return;
  }
  // PSXPORT_SBS_WW_FROMFRAME=<n> — suppress the persist per-store logging before lockstep frame n.
  // Lets PREWATCH stay armed from boot (so the FIRST divergent frame is fully captured) without
  // writing hundreds of thousands of store lines for the clean frames before it.
  static const uint32_t ww_from_frame = []{ const char* e = getenv("PSXPORT_SBS_WW_FROMFRAME"); return e && *e ? (uint32_t)strtoul(e, 0, 0) : 0u; }();
  if (mWwPersist && mFrame >= ww_from_frame) {  // PREWATCH's continuous logging — per-store attribution to A vs B
    // pc = c->pc (fn entry set by the last wrapper — often STALE, reflecting the last jal-callee).
    // ra = c->r[31] (guest return address) — points into the CALLER just past its jal, so it names
    //      the true call site regardless of stale c->pc. If ra differs A vs B for the same address,
    //      the two cores took different call paths to reach the write — that names the upstream
    //      divergence without another PREWATCH chase.
    // sp = c->r[29] — for the guest-stack backtrace we already dump on real divergence.
    fprintf(stderr, "[sbs-ww] f%u %c wrote [%08X]=%08X (pc=%08X ra=%08X sp=%08X stage=%08X) [c=%p mA=%p mB=%p]\n",
            mFrame, which ? 'B' : 'A', a, v, c->pc, c->r[31], c->r[29], c->mem_r32(0x801fe00c),
            (void*)c, (void*)&mA->core, (void*)&mB->core);
    // t/v/a regs per store: the substrate packet emitters (gen_func_8007FDB0 etc.) keep their
    // prim-walk state in t-regs (t5=r13 prim ptr, t2=r10 pool cursor). Printing them per store lets
    // an offline diff of the A vs B store sequences name the exact prim where the walks diverge.
    fprintf(stderr, "[sbs-ww]     t: v0=%08X v1=%08X t0=%08X t1=%08X t2=%08X t3=%08X t4=%08X t5=%08X t6=%08X t7=%08X a0=%08X a1=%08X a2=%08X a3=%08X\n",
            c->r[2], c->r[3], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], c->r[15],
            c->r[4], c->r[5], c->r[6], c->r[7]);
    // GTE control regs at the store — the packet emitters are pure functions of (prim data, CR
    // rotation+translation, CR projection). When RAM matches but the emit diverges, this is the
    // input that differs. CR0-7 = composed rotation+translation, CR24-30 = OFX/OFY/H/DQA/DQB/ZSF3/ZSF4.
    {
      // gte_read_ctrl reads the CURRENT core's GTE instance — correct here because the store
      // callback runs synchronously inside core c's execution (GTE_CurState is c's).
      uint32_t cr[8], pj[7];
      for (int k = 0; k < 8; k++) cr[k] = gte_read_ctrl(k);
      for (int k = 0; k < 7; k++) pj[k] = gte_read_ctrl(24 + k);
      fprintf(stderr, "[sbs-ww]     gte: cr0-7=%08X %08X %08X %08X %08X %08X %08X %08X  cr24-30=%08X %08X %08X %08X %08X %08X %08X\n",
              cr[0], cr[1], cr[2], cr[3], cr[4], cr[5], cr[6], cr[7],
              pj[0], pj[1], pj[2], pj[3], pj[4], pj[5], pj[6]);
    }
    // Peek AFTER the actual host write, so we see the byte the store LANDED in. (mem_w8 does wwatch_check
    // BEFORE the write, so we peek RIGHT NOW = pre-store, but the write is imminent one-line below.)
    fprintf(stderr, "[sbs-ww]     pre-store peek A[%08X]=%u  B[%08X]=%u\n",
            a, mA->core.mem_r8(a), a, mB->core.mem_r8(a));
    // Guest stack backtrace at write time (walks c->r[29] upward looking for plausible ra values).
    // This is often empty when sp is near stack-top (write reached from a leaf with no callers on the
    // guest stack) — in that case the HOST backtrace below is the useful one.
    const char* gbt = which ? mWwBtB : mWwBtA;
    if (gbt[0]) fprintf(stderr, "[sbs-ww]     guest bt (core %c):\n%s", which ? 'B' : 'A', gbt);
    // Host-side C backtrace — names the actual C function that called mem_w*. This is what pins a
    // NATIVE write vs a SUBSTRATE write (line-105 native_step_frame vs func_XXXX substrate). Even
    // when the guest stack is empty, this is populated (it's the C call stack of the store).
    void** hbt = which ? mWwHostBtB : mWwHostBtA;
    int    nbt = which ? mWwHostBtNB : mWwHostBtNA;
    if (nbt > 0) {
      char** syms = backtrace_symbols(hbt, nbt);
      fprintf(stderr, "[sbs-ww]     host bt (core %c, %d frames):\n", which ? 'B' : 'A', nbt);
      for (int j = 0; j < nbt && j < 12; j++) fprintf(stderr, "        %s\n", syms ? syms[j] : "?");
      free(syms);
    }
  }
}

// `sbs …` debug-server commands. Returns 1 if handled (dbg_exec stops), 0 otherwise.
int Sbs::Impl::dbgCmd(FILE* out, const char* line) {
  char cmd[16] = {0}, sub[32] = {0};
  if (sscanf(line, "%15s", cmd) != 1 || strcmp(cmd, "sbs") != 0) return 0;
  if (!mA) { fprintf(out, "sbs: harness not running (set PSXPORT_SBS=1)\n"); return 1; }
  sscanf(line, "%*s %31s", sub);

  if (!sub[0] || !strcmp(sub, "status")) {
    fprintf(out, "sbs mode=%s frame=%u selected=%c paused=%d\n", modeName(), mFrame, mSel ? 'B' : 'A', mA->dbg_server.isPaused() ? 1 : 0);
    if (mDivFound) fprintf(out, "  divergence: frame %u 0x%08X..0x%08X\n", mDivFrame, mDivAddr, mDivEnd);
    else           fprintf(out, "  divergence: none yet\n");
    if (mWwArmed)  fprintf(out, "  write-watch ARMED on 0x%08X (hit mask=%d)\n", mWwAddr, mWwHit);
  } else if (!strcmp(sub, "diff")) {
    if (!mDivFound) { fprintf(out, "sbs: no divergence yet\n"); return 1; }
    fprintf(out, "divergence @lockstep-frame %u  0x%08X..0x%08X  in %s\n",
            mDivFrame, mDivAddr, mDivEnd, isSpad(mDivAddr) ? "scratchpad" : "main RAM");
    // Detection-time bytes (captured in recordDivergence) — NOT live memory, which after a
    // rewind-and-arm has been restored to the pre-frame state and reads identical A vs B.
    fprintf(out, "  detection-time bytes:\n");
    fprintf(out, "  A:"); for (uint32_t i = 0; i < mDivBytesN; i++) fprintf(out, " %02X", mDivBytesA[i]);
    fprintf(out, "\n  B:"); for (uint32_t i = 0; i < mDivBytesN; i++) fprintf(out, " %02X", mDivBytesB[i]);
    fprintf(out, "\n   :"); for (uint32_t i = 0; i < mDivBytesN; i++) fprintf(out, " %s", mDivBytesA[i] != mDivBytesB[i] ? "^^" : "  ");
    fprintf(out, "\n  live bytes NOW (post-rewind these may match):\n");
    uint32_t end = mDivEnd; if (end > mDivAddr + 64) end = mDivAddr + 64;
    fprintf(out, "  A:"); for (uint32_t x = mDivAddr; x < end; x++) fprintf(out, " %02X", mA->core.mem_r8(x));
    fprintf(out, "\n  B:"); for (uint32_t x = mDivAddr; x < end; x++) fprintf(out, " %02X", mB->core.mem_r8(x));
    fprintf(out, "\n");
    if (mLwOn) for (uint32_t i = 0; i < mDivBytesN; i++)
      if (mDivBytesA[i] != mDivBytesB[i]) { lwReport(mDivAddr + i, out); break; }
  } else if (!strcmp(sub, "lw")) {
    unsigned addr = 0;
    if (sscanf(line, "%*s %*s %x", &addr) != 1 || !addr) { fprintf(out, "usage: sbs lw <hex-addr>\n"); return 1; }
    lwReport(addr, out);
  } else if (!strcmp(sub, "bt")) {
    if (!mDivFound && !mWwHit) { fprintf(out, "sbs: no divergence yet, no write-watch hit yet\n"); return 1; }
    if (mDivFound) {
      fprintf(out, "== core A backtrace (frame-boundary, @divergence) ==\n%s", mBtA);
      fprintf(out, "== core B backtrace (frame-boundary, @divergence) ==\n%s", mBtB);
    }
    if (mWwHit) {
      if (mWwHit & 1) fprintf(out, "== WRITE SITE — core A wrote 0x%08X=%08X ==\n%s", mWwAddr, mWwVa, mWwBtA);
      else            fprintf(out, "== WRITE SITE — core A: no store this frame ==\n");
      if (mWwHit & 2) fprintf(out, "== WRITE SITE — core B wrote 0x%08X=%08X ==\n%s", mWwAddr, mWwVb, mWwBtB);
      else            fprintf(out, "== WRITE SITE — core B: no store this frame ==\n");
    }
  } else if (!strcmp(sub, "watch") || !strcmp(sub, "watchp")) {
    unsigned addr = 0;
    if (sscanf(line, "%*s %*s %x", &addr) != 1) addr = mDivAddr;
    if (!addr) { fprintf(out, "sbs watch: no address (no divergence yet, give one: `sbs watch <hex>`)\n"); return 1; }
    mWwAddr = addr; mWwArmed = true; mWwHit = 0; mWwBtA[0] = mWwBtB[0] = 0;
    // watchp = PERSIST watch: log EVERY store (with regs) to stderr instead of pausing on the first
    // fire — the manual-arm equivalent of rewindAndArm's continuous logging, for when the rewind is
    // unavailable (fiber live). Diff the A/B store sequences offline to find the first divergent one.
    mWwPersist = (sub[5] == 'p');
    mA->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    mB->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    fprintf(out, "write-watch armed on 0x%08X%s — `sbs resume`; %s\n", addr,
            mWwPersist ? " (persist)" : "",
            mWwPersist ? "every store logs to stderr with regs." : "the diverging write will re-pause with the site.");
  } else if (!strcmp(sub, "show")) {
    char w = 0; sscanf(line, "%*s %*s %c", &w);
    if (w == 'b' || w == 'B') mSel = 1; else if (w == 'a' || w == 'A') mSel = 0;
    fprintf(out, "selected core %c (window + r/rw/ents target)\n", mSel ? 'B' : 'A');
  } else if (!strcmp(sub, "resume") || !strcmp(sub, "play")) {
    mA->dbg_server.setPaused(false); fprintf(out, "resumed\n");
  } else if (!strcmp(sub, "step")) {
    unsigned n = 0; sscanf(line, "%*s %*s %u", &n); if (!n) n = 1;
    mA->dbg_server.addStep((int)n);
    fprintf(out, "step +%u\n", n);
  } else if (!strcmp(sub, "dump")) {
    char path[256] = {0}; if (sscanf(line, "%*s %*s %255s", path) != 1) snprintf(path, sizeof path, "scratch/screenshots/sbs.ppm");
    dumpPpm(path); fprintf(out, "dumped side-by-side panes -> %s\n", path);
  } else if (!strcmp(sub, "ramdiff")) {
    unsigned cap = 0; sscanf(line, "%*s %*s %u", &cap); if (!cap) cap = 24;
    const uint8_t* a = mA->core.ram + (mLo - 0x80000000u);
    const uint8_t* b = mB->core.ram + (mLo - 0x80000000u);
    uint32_t n = mHi - mLo, spans = 0, bytes = 0, listed = 0;
    for (uint32_t i = 0; i < n; ) {
      if (a[i] != b[i]) {
        uint32_t start = mLo + i;
        while (i < n && a[i] != b[i]) { i++; bytes++; }
        spans++;
        if (listed++ < cap)
          fprintf(out, "  RAM  0x%08X..0x%08X (%u B)  A=%02X%02X%02X%02X B=%02X%02X%02X%02X\n",
                  start, mLo + i, (mLo + i) - start,
                  mA->core.mem_r8(start), mA->core.mem_r8(start+1), mA->core.mem_r8(start+2), mA->core.mem_r8(start+3),
                  mB->core.mem_r8(start), mB->core.mem_r8(start+1), mB->core.mem_r8(start+2), mB->core.mem_r8(start+3));
      } else i++;
    }
    uint32_t sspans = 0, sbytes = 0;
    for (uint32_t i = 0; i < 0x400; ) {
      if (mA->core.scratch[i] != mB->core.scratch[i]) {
        uint32_t start = 0x1F800000u + i;
        while (i < 0x400 && mA->core.scratch[i] != mB->core.scratch[i]) { i++; sbytes++; }
        sspans++;
        if (listed++ < cap)
          fprintf(out, "  SPAD 0x%08X..0x%08X (%u B)\n", start, 0x1F800000u + i, (0x1F800000u + i) - start);
      } else i++;
    }
    fprintf(out, "sbs ramdiff @frame %u: RAM %u spans / %u B diverge (region 0x%08X..0x%08X), "
                 "scratchpad %u spans / %u B. A=PC(native) B=PSX(full-recomp).\n",
            mFrame, spans, bytes, mLo, mHi, sspans, sbytes);
  } else if (!strcmp(sub, "allocra")) {
    dumpAllocRa(out);
  } else if (!strcmp(sub, "bytetrace")) {
    dumpByteTrace(out);
  } else {
    fprintf(out, "sbs subcommands: status | diff | bt | watch [hex] | show a|b | resume | step [n] | dump [path] | ramdiff [N] | allocra | bytetrace\n");
  }
  return 1;
}

// Settled-state per-ra bucket dump — the workflow-first fix for +N-alloc attribution. Compares
// per-caller-ra alloc+release COUNTS over the whole run (not ordinal-point-in-time), so a timing-shifted
// caller that fires on both cores in different frames shows up as SYMMETRIC (delta=0), and a real
// caller-side divergence shows up as ASYMMETRIC. Sorts by |A-B| net; hides symmetric rows unless
// PSXPORT_SBS_ALLOCRA_ALL=1. Called at end-of-run (SBS AUTONAV loop exit) and by REPL `sbs allocra`.
void Sbs::Impl::dumpAllocRa(FILE* out) {
  if (!mAllocTraceOn) { fprintf(out, "sbs allocra: PSXPORT_SBS_ALLOCTRACE=1 required to collect buckets\n"); return; }
  bool showAll = false;
  { const char* e = getenv("PSXPORT_SBS_ALLOCRA_ALL"); if (e && *e && strcmp(e, "0") != 0) showAll = true; }
  std::vector<std::pair<uint32_t, RaBucket>> v(mAllocRa.begin(), mAllocRa.end());
  std::sort(v.begin(), v.end(), [](const std::pair<uint32_t, RaBucket>& x, const std::pair<uint32_t, RaBucket>& y){
    int nx = std::abs((x.second.allocA - x.second.allocB) - (x.second.relA - x.second.relB));
    int ny = std::abs((y.second.allocA - y.second.allocB) - (y.second.relA - y.second.relB));
    return nx > ny;
  });
  int totA=0, totB=0;
  for (const auto& kv : v) { totA += kv.second.allocA; totB += kv.second.allocB; }
  fprintf(out, "[sbs allocra] ra buckets on 0x800ED098 stores (settled-state, %zu unique ra's)\n"
               "              totalA=%d totalB=%d net=%+d\n"
               "              ra=DEAD0000 (A-only) = native record_gate — the previous r[31] wasn't set by a JAL to the alloc.\n"
               "%s\n"
               "%12s  %7s %7s  %6s %6s   %s\n",
          v.size(), totA, totB, totA - totB,
          showAll ? "(all rows shown)" : "(SYMMETRIC rows hidden — set PSXPORT_SBS_ALLOCRA_ALL=1 to show)",
          "ra", "A_alloc", "B_alloc", "A_rel", "B_rel", "net(A-B):alloc,rel");
  for (const auto& kv : v) {
    uint32_t ra = kv.first;
    const RaBucket& b = kv.second;
    int da = b.allocA - b.allocB;
    int dr = b.relA - b.relB;
    if (!showAll && da == 0 && dr == 0) continue;
    fprintf(out, "  0x%08X  %7d %7d  %6d %6d   %+8d, %+d\n", ra, b.allocA, b.allocB, b.relA, b.relB, da, dr);
  }
}

// BYTETRACE settled-state classifier: for each recorded byte in [mByteTraceLo, mByteTraceHi),
// decide whether A vs B match at SETTLED STATE. Two-class outcome per byte:
//   PHASE : identical (value → count) maps on both cores + (optionally) live RAM differs. The only
//           asymmetry is the SNAPSHOT phase — one core is at value X while the other is at value Y,
//           but both cores VISIT the same values with the same counts over the run. Filterable noise.
//   REAL  : (value → count) maps differ — some value has A_count != B_count, or one core wrote a
//           value the other never did. Genuine port gap. Needs decomp / code-level attribution.
// Emits two sections: (1) per-byte classification (skips CLEAN bytes = same live RAM + same maps),
// (2) REAL bytes as concrete investigation targets. Env PSXPORT_SBS_BYTETRACE_ALL=1 shows CLEAN
// bytes too (usually noise). PHASE/SOFT bytes are still classified so a bytetrace pass surfaces
// their pattern, but they are NOT emitted as noise-filter suggestions — every diverging byte is
// investigation-worthy (see no_residual_ram_diverges).
void Sbs::Impl::dumpByteTrace(FILE* out) {
  if (!mByteTraceOn) { fprintf(out, "sbs bytetrace: PSXPORT_SBS_BYTETRACE=<lo>,<hi> required\n"); return; }
  bool showAll = false;
  { const char* e = getenv("PSXPORT_SBS_BYTETRACE_ALL"); if (e && *e && strcmp(e, "0") != 0) showAll = true; }
  fprintf(out, "[sbs bytetrace] range=0x%08X..0x%08X  recorded %zu unique byte addresses (settled state)\n",
          mByteTraceLo, mByteTraceHi, mByteTrace.size());
  // Pass 1 — classify each byte address. Four buckets on the value-set + counts:
  //   CLEAN      live_A==live_B AND identical value→count histograms → nothing to see.
  //   PHASE      exactly identical value→count histograms + live differs → snapshot phase only.
  //   SOFT-PHASE value SETS match; per-value counts differ by ≤ tolerance (default: min(2, 5% of max)).
  //              This is the 1-tick-off-by-one residual class — real per-execution count difference
  //              but too small to be a code-path divergence (both cores visit the same values, one
  //              just did it a couple more times). Filterable as noise for the same reason PHASE is.
  //   REAL       value sets differ OR counts differ by more than tolerance → real port gap.
  // Tolerance is per-value: |ca - cb| ≤ max(2, max(ca,cb)/20). Env PSXPORT_SBS_BYTETRACE_STRICT=1
  // forces strict PHASE/REAL split (no SOFT class) for a conservative one-shot audit.
  enum Cls { CLEAN=0, PHASE=1, SOFT=2, REAL=3 };
  bool strict = false;
  { const char* e = getenv("PSXPORT_SBS_BYTETRACE_STRICT"); if (e && *e && strcmp(e, "0") != 0) strict = true; }
  auto classify_soft = [&](const BytePerCore& A, const BytePerCore& B)->bool {
    // Both value SETS must match (else it's a REAL divergence).
    if (A.vals.size() != B.vals.size()) return false;
    for (const auto& kv : A.vals) if (!B.vals.count(kv.first)) return false;
    // Per-value counts within tolerance.
    for (const auto& kv : A.vals) {
      uint32_t ca = kv.second;
      uint32_t cb = B.vals.at(kv.first);
      uint32_t hi = ca > cb ? ca : cb;
      uint32_t tol = hi / 20 > 2 ? hi / 20 : 2;
      if ((ca > cb ? ca - cb : cb - ca) > tol) return false;
    }
    return true;
  };
  std::map<uint32_t, Cls> classification;
  int nClean = 0, nPhase = 0, nSoft = 0, nReal = 0;
  for (const auto& kv : mByteTrace) {
    uint32_t a = kv.first;
    const ByteRow& r = kv.second;
    uint8_t ra_live = (a & 0x1FFFFFFF) < 0x200000 ? mA->core.mem_r8(a) : 0;
    uint8_t rb_live = (a & 0x1FFFFFFF) < 0x200000 ? mB->core.mem_r8(a) : 0;
    bool live_eq = (ra_live == rb_live);
    bool vals_eq = (r.a.vals == r.b.vals);
    Cls c;
    if (live_eq && vals_eq)                        c = CLEAN;
    else if (vals_eq && !live_eq)                  c = PHASE;
    else if (!strict && classify_soft(r.a, r.b))   c = SOFT;
    else                                           c = REAL;
    classification[a] = c;
    if      (c == CLEAN) nClean++;
    else if (c == PHASE) nPhase++;
    else if (c == SOFT)  nSoft++;
    else                 nReal++;
  }
  fprintf(out, "               classified: %d CLEAN, %d PHASE, %d SOFT-PHASE, %d REAL  (strict=%d)\n",
          nClean, nPhase, nSoft, nReal, strict ? 1 : 0);
  // Section 1 — per-byte lines (hide CLEAN unless _ALL).
  fprintf(out, "%s\n%12s  %-5s  %-8s  %-8s  %s\n",
          showAll ? "(CLEAN rows shown)" : "(CLEAN rows hidden — set PSXPORT_SBS_BYTETRACE_ALL=1)",
          "addr", "class", "live_A", "live_B", "note");
  for (const auto& kv : classification) {
    uint32_t a = kv.first;
    Cls c = kv.second;
    if (c == CLEAN && !showAll) continue;
    uint8_t ra_live = (a & 0x1FFFFFFF) < 0x200000 ? mA->core.mem_r8(a) : 0;
    uint8_t rb_live = (a & 0x1FFFFFFF) < 0x200000 ? mB->core.mem_r8(a) : 0;
    const char* cls = c == CLEAN ? "clean" : c == PHASE ? "PHASE" : c == SOFT ? "SOFT" : "REAL";
    const ByteRow& r = mByteTrace[a];
    char note[256] = {0};
    if (c == PHASE) {
      // Summarize the shared value set (up to 3 values).
      int shown = 0; size_t p = 0;
      for (const auto& vv : r.a.vals) {
        if (shown++ >= 3) break;
        p += snprintf(note + p, sizeof(note) - p, "%s0x%02X×%u", shown > 1 ? " " : "", vv.first, vv.second);
      }
      if (r.a.vals.size() > 3) snprintf(note + p, sizeof(note) - p, " …");
    } else if (c == REAL) {
      // Find the first value with a different A vs B count (or a one-sided value).
      std::set<uint8_t> allv;
      for (const auto& vv : r.a.vals) allv.insert(vv.first);
      for (const auto& vv : r.b.vals) allv.insert(vv.first);
      for (uint8_t v : allv) {
        uint32_t ca = r.a.vals.count(v) ? r.a.vals.at(v) : 0;
        uint32_t cb = r.b.vals.count(v) ? r.b.vals.at(v) : 0;
        if (ca != cb) { snprintf(note, sizeof note, "val=0x%02X  A×%u  B×%u", v, ca, cb); break; }
      }
    }
    fprintf(out, "  0x%08X  %-5s  0x%02X      0x%02X      %s\n", a, cls, ra_live, rb_live, note);
  }
  // Section 2 — REAL bytes as concrete investigation targets. Summarize with the TOP 3 divergent
  // (val, A, B) rows sorted by |A-B|, plus a shape hint that names the asymmetry class:
  //   ONE-SIDED  A writes values B never wrote (or vice versa) — sharpest signal for a real bug.
  //   SKEWED     both cores write the same values but with wildly different counts (>2x).
  //   MIXED      distributions overlap with mild asymmetry.
  // (Detailed per-ra breakdown is available in the raw wwatch log via PSXPORT_WWATCH on the byte.)
  fprintf(out, "\n[sbs bytetrace] REAL bytes (concrete port-gap targets to decomp):\n");
  int reals = 0;
  for (const auto& kv : classification) {
    if (kv.second != REAL) continue;
    uint32_t a = kv.first;
    const ByteRow& r = mByteTrace[a];
    uint8_t ra_live = (a & 0x1FFFFFFF) < 0x200000 ? mA->core.mem_r8(a) : 0;
    uint8_t rb_live = (a & 0x1FFFFFFF) < 0x200000 ? mB->core.mem_r8(a) : 0;
    // Union of value sets + counts.
    std::set<uint8_t> allv;
    for (const auto& vv : r.a.vals) allv.insert(vv.first);
    for (const auto& vv : r.b.vals) allv.insert(vv.first);
    // Classify shape.
    int oneSided = 0, both = 0;
    uint32_t totA = 0, totB = 0;
    for (uint8_t v : allv) {
      uint32_t ca = r.a.vals.count(v) ? r.a.vals.at(v) : 0;
      uint32_t cb = r.b.vals.count(v) ? r.b.vals.at(v) : 0;
      totA += ca; totB += cb;
      if ((ca > 0) != (cb > 0)) oneSided++; else if (ca > 0 && cb > 0) both++;
    }
    const char* shape = "MIXED";
    if (oneSided > 0 && both == 0) shape = "ONE-SIDED";
    else if (oneSided > both)      shape = "ONE-SIDED";
    else if (totA > 0 && totB > 0 && (totA > 2*totB || totB > 2*totA)) shape = "SKEWED";
    // Top 3 rows by |A-B|.
    std::vector<std::tuple<int, uint8_t, uint32_t, uint32_t>> rows;
    for (uint8_t v : allv) {
      uint32_t ca = r.a.vals.count(v) ? r.a.vals.at(v) : 0;
      uint32_t cb = r.b.vals.count(v) ? r.b.vals.at(v) : 0;
      if (ca != cb) rows.push_back({std::abs((int)ca - (int)cb), v, ca, cb});
    }
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y){ return std::get<0>(x) > std::get<0>(y); });
    fprintf(out, "  0x%08X  live A=0x%02X B=0x%02X  [%s]  totA=%u totB=%u  top:",
            a, ra_live, rb_live, shape, totA, totB);
    for (size_t i = 0; i < 3 && i < rows.size(); i++)
      fprintf(out, "  val=0x%02X A=%u B=%u", std::get<1>(rows[i]), std::get<2>(rows[i]), std::get<3>(rows[i]));
    fprintf(out, "\n");
    // Top-RA on each side — the concrete decomp target. RA is the guest r[31] at write-time
    // (jal delay-slot successor of the caller), which points inside the caller function body.
    auto topRas = [&](const std::map<uint32_t,uint32_t>& ras) {
      std::vector<std::pair<uint32_t,uint32_t>> v(ras.begin(), ras.end());
      std::sort(v.begin(), v.end(), [](auto& x, auto& y){ return x.second > y.second; });
      return v;
    };
    auto ra_a = topRas(r.a.ras), ra_b = topRas(r.b.ras);
    fprintf(out, "                A-ras:");
    for (size_t i = 0; i < 3 && i < ra_a.size(); i++) fprintf(out, " 0x%08X×%u", ra_a[i].first, ra_a[i].second);
    if (ra_a.empty()) fprintf(out, " (none)");
    fprintf(out, "\n                B-ras:");
    for (size_t i = 0; i < 3 && i < ra_b.size(); i++) fprintf(out, " 0x%08X×%u", ra_b[i].first, ra_b[i].second);
    if (ra_b.empty()) fprintf(out, " (none)");
    fprintf(out, "\n");
    if (++reals >= 20) { fprintf(out, "  … (%d more REAL bytes; scope your BYTETRACE range tighter)\n", nReal - reals); break; }
  }
}

void Sbs::Impl::run(const char* exePath, Sbs* facade) {
  watchdog_disable();   // the SBS pauses indefinitely on a divergence for live inspection — not a hang

  // Mode selection (PSXPORT_SBS_MODE)
  const char* m = getenv("PSXPORT_SBS_MODE");
  if (m) {
    if (!strcmp(m, "gameplay"))               mMode = M_GAMEPLAY;
    else if (!strcmp(m, "full") || !strcmp(m, "both")) mMode = M_FULL;   // "both" = legacy alias
    else if (!strcmp(m, "oracle"))            mMode = M_ORACLE;
    else if (!strcmp(m, "skip"))              mMode = M_SKIP;
    else                                       mMode = M_RENDER;
  }
  { const char* e = getenv("PSXPORT_SBS_LO"); if (e && *e) mLo = (uint32_t)strtoul(e, 0, 0); }
  { const char* e = getenv("PSXPORT_SBS_HI"); if (e && *e) mHi = (uint32_t)strtoul(e, 0, 0); }

  // Mark the harness active (native_fmv/native_boot gate off this). The per-core write-watch
  // callback is installed on each Core right after the two Games are created below.
  mSbs = true;

  { const char* e = getenv("PSXPORT_SBS_BYTETRACE");
    if (e && *e) {
      unsigned long lo=0, hi=0;
      if (sscanf(e, "%lx,%lx", &lo, &hi) == 2 && hi > lo) {
        mByteTraceOn = 1; mByteTraceLo = (uint32_t)lo; mByteTraceHi = (uint32_t)hi;
        fprintf(stderr, "[sbs] BYTETRACE on — per-byte value+ra bucketing over 0x%08X..0x%08X (settled-state classifier at exit)\n",
                mByteTraceLo, mByteTraceHi);
      } else {
        fprintf(stderr, "[sbs] BYTETRACE: bad range '%s' (want <lo>,<hi>, hex, e.g. 0x800EE0DC,0x800EE10D)\n", e);
      }
    }
  }
  { const char* e = getenv("PSXPORT_SBS_ALLOCTRACE"); if (e && *e && strcmp(e, "0") != 0) mAllocTraceOn = 1; }
  if (mAllocTraceOn)
    fprintf(stderr, "[sbs] ALLOCTRACE on — per-frame decrement count of 0x800ED098 logged when A != B\n");
  {
    // Unified atexit/SIGTERM/SIGINT dump registration — ONE handler per signal for the whole
    // harness, not one per feature. Each opt-in diagnostic (ALLOCTRACE/BYTETRACE) plus the
    // MODE=skip rendezvous-site audit (see sbs.h skipRendezvousReached / dumpRendezvousSites)
    // registers its own "am I on" flag and dump function here so a run with several diagnostics
    // active at once (or killed by `timeout` mid-run) gets every one of them, instead of the last
    // std::signal() call silently replacing all the earlier installs (that WAS the shape here
    // before this dump was added — SANCTIONED ATEXIT/SIGNAL EXCEPTION: atexit/signal handlers take
    // no context, so the live Sbs::Impl is reachable only through this one static pointer).
    static Sbs::Impl* s_self = nullptr;
    s_self = this;
    static void (*dumpAll)() = +[]{
      if (!s_self) return;
      if (s_self->mAllocTraceOn || s_self->mByteTraceOn) {
        if (!s_self->mAllocRaDumped) { s_self->mAllocRaDumped = 1; s_self->dumpAllocRa(stderr); s_self->dumpByteTrace(stderr); }
      }
      if (s_self->mMode == M_SKIP) {
        if (!s_self->mRvDumped) { s_self->mRvDumped = true; s_self->dumpRendezvousSites(stderr); }
      }
    };
    atexit(dumpAll);
    static bool s_sigHooked = false;
    if (!s_sigHooked) {
      s_sigHooked = true;
      auto handler = +[](int sig){
        dumpAll();
        fflush(stderr);
        _exit(128 + sig);
      };
      std::signal(SIGTERM, handler);
      std::signal(SIGINT,  handler);
    }
  }

  // psx_fallback per mode: gameplay/full run PSX gameplay on core B; render runs native gameplay on both;
  // oracle runs the PURE interpreter+soft-rasterizer oracle on B (docs/oracle.md).
  int fb_b = (mMode == M_RENDER) ? 0 : 1;
  // Core A is HARD-WIRED to pc_skip=false (USER 2026-07-07): the SBS harness IS the strict
  // pc_faithful oracle compare — Core A runs the ported native faithful path (fiber stage
  // bodies, ported primitives; scheduler stanzas gate their pc_skip flavors off), Core B the
  // pure substrate. No flag: strictness is the mode. (The old note claiming pc_skip=false
  // "routes every task to the fiber substrate" described the REJECTED fiber-only design; under
  // the faithful-execution model pc_skip=false runs NATIVE bodies — docs/faithful-execution.md.)
  // M_SKIP (USER 2026-07-08, frame-aligned 2026-07-10): core A runs the REAL default config
  // (pc_skip=true, the ./run.sh path) against the recomp oracle on core B, compared on OBSERVABLE
  // OUTPUT state (checkObservables). Each pc_skip fork that collapses a multi-frame substrate load
  // into one native call is expected to call skipRendezvousReached() (sbs.h) after doing its own
  // (harmless, host-only) work but BEFORE flipping any guest-visible "load complete" state, so A
  // idles rather than racing B at the same lockstep frame index — see docs/findings/sbs.md "SKIP-
  // mode frame alignment" for which forks are wired vs still-TODO. Because of that barrier,
  // checkObservables no longer needs a settled-divergence tolerance (kObsPersist=1, strict per-
  // frame) for content downstream of a WIRED fork; a fork this pass hasn't wired yet can still
  // legitimately drift and will show up honestly as a divergence rather than being silently masked.
  // All other modes keep core A hard-wired to pc_faithful strict (unaffected by any of this).
  mA = new Game(); mA->psx_fallback = 0;     mA->sbs = facade; mA->pc_skip = (mMode == M_SKIP);
  mB = new Game(); mB->psx_fallback = fb_b;  mB->sbs = facade; mB->pc_skip = false;
  // Per-Game enhancement state (mods + oracle are Game members now, not process globals):
  // - Core A keeps the USER'S mods (loaded from the settings file in the Game ctor) — pane A must
  //   look exactly like `PSXPORT_PC_SKIP=0 ./run.sh` (USER 2026-07-10), widescreen included. The
  //   one guest-poking enhancement (the widescreen cull re-include) is suppressed under SBS at its
  //   site (cull.cpp reads game->sbs) so guest evolution stays byte-identical to core B; its only
  //   cost is margin pop-in for dynamic entities in wide panes. fps60 presentation is inert under
  //   diff_mode (panes sample real frames).
  // - Core B gets the full oracle pin (Game::setOracle) whenever it runs the substrate (fb_b) —
  //   the SAME per-Game config as standalone `PSXPORT_ORACLE=1 ./run.sh`, so pane B IS the oracle
  //   picture (render_observer/billboard/painter/wide gates all read game->oracle per core).
  if (fb_b) mB->setOracle(); else mB->mods.forceNeutral();
  fprintf(stderr, "[sbs] per-core config: A = user mods (aspect=%d fps60=%d), B %s\n",
          mA->mods.aspect, mA->mods.fps60,
          fb_b ? "ORACLE (recomp + neutral mods, game->oracle=1)" : "mods=neutral");
  mA->core.storeWatchCb = &Sbs::storeCb;     // write-watch trampoline (fires only once wwatch_arm'd)
  mB->core.storeWatchCb = &Sbs::storeCb;
  { // last-writer map: on by default (PSXPORT_SBS_LASTWRITER=0 disables). Arms the wwatch range
    // over ALL of RAM+scratchpad so every store reaches storeCb; the narrow wwatch filters by
    // mWwAddr inside storeCb, so PREWATCH/rewind behavior is unchanged.
    const char* lw = getenv("PSXPORT_SBS_LASTWRITER");
    mLwOn = !(lw && *lw && lw[0] == '0');
    if (mLwOn) {
      mLwA = new LastW[LW_N];
      mLwB = new LastW[LW_N];
      mA->core.wwatch_arm(0x00000000u, 0x1F800000u + LW_SPAD);
      mB->core.wwatch_arm(0x00000000u, 0x1F800000u + LW_SPAD);
    }
  }
  // Scratch mask (see isPcSkipScratch above): apply ONLY under pc_skip=true (shortcut branches).
  // Under pc_skip=false native FAITHFUL branches target byte-exact — mask off, no residuals.
  mPcSkipMask = false;   // strict mode: no scratch mask ever (core A is always pc_faithful)
  // Allocate per-Core SPU-write logs so audio-relevant divergences (Issue #29) surface as
  // register-write drift, not just RAM byte drift. Bound by spu_bind on every frame step.
  mA->spu.writeLog = spu_new_log();
  mB->spu.writeLog = spu_new_log();
  fprintf(stderr, "[sbs] core A pc_faithful (hard-wired): native faithful path, byte-exact strict "
                  "— B recomp is the oracle\n");
  if (mMode == M_ORACLE) { mB->core.use_interp = 1; mB->gpu.soft_gpu = 1; }
  load_exe(exePath, &mA->core); dc_boot_init(&mA->core);
  load_exe(exePath, &mB->core); dc_boot_init(&mB->core);
  fprintf(stderr, "[sbs] core-map A=%p B=%p (use to attribute [wwatch] lines)\n",
          (void*)&mA->core, (void*)&mB->core);

  // BOOT-SYNC CHECK: both cores just loaded the same MAIN.EXE and ran dc_boot_init. Their RAM +
  // scratchpad should be BYTE-IDENTICAL at this point — anything else means the boot code itself
  // diverges (a native boot step vs its recomp does something different, before autonav even
  // starts). Report the first N differences so we can fix them, but DO NOT force sync (per user
  // directive: verify, don't paper over). If different, downstream divergence hunts are chasing
  // shadows — fix the boot divergence first.
  {
    int nDiff = 0, nSpad = 0, firstAddr = -1;
    for (uint32_t a = 0; a < 0x200000; a++) {
      if (mA->core.ram[a] != mB->core.ram[a]) {
        if (firstAddr < 0) firstAddr = (int)a;
        if (nDiff < 8) fprintf(stderr, "[sbs] BOOT-DIFF main 0x%08X: A=%02X B=%02X\n",
                               0x80000000u + a, mA->core.ram[a], mB->core.ram[a]);
        nDiff++;
      }
    }
    for (uint32_t i = 0; i < 0x400; i++) {
      if (mA->core.scratch[i] != mB->core.scratch[i]) {
        if (nSpad < 8) fprintf(stderr, "[sbs] BOOT-DIFF spad 0x%08X: A=%02X B=%02X\n",
                               0x1F800000u + i, mA->core.scratch[i], mB->core.scratch[i]);
        nSpad++;
      }
    }
    if (nDiff || nSpad) {
      fprintf(stderr, "[sbs] *** BOOT DIVERGENCE: %d RAM bytes, %d spad bytes differ AT BOOT (first RAM addr 0x%08X). "
                      "Downstream analysis is unreliable until this is fixed. ***\n",
              nDiff, nSpad, 0x80000000u + firstAddr);
    } else {
      fprintf(stderr, "[sbs] BOOT sync verified: RAM + scratchpad byte-identical at boot start.\n");
    }
  }

  // ALLOCTRACE/BYTETRACE arm — after Cores exist. wwatch_check only fires the store callback for
  // armed addresses. Compose a single covering range: if BYTETRACE is on we use its range and, when
  // ALLOCTRACE is also on, extend so 0x800ED098 falls inside. If only ALLOCTRACE is on we arm just
  // the 0x800ED098 word. storeCb filters by exact address so overshoot has zero effect other than
  // more callback calls.
  if (mAllocTraceOn || mByteTraceOn) {
    uint32_t lo = mByteTraceOn ? mByteTraceLo : (0x800ED098u & ~3u);
    uint32_t hi = mByteTraceOn ? mByteTraceHi : ((0x800ED098u & ~3u) + 4);
    if (mAllocTraceOn) {
      if (0x800ED098u < lo) lo = 0x800ED098u & ~3u;
      if (0x800ED09Cu > hi) hi = 0x800ED09Cu;
    }
    mA->core.wwatch_arm(lo, hi);
    mB->core.wwatch_arm(lo, hi);
  }

  // PSXPORT_SBS_PREWATCH=<hex> — arm SBS write-watch at boot so the FIRST divergent store to the
  // address is caught, not the first store AFTER the frame-boundary divergence pause (which happens
  // one frame late — you can never watch a write that already happened). Fires from frame 0.
  if (const char* w = getenv("PSXPORT_SBS_PREWATCH"); w && *w) {
    uint32_t addr = (uint32_t)strtoul(w, 0, 0);
    // Kernelize so scratchpad (0x1F80xxxx) and main-RAM addrs both match wwatch_check's kernelized store.
    mWwAddr = addr | 0x80000000u; mWwArmed = true; mWwPersist = true; mWwHit = 0; mWwBtA[0] = mWwBtB[0] = 0;
    mA->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    mB->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    fprintf(stderr, "[sbs] PREWATCH armed at boot on 0x%08X — pauses at end of the first frame with a DIVERGENT store.\n", addr);
  }

  sbs_rl_init();

  fprintf(stderr, "[sbs] LIVE side-by-side: mode=%s  A=%s  B=%s  diff region 0x%08X..0x%08X + scratchpad\n",
          modeName(),
          mMode == M_RENDER ? "native-gp/native-render" : mMode == M_GAMEPLAY ? "native-gp/PSX-render" : "FULL native",
          mMode == M_RENDER ? "native-gp/PSX-render"    : mMode == M_GAMEPLAY ? "PSX-gp/PSX-render"   :
          mMode == M_ORACLE ? "PURE-ORACLE(interp+softGPU)" : "FULL PSX",
          mLo, mHi);

  mHaveDbgsrv = cfg_on("PSXPORT_DEBUG_SERVER") != 0;
  mA->dbg_server.start(&mA->core);

  // Concurrent boot to gameplay-start (both cores lockstep, one frame per iteration, both panes present
  // every frame). YOU drive both cores from frame 0 by default; opt into AUTO-NAV with PSXPORT_SBS_AUTONAV=1.
  const char* sbs_autonav_env = getenv("PSXPORT_SBS_AUTONAV");
  const bool  sbsAutonav = sbs_autonav_env && *sbs_autonav_env && strcmp(sbs_autonav_env, "0") != 0;
  const char* sbsDumpPath = getenv("PSXPORT_SBS_DUMP");
  bool dumped = false;
  fprintf(stderr, "[sbs] %s — then drive both panes with the window keyboard (WASD/arrows, K=Cross, "
                  "Enter=Start, …) or the debug server; inspect via `sbs` cmds.\n",
          sbsAutonav ? "AUTO-NAV to the field" : "LOCKSTEP from boot (no auto-nav)");

  for (;;) {
    if (sbs_rl_should_close()) { fprintf(stderr, "[sbs] window closed — exiting.\n"); break; }
    Core* sel = mSel ? &mB->core : &mA->core;
    DbgServer& dbg = mA->dbg_server;   // one endpoint per process; mA owns it
    // TRACE: pre-service state
    const bool ww_trace_ext = mWwArmed && mWwAddr == 0x800BF81Eu && mFrame >= 180 && mFrame <= 200;
    if (ww_trace_ext)
      fprintf(stderr, "[sbs-trace] f%u pre-service     A[0x800BF81E]=%u  B[0x800BF81E]=%u\n",
              mFrame, mA->core.mem_r8(0x800BF81Eu), mB->core.mem_r8(0x800BF81Eu));
    dbg.service(sel);
    if (ww_trace_ext)
      fprintf(stderr, "[sbs-trace] f%u post-service    A[0x800BF81E]=%u  B[0x800BF81E]=%u\n",
              mFrame, mA->core.mem_r8(0x800BF81Eu), mB->core.mem_r8(0x800BF81Eu));
    bool nav_done = !sbsAutonav || (mNavA.phase == DONE && mNavB.phase == DONE);
    // PSXPORT_SBS_POSTDRIVE=1 / PSXPORT_SBS_AUTONAV=combat: keep calling navStep() past nav_done too —
    // its DONE case is where the post-control walk/jump SCRIPT lives (Nav::DONE below). Without this,
    // nav_done short-circuits the dispatch straight to feedInput() (host keyboard) forever and the
    // DONE-phase script never runs — it was DEAD CODE before the 2026-07-08 postdrive fix, and the
    // SAME bug class bit the combat leg on introduction (2026-07-10: sbsCombatOn() was missing from
    // this condition, so the combat script in Nav::DONE never ran despite being wired). Fall back to
    // feedInput() (live keyboard/debug-server driving) when neither is on, preserving that path.
    if (!nav_done || sbsPostdriveOn() || sbsCombatOn()) { navStep(&mA->core, mNavA, mFrame, "A"); navStep(&mB->core, mNavB, mFrame, "B"); }
    else feedInput();
    if (ww_trace_ext)
      fprintf(stderr, "[sbs-trace] f%u post-nav/input  A[0x800BF81E]=%u  B[0x800BF81E]=%u\n",
              mFrame, mA->core.mem_r8(0x800BF81Eu), mB->core.mem_r8(0x800BF81Eu));
    if (dbg.isPaused() && !dbg.stepPending()) {
      presentPanes();
      usleep(15000);
      continue;
    }
    if (dbg.stepPending()) dbg.consumeStep();

    // ATTACK-(a) trace: log stage/substate per core per frame during the DEMO→GAME→cutscene window
    // where the 2-frame lead is introduced. Only enabled with PSXPORT_SBS_STAGETRACE=1.
    // On-change: normal (whole boot window). Verbose per-tick: PSXPORT_SBS_STAGETRACE=2 (dumps EVERY
    // tick in f22..f36 so slip-window diffs are visible even when the state doesn't nominally change).
    static const int stagetrace = []{ const char* e = getenv("PSXPORT_SBS_STAGETRACE"); return e ? atoi(e) : 0; }();
    if (stagetrace && mFrame < 250) {
      auto smState = [](Core* c) {
        uint32_t sm = c->mem_r32(0x1f800138u);
        return std::make_tuple(c->mem_r32(0x801fe00cu),                  // TASK0_ENTRY (base+0xc)
                               c->mem_r16(0x801fe000u),                    // TASK0 base state (base+0)
                               c->mem_r16(sm + 0x48), c->mem_r16(sm + 0x4a),
                               c->mem_r16(sm + 0x4c), c->mem_r16(sm + 0x4e),
                               c->mem_r16(sm + 0x50),                      // sm[0x50] (submode0's inner var)
                               c->mem_r8(0x1f800137u),                     // cut flag
                               c->mem_r8(0x1f800134u));                    // init48 selector
      };
      auto [aE, aS_, a48, a4a, a4c, a4e, a50, aCut, aI34] = smState(&mA->core);
      auto [bE, bS_, b48, b4a, b4c, b4e, b50, bCut, bI34] = smState(&mB->core);
      uint32_t& aP = mStageTraceSigA; uint32_t& bP = mStageTraceSigB;
      // Signature includes ALL logged fields so any change triggers a log line.
      uint32_t aSig = aE ^ (aS_<<1) ^ (a48<<4) ^ (a4a<<8) ^ (a4c<<12) ^ (a4e<<16) ^ (a50<<20) ^ (aCut<<24) ^ (aI34<<26);
      uint32_t bSig = bE ^ (bS_<<1) ^ (b48<<4) ^ (b4a<<8) ^ (b4c<<12) ^ (b4e<<16) ^ (b50<<20) ^ (bCut<<24) ^ (bI34<<26);
      bool verbose_window = (stagetrace >= 2) && ((mFrame >= 22 && mFrame <= 36) || mFrame <= 12);
      if (verbose_window || aSig != aP || bSig != bP) {
        fprintf(stderr, "[stagetrace] f%u A entry=%08X st=%u sm48=%u/4a=%u/4c=%u/4e=%u/50=%u cut=%u i34=%u | B entry=%08X st=%u sm48=%u/4a=%u/4c=%u/4e=%u/50=%u cut=%u i34=%u\n",
                mFrame, aE, aS_, a48, a4a, a4c, a4e, a50, aCut, aI34,
                        bE, bS_, b48, b4a, b4c, b4e, b50, bCut, bI34);
        aP = aSig; bP = bSig;
      }
    }
    // ALLOCTRACE: reset per-frame counters and, if A != B this frame, log both.
    if (mAllocTraceOn && (mAllocA != mAllocB || (mAllocA + mAllocB) > 0 && (mAllocCumA != mAllocCumB))) {
      fprintf(stderr, "[alloctrace] f%u  A: this=%d cum=%d  |  B: this=%d cum=%d  |  A-B this=%+d cum=%+d\n",
              mFrame, mAllocA, mAllocCumA, mAllocB, mAllocCumB,
              mAllocA - mAllocB, mAllocCumA - mAllocCumB);
    }
    mAllocA = 0; mAllocB = 0;
    mWwHit = 0; mWwVa = mWwVb = 0;
    // TRACE 2026-07-03: instrument where 0x800BF81E flips during a frame in RENDER mode.
    // Log the byte on both cores at each waypoint of the frame to name the exact stage.
    const bool ww_trace = mWwArmed && mWwAddr == 0x800BF81Eu && mFrame >= 180 && mFrame <= 200;
    auto ww_log = [&](const char* tag){
      if (!ww_trace) return;
      fprintf(stderr, "[sbs-trace] f%u %-14s  A[0x800BF81E]=%u  B[0x800BF81E]=%u\n",
              mFrame, tag, mA->core.mem_r8(0x800BF81Eu), mB->core.mem_r8(0x800BF81Eu));
    };
    ww_log("frame-start");
    // Divergence check runs THROUGHOUT the run (2026-07-04 user directive [[sbs-two-compare-modes]]
    // reinforced: "autonav can press Start but it can't skip diverges happening during autonav").
    // Autonav is a pad-driving convenience; the byte-exact compare must be active from f0.
    // PSXPORT_SBS_PRENAV=0 kept as an escape hatch to DEFER checkDivergence until nav completes —
    // useful if you're chasing a post-nav-only bug and don't want to be interrupted en route.
    static const int prenav = []{ const char* e = getenv("PSXPORT_SBS_PRENAV");
      return (e && *e && strcmp(e,"0")==0) ? 0 : 1; }();   // default ON; set =0 to defer
    // Pre-step snapshot for the rewind-on-divergence fix. Snap always by default so any tick's
    // divergence can be pinned by rewind. Skip during the rewind re-step itself so we don't
    // overwrite the good snapshot.
    if ((nav_done || prenav) && !mDivFound && !mRewindActive) takePreStepSnap();
    // Reset per-Core SPU write logs so this frame's writes accumulate cleanly.
    spu_log_reset(mA->spu.writeLog);
    spu_log_reset(mB->spu.writeLog);
    stepCore(mA, 0);              ww_log("post-stepA");
    grabPane(mA, mRgbaA, &mWa, &mHa); ww_log("post-grabA");
    stepCore(mB, 1);              ww_log("post-stepB");
    grabPane(mB, mRgbaB, &mWb, &mHb); ww_log("post-grabB");
    checkPaneDiff();              // PICTURE compare: port pane (A) vs oracle pane (B) — render bugs
    // PSXPORT_SBS_SHOT=<frame>:<prefix> — dump each pane SEPARATELY at one lockstep frame, as
    // <prefix>_A.ppm / <prefix>_B.ppm. Mechanical pane-vs-standalone-`shot` comparison (the
    // "SBS pane must equal the standalone config's picture" gate) — the composite dumpPpm can't
    // be diffed against a single-config shot.
    { static int shotInit = 0; static long shotFrames[32]; static int shotN = 0; static char shotPrefix[192];
      if (!shotInit) { shotInit = 1;
        if (const char* e = getenv("PSXPORT_SBS_SHOT"); e && *e) {
          const char* colon = strrchr(e, ':');
          if (colon) {
            snprintf(shotPrefix, sizeof shotPrefix, "%.*s", (int)(sizeof shotPrefix - 1), colon + 1);
            char list[192]; snprintf(list, sizeof list, "%.*s", (int)(colon - e), e);
            for (char* p = strtok(list, ","); p && shotN < 32; p = strtok(nullptr, ",")) shotFrames[shotN++] = atol(p);
          }
        } }
      bool hit = false; for (int i = 0; i < shotN; i++) if ((long)mFrame == shotFrames[i]) hit = true;
      if (hit) {
        auto wr = [this](const char* pfx, char side, const uint8_t* rgba, int w, int h) {
          char p[240]; snprintf(p, sizeof p, "%s_f%u_%c.ppm", pfx, mFrame, side);
          FILE* f = fopen(p, "wb"); if (!f) { fprintf(stderr, "[sbs] SHOT: cannot open %s\n", p); return; }
          fprintf(f, "P6\n%d %d\n255\n", w, h);
          for (int i = 0; i < w * h; i++) fwrite(rgba + (size_t)i * 4, 1, 3, f);
          fclose(f); fprintf(stderr, "[sbs] SHOT f%u pane %c (%dx%d) -> %s\n", mFrame, side, w, h, p);
        };
        wr(shotPrefix, 'A', mRgbaA, mWa, mHa);
        wr(shotPrefix, 'B', mRgbaB, mWb, mHb);
      } }
    // Compare per-Core SPU write logs. For each SPU register touched by EITHER core this frame,
    // compare the LAST value written to it. If A and B end this frame with different values in
    // a given SPU register, that's an audio-relevant divergence (e.g. voice N's StartAddr / Pitch
    // / ADSR — the #29 wrong-sample signature is `Voice[i].reg[0x06]` = sample-select halfword
    // holding a different value on A vs B). This is order-invariant unlike the raw sequence compare,
    // which was flagging reordered-but-identical writes to admin regs (main vol / SPUCNT / CD vol).
    {
      uint32_t na = spu_log_count(mA->spu.writeLog);
      uint32_t nb = spu_log_count(mB->spu.writeLog);
      uint16_t last_a[1024] = {0}; uint32_t seen_a[32] = {0};   // seen_a bitmap over 0x000..0x3FF/16 words
      uint16_t last_b[1024] = {0}; uint32_t seen_b[32] = {0};
      for (uint32_t i = 0; i < na; i++) {
        uint32_t off = spu_log_entry(mA->spu.writeLog, i, 0) & 0x3FFu;
        last_a[off] = (uint16_t)spu_log_entry(mA->spu.writeLog, i, 1);
        seen_a[(off >> 1) >> 5] |= 1u << ((off >> 1) & 31);
      }
      for (uint32_t i = 0; i < nb; i++) {
        uint32_t off = spu_log_entry(mB->spu.writeLog, i, 0) & 0x3FFu;
        last_b[off] = (uint16_t)spu_log_entry(mB->spu.writeLog, i, 1);
        seen_b[(off >> 1) >> 5] |= 1u << ((off >> 1) & 31);
      }
      int flagged = 0;
      for (uint32_t off = 0; off < 0x400; off += 2) {
        uint32_t bit = (off >> 1) & 31, word = (off >> 1) >> 5;
        bool sa = (seen_a[word] >> bit) & 1;
        bool sb = (seen_b[word] >> bit) & 1;
        if (!sa && !sb) continue;
        uint16_t va = sa ? last_a[off] : 0, vb = sb ? last_b[off] : 0;
        // If only one core touched it, only meaningful when the OTHER's stale value is different.
        // Simple: flag any address where at least one wrote AND the two cores don't agree on end value.
        // Cores that didn't write see whatever was there before — for a clean divergence check we
        // only compare within writes; use "same set of addrs + same values" as the invariant.
        if (sa != sb) {
          // Address touched by only one core — that's an ordering/cadence hit, not a value hit. Log
          // it but keep hunting for a real value-mismatch (which is the #29 signature).
          fprintf(stderr, "[sbs-div] f%u [AUDIO spu_reg 0x%03X only-%c] val=0x%04X\n",
                  mFrame, off, sa ? 'A' : 'B', sa ? va : vb);
          if (++flagged >= 8) break;
        } else if (va != vb) {
          const char* voice_hint = "";
          if (off < 0x180) { static char buf[32]; snprintf(buf, sizeof buf, "voice%u+0x%02X", off >> 4, off & 0xF); voice_hint = buf; }
          fprintf(stderr, "[sbs-div] f%u [AUDIO spu_reg 0x%03X %s] A=0x%04X  B=0x%04X\n",
                  mFrame, off, voice_hint, va, vb);
          if (++flagged >= 8) break;
        }
      }
    }
    presentPanes();               ww_log("post-present");
    static const int only_on_value_diverge_ss = []{ const char* e = getenv("PSXPORT_SBS_WW_ONVALUEDIVERGE"); return e && *e && e[0] != '0' ? 1 : 0; }();
    // Per-lockstep-frame RNG advance-count divergence check. When PSXPORT_SBS_WW_ONVALUEDIVERGE is
    // set on 0x80105EE8, we want the FIRST frame where A's advance count != B's — that's the frame
    // where one core made an extra (or missed a) RNG call vs the other. Every store to 0x80105EE8
    // increments its side's counter (via storeCb's PREWATCH path); we compare at the end of each
    // lockstep frame and dump the divergent core's stack on first mismatch.
    if (only_on_value_diverge_ss && mWwArmed) {
      // Divergence trigger: EITHER cadence-count differs (one core advanced N times, the other M
      // times) OR both cadences match but the end-of-frame value at the armed byte differs (a
      // VALUE-MISMATCH within matched cadence, e.g. same fn writes different data to the same
      // address). Watch the exact armed byte, not a hardcoded RNG seed addr — this makes the
      // probe usable for any address, not just 0x80105EE8.
      uint32_t seedA = mA->core.mem_r8(mWwAddr & 0x1FFFFFFFu);
      uint32_t seedB = mB->core.mem_r8(mWwAddr & 0x1FFFFFFFu);
      bool count_diverge = (mWwCountA != mWwCountB);
      bool value_diverge = (seedA != seedB);
      if ((count_diverge || value_diverge) && !mDivFound) {
        fprintf(stderr, "[sbs] === RNG advance-count divergence: f%u  A_calls=%u  B_calls=%u  (delta=%d)   endA=0x%08X endB=0x%08X ===\n",
                mFrame, mWwCountA, mWwCountB, (int)mWwCountA - (int)mWwCountB, seedA, seedB);
        fprintf(stderr, "[sbs] Last-write host stack per core is the fn that made the EXTRA (or first missed) advance THIS FRAME.\n");
        auto dump_bt = [&](const char* tag, void** bt, int n) {
          if (n <= 0) { fprintf(stderr, "[sbs] === HOST BACKTRACE — %s (empty) ===\n", tag); return; }
          fprintf(stderr, "[sbs] === HOST BACKTRACE — %s (%d frames) ===\n", tag, n);
          char** syms = backtrace_symbols(bt, n);
          if (syms) { for (int i = 0; i < n; i++) fprintf(stderr, "[sbs]   #%d %s\n", i, syms[i]); free(syms); }
        };
        dump_bt("core A (last RNG advance THIS frame)", mWwHostBtA, mWwHostBtNA);
        dump_bt("core B (last RNG advance THIS frame)", mWwHostBtB, mWwHostBtNB);
        // Overlay .rodata content probe: many divergent writes read tables from mode-overlay .rodata
        // (0x8010xxxx..0x8014xxxx). If A and B have different overlays resident, table reads return
        // different values → the divergence surfaces as a VALUE-MISMATCH inside a matched code path.
        // Dump the neighborhoods around a few common overlay .rodata addresses to name the diff.
        {
          fprintf(stderr, "[sbs] === overlay .rodata sample (byte@addr, A vs B) ===\n");
          for (uint32_t addr : {0x80105EE8u, 0x800BFA13u, 0x800BF873u, 0x800ED098u, 0x800E7E74u, 0x800ECFD4u}) {
            uint32_t a = mA->core.mem_r32(addr), b = mB->core.mem_r32(addr);
            fprintf(stderr, "[sbs]   [0x%08X]: A=0x%08X  B=0x%08X  %s\n",
                    addr, a, b, a == b ? "match" : "!! DIVERGE !!");
          }
          // Scan main RAM for locations that hold the write address as a 4-byte value. A common
          // divergence is "different object owns render-record at addr X" — search for the addr in
          // both cores' RAM and dump any locations that hold it. Only main RAM (0x80010000..
          // 0x80200000); scratchpad is too small to matter. Cap at 20 hits per core to keep the
          // dump small.
          {
            uint32_t target = mWwAddr & 0x00FFFFFFu;   // strip kseg bits
            target |= 0x80000000u;
            fprintf(stderr, "[sbs] === RAM scan for the write-target ptr (0x%08X, and nearby) ===\n", target);
            auto scan_range = [&](const char* tag, Core* c, uint32_t lo, uint32_t hi) {
              int hits = 0;
              for (uint32_t a = 0x80010000u; a < 0x80200000u && hits < 20; a += 4) {
                uint32_t v = c->mem_r32(a);
                if (v >= lo && v <= hi) {
                  fprintf(stderr, "[sbs]   %s: 0x%08X holds ptr 0x%08X\n", tag, a, v);
                  hits++;
                }
              }
              if (hits == 0) fprintf(stderr, "[sbs]   %s: no matches in [0x%08X, 0x%08X]\n", tag, lo, hi);
            };
            // Broaden window ± 128 bytes — render records are 128-byte structures, the write may
            // land at any offset inside one.
            scan_range("core A", &mA->core, target - 128, target);
            scan_range("core B", &mB->core, target - 128, target);
            // For any obj+0xC0 that holds a render-rec ptr inside the target window, dump the OBJECT
            // fields on both cores. Divergence in obj+4 (state), obj+8 (sub-count), obj+9 (active
            // gate), obj+0x1C (handler) names why one core fires the write and the other doesn't.
            fprintf(stderr, "[sbs] === candidate owner-object state (obj+0xC0 = render-rec ptr) ===\n");
            for (uint32_t a = 0x80010000u; a < 0x80200000u; a += 4) {
              uint32_t v = mA->core.mem_r32(a);
              if (v < target - 128 || v > target) continue;
              // Assume `a` is at obj+0xC0. obj_base = a - 0xC0.
              uint32_t obj = a - 0xC0u;
              fprintf(stderr, "[sbs]   obj @ 0x%08X (rec ptr 0x%08X, delta from write = 0x%X):\n",
                      obj, v, target - v);
              for (uint32_t off : {0x00u, 0x04u, 0x05u, 0x08u, 0x09u, 0x0Cu, 0x1Cu, 0x24u, 0x3Cu}) {
                uint32_t va = obj + off;
                uint32_t aval = mA->core.mem_r32(va), bval = mB->core.mem_r32(va);
                fprintf(stderr, "[sbs]     obj+0x%02X: A=0x%08X  B=0x%08X  %s\n",
                        off, aval, bval, aval == bval ? "match" : "!! DIVERGE !!");
              }
            }
          }
        }
        fprintf(stderr, "[sbs] headless: exiting after RNG-count divergence.\n");
        fflush(stderr);
        sbs_rl_shutdown();
        exit(0);
      }
      // Reset per-frame counters + captured stacks for the next frame's compare. Keep mWwVa/mWwVb
      // as the last-value marker (already matched this frame, otherwise the storeCb hook would have
      // triggered and exited).
      mWwCountA = mWwCountB = 0;
      mWwHostBtNA = mWwHostBtNB = 0;
    }
    // Parity surface: with both cores past AUTO-NAV, name any RAM/scratchpad divergence. On the
    // FIRST byte that differs, `checkDivergence` records the range + backtraces + pauses (via the
    // debug server) so `sbs diff` / `sbs bt` / `sbs watch` can inspect. The 30-frame summary is
    // the running "how far apart are they" metric so you see divergence GROW even before the first
    // recorded hit (in render/full modes the render regions are excluded by design).
    if (nav_done || prenav) {
      summarizeDivergence(30);
      checkDivergence();
    }
    if (sbsDumpPath && nav_done && !dumped && mWa > 0 && mWb > 0) { dumpPpm(sbsDumpPath); dumped = true; }

    // Under PSXPORT_SBS_WW_ONVALUEDIVERGE, the storeCb itself decides when to trigger — the WRITE-
    // SITE post-step path here would otherwise pause on the first single-side write, missing the
    // "wait for both cores to have written differing values" semantic. Skip this path in that mode.
    if (only_on_value_diverge_ss) { /* handled entirely in storeCb + frame-boundary count check */ }
    else if (mWwArmed && mWwHit) {
      // A hit is divergent if only one core wrote (mask != 3) OR both wrote different values.
      // BUT: an asymmetric store where both sides wrote the SAME value is boot-timing noise (each
      // core's boot init writes 0 to the address in a different frame — mask flips to 1 or 2 with
      // mWwVa/mWwVb still zero). Those aren't real divergence — the address would agree the moment
      // the other core catches up. So treat "mask asymmetric but values equal (or both zero)" as
      // NOT divergent: keep the watch armed and continue. Real divergence = mask!=3 with the
      // written value differing from the OTHER core's current byte value, or both wrote unequal
      // values (mask==3, va!=vb). Non-PREWATCH `sbs watch` still pauses on the first hit either
      // way (mWwPersist=false), matching pre-PREWATCH behavior.
      bool divergent;
      if (mWwHit == 3) {
        divergent = (mWwVa != mWwVb);                    // both wrote — pause iff values differ
      } else {
        int which_wrote = (mWwHit == 1) ? 0 : 1;         // 0=A wrote, 1=B wrote
        uint32_t v_written = which_wrote ? mWwVb : mWwVa;
        uint32_t v_other   = which_wrote ? mA->core.mem_r8(mWwAddr) : mB->core.mem_r8(mWwAddr);
        divergent = (v_written != v_other);              // asymmetric — pause iff writer's value ≠ other's current
      }
      if (divergent || !mWwPersist) {
        fprintf(stderr, "[sbs] *** WRITE-SITE caught 0x%08X (A=%08X B=%08X, mask=%d) at frame %u ***\n",
                mWwAddr, mWwVa, mWwVb, mWwHit, mFrame);
        // Auto-diagnosis: compare per-core call-site metadata captured during the rewind. Reports the
        // most likely CLASS of divergence so the operator doesn't have to eyeball the raw wwatch log.
        //  - VALUE-MISMATCH  : both cores wrote different values via the SAME call path (same pc + ra).
        //                       Root cause is upstream input state; probe further with BYTETRACE on the
        //                       fields that fed this branch.
        //  - CALLSITE-DIVERGE: cores wrote from different guest ra's — they took different call paths
        //                       to reach the store. The ra pair NAMES the split. `python3 tools/disas.py
        //                       <ra_a> 4` / `<ra_b>` shows the calling instructions.
        //  - FN-DIVERGE      : cores wrote from different containing fns (differing pc). Same shape as
        //                       CALLSITE-DIVERGE but c->pc is the fn ENTRY (or stale from the last jal),
        //                       so it names the leaf recomp function context, not the caller.
        //  - COUNT-MISMATCH  : one core wrote the address more times than the other in the rewind frame
        //                       — a loop / dispatch that fires more iterations on one side. Almost
        //                       always a state-machine or object-list divergence upstream.
        //  - ASYMMETRIC      : only one core wrote in the rewind frame (mWwHit != 3). The other core's
        //                       path never touches this address this tick; look at the frame BEFORE to
        //                       find why the writer's caller was taken (state, flag, spawn count).
        fprintf(stderr, "[sbs] === auto-diagnosis ===\n");
        fprintf(stderr, "[sbs]   A: pc=0x%08X ra=0x%08X sp=0x%08X val=0x%08X hits=%u\n",
                mWwPcA, mWwRaA, mWwSpA, mWwVa, mWwCountA);
        fprintf(stderr, "[sbs]   B: pc=0x%08X ra=0x%08X sp=0x%08X val=0x%08X hits=%u\n",
                mWwPcB, mWwRaB, mWwSpB, mWwVb, mWwCountB);
        auto emit_class = [&](const char* cls, const char* detail) {
          fprintf(stderr, "[sbs]   CLASS: %s — %s\n", cls, detail);
        };
        if (mWwHit != 3) {
          emit_class("ASYMMETRIC", "only one core stored this frame; look at prior frames for the flag that gates the writer's caller");
        } else if (mWwCountA != mWwCountB) {
          char buf[128]; snprintf(buf, sizeof buf, "A wrote %u× vs B wrote %u× — loop/dispatch runs more iterations on one core",
                                  mWwCountA, mWwCountB);
          emit_class("COUNT-MISMATCH", buf);
        } else if (mWwPcA != mWwPcB || mWwRaA != mWwRaB) {
          char buf[192]; snprintf(buf, sizeof buf,
              "A came via ra=0x%08X pc=0x%08X; B came via ra=0x%08X pc=0x%08X. Disasm ra-8 on each to see the calling jal / branch that split.",
              mWwRaA, mWwPcA, mWwRaB, mWwPcB);
          emit_class(mWwPcA != mWwPcB ? "FN-DIVERGE" : "CALLSITE-DIVERGE", buf);
        } else if (mWwVa != mWwVb) {
          emit_class("VALUE-MISMATCH",
              "same caller/pc, different value — upstream input state differs. BYTETRACE the fields feeding the writer's branch.");
        } else {
          emit_class("(no signal)", "same caller/pc/value/hits — probably filtered out earlier; investigate manually");
        }
        // Struct-layout probe: registry of known guest RAM arrays. When the divergent address falls
        // inside a registered array, dump the record INDEX + offset within-record so a caller doesn't
        // have to hand-decode it. Every new hit that recurs across sessions ought to land here.
        struct StructLayout { uint32_t base; uint32_t stride; uint32_t count; const char* name; };
        static constexpr StructLayout kLayouts[] = {
          // input-processor record table (input.cpp FUN_800931C0): iterated over records[0, N)
          // where N = (int8)0x80105CEC. Each 56 B record holds pad/controller state; +0x00 is the
          // h0 field written by FUN_8009A1D0. Native input_dispatch_931c0 references it verbatim.
          { 0x801054CEu, 56u, 25u, "input.record" },
          // Task-slot table (0x801FE000, stride 0x70, 3 slots) — scheduler state
          { 0x801FE000u, 0x70u, 3u, "task_slot" },
          // Object arm-slot table (0x800BE238, stride 12, 24 slots) — walked by AreaSlots::updateTail
          { 0x800BE238u, 12u, 24u, "area.arm_slot" },
          // Voice/audio state at 0x800BE1F8 (single struct, 0x40 B typical)
          { 0x800BE1F8u, 0x40u, 1u, "voice.state" },
          // libgs graphics context struct (set by ResetGraph, mutated by LoadImage/DrawSync chain)
          { 0x800AC5F8u, 0x100u, 1u, "libgs.gfx_ctx" },
          // ScreenFade held-fully-faded latch (game/render/screen_fade)
          { 0x800E7DE0u, 8u, 1u, "screen_fade.state" },
          // Object-pool T2 node table (0x800EE480 typical — record stride 0x40)
          // (Approximate — the pool has multiple sub-tables; treat this as a hint region)
          { 0x800EE480u, 0x40u, 32u, "object.pool[T2]" },
        };
        for (const auto& L : kLayouts) {
          uint32_t end = L.base + L.stride * L.count;
          if (mWwAddr >= L.base && mWwAddr < end) {
            uint32_t off  = mWwAddr - L.base;
            uint32_t idx  = off / L.stride;
            uint32_t roff = off % L.stride;
            fprintf(stderr, "[sbs] === struct layout: %s[%u] + 0x%02X @ base 0x%08X (stride 0x%X, count %u) ===\n",
                    L.name, idx, roff, L.base, L.stride, L.count);
            break;
          }
        }

        // Upstream state cross-check: dump a handful of commonly-diverging globals so the caller can
        // see at a glance whether RNG or well-known state has drifted before the visible divergence.
        // Cheap (8 words) and often decisive — if RNG matches, drift is downstream of RNG.
        fprintf(stderr, "[sbs] === upstream state cross-check ===\n");
        struct GlobalCheck { uint32_t addr; uint8_t width; const char* name; };
        static constexpr GlobalCheck kUpstream[] = {
          { 0x80105EE8u, 4, "RNG.seed" },
          { 0x800BE358u, 4, "arm-mask" },
          { 0x800BED84u, 2, "hword.0BED84" },
          { 0x800A4F7Eu, 2, "hword.0A4F7E" },
          { 0x800BF870u, 1, "area.idx" },
          { 0x1F800137u, 1, "cutMode" },
          { 0x1F800138u, 4, "CUR_TASK" },
          { 0x1F80019Bu, 1, "done_flag" },
        };
        for (const auto& g : kUpstream) {
          uint32_t va = 0, vb = 0;
          if (g.width == 1) { va = mA->core.mem_r8(g.addr); vb = mB->core.mem_r8(g.addr); }
          else if (g.width == 2) { va = mA->core.mem_r16(g.addr); vb = mB->core.mem_r16(g.addr); }
          else { va = mA->core.mem_r32(g.addr); vb = mB->core.mem_r32(g.addr); }
          fprintf(stderr, "[sbs]   %-14s @0x%08X (%uB): A=0x%08X B=0x%08X %s\n",
                  g.name, g.addr, g.width, va, vb, va == vb ? "match" : "!! DIVERGE !!");
        }

        // Task-slot state dump. Each slot's state (+0x00), entry pc (+0x0C), done-mark (+0x02).
        // Divergent slot state = task-scheduling divergence — the most common cause of a wrong
        // CUR_TASK / wrong writer during multitask cooperative code (task-1 preload etc.).
        fprintf(stderr, "[sbs] === task-slot state ===\n");
        for (int slot = 0; slot < 3; slot++) {
          uint32_t base = 0x801FE000u + (uint32_t)slot * 0x70u;
          uint16_t sa_st = mA->core.mem_r16(base + 0x00), sb_st = mB->core.mem_r16(base + 0x00);
          uint16_t sa_02 = mA->core.mem_r16(base + 0x02), sb_02 = mB->core.mem_r16(base + 0x02);
          uint32_t sa_ep = mA->core.mem_r32(base + 0x0C), sb_ep = mB->core.mem_r32(base + 0x0C);
          fprintf(stderr, "[sbs]   task[%d] @0x%08X: state A=%u B=%u %s  +2 A=0x%X B=0x%X %s  entry A=0x%08X B=0x%08X %s\n",
                  slot, base, sa_st, sb_st, sa_st == sb_st ? "==" : "!!",
                  sa_02, sb_02, sa_02 == sb_02 ? "==" : "!!",
                  sa_ep, sb_ep, sa_ep == sb_ep ? "==" : "!!");
        }

        // Guest stack window near write-sp on both cores. The bytes around sp reveal the actual
        // callee-save spills (sw ra, sw sN) that produced the divergent stack scratch — same shape
        // as the guest-stack backtrace but showing ACTUAL bytes not just plausible-ra hits.
        auto dump_stack_window = [&](const char* tag, Core* c, uint32_t sp) {
          if (!sp || sp < 0x80010000u || sp >= 0x80200000u) return;
          fprintf(stderr, "[sbs] === guest stack window %s (sp=0x%08X, sp-16..sp+64) ===\n", tag, sp);
          for (int32_t off = -16; off <= 64; off += 4) {
            uint32_t va = sp + (uint32_t)off;
            if (va < 0x80010000u || va >= 0x80200000u) continue;
            uint32_t w = c->mem_r32(va);
            fprintf(stderr, "[sbs]   sp%+d @0x%08X = 0x%08X%s\n",
                    off, va, w, off == 0 ? " <-- sp" : "");
          }
        };
        if (mWwHit & 1) dump_stack_window("A", &mA->core, mWwSpA);
        if (mWwHit & 2) dump_stack_window("B", &mB->core, mWwSpB);

        // Call-chain depth heuristic. Same pc/ra + different sp = the CALL CHAIN reaching the writer
        // has a different depth on each core. Point that out so the caller doesn't have to eyeball sp.
        if (mWwHit == 3 && mWwPcA == mWwPcB && mWwRaA == mWwRaB && mWwSpA != mWwSpB) {
          int32_t delta = (int32_t)mWwSpA - (int32_t)mWwSpB;
          fprintf(stderr, "[sbs]   CALL-CHAIN DEPTH DIFFERS: A.sp=0x%08X B.sp=0x%08X (delta %+d B — %s%s)\n",
                  mWwSpA, mWwSpB, delta,
                  delta > 0 ? "B is deeper" : "A is deeper",
                  " — a caller above the writer differs; disasm the fn containing ra to find the split");
        }

        // List-membership probe: for a divergence in the object-pool byte range, walk the two per-frame
        // object lists on both cores and report which list(s) contain the divergent record's base. A
        // node appearing on a list on one core but not the other names an upstream spawn/list-migration
        // divergence — the object was moved between lists asymmetrically. Cheap: 200-node cap per list.
        {
          // Object base heuristic: the wwatch fires on obj+T2OBJ_RENDER_FLAG (=+1) for T2 nodes.
          // mWwAddr is the WORD-aligned range the wwatch armed on (byte addr & ~3u). For a T2
          // node whose base is word-aligned (typical), obj_base = mWwAddr. If the store target
          // byte is offset 1 (0x800EE489) and mWwAddr = 0x800EE488, then obj = mWwAddr.
          uint32_t obj_base = mWwAddr;
          if (obj_base >= 0x800E0000u && obj_base < 0x80200000u) {
            auto find_on_list = [&](Core* c, uint32_t head_addr, uint32_t target, int* pos_out) -> int {
              uint32_t head = c->mem_r32(head_addr);
              int idx = 0;
              for (uint32_t n = head; n && idx < 200; idx++) {
                // Any node whose record footprint covers the write addr counts as containing it (the
                // record stride varies across pools, but the write is always to base+small-offset).
                if (n <= mWwAddr && mWwAddr < n + 0x80u) { if (pos_out) *pos_out = idx; return 1; }
                n = c->mem_r32(n + 0x24u);   // T2OBJ_NEXT
              }
              if (pos_out) *pos_out = -1;
              return 0;
            };
            struct L { uint32_t head; const char* name; } lists[] = {
              { 0x800FB168u, "OBJLIST_1" }, { 0x800F2624u, "OBJLIST_2" },
              { 0x800F2738u, "OBJLIST_3" }, // AUX_LIST_HEAD candidate (walkAux uses one of these)
            };
            fprintf(stderr, "[sbs] === list-membership probe (write addr 0x%08X) ===\n", mWwAddr);
            for (auto& L : lists) {
              int pos_a = -1, pos_b = -1;
              int on_a = find_on_list(&mA->core, L.head, mWwAddr, &pos_a);
              int on_b = find_on_list(&mB->core, L.head, mWwAddr, &pos_b);
              const char* verdict = (on_a == on_b) ? "match" : "!! DIVERGE !!";
              fprintf(stderr, "[sbs]   %s (head@%08X): A=%s(idx=%d) B=%s(idx=%d) %s\n",
                      L.name, L.head, on_a ? "on" : "off", pos_a, on_b ? "on" : "off", pos_b, verdict);
            }
            // Dump the object record's key fields on both cores. If the containing object is a linked-
            // list node its per-obj handler ptr lives at obj+0x1c (T2OBJ_HANDLER); state bytes typically
            // at obj+4/5/6/7. Divergence in these fields IS the upstream root when list membership
            // matches — the same node has different data on each core.
            fprintf(stderr, "[sbs] === object record dump (base 0x%08X, T2 offsets) ===\n", obj_base);
            // Bytes at meaningful T2 offsets (byte-precise reads so we see the actual flag values,
            // not the u32 they're packed into).
            for (uint32_t off : {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u,
                                  0x0Au, 0x0Bu, 0x28u}) {
              uint32_t va = obj_base + off;
              uint8_t a = mA->core.mem_r8(va), b = mB->core.mem_r8(va);
              fprintf(stderr, "[sbs]   +0x%02X byte: A=0x%02X  B=0x%02X  %s\n",
                      off, a, b, a == b ? "match" : "!! DIVERGE !!");
            }
            // Key u32 fields at natural alignment. Read at obj_base + off (obj_base is aligned since
            // T2 records live on aligned addresses).
            for (uint32_t off : {0x1Cu, 0x24u}) {
              uint32_t va = obj_base + off;
              uint32_t a4 = mA->core.mem_r32(va), b4 = mB->core.mem_r32(va);
              fprintf(stderr, "[sbs]   +0x%02X word (@%08X): A=0x%08X  B=0x%08X  %s%s\n",
                      off, va, a4, b4, a4 == b4 ? "match" : "!! DIVERGE !!",
                      off == 0x1Cu ? "  (T2OBJ_HANDLER)" : "  (T2OBJ_NEXT)");
            }
            // Object position (obj+0x2C/2E/30) — cull inputs. If they diverge, the divergence is
            // upstream in physics/spawn, not in the cull itself.
            fprintf(stderr, "[sbs] === object position (cull input) + camera scratchpad ===\n");
            for (uint32_t off : {0x2Cu, 0x2Eu, 0x30u}) {
              uint32_t va = obj_base + off;
              int16_t a = (int16_t)mA->core.mem_r16(va), b = (int16_t)mB->core.mem_r16(va);
              fprintf(stderr, "[sbs]   obj+0x%02X (s16): A=%d  B=%d  %s\n",
                      off, a, b, a == b ? "match" : "!! DIVERGE !!");
            }
            // Camera pos + fwd vec (scratchpad, cull-cone inputs).
            for (uint32_t va : {0x1F8000D2u, 0x1F8000D6u, 0x1F8000DAu, 0x1F8000E8u, 0x1F8000EAu, 0x1F8000ECu}) {
              int16_t a = (int16_t)mA->core.mem_r16(va), b = (int16_t)mB->core.mem_r16(va);
              const char* what =
                  va == 0x1F8000D2u ? "cam.x" : va == 0x1F8000D6u ? "cam.y" : va == 0x1F8000DAu ? "cam.z" :
                  va == 0x1F8000E8u ? "fwd.x" : va == 0x1F8000EAu ? "fwd.y" : "fwd.z";
              fprintf(stderr, "[sbs]   @0x%08X (%s, s16): A=%d  B=%d  %s\n",
                      va, what, a, b, a == b ? "match" : "!! DIVERGE !!");
            }
          }
        }
        // Host-side C-stack backtrace at the last-captured wwatch fire per core. Cuts through the
        // stale-c->pc problem: the guest pc/ra can lie (a wrapper set c->pc long ago and the store
        // happens elsewhere), but the host backtrace names the ACTUAL C function running when
        // mem_w8/w16/w32 fired — the uncontested writer. Filter with symres or head -N as needed.
        auto dump_host_bt = [](const char* tag, void** bt, int n) {
          if (n <= 0) { fprintf(stderr, "[sbs] === HOST BACKTRACE — %s (empty) ===\n", tag); return; }
          fprintf(stderr, "[sbs] === HOST BACKTRACE — %s (%d frames, last-fire) ===\n", tag, n);
          char** syms = backtrace_symbols(bt, n);
          if (syms) {
            for (int i = 0; i < n; i++) fprintf(stderr, "[sbs]   #%d %s\n", i, syms[i]);
            free(syms);
          } else {
            // backtrace_symbols failed (rare); fall back to raw ptrs so we still have SOMETHING.
            for (int i = 0; i < n; i++) fprintf(stderr, "[sbs]   #%d %p (unresolved)\n", i, bt[i]);
          }
        };
        if (mWwHit & 1) {
          fprintf(stderr, "[sbs] === WRITE SITE — core A wrote 0x%08X=%08X ===\n%s",
                  mWwAddr, mWwVa, mWwBtA[0] ? mWwBtA : "(empty)\n");
          dump_host_bt("core A", mWwHostBtA, mWwHostBtNA);
        }
        if (mWwHit & 2) {
          fprintf(stderr, "[sbs] === WRITE SITE — core B wrote 0x%08X=%08X ===\n%s",
                  mWwAddr, mWwVb, mWwBtB[0] ? mWwBtB : "(empty)\n");
          dump_host_bt("core B", mWwHostBtB, mWwHostBtNB);
        }
        mWwArmed = false;
        mA->core.wwatch_arm(0, 0); mB->core.wwatch_arm(0, 0);
        mA->dbg_server.setPaused(true);
        // Headless (no debug server): the write-site IS the answer — exit so the log ends with it.
        if (!mHaveDbgsrv) { fprintf(stderr, "[sbs] headless: exiting after write-site capture.\n"); sbs_rl_shutdown(); exit(0); }
      }
      // Else: identical shared write in PREWATCH mode — silently continue and keep watching.
    }
    mFrame++;
    // PSXPORT_SBS_EXIT_FRAME=<n>: CLEAN exit(0) once frame n is reached, so atexit dumps
    // (engine_override_thunk per-address hit counts, EngineOverrides ovhit) actually print.
    // A `timeout`-killed gate dies via the watchdog's SIGTERM _exit(130), which skips atexit —
    // wiring passes need the hit counts to prove every registered address FIRED (docs/config.md).
    { static int s_exitFrame = -2;
      if (s_exitFrame == -2) s_exitFrame = cfg_int("PSXPORT_SBS_EXIT_FRAME", -1);
      if (s_exitFrame >= 0 && mFrame >= (uint32_t)s_exitFrame) {
        fprintf(stderr, "[sbs] PSXPORT_SBS_EXIT_FRAME=%d reached — clean exit for atexit dumps.\n", s_exitFrame);
        sbs_rl_shutdown();
        exit(0);
      } }
  }
  sbs_rl_shutdown();
  exit(0);
}

// ============================================================================
// Public Sbs — pimpl forwarders. Instance lives on the stack in Sbs::run(); every other Sbs method
// dispatches through mImpl. Two Games each hold `game->sbs` back-pointer to this instance so any
// code with a `Core* c` reaches the harness via `c->game->sbs`.
// ============================================================================

Sbs::Sbs()  : mImpl(new Impl()) {}
Sbs::~Sbs() { delete mImpl; }

void Sbs::run(const char* exePath) {
  Sbs harness;
  harness.mImpl->run(exePath, &harness);   // wires game->sbs on both Games inside; drives loop; exit(0)
}

bool     Sbs::active() const                                { return mImpl->active(); }
int      Sbs::coreId(Core* c) const                         { return mImpl->coreId(c); }
uint32_t Sbs::frame() const                                 { return mImpl->frameNum(); }
int      Sbs::dbgCmd(FILE* out, const char* line)           { return mImpl->dbgCmd(out, line); }
void Sbs::storeCb(Core* c, uint32_t addr, uint32_t val, uint32_t width) {
  if (c->game && c->game->sbs) c->game->sbs->mImpl->storeCb(c, addr, val, width);
}
Core*    Sbs::coreByLetter(char which) const                { return mImpl->coreByLetter(which); }
Core*    Sbs::shownCore() const                             { return mImpl->shownCore(); }
bool     Sbs::skipCompareMode() const                       { return mImpl->skipCompareMode(); }
bool     Sbs::skipRendezvousReached(Core* c, uint32_t addr, uint32_t minVal, const char* label) {
  return mImpl->skipRendezvousReached(c, addr, minVal, label);
}
