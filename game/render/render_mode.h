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

private:
  bool mPsxRender = false;
  bool mDualview  = false;
};
