// class RenderMode — the RENDER PATH of one Core, plus the dual-view compare pass.
//
// Per-Core state so SBS / dualcore can set them independently on core A vs core B without contamination
// (was the process-globals g_render_psx / g_dualview; deglobalize-game 2026-07-02). Reached as
// `core->rsub.mode`. Design + rationale: docs/plans/render-path-tristate.md.
#pragma once
#include <strings.h> // strcasecmp — render_path_parse

// THE RENDER PATH — one enum answering the two questions a frame's picture depends on: who produced
// the geometry, and who rasterized it. USER, 2026-08-11: "need a toggle to switch between PC render
// native, PC render from GTE and pure PSX restraizer".
//
// It is ONE enum rather than three switches because the three switches it replaces (mPsxRender, the
// GpuState soft_gpu flag, and PSXPORT_ORACLE's enhancement lockout) could spell combinations that are
// not modes. "PSX rasterizer + native producers" is the worst of them: the native producers push
// geometry to the VK backend while the presenter shows the software framebuffer nothing rasterized
// into, i.e. a black screen. Deriving every answer from this enum makes that unspellable
// (tests/test_render_path.cpp).
enum class RenderPath {
  // The shipping picture: PC-native producers draw from game state, PC rasterizer, PC enhancements on.
  Native = 0,
  // The guest's OWN geometry — its GTE output walked out of the ordering table and replayed as GP0 —
  // rasterized by the PC. PURE: no PC enhancement may touch it (see enhancementsAllowed).
  Gte = 1,
  // The guest's own geometry AND the PSX software rasterizer, into s_vram. The reference picture.
  // Differs from Gte by EXACTLY ONE THING, the rasterizer, which is what makes the pair an A/B whose
  // difference is attributable to rasterization and nothing else.
  Psx = 2,
};

inline const char *render_path_name(RenderPath p) {
  switch (p) {
  case RenderPath::Native:
    return "native";
  case RenderPath::Gte:
    return "gte";
  case RenderPath::Psx:
    return "psx";
  }
  return "?";
}

// CYCLE to the next path — the one definition of "next", so the RmlUi selector and bare `renderpath`
// cannot drift into different orders. Native -> Gte -> Psx -> Native. The order is deliberate and is the
// order a comparison wants: the shipping picture, then the guest's own geometry on the PC rasterizer, then
// that same geometry on the PSX rasterizer — so consecutive presses isolate ONE variable at a time
// (producers, then rasterizer) instead of changing two things at once.
inline RenderPath render_path_next(RenderPath p) {
  switch (p) {
  case RenderPath::Native:
    return RenderPath::Gte;
  case RenderPath::Gte:
    return RenderPath::Psx;
  case RenderPath::Psx:
    return RenderPath::Native;
  }
  return RenderPath::Native;
}

// Parse a path NAME. Returns false and leaves *out untouched on anything it does not recognise — no
// prefix matching, no fallback to `native`: a knob whose value matched nothing must be reported as
// matching nothing (the CVar audit's rule), never silently resolved to the default.
inline bool render_path_parse(const char *s, RenderPath *out) {
  if (!s || !*s || !out) {
    return false;
  }
  if (!strcasecmp(s, "native")) {
    *out = RenderPath::Native;
    return true;
  }
  if (!strcasecmp(s, "gte")) {
    *out = RenderPath::Gte;
    return true;
  }
  if (!strcasecmp(s, "psx")) {
    *out = RenderPath::Psx;
    return true;
  }
  return false;
}

struct Core;
// render_path_install — resolve and install this Core's render path from configuration (the CVar
// ladder + the PSXPORT_RENDER_PSX alias + PSXPORT_ORACLE), and ANNOUNCE it. Called by every boot
// spine: native_boot_run, and a port whose boot does not go through it (spyro). One parser, one
// announce line. Definition in render_path.cpp.
void render_path_install(Core *c);

class RenderMode {
public:
  RenderPath path() const {
    return mPath;
  }
  void setPath(RenderPath p) {
    mPath = p;
  }

  // Route the field render through the PSX recomp path (the guest's own GTE+OT) instead of the native
  // scene-walk. DERIVED from the path — there is no independent setter, because "which geometry" and
  // "which rasterizer" have to agree.
  bool psxRender() const {
    return mPath != RenderPath::Native;
  }

  // Rasterize this Core's GP0 stream in SOFTWARE into s_vram (the tri()/raster_sprite() path) instead
  // of teeing prims to the VK backend. Was GpuState::soft_gpu, which only the SBS oracle leg and the
  // selftest could reach; it is a property of the render PATH, so it lives with the path.
  bool softGpu() const {
    return mPath == RenderPath::Psx;
  }

  // MAY A PC ENHANCEMENT TOUCH THIS CORE'S PICTURE? — fps60 interpolation, widescreen geometry,
  // internal-resolution scaling, the deferred SSAO/light/shadow passes, observer tagging.
  //
  // NATIVE-ONLY, and that is a USER decision, not an inference: *"I don't want GTE enhancements,
  // GTE/OT should stay pure"* and, when the consequence was put to them, *"Yes fps60/wide/native-depth
  // is supposed to be native-only"* (2026-08-11). So this is false for BOTH guest paths.
  //
  // Enhancements are gated HERE, at the read sites, rather than by mutating Mods (what
  // Game::setOracle's forceNeutral() does): a live toggle that clobbered the user's saved settings on
  // the way into `gte` could not restore them on the way back out. Mods stays the user's; this decides
  // whether the picture is allowed to honour it.
  bool enhancementsAllowed() const {
    return mPath == RenderPath::Native;
  }

  // Dual-view: render ONE game state two ways side-by-side (engine-native left | PSX-recomp right).
  bool dualview() const {
    return mDualview;
  }
  void setDualview(bool on) {
    mDualview = on;
  }

  // pc_render DISPLAY-PASS guard (FAIL-FAST invariant, CLAUDE.md "READ-ONLY OVERLAY"): pc_render
  // reads guest RAM + engine state and draws to HOST memory only — it must NEVER write guest main
  // RAM or scratchpad. True only while the native picture-producing display pass (sceneNative() +
  // the native OT/queue draw it triggers, in game_tomba2.cpp's Engine::drawOTag) is executing on
  // THIS core. Core::mem_w8/16/32 (runtime/recomp/mem.cpp) check this and abort with a guest
  // backtrace on any guest-memory write while armed. Per-Core so SBS's two cores (and psx_render,
  // which never arms it) never cross-contaminate. Set/cleared ONLY via DisplayPassGuard (below) —
  // never toggled by hand — so an early return/exception can't leave it stuck on.
  bool displayPassArmed() const {
    return mDisplayPassArmed;
  }
  void setDisplayPassArmed(bool on) {
    mDisplayPassArmed = on;
  }

private:
  RenderPath mPath = RenderPath::Native; // the shipping picture is the default
  bool mDualview = false;
  bool mDisplayPassArmed = false;
};

// RAII scope guard for RenderMode::displayPassArmed(): arms it for the guard's lifetime and
// restores the PRIOR value on scope exit (nest-safe, exception/early-return safe). Construct one
// around pc_render's own picture-producing calls (sceneNative() + the native OT/queue draw), never
// around the substrate render orchestrator (Render::frame()/frameX()) — that legitimately writes
// the guest OT/packet-pool on both the pc_faithful and recomp_path cores.
class DisplayPassGuard {
public:
  explicit DisplayPassGuard(RenderMode &mode) : mMode(mode), mPrev(mode.displayPassArmed()) {
    mMode.setDisplayPassArmed(true);
  }
  ~DisplayPassGuard() {
    mMode.setDisplayPassArmed(mPrev);
  }
  DisplayPassGuard(const DisplayPassGuard &) = delete;
  DisplayPassGuard &operator=(const DisplayPassGuard &) = delete;

private:
  RenderMode &mMode;
  bool mPrev;
};
