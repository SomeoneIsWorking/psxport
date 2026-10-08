// RenderMode owns the render path and display-pass write guard for one Core.
#pragma once
#include <strings.h> // strcasecmp — render_path_parse

// THE RENDER PATH — who produced the geometry, and who rasterized it. One enum, so no combination of
// switches can name a mode that does not exist (tests/test_render_path.cpp).
enum class RenderPath {
  // The shipping picture: PC-native producers draw from game state, PC rasterizer, PC enhancements on.
  Native = 0,
  // The guest's OWN geometry — its GTE output walked out of the ordering table and replayed as GP0 —
  // rasterized by the PC. PURE: no PC enhancement may touch it (see enhancementsAllowed).
  Gte = 1,
  // The guest's own geometry drawn by the GPU device (Beetle), its display area presented as is.
  // Differs from Gte only in the rasterizer.
  Device = 2,
  // The device's executed GP0 work replayed from the frame record into one VRAM image at scale.
  Record = 3,
};

inline const char *render_path_name(RenderPath p) {
  switch (p) {
  case RenderPath::Native:
    return "native";
  case RenderPath::Gte:
    return "gte";
  case RenderPath::Device:
    return "device";
  case RenderPath::Record:
    return "record";
  }
  return "?";
}

// CYCLE to the next path — the one definition of "next", so the RmlUi selector and bare `renderpath`
// cannot drift into different orders. Native -> Gte -> Device -> Record -> Native: consecutive presses
// change one variable at a time (producers, then rasterizer).
inline RenderPath render_path_next(RenderPath p) {
  switch (p) {
  case RenderPath::Native:
    return RenderPath::Gte;
  case RenderPath::Gte:
    return RenderPath::Device;
  case RenderPath::Device:
    return RenderPath::Record;
  case RenderPath::Record:
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
  if (!strcasecmp(s, "device")) {
    *out = RenderPath::Device;
    return true;
  }
  if (!strcasecmp(s, "record")) {
    *out = RenderPath::Record;
    return true;
  }
  return false;
}

class Core;
// render_path_install — resolve and install this Core's render path from configuration (the CVar
// configuration ladder, and announce it. Called by every boot
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

  // Route the field render through the PSX guest path (the guest's own GTE+OT) instead of the native
  // scene-walk. DERIVED from the path — there is no independent setter, because "which geometry" and
  // "which rasterizer" have to agree.
  bool psxRender() const {
    return mPath != RenderPath::Native;
  }

  // MAY A PC ENHANCEMENT TOUCH THIS CORE'S PICTURE? — fps60 interpolation, host-owned widescreen geometry,
  // internal-resolution scaling, observer tagging.
  //
  // NATIVE-ONLY, and that is a USER decision, not an inference: *"I don't want GTE enhancements,
  // GTE/OT should stay pure"* and, when the consequence was put to them, *"Yes fps60/wide/native-depth
  // is supposed to be native-only"* (2026-08-11). So this is false for BOTH guest paths. A title-owned
  // GTE projection may separately publish its matching host extent through guestWidescreenAllowed;
  // that contract does not enable interpolation, internal resolution or deferred native passes.
  //
  // Enhancements are gated HERE, at the read sites, rather than by mutating Mods (what
  // Game::setOracle's forceNeutral() does): a live toggle that clobbered the user's saved settings on
  // the way into `gte` could not restore them on the way back out. Mods stays the user's; this decides
  // whether the picture is allowed to honour it.
  bool enhancementsAllowed() const {
    return mPath == RenderPath::Native;
  }
  // Record rasterizes the device's own work at the internal resolution; nothing else changes.
  bool internalResolutionAllowed() const {
    return mPath == RenderPath::Native || mPath == RenderPath::Record;
  }

  // MAY A TITLE-DECLARED GUEST WIDENING EXPOSE ITS WIDE PRESENTATION EXTENT? This is not the broad
  // PC-enhancement permission above. On Gte the title changes its own projection/culling/layout and
  // latches it; on Record the canvas extends the clip around the displayed buffer. Device remains the
  // untouched 4:3 reference, and Native uses the existing host-owned widescreen pipeline.
  bool guestWidescreenAllowed() const {
    return mPath == RenderPath::Gte || mPath == RenderPath::Record;
  }

  // pc_render DISPLAY-PASS guard (FAIL-FAST invariant, CLAUDE.md "READ-ONLY OVERLAY"): pc_render
  // reads guest RAM + engine state and draws to HOST memory only — it must NEVER write guest main
  // RAM or scratchpad. True only while the native picture-producing display pass (sceneNative() +
  // the native OT/queue draw it triggers, in game_tomba2.cpp's Engine::drawOTag) is executing on
  // THIS core. Core::mem_w8/16/32 (runtime/psx/core/mem.cpp) check this and abort with a guest
  // backtrace on any guest-memory write while armed. Per-Core so differential test's two cores (and psx_render,
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
  bool mDisplayPassArmed = false;
};

// RAII scope guard for RenderMode::displayPassArmed(): arms it for the guard's lifetime and
// restores the PRIOR value on scope exit (nest-safe, exception/early-return safe). Construct one
// around pc_render's own picture-producing calls (sceneNative() + the native OT/queue draw), never
// around the substrate render orchestrator (Render::frame()/frameX()) — that legitimately writes
// the guest OT/packet-pool on both the pc_faithful and guest_path cores.
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
