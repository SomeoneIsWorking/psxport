// Engine-owned render queue — see render_queue.h. Per-instance state lives on Game (game.h);
// the free rq_* API forwards to core->game->rq.
#include "render_queue.h"
#include "game.h"
#include "cfg.h"
#include <algorithm>
#include <stdio.h>

// The render queue is THE render path — one behavior, the PC game. No env gate (user directive
// 2026-06-20: "have only one behavior that is PC game"). The lone exception is the PSXPORT_SBS dual-channel
// debug COMPARE tool, which keeps its own inline path; callers check gpu_sbs_get() for that, not this.
int rq_active(void) { return 1; }

void RenderQueue::reset() { n = 0; seq = 0; consumed = 0; }

RqItem* RenderQueue::push() {
  if (consumed) reset();                       // first push after a flush -> new frame
  if (n >= RQ_MAX) {
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[rq] WARN: render queue full (%d) — dropping prims\n", RQ_MAX);
    return 0;
  }
  RqItem* it = &items[n++];
  it->seq = seq++;
  return it;
}

void RenderQueue::mark_consumed() { if (n) consumed = 1; }

void RenderQueue::flush(Core* core) {
  // Engine-decided order: layer low->high, submission order within a layer. stable_sort keeps the
  // within-layer submission order exactly (matters for semi-transparent blending). The D32 depth buffer
  // does fine-grained occlusion inside RQ_WORLD regardless of this order.
  if (n) std::stable_sort(items, items + n, [](const RqItem& a, const RqItem& b) {
    return a.layer != b.layer ? a.layer < b.layer : a.seq < b.seq;
  });
  // fps60: the interpolated-60fps tier OWNS presentation — it needs to emit this frame TWICE (the lerped
  // in-between, then the real frame), so it must hold the items rather than have flush emit them now.
  // Snapshot the sorted queue to it and skip the inline emit; fps60_present_vk emits + presents both.
  extern int g_fps60_on;
  if (g_fps60_on) { core->game->fps60.rq_capture(items, n); mark_consumed(); return; }
  if (!n) { mark_consumed(); return; }
  for (int i = 0; i < n; i++) gpu_emit_rq_item(core, &items[i]);
  mark_consumed();
}

RqItem* rq_push(Core* core)   { return core->game->rq.push(); }
void    rq_flush(Core* core)  { core->game->rq.flush(core); }
