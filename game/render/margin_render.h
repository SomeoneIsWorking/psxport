// Tomba2Engine — native widescreen-margin renderer (approach A, user-confirmed 2026-06-18).
//
// The game culls objects outside the 4:3 frustum, so at 16:9 the widened side/corner geometry is
// dropped. We must render those margin objects WITHOUT perturbing gameplay. RE (docs/journal.md
// later-133): each object node embeds its own render-command list (count@node+8, cmd-ptr array@
// node+0xc0[i]); a visible object is rendered by `gen_func_8003CDD8(node, flag)`, which composes
// camera×object transform and dispatches each persistent command into the OT. Poking the +1 render
// flag DOES render the margin but runs the handler's VISIBLE branch -> 5638 B of gameplay-state
// divergence (object structs). Widening the projection ALONE is gameplay 0-diff (4 render-pointer
// bytes). So: COLLECT the re-include-eligible nodes during the cull (no +1 poke), then after the
// entity walk call the per-node flush directly -> margin renders, gameplay untouched.
//
// One instance per Core, owned by Render (`rend(c)->margin`).
#ifndef PSXPORT_MARGIN_RENDER_HPP
#define PSXPORT_MARGIN_RENDER_HPP
#include <stdint.h>
#include <vector>
#include <unordered_set>
#include "core.h"

class MarginRenderer {
public:
  // True when the native-margin path is active (default): collect-and-flush instead of poking +1.
  // A/B fallback PSXPORT_MARGIN_POKE=1 keeps the old +1 re-include (perturbs gameplay; for diffing).
  // Latches the cfg + the `margin` debug channel on first call.
  int nativeEnabled();

  // Called from the cull for an object the wide frustum re-includes. Records the node for the
  // post-walk flush. Deduped per frame (the cull runs several times per object via the submit
  // wrappers). Takes the Core to read the node's type from this instance's RAM.
  void collect(Core* c, uint32_t node);

  // Called from ObjectList::walkAll AFTER both list walks. Renders each collected margin node via
  // gen_func_8003CDD8(node, 0), then clears the collection for the next frame.
  void flush(Core* c);

  bool dbg_ = false;

private:
  int mNativeEnabled = -1;    // lazy PSXPORT_MARGIN_POKE latch (-1 = not read yet)
  std::vector<uint32_t>        nodes_;
  std::unordered_set<uint32_t> seen_;
};
#endif
