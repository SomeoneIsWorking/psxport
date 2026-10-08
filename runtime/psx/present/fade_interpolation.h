// fade_interpolation.h — how two screen-fade endpoints resolve at an interpolation factor.
//
// A screen fade is a PRESENT-TIME COMPOSITE, not a queue item, so it is not carried by the captured
// frame and no producer reconstructs it. Both presents of a logic frame therefore used to read the
// title's CURRENT fade level and composite the same one, which puts the in-between present a whole
// frame early. During a fade that is the entire picture: measured on Tomba! 2's opening narration
// (2026-09-22), 66 consecutive real/interp/real triples had every changed pixel sitting at the next
// endpoint whatever t was, while 98.1% of the scene's objects were lerping correctly underneath.
// Nothing was moving; the level was, 8 steps of 255 per logic frame.
//
// This header owns the RESOLUTION only. Fps60 owns the endpoints and when they roll, and the title
// owns what its fade means. Keeping the policy separate is what makes it testable without a Core,
// a window or a game substrate.
#pragma once

#include "legacy_game_hooks.h" // FadeState

namespace psxport::fade {

// The fade to composite at `t`, given the previous logic frame's endpoint and this one's.
//
// Returns `cur` unchanged when there is no previous endpoint and when the two endpoints have
// different MODES. A mode change is a discontinuity rather than a ramp — fading to black and fading
// to white are not two points on one line — so blending through it would composite a colour the
// title never asked for. At t = 1 the arithmetic yields cur on its own; there is deliberately no
// branch for the real present.
FadeState resolve(const FadeState &prev, const FadeState &cur, bool havePrev, float t);

// The fade endpoints and the factor of the present in flight.
//
// DELIBERATELY NOT PART OF Fps60. A product with no temporal presentation still composites a fade,
// and the renderer must not acquire a link-time dependency on the interpolation tier to ask for it
// — psxport's own gate for that (tests/test_direct_runtime_no_temporal_link.cpp) failed the moment
// the renderer reached through Fps60 for this. With nothing driving it the endpoints stay empty and
// `resolveNow` returns the title's current state, which is exactly right for such a product.
class PresentFade {
public:
  // Roll the endpoints once per LOGIC frame, before either present of that frame runs. Capturing at
  // present time instead would read the same value twice, which is the defect this exists to fix.
  void capture(const FadeState &current);
  // The factor of the present now being drawn. 1 is the real present.
  void setFactor(float t) {
    factor_ = t;
  }
  // `current` is the title's live state; the result is it resolved against the previous endpoint.
  FadeState resolveNow(const FadeState &current) const;

  const FadeState &previous() const {
    return previous_;
  }
  const FadeState &current() const {
    return current_;
  }
  bool havePrevious() const {
    return havePrevious_;
  }

private:
  FadeState previous_{};
  FadeState current_{};
  // True from the SECOND capture on: after one, `previous_` is a default-constructed FadeState and
  // not a frame the title ever drew, so lerping from it would fade the opening frame in from
  // nothing.
  bool havePrevious_ = false;
  unsigned captures_ = 0;
  float factor_ = 1.0f;
};

} // namespace psxport::fade
