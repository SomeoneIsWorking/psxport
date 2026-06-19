// Tomba2Engine — native widescreen-margin renderer. See margin_render.hpp for the architecture.
// RE: docs/journal.md later-133, docs/engine_re.md "Deferred render pipeline".
#include "margin_render.hpp"
#include "cfg.h"
#include <vector>
#include <unordered_set>
#include <cstdio>

// mem_r8/mem_w8/rec_dispatch come from r3000.h (via margin_render.hpp).
int gpu_frame_no(Core*);                             // present-frame counter (gpu_native.cpp)

// The per-object render dispatch. RE later-133: gen_func_8003CCA4(a0=node) builds the object's transform
// (dispatching by node+0xd via the jump table @0x80014ec8) and calls the per-object flush
// gen_func_8003CDD8(node,flag) -> dispatcher -> per-mode renderer. This is the complete render the
// handler's visible branch would do, with NO gameplay logic. (Calling gen_func_8003CDD8 directly skips
// the transform-build -> degenerate zero-rotation matrix for never-visible culled objects.)
#define T2_PEROBJ_RENDER 0x8003CCA4u
// The per-object transform BUILDER. RE later-132/133: gen_func_80051C8C(a0=node) builds node+0x98 = the
// rotation matrix (from node euler angles +0x54/56/58) and node+0xac = translation (from position
// +0x2e/32/36), then propagates it to the command struct (cmd+0x18). The handler's visible branch runs
// this each frame; a culled (never-visible) object's matrix is stale/zero -> the flush composes a
// degenerate (zero-rotation) transform. We run it so the margin object gets its correct current
// transform. Reads CURRENT gameplay position/rotation; writes only the node's render-matrix cache.
#define T2_BUILD_XFORM   0x80051C8Cu
// Entity type (node+0xc) of the static world-geometry objects rendered via this per-object path.
// later-133: exactly these account for the +24 widescreen-margin commands at the field.
#define T2_WORLDGEO_TYPE 0x03

namespace {

class MarginRenderer {
public:
  // Record a re-include-eligible node (deduped within the frame). FILTER to entity type 0x03 (the
  // static world-geometry object type): later-133 proved that of all the wide-frustum re-include-
  // eligible objects, ONLY type-0x03 nodes actually render in the real +1 path — they render via the
  // per-object flush gen_func_8003CDD8(node, 0), and the 10 type-0x03 margin nodes reproduce EXACTLY
  // the +24 margin commands (matching geomblks). Other types (02/04/05/09) carry command lists too but
  // their handlers render through different paths (or not at all in the margin), so flushing them
  // over-renders. The type is the correct semantic gate, not a magic offset.
  void collect(Core* c, uint32_t node) {
    if (node == 0) return;
    if (c->mem_r8(node + 0xc) != T2_WORLDGEO_TYPE) return;   // only world-geometry objects render here
    if (!seen_.insert(node).second) return;               // already collected this frame
    nodes_.push_back(node);
  }

  // Render every collected node's persistent command list, then reset for the next frame.
  void flush(Core* c) {
    if (!nodes_.empty()) {
      // Preserve caller-saved arg registers around the dispatches (we are mid-walk-return).
      const uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
      for (uint32_t node : nodes_) {
        if (dbg_ && gpu_frame_no(c) == 2900)
          fprintf(stderr, "[margin]   node=%08x type=%02x cnt=%u\n",
                  node, (unsigned)c->mem_r8(node + 0xc), (unsigned)c->mem_r8(node + 8));
        // Call the FULL per-object render dispatch (gen_func_8003CCA4): it builds the object's transform
        // (cmd+0x18, stale for a never-visible culled object) AND flushes the command list. Calling the
        // low-level flush gen_func_8003CDD8 directly produced a degenerate (zero-rotation) transform
        // because the build runs only in the handler's visible branch we skip. This dispatch touches only
        // render scratch (0x1f80028c), not gameplay state.
        c->r[4] = node;                            // a0 = node — build the current transform first
        rec_dispatch(c, T2_BUILD_XFORM);
        c->r[4] = node;                            // a0 = node — then render the command list
        rec_dispatch(c, T2_PEROBJ_RENDER);
      }
      c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;

      if (dbg_) fprintf(stderr, "[margin] f%d rendered %zu margin nodes\n", gpu_frame_no(c), nodes_.size());
    }
    nodes_.clear();
    seen_.clear();
  }

  bool dbg_ = false;

private:
  std::vector<uint32_t>        nodes_;
  std::unordered_set<uint32_t> seen_;
};

MarginRenderer g_margin;

}  // namespace

int margin_native_enabled(void) {
  static int v = -1;
  if (v < 0) {
    // Default ON (this IS the widescreen margin path). PSXPORT_MARGIN_POKE=1 falls back to the old
    // +1 re-include (gameplay-perturbing) for A/B diffing.
    v = cfg_on("PSXPORT_MARGIN_POKE") ? 0 : 1;
    g_margin.dbg_ = cfg_dbg("margin") != 0;
  }
  return v;
}

void margin_collect(Core* c, uint32_t node) {
  g_margin.collect(c, node);
}

void margin_render_flush(Core* c) {
  g_margin.flush(c);
}
