// render_capabilities.h — one title-owned policy for renderer and temporal choices.
//
// A render path existing in the framework does not mean every title implements it.  In particular,
// an already-60fps widescreen-only title owns no native producers and no interpolation product.  This
// policy is consumed by startup configuration, live diagnostics, and the player menu so none of those
// surfaces can invent a capability the title did not declare.
#pragma once

#include "render_mode.h"

class Game;

enum class RenderPathAudience {
  Player,
  Diagnostic,
};

enum class RenderPathSelectionResult {
  Applied,
  Unsupported,
};

// The PSX authored polygon order in an ordering table. Depth is the PC-native enhancement;
// Authored reproduces the title's frame-wide OT buckets and AddPrim insertion order.
enum FaceOrder {
  FACE_ORDER_DEPTH = 0,
  FACE_ORDER_AUTHORED = 1,
};

struct RenderCapabilities {
  RenderPath defaultPath = RenderPath::Native;
  bool nativeRenderPath = true;
  bool temporalInterpolation = false;
  FaceOrder defaultFaceOrder = FACE_ORDER_DEPTH;

  static constexpr RenderCapabilities direct() {
    return {};
  }

  static constexpr RenderCapabilities interpolatedNative(FaceOrder defaultFaceOrder = FACE_ORDER_DEPTH) {
    return {
        .defaultPath = RenderPath::Native,
        .nativeRenderPath = true,
        .temporalInterpolation = true,
        .defaultFaceOrder = defaultFaceOrder,
    };
  }

  static constexpr RenderCapabilities widescreenOnly() {
    return {
        .defaultPath = RenderPath::Gte,
        .nativeRenderPath = false,
        .temporalInterpolation = false,
        .defaultFaceOrder = FACE_ORDER_DEPTH,
    };
  }

  // Widescreen-only geometry, PLUS a temporal product whose in-between fields are made of the GUEST'S
  // OWN captured primitives, interpolated between two real frames by projection provenance (see
  // InBetweenStrategy::interpolatesGuestGeometry). Gte remains the shipping path and the real frame is
  // presented exactly as `widescreenOnly()` ships it. `interpolatedNative()` is the other shape — Native
  // producers AND interpolation — and neither implies the other.
  static constexpr RenderCapabilities guestInterpolated() {
    return {
        .defaultPath = RenderPath::Gte,
        .nativeRenderPath = false,
        .temporalInterpolation = true,
        .defaultFaceOrder = FACE_ORDER_DEPTH,
    };
  }

  constexpr bool supports(RenderPath path) const {
    return path != RenderPath::Native || nativeRenderPath;
  }

  constexpr bool playerSelectable(RenderPath path) const {
    return path != RenderPath::Device && path != RenderPath::Record && supports(path);
  }

  constexpr int playerPathCount() const {
    return (playerSelectable(RenderPath::Native) ? 1 : 0) + (playerSelectable(RenderPath::Gte) ? 1 : 0);
  }
};

// Resolve a launch request without guessing a game-specific fallback. A capability's default is the
// title's shipping answer; the GTE path is the final invariant fallback because every runtime supports
// guest geometry and the widescreen-only profile deliberately ships it.
constexpr RenderPath render_path_resolve(RenderPath requested, const RenderCapabilities &capabilities) {
  if (capabilities.supports(requested)) {
    return requested;
  }
  if (capabilities.supports(capabilities.defaultPath)) {
    return capabilities.defaultPath;
  }
  return RenderPath::Gte;
}

constexpr RenderPath
render_path_next_supported(RenderPath current, const RenderCapabilities &capabilities, RenderPathAudience audience) {
  RenderPath candidate = current;
  for (int i = 0; i < 4; ++i) {
    candidate = render_path_next(candidate);
    const bool allowed = audience == RenderPathAudience::Player ? capabilities.playerSelectable(candidate)
                                                                : capabilities.supports(candidate);
    if (allowed) {
      return candidate;
    }
  }
  return render_path_resolve(current, capabilities);
}

// The one live-selection validator. Player selection additionally excludes the diagnostic device
// and record pictures.
RenderPathSelectionResult render_path_apply(Game &game, RenderPath requested, RenderPathAudience audience);

// Forgets a Core that a live `render path` switch was addressed to, so the remembered pointer cannot
// outlive the machine it names. Called from ~Core: the switch is per-Core state held in a
// process-global, and a dangling Core there would hand the next Core somebody else's path.
void render_path_forget(const Core *core);
