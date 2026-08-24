#pragma once

#include <utility>

// The standalone native-loop boundary between the frame captured last tick and the frame whose
// guest work is about to start. Callbacks keep this ownership rule hermetic-testable without a disc,
// GPU, window, or game-specific Core.
template <typename PresentPending, typename BeginCapture, typename ApplyWarp, typename RunGuestFrame>
void standalone_frame_boundary(PresentPending &&presentPending,
                               BeginCapture &&beginCapture,
                               ApplyWarp &&applyWarp,
                               RunGuestFrame &&runGuestFrame) {
  std::forward<PresentPending>(presentPending)();
  std::forward<BeginCapture>(beginCapture)();
  // A cold warp replaces the guest scene wholesale. It therefore belongs after every consumer of
  // the pending old-scene capture and before any producer starts the destination scene's capture.
  std::forward<ApplyWarp>(applyWarp)();
  std::forward<RunGuestFrame>(runGuestFrame)();
}
