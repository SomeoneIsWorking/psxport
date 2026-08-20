// test_render_queue_keyorder — RenderQueue::resolveKeyOrder: what it DECIDES, and what it COSTS.
//
// WHY THIS TEST EXISTS (Tomba!2, 2026-08-04). The DEMO attract loop wedged the frame loop dead at
// gpu frame 1822: the watchdog's backtrace landed inside resolveKeyOrder, and the run never
// presented another frame. It was not an infinite loop — the function has no unbounded loop — it
// was the pair contest doing 596,134,804 pair tests and 496,339,081 interior-grid samples for ONE
// frame, because two guest object nodes each emitted tens of thousands of keyed faces that frame
// (measured: maxgroup=31308 faces on node 800F06D8, vs 143 on a normal frame).
//
// WHAT THE FUNCTION ACTUALLY COMPUTES. Its output is one bit PER FACE — "snap this face's test
// depth to its key's ord" — and that bit is a pure existence question:
//
//     snap[x]  ==  there EXISTS some other face y of the same object such that x and y are
//                  "in contest": either they carry the SAME sort key and are exactly coincident
//                  (the rotated-vertex decal case), or their keys differ and the farther-keyed one
//                  interpolates NEARER somewhere both polygons cover (the depth-buffer-contradicts-
//                  the-game case).
//
// An existence question needs ONE witness. The pre-fix implementation enumerated the complete
// pairwise relation instead — every C(n,2) pair of every object group — so a face that had already
// found its witness kept being tested against every remaining face in the group. On the wedge frame
// 45,917 of 45,993 faces were snapped (99.83%), i.e. essentially every one of those half-billion
// pair tests was re-deciding a face whose answer was already known.
//
// SO THIS FILE ASSERTS TWO THINGS, and they have to be asserted together:
//   1. EQUIVALENCE — the snap set is exactly the brute-force existence set, on inputs that exercise
//      both contest rules and the negative case. Without this, "fast" is meaningless.
//   2. WORK — on a large single-object group the pair-test count is proportional to the group, not
//      to its square. Without this, a re-introduced exhaustive scan is invisible until a game
//      wedges again.
//
// Hermetic: no Core, no disc, no GPU. resolveKeyOrderFaces(frame) is the Core-free entry point;
// the RqItems are built directly.
#include "testutil.h"

#include "render_queue.h"

#include <math.h>
#include <memory>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace {

// A queue is ~16 MB of RqItem storage, so it lives on the heap and is reused across cases.
std::unique_ptr<RenderQueue> make_queue(void) {
  std::unique_ptr<RenderQueue> q(new RenderQueue());
  q->n = 0;
  q->seq = 0;
  return q;
}

// Append one keyed WORLD face. Every field resolveKeyOrder reads is set explicitly here: the layer/
// order-mode/sort-key triple that makes it "keyed", the dbg_node that decides its OBJECT GROUP, the
// float screen XY, and the per-vertex ord (larger = nearer, the renderer's convention).
void push_face(RenderQueue &q,
               uint32_t node,
               int sort_key,
               float x0,
               float y0,
               float x1,
               float y1,
               float ord_at_v0,
               float ord_at_v2) {
  RqItem &it = q.items[q.n];
  memset(&it, 0, sizeof(it));
  it.seq = q.seq++;
  it.layer = RQ_WORLD;
  it.order_mode = RQ_OM_DEPTH;
  it.nv = 4;
  it.has_xyf = 1;
  it.dbg_node = node;
  it.sort_key = sort_key;
  // key_ord is deliberately given a value NO face uses as a vertex ord. Snapping is observed by
  // "every vertex ord became key_ord", so a face whose real ords happened to equal its key_ord would
  // read as snapped whether it was or not — an ambiguity that silently passed three cases the first
  // time this test was run. Nothing under test reads key_ord's magnitude (the contest rules use
  // sort_key and the vertex ords only), so a distinctive value costs the test nothing.
  it.key_ord = 1000.0f + (float)q.n;
  // Vertex winding 0=(x0,y0) 1=(x1,y0) 2=(x1,y1) 3=(x0,y1): an axis-aligned quad, split by the
  // rasterizer into tris (0,1,2) and (1,2,3) — the same split rq_ord_at samples with.
  const float vx[4] = {x0, x1, x1, x0};
  const float vy[4] = {y0, y0, y1, y1};
  for (int k = 0; k < 4; k++) {
    it.xsf[k] = vx[k];
    it.ysf[k] = vy[k];
    it.xs[k] = (int)vx[k];
    it.ys[k] = (int)vy[k];
  }
  // Ramp the ord across the quad so the two faces of a pair genuinely CROSS in depth rather than
  // being uniformly in front of one another (a uniform pair is rejected by the cheap dmax/dmin test
  // and would never reach the interior contest this function exists for).
  it.depth[0] = ord_at_v0;
  it.depth[1] = 0.5f * (ord_at_v0 + ord_at_v2);
  it.depth[2] = ord_at_v2;
  it.depth[3] = 0.5f * (ord_at_v0 + ord_at_v2);
  q.n++;
}

// The specification, implemented independently of the unit: snap[x] iff SOME other face of the same
// object is in contest with x. Deliberately the dumbest possible O(n^2) formulation — it is the
// oracle, so it must be obviously right rather than fast.
std::vector<uint8_t> brute_force_snap(const RenderQueue &q) {
  std::vector<int> keyed;
  for (int i = 0; i < q.n; i++) {
    const RqItem &it = q.items[i];
    if (it.layer != RQ_WORLD || it.order_mode != RQ_OM_DEPTH || it.sort_key < 0) {
      continue;
    }
    if (it.dbg_node < 0x80000000u || it.dbg_node >= 0x80200000u) {
      continue;
    }
    keyed.push_back(i);
  }
  std::vector<uint8_t> snap(q.n, 0);
  for (size_t a = 0; a < keyed.size(); a++) {
    for (size_t b = 0; b < keyed.size(); b++) {
      if (a == b) {
        continue;
      }
      const RqItem &A = q.items[keyed[a]];
      const RqItem &B = q.items[keyed[b]];
      if (A.dbg_node != B.dbg_node) {
        continue;
      }
      if (rq_faces_in_contest(A, B)) {
        snap[keyed[a]] = 1;
        snap[keyed[b]] = 1;
      }
    }
  }
  return snap;
}

// Compare the unit's decision against the oracle. Returns the number of faces that DISAGREE, and
// reports the totals so a "0 disagreements" result carries its denominator — over how many faces,
// and how many of them the oracle expected to be snapped. A run where the oracle snapped nothing
// proves nothing about the snapping rules, so cases assert the expected snap count too.
struct SnapCompare {
  int faces;
  int oracle_snapped;
  int unit_snapped;
  int disagreements;
};

SnapCompare run_and_compare(RenderQueue &q) {
  // The oracle reads pre-resolve depths, so it must run BEFORE the unit overwrites them with key_ord.
  std::vector<uint8_t> want = brute_force_snap(q);
  std::vector<float> ord_before(q.n);
  for (int i = 0; i < q.n; i++) {
    ord_before[i] = q.items[i].key_ord;
  }

  q.resolveKeyOrderFaces(0);

  SnapCompare r = {q.n, 0, 0, 0};
  for (int i = 0; i < q.n; i++) {
    // A snapped face has every vertex ord replaced by its key_ord — that IS the observable effect.
    bool got = q.items[i].depth[0] == ord_before[i] && q.items[i].depth[1] == ord_before[i] &&
               q.items[i].depth[2] == ord_before[i] && q.items[i].depth[3] == ord_before[i];
    if (want[i]) {
      r.oracle_snapped++;
    }
    if (got) {
      r.unit_snapped++;
    }
    if ((int)want[i] != (int)got) {
      r.disagreements++;
    }
  }
  return r;
}

} // namespace

// ---- 1. the contradiction rule: two faces of one object whose depth inverts the game's key order --
static void test_contradicting_pair_snaps_both(void) {
  std::unique_ptr<RenderQueue> q = make_queue();
  // Same object, overlapping on screen. Face NEAR has the smaller sort key (the game files it in
  // front) but ramps to a FARTHER ord across the overlap, so the depth buffer would hand those
  // pixels to FAR — exactly the barrel-cap case the rule exists for.
  push_face(*q, 0x800FD850u, 400, 0, 0, 40, 40, 0.90f, 0.10f);
  push_face(*q, 0x800FD850u, 460, 0, 0, 40, 40, 0.20f, 0.80f);
  SnapCompare r = run_and_compare(*q);
  CHECK_EQ(r.faces, 2);
  CHECK_EQ(r.oracle_snapped, 2); // the rule must actually FIRE here, else the case tests nothing
  CHECK_EQ(r.disagreements, 0);
}

// ---- 2. the negative: an ordinary mesh must be left entirely alone ------------------------------
static void test_disjoint_faces_never_snap(void) {
  std::unique_ptr<RenderQueue> q = make_queue();
  // Eight faces of one object, side by side and not overlapping, keys ascending with depth. No pair
  // can contest, so NOTHING may be snapped — the property that makes this a discriminator rather
  // than the reverted "re-order every face" ramp.
  for (int i = 0; i < 8; i++) {
    push_face(
        *q, 0x800FD850u, 400 + i, (float)(i * 50), 0, (float)(i * 50 + 40), 40, 0.5f - 0.01f * i, 0.5f - 0.01f * i);
  }
  SnapCompare r = run_and_compare(*q);
  CHECK_EQ(r.faces, 8);
  CHECK_EQ(r.oracle_snapped, 0);
  CHECK_EQ(r.unit_snapped, 0);
  CHECK_EQ(r.disagreements, 0);
}

// ---- 3. object grouping: a contest ACROSS two objects is not a contest --------------------------
static void test_contest_does_not_cross_objects(void) {
  std::unique_ptr<RenderQueue> q = make_queue();
  // Byte-for-byte the pair from case 1, except the two faces belong to DIFFERENT guest nodes. The
  // game's sort key only orders faces within one object, so nothing may snap.
  push_face(*q, 0x800FD850u, 400, 0, 0, 40, 40, 0.90f, 0.10f);
  push_face(*q, 0x800FD860u, 460, 0, 0, 40, 40, 0.20f, 0.80f);
  SnapCompare r = run_and_compare(*q);
  CHECK_EQ(r.faces, 2);
  CHECK_EQ(r.oracle_snapped, 0);
  CHECK_EQ(r.unit_snapped, 0);
  CHECK_EQ(r.disagreements, 0);
}

// ---- 4. the same-key rule: exactly coincident decal, rotated vertex order -----------------------
static void test_coincident_same_key_pair_snaps(void) {
  std::unique_ptr<RenderQueue> q = make_queue();
  // Two faces with the SAME sort key and the same four projected corners at the same four depths.
  // The key says nothing about which is in front, and the fixed quad triangulation splits a rotated
  // listing on the opposite diagonal — so both snap and submission order decides.
  push_face(*q, 0x800FD850u, 408, 10, 10, 50, 50, 0.40f, 0.40f);
  RqItem &rot = q->items[q->n];
  rot = q->items[0];
  rot.seq = q->seq++;
  // Rotate the vertex listing by one: same multiset of corners, different diagonal.
  for (int k = 0; k < 4; k++) {
    rot.xsf[k] = q->items[0].xsf[(k + 1) & 3];
    rot.ysf[k] = q->items[0].ysf[(k + 1) & 3];
    rot.depth[k] = q->items[0].depth[(k + 1) & 3];
  }
  q->n++;
  SnapCompare r = run_and_compare(*q);
  CHECK_EQ(r.faces, 2);
  CHECK_EQ(r.oracle_snapped, 2);
  CHECK_EQ(r.disagreements, 0);
}

// ---- 5. a mixed group: both rules and non-participants together ---------------------------------
static void test_mixed_group_matches_oracle(void) {
  std::unique_ptr<RenderQueue> q = make_queue();
  // A contesting pair, a coincident same-key pair, and four faces that touch nothing — all in one
  // object, so the unit has to get every face's bit right rather than a blanket answer.
  push_face(*q, 0x800FD850u, 400, 0, 0, 40, 40, 0.90f, 0.10f);
  push_face(*q, 0x800FD850u, 460, 0, 0, 40, 40, 0.20f, 0.80f);
  push_face(*q, 0x800FD850u, 408, 200, 200, 240, 240, 0.40f, 0.40f);
  push_face(*q, 0x800FD850u, 408, 200, 200, 240, 240, 0.40f, 0.40f);
  for (int i = 0; i < 4; i++) {
    push_face(*q, 0x800FD850u, 500 + i, (float)(600 + i * 60), 0, (float)(600 + i * 60 + 40), 40, 0.2f, 0.2f);
  }
  SnapCompare r = run_and_compare(*q);
  CHECK_EQ(r.faces, 8);
  CHECK_EQ(r.oracle_snapped, 4); // the two contest pairs, and only those
  CHECK_EQ(r.disagreements, 0);
}

// ---- 6. THE WEDGE: cost must scale with the group, not with its square --------------------------
static void test_large_group_work_is_not_quadratic(void) {
  std::unique_ptr<RenderQueue> q = make_queue();
  // The shape measured on the wedge frame: ONE object node, tens of thousands of keyed faces, all
  // mutually overlapping on screen (the real ones spanned the whole -1024..1023 clamp range, so 89%
  // of pairs passed the cheap bbox reject), and essentially every face in contest with something
  // (99.83% snapped). Scaled down to 6000 faces so the PRE-FIX behaviour is merely slow rather than
  // unrunnable — 6000 faces is still 17,997,000 pairs against ~6000 witnesses.
  const int kFaces = 6000;
  for (int i = 0; i < kFaces; i++) {
    // Alternating ramp directions guarantee adjacent faces contest: even faces ramp near->far,
    // odd faces far->near, and consecutive keys make the odd one the "farther-keyed" partner.
    bool even = (i % 2) == 0;
    push_face(*q, 0x800F06D8u, 1500 + i, 0, 0, 300, 200, even ? 0.90f : 0.20f, even ? 0.10f : 0.80f);
  }
  SnapCompare r = run_and_compare(*q);
  CHECK_EQ(r.faces, kFaces);
  CHECK_EQ(r.disagreements, 0);
  // The negative would be meaningless if nothing snapped — this input must reproduce the wedge
  // frame's regime, where nearly every face finds a witness.
  CHECK(r.oracle_snapped > kFaces - 10);

  // THE ACTUAL GATE. An existence question over n faces needs one witness per face, so the work is
  // O(n) tests for the faces that find one plus O(n) per face that does not. Exhaustive pairwise
  // enumeration is n*(n-1)/2 = 17,997,000 here. The bound is 64*n = 384,000: twenty times the
  // witness-per-face ideal, and forty-six times BELOW the exhaustive count — no threshold tuning
  // can slip between those.
  CHECK(q->keyOrderPairTests <= (uint64_t)kFaces * 64u);
  // ...and it must not be trivially small either: every face has to have been examined at least
  // once, or the unit skipped work rather than avoiding it.
  CHECK(q->keyOrderPairTests >= (uint64_t)kFaces / 2u);
}

int main(void) {
  RUN(contradicting_pair_snaps_both);
  RUN(disjoint_faces_never_snap);
  RUN(contest_does_not_cross_objects);
  RUN(coincident_same_key_pair_snaps);
  RUN(mixed_group_matches_oracle);
  RUN(large_group_work_is_not_quadratic);
  return pt_summary();
}
