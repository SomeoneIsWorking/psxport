// class RenderMode — the render-path COMPARE-MODE switches for one Core.
//
// Diagnostic instruments (NOT shipped behavior toggles): flip the field render between the PC-native
// world-coord path and the PSX-recomp path, and enable the dual-view side-by-side second render pass.
// Per-Core state so SBS / dualcore can set them independently on core A vs core B without contamination
// (was the process-globals g_render_psx / g_dualview; deglobalize-game 2026-07-02).
//
// Set at boot from PSXPORT_RENDER_PSX / PSXPORT_DUALVIEW; toggled live via the REPL (`renderpsx`) and
// the SBS/dualcore harness's per-core apply_mode. Reached as `core->mRender->mode`.
#pragma once

class RenderMode {
public:
  // Route the field render through the PSX recomp path instead of the native scene-walk.
  bool psxRender() const { return mPsxRender; }
  void setPsxRender(bool on) { mPsxRender = on; }

  // Dual-view: render ONE game state two ways side-by-side (engine-native left | PSX-recomp right).
  bool dualview() const { return mDualview; }
  void setDualview(bool on) { mDualview = on; }

  // pc_render DISPLAY-PASS guard (FAIL-FAST invariant, CLAUDE.md "READ-ONLY OVERLAY"): pc_render
  // reads guest RAM + engine state and draws to HOST memory only — it must NEVER write guest main
  // RAM or scratchpad. True only while the native picture-producing display pass (sceneNative() +
  // the native OT/queue draw it triggers, in game_tomba2.cpp's Engine::drawOTag) is executing on
  // THIS core. Core::mem_w8/16/32 (runtime/recomp/mem.cpp) check this and abort with a guest
  // backtrace on any guest-memory write while armed. Per-Core so SBS's two cores (and psx_render,
  // which never arms it) never cross-contaminate. Set/cleared ONLY via DisplayPassGuard (below) —
  // never toggled by hand — so an early return/exception can't leave it stuck on.
  bool displayPassArmed() const { return mDisplayPassArmed; }
  void setDisplayPassArmed(bool on) { mDisplayPassArmed = on; }

private:
  bool mPsxRender = false;
  bool mDualview  = false;
  bool mDisplayPassArmed = false;
};

// RAII scope guard for RenderMode::displayPassArmed(): arms it for the guard's lifetime and
// restores the PRIOR value on scope exit (nest-safe, exception/early-return safe). Construct one
// around pc_render's own picture-producing calls (sceneNative() + the native OT/queue draw), never
// around the substrate render orchestrator (Render::frame()/frameX()) — that legitimately writes
// the guest OT/packet-pool on both the pc_faithful and recomp_path cores.
class DisplayPassGuard {
public:
  explicit DisplayPassGuard(RenderMode& mode) : mMode(mode), mPrev(mode.displayPassArmed()) {
    mMode.setDisplayPassArmed(true);
  }
  ~DisplayPassGuard() { mMode.setDisplayPassArmed(mPrev); }
  DisplayPassGuard(const DisplayPassGuard&) = delete;
  DisplayPassGuard& operator=(const DisplayPassGuard&) = delete;
private:
  RenderMode& mMode;
  bool mPrev;
};
