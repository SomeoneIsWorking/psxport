// face_contest.h — can the depth buffer be trusted to order these two faces of ONE object?
//
// The queue resolves the order a GAME authored, and it does so by asking one question per pair of faces
// of the same object: does the interpolated per-vertex depth contradict the game's own ordering-table
// key? Where it does, both faces' depth is snapped to their key's band and the authored order wins.
//
// This owner is that question and its geometry. It is deliberately Core-free — no game, no queue, no
// VRAM — so the rule can be driven on its inputs alone, and it is deliberately STATELESS: the only things
// it accumulates are the census counts a diagnostic needs, and they are reported through one accessor
// rather than reached into from another translation unit.
//
// WHY THE RULE IS NOT "ADD A BIAS RAMP". A previous form re-ordered every face of every object by
// view-independent storage order. Measured net-negative: it fixed 95 of 6 inversion cases in the barrel
// it was written for and flipped 173 of 311 elsewhere. The rule here is a discriminator, not a nudge.
#ifndef PSXPORT_FACE_CONTEST_H
#define PSXPORT_FACE_CONTEST_H

#include <cstdint>

struct RqItem;

namespace psx::gpu {

// The screen bbox and depth range of one face, as the cheap rejects consume them.
//
// `nearest`/`farthest` are the face's bounds in the renderer's depth convention, where a LARGER value
// is NEARER. That inversion is the renderer's, not the guest's, and every comparison in this owner
// depends on it: a reject written as `farthest > nearest` is a reject that can never fire if the
// convention is assumed rather than checked.
struct FaceExtent {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  float nearest = 0.0f;
  float farthest = 0.0f;
};

// One face's two triangles, each precomputed for repeated barycentric evaluation. See faceSetup().
//
// `den` is the triangle's doubled signed area; zero means degenerate, and every consumer skips such a
// triangle rather than dividing by it. The sign flips with the winding, so a PSX quad's two triangles
// need not agree — which is why winding is normalised inside the contest rather than assumed.
struct FaceTriangleSetup {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
  float den = 0.0f;
  float d0 = 0.0f, d1 = 0.0f, d2 = 0.0f;
};

struct FaceSetup {
  int triangleCount = 0;
  FaceTriangleSetup triangle[2];
};

// The vertex position the RASTERIZER consumes: sub-pixel float XY when the producer supplied it, the
// rounded integer XY otherwise. Every geometric test in this owner must agree with the rasterizer on
// this, or it is reasoning about a different polygon than the one that gets drawn.
float rasterizerVertexX(const RqItem &item, int vertex);
float rasterizerVertexY(const RqItem &item, int vertex);

// Precompute everything that depends only on the face. The interior contest samples many points and
// asks each face for its interpolated depth at every one, so without this the six vertex fetches and the
// determinant are recomputed once per SAMPLE rather than once per face — measured at 18.81% of a whole
// frame, the single largest entry in the host profile.
FaceSetup faceSetup(const RqItem &item);
// The face's screen bbox and depth range, computed the same way and for the same reason: measured at
// 16.18% of a frame when recomputed per partner.
FaceExtent faceExtent(const RqItem &item);

// The per-face snapshot the debug pixel-probe resolves, taken with a caller-supplied setup so the
// expensive precompute is not repeated per sample. Declared here because the setup is this owner's.
bool faceDepthAt(const FaceSetup &setup, float x, float y, float *outDepth);

// The CONTEST, taking precomputed extents and setups.
//
// There are exactly TWO ways a pair is in contest, and they are different rules with different evidence:
//
//   SAME KEY   the game filed both in the SAME ordering-table bucket, so the key expresses no order at
//              all and real depth is normally right. The one exception is a pair that is EXACTLY
//              coincident — a decal quad filed on the wall quad it decorates: same corners, same
//              depths — but listed with a ROTATED vertex order. The quad triangulation is fixed at
//              (0,1,2)+(1,2,3), so a rotation splits the two faces on OPPOSITE diagonals and their
//              interiors then interpolate differently. Such a pair carries no depth to preserve.
//
//   KEYS DIFFER the game DECLARED an order (smaller OT index = nearer). They are in contest when the
//              depth buffer would INVERT that declaration: at some point strictly inside both polygons,
//              the farther-keyed face interpolates nearer.
//
// Both rules are SYMMETRIC in the two faces, which is what lets a caller treat "is this face in contest
// with anything" as an existence question and stop at the first witness.
//
// The census counts the rejects, so a caller can tell "no pair was in contest" from "the sweep never
// looked". Those are opposite answers and a silent zero reads as the second.
bool facesInContest(const RqItem &a,
                    const RqItem &b,
                    const FaceExtent &extentA,
                    const FaceExtent &extentB,
                    const FaceSetup &setupA,
                    const FaceSetup &setupB);

// The convenience form for a caller that does not already hold the extents and setups, including the
// brute-force oracle in tests/test_render_queue_keyorder.cpp. It delegates; there is one implementation
// of the rule and this is not a second one.
bool facesInContest(const RqItem &a, const RqItem &b);

// The rule's cumulative census: how many pairs each decision turned away, and how many reached the exact
// interior test. Cumulative across the PROCESS, exactly as the counters were before the rule moved out of
// the queue — a per-queue lifetime would silently reset them under a reader, and these are a diagnostic
// total rather than a per-frame measurement.
//
// This is the access the reporting caller uses. The counters are not exported, so nothing outside this
// owner can increment them: a second writer could not keep the identity "examined = rejected + reached"
// true, and that identity is the whole point of reporting them.
struct ContestCensus {
  // The two faces' screen rects do not overlap at all.
  std::uint64_t rejectedByBounds = 0;
  // The far-keyed face can never out-depth the near-keyed one anywhere.
  std::uint64_t rejectedByDepth = 0;
  // RETIRED 2026-08-20 with the separating-axis pre-filter (see face_contest.cpp). Always zero, and it is
  // kept only because the frame's census line prints it: a diagnostic that quietly drops a field makes
  // two runs' logs incomparable.
  std::uint64_t rejectedBySeparation = 0;
  // Pairs that survived both cheap rejects and reached the exact interior test.
  std::uint64_t reachedExactTest = 0;
  // ...of which an inversion was actually found.
  std::uint64_t foundInversion = 0;
};
ContestCensus contestCensus();

} // namespace psx::gpu

#endif // PSXPORT_FACE_CONTEST_H
