#pragma once
// pace_plan — the FRAME-PACING DECISION, as a pure function of its inputs.
//
// WHY THIS IS A SEPARATE HEADER AND NOT JUST A BLOCK IN gpu_native.cpp. Two USER-flagged defects
// lived in that block, and both of them were invisible because the decision was tangled with a
// clock, a sleep and a window query:
//
//   1. `if (!gpu_has_window() || cfg_on("PSXPORT_NOPACE")) return;`
//      Headless was NEVER paced. Every headless timing number therefore described a program the
//      user never runs — and headless is where essentially every measurement in this project is
//      taken. `PSXPORT_NOPACE` was ALREADY the independent switch for "run unpaced", so the
//      `!gpu_has_window()` term was pure redundancy that coupled SPEED to WINDOWING. Speed is
//      orthogonal to windowing; if you want an unpaced run, ask for an unpaced run.
//
//      So `PaceInputs` HAS NO WINDOW FIELD. Leg-independence is not asserted by a test here, it is
//      structural: there is no input a window could change. The test file's job is to pin the fact
//      that the OLD rule (transcribed there) fails that property, so the header cannot quietly
//      regrow the term.
//
//   2. `double interval_ms = quota * 1000.0 / 60.0 / parts;`
//      A literal 60.000 Hz, while the CONSUMER of the pacing — a port's vblank counter — advances
//      at the game's REAL display field rate (NTSC is 60000/1001 = 59.940 Hz, not 60). Two clocks
//      at different rates across one wait loop is a beat, and a beat in a wait loop is what reaches
//      the screen. The rate is now an INPUT, taken from the standard the guest itself programmed
//      into GP1(0x08) bit 3 (gpu_native.cpp `gpu_field_rate_millihz`) — not a second literal, which
//      would have been the same bug with a different number.
//
// UNITS. Everything in milliseconds except the field rate, which is in MILLI-HERTZ because that is
// the unit the framework's other field-clock consumer already speaks (`rec_host_turn_register`) and
// because 60000/1001 has no exact representation in integer Hz.
//
// This header has NO includes and no state: it is safe to include from a test with nothing linked.

// The inputs to one pacing call. Deliberately minimal — and deliberately WITHOUT a window flag.
struct PaceInputs {
  // PSXPORT_NOPACE — the ONE switch that means "run as fast as the host can". It is the only thing
  // that may suppress pacing, in either leg.
  bool     unpaced = false;
  // GameConfig::paceQuota — how many DISPLAY FIELDS one pacing call represents. < 1 means the port
  // has not declared its cadence; that is a configuration defect, reported via quotaUnset rather
  // than guessed at silently.
  int      quota = 0;
  // Sub-frames per pacing call: 1 = pace a whole logic frame, 2 = half (fps60 presents twice per
  // logic frame). < 1 is clamped to 1.
  int      parts = 1;
  // The DISPLAY FIELD RATE in milli-hertz — 59940 for NTSC (60000/1001 Hz), 50000 for PAL. Zero is
  // not a rate: it is reported via rateUnset and produces no sleep, because dividing by it would be
  // the arithmetic equivalent of pacing on nothing.
  unsigned fieldRateMilliHz = 0;
  // Monotonic clock reading for this call, and the running deadline carried from the previous
  // paced call. `seeded` false means there has been no previous paced call, so the deadline starts
  // at now.
  double   nowMs = 0.0;
  double   nextMs = 0.0;
  bool     seeded = false;
};

// The decision. `nextMs` is only meaningful (and must only be stored back) when `paced` is true —
// a non-pacing call leaves the deadline exactly where it was, so that turning pacing off and on
// cannot inject a bogus catch-up.
struct PacePlan {
  bool   paced = false;       // did this call pace at all?
  bool   quotaUnset = false;  // quota < 1: the port has not derived its cadence (paced at 1 field)
  bool   rateUnset = false;   // fieldRateMilliHz == 0: no display clock to pace against
  double intervalMs = 0.0;    // the target wall-clock spacing of this (sub)frame
  double sleepMs = 0.0;       // 0 = the deadline has already passed; do not sleep
  double nextMs = 0.0;        // the deadline to carry forward (valid only when paced)
  bool   resync = false;      // the deadline was more than one interval in the past: drop the debt
};

inline PacePlan pace_plan(const PaceInputs& in) {
  PacePlan p;
  p.nextMs = in.nextMs;                       // untouched unless we actually pace

  // The ONE switch. Nothing else — not the presence of a window, not the render path, not the leg.
  if (in.unpaced) return p;

  // A zero field rate cannot be paced against. Say so instead of substituting a number: the whole
  // point of this change is that the pacing rate is never invented locally.
  if (in.fieldRateMilliHz == 0) { p.rateUnset = true; return p; }

  int parts = in.parts < 1 ? 1 : in.parts;
  int quota = in.quota;
  if (quota < 1) { p.quotaUnset = true; quota = 1; }

  p.paced = true;
  // fields / (fields per second) -> seconds -> ms. quota * 1e6 / millihz is that, without ever
  // materialising the rate in Hz (60000/1001 is not exact there).
  p.intervalMs = (double)quota * 1000000.0 / (double)in.fieldRateMilliHz / (double)parts;

  double next = (in.seeded ? in.nextMs : in.nowMs) + p.intervalMs;
  if (next > in.nowMs) {
    p.sleepMs = next - in.nowMs;
  } else if (in.nowMs - next > p.intervalMs) {
    // More than a whole interval late: the host hitched. Absorb the debt rather than sprinting to
    // catch up, which would run the game fast for as long as the debt lasted.
    next = in.nowMs;
    p.resync = true;
  }
  p.nextMs = next;
  return p;
}
