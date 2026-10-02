// guest_geometry_interpolation.h — an in-between field made of the guest's own primitives.
//
// The real frame is the guest's captured queue, presented verbatim. The in-between is the SAME captured
// queue with every vertex whose provenance is proven moved part of the way back toward where that same
// vertex was in the previous real frame. Nothing is re-run, nothing is written to guest memory, nothing
// is sampled from a picture: a primitive's vertices are paired to the projections that produced them
// (ProjectionProvenance), and a vertex is interpolated only when the previous frame projected the same
// (scope, epoch, model-space vertex).
//
// Resolution of a drawn vertex is exact or refused. Its packet words ARE the SXY the GTE produced, so the
// producing projection is among the frame's projections with that screen point. Other vertices can share
// that point: an edge seen end-on, a face the guest projected and then culled, the same world geometry
// drawn by a second producer. So a primitive is read first from the instructions that project a face's
// corners together (one RTPT, with an adjacent RTPS for a quad), and only when no such group fits, under
// its transform alone (a producer that projects a vertex pool and indexes it afterwards). Either way every
// corner must find a projection in ONE scope and epoch. Readings that name different vertices are
// accepted only when those vertices were also on one pixel in the previous frame, so the in-between is
// the same whichever produced the packet; otherwise the pairing cannot see through the coincidence and
// refuses the primitive instead of choosing.
#pragma once

#include "projection_provenance.h"
#include "render_queue.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace psxport::temporal {

class GuestGeometryInterpolation {
public:
  struct Census {
    uint32_t items = 0;
    uint32_t polygons = 0;  // guest polygons carrying their packet's vertex words
    uint32_t resolved = 0;  // every vertex paired to its projection
    uint32_t unmatched = 0; // some vertex has no projection in a common scope/epoch (unscoped producer)
    uint32_t ambiguous = 0; // vertex sets that moved differently fit: refused
    uint32_t mapped = 0;    // host coordinates are not the guest's plus one offset: refused
    uint32_t interpolatedVertices = 0;
    uint32_t heldVertices = 0; // resolved, but the previous frame never projected that vertex
  };

  // Resolve one captured frame against the projections that produced it. `items` must stay valid until
  // rotate(); FramePresenter keeps it for the whole presentation of the frame.
  void beginFrame(std::span<const RqItem> items, std::span<const ProjectedVertex> projections);

  bool owns(const RqItem &item) const;
  bool hasPrevious() const {
    return havePrevious_;
  }

  // Push every owned item of the current frame into `sink`, each resolved vertex moved to the point
  // between its previous and current screen position at `t` (t = 1 is the current frame).
  void emit(float t, RenderQueue &sink);

  // The current frame's projections become the previous endpoint.
  void rotate();
  // Forget the previous endpoint (a discontinuity: level load, camera cut, incomplete log), so the
  // current frame pairs with nothing.
  void clear();

  const Census &census() const {
    return census_;
  }

private:
  struct ScreenPoint {
    int16_t x = 0;
    int16_t y = 0;
  };
  using ScreenMap = std::unordered_map<VertexIdentity, ScreenPoint, VertexIdentityHash>;

  // Per owned item: the identity of each vertex.
  struct Resolution {
    size_t item = 0;
    std::array<VertexIdentity, 4> vertices{};
  };

  void resolve(size_t index, const RqItem &item, std::span<const ProjectedVertex> projections);

  std::span<const RqItem> items_;
  std::vector<Resolution> resolutions_;
  std::vector<uint8_t> owned_; // by item index
  // Projections sorted by packed screen point: the lookup from a packet's vertex word to its producers.
  std::vector<std::pair<uint32_t, uint32_t>> byScreen_;
  ScreenMap current_;
  ScreenMap previous_;
  bool havePrevious_ = false;
  Census census_;
};

} // namespace psxport::temporal
