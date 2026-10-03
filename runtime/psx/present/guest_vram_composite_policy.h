#pragma once

struct GuestVramCompositePlan {
  bool rebuildForOwnership = false;
  bool uploadWholeVram = false;
};

// Per-Game ownership latch for the persistent host composite. Guest VRAM can be picture content for
// one frame and texture storage for the next, so changing ownership invalidates the existing
// composite even when neither geometry nor VRAM write counters changed.
class GuestVramCompositePolicy {
public:
  GuestVramCompositePlan plan(bool guestVramIsPicture) const {
    const bool changed = !built_ || builtGuestVramIsPicture_ != guestVramIsPicture;
    return {
        .rebuildForOwnership = changed,
        .uploadWholeVram = changed && guestVramIsPicture,
    };
  }

  void didBuild(bool guestVramIsPicture) {
    built_ = true;
    builtGuestVramIsPicture_ = guestVramIsPicture;
  }

private:
  bool built_ = false;
  bool builtGuestVramIsPicture_ = false;
};
