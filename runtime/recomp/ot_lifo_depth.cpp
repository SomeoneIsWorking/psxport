#include "ot_lifo_depth.h"

#include "gpu_vk.h"
#include "render_queue.h"

#include <algorithm>
#include <cmath>
#include <lucent/log.h>
#include <stdlib.h>

// The guest's AddPrim is head insertion: submissions A,B,C make the bucket chain C->B->A, so A
// paints last and wins. Native emits A,B,C, therefore equal depths would make C win under
// GREATER_OR_EQUAL — the opposite answer. Give A the nearest representable value, B the next, and
// leave C at the bucket's base ord.
//
// Values advance only toward the next NEARER keyed band, and this refuses a frame if float D32
// cannot represent the required number of ties without entering that band. Silently collapsing a
// suffix back to equal depth would reinstate the wrong answer exactly on the largest bucket. The
// key->ord map is required to be strictly monotone, so non-monotone input is also an invariant
// failure rather than something to paper over.
size_t rq_apply_ot_lifo_depths(RqItem *items, std::vector<int> selected) {
  if (selected.empty()) {
    return 0;
  }
  std::sort(selected.begin(), selected.end(), [items](int l, int r) {
    const RqItem &a = items[l];
    const RqItem &b = items[r];
    if (a.sort_key != b.sort_key) {
      return a.sort_key < b.sort_key; // smaller guest bucket is nearer
    }
    return a.seq > b.seq; // guest bucket walk: later submission is visited first
  });

  size_t tied = 0;
  float nearer_ord = INFINITY;
  for (size_t group_start = 0; group_start < selected.size();) {
    size_t group_end = group_start + 1;
    const RqItem &first = items[selected[group_start]];
    while (group_end < selected.size() && items[selected[group_end]].sort_key == first.sort_key) {
      if (items[selected[group_end]].key_ord != first.key_ord) {
        lucent::error("keyord",
                      "FATAL: one OT bucket {} has inconsistent ords {:.9f} and {:.9f}",
                      first.sort_key,
                      (double)first.key_ord,
                      (double)items[selected[group_end]].key_ord);
        abort();
      }
      group_end++;
    }
    if (!(first.key_ord < nearer_ord)) {
      lucent::error("keyord",
                    "FATAL: OT key->ord is not strictly monotone at key {}: ord {:.9f}, nearer band {:.9f}",
                    first.sort_key,
                    (double)first.key_ord,
                    (double)nearer_ord);
      abort();
    }

    float ord = first.key_ord;
    for (size_t i = group_start; i < group_end; i++) {
      if (i != group_start) {
        // One input ULP is not necessarily one raster-depth ULP: ord3d scales the authored value
        // into the reserved 3D band and can round adjacent inputs back to the same float. Ask the
        // shipping depth mapper for the next value the D32 attachment can actually distinguish.
        const float next = gpu_vk_next_distinct_3d_depth(ord, nearer_ord);
        if (!(next < nearer_ord)) {
          lucent::error("keyord",
                        "FATAL: OT bucket {} needs {} distinct D32 ties between ord {:.9f} and nearer band {:.9f}",
                        first.sort_key,
                        group_end - group_start,
                        (double)first.key_ord,
                        (double)nearer_ord);
          abort();
        }
        ord = next;
      }
      RqItem &it = items[selected[i]];
      it.authored_depth = 1;
      for (int k = 0; k < 4; k++) {
        it.depth[k] = ord;
      }
    }
    tied += group_end - group_start > 1 ? group_end - group_start : 0;
    nearer_ord = first.key_ord;
    group_start = group_end;
  }
  return tied;
}
