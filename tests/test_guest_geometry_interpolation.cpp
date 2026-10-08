// test_guest_geometry_interpolation — pairing a drawn primitive with the projections that made it, and
// the in-between that moves only what was paired.
//
// The in-between of a 60 fps title on the guest-geometry path is the captured frame with each proven
// vertex moved back toward where the SAME model vertex was projected a frame earlier. Each case below is
// a decision the pairing must make: pair and move, refuse a coincidence it cannot see through, leave an
// unscoped producer's primitive where the real frame drew it, and cut the log where the guest hands its
// table to the GPU rather than where the host presents.
//
// Hermetic: the projections are recorded through ProjectionProvenance's own API and the RqItems are built
// directly; no Core, no GTE, no GPU.
#include "testutil.h"

#include "guest_geometry_interpolation.h"
#include "projection_provenance.h"
#include "render_queue.h"

#include <memory>
#include <vector>

namespace {

using psxport::temporal::GuestGeometryInterpolation;
using psxport::temporal::ProjectionProvenance;
using psxport::temporal::ProjectionSample;

constexpr int kOffsetX = 86; // the host draw offset a 16:9 frame adds to every guest x
constexpr int kOffsetY = 0;

uint32_t sxy(int x, int y) {
  return (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) | static_cast<uint16_t>(x);
}

// One RTPT of three model vertices (0,0,0), (100,0,0), (0,100,0) under `transform` landing at `screen`.
ProjectionSample rtpt(const int (&screen)[3][2], uint32_t transform, int16_t modelBase = 0) {
  ProjectionSample sample;
  sample.count = 3;
  sample.inputs = {modelBase, 0, 0, static_cast<int16_t>(modelBase + 100), 0, 0, modelBase, 100, 0};
  for (int v = 0; v < 3; ++v) {
    sample.screen[v] = sxy(screen[v][0], screen[v][1]);
  }
  sample.transform[5] = transform; // a translation word: the camera or the object moved
  return sample;
}

RqItem triangle(const int (&screen)[3][2]) {
  RqItem item{};
  item.nv = 3;
  item.has_guest_xy = 1;
  for (int v = 0; v < 3; ++v) {
    item.guest_x[v] = static_cast<int16_t>(screen[v][0]);
    item.guest_y[v] = static_cast<int16_t>(screen[v][1]);
    item.xs[v] = screen[v][0] + kOffsetX;
    item.ys[v] = screen[v][1] + kOffsetY;
  }
  return item;
}

// Record one guest frame under `scope` and hand its table to the GPU.
void produce(ProjectionProvenance &log, uint64_t scope, const std::vector<ProjectionSample> &samples) {
  {
    const ProjectionProvenance::Scope open(log, scope);
    for (const ProjectionSample &sample : samples) {
      log.record(sample);
    }
  }
  log.sealForDraw();
}

constexpr int kPrevious[3][2] = {{10, 20}, {50, 20}, {10, 60}};
constexpr int kCurrent[3][2] = {{20, 30}, {60, 30}, {20, 70}};

} // namespace

// A vertex the previous frame projected moves halfway back at t = 0.5, in guest and host space alike,
// and the real frame's own item is left untouched.
static void test_paired_vertices_move_halfway(void) {
  ProjectionProvenance log;
  log.setArmed(true);
  GuestGeometryInterpolation interpolation;

  produce(log, 7, {rtpt(kPrevious, 1)});
  const std::vector<RqItem> previous{triangle(kPrevious)};
  interpolation.beginFrame(previous, log.frame());
  CHECK_EQ(interpolation.census().resolved, 1u);
  interpolation.rotate();
  log.clearFrame();

  produce(log, 7, {rtpt(kCurrent, 2)});
  const std::vector<RqItem> current{triangle(kCurrent)};
  interpolation.beginFrame(current, log.frame());
  CHECK(interpolation.hasPrevious());
  CHECK(interpolation.owns(current[0]));

  auto sink = std::make_unique<RenderQueue>();
  interpolation.emit(0.5f, *sink);
  CHECK_EQ(sink->n, 1);
  CHECK_EQ(interpolation.census().interpolatedVertices, 3u);
  for (int v = 0; v < 3; ++v) {
    CHECK_EQ(sink->items[0].guest_x[v], (kPrevious[v][0] + kCurrent[v][0]) / 2);
    CHECK_EQ(sink->items[0].guest_y[v], (kPrevious[v][1] + kCurrent[v][1]) / 2);
    CHECK_EQ(sink->items[0].xs[v], (kPrevious[v][0] + kCurrent[v][0]) / 2 + kOffsetX);
    CHECK_EQ(current[0].xs[v], kCurrent[v][0] + kOffsetX);
  }
}

// A primitive whose screen points no scoped projection produced (a 2D sprite, an unscoped producer) is
// not owned, so the in-between draws the captured item exactly where the real frame did.
static void test_unscoped_primitive_is_not_owned(void) {
  ProjectionProvenance log;
  log.setArmed(true);
  GuestGeometryInterpolation interpolation;
  {
    // Projected outside any scope: recorded nothing.
    log.record(rtpt(kCurrent, 1));
  }
  log.sealForDraw();
  CHECK_EQ(log.frame().size(), 0u);
  const std::vector<RqItem> items{triangle(kCurrent)};
  interpolation.beginFrame(items, log.frame());
  CHECK_EQ(interpolation.census().polygons, 1u);
  CHECK_EQ(interpolation.census().unmatched, 1u);
  CHECK(!interpolation.owns(items[0]));
}

// Two different model vertices of one scope and epoch that land on the same pixels now but were apart a
// frame ago: the in-between depends on which one the packet used, which the pairing cannot see, so it
// refuses the primitive rather than choosing.
static void test_coincidence_that_moved_differently_is_refused(void) {
  constexpr int kElsewhere[3][2] = {{200, 100}, {240, 100}, {200, 140}};
  ProjectionProvenance log;
  log.setArmed(true);
  GuestGeometryInterpolation interpolation;
  produce(log, 7, {rtpt(kPrevious, 1, 0), rtpt(kElsewhere, 1, 500)});
  const std::vector<RqItem> previous{triangle(kPrevious)};
  interpolation.beginFrame(previous, log.frame());
  interpolation.rotate();
  log.clearFrame();

  produce(log, 7, {rtpt(kCurrent, 1, 0), rtpt(kCurrent, 1, 500)});
  const std::vector<RqItem> items{triangle(kCurrent)};
  interpolation.beginFrame(items, log.frame());
  CHECK_EQ(interpolation.census().ambiguous, 1u);
  CHECK(!interpolation.owns(items[0]));
}

// Two producers drawing the same world geometry (different model scales, so different identities) on the
// same pixels in both frames: whichever wrote the packet, the in-between is the same, so it is resolved.
static void test_coincidence_with_one_past_is_resolved(void) {
  ProjectionProvenance log;
  log.setArmed(true);
  GuestGeometryInterpolation interpolation;
  const auto bothProducers = [&log](const int (&screen)[3][2]) {
    {
      const ProjectionProvenance::Scope first(log, 7);
      log.record(rtpt(screen, 1, 0));
    }
    {
      const ProjectionProvenance::Scope second(log, 9);
      log.record(rtpt(screen, 4, 500));
    }
    log.sealForDraw();
  };
  bothProducers(kPrevious);
  const std::vector<RqItem> previous{triangle(kPrevious)};
  interpolation.beginFrame(previous, log.frame());
  interpolation.rotate();
  log.clearFrame();

  bothProducers(kCurrent);
  const std::vector<RqItem> current{triangle(kCurrent)};
  interpolation.beginFrame(current, log.frame());
  CHECK_EQ(interpolation.census().resolved, 1u);
  auto sink = std::make_unique<RenderQueue>();
  interpolation.emit(0.5f, *sink);
  CHECK_EQ(sink->items[0].guest_x[1], (kPrevious[1][0] + kCurrent[1][0]) / 2);
}

// A face is read from the instruction that projected its corners together: another face of the same
// transform whose vertex shares one pixel (an edge seen end-on, a culled face) does not make it ambiguous.
static void test_a_face_is_read_from_its_own_instruction(void) {
  constexpr int kNeighbour[3][2] = {{20, 30}, {90, 90}, {120, 30}};
  ProjectionProvenance log;
  log.setArmed(true);
  GuestGeometryInterpolation interpolation;
  produce(log, 7, {rtpt(kPrevious, 1, 0), rtpt(kNeighbour, 1, 500)});
  const std::vector<RqItem> previous{triangle(kPrevious)};
  interpolation.beginFrame(previous, log.frame());
  interpolation.rotate();
  log.clearFrame();

  produce(log, 7, {rtpt(kCurrent, 2, 0), rtpt(kNeighbour, 2, 500)});
  const std::vector<RqItem> current{triangle(kCurrent)};
  interpolation.beginFrame(current, log.frame());
  CHECK_EQ(interpolation.census().resolved, 1u);
  CHECK_EQ(interpolation.census().ambiguous, 0u);
}

// Projections recorded after the table was handed to the GPU belong to the NEXT table: a guest that
// builds one table while the GPU draws the other must pair the drawn table with its own projections.
static void test_log_is_cut_at_the_table_hand_off(void) {
  ProjectionProvenance log;
  log.setArmed(true);
  produce(log, 7, {rtpt(kPrevious, 1)});
  {
    const ProjectionProvenance::Scope open(log, 7);
    log.record(rtpt(kCurrent, 2)); // the next table, still being built at present
  }
  CHECK_EQ(log.frame().size(), 3u);
  CHECK_EQ(log.seals(), 1u);
  CHECK_EQ(log.frame()[0].screenX, kPrevious[0][0]);
  log.clearFrame();
  CHECK_EQ(log.frame().size(), 0u);
  log.sealForDraw();
  CHECK_EQ(log.frame().size(), 3u);
  CHECK_EQ(log.frame()[0].screenX, kCurrent[0][0]);
}

// A vertex whose (scope, epoch, model vertex) the previous frame never projected is held at its real
// position, and a cleared history pairs nothing.
static void test_unpaired_vertices_hold(void) {
  ProjectionProvenance log;
  log.setArmed(true);
  GuestGeometryInterpolation interpolation;
  produce(log, 7, {rtpt(kPrevious, 1)});
  const std::vector<RqItem> previous{triangle(kPrevious)};
  interpolation.beginFrame(previous, log.frame());
  interpolation.rotate();
  log.clearFrame();

  produce(log, 8, {rtpt(kCurrent, 1)}); // a different producer instance
  const std::vector<RqItem> current{triangle(kCurrent)};
  interpolation.beginFrame(current, log.frame());
  auto sink = std::make_unique<RenderQueue>();
  interpolation.emit(0.5f, *sink);
  CHECK_EQ(interpolation.census().heldVertices, 3u);
  CHECK_EQ(interpolation.census().interpolatedVertices, 0u);
  CHECK_EQ(sink->items[0].xs[0], kCurrent[0][0] + kOffsetX);

  interpolation.clear();
  CHECK(!interpolation.hasPrevious());
}

int main(void) {
  RUN(paired_vertices_move_halfway);
  RUN(unscoped_primitive_is_not_owned);
  RUN(coincidence_that_moved_differently_is_refused);
  RUN(coincidence_with_one_past_is_resolved);
  RUN(a_face_is_read_from_its_own_instruction);
  RUN(log_is_cut_at_the_table_hand_off);
  RUN(unpaired_vertices_hold);
  return pt_summary();
}
