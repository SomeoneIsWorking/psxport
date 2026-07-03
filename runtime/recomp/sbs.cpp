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
#include "render/render.h"    // Render::setPsxRender (per-Core render-path switch)
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <tuple>
#include <map>
#include <algorithm>
#include <csignal>

// --- runtime entry points reused from the normal boot path / dual-core harness ---
void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
extern "C" int cfg_on(const char*);
extern "C" void watchdog_disable(void);
extern "C" void guest_backtrace_to(Core* c, FILE* out);
extern "C" void mem_set_store_watch_cb(void (*cb)(Core*, uint32_t, uint32_t));
void gpu_gpu_render_readback(Core* core, const uint16_t* vram, int sx, int sy, int w, int h, uint8_t* rgba);
void gpu_gpu_select_target(int t);
void gpu_gpu_frame_end(Core* core, const uint16_t* svram, int frame);
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

enum Mode { M_RENDER, M_GAMEPLAY, M_FULL, M_ORACLE };
constexpr uint32_t GAME_ENTRY  = 0x8010637Cu;  // task0 entry while the GAME stage runs (in the field)
constexpr uint32_t TASK0_ENTRY = 0x801fe00cu;  // task0 obj +0xc = current stage entry
constexpr uint32_t CUT_FLAG    = 0x1F800137u;  // cutscene-active byte (1 = intro cutscene, 0 = free-roam)
constexpr uint16_t BTN_CROSS = 0x4000, BTN_START = 0x0008, BTN_NONE = 0xFFFF;

enum Phase { REACH_GAME, AWAIT_CUT, SKIP_CUT, DONE };
struct Nav { Phase phase = REACH_GAME; int idle = 0; };
struct SbsKey { uint32_t from, to; uint16_t btn; };

// Pimpl body — all Sbs state and dispatch lives here. Accessed from Sbs's public methods (below)
// through `mImpl`; the header stays light.
class Sbs::Impl {
public:
  void run(const char* exePath, Sbs* facade);
  int  dbgCmd(FILE* out, const char* line);
  void dumpAllocRa(FILE* out);
  void storeCb(Core* c, uint32_t addr, uint32_t val);
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
  char     mBtA[4096] = {0}, mBtB[4096] = {0};
  bool     mHaveDbgsrv = false;

  // ---- write-watchpoint record (exact corrupting-write site) ----
  bool     mWwArmed = false;
  bool     mWwPersist = false;      // PREWATCH: stay armed until first DIVERGENT write (not first write)
  uint32_t mWwAddr  = 0;
  int      mWwHit   = 0;            // bit0 = A wrote, bit1 = B wrote
  uint32_t mWwVa = 0, mWwVb = 0;
  char     mWwBtA[4096] = {0}, mWwBtB[4096] = {0};

  // ---- ALLOCTRACE: per-frame count of writes to 0x800ED098 (the free-slot count) per core ----
  // Attack (a) instrumentation: names the frame(s) where A allocates more than B. If A > B on a
  // specific frame, that's where the 3-slot lead grows. Enabled with PSXPORT_SBS_ALLOCTRACE=1.
  int      mAllocTraceOn = 0;
  int      mAllocA = 0, mAllocB = 0;   // per-frame decrement count (any write value < current)
  int      mAllocCumA = 0, mAllocCumB = 0;
  // Per-ra bucket: {alloc, release} counts by guest r[31] at store time, split A vs B. Landed as the
  // durable workflow-first invariant for +N-alloc attribution — ordinal-point-in-time comparison is
  // misleading (a timing-shifted alloc reads as "A-only") and the correct compare is at SETTLED STATE
  // (per-ra totals over the whole run). Asymmetric buckets = real caller divergence; symmetric = a
  // timing shift. Dumped at the end of every run when ALLOCTRACE is on; opt-in REPL `sbs allocra`.
  struct RaBucket { int allocA=0, allocB=0, relA=0, relB=0; };
  std::map<uint32_t, RaBucket> mAllocRa;
  int      mAllocRaDumped = 0;

  // ---- scripted headless input (PSXPORT_SBS_KEYS) ----
  std::vector<SbsKey> mKeys;
  bool                mKeysParsed = false;

  // ---- navigation state (concurrent boot AUTO-NAV to free-roam) ----
  Nav mNavA, mNavB;

  // ---- helpers / stages ----
  const char* modeName() const;
  bool  isRenderRegion(uint32_t a) const;
  bool  isRenderSpad(uint32_t a) const;
  bool  isCdCacheNoise(uint32_t a) const;
  bool  isAudioNoise(uint32_t a) const;
  bool  isObjectPoolNoise(uint32_t a) const;
  bool  isDiffNoise(uint32_t a) const { return isRenderRegion(a) || isCdCacheNoise(a) || isAudioNoise(a) || isObjectPoolNoise(a); }
  static bool isSpad(uint32_t a) { return a >= 0x1F800000u && a < 0x1F800400u; }
  void  capBt(Core* c, char* buf, size_t n);
  bool  navStep(Core* c, Nav& nv, uint32_t f, const char* tag);
  void  applyMode(Game* g, int which);
  void  recordDivergence(uint32_t addr);
  void  checkDivergence();
  // Per-frame divergence SUMMARY: count differing bytes (excluding render noise) across main RAM
  // + scratchpad and log a one-line report each `every` frames. Doesn't pause on the first byte
  // (that's `checkDivergence()`); it surfaces the running "how far apart are the two cores" number
  // so a divergence trend is visible even before checkDivergence trips.
  void  summarizeDivergence(uint32_t every);
  void  stepCore(Game* g, int which);
  void  grabPane(Game* g, uint8_t* rgba, int* ow, int* oh);
  void  presentPanes() { sbs_rl_present(mA, mRgbaA, mWa, mHa, mRgbaB, mWb, mHb); }
  uint16_t btnBit(const char* n) const;
  void  parseKeys();
  void  feedInput();
  void  dumpPpm(const char* path);
};

const char* Sbs::Impl::modeName() const {
  return mMode == M_RENDER ? "render" : mMode == M_GAMEPLAY ? "gameplay" :
         mMode == M_ORACLE ? "oracle" : "full";
}

// Legit render-only guest regions in MAIN RAM: the native vs PSX render paths write GP0 packets / OT /
// pool pointers here (render + full mode). Divergence here is render noise, not the gameplay we hunt.
bool Sbs::Impl::isRenderRegion(uint32_t a) const {
  if (a >= 0x800BF4F0u && a < 0x800BF54Cu) return true;   // pool ptrs + dwell
  if (a >= 0x800BFE68u && a < 0x800EA200u) return true;   // packet pool (×2) + OT (×2) + env
  return false;
}

// libcd's CD_cachefile / CD_newmedia directory-cache tables. The native gameplay path
// (`cd_override.cpp`) replaces the whole libcd read path with synchronous host disc I/O and NEVER
// touches these tables; the recomp gameplay path (core B with psx_fallback=1) fills them as libcd
// caches directory contents. Divergence here is CD-implementation noise: NO gameplay reader
// consumes these (only libcd's own primitives — FUN_8008BE?? / FUN_8008bf50 read/write; grep of
// scratch/decomp/ram_f1000_all.c shows no other consumers of 0x800AC2D4 or the cache tables).
// Only applies to modes where B runs PSX gameplay (fb_b=1): GAMEPLAY / FULL / ORACLE. In RENDER
// mode both cores run native gameplay, so no libcd path runs on either side.
bool Sbs::Impl::isCdCacheNoise(uint32_t a) const {
  if (mMode == M_RENDER) return false;
  // libcd flags cluster (drive-ready, media-serial, cached-dir index, dir handles).
  // Confirmed noise: 800AC2D4 (FUN_8008bf50 read/write, no other reader), 800AC2D8 (compared to
  // 800ABFD0 media-serial by the CD file-search primitive, sets on rescan; no gameplay reader).
  if (a >= 0x800AC280u && a < 0x800AC300u) return true;
  if (a >= 0x801026E8u && a < 0x80102790u) return true;   // per-cache-entry result buffer
  if (a >= 0x80102D44u && a < 0x80104344u) return true;   // 128-entry cache table (stride 0x2C)
  if (a >= 0x80104368u && a < 0x80104B80u) return true;   // directory-scratch sector buffer
  return false;
}

// libspu / libsnd voice-state cluster. FUN_80074B44 / FUN_8007496C / FUN_80092E3C write these; the
// only readers are the audio driver's own voice-mode primitives (libspu). Native gameplay
// (core A) drives audio via native_music + spu_audio (bypasses libspu voice tables); PSX
// gameplay (core B) runs libsnd which fills 800BE238+ per voice tick. Divergence here is audio-
// driver noise, not gameplay state.
bool Sbs::Impl::isAudioNoise(uint32_t a) const {
  if (mMode == M_RENDER) return false;
  if (a >= 0x800BE238u && a < 0x800BE360u) return true;   // 24-voice × 12-byte SPU state + mask
  return false;
}

// Object-list arena offset. In FULL / GAMEPLAY modes native gp (core A) and PSX substrate gp (core B)
// allocate their per-list-slot object nodes at different arena positions (their pool head starts at the
// same value 0x800F7734 but the two paths add objects at different rates during boot, so by field entry
// the list-head pointer array at 0x800ED550..0x800EDF00 stores the same object types at different
// addresses — verified 2026-07-03 in $CLAUDE_JOB_DIR/tmp/full5.log: both cores write the same head
// 0x800F7734 at f28/f30, then diverge as objects allocate). The divergence is a fixed-delta pointer
// offset (~10676 B on one repro = 157 × 68-byte objects); every entry in the pointer array inherits
// it. NO gameplay code reads a literal 0x800F77xx / 0x800F4Dxx address (grep -E '0x800F77[0-9a-fA-F]{2}
// |0x800F4D[0-9a-fA-F]{2}' game/ runtime/ = 0 hits), so the offset is invisible to game logic — it's
// implementation noise, not a gameplay bug. Only the LIST HEADS at 0x800ED550+ are excluded (~1KB); the
// actual object storage on the 0x800F0000 page is KEPT in the diff so real per-object state divergences
// surface. In RENDER mode both cores are native gp — no allocator divergence, keep the whole diff.
bool Sbs::Impl::isObjectPoolNoise(uint32_t a) const {
  if (mMode == M_RENDER) return false;
  if (a >= 0x800ED550u && a < 0x800EDF00u) return true;   // 3-list × N-slot pointer array (list heads)
  // Field-frame counter — engine_stage.cpp:393 unconditional `mem_w32(0x800BF878, prev+1)`. Native
  // gp (A) and PSX substrate gp (B) both tick it once per Engine::fieldFrame call, but the two
  // paths enter the field at slightly different boot frames (native reaches it one frame earlier
  // than the PSX coroutine yields into it), so the counter runs off-by-one for the whole session.
  // Bounded, timing-only, no gameplay reader consumes the exact value — filter in FULL/GAMEPLAY.
  if (a >= 0x800BF878u && a < 0x800BF87Cu) return true;
  return false;
}

// Legit render-only SCRATCHPAD workspace. In render/full mode the two cores run IDENTICAL gameplay but
// DIFFERENT render layers (A=native render, B=PSX recomp render). The render workspace addresses are
// documented in engine_submit.cpp / engine_project.cpp (#define SCR 0x1F800000):
//   0x1F800000..0x1F800100  GTE-compose work matrices, camera/RotMatrix, world-readout
//   0x1F800140..0x1F800160  per-frame render-list write-ptr / count / cap
// The GAP 0x1F800100..0x1F800140 is GAMEPLAY scratchpad (cutscene-active flag 0x1F800137, sub-mode bytes)
// and is STILL diffed, so real gameplay corruption is caught. Excluded ONLY when the render paths actually
// differ (render/full); gameplay mode renders PSX on both, so nothing here diverges — keep the FULL diff.
bool Sbs::Impl::isRenderSpad(uint32_t a) const {
  if (mMode == M_GAMEPLAY) return false;
  if (a >= 0x1F800000u && a < 0x1F800100u) return true;
  if (a >= 0x1F800140u && a < 0x1F800160u) return true;
  return false;
}

void Sbs::Impl::capBt(Core* c, char* buf, size_t n) {
  buf[0] = 0;
  FILE* f = fmemopen(buf, n, "w");
  if (f) { guest_backtrace_to(c, f); fclose(f); }
}

// 3-phase navigation to gameplay-start (concurrent per-core AUTO-NAV — identical shape to AUTO_SKIP /
// dualcore): (0) tap Cross until the GAME stage, (1) wait for the intro cutscene to begin, (2) pulse
// Start while the cutscene flag is up, finishing once it has read 0 for 60 consecutive frames (fade settled).
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
    case SKIP_CUT:
      if (cut) { nv.idle = 0; if ((f % 40u) == 0) c->game->pad.driveTap((uint16_t)(BTN_NONE & ~BTN_START), 6); }
      else if (++nv.idle >= 60) { fprintf(stderr, "[sbs] %s gameplay-start @f%u\n", tag, f); nv.phase = DONE; return true; }
      break;
    case DONE: return true;
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
  }
}

void Sbs::Impl::recordDivergence(uint32_t addr) {
  bool spad = isSpad(addr);
  uint32_t end_addr = spad ? 0x1F800400u : mHi;
  uint32_t last = addr, gap = 0;
  for (uint32_t x = addr + 1; x < end_addr && gap < 64; x++) {
    bool noise = spad ? isRenderSpad(x) : isDiffNoise(x);
    if (mA->core.mem_r8(x) != mB->core.mem_r8(x) && !noise) { last = x; gap = 0; } else gap++;
  }
  mDivFound = true; mDivFrame = mFrame; mDivAddr = addr; mDivEnd = last + 1;
  capBt(&mA->core, mBtA, sizeof mBtA);
  capBt(&mB->core, mBtB, sizeof mBtB);
  fprintf(stderr, "\n[sbs] *** DIVERGENCE at lockstep frame %u: 0x%08X..0x%08X (mode=%s) ***\n",
          mFrame, mDivAddr, mDivEnd, modeName());
  if (mHaveDbgsrv) {
    fprintf(stderr, "[sbs] paused. Inspect over the debug server: `sbs diff`, `sbs bt`, `sbs watch`.\n");
    mA->dbg_server.setPaused(true);
  } else {
    fprintf(stderr, "[sbs] (no debug server: logging and continuing; set PSXPORT_DEBUG_SERVER=1 to pause + `sbs diff`)\n");
  }
}

void Sbs::Impl::checkDivergence() {
  if (mDivFound) return;   // first-hit only: recordDivergence already captured + paused
  const uint8_t* a = mA->core.ram + (mLo - 0x80000000u);
  const uint8_t* b = mB->core.ram + (mLo - 0x80000000u);
  uint32_t n = mHi - mLo;
  for (uint32_t i = 0; i < n; i++) if (a[i] != b[i] && !isDiffNoise(mLo + i)) { recordDivergence(mLo + i); return; }
  for (uint32_t i = 0; i < 0x400; i++)
    if (mA->core.scratch[i] != mB->core.scratch[i] && !isRenderSpad(0x1F800000u + i)) { recordDivergence(0x1F800000u + i); return; }
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
  uint32_t nDiff = 0, firstAddr = 0, lastAddr = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (a[i] == b[i]) continue;
    if (isDiffNoise(mLo + i)) continue;
    if (!nDiff) firstAddr = mLo + i;
    lastAddr = mLo + i;
    pageCount[(mLo + i - 0x80000000u) >> PAGE_SHIFT]++;
    nDiff++;
  }
  uint32_t nSpad = 0;
  for (uint32_t i = 0; i < 0x400; i++) {
    if (mA->core.scratch[i] == mB->core.scratch[i]) continue;
    if (isRenderSpad(0x1F800000u + i)) continue;
    nSpad++;
  }
  if (nDiff == 0 && nSpad == 0) {
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
  fprintf(stderr, "\n");
}

// Step ONE core's frame for the SBS composite: diff_mode=1 suppresses its OWN per-core present/pace/audio
// (the SBS loop owns the single window present); sbs_render=1 re-enables the render-submit so it EMITS its
// geometry into VK batch `which` (gpu_gpu_select_target). Neither core presents; the SBS composite does.
void Sbs::Impl::stepCore(Game* g, int which) {
  g->core.game->diff_mode  = 1;
  g->core.game->sbs_render = 1;
  // PSXPORT_SBS intro-FMV: the OP.STR opening movie is OWNED by the native FMV player (skipped in SBS),
  // but the PSX core (B) runs the GUEST demo machine whose STR streamer strNext (FUN_8010755c) waits on
  // CD-streamed STR sectors that are NEVER fed here → busy-polls StGetNext ~2000x2000 times per attract
  // cycle, stalls the lockstep. Convert that async wait to a SYNC skip using the game's OWN skip-request
  // flag DAT_1f80019d (the demo machine's prologue forces teardown when set). Set only while the DEMO
  // stage is in its intro-FMV sub-state (SM[0x48]==1); the GAME opening cutscene is never skipped.
  { Core* c = &g->core;
    if (c->mem_r32(0x801fe00cu) == 0x801062E4u) {            // DEMO stage
      uint32_t sm = c->mem_r32(0x1f800138u);
      if (sm && c->mem_r16(sm + 0x48u) == 1)
        c->mem_w8(0x1f80019du, 1);
    } }
  applyMode(g, which);
  gpu_gpu_select_target(which);
  dc_step_frame(&g->core, mFrame);
}

// Render ONE core's just-emitted frame into the shared VK target HEADLESS and read it back to its CPU RGBA
// pane buffer (SDL_GPU window then draws it). Resets the VK geometry batch. Records the live display-region
// size for the SDL_GPU window upload.
void Sbs::Impl::grabPane(Game* g, uint8_t* rgba, int* ow, int* oh) {
  int sx, sy, w, h; gpu_disp_region(&g->core, &sx, &sy, &w, &h);
  if (w < 1) w = 1; if (h < 1) h = 1;
  if (w > 1024) w = 1024; if (h > 512) h = 512;
  gpu_gpu_render_readback(&g->core, gpu_vram_ptr(&g->core), sx, sy, w, h, rgba);
  gpu_gpu_frame_end(&g->core, gpu_vram_ptr(&g->core), (int)mFrame);
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
void Sbs::Impl::storeCb(Core* c, uint32_t a, uint32_t v) {
  // ALLOCTRACE: sniff writes to 0x800ED098 (free-slot count) — count per-frame decrements per core.
  // Fires INDEPENDENTLY of mWwArmed so it stays live across the whole run without arming a watch.
  // Exact-address check (a == 0x800ED098): word-aligned would count neighboring-byte writes too.
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
  if (which) { mWwVb = v; capBt(c, mWwBtB, sizeof mWwBtB); }
  else       { mWwVa = v; capBt(c, mWwBtA, sizeof mWwBtA); }
  mWwHit |= (1 << which);
  if (mWwPersist) {  // PREWATCH's continuous logging — per-store attribution to A vs B
    fprintf(stderr, "[sbs-ww] f%u %c wrote [%08X]=%08X (pc=%08X stage=%08X) [c=%p mA=%p mB=%p]\n",
            mFrame, which ? 'B' : 'A', a, v, c->pc, c->mem_r32(0x801fe00c),
            (void*)c, (void*)&mA->core, (void*)&mB->core);
    // Peek AFTER the actual host write, so we see the byte the store LANDED in. (mem_w8 does wwatch_check
    // BEFORE the write, so we peek RIGHT NOW = pre-store, but the write is imminent one-line below.)
    fprintf(stderr, "[sbs-ww]     pre-store peek A[%08X]=%u  B[%08X]=%u\n",
            a, mA->core.mem_r8(a), a, mB->core.mem_r8(a));
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
    uint32_t end = mDivEnd; if (end > mDivAddr + 24) end = mDivAddr + 24;
    fprintf(out, "  A:"); for (uint32_t x = mDivAddr; x < end; x++) fprintf(out, " %02X", mA->core.mem_r8(x));
    fprintf(out, "\n  B:"); for (uint32_t x = mDivAddr; x < end; x++) fprintf(out, " %02X", mB->core.mem_r8(x));
    fprintf(out, "\n");
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
  } else if (!strcmp(sub, "watch")) {
    unsigned addr = 0;
    if (sscanf(line, "%*s %*s %x", &addr) != 1) addr = mDivAddr;
    if (!addr) { fprintf(out, "sbs watch: no address (no divergence yet, give one: `sbs watch <hex>`)\n"); return 1; }
    mWwAddr = addr; mWwArmed = true; mWwHit = 0; mWwBtA[0] = mWwBtB[0] = 0;
    mA->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    mB->core.wwatch_arm(addr & ~3u, (addr & ~3u) + 4);
    fprintf(out, "write-watch armed on 0x%08X — `sbs resume`; the diverging write will re-pause with the site.\n", addr);
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
      if (a[i] != b[i] && !isDiffNoise(mLo + i)) {
        uint32_t start = mLo + i;
        while (i < n && a[i] != b[i] && !isDiffNoise(mLo + i)) { i++; bytes++; }
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
      if (mA->core.scratch[i] != mB->core.scratch[i] && !isRenderSpad(0x1F800000u + i)) {
        uint32_t start = 0x1F800000u + i;
        while (i < 0x400 && mA->core.scratch[i] != mB->core.scratch[i] && !isRenderSpad(0x1F800000u + i)) { i++; sbytes++; }
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
  } else {
    fprintf(out, "sbs subcommands: status | diff | bt | watch [hex] | show a|b | resume | step [n] | dump [path] | ramdiff [N] | allocra\n");
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

void Sbs::Impl::run(const char* exePath, Sbs* facade) {
  watchdog_disable();   // the SBS pauses indefinitely on a divergence for live inspection — not a hang

  // Mode selection (PSXPORT_SBS_MODE)
  const char* m = getenv("PSXPORT_SBS_MODE");
  if (m) {
    if (!strcmp(m, "gameplay"))               mMode = M_GAMEPLAY;
    else if (!strcmp(m, "full") || !strcmp(m, "both")) mMode = M_FULL;   // "both" = legacy alias
    else if (!strcmp(m, "oracle"))            mMode = M_ORACLE;
    else                                       mMode = M_RENDER;
  }
  { const char* e = getenv("PSXPORT_SBS_LO"); if (e && *e) mLo = (uint32_t)strtoul(e, 0, 0); }
  { const char* e = getenv("PSXPORT_SBS_HI"); if (e && *e) mHi = (uint32_t)strtoul(e, 0, 0); }

  // Install the write-watch callback + mark the harness active (native_fmv/native_boot gate off this).
  mem_set_store_watch_cb(&Sbs::storeCb);
  mSbs = true;

  { const char* e = getenv("PSXPORT_SBS_ALLOCTRACE"); if (e && *e && strcmp(e, "0") != 0) mAllocTraceOn = 1; }
  if (mAllocTraceOn) {
    fprintf(stderr, "[sbs] ALLOCTRACE on — per-frame decrement count of 0x800ED098 logged when A != B\n");
    // Register a per-ra bucket dump at process exit so the settled-state per-caller table lands even
    // when the run is killed by SIGTERM (SBS AUTONAV normally runs indefinitely). Guarded via mSelfPtr
    // so the atexit lambda can find the live Sbs::Impl without a global.
    static Sbs::Impl* s_selfForAtExit = nullptr;
    s_selfForAtExit = this;
    atexit([]{ if (s_selfForAtExit) s_selfForAtExit->dumpAllocRa(stderr); });
    // Also trap SIGTERM/SIGINT (the common shell-timeout / Ctrl-C path) so the settled-state per-ra
    // table lands under `timeout N …` too. Dump then call _exit — cheap, no atexit chain re-entry.
    static bool s_sigHooked = false;
    if (!s_sigHooked) {
      s_sigHooked = true;
      auto handler = +[](int sig){
        if (s_selfForAtExit) s_selfForAtExit->dumpAllocRa(stderr);
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
  mA = new Game(); mA->psx_fallback = 0; mA->sbs = facade;
  mB = new Game(); mB->psx_fallback = fb_b; mB->sbs = facade;
  if (mMode == M_ORACLE) { mB->core.use_interp = 1; mB->gpu.soft_gpu = 1; }
  load_exe(exePath, &mA->core); dc_boot_init(&mA->core);
  load_exe(exePath, &mB->core); dc_boot_init(&mB->core);
  fprintf(stderr, "[sbs] core-map A=%p B=%p (use to attribute [wwatch] lines)\n",
          (void*)&mA->core, (void*)&mB->core);

  // ALLOCTRACE arm — after Cores exist. wwatch_check only fires the store callback for armed
  // addresses; arm 0x800ED098 (word-aligned) on both cores so storeCb sees every write.
  if (mAllocTraceOn) {
    mA->core.wwatch_arm(0x800ED098u & ~3u, (0x800ED098u & ~3u) + 4);
    mB->core.wwatch_arm(0x800ED098u & ~3u, (0x800ED098u & ~3u) + 4);
  }

  // PSXPORT_SBS_PREWATCH=<hex> — arm SBS write-watch at boot so the FIRST divergent store to the
  // address is caught, not the first store AFTER the frame-boundary divergence pause (which happens
  // one frame late — you can never watch a write that already happened). Fires from frame 0.
  if (const char* w = getenv("PSXPORT_SBS_PREWATCH"); w && *w) {
    uint32_t addr = (uint32_t)strtoul(w, 0, 0);
    mWwAddr = addr; mWwArmed = true; mWwPersist = true; mWwHit = 0; mWwBtA[0] = mWwBtB[0] = 0;
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
    if (!nav_done) { navStep(&mA->core, mNavA, mFrame, "A"); navStep(&mB->core, mNavB, mFrame, "B"); }
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
      static uint32_t aP=0, bP=0;
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
    stepCore(mA, 0);              ww_log("post-stepA");
    grabPane(mA, mRgbaA, &mWa, &mHa); ww_log("post-grabA");
    stepCore(mB, 1);              ww_log("post-stepB");
    grabPane(mB, mRgbaB, &mWb, &mHb); ww_log("post-grabB");
    presentPanes();               ww_log("post-present");
    // Parity surface: with both cores past AUTO-NAV, name any RAM/scratchpad divergence. On the
    // FIRST byte that differs, `checkDivergence` records the range + backtraces + pauses (via the
    // debug server) so `sbs diff` / `sbs bt` / `sbs watch` can inspect. The 30-frame summary is
    // the running "how far apart are they" metric so you see divergence GROW even before the first
    // recorded hit (in render/full modes the render regions are excluded by design).
    if (nav_done) {
      summarizeDivergence(30);
      checkDivergence();
    }
    if (sbsDumpPath && nav_done && !dumped && mWa > 0 && mWb > 0) { dumpPpm(sbsDumpPath); dumped = true; }

    if (mWwArmed && mWwHit) {
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
        fprintf(stderr, "[sbs] write-watch caught 0x%08X (A=%08X B=%08X, mask=%d) at frame %u — paused; see `sbs bt`.\n",
                mWwAddr, mWwVa, mWwVb, mWwHit, mFrame);
        mWwArmed = false;
        mA->core.wwatch_arm(0, 0); mB->core.wwatch_arm(0, 0);
        mA->dbg_server.setPaused(true);
      }
      // Else: identical shared write in PREWATCH mode — silently continue and keep watching.
    }
    mFrame++;
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
void Sbs::storeCb(Core* c, uint32_t addr, uint32_t val) {
  if (c->game && c->game->sbs) c->game->sbs->mImpl->storeCb(c, addr, val);
}
Core*    Sbs::coreByLetter(char which) const                { return mImpl->coreByLetter(which); }
Core*    Sbs::shownCore() const                             { return mImpl->shownCore(); }
