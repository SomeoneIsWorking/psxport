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
  uint32_t mWwAddr  = 0;
  int      mWwHit   = 0;            // bit0 = A wrote, bit1 = B wrote
  uint32_t mWwVa = 0, mWwVb = 0;
  char     mWwBtA[4096] = {0}, mWwBtB[4096] = {0};

  // ---- scripted headless input (PSXPORT_SBS_KEYS) ----
  std::vector<SbsKey> mKeys;
  bool                mKeysParsed = false;

  // ---- navigation state (concurrent boot AUTO-NAV to free-roam) ----
  Nav mNavA, mNavB;

  // ---- helpers / stages ----
  const char* modeName() const;
  bool  isRenderRegion(uint32_t a) const;
  bool  isRenderSpad(uint32_t a) const;
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
    bool noise = spad ? isRenderSpad(x) : isRenderRegion(x);
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
  for (uint32_t i = 0; i < n; i++) if (a[i] != b[i] && !isRenderRegion(mLo + i)) { recordDivergence(mLo + i); return; }
  for (uint32_t i = 0; i < 0x400; i++)
    if (mA->core.scratch[i] != mB->core.scratch[i] && !isRenderSpad(0x1F800000u + i)) { recordDivergence(0x1F800000u + i); return; }
}

void Sbs::Impl::summarizeDivergence(uint32_t every) {
  if (!every || (mFrame % every) != 0) return;
  const uint8_t* a = mA->core.ram + (mLo - 0x80000000u);
  const uint8_t* b = mB->core.ram + (mLo - 0x80000000u);
  uint32_t n = mHi - mLo;
  uint32_t nDiff = 0, firstAddr = 0, lastAddr = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (a[i] == b[i]) continue;
    if (isRenderRegion(mLo + i)) continue;
    if (!nDiff) firstAddr = mLo + i;
    lastAddr = mLo + i;
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
  } else {
    fprintf(stderr, "[sbs] f%u: A/B differ in %u RAM bytes [0x%08X..0x%08X] + %u scratchpad bytes (mode=%s)\n",
            mFrame, nDiff, firstAddr, lastAddr, nSpad, modeName());
  }
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
  if (!mWwArmed || (a & ~3u) != (mWwAddr & ~3u)) return;
  int which = (mB && c == &mB->core) ? 1 : 0;
  if (which) { mWwVb = v; capBt(c, mWwBtB, sizeof mWwBtB); }
  else       { mWwVa = v; capBt(c, mWwBtA, sizeof mWwBtA); }
  mWwHit |= (1 << which);
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
    if (!mDivFound) { fprintf(out, "sbs: no divergence yet\n"); return 1; }
    fprintf(out, "== core A backtrace (frame-boundary, @divergence) ==\n%s", mBtA);
    fprintf(out, "== core B backtrace (frame-boundary, @divergence) ==\n%s", mBtB);
    if (mWwHit) {
      fprintf(out, "== WRITE SITE — core A wrote 0x%08X=%08X ==\n%s", mWwAddr, mWwVa, mWwBtA);
      fprintf(out, "== WRITE SITE — core B wrote 0x%08X=%08X ==\n%s", mWwAddr, mWwVb, mWwBtB);
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
      if (a[i] != b[i] && !isRenderRegion(mLo + i)) {
        uint32_t start = mLo + i;
        while (i < n && a[i] != b[i] && !isRenderRegion(mLo + i)) { i++; bytes++; }
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
  } else {
    fprintf(out, "sbs subcommands: status | diff | bt | watch [hex] | show a|b | resume | step [n] | dump [path] | ramdiff [N]\n");
  }
  return 1;
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

  // psx_fallback per mode: gameplay/full run PSX gameplay on core B; render runs native gameplay on both;
  // oracle runs the PURE interpreter+soft-rasterizer oracle on B (docs/oracle.md).
  int fb_b = (mMode == M_RENDER) ? 0 : 1;
  mA = new Game(); mA->psx_fallback = 0; mA->sbs = facade;
  mB = new Game(); mB->psx_fallback = fb_b; mB->sbs = facade;
  if (mMode == M_ORACLE) { mB->core.use_interp = 1; mB->gpu.soft_gpu = 1; }
  load_exe(exePath, &mA->core); dc_boot_init(&mA->core);
  load_exe(exePath, &mB->core); dc_boot_init(&mB->core);

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
    dbg.service(sel);
    bool nav_done = !sbsAutonav || (mNavA.phase == DONE && mNavB.phase == DONE);
    if (!nav_done) { navStep(&mA->core, mNavA, mFrame, "A"); navStep(&mB->core, mNavB, mFrame, "B"); }
    else feedInput();
    if (dbg.isPaused() && !dbg.stepPending()) {
      presentPanes();
      usleep(15000);
      continue;
    }
    if (dbg.stepPending()) dbg.consumeStep();

    mWwHit = 0;
    stepCore(mA, 0); grabPane(mA, mRgbaA, &mWa, &mHa);
    stepCore(mB, 1); grabPane(mB, mRgbaB, &mWb, &mHb);
    presentPanes();
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
      fprintf(stderr, "[sbs] write-watch caught 0x%08X (A=%08X B=%08X) at frame %u — paused; see `sbs bt`.\n",
              mWwAddr, mWwVa, mWwVb, mFrame);
      mWwArmed = false;
      mA->core.wwatch_arm(0, 0); mB->core.wwatch_arm(0, 0);
      mA->dbg_server.setPaused(true);
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
