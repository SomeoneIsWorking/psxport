// test_face_contest — the pair rule: when may the depth buffer NOT be trusted to order two faces?
//
// The queue resolves the order a GAME authored, and it does so by asking this one question per pair of
// faces of the same object. The rule is pure geometry over an RqItem and a declared sort key, so it can
// be driven on its inputs alone — which is what makes the interesting cases testable at all, because
// they are cases the real scene almost never produces.
//
// THE CASE THAT MATTERS MOST IS NOT IN THE OTHER SUITE.
// tests/test_render_queue_keyorder.cpp checks the SWEEP against a brute-force oracle built from this
// same predicate — which means the two cannot disagree about the rule, and the oracle's coverage is the
// rule's coverage. That suite does NOT exercise EDGE-TOUCHING face pairs, and its own comment records
// why: a separating-axis pre-filter that treated touching faces as disjoint was verified "identical"
// and was wrong, dropping the snap count from 109 of 262 faces to 98. The same failure mode applies to
// the exact test's own degenerate-region reject, which was also measured missing (221 of 262 snapped
// instead of 109) when the reject was dropped. Both are invisible to the oracle, so they are asserted
// HERE, against the geometry directly.
//
// Hermetic: no queue, no Core, no GPU, no disc. Every case is two RqItems and a boolean.
#include "face_contest.h"
#include "render_queue.h"
#include "testutil.h"

#include <cmath>
#include <cstdint>

namespace {

// A face whose four corners are the unit square scaled by `size` and offset by (x0,y0), with a
// per-vertex depth interpolated across the corners. `depthAtVertex` lets a case make the depth PLANE
// (affine) or tilted, which is the difference between "the faces agree" and "they cross".
RqItem makeFace(int x0, int y0, int size, int32_t sortKey, float nearDepth, float farDepth) {
  RqItem item{};
  item.nv = 4;
  item.layer = RQ_WORLD;
  item.order_mode = RQ_OM_DEPTH;
  item.sort_key = sortKey;
  item.dbg_node = 0x80010000u;
  const int x1 = x0 + size, y1 = y0 + size;
  // depth[] is per VERTEX, so the corners carry the plane's value at those corners: near at the top
  // edge, far at the bottom. A linear ramp over the square, which is what a real projected face has.
  const int xs[4] = {x0, x1, x0, x1};
  const int ys[4] = {y0, y0, y1, y1};
  const float ds[4] = {nearDepth, nearDepth, farDepth, farDepth};
  for (int i = 0; i < 4; i++) {
    item.xs[i] = xs[i];
    item.ys[i] = ys[i];
    item.depth[i] = ds[i];
  }
  return item;
}

// Rotate a face's vertex LIST by one, keeping the same four corners and the same four depths. This is
// the decal case: a quad filed on the wall quad it decorates, listed starting at a different corner.
RqItem rotateVertices(const RqItem &source) {
  RqItem item = source;
  for (int i = 0; i < 4; i++) {
    item.xs[i] = source.xs[(i + 1) & 3];
    item.ys[i] = source.ys[(i + 1) & 3];
    item.depth[i] = source.depth[(i + 1) & 3];
  }
  return item;
}

// A face with an EXPLICIT vertex order. The degenerate-area reject is only reachable when two faces
// share a whole TRIANGLE edge rather than a single vertex, and whether they do depends entirely on the
// vertex order — so a case that does not control the order is not testing that reject at all.
RqItem makeOrderedFace(const int xs[4], const int ys[4], int32_t sortKey) {
  RqItem item{};
  item.nv = 4;
  item.layer = RQ_WORLD;
  item.order_mode = RQ_OM_DEPTH;
  item.sort_key = sortKey;
  item.dbg_node = 0x80010000u;
  // depth 0.9 along y == 0 and 0.1 along y == 100, so the plane tilts and any two faces that overlap
  // with real area cross inside the overlap.
  for (int i = 0; i < 4; i++) {
    item.xs[i] = xs[i];
    item.ys[i] = ys[i];
    item.depth[i] = (ys[i] == 0) ? 0.9f : 0.1f;
  }
  return item;
}

// A TRIANGLE face with explicit vertices. The degenerate-area reject is only reachable for a pair whose
// BOUNDING BOXES overlap with positive width — a pair whose boxes merely touch is turned away earlier by
// the cheap bounds reject, and so never reaches it. The reachable case is two faces that share a whole
// TRIANGLE EDGE running diagonally, which is the ordinary shape of a mesh seam once the fixed
// (0,1,2)+(1,2,3) split is taken into account.
RqItem makeTriangle(const int xs[3], const int ys[3], const float depths[3], int32_t sortKey) {
  RqItem item{};
  item.nv = 3;
  item.layer = RQ_WORLD;
  item.order_mode = RQ_OM_DEPTH;
  item.sort_key = sortKey;
  item.dbg_node = 0x80010000u;
  for (int i = 0; i < 3; i++) {
    item.xs[i] = xs[i];
    item.ys[i] = ys[i];
    item.depth[i] = depths[i];
  }
  return item;
}

} // namespace

// ---- the two rules -------------------------------------------------------------------------------

// RULE 1, KEYS DIFFER: the game declared an order and the depth buffer would INVERT it — at some point
// strictly inside both polygons, the FARTHER-keyed face interpolates NEARER. The farther-keyed face is
// the one with the LARGER sort key (a smaller OT index is nearer), and "nearer" is a LARGER depth.
static void test_a_farther_keyed_face_that_interpolates_nearer_is_in_contest(void) {
  // Two overlapping squares at the same place. `near` is keyed 1 (nearer per the game) but its depth
  // ramp puts it FARTHER on screen; `far` is keyed 2 but interpolates nearer. The game says `far` is
  // behind, the depth says it is in front: exactly the contradiction the rule exists for.
  const RqItem near = makeFace(0, 0, 100, 1, 0.10f, 0.90f);
  const RqItem far = makeFace(0, 0, 100, 2, 0.90f, 0.10f);
  CHECK(psx::gpu::facesInContest(near, far));
}

// THE NEGATIVE FOR THAT RULE. Identical geometry, keys in the SAME direction as the depth: the game's
// order and the depth buffer agree, so the depth buffer may be trusted and nothing is snapped. A rule
// that fired here would snap every face of every mesh and destroy the depth buffer's actual job.
static void test_agreeing_depth_is_not_in_contest(void) {
  // Both depth planes are CONSTANT here (the near/far arguments are equal), because the point of the
  // case is that the two never cross at all — a ramp would cross by construction and the case would
  // pass for the wrong reason.
  const RqItem near = makeFace(0, 0, 100, 1, 0.90f, 0.90f); // keyed nearer AND uniformly nearer
  const RqItem far = makeFace(0, 0, 100, 2, 0.10f, 0.10f);
  CHECK(near.depth[0] > far.depth[0]);
  CHECK(!psx::gpu::facesInContest(near, far));
}

// The rule is SYMMETRIC — the caller's whole witness search depends on one witness settling both ends —
// so the answer must not depend on the argument order.
static void test_the_rule_is_symmetric(void) {
  const RqItem a = makeFace(0, 0, 100, 1, 0.10f, 0.90f);
  const RqItem b = makeFace(0, 0, 100, 2, 0.90f, 0.10f);
  CHECK_EQ(psx::gpu::facesInContest(a, b), psx::gpu::facesInContest(b, a));
  const RqItem c = makeFace(0, 0, 100, 1, 0.90f, 0.10f);
  const RqItem d = makeFace(0, 0, 100, 2, 0.10f, 0.90f);
  CHECK_EQ(psx::gpu::facesInContest(c, d), psx::gpu::facesInContest(d, c));
}

// RULE 2, SAME KEY: the game filed both in the SAME ordering-table bucket, so the key expresses no
// order at all and real depth is normally the right answer. The one exception is a pair that is EXACTLY
// coincident but listed with a ROTATED vertex order — a decal quad filed on the wall quad it decorates.
// The fixed (0,1,2)+(1,2,3) split then puts the two faces on OPPOSITE diagonals, so their interiors
// interpolate differently and one of them wins a half it should not. Such a pair carries no depth
// information to preserve, so it IS in contest.
static void test_a_rotated_coincident_decal_in_one_bucket_is_in_contest(void) {
  const RqItem wall = makeFace(0, 0, 100, 7, 0.25f, 0.75f);
  const RqItem decal = rotateVertices(wall);
  // Precondition, asserted so the case cannot pass for the wrong reason: the same corners, the same
  // depths, and the same key.
  CHECK_EQ(wall.sort_key, decal.sort_key);
  CHECK_EQ(wall.depth[0], decal.depth[1]);
  CHECK(psx::gpu::facesInContest(wall, decal));
}

// THE NEGATIVE FOR RULE 2, and the one that costs the most if it is lost. Two faces in the same bucket
// that are NOT coincident — the ordinary case of a bucket holding several separate quads — must not be
// in contest. Their interiors are disjoint, so there is nothing to decide.
static void test_two_separate_faces_in_one_bucket_are_not_in_contest(void) {
  const RqItem left = makeFace(0, 0, 100, 7, 0.25f, 0.75f);
  const RqItem right = makeFace(200, 0, 100, 7, 0.25f, 0.75f);
  CHECK_EQ(left.sort_key, right.sort_key);
  CHECK(!psx::gpu::facesInContest(left, right));
}

// ---- the case the other suite cannot see ---------------------------------------------------------

// MESH-ADJACENT FACES, TOUCHING ALONG AN EDGE, ARE NOT IN CONTEST.
//
// This is the recorded incident. The exact interior test clips one triangle by the other and then
// evaluates the affine depth difference at the surviving vertices. Clipping ADMITS points ON the
// boundary, so two faces meeting along a shared edge survive as a degenerate sliver — three or more
// vertices, ZERO AREA — and the affine difference is generally non-zero along that edge. Without an
// area reject, that reports an inversion for every adjacent pair in a mesh. Measured when the reject was
// missing: 221 of 262 faces snapped instead of 109, and inversions found went 152,304 -> 385,622.
//
// It is invisible to tests/test_render_queue_keyorder.cpp, whose own comment says the oracle does not
// exercise edge-touching pairs — which is exactly why a pre-filter that made the same mistake was
// "verified identical" and shipped.
static void test_faces_touching_along_an_edge_are_not_in_contest(void) {
  // Two squares side by side that share the whole vertical edge x == 100, listed so that the shared
  // edge IS a triangle edge of each: the fixed (0,1,2)+(1,2,3) split puts the quad's WRAP edge (v0,v3)
  // in no triangle at all, so whether a shared boundary edge is reachable depends on the vertex order.
  // A case that does not control the order can share a single VERTEX instead and never reach the reject
  // — which is exactly the case a mutant survives.
  static const int leftX[4] = {0, 100, 100, 0};
  static const int leftY[4] = {0, 0, 100, 100};
  static const int rightX[4] = {200, 100, 100, 200};
  static const int rightY[4] = {0, 0, 100, 100};
  const RqItem left = makeOrderedFace(leftX, leftY, 1);
  const RqItem right = makeOrderedFace(rightX, rightY, 2);
  // Preconditions, asserted so the case cannot pass for the wrong reason: the two really do share the
  // whole segment x == 100, y in [0,100] as a triangle edge (v1-v2 of each), and the keys DIFFER so this
  // exercises the keys-differ rule rather than the same-bucket one.
  CHECK_EQ(left.xs[1], right.xs[1]);
  CHECK_EQ(left.xs[2], right.xs[2]);
  CHECK_EQ(left.ys[1], right.ys[1]);
  CHECK_EQ(left.ys[2], right.ys[2]);
  CHECK(left.xs[1] == right.xs[2] && left.xs[2] == right.xs[1]); // the same two corners, in one triangle each
  CHECK(left.sort_key != right.sort_key);
  CHECK(!psx::gpu::facesInContest(left, right));
}

// The same pair with a one-pixel GAP is not in contest either — but for a different reason, the cheap
// bounds reject. Asserting both keeps the two rejects distinguishable: a mutant that widens the
// tolerance by a pixel would make the touching case fail without this one noticing.
static void test_faces_with_a_gap_are_rejected_by_bounds_not_by_the_exact_test(void) {
  const RqItem left = makeFace(0, 0, 100, 1, 0.10f, 0.90f);
  const RqItem right = makeFace(101, 0, 100, 2, 0.90f, 0.10f);
  CHECK(left.xs[1] < right.xs[0]);
  CHECK(!psx::gpu::facesInContest(left, right));
}

// A face that OVERLAPS another by a real area IS in contest even when the shared region is a thin
// strip. This is the case an area reject must NOT swallow: a sliver of a pixel and a sliver of a pixel
// are different things, and a reject that treats them alike is the mutant this whole file guards.
static void test_a_real_overlap_is_in_contest_even_when_thin(void) {
  // One pixel of true overlap along the shared edge is not enough; four is a real region, and the
  // depth ramp crosses inside it, so the far-keyed face is genuinely nearer there.
  const RqItem near = makeFace(0, 0, 100, 1, 0.10f, 0.90f);
  const RqItem far = makeFace(96, 0, 100, 2, 0.90f, 0.10f);
  CHECK_EQ(near.xs[1], far.xs[0] + 4);
  CHECK(psx::gpu::facesInContest(near, far));
}

// THE DEGENERATE-AREA REJECT, and the only case that reaches it.
//
// The two cheap rejects cannot turn this pair away: their BOUNDING BOXES overlap with positive width on
// both axes (so the bounds reject passes) and the far face's nearest bound exceeds the near face's
// farthest (so the depth reject passes). What they share is a single DIAGONAL — the (0,0)-(100,100) edge
// of the near quad's first triangle — which has zero area. The affine depth difference is non-zero along
// that edge, so without the area reject the rule reports an inversion for a pair that shares nothing but
// a line.
//
// This is the shape of every mesh seam the fixed (0,1,2)+(1,2,3) split produces, which is why the
// measured failure was 221 of 262 faces snapping instead of 109.
static void test_faces_sharing_only_a_diagonal_are_not_in_contest(void) {
  // TWO TRIANGLES, one on each side of the line y == x, meeting along it and nowhere else. A quad would
  // not do: its SECOND triangle (1,2,3) covers the upper-left half of the square and genuinely overlaps
  // the far face with real area, so the rule is right to answer YES for it and the case would pass for
  // the wrong reason. Only a pair whose every triangle pair is degenerate reaches the area reject.
  static const int nearX[3] = {0, 100, 100};
  static const int nearY[3] = {0, 0, 100};
  static const float nearDepth[3] = {0.9f, 0.9f, 0.1f};
  const RqItem near = makeTriangle(nearX, nearY, nearDepth, 1);
  static const int farX[3] = {0, 100, 0};
  static const int farY[3] = {0, 100, 200};
  static const float farDepth[3] = {0.95f, 0.15f, 0.15f};
  const RqItem far = makeTriangle(farX, farY, farDepth, 2);

  // Preconditions, asserted so the case cannot pass for the wrong reason.
  const psx::gpu::FaceExtent nearExtent = psx::gpu::faceExtent(near);
  const psx::gpu::FaceExtent farExtent = psx::gpu::faceExtent(far);
  // The BOUNDING BOXES overlap with positive width and height, so the cheap bounds reject cannot fire.
  CHECK(farExtent.x0 < nearExtent.x1);
  CHECK(nearExtent.x0 < farExtent.x1);
  CHECK(farExtent.y0 < nearExtent.y1);
  CHECK(nearExtent.y0 < farExtent.y1);
  // The depth ranges really do cross, so the depth reject cannot fire either.
  CHECK(farExtent.nearest > nearExtent.farthest);
  // The two really do share the two corners of the diagonal.
  CHECK_EQ(near.xs[0], far.xs[0]);
  CHECK_EQ(near.xs[2], far.xs[1]);
  CHECK_EQ(near.ys[0], far.ys[0]);
  CHECK_EQ(near.ys[2], far.ys[1]);
  // And the far face really is uniformly NEARER on that shared edge, so the area reject is the ONLY
  // thing standing between this pair and a snap.
  CHECK(far.depth[0] > near.depth[0]);
  CHECK(far.depth[2] > near.depth[2]);

  CHECK(!psx::gpu::facesInContest(near, far));
}

static void test_the_depth_reject_reads_the_renderers_inverted_convention(void) {
  // `far` is keyed farther (larger sort key) yet is uniformly FARTHER on screen: its whole depth range
  // is below `near`'s. No point of `far` can out-depth `near`, so the exact test has nothing to find.
  const RqItem near = makeFace(0, 0, 100, 1, 0.80f, 0.95f);
  const RqItem far = makeFace(0, 0, 100, 2, 0.05f, 0.20f);
  CHECK(far.depth[0] < near.depth[3]);
  CHECK(!psx::gpu::facesInContest(near, far));
}

// A degenerate face — three coincident vertices, zero area — must be skipped rather than divided by.
// Every consumer of a FaceSetup has to refuse it, because a zero `den` is a division by zero.
static void test_a_degenerate_face_is_skipped_rather_than_divided_by(void) {
  RqItem degenerate{};
  degenerate.nv = 3;
  degenerate.sort_key = 1;
  for (int i = 0; i < 3; i++) {
    degenerate.xs[i] = 10;
    degenerate.ys[i] = 10;
    degenerate.depth[i] = 0.5f;
  }
  const RqItem solid = makeFace(0, 0, 100, 2, 0.90f, 0.10f);
  const psx::gpu::FaceSetup setup = psx::gpu::faceSetup(degenerate);
  CHECK_EQ(setup.triangleCount, 1);
  CHECK(setup.triangle[0].den == 0.0f);
  // The rule must not fire on it, and asking must not produce a NaN.
  const bool decided = psx::gpu::facesInContest(degenerate, solid);
  CHECK(!decided);
}

// ---- the precomputed forms must agree with the convenience form -----------------------------------
// The queue holds the whole group and computes each face's extent and setup ONCE; this asserts the
// six-argument rule and the two-argument convenience form are the same rule, so the memoisation the
// caller does cannot change a decision.
static void test_the_precomputed_and_convenience_forms_agree(void) {
  const RqItem pairs[][2] = {
      {makeFace(0, 0, 100, 1, 0.10f, 0.90f), makeFace(0, 0, 100, 2, 0.90f, 0.10f)},
      {makeFace(0, 0, 100, 1, 0.90f, 0.10f), makeFace(0, 0, 100, 2, 0.10f, 0.90f)},
      {makeFace(0, 0, 100, 7, 0.25f, 0.75f), rotateVertices(makeFace(0, 0, 100, 7, 0.25f, 0.75f))},
      {makeFace(0, 0, 100, 1, 0.10f, 0.90f), makeFace(100, 0, 100, 2, 0.90f, 0.10f)},
      {makeFace(0, 0, 100, 1, 0.10f, 0.90f), makeFace(300, 300, 50, 2, 0.90f, 0.10f)},
  };
  for (const auto &pair : pairs) {
    const psx::gpu::FaceExtent extentA = psx::gpu::faceExtent(pair[0]);
    const psx::gpu::FaceExtent extentB = psx::gpu::faceExtent(pair[1]);
    const psx::gpu::FaceSetup setupA = psx::gpu::faceSetup(pair[0]);
    const psx::gpu::FaceSetup setupB = psx::gpu::faceSetup(pair[1]);
    const bool withSetups = psx::gpu::facesInContest(pair[0], pair[1], extentA, extentB, setupA, setupB);
    const bool convenience = psx::gpu::facesInContest(pair[0], pair[1]);
    CHECK_EQ(withSetups, convenience);
  }
}

// ---- the extents and setups are what the rule consumes --------------------------------------------
// The broadphase sorts and rejects on the extent's bbox, so a wrong extent is a wrong sweep even when
// the rule itself is right.
static void test_extent_matches_the_faces_own_corners(void) {
  const RqItem face = makeFace(10, 20, 60, 3, 0.9f, 0.1f); // corners (10,20) .. (70,80)
  const psx::gpu::FaceExtent extent = psx::gpu::faceExtent(face);
  CHECK_EQ(extent.x0, 10.0f);
  CHECK_EQ(extent.y0, 20.0f);
  CHECK_EQ(extent.x1, 70.0f);
  CHECK_EQ(extent.y1, 80.0f);
  // `nearest`/`farthest` are the face's EXTREMES, and `nearest` is the LARGER value by the renderer's
  // convention — so a face whose whole range is deep (small values) has nearest < farthest.
  CHECK(extent.farthest == 0.1f);
  CHECK(extent.nearest == 0.9f);
  CHECK(extent.nearest > extent.farthest);
}

// A sub-pixel float position, when the producer supplied one, is the position the RASTERIZER uses — so
// the geometric tests must read it too, or they reason about a different polygon than the one drawn.
static void test_the_setup_reads_the_rasterizers_own_vertex_positions(void) {
  RqItem integer = makeFace(0, 0, 100, 1, 0.9f, 0.1f);
  RqItem subpixel = integer;
  subpixel.has_xyf = 1;
  for (int i = 0; i < 4; i++) {
    subpixel.xsf[i] = static_cast<float>(integer.xs[i]) + 0.25f;
    subpixel.ysf[i] = static_cast<float>(integer.ys[i]) + 0.25f;
  }
  const psx::gpu::FaceExtent fromInteger = psx::gpu::faceExtent(integer);
  const psx::gpu::FaceExtent fromSubpixel = psx::gpu::faceExtent(subpixel);
  CHECK_EQ(fromInteger.x0, 0.0f);
  CHECK_EQ(fromSubpixel.x0, 0.25f);
  CHECK_EQ(fromSubpixel.x1, 100.25f);
}

// ---- the census -------------------------------------------------------------------------------------
// The census is what makes "nothing was in contest" and "the sweep never looked" different sentences.
// It is read by value so no caller can invent a figure, and the identity it must satisfy is that every
// pair offered to the exact test was either rejected by a cheap reject or reached it.
static void test_the_census_counts_what_the_rule_decided(void) {
  const psx::gpu::ContestCensus before = psx::gpu::contestCensus();

  // One pair that the bounds reject turns away (far apart on screen).
  const RqItem left = makeFace(0, 0, 100, 1, 0.9f, 0.1f);
  const RqItem right = makeFace(400, 400, 100, 2, 0.1f, 0.9f);
  CHECK(!psx::gpu::facesInContest(left, right));

  // One pair that reaches the exact test and finds an inversion.
  const RqItem near = makeFace(0, 0, 100, 1, 0.10f, 0.90f);
  const RqItem far = makeFace(0, 0, 100, 2, 0.90f, 0.10f);
  CHECK(psx::gpu::facesInContest(near, far));

  const psx::gpu::ContestCensus after = psx::gpu::contestCensus();
  CHECK_EQ(after.rejectedByBounds - before.rejectedByBounds, 1u);
  CHECK_EQ(after.reachedExactTest - before.reachedExactTest, 1u);
  CHECK_EQ(after.foundInversion - before.foundInversion, 1u);
  // The retired separating-axis counter stays at zero, which is what its "always 0" comment claims.
  CHECK_EQ(after.rejectedBySeparation - before.rejectedBySeparation, 0u);
}

int main(void) {
  RUN(a_farther_keyed_face_that_interpolates_nearer_is_in_contest);
  RUN(agreeing_depth_is_not_in_contest);
  RUN(the_rule_is_symmetric);
  RUN(a_rotated_coincident_decal_in_one_bucket_is_in_contest);
  RUN(two_separate_faces_in_one_bucket_are_not_in_contest);
  RUN(faces_touching_along_an_edge_are_not_in_contest);
  RUN(faces_with_a_gap_are_rejected_by_bounds_not_by_the_exact_test);
  RUN(a_real_overlap_is_in_contest_even_when_thin);
  RUN(faces_sharing_only_a_diagonal_are_not_in_contest);
  RUN(the_depth_reject_reads_the_renderers_inverted_convention);
  RUN(a_degenerate_face_is_skipped_rather_than_divided_by);
  RUN(the_precomputed_and_convenience_forms_agree);
  RUN(extent_matches_the_faces_own_corners);
  RUN(the_setup_reads_the_rasterizers_own_vertex_positions);
  RUN(the_census_counts_what_the_rule_decided);
  return pt_summary();
}
