// The ONE seam between Fps60's half-rate pacing and the way a title builds its in-between frame.
//
// The real field is always the game's own untouched output: Fps60 presents it exactly as captured,
// and the strategy is only ever asked about the OTHER half. How that half is made is the title's
// choice, and there are two real answers, each with its own named strategy:
//
//   HostWorldPassStrategy (host_world_pass_strategy.h) — the host re-runs the world pass from the
//     game's object memory at a lerped camera, and the in-between's geometry is the host's.
//   GuestGeometrySceneSource (guest_geometry_scene_source.h) — the captured guest primitives with
//     proven-provenance vertices interpolated; the in-between's geometry is still the guest's.
//
// So this interface describes the HOLE, not a preferred implementation: a title that adds a third
// kind of in-between implements it here and nothing in the framework moves.
#pragma once

#include "frame_presenter.h" // CapturedFrameView

class Core;
struct RqItem;

class InBetweenStrategy {
public:
  virtual ~InBetweenStrategy() = default;

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

  // WHY THIS STRATEGY'S IN-BETWEEN MAY PRESENT WITHOUT THE GUEST'S RENDERER BEING LIVE.
  //
  // A render path that keeps the guest's renderer live (Gte) presents the guest's own picture for the
  // real field. Whether an IN-BETWEEN may be presented there too is a property of the strategy, not of
  // the path, and the strategy is the only thing that can answer it: only it knows what it reads.
  //
  //   None — the default, and nothing is claimed. This strategy's in-between is a host-side product of
  //     the guest's renderer, so it needs RenderMode::enhancementsAllowed() and the Native path.
  //   GuestPrimitives — the in-between IS the guest's own primitives, with vertices interpolated
  //     between two real frames by proven provenance (GuestGeometrySceneSource). No host geometry and
  //     no re-run: the guest's picture at another instant.
  //   HostRebuiltFromGuestMemory — the in-between is the guest's own PICTURE rebuilt on the host out of
  //     a read-only reading of the guest's memory: no guest call, no guest write, and the guest's own
  //     records as the only input (a title's world pass re-running the guest's own scene producer over
  //     host memory). It is the guest's picture at another instant for the same reason
  //     GuestPrimitives is, so it is admitted on the same paths.
  //
  // The claim is about READING, and it is the strategy's to make and the framework's to hold it to. A
  // strategy that writes guest memory, calls guest code, or reads renderer state the guest only keeps
  // live cannot claim either, and does not.
  enum class GuestPathClaim : std::uint8_t {
    None,
    GuestPrimitives,
    HostRebuiltFromGuestMemory,
  };
  virtual GuestPathClaim guestPathClaim() const {
    return GuestPathClaim::None;
  }
};
