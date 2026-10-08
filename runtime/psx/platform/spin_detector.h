// spin_detector.h — fail fast when the guest hangs silently.
//
// THE BUG CLASS THIS KILLS (issue #25, Vagrant Story): the intro movie waits on a libcd result
// slot by polling inside ONE generated body while the CD layer streams sectors forever. No frame
// presents again, no function boundary is reached, so no event pump and no watchdog pet ever
// runs — the window freezes with a dead close button and tells you nothing.
//
// The guest-side definition of that state, general enough to be trusted and narrow enough to be
// believed: instruction time keeps being consumed while
//   * the host is OWED turns it never takes (PW_HOST stays set — no call boundary reached), and
//   * execution never leaves one ±32KB code region.
// When that persists for `max_run` consecutive windows of `window_ticks` instructions, it IS a
// spin; the caller fail-fasts naming the region (watchdog_spin_fault).
//
// Anything else resets the run: the host got serviced (healthy frame loop), or execution moved on
// (legitimate long compute walking other functions). Deliberately MAY-fail-to-detect rather than
// may-false-positive: a missed spin freezes a window; a wrong claim kills a working game.
#pragma once
#include <cstdint>

struct SpinDetectorState {
  uint64_t window_ticks = 0; // ticks accumulated toward the next decision
  uint32_t anchor = 0;       // pc of the first sample of the current run (0 = none yet)
  int run = 0;               // consecutive starved in-region decisions
};

// Feed one executed chunk. Returns true exactly when a spin is declared (the caller then reports
// and aborts; this function never aborts, so it stays hermetic-testable).
bool spin_detector_sample(
    SpinDetectorState &st, uint32_t pc, bool host_starved, uint32_t ticks, uint64_t window_ticks, int max_run);
