// guest_widescreen_projection.h — title-owned projection matched to a host presentation extent.
#pragma once

#include "pixel_size.h"
#include "render_mode.h"
#include "video_plan.h"

#include <cstdint>

class Core;
struct Mods;

namespace psx::present {

// Values equal Mods::aspect's ASPECT_*, which the `wide` log prints.
enum class PresentationAspect : std::uint8_t {
  Standard4x3 = 0,
  Wide16x9 = 1,
  UltraWide21x9 = 2,
  MatchSink = 3,
};

// The player's aspect setting.
PresentationAspect presentationAspectFor(const Mods &mods);

// A title implements this only when its own guest code owns projection, culling and layout changes.
// Returning a wide aspect does not patch guest state and does not stretch the picture: the title must
// explicitly latch the matching plan when it publishes its guest projection.
class GuestWidescreenProjection {
public:
  virtual ~GuestWidescreenProjection() = default;
  // The player's aspect setting; a title overrides this only to hold 4:3 in some state.
  virtual PresentationAspect presentationAspect(const Core &core) const;
};

struct GuestProjectionGeometry {
  PixelSize extent;
  int drawWidth = 0;

  bool valid() const {
    return extent.width > 0 && extent.height > 0 && drawWidth > 0;
  }
};

struct GuestProjectionPlan {
  PresentationAspect aspect = PresentationAspect::Standard4x3;
  PixelSize nativeExtent;
  PixelSize presentationExtent;
  PixelSize nativeProjectionExtent;
  PixelSize projectionExtent;
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

// The canvas a 4:3 extent is widened onto: the sink's shape for MatchSink, capped at the VRAM width.
struct WideningTarget {
  PixelSize sink;
  PresentationAspect aspect = PresentationAspect::Standard4x3;
  int vramWidth = 0;
};

// A title-authored 4:3 width widened to the smallest even width that reaches the aspect: 320->428 at 16:9.
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

// The record path alone widens; Device stays the 4:3 reference picture.
struct GuestProjectionInputs {
  RenderPath path = RenderPath::Device;
  PresentationAspect requested = PresentationAspect::Standard4x3;
  PixelSize nativePresentation;
  GuestProjectionGeometry nativeProjection;
  PixelSize sink;
  int vramWidth = 0;
};

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
  // The guest's drawn window on the widened canvas: the run of columns it fills, both ends named.
  plan.guestClipLeft = plan.presentationExtent.width - plan.guestDrawWidth;
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

// Is the latched guest projection wider than 4:3, and its presented width.
bool widePresentation(const Core &core);
int widePresentationWidth(const Core &core);
GuestProjectionPlan latchGuestProjection(Core &core, GuestProjectionGeometry geometry);
// The guest's display size, 320x240 before it programmed one.
PixelSize nativeDisplaySize(Core &core);
// RenderPath::Record: latches the plan for a guest display of `display` at the title's declared
// aspect. The guest projection is not moved; its margin is how far the record canvas extends the clip.
GuestProjectionPlan latchRecordDisplay(Core &core, PixelSize display);

} // namespace psx::present
