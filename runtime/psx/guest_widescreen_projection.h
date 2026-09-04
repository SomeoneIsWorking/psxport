// guest_widescreen_projection.h — title-owned projection matched to a host presentation extent.
#pragma once

#include "render_mode.h"
#include "video_plan.h"

#include <cstdint>

class Core;

enum class PresentationAspect {
  Standard4x3,
  Wide16x9,
  UltraWide21x9,
  MatchSink,
};

// A title implements this only when its own guest code owns projection, culling and layout changes.
// Returning a wide aspect does not patch guest state and does not stretch the picture: the title must
// explicitly latch the matching plan when it publishes its guest projection.
class GuestWidescreenProjection {
public:
  virtual ~GuestWidescreenProjection() = default;
  virtual PresentationAspect presentationAspect(const Core &core) const = 0;
};

struct GuestPresentationExtent {
  int width = 0;
  int height = 0;
};

struct GuestProjectionGeometry {
  GuestPresentationExtent extent;
  int drawWidth = 0;

  bool valid() const {
    return extent.width > 0 && extent.height > 0 && drawWidth > 0;
  }
};

struct GuestProjectionPlan {
  PresentationAspect aspect = PresentationAspect::Standard4x3;
  GuestPresentationExtent nativeExtent;
  GuestPresentationExtent presentationExtent;
  GuestPresentationExtent nativeProjectionExtent;
  GuestPresentationExtent projectionExtent;
  int nativeGuestDrawWidth = 0;
  int guestDrawWidth = 0;
  int presentationHorizontalMargin = 0;
  int projectionHorizontalMargin = 0;
  int projectionCenterX = 0;
  int projectionClipRight = 0;
  int guestClipRight = 0;

  bool widescreen() const {
    return presentationExtent.width > nativeExtent.width;
  }
};

// Widen one title-authored 4:3 horizontal extent to the requested presentation aspect. The result
// is the smallest even width that does not undershoot that aspect: 320->428 and 368->492 at 16:9,
// while an exactly divisible 384-wide projection becomes 512 rather than inheriting 320's rounding.
// This is deliberately independent of the host-native video plan, whose legacy 320 reference targets
// are a different ownership domain.
inline int
guest_wide_extent_width(int nativeWidth, int sinkWidth, int sinkHeight, PresentationAspect aspect, int vramWidth) {
  if (nativeWidth <= 0 || aspect == PresentationAspect::Standard4x3) {
    return nativeWidth;
  }

  std::int64_t numerator = nativeWidth;
  std::int64_t denominator = 1;
  switch (aspect) {
  case PresentationAspect::Wide16x9:
    numerator *= 4;
    denominator = 3;
    break;
  case PresentationAspect::UltraWide21x9:
    numerator *= 7;
    denominator = 4;
    break;
  case PresentationAspect::MatchSink:
    if (sinkWidth <= 0 || sinkHeight <= 0) {
      return nativeWidth;
    }
    numerator *= static_cast<std::int64_t>(3) * sinkWidth;
    denominator = static_cast<std::int64_t>(4) * sinkHeight;
    break;
  case PresentationAspect::Standard4x3:
    return nativeWidth;
  }

  if (numerator <= static_cast<std::int64_t>(nativeWidth) * denominator) {
    return nativeWidth;
  }
  std::int64_t widened = (numerator + denominator - 1) / denominator;
  if ((widened & 1) != 0) {
    ++widened;
  }
  if (vramWidth > 0 && widened > vramWidth) {
    widened = vramWidth & ~1;
  }
  return static_cast<int>(widened);
}

inline GuestPresentationExtent guest_presentation_extent(
    int nativeWidth, int nativeHeight, int sinkWidth, int sinkHeight, PresentationAspect aspect, int vramWidth) {
  return {guest_wide_extent_width(nativeWidth, sinkWidth, sinkHeight, aspect, vramWidth), nativeHeight};
}

// Pure plan builder shared by the runtime latch and its falsifiers. Only the GTE path may expose a
// title-authored guest projection; Native has its existing host-owned enhancement path and Psx remains
// the 4:3 reference picture.
struct GuestProjectionInputs {
  RenderPath path = RenderPath::Native;
  PresentationAspect requested = PresentationAspect::Standard4x3;
  GuestPresentationExtent nativePresentation;
  GuestProjectionGeometry nativeProjection;
  GuestPresentationExtent sink;
  int vramWidth = 0;
};

inline GuestProjectionPlan guest_projection_plan(const GuestProjectionInputs &inputs) {
  GuestProjectionPlan plan;
  plan.nativeExtent = inputs.nativePresentation;
  plan.nativeProjectionExtent = inputs.nativeProjection.extent;
  plan.nativeGuestDrawWidth = inputs.nativeProjection.drawWidth;
  plan.aspect = inputs.path == RenderPath::Gte ? inputs.requested : PresentationAspect::Standard4x3;
  plan.presentationExtent = guest_presentation_extent(inputs.nativePresentation.width,
                                                      inputs.nativePresentation.height,
                                                      inputs.sink.width,
                                                      inputs.sink.height,
                                                      plan.aspect,
                                                      inputs.vramWidth);
  plan.projectionExtent = guest_presentation_extent(inputs.nativeProjection.extent.width,
                                                    inputs.nativeProjection.extent.height,
                                                    inputs.sink.width,
                                                    inputs.sink.height,
                                                    plan.aspect,
                                                    inputs.vramWidth);
  const GuestPresentationExtent guestDrawExtent = guest_presentation_extent(inputs.nativeProjection.drawWidth,
                                                                            inputs.nativePresentation.height,
                                                                            inputs.sink.width,
                                                                            inputs.sink.height,
                                                                            plan.aspect,
                                                                            inputs.vramWidth);
  plan.guestDrawWidth = guestDrawExtent.width;
  plan.presentationHorizontalMargin = (plan.presentationExtent.width - inputs.nativePresentation.width) / 2;
  plan.projectionHorizontalMargin = (plan.projectionExtent.width - inputs.nativeProjection.extent.width) / 2;
  plan.projectionCenterX = plan.projectionExtent.width / 2;
  plan.projectionClipRight = plan.projectionExtent.width - 1;
  plan.guestClipRight = plan.guestDrawWidth - 1;
  return plan;
}

// Per-Game latch. The host presenter consumes exactly this stored plan; it never calls a title policy
// late and cannot widen a picture until the title has positively published matching guest projection.
class GuestPresentationState {
public:
  void latch(GuestProjectionPlan plan) {
    plan_ = plan;
  }

  const GuestProjectionPlan &plan() const {
    return plan_;
  }

private:
  GuestProjectionPlan plan_;
};
