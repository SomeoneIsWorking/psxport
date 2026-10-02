// A title owns temporal inputs and the exact primitive producers it can reconstruct.
#pragma once

#include "frame_presenter.h" // CapturedFrameView

class Core;
struct RqItem;

class TemporalSceneSource {
public:
  virtual ~TemporalSceneSource() = default;

  // Once per presented frame, before either present runs and before `eligible` is asked: the frame
  // the presenter is about to show, and whether an in-between will be requested for it at all. A
  // source that pairs captured frames resolves this one here; the default ignores it.
  virtual void beginPresentation(Core &core, CapturedFrameView frame, bool interpolating) {
    (void)core;
    (void)frame;
    (void)interpolating;
  }

  // Stable for both presents of one frame. False preserves the complete captured queue.
  virtual bool eligible(const Core &core) const = 0;
  virtual bool owns(const RqItem &item) const = 0;

  // Submit only owned producers through Core::game->rqRedirect. Fps60 supplies an isolated queue and a read-only
  // display scope; t=1 is the current endpoint, t=0 the previous endpoint.
  virtual void reconstruct(Core &core, float t) = 0;

  // Called once after the real presentation, including disabled/ineligible frames. The source
  // owns invalidating missing endpoints as well as advancing captured history.
  virtual void rotate(Core &core) = 0;

  // Capture-only producers need a current-endpoint draw even when interpolation is disabled.
  // Sources whose captured queue already contains their geometry keep the default.
  virtual bool requiresEndpointReconstruction() const {
    return false;
  }

  // Is the REAL pass's captured queue this source's finished geometry? False — the default, and the
  // answer for a source whose captured items are capture-only inputs: those need the reconstruction at
  // t=1 too, or the current endpoint would never be drawn. True for a source whose in-between is made
  // from the captured frame itself: replacing that frame with a copy of itself at t=1 could only make
  // the real frame worse. It decides the REAL PASS ONLY; the in-between slot always reconstructs.
  virtual bool capturedQueueIsComplete() const {
    return false;
  }

  // ARE THIS SOURCE'S IN-BETWEENS MADE OF THE GUEST'S OWN PRIMITIVES?
  //
  // True for a source whose in-between field is the guest's captured primitives with their vertices
  // interpolated between two real frames by proven provenance (guest_geometry_scene_source.h): no host
  // geometry, no re-run, no guest write. That is the guest's picture at another instant rather than a
  // PC enhancement of it, so it is permitted on the Gte path (RenderCapabilities::guestInterpolated,
  // Fps60::interpolationPermitted).
  //
  // False — the default, and the answer for every source that reconstructs on the HOST: the legacy
  // camera/object lerp, any native producer's geometry. Those remain Native-only, which is what
  // RenderMode::enhancementsAllowed() expresses and what the user chose.
  virtual bool interpolatesGuestGeometry() const {
    return false;
  }
};
