// Resolved primitive construction is shared by live and isolated admission queues.
#include "render_queue.h"

#include "census_frame.h"
#include "game.h"
#include "host_backtrace.h"

#include <cstdlib>
#include <lucent/log.h>

namespace {
void observeSubmission(Core *core, int layer) {
  // ---- native graphics-producer census ---------------------------------------------------------
  // THE one chokepoint: drawWorldQuad and push2dQuad both funnel here, so counting once here covers
  // every native push and cannot double-count. An open ProducerScope names the producer; with none
  // open the key is NONE and the prim lands in unscopedNative() — real drawing by an UNDECLARED
  // producer, which is exactly the row the DB exists to surface. Never dropped, never charged to
  // whichever producer happened to be last. Host-only counters, no guest write: differential test-neutral.
  // `layer` is passed so an UNDECLARED push is recorded with the PASS it came from: the report then
  // ranks the undeclared work by layer and names which producer family to scope next, instead of only
  // reporting how much of it there is.
  // The scope's NAME travels with the key: for a PC-only producer the key is an interned hash, so the
  // name is the only thing that says which code the row belongs to — and holding the first name is what
  // lets the census DETECT two producers colliding on one iid instead of merging them silently.
  // GUEST-ORIGIN FIRST. A push made while the guest's own GP0 execution is on the stack is the guest's
  // prim, not an undeclared native producer, and the two must not share a counter: "undeclared native"
  // names remaining WORK, and a guest prim can never be declared, so mixing them made the number
  // unreachable on any leg that walks the guest OT and pointed the next reader at the one fix that would
  // mint a false row — a ProducerScope on a guest function.
  if (core->rsub.guestGp0Depth > 0) {
    core->rsub.census.noteGuestOriginPush(1u);
  } else {
    core->rsub.census.noteNativeLayer(
        core->rsub.producerScope.currentKey(), 1u, census_frame(core), layer, core->rsub.producerScope.currentName());
  }

  // WHO draws the undeclared prims — `PSXPORT_DEBUG=unscoped`.
  //
  // The census can say HOW MANY undeclared prims a layer holds; it cannot say WHICH C++ producer pushed
  // them, and without that the only way to shrink the number is to guess a file, scope it, and re-measure.
  // That guessing already cost a round: four producers were identified and scoped on solid evidence, and
  // the undeclared totals did not move by a single prim, because none of the four runs in that replay.
  // So capture the CALL SITE at the moment a prim arrives with no producer declared.
  //
  // Deduplicated by stack, and capped by NOVELTY rather than by count: every DISTINCT stack is printed
  // once, so a producer pushing 300k prims and one pushing 12 are equally visible. A plain "first N"
  // cap would have printed 8 lines of the same hot loop and hidden every other producer behind it.
  if (!core->rsub.producerScope.active() && core->rsub.guestGp0Depth == 0 && lucent::channel_on("unscoped")) {
    void *frames[24];
    const int n = backtrace(frames, 24);
    // Cheap order-sensitive hash of the return addresses — enough to tell distinct call sites apart.
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
      h ^= (uint64_t)(uintptr_t)frames[i];
      h *= 1099511628211ull;
    }
    static uint64_t seen[64];
    static int seenN = 0;
    bool fresh = true;
    for (int i = 0; i < seenN; i++) {
      if (seen[i] == h) {
        fresh = false;
        break;
      }
    }
    if (fresh) {
      if (seenN < (int)(sizeof seen / sizeof seen[0])) {
        seen[seenN++] = h;
      }
      char **sym = backtrace_symbols(frames, n);
      lucent::Line ln;
      ln.add("UNDECLARED native prim #{} layer={} — no ProducerScope open. Call site:\n", seenN, (int)layer);
      // Skip this function's own frame; print the producer chain above it.
      for (int i = 1; i < n && i < 12; i++) {
        ln.add("    {}\n", sym && sym[i] ? sym[i] : "?");
      }
      ln.flush(lucent::Level::Warn, "unscoped");
      free(sym);
    } else if (seenN >= (int)(sizeof seen / sizeof seen[0])) {
      // The table is full: say so ONCE rather than silently deduplicating against a truncated set,
      // which would read as "these are all the producers".
      static bool warned = false;
      if (!warned) {
        warned = true;
        lucent::warn("unscoped",
                     "distinct-call-site table FULL at {} entries — further NEW sites are "
                     "no longer reported. Scope what is listed and re-run.",
                     seenN);
      }
    }
  }
}
} // namespace

void RenderQueue::emitOrQueue(Core *core,
                              int capture,
                              int layer,
                              int order_mode,
                              int nv,
                              int semi,
                              int raw,
                              const int *xs,
                              const int *ys,
                              const float *xsf,
                              const float *ysf,
                              const int *us,
                              const int *vs,
                              const unsigned char *rs,
                              const unsigned char *gs,
                              const unsigned char *bs,
                              const float *depth,
                              int mode,
                              int tp_x,
                              int tp_y,
                              int clut_x,
                              int clut_y,
                              int tw_mx,
                              int tw_my,
                              int tw_ox,
                              int tw_oy,
                              int da_x0,
                              int da_y0,
                              int da_x1,
                              int da_y1,
                              int tp_blend,
                              const float (*sv)[3],
                              int sort_key,
                              float key_ord,
                              int shade_gouraud,
                              int dither,
                              PainterReplayOrder painter_replay,
                              uint32_t guest_packet,
                              uint32_t guest_ot_order) {
  if (observation == Observation::Live) {
    observeSubmission(core, layer);
  }

  // ---- WIDESCREEN 2D layout — the ONE layout authority for NATIVE screen-space producers (USER
  // 2026-07-16: dialog/prompt panels sat left-anchored in wide). The wide FB spans [0,ww) with the
  // world centred at ww/2, so a 4:3-authored x hugs the left edge until it is centred.
  //
  // WHICH SPACE the coordinates are in is DECLARED by the producer (RenderQueue::Space2dScope), not
  // inferred here. It used to be inferred, and the inference was wrong for every producer whose x
  // comes out of the widened projection rather than a 4:3 layout — see rq_2d_xform and kanban #73.
  // The rule itself lives in rq_2d_xform (hermetically tested); this is only its application.
  // 3D (RQ_OM_DEPTH) never enters. At 4:3 the transform is the identity.
  int wxs[4];
  float wxsf[4];
  {
    int gpu_vk_wide_engine(Core *), gpu_vk_wide_engine_w(Core *), gpu_vk_native_w(Core *);
    if (order_mode != RQ_OM_DEPTH && gpu_vk_wide_engine(core)) {
      // The material shape selects the background stretch: only a UNIFORM SOLID FILL (flat vertex
      // colour AND untextured) may be spread across the wide FB.
      const bool flat = rs && gs && bs && rs[0] == rs[1] && rs[1] == rs[2] && rs[2] == rs[3] && gs[0] == gs[1] &&
                        gs[1] == gs[2] && gs[2] == gs[3] && bs[0] == bs[1] && bs[1] == bs[2] && bs[2] == bs[3];
      const bool untextured = (!us || (us[0] == 0 && us[1] == 0 && us[2] == 0 && us[3] == 0)) &&
                              (!vs || (vs[0] == 0 && vs[1] == 0 && vs[2] == 0 && vs[3] == 0));
      const Rq2dXform t =
          rq_2d_xform(gpu_vk_wide_engine_w(core), gpu_vk_native_w(core), m2dSpace, layer, flat, untextured);
      for (int i = 0; i < nv; i++) {
        wxs[i] = t.apply(xs[i]);
        if (xsf) {
          wxsf[i] = t.applyf(xsf[i]);
        }
      }
      xs = wxs;
      if (xsf) {
        xsf = wxsf;
      }
    }
  }
  // Zero-init: only the later key-order resolver may promote authored_depth from ordinary real depth.
  RqItem it{};
  it.flush_ordinal = 0;
  it.layer = (uint8_t)layer;
  it.semi = semi ? 1 : 0;
  it.nv = (uint8_t)nv;
  it.raw = raw ? 1 : 0;
  it.order_mode = (uint8_t)order_mode;
  it.painter_object = mPainterObject;
  it.painter_replay = painter_replay;
  it.guest_packet = guest_packet;
  it.guest_ot_order = guest_ot_order;
  it.painter_flags = mPainterFlags;
  it.shade_gouraud = shade_gouraud ? 1 : 0;
  it.dither = (dither || (mPainterFlags & PAINTER_OBJECT_DITHER)) ? 1 : 0;
  // objid overlay: stamp the entity node the native render walk is currently rendering (submit.cpp).
  // Every world prim an object emits gets its node, so the overlay labels ALL rendered objects. Terrain/
  // static/background prims render with no per-object scope (mDbgRenderNode==0) → correctly unlabeled.
  // RQ_BACKGROUND also carries currentNode() (#54): Render::backdropRender scopes itself with
  // kBackdropDbgNode (render_queue.h) the same way world producers do, so Fps60::isTier1Owned can key on
  // ITS prims specifically. Any RQ_BACKGROUND item from OUTSIDE that scope (the generic guest-OT-walk bg
  // classification in gpu_native.cpp — no beginObject wraps it) still gets dbg_node==0, unchanged.
  it.dbg_node = (layer == RQ_WORLD || layer == RQ_BACKGROUND) ? core->rsub.diag.currentNode() : 0;
  it.sort_key = sort_key;
  it.key_ord = key_ord; // game's own OT sort key (kanban #11) — -1 = none
  // Shadow capture: an opaque world prim with view-space verts casts into the shadow map. Carried on the
  // item so emitItem re-pushes it to the shadow VBO on EVERY emit (= on both 60fps present passes).
  it.sh_cast = sv ? 1 : 0;
  if (sv) {
    for (int k = 0; k < 4; k++) {
      int s = k < nv ? k : nv - 1;
      it.sh_vx[k] = sv[s][0];
      it.sh_vy[k] = sv[s][1];
      it.sh_vz[k] = sv[s][2];
    }
  }
  it.has_xyf = (xsf && ysf) ? 1 : 0; // sub-pixel float XY (vertex smoothing) supplied by the world path
  for (int i = 0; i < nv; i++) {
    it.xs[i] = xs[i];
    it.ys[i] = ys[i];
    it.us[i] = us[i];
    it.vs[i] = vs[i];
    it.xsf[i] = it.has_xyf ? xsf[i] : (float)xs[i];
    it.ysf[i] = it.has_xyf ? ysf[i] : (float)ys[i];
    it.rs[i] = rs[i];
    it.gs[i] = gs[i];
    it.bs[i] = bs[i];
    it.depth[i] = depth ? depth[i] : 0.0f;
  }
  it.mode = mode;
  it.tp_x = tp_x;
  it.tp_y = tp_y;
  it.clut_x = clut_x;
  it.clut_y = clut_y;
  it.tw_mx = tw_mx;
  it.tw_my = tw_my;
  it.tw_ox = tw_ox;
  it.tw_oy = tw_oy;
  it.da_x0 = da_x0;
  it.da_y0 = da_y0;
  it.da_x1 = da_x1;
  it.da_y1 = da_y1;
  it.tp_blend = tp_blend;
  if (capture) {
    RqItem *slot = push();
    if (slot) {
      uint32_t sq = slot->seq;
      uint32_t dsq = slot->draw_seq;
      *slot = it;
      slot->seq = sq;
      slot->draw_seq = dsq;
    }
  } else {
    emitItem(core, &it);
  }
}
