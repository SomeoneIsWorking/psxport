// guest_geometry_scene_source.h — a InBetweenStrategy whose in-between is the guest's own geometry.
//
// The framework half of a title's guest-geometry interpolation. The real frame is presented exactly as
// captured; the in-between is the captured frame with each provenance-proven vertex interpolated toward
// the previous real frame (GuestGeometryInterpolation). The title supplies the two things the framework
// cannot know: which guest calls are producer instances (ProjectionProvenance::Scope around them), and
// whether the current frame is continuous with the previous one at all.
#pragma once

#include "guest_geometry_interpolation.h"
#include "in_between_strategy.h"

namespace psxport::temporal {

class GuestGeometrySceneSource : public ::InBetweenStrategy {
public:
  void beginPresentation(Core &core, CapturedFrameView frame, bool interpolating) final;
  bool eligible(const Core &core) const final;
  bool owns(const RqItem &item) const final;
  void reconstruct(Core &core, float t) final;
  void rotate(Core &core) final;

  bool capturedQueueIsComplete() const final {
    return true;
  }
  // Its in-between is the guest's own captured primitives with provenance-proven vertices
  // interpolated, so it claims exactly what it always had: the guest's picture at another instant.
  GuestPathClaim guestPathClaim() const final {
    return GuestPathClaim::GuestPrimitives;
  }

protected:
  // May the frame about to be presented be paired with the previous one? False at a discontinuity the
  // title can prove — a camera cut, a scene change, a frame that is not gameplay — so the in-between
  // is skipped and pairing restarts from this frame. Asked once per presented frame.
  virtual bool continuousWithPrevious(Core &core) = 0;

private:
  GuestGeometryInterpolation interpolation_;
  bool frameResolved_ = false;
};

} // namespace psxport::temporal
