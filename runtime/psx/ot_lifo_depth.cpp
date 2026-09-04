#include "ot_lifo_depth.h"

#include "render_queue.h"

#include <algorithm>
#include <cmath>
#include <lucent/log.h>
#include <stdlib.h>

// The guest's AddPrim is head insertion: submissions A,B,C make the bucket chain C->B->A, so A
// paints last and WINS. Native draws in queue order, so with equal depths and GREATER_OR_EQUAL the
// last one drawn wins — C, the opposite answer.
//
// The fix is to draw the bucket in reverse: C,B,A, so A is drawn last and wins. That is exactly the
// guest's LIFO walk, and it costs ONE depth value for the whole bucket.
//
// This replaces separating ties by depth (A nearest, B next, C at the band base). That worked only
// while buckets stayed small: a band is 1/domain of the ord range, which maps into the renderer's 3D
// depth band where a float32 ulp is 2^-24, so a band holds ~448 distinct depths. Crash Bash's
// attract flow produced a bucket of 472 tied faces and the frame was refused. Depth resolution is
// the wrong currency for a question that is purely about paint ORDER — the buffer only has to say
// which BUCKET is nearer, and draw order says which face inside it wins. Buckets are now unbounded.
//
// Draw order is permuted through `draw_seq`, never `seq`: `seq` is what the guest submitted, and the
// source-OT oracle (rq_source_ot_candidate_wins) and fps60's pairing both reason about submission
// order. The permutation is of the group's OWN draw_seq values, so items outside the bucket keep
// their positions exactly and only the intra-bucket order flips.
//
// The key->ord map is still required to be strictly monotone across buckets and consistent within
// one, so violated input is an invariant failure rather than something to paper over.
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
  static thread_local std::vector<uint32_t> group_draw_seqs;
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

    const size_t group_size = group_end - group_start;
    const float ord = first.key_ord;
    // The whole bucket sits at its band: one depth, so the depth test only ever decides BETWEEN
    // buckets. Which face wins inside the bucket is decided by draw order below.
    for (size_t i = group_start; i < group_end; i++) {
      RqItem &it = items[selected[i]];
      it.authored_depth = 1;
      for (int k = 0; k < 4; k++) {
        it.depth[k] = ord;
      }
    }
    if (group_size > 1) {
      // `selected[group_start..group_end)` is in DESCENDING submission order (the sort above), which
      // is the guest's bucket walk C,B,A. Handing that sequence the group's own draw_seq values in
      // ASCENDING order makes the queue draw it in exactly that order, so A — the head of the guest
      // chain — is drawn last and wins.
      group_draw_seqs.clear();
      group_draw_seqs.reserve(group_size);
      for (size_t i = group_start; i < group_end; i++) {
        group_draw_seqs.push_back(items[selected[i]].draw_seq);
      }
      std::sort(group_draw_seqs.begin(), group_draw_seqs.end());
      // draw_seq comes from push()'s monotonic counter, so a bucket's values are necessarily
      // distinct. Duplicates mean an RqItem reached the queue without draw_seq seeded, and the
      // permutation below would then be a no-op that silently paints the bucket in the WRONG order
      // — the exact answer this function exists to prevent. Refuse instead of ordering by luck.
      if (std::adjacent_find(group_draw_seqs.begin(), group_draw_seqs.end()) != group_draw_seqs.end()) {
        lucent::error("keyord",
                      "FATAL: OT bucket {} has {} face(s) but duplicate draw_seq values (first "
                      "duplicate {}) — an item reached the queue without draw_seq seeded from seq",
                      first.sort_key,
                      group_size,
                      *std::adjacent_find(group_draw_seqs.begin(), group_draw_seqs.end()));
        abort();
      }
      for (size_t i = group_start; i < group_end; i++) {
        items[selected[i]].draw_seq = group_draw_seqs[i - group_start];
      }
      tied += group_size;
    }
    nearer_ord = ord;
    group_start = group_end;
  }
  return tied;
}
