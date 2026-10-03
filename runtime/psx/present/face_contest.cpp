// face_contest.cpp — the face-contest rule: geometry, the two decisions, and their census.
//
// Split out of render_queue.cpp. The concept is the rule declared in face_contest.h, and nothing else:
// no queue state, no Core, no logging beyond the census the caller prints. What moved is the FILE — the
// queue owns WHEN the rule is asked and what it does with the answer, and this owns WHETHER.
//
// The arithmetic here is character-for-character what it was in render_queue.cpp, and it must stay so.
// The interior test keeps its DIVISION by the doubled area rather than multiplying by a reciprocal: the
// contest's answer is a comparison, and a boundary case that rounds the other way flips a face's snap
// decision — a visible ordering change, from a rounding difference. tests/test_render_queue_keyorder.cpp
// checks the rule against a brute-force oracle; that is what pins it.
#include "face_contest.h"

#include "render_queue.h"

namespace psx::gpu {

// One face's triangle, precomputed for repeated barycentric evaluation.
//
// `den` is the triangle's doubled signed AREA: zero means degenerate, and every consumer skips such a
// triangle rather than dividing by it. Its sign flips with the winding, so a PSX quad's two triangles
// need not agree — which is why the contest normalises winding instead of assuming it.

namespace {

// WHY THERE IS NO SEPARATING-AXIS PRE-FILTER HERE, having tried one (2026-08-20). A 2D SAT over the
// two faces' triangles is a correct way to skip the exact test, and it was implemented, verified to
// produce IDENTICAL snap decisions (109/262 faces, 152,304 inversions, 0 of 524,288 pixels differing),
// and then REMOVED because it is not worth it:
//
//     exact test without it   2,374,502     with it   1,964,280      -17.3%
//     wall clock              5.044 s                 4.962 s        -1.6%
//
// It pays ~150 float ops on every pair that reaches here to skip 64 evaluations on 17% of them.
//
// IT ALSO CARRIED A REAL BUG WORTH REMEMBERING. The first version separated on `<=`, treating faces
// that touch exactly at an edge as disjoint. The interior test admits points ON the boundary, so
// touching faces DO share admissible points, and the filter silently dropped inversions: 98 of 262 faces
// snapped instead of 109. The hermetic oracle test passed either way, because it does not exercise
// edge-touching pairs. Only the real scene's snap count showed it.
//
// THE FIX THAT WOULD ACTUALLY WORK is not a better filter but a better test. Each triangle interpolates
// depth LINEARLY in screen space, so (far - near) is affine over the region where both are defined; its
// extreme over a convex overlap lies at a VERTEX of that overlap. That is what facesInvert() does.

// A triangle interpolates depth with barycentrics that are themselves affine in (x,y), so
// depth(x,y) = A*x + B*y + C over that triangle. Derived from faceDepthAt()'s own expression:
//     depth = d2 + l0*(d0-d2) + l1*(d1-d2),  with l0,l1 affine  =>  A,B,C below.
struct OrdPlane {
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
};

OrdPlane ordPlane(const FaceTriangleSetup &triangle) {
  const float edge0 = triangle.d0 - triangle.d2;
  const float edge1 = triangle.d1 - triangle.d2;
  OrdPlane plane;
  plane.a = ((triangle.y1 - triangle.y2) * edge0 + (triangle.y2 - triangle.y0) * edge1) / triangle.den;
  plane.b = ((triangle.x2 - triangle.x1) * edge0 + (triangle.x0 - triangle.x2) * edge1) / triangle.den;
  plane.c = triangle.d2 - plane.a * triangle.x2 - plane.b * triangle.y2;
  return plane;
}

// THE CENSUS. Cumulative across the process, exactly as the counters were before the rule moved out of
// the queue: a per-queue lifetime would silently reset them under a reader, and these are a DIAGNOSTIC
// total rather than a per-frame measurement.
//
ContestCensus &census() {
  static ContestCensus counts;
  return counts;
}

// The public surface begins here. Everything above is private to this file: the affine depth plane and
// the census counters have no business being reachable from another translation unit, and a reader
// looking for the rule's API should find it in the header.
} // namespace

// The guest-RAM window a real entity node lives in. The reserved dbg_node sentinels
// (kTerrainDbgNode / kSceneTableDbgNode / kBackdropDbgNode, render_queue.h) sit far above it, which is
// what lets the queue's gather take "is this a real object" as an address test.
constexpr std::uint32_t kGuestRamBase = 0x80000000u;
constexpr std::uint32_t kGuestRamEnd = 0x80200000u;

float rasterizerVertexX(const RqItem &item, int vertex) {
  return item.has_xyf ? item.xsf[vertex] : static_cast<float>(item.xs[vertex]);
}

float rasterizerVertexY(const RqItem &item, int vertex) {
  return item.has_xyf ? item.ysf[vertex] : static_cast<float>(item.ys[vertex]);
}

// Interpolated ord of item `it` at screen point (x,y), using the same triangle split + barycentric the
// rasterizer applies (tri 0 = verts 0,1,2; tri 1 = verts 1,2,3). Returns false when outside both tris.
// TEMPORARY CENSUS (kanban #118) — how far does a pair actually get? The contest's cost is the 8x8
// interior grid, and the grid only runs for pairs that pass BOTH cheap rejects. These four counters
// say whether the expensive path is rare-and-worth-it or common-and-wasteful, which decides whether
// the fix is a better filter or a different design.

// BROADPHASE DENOMINATORS. Without these, "the pair tests dropped by 90%" and "the search silently
// stopped looking" print the same number. `examined` counts every candidate the sweep actually looked
// at, `skip_settled` those both of whose faces were already snapped (testing them could only re-set a
// bit that is already set), and `reject_y` those the sweep rejected on the y axis before offering the
// pair to the contest. examined = skip_settled + reject_y + (pairs offered to the contest).

// EVERYTHING IN THE BARYCENTRIC SETUP THAT DEPENDS ONLY ON THE FACE, lifted out of the per-sample
// loop. The interior contest samples an 8x8 grid and asks each face for its interpolated ord at every
// point, so the six vertex fetches and the determinant were being recomputed 64 times per face per
// PAIR — and the host profile put rq_ord_at at 18.81% of the whole frame, the single largest entry.
// The arithmetic below is character-for-character what the per-sample code did, including keeping the
// DIVISION by `den` rather than multiplying by a reciprocal: this must stay bit-identical, because
// the contest's answer is a comparison and a boundary case that rounds the other way flips a face's
// snap decision. tests/test_render_queue_keyorder.cpp checks the rule against a brute-force oracle.
FaceSetup faceSetup(const RqItem &it) {
  const int nv = it.nv ? it.nv : 4;
  FaceSetup f;
  if (nv < 3) {
    return f;
  }
  f.triangleCount = (nv == 4 ? 2 : 1);
  for (int t = 0; t < f.triangleCount; t++) {
    const int i0 = t, i1 = t + 1, i2 = t + 2;
    FaceTriangleSetup &triangle = f.triangle[t];
    triangle.x0 = rasterizerVertexX(it, i0);
    triangle.y0 = rasterizerVertexY(it, i0);
    triangle.x1 = rasterizerVertexX(it, i1);
    triangle.y1 = rasterizerVertexY(it, i1);
    triangle.x2 = rasterizerVertexX(it, i2);
    triangle.y2 = rasterizerVertexY(it, i2);
    triangle.den = (triangle.y1 - triangle.y2) * (triangle.x0 - triangle.x2) +
                   (triangle.x2 - triangle.x1) * (triangle.y0 - triangle.y2);
    triangle.d0 = it.depth[i0];
    triangle.d1 = it.depth[i1];
    triangle.d2 = it.depth[i2];
  }
  return f;
}

// WHY THERE IS NO SEPARATING-AXIS PRE-FILTER HERE, having tried one (2026-08-20). A 2D SAT over the
// two faces' triangles is a correct way to skip the grid — the grid can only fire at a point admissible
// to both faces — and it was implemented, verified to produce IDENTICAL snap decisions (109/262 faces,
// 152,304 inversions, 0 of 524,288 pixels differing), and then REMOVED, because it is not worth it:
//
//     grid runs without it   2,374,502     with it   1,964,280      -17.3%
//     wall clock             5.044 s                 4.962 s        -1.6%
//
// It pays ~150 float ops on every pair that reaches here to skip 64 samples on 17% of them. The census
// says why the yield is low: pairs that get this far are genuinely OVERLAPPING faces of one object, so
// separation is the uncommon case, not the common one.
//
// IT ALSO CARRIED A REAL BUG WORTH REMEMBERING. The first version separated on `<=`, treating faces
// that touch exactly at an edge as disjoint. faceDepthAt admits samples with barycentrics >= 0 —
// points ON the edge — so touching faces DO share admissible points, and the filter silently dropped
// inversions: 98 of 262 faces snapped instead of 109. The hermetic oracle test passed either way,
// because it does not exercise edge-touching pairs. Only the real scene's snap count showed it.
//
// THE FIX THAT WOULD ACTUALLY WORK is not a better filter but a better test. Each triangle interpolates
// ord LINEARLY in screen space, so (ord_far - ord_near) is a linear function over the region where both
// are defined; its extreme over a convex overlap lies at a VERTEX of that overlap. That replaces 64
// samples with a handful of evaluations AND removes the documented sub-sample sliver miss, because it
// is exact rather than sampled. That is the next move on this code, not another pre-filter.

// ORD IS AFFINE IN SCREEN SPACE, so the contest does not need sampling at all.
//
// Does the far triangle interpolate NEARER than the near one anywhere both cover? Exact, by clipping
// one triangle against the other (Sutherland-Hodgman, convex) and evaluating the affine difference at
// the surviving vertices. Winding is normalised from `den`'s sign so "inside" is consistent for either
// orientation — a PSX quad's two triangles need not wind the same way.
static bool trianglePairInverts(const FaceTriangleSetup &nearT, const FaceTriangleSetup &farT) {
  float px[8], py[8];
  int n = 3;
  px[0] = nearT.x0;
  py[0] = nearT.y0;
  px[1] = nearT.x1;
  py[1] = nearT.y1;
  px[2] = nearT.x2;
  py[2] = nearT.y2;

  const float fx[3] = {farT.x0, farT.x1, farT.x2};
  const float fy[3] = {farT.y0, farT.y1, farT.y2};
  const float wind = farT.den >= 0.f ? 1.f : -1.f;
  for (int e = 0; e < 3 && n; e++) {
    const int e1 = (e + 1) % 3;
    const float ex = fx[e1] - fx[e], ey = fy[e1] - fy[e];
    float qx[8], qy[8];
    int m = 0;
    for (int i = 0; i < n; i++) {
      const int j = (i + 1) % n;
      // Signed area of (edge, point): >= 0 is inside for the normalised winding.
      const float di = wind * (ex * (py[i] - fy[e]) - ey * (px[i] - fx[e]));
      const float dj = wind * (ex * (py[j] - fy[e]) - ey * (px[j] - fx[e]));
      if (di >= 0.f) {
        qx[m] = px[i];
        qy[m] = py[i];
        m++;
      }
      if ((di >= 0.f) != (dj >= 0.f) && m < 8) {
        const float t = di / (di - dj);
        qx[m] = px[i] + t * (px[j] - px[i]);
        qy[m] = py[i] + t * (py[j] - py[i]);
        m++;
      }
    }
    n = m;
    for (int i = 0; i < n; i++) {
      px[i] = qx[i];
      py[i] = qy[i];
    }
  }
  if (n < 3) {
    return false; // interiors disjoint — nothing to contest
  }
  // THE SHARED REGION MUST HAVE AREA. Clipping admits points ON the boundary, so two mesh-adjacent
  // faces meeting along an edge survive as a degenerate sliver — three or more vertices, zero area —
  // and the affine difference is generally non-zero along that edge, which would report an inversion
  // for every adjacent pair in a mesh. The sampled grid never did: a measure-zero region contains no
  // sample point, and the file's own comment says adjacent faces must not hit. Measured when this was
  // missing: 221 of 262 faces snapped instead of 109, and inversions found went 152,304 -> 385,622.
  float area2 = 0.f;
  for (int i = 0; i < n; i++) {
    const int j = (i + 1) % n;
    area2 += px[i] * py[j] - px[j] * py[i];
  }
  if (area2 < 0.f) {
    area2 = -area2;
  }
  if (area2 <= 0.f) {
    return false;
  }
  const OrdPlane pn = ordPlane(nearT), pf = ordPlane(farT);
  const float da = pf.a - pn.a, db = pf.b - pn.b, dc = pf.c - pn.c;
  for (int i = 0; i < n; i++) {
    if (da * px[i] + db * py[i] + dc > 0.f) {
      return true; // the far face interpolates nearer here
    }
  }
  return false;
}

static bool facesInvert(const FaceSetup &nearF, const FaceSetup &farF) {
  for (int a = 0; a < nearF.triangleCount; a++) {
    if (nearF.triangle[a].den == 0.f) {
      continue;
    }
    for (int b = 0; b < farF.triangleCount; b++) {
      if (farF.triangle[b].den == 0.f) {
        continue;
      }
      if (trianglePairInverts(nearF.triangle[a], farF.triangle[b])) {
        return true;
      }
    }
  }
  return false;
}

bool faceDepthAt(const FaceSetup &f, float x, float y, float *out) {
  for (int t = 0; t < f.triangleCount; t++) {
    const FaceTriangleSetup &triangle = f.triangle[t];
    if (triangle.den == 0.f) {
      continue;
    }
    const float l0 =
        ((triangle.y1 - triangle.y2) * (x - triangle.x2) + (triangle.x2 - triangle.x1) * (y - triangle.y2)) /
        triangle.den;
    const float l1 =
        ((triangle.y2 - triangle.y0) * (x - triangle.x2) + (triangle.x0 - triangle.x2) * (y - triangle.y2)) /
        triangle.den;
    const float l2 = 1.f - l0 - l1;
    if (l0 < 0.f || l1 < 0.f || l2 < 0.f) {
      continue; // outside this triangle (strict interior sampling)
    }
    *out = l0 * triangle.d0 + l1 * triangle.d1 + l2 * triangle.d2;
    return true;
  }
  return false;
}

// Do two faces occupy the IDENTICAL polygon — same vertex count, and every vertex of one matching a
// vertex of the other in screen position AND depth? Exact comparison on the values the rasterizer
// actually consumes (float XY when the producer supplied sub-pixel XY, integer XY otherwise), so this
// is a structural test, not a proximity test: a decal filed on the face it decorates matches, ordinary
// neighbouring or overlapping geometry does not. Vertex ORDER is deliberately ignored — a rotated
// listing of the same corners is precisely the case that defeats the depth buffer (see the caller).
static bool facesCoincident(const RqItem &A, const RqItem &B) {
  const int nv = A.nv ? A.nv : 4;
  if ((B.nv ? B.nv : 4) != nv) {
    return false;
  }
  if (A.has_xyf != B.has_xyf) {
    return false;
  }
  bool used[4] = {false, false, false, false};
  for (int i = 0; i < nv; i++) {
    int m = -1;
    for (int j = 0; j < nv && m < 0; j++) {
      if (!used[j] && rasterizerVertexX(A, i) == rasterizerVertexX(B, j) &&
          rasterizerVertexY(A, i) == rasterizerVertexY(B, j) && A.depth[i] == B.depth[j]) {
        m = j;
      }
    }
    if (m < 0) {
      return false;
    }
    used[m] = true;
  }
  return true;
}

FaceExtent faceExtent(const RqItem &it) {
  const int nv = it.nv ? it.nv : 4;
  FaceExtent e;
  e.x0 = e.x1 = rasterizerVertexX(it, 0);
  e.y0 = e.y1 = rasterizerVertexY(it, 0);
  e.farthest = e.nearest = it.depth[0];
  for (int k = 1; k < nv; k++) {
    const float x = rasterizerVertexX(it, k), y = rasterizerVertexY(it, k);
    if (x < e.x0) {
      e.x0 = x;
    }
    if (x > e.x1) {
      e.x1 = x;
    }
    if (y < e.y0) {
      e.y0 = y;
    }
    if (y > e.y1) {
      e.y1 = y;
    }
    // A LARGER depth value is NEARER, so the face's largest value is its nearest bound and its smallest
    // is its farthest. The comparison below is where a "bigger means closer" assumption would go wrong,
    // and it is why the reject at the call site reads `far.nearest <= near.farthest`.
    if (it.depth[k] < e.farthest) {
      e.farthest = it.depth[k];
    }
    if (it.depth[k] > e.nearest) {
      e.nearest = it.depth[k];
    }
  }
  return e;
}

// The real body, taking PRECOMPUTED extents and setups. An extent depends only on the face, but the
// contest is asked about PAIRS, so recomputing one per call recomputes a face's extent once per partner
// it is tested against. Measured on Tomba!2's native path at f1201: 14,772 pair tests over 262 keyed
// faces = 29,544 extent computations of 262 distinct values, and faceExtent was 16.18% of the frame in
// the host profile. The caller that has the whole group in hand computes each once.
bool facesInContest(const RqItem &A,
                    const RqItem &B,
                    const FaceExtent &extA,
                    const FaceExtent &extB,
                    const FaceSetup &setA,
                    const FaceSetup &setB) {
  if (A.sort_key == B.sort_key) { // SAME OT BUCKET (kanban #29 — hut wall decals)
    return facesCoincident(A, B);
  }

  // near = the face the game files NEARER (smaller OT index); far = the other.
  const bool a_is_near = A.sort_key < B.sort_key;
  const RqItem &near_face = a_is_near ? A : B;
  const RqItem &far_face = a_is_near ? B : A;
  const FaceExtent &near_ext = a_is_near ? extA : extB;
  const FaceExtent &far_ext = a_is_near ? extB : extA;
  const FaceSetup &near_set = a_is_near ? setA : setB;
  const FaceSetup &far_set = a_is_near ? setB : setA;

  ContestCensus &counts = census();
  // Cheap rejects. No screen overlap at all, or the far face can never out-depth the near one (ord:
  // larger = nearer, so an inversion requires the far face's NEAREST bound to exceed the near
  // face's FARTHEST bound — the two must actually cross.
  const float ox0 = near_ext.x0 > far_ext.x0 ? near_ext.x0 : far_ext.x0;
  const float ox1 = near_ext.x1 < far_ext.x1 ? near_ext.x1 : far_ext.x1;
  const float oy0 = near_ext.y0 > far_ext.y0 ? near_ext.y0 : far_ext.y0;
  const float oy1 = near_ext.y1 < far_ext.y1 ? near_ext.y1 : far_ext.y1;
  if (ox0 >= ox1 || oy0 >= oy1) {
    counts.rejectedByBounds++;
    return false;
  }
  if (far_ext.nearest <= near_ext.farthest) {
    counts.rejectedByDepth++;
    return false;
  }
  // Counted ONCE. This increment used to appear twice with no branch between (here and again just
  // before facesInvert), so every "REACHED GRID" figure this census ever printed was exactly 2x
  // the truth — including the ones quoted on kanban #118.
  counts.reachedExactTest++;

  // EXACT INTERIOR CONTEST. This was an 8x8 SAMPLED grid: 64 points over the bbox intersection, two
  // barycentric interpolations each, returning true on the first inversion found. It cost 20.8% of a
  // 3D frame in the host profile — the single largest entry — and it could MISS, because a sampled
  // test only sees inversions wide enough to contain a sample point (the "sub-pixel slivers"
  // documented as a known residual).
  //
  // Both problems have the same cause: sampling something that does not need to be sampled. ord is
  // AFFINE per triangle, so the difference between the two faces is affine over the region they
  // share, that region is convex, and an affine function's maximum over a convex polygon is at a
  // VERTEX. Clip one triangle by the other and test the surviving vertices — exact, and a handful of
  // evaluations instead of 128.
  if (facesInvert(near_set, far_set)) {
    counts.foundInversion++;
    return true;
  }
  return false;
}

// The census, by value. A COPY on purpose: a caller cannot write back a value it invented, so the
// identity the frame's line reports — examined = rejected + reached — cannot be broken from outside.
ContestCensus contestCensus() {
  return census();
}

// The convenience form: compute the two extents and setups, then ask the one rule. Declared in the
// header, and what the brute-force oracle in tests/test_render_queue_keyorder.cpp calls.
bool facesInContest(const RqItem &a, const RqItem &b) {
  const FaceExtent extentA = faceExtent(a);
  const FaceExtent extentB = faceExtent(b);
  const FaceSetup setupA = faceSetup(a);
  const FaceSetup setupB = faceSetup(b);
  return facesInContest(a, b, extentA, extentB, setupA, setupB);
}

} // namespace psx::gpu
