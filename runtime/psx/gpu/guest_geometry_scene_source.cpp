#include "guest_geometry_scene_source.h"

#include "core.h"
#include "game.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psxport::temporal {
namespace {
const lucent::Channel censusChannel{"fps60guest"};
} // namespace

void GuestGeometrySceneSource::beginPresentation(Core &core, psx::frame::CapturedFrameView frame, bool interpolating) {
  ProjectionProvenance &provenance = core.rsub.projectionProvenance;
  // Recording costs a few registers per projection; it runs only while an in-between is wanted.
  provenance.setArmed(interpolating);
  frameResolved_ = false;
  if (!interpolating) {
    interpolation_.clear();
    return;
  }
  if (!provenance.complete()) {
    // An incomplete log cannot prove anything about this frame, and pairing the next frame with it
    // would pair against a partial endpoint.
    interpolation_.clear();
    lucent::warn("fps60", "projection log overflowed this frame; its in-between is skipped");
    return;
  }
  if (!continuousWithPrevious(core)) {
    interpolation_.clear();
  }
  interpolation_.beginFrame(frame.items, provenance.frame());
  frameResolved_ = true;
  const GuestGeometryInterpolation::Census &census = interpolation_.census();
  lucent::debug(censusChannel,
                "f{} items={} polygons={} resolved={} unmatched={} ambiguous={} mapped={} projections={} "
                "seals={} previous={}",
                frame.fence,
                census.items,
                census.polygons,
                census.resolved,
                census.unmatched,
                census.ambiguous,
                census.mapped,
                provenance.frame().size(),
                provenance.seals(),
                interpolation_.hasPrevious());
}

bool GuestGeometrySceneSource::eligible(const Core &core) const {
  (void)core;
  return frameResolved_ && interpolation_.hasPrevious();
}

bool GuestGeometrySceneSource::owns(const RqItem &item) const {
  return frameResolved_ && interpolation_.owns(item);
}

void GuestGeometrySceneSource::reconstruct(Core &core, float t) {
  RenderQueue *const sink = core.game->rqRedirect;
  if (sink == nullptr) {
    lucent::error("fps60", "guest-geometry in-between requested with no reconstruction queue open");
    std::abort();
  }
  interpolation_.emit(t, *sink);
  const GuestGeometryInterpolation::Census &census = interpolation_.census();
  lucent::debug(censusChannel,
                "t={:.3f} emitted={} interpolated-vertices={} held-vertices={}",
                t,
                census.resolved,
                census.interpolatedVertices,
                census.heldVertices);
}

void GuestGeometrySceneSource::rotate(Core &core) {
  interpolation_.rotate();
  core.rsub.projectionProvenance.clearFrame();
  frameResolved_ = false;
}

} // namespace psxport::temporal
