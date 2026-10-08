// marker_render.h — a state producer for tests: one triangle at a saved x, in a saved bucket.
#pragma once

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

inline psx::present::StateProducers renders(std::uint32_t producer) {
  psx::present::StateProducers installed;
  installed.install(producer, std::make_unique<MarkerRender>());
  return installed;
}

} // namespace marker
