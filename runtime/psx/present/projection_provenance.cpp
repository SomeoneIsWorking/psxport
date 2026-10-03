#include "projection_provenance.h"

#include <algorithm>

namespace psxport::temporal {

size_t VertexIdentityHash::operator()(const VertexIdentity &identity) const {
  uint64_t h = identity.scope * 0x9E3779B97F4A7C15ull;
  h ^= (static_cast<uint64_t>(identity.epoch) << 48) ^
       (static_cast<uint64_t>(static_cast<uint16_t>(identity.x)) << 32) ^
       (static_cast<uint64_t>(static_cast<uint16_t>(identity.y)) << 16) ^ static_cast<uint16_t>(identity.z);
  h *= 0xBF58476D1CE4E5B9ull;
  return static_cast<size_t>(h ^ (h >> 31));
}

ProjectionProvenance::Scope::Scope(ProjectionProvenance &owner, uint64_t key) : owner_(owner), previous_(owner.scope_) {
  owner_.scope_ = key;
}

ProjectionProvenance::Scope::~Scope() {
  owner_.scope_ = previous_;
}

void ProjectionProvenance::setArmed(bool armed) {
  if (armed_ && !armed) {
    clearRecording();
    clearFrame();
  }
  armed_ = armed;
}

ProjectionProvenance::ScopeState &ProjectionProvenance::stateFor(uint64_t scope) {
  for (ScopeState &state : scopes_) {
    if (state.scope == scope) {
      return state;
    }
  }
  scopes_.push_back({.scope = scope, .transforms = {}});
  return scopes_.back();
}

uint32_t ProjectionProvenance::epochOf(ScopeState &state, const Transform &transform) {
  const auto seen = std::find(state.transforms.begin(), state.transforms.end(), transform);
  if (seen != state.transforms.end()) {
    return static_cast<uint32_t>(seen - state.transforms.begin()) + 1u;
  }
  state.transforms.push_back(transform);
  return static_cast<uint32_t>(state.transforms.size());
}

void ProjectionProvenance::record(const ProjectionSample &sample) {
  if (!recording() || sample.count == 0) {
    return;
  }
  if (recording_.size() + sample.count > kMaxVerticesPerSegment) {
    recordingOverflowed_ = true;
    return;
  }
  const uint32_t epoch = epochOf(stateFor(scope_), sample.transform);
  const uint32_t instruction = instruction_++;
  for (uint8_t vertex = 0; vertex < sample.count; ++vertex) {
    const uint32_t xy = sample.screen[vertex];
    recording_.push_back({
        .identity = {.scope = scope_,
                     .epoch = epoch,
                     .x = sample.inputs[vertex * 3u],
                     .y = sample.inputs[vertex * 3u + 1u],
                     .z = sample.inputs[vertex * 3u + 2u]},
        .screenX = static_cast<int16_t>(xy & 0xFFFFu),
        .screenY = static_cast<int16_t>(xy >> 16),
        .instruction = instruction,
    });
  }
}

void ProjectionProvenance::sealForDraw() {
  if (!armed_) {
    return;
  }
  drawn_.insert(drawn_.end(), recording_.begin(), recording_.end());
  drawnOverflowed_ = drawnOverflowed_ || recordingOverflowed_;
  ++seals_;
  clearRecording();
}

void ProjectionProvenance::clearFrame() {
  drawn_.clear();
  drawnOverflowed_ = false;
  seals_ = 0;
}

void ProjectionProvenance::clearRecording() {
  recording_.clear();
  scopes_.clear();
  recordingOverflowed_ = false;
}

} // namespace psxport::temporal
