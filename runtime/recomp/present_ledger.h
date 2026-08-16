// present_ledger.h — CAPTURED vs PRESENTED, per layer, per logic frame.
//
// THE GAP THIS CLOSES. Nothing in this project asserted that a prim a producer PUSHED actually reached
// the screen. The producer census counts prims that ARRIVE (and attributes them); it has no counter for
// prims that never arrive, no abort path, and its `prims_native_max` is monotonic-max-folded so a drop
// from N to 0 can never show. Measured 2026-08-16: the census run-end line read
// `prims seen 1728103 = attributed 1708014 + unscoped-native 20089` — fully green — while the ENTIRE 2D
// panel/prompt/dialog layer was missing from the screen (kanban #94/#35). Four render bugs were found
// that day and not one was caught by a test; all four were found by a person looking at the picture,
// days to weeks late.
//
// WHAT IT ASSERTS, and deliberately no more: a layer that CAPTURED prims this logic frame must EMIT
// prims in the real present. Not a conservation law — the world layer legitimately arrives from the
// present-time build rather than the capture (docs/one-renderer.md), so exact equality is not the
// invariant. "A whole layer was captured and then drawn by nobody" is, and that is the shape every one
// of these regressions actually had.
//
// DESIGN THE NEGATIVE FIRST (the project's own rule, and the reason this file exists at all): the three
// outputs are specified before the counters, and they are mutually distinguishable —
//   OK        : names the denominator, so a pass cannot be confused with a run that measured nothing.
//   DROPPED   : names the layer and both counts, and is an ERROR (fatal under PSXPORT_GATE_PRESENTATION).
//   NEVER FED : the case that makes silence dangerous — it must never read like OK.
// `selftest()` drives a captured-but-unpresented layer through the ledger and requires it to say
// DROPPED, so the instrument is shown producing the positive verdict before it is trusted to produce
// negatives.
#ifndef PRESENT_LEDGER_H
#define PRESENT_LEDGER_H
#include <stdint.h>
#include <lucent/log.h>

// RQ_BACKGROUND / RQ_WORLD / RQ_OVERLAY / RQ_HUD — the layer split render_queue.h owns. Kept as a bare
// count so this header does not depend on that one (it is included from it).
static const int kLedgerLayers = 4;

struct PresentLedger {
  long captured[kLedgerLayers] = {0, 0, 0, 0};   // prims the logic frame put into the capture
  long emitted [kLedgerLayers] = {0, 0, 0, 0};   // prims the REAL present handed to the rasterizer
  bool inRealPresent = false;                     // set only around the real (t=1) present pass
  long framesReconciled = 0;                      // the run-end denominator
  long framesDropped    = 0;

  void noteCaptured(int layer) { if ((unsigned)layer < kLedgerLayers) captured[layer]++; }
  void noteEmitted (int layer) { if (inRealPresent && (unsigned)layer < kLedgerLayers) emitted[layer]++; }

  void beginFrame() { for (int i = 0; i < kLedgerLayers; i++) captured[i] = emitted[i] = 0; }

  static const char* layerName(int l) {
    switch (l) { case 0: return "background"; case 1: return "world"; case 2: return "overlay";
                 case 3: return "hud"; default: return "?"; }
  }

  // Returns the number of layers that were captured and then presented by nobody. `fatal` makes a drop
  // abort rather than log, for the gate build.
  int reconcile(long frame, bool fatal) {
    int dropped = 0;
    long capTotal = 0, emitTotal = 0;
    for (int i = 0; i < kLedgerLayers; i++) {
      capTotal += captured[i]; emitTotal += emitted[i];
      if (captured[i] > 0 && emitted[i] == 0) dropped++;
    }
    framesReconciled++;
    if (!dropped) {
      // The denominator travels with the OK, so a green line cannot be confused with a frame in which
      // the ledger happened to see nothing at all.
      lucent::debug("ledger", "f{} OK — captured {} presented {} across {} layer(s); "
                    "bg {}/{} world {}/{} overlay {}/{} hud {}/{}",
                    frame, capTotal, emitTotal, kLedgerLayers,
                    emitted[0], captured[0], emitted[1], captured[1],
                    emitted[2], captured[2], emitted[3], captured[3]);
      return 0;
    }
    framesDropped++;
    lucent::error("ledger", "f{} DROPPED — {} layer(s) captured prims that the real present drew for "
                  "NOBODY. A producer that pushes prims nothing presents is INVISIBLE ON SCREEN; this "
                  "is not noise.", frame, dropped);
    for (int i = 0; i < kLedgerLayers; i++)
      if (captured[i] > 0 && emitted[i] == 0)
        lucent::error("ledger", "  layer {:<10} captured {:>6}  presented {:>6}", layerName(i),
                      captured[i], emitted[i]);
    lucent::error("ledger", "  totals this frame: captured {} presented {}. See docs/one-renderer.md — "
                  "the usual cause is a frame-build path that consumes only part of the capture.",
                  capTotal, emitTotal);
    if (fatal) abort();
    return dropped;
  }

  // The run-end line. The NEVER-FED case is the whole point: silence is what let every one of these
  // regressions ship, so "the ledger never ran" must be impossible to read as "nothing was dropped".
  void runEnd() const {
    if (!framesReconciled) {
      lucent::warn("ledger", "run-end: the ledger was NEVER FED — 0 frames reconciled. This does NOT "
                   "mean nothing was dropped. It means the counters are not wired, or this run never "
                   "reached a real present (diff_mode / early abort / no frame committed). This run "
                   "proves NOTHING about whether captured prims reach the screen.");
      return;
    }
    lucent::info("ledger", "run-end: {} frame(s) reconciled, {} with a dropped layer. Denominator: "
                 "every logic frame that reached the frame fence.", framesReconciled, framesDropped);
  }

  // Prove the instrument fires. Feeds a layer that is captured and never presented and requires
  // DROPPED; then feeds a clean frame and requires 0. Returns 0 on success.
  static int selftest() {
    PresentLedger L;
    L.beginFrame();
    L.inRealPresent = true;
    for (int i = 0; i < 3; i++) L.noteCaptured(2);   // overlay: 3 captured...
    for (int i = 0; i < 9; i++) { L.noteCaptured(1); L.noteEmitted(1); }   // ...world fine
    const int drops = L.reconcile(/*frame=*/0, /*fatal=*/false);
    if (drops != 1) {
      lucent::error("ledger", "SELFTEST FAILED: a captured-but-unpresented overlay layer reported {} "
                    "drop(s), expected 1. The instrument cannot see the bug it exists for.", drops);
      return 1;
    }
    L.beginFrame();
    for (int i = 0; i < 4; i++) { L.noteCaptured(3); L.noteEmitted(3); }
    if (L.reconcile(/*frame=*/1, /*fatal=*/false) != 0) {
      lucent::error("ledger", "SELFTEST FAILED: a clean frame reported a drop — the instrument cries "
                    "wolf, which is how a real drop gets ignored.");
      return 1;
    }
    lucent::info("ledger", "selftest OK: reports DROPPED on a captured-but-unpresented layer and clean "
                 "on a matched frame — both answers observed.");
    return 0;
  }
};

#endif
