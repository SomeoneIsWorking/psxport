#ifndef PSXPORT_PAINTER_BAND_DEPTH_H
#define PSXPORT_PAINTER_BAND_DEPTH_H

#include "painter_object_layer.h"

#include <span>
#include <stddef.h>
#include <stdint.h>

struct RqItem;
class Core;

// A console PSX has no depth buffer: an ordering table IS the answer to what covers what. A port that
// replays an authored OT and ALSO depth-tests its faces has two authorities, and where they disagree
// the depth buffer wins silently, because it runs per pixel after the draw order is already fixed.
//
// Measured (Spyro issue 0120, 2026-09-19): over 833,749 queued faces of one replay, every one of 655
// matched primitives authored the bin retail did — 100% — while `ord x bin`, which is one constant
// when a producer's depth and its bin come from the same view Z, measured 0.90, 1.06, 1.09, 1.26,
// 1.30, 2.31, 4.66 and 4.89 across eight producers, each tight inside its own p10..p90 band. Eight
// scales, one shared D32 buffer. The user's report — gems drawing through terrain — is one pair of
// that table: retail ordered gem 8016F0C8 at bin 171 BEHIND object 8016FA10 at bin 105, and the port
// submitted 0.012709 against 0.009630, so GREATER_OR_EQUAL put the gem in front.
//
// So a producer that replays an authored order may declare that its faces are BANDED: every face at
// one `ot_bin` is drawn at that bin's single depth, and the depth buffer then separates bins without
// ever contradicting the replay. `rq_apply_ot_lifo_depths` does exactly this for `sort_key` items;
// the painter path never reached it, because `render_queue.cpp` excludes `painter_object` items from
// that selection and every Spyro prim carries one.
//
// The band depth is the GAME's value, submitted in `RqItem::depth` as depth always has been, because
// only the game knows how its ordering table quantises view Z. `PainterReplayOrder::band_ord` is the
// DECLARATION of it, and this owner checks the queue against that declaration: one bin, one depth;
// farther bin, farther depth; and the submitted depth is the one that was declared. A producer that
// declares nothing keeps per-vertex depth and is counted, so "the game declined to own its order" and
// "this ran and saw nothing" are different answers.

struct PainterBandDepthResult {
  size_t commands_scanned = 0; // every command in every authored range
  size_t banded = 0;           // commands whose depth is their bin's declared band
  size_t kept_own_depth = 0;   // commands whose producer declared no band
  size_t bins = 0;             // distinct bins banded, summed over authored domains
  size_t domains = 0;          // authored domains seen

  enum class Fault : uint8_t {
    None = 0,
    InconsistentBin, // one bin carried two different band ords
    NonMonotone,     // a nearer-drawn bin was not given a nearer depth
    DepthNotBanded,  // the submitted vertex depths are not the declared band
  };
  Fault fault = Fault::None;
  size_t fault_command = SIZE_MAX;
  uint16_t fault_bin = 0;
  float fault_ord = 0.0f;
  float fault_other = 0.0f;

  bool ok() const {
    return fault == Fault::None;
  }
};

const char *painterBandDepthFaultName(PainterBandDepthResult::Fault fault);

// Check every command in `plan`'s authored domains against its declared band. `stream` is what the
// plan's `item_index` values address. Ordinary items and isolated-object ranges are not examined:
// they carry no frame-wide authored position to replay. Nothing is mutated.
PainterBandDepthResult rq_check_painter_band_depths(std::span<const RqItem *const> stream,
                                                    const PainterObjectPlan &plan);

// The shipping call: check, report on channel `painterband` with its denominators, and abort by name
// on a fault. Kept here rather than at the flush so the check, its verdict and its wording have one
// owner, and so `render_queue.cpp` does not grow another inline policy block.
void rq_verify_painter_band_depths(Core *core, std::span<const RqItem *const> stream, const PainterObjectPlan &plan);

#endif // PSXPORT_PAINTER_BAND_DEPTH_H
