// wide_2d_layout — the 2D layout transform for ONE Core, from whichever mechanism widened it.
//
// WHY THIS EXISTS. `rq_2d_xform` (render_queue.h) has always been the widescreen 2D layout rule, and
// it is correct arithmetic: authored-4:3 coordinates are centred by (ww - native_w)/2, and a uniform
// untextured fill stretches instead. What was wrong was the QUESTION its one application site asked
// before applying it, and that question was answered from the host wide engine alone:
//
//     if (order_mode != RQ_OM_DEPTH && gpu_vk_wide_engine(core)) { ... }
//
// `gpu_vk_wide_engine` is `Mods::aspect != ASPECT_4_3 && RenderMode::enhancementsAllowed()`, and
// `enhancementsAllowed()` is `path == RenderPath::Native`. Every widescreen-only title declares
// `RenderCapabilities::widescreenOnly()`, whose default path is Gte — so on Mega Man X4, Tekken 3 and
// Tomba! 1 that conjunction is FALSE ON EVERY FRAME, the 2D rule never ran, and authored 4:3
// coordinates stayed in [0, native_w) while the title's own projection centre had moved.
//
// Measured on Mega Man X4 (issue 0019), which is what named the cause: the title's all-2D 320-wide
// composition begins at host x=0 while 4:3 content begins at x=164, and the frame's primitive dump is
// 635 sprites, 635/635 marked 2D and 0/635 marked 3D, so SetGeomOffset/OFX moves none of that screen.
// The rule would produce +54 for X4's 320 -> 428 (pinned by tests/test_rq_widen_2d.cpp:61).
//
// There are TWO widening mechanisms and this owner asks about both:
//
//   1. the host PC enhancement, `gpu_vk_wide_engine`, on a title that renders natively; and
//   2. the title-owned `GuestWidescreenProjection`, `gpu_vk_wide_presentation`, on a GTE-path title,
//      which is the whole point of the non-temporal guest-widescreen contract: the GUEST widens its
//      own projection and the host presents the wider canvas.
//
// Asking only about (1) is what made this invisible for so long: every widescreen measurement in the
// workspace so far came from a native-render title, where (1) is true.
//
// ONE ANSWER, BECAUSE THE QUESTION HAD TWO ANSWERS BEFORE. The rule is applied inside
// `RenderQueue::emitOrQueue` (render_submission.cpp). A second, older site maps sprite x in
// `gpu_native.cpp` before the queue — but that is the NATIVE sprite emitter, which a GTE-path title
// never reaches, so the two are on disjoint render paths and a Gte title cannot be shifted twice. That
// is a property worth stating rather than rediscovering: if a future change routes guest 2D through
// the native emitter as well, this owner must be consulted at ONE of them, not both.
#pragma once

#include "render_queue.h" // Rq2dXform, Rq2dSpace — the rule and the producer's declaration of space

class Core;

// The two widening mechanisms' answers, as the decision sees them. `host_engaged` is
// `gpu_vk_wide_engine` and `guest_engaged` is `gpu_vk_wide_presentation`; each width is that
// mechanism's own and is only meaningful when its mechanism is engaged. They are ALTERNATIVES: a title
// either renders natively and widens through the host engine, or it is a Gte-path title whose guest
// publishes the projection, and asking the other accessor on such a Core reads a plan that was never
// latched. So the host is taken first and the guest is the fallback, never a sum.
struct Wide2dExtent {
  int wide = 0;   // the presented width, or `native` when nothing widened
  int native = 0; // the game's own 4:3 width
};

// THE decision, as a pure function of those four facts, so it can be asked without a product. This
// is the seam the hermetic test drives; the Core-facing functions below only gather the facts.
Wide2dExtent wide_2d_extent(int host_wide, bool host_engaged, int guest_wide, bool guest_engaged, int native);

// Whether that extent is genuinely a widening. A plan that resolved narrow — ASPECT_AUTO against a 4:3
// sink, say — is NOT one, and must not lay the 2D layer out for nothing.
bool wide_2d_layout_active_for(int host_wide, bool host_engaged, int guest_wide, bool guest_engaged, int native);

// The transform for this Core's 2D layer, or the identity when its picture is not wider than the
// game's own 4:3 width. `space`, `layer`, `flat` and `untextured` are the producer's declaration of
// what it is drawing; they are not inferred here. `layer` is an Rq layer id, matching emitOrQueue.
Rq2dXform wide_2d_layout(Core &core, Rq2dSpace space, int layer, bool flat, bool untextured);

// Whether this Core's picture is genuinely wider than the game's own 4:3 width, by either mechanism.
bool wide_2d_layout_active(Core &core);
