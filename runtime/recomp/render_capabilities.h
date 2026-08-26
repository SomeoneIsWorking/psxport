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
  ReferenceLocked,
};

struct RenderCapabilities {
  RenderPath defaultPath = RenderPath::Native;
  bool nativeRenderPath = true;
  bool temporalInterpolation = false;

  static constexpr RenderCapabilities direct() {
    return {};
  }

  static constexpr RenderCapabilities interpolatedNative() {
    return {
        .defaultPath = RenderPath::Native,
        .nativeRenderPath = true,
        .temporalInterpolation = true,
    };
  }

  static constexpr RenderCapabilities widescreenOnly() {
    return {
        .defaultPath = RenderPath::Gte,
        .nativeRenderPath = false,
        .temporalInterpolation = false,
    };
  }

  constexpr bool supports(RenderPath path) const {
    return path != RenderPath::Native || nativeRenderPath;
  }

  constexpr bool playerSelectable(RenderPath path) const {
    return path != RenderPath::Psx && supports(path);
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
  for (int i = 0; i < 3; ++i) {
    candidate = render_path_next(candidate);
    const bool allowed = audience == RenderPathAudience::Player ? capabilities.playerSelectable(candidate)
                                                                : capabilities.supports(candidate);
    if (allowed) {
      return candidate;
    }
  }
  return render_path_resolve(current, capabilities);
}

// The one live-selection validator. Player selection additionally excludes the diagnostic software
// rasterizer; both player and diagnostic controls are locked while a Game is a reference leg.
RenderPathSelectionResult render_path_apply(Game &game, RenderPath requested, RenderPathAudience audience);
