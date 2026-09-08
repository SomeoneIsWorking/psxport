// A title owns temporal inputs and the exact primitive producers it can reconstruct.
#pragma once

class Core;
struct RqItem;

class TemporalSceneSource {
public:
  virtual ~TemporalSceneSource() = default;

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
};
