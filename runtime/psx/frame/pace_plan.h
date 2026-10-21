// pacePlan: the frame-pacing decision as a pure function of its inputs. Times are in milliseconds,
// the field rate in milli-hertz (field_rate.h).
#pragma once

#include "field_rate.h"

#include <cmath>

namespace psx::frame {

// No window field: whether a run is paced never depends on whether it has a window.
struct PaceInputs {
  // PSXPORT_NOPACE or a fast-forward: the only things that suppress pacing.
  bool unpaced = false;
  // Display fields one call represents, from the title's frame driver; < 1 is reported as quotaUnset.
  int quota = 0;
  // Sub-frames per call: 1 paces a whole logic frame, 2 half of one (fps60). < 1 is clamped to 1.
  int parts = 1;
  // 59940 for NTSC, 50000 for PAL; zero is reported as rateUnset and never paced against.
  unsigned fieldRateMilliHz = 0;
  // Monotonic now, and the deadline carried from the previous paced call (unseeded: start at now).
  double nowMs = 0.0;
  double nextMs = 0.0;
  bool seeded = false;
};

// `nextMs` is stored back only when `paced`, so turning pacing off and on injects no catch-up.
struct PacePlan {
  bool paced = false;
  bool quotaUnset = false; // quota < 1: the title passed no cadence (paced at 1 field)
  bool rateUnset = false;  // no display field rate to pace against
  int effectiveQuota = 0;  // normalized display fields represented by this call
  int effectiveParts = 0;  // normalized subdivisions of that field quota
  double intervalMs = 0.0; // the target wall-clock spacing of this (sub)frame
  double sleepMs = 0.0;    // 0 = the deadline has already passed
  double nextMs = 0.0;     // the deadline to carry forward (valid only when paced)
  bool resync = false;     // the deadline was more than one interval in the past: whole intervals are dropped
};

inline PacePlan pacePlan(const PaceInputs &in) {
  PacePlan p;
  p.nextMs = in.nextMs;

  p.effectiveParts = in.parts < 1 ? 1 : in.parts;
  p.effectiveQuota = in.quota;
  if (p.effectiveQuota < 1) {
    p.quotaUnset = true;
    p.effectiveQuota = 1;
  }
  p.rateUnset = in.fieldRateMilliHz == 0;
  if (in.unpaced || p.rateUnset) {
    return p;
  }

  p.paced = true;
  p.intervalMs = fieldIntervalMs(
      in.fieldRateMilliHz, static_cast<unsigned>(p.effectiveQuota), static_cast<unsigned>(p.effectiveParts));

  double next = (in.seeded ? in.nextMs : in.nowMs) + p.intervalMs;
  if (next > in.nowMs) {
    p.sleepMs = next - in.nowMs;
  } else if (in.nowMs - next > p.intervalMs) {
    // A host hitch: absorb the debt in whole intervals, so the deadlines keep their place on the vblank grid.
    next += std::ceil((in.nowMs - next) / p.intervalMs) * p.intervalMs;
    p.sleepMs = next - in.nowMs;
    p.resync = true;
  }
  p.nextMs = next;
  return p;
}

} // namespace psx::frame
