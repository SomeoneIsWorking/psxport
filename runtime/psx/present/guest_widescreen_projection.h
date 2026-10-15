// guest_widescreen_projection.h — title-owned projection matched to a host presentation extent.
#pragma once

#include "pixel_size.h"
#include "render_mode.h"
#include "video_plan.h"

#include <cstdint>

class Core;

enum class PresentationAspect : std::uint8_t {
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
  // The player's aspect setting; a title overrides this only to hold 4:3 in some state.
  virtual PresentationAspect presentationAspect(const Core &core) const;
  // True while the guest's screen coordinates already come out of the widened projection (a 3D
  // frame), so the host must not centre them again. A title that cannot tell leaves it false.
  virtual bool guestCoordinatesWidened(const Core &core) const;
};

struct GuestProjectionGeometry {
  psx::present::PixelSize extent;
  int drawWidth = 0;

  bool valid() const {
    return extent.width > 0 && extent.height > 0 && drawWidth > 0;
  }
};

struct GuestProjectionPlan {
  PresentationAspect aspect = PresentationAspect::Standard4x3;
  psx::present::PixelSize nativeExtent;
  psx::present::PixelSize presentationExtent;
  psx::present::PixelSize nativeProjectionExtent;
  psx::present::PixelSize projectionExtent;
  int nativeGuestDrawWidth = 0;
  int guestDrawWidth = 0;
  int presentationHorizontalMargin = 0;
  int projectionHorizontalMargin = 0;
  int projectionCenterX = 0;
  int projectionClipRight = 0;
  int guestClipLeft = 0;
  int guestClipRight = 0;

  bool widescreen() const {
    return presentationExtent.width > nativeExtent.width;
  }
};

namespace psx::present {

// Widen one title-authored 4:3 horizontal extent to the requested presentation aspect. The result
// is the smallest even width that does not undershoot that aspect: 320->428 and 368->492 at 16:9,
// while an exactly divisible 384-wide projection becomes 512 rather than inheriting 320's rounding.
// This is deliberately independent of the host-native video plan, whose legacy 320 reference targets
// are a different ownership domain.
// The canvas a 4:3 extent is widened onto: the sink's shape for MatchSink, capped at the VRAM width.
struct WideningTarget {
  PixelSize sink;
  PresentationAspect aspect = PresentationAspect::Standard4x3;
  int vramWidth = 0;
};

inline int guestWideExtentWidth(int nativeWidth, const WideningTarget &target) {
  const PresentationAspect aspect = target.aspect;
  const int sinkWidth = target.sink.width;
  const int sinkHeight = target.sink.height;
  const int vramWidth = target.vramWidth;
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

inline PixelSize guestPresentationExtent(PixelSize native, const WideningTarget &target) {
  return {guestWideExtentWidth(native.width, target), native.height};
}

} // namespace psx::present

// Pure plan builder shared by the runtime latches and their falsifiers. Only the record path exposes a
// title-declared guest widening; Device remains the 4:3 reference picture.
struct GuestProjectionInputs {
  RenderPath path = RenderPath::Device;
  PresentationAspect requested = PresentationAspect::Standard4x3;
  psx::present::PixelSize nativePresentation;
  GuestProjectionGeometry nativeProjection;
  psx::present::PixelSize sink;
  int vramWidth = 0;
};

namespace psx::present {

inline GuestProjectionPlan guestProjectionPlan(const GuestProjectionInputs &inputs) {
  GuestProjectionPlan plan;
  plan.nativeExtent = inputs.nativePresentation;
  plan.nativeProjectionExtent = inputs.nativeProjection.extent;
  plan.nativeGuestDrawWidth = inputs.nativeProjection.drawWidth;
  plan.aspect = inputs.path == RenderPath::Record ? inputs.requested : PresentationAspect::Standard4x3;
  const WideningTarget target{.sink = inputs.sink, .aspect = plan.aspect, .vramWidth = inputs.vramWidth};
  plan.presentationExtent = guestPresentationExtent(inputs.nativePresentation, target);
  plan.projectionExtent = guestPresentationExtent(inputs.nativeProjection.extent, target);
  // The record canvas adds the margins around the buffer, so the guest keeps its retail centre and width.
  plan.guestDrawWidth = inputs.nativeProjection.drawWidth;
  plan.presentationHorizontalMargin = (plan.presentationExtent.width - inputs.nativePresentation.width) / 2;
  plan.projectionHorizontalMargin = (plan.projectionExtent.width - inputs.nativeProjection.extent.width) / 2;
  plan.projectionCenterX = inputs.nativeProjection.extent.width / 2;
  plan.projectionClipRight = plan.projectionExtent.width - 1;
  // The guest's drawn window on the widened canvas, and it BRACKETS THE PRESENTATION: its right
  // edge is the last column the guest fills, its left edge is where that run starts. The two are
  // published together because a clip is only correct when both ends are named — the right end
  // alone was published for a long time and the left end was left to the producer, which is what
  // cut off one margin while widening the other.
  plan.guestClipLeft = plan.presentationExtent.width - plan.guestDrawWidth;
  plan.guestClipRight = plan.guestDrawWidth - 1;
  return plan;
}

} // namespace psx::present

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

namespace psx::present {

// Is the latched guest projection wider than 4:3, and its presented width.
bool widePresentation(const Core &core);
int widePresentationWidth(const Core &core);
GuestProjectionPlan latchGuestProjection(Core &core, GuestProjectionGeometry geometry);
// The guest left the canvas it widened: publish the plan for its current display mode at 4:3, so the
// presenter samples the authored columns only.
void unlatchGuestProjection(Core &core);
// RenderPath::Record: latches the plan for a guest display of `display` at the title's declared
// aspect. The guest projection is not moved; its margin is how far the record canvas extends the clip.
GuestProjectionPlan latchRecordDisplay(Core &core, psx::present::PixelSize display);

} // namespace psx::present
