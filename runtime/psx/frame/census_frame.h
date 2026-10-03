// census_frame.h — THE ONE frame number the producer census stamps rows with.
//
// It exists because the two feed sites (the native render queue's chokepoint and the guest leg's GP0
// census) each reached for the frame counter nearest to hand, and both reached for the WRONG one:
// GpuState::s_frame counts PRESENTS. On a leg where presents are rare or paced differently from logic —
// which is every guest leg — every row stamped the same number, and the census reported `frames 1 (f3..f3)`
// for producers that drew for hundreds of frames. Measured 2026-08-12: all 14 guest rows of a 300-frame
// run read `f3..f3`.
//
// THE SAME ROOT CAUSE WAS ALREADY FIXED ONCE, one layer down: OtAttr's span table used to reset off
// s_frame, "so on a path where presents are rare it never fired and the table saturated with stale spans"
// (ot_attr.cpp). Fixing that reset without fixing the row STAMP left the identical defect in the field a
// human reads. Hence one shared definition rather than a second correct-looking local choice.
//
// The logic frame advances once per title-owned FrameDriver step, which is the tick a producer's lifetime is actually
// measured in: "this effect drew for 54 frames" is a statement about game time, not about how often the
// host chose to present.
#pragma once
#include "core.h"
#include "game.h"
#include <stdint.h>

inline uint32_t census_frame(Core *c) {
  return c->game->timing.logicFrame;
}
