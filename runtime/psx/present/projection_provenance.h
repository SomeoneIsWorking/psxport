// projection_provenance.h — which guest projection produced each screen vertex of a frame.
//
// A guest primitive reaches the GPU as screen coordinates only: the camera, the object transform and the
// model vertex are all baked in by the time the packet is written. To interpolate the guest's OWN
// geometry between two real frames, each drawn vertex has to be paired with the same vertex in the
// previous frame, and that pairing must come from where the vertex was made, never from what it looks
// like on screen. This owner records exactly that, at the one place every projected vertex passes
// through: the GTE's RTPS/RTPT.
//
// A projected vertex's identity is (scope, epoch, model-space input):
//   * scope  — a title-proven producer instance (a mesh instance, a character, a room section), opened by
//              the title around the guest call that submits it. Only scoped projections are recorded:
//              the framework cannot know which guest object a projection belongs to, and a guess would
//              pair two different objects.
//   * epoch  — the ordinal of the distinct transform (rotation, translation, projection) the scope has
//              projected with so far this frame. A segmented character projects each segment under its
//              own matrix; the epoch keeps segment-local coordinates of different segments apart.
//   * input  — the V0..V2 operand: the model-space vertex the guest loaded. Static for a rigid mesh, and
//              the same model vertex yields the same screen point under one transform, so duplicate
//              loads of one vertex can never disagree.
//
// Host-only value state: it reads GTE registers after the guest's own instruction and writes nothing back.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace psxport::temporal {

// The GTE state one RTPS/RTPT consumed and produced, as the GTE boundary hands it over.
struct ProjectionSample {
  // V0..V2 x/y/z. RTPS consumes only V0.
  std::array<int16_t, 9> inputs{};
  // SXY words in output order (RTPT: v0, v1, v2; RTPS: its single vertex).
  std::array<uint32_t, 3> screen{};
  uint8_t count = 0; // 1 for RTPS, 3 for RTPT
  // CR0..CR7 (rotation + translation) and CR24..CR26 (OFX, OFY, H): the whole transform.
  std::array<uint32_t, 11> transform{};
};

struct VertexIdentity {
  uint64_t scope = 0;
  uint32_t epoch = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;

  bool operator==(const VertexIdentity &) const = default;
};

struct VertexIdentityHash {
  size_t operator()(const VertexIdentity &identity) const;
};

struct ProjectedVertex {
  VertexIdentity identity;
  int16_t screenX = 0;
  int16_t screenY = 0;
  // Which GTE instruction of the segment produced it. One RTPT projects a face's three corners together
  // (with an adjacent RTPS for a quad's fourth), which is what tells a face's own vertices apart from
  // another face's vertices that happen to project onto the same pixels.
  uint32_t instruction = 0;
};

class ProjectionProvenance {
public:
  // A segment's worth of projected vertices. Past it the frame is marked incomplete rather than silently
  // truncated, and a consumer must refuse to pair it.
  static constexpr size_t kMaxVerticesPerSegment = 1u << 17;

  // The title's proof that the projections inside belong to one producer instance. Nests: an inner scope
  // owns its projections and the outer one resumes on exit. A key of 0 is "no scope" and records nothing.
  class Scope {
  public:
    Scope(ProjectionProvenance &owner, uint64_t key);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    ProjectionProvenance &owner_;
    uint64_t previous_;
  };

  void setArmed(bool armed);
  bool armed() const {
    return armed_;
  }
  // Recording is live only while armed AND inside a scope; the GTE boundary asks before sampling.
  bool recording() const {
    return armed_ && scope_ != 0;
  }

  void record(const ProjectionSample &sample);

  // The guest is handing an ordering table to the GPU. Every projection recorded since the previous
  // hand-off built the packets that table links, so they join the frame being drawn, and the next
  // projections start a fresh segment with fresh epochs. Cutting here rather than at present keeps a
  // guest that builds one table while the GPU draws the other paired with the table actually drawn.
  void sealForDraw();

  // The projections behind the tables handed to the GPU since the last clearFrame().
  std::span<const ProjectedVertex> frame() const {
    return drawn_;
  }
  bool complete() const {
    return !drawnOverflowed_;
  }
  // How many tables the current frame's projections were sealed for.
  uint32_t seals() const {
    return seals_;
  }
  // The drawn frame has been presented: the next presented frame starts empty. Projections still
  // being recorded for a table not yet handed over are kept.
  void clearFrame();

private:
  using Transform = std::array<uint32_t, 11>;
  struct ScopeState {
    uint64_t scope = 0;
    std::vector<Transform> transforms; // distinct, in first-seen order: the epoch is an index + 1
  };
  ScopeState &stateFor(uint64_t scope);
  static uint32_t epochOf(ScopeState &state, const Transform &transform);

  void clearRecording();

  bool armed_ = false;
  bool recordingOverflowed_ = false;
  bool drawnOverflowed_ = false;
  uint64_t scope_ = 0;
  uint32_t instruction_ = 0;
  uint32_t seals_ = 0;
  std::vector<ProjectedVertex> recording_;
  std::vector<ProjectedVertex> drawn_;
  std::vector<ScopeState> scopes_; // few per frame; linear scan beats hashing at this size
};

} // namespace psxport::temporal
