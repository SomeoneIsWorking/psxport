// marker_render.h — a state producer for tests: one triangle at a saved x, in a saved bucket.
#pragma once

#include "frame_record.h"
#include "frame_state.h"
#include "state_producer.h"

#include <memory>

namespace marker {

struct Marker {
  float x = 0.0f;
  std::uint32_t bucket = 0;
  std::uint16_t clutWord = 0;
};

class MarkerRender final : public psx::present::StateProducer {
public:
  void render(std::span<const std::byte> from,
              std::span<const std::byte> to,
              float t,
              psx::present::PrimitiveSink &sink) const override {
    const auto a = psx::present::stateAs<Marker>(from);
    const auto b = psx::present::stateAs<Marker>(to);
    psx::present::DrawPrimitive primitive;
    primitive.vertexCount = 3;
    primitive.textured = b.clutWord != 0;
    primitive.clutWord = b.clutWord;
    const float x = a.x + (b.x - a.x) * t;
    psx::present::placeVertex(primitive.vertices[0], x, 0.0f, primitive.kind);
    psx::present::placeVertex(primitive.vertices[1], x + 8.0f, 0.0f, primitive.kind);
    psx::present::placeVertex(primitive.vertices[2], x, 8.0f, primitive.kind);
    sink.emit(psx::present::OtSlot{0, b.bucket}, primitive);
  }
};

inline psx::present::StateProducerTable renders(std::uint32_t producer) {
  psx::present::StateProducerTable installed;
  installed.install(producer, std::make_unique<MarkerRender>());
  return installed;
}

// A record holding one primitive of `key` in bucket 1 of table 0, at `x`.
inline psx::present::FrameRecord keyedRecord(std::uint64_t sequence, const psx::present::RecordKey &key, int x) {
  psx::present::DrawPrimitive primitive;
  primitive.vertexCount = 3;
  primitive.vertices[0].x = x;
  primitive.vertices[1].x = x + 4;
  primitive.vertices[2].x = x;
  primitive.key = key;
  primitive.slot = psx::present::OtSlot{0, 1};
  psx::present::FrameRecord record(sequence, true);
  record.beginSlot({0, 1}, true);
  record.append(primitive);
  return record;
}

// The Marker `key` saved at `x` for `record`.
inline psx::present::FrameState
savedState(const psx::present::FrameRecord &record, const psx::present::RecordKey &key, float x) {
  psx::present::FrameStates states;
  states.save(key, Marker{x, 1, 0});
  return states.collect(record);
}

} // namespace marker
