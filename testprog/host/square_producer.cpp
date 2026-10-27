#include "square_producer.h"

#include "core.h"
#include "facts.h"
#include "frame_state.h"
#include "gp0_primitive_decode.h"
#include "native_dispatch.h"
#include "projected_vertex.h"
#include "runtime.h"

#include <lucent/log.h>

#include <cmath>
#include <cstdlib>
#include <memory>

namespace psx::testprog {
namespace {

constexpr float kTurn = 64.0f;
// square_draw's corners, before the turn.
constexpr std::array<std::array<float, 2>, 4> kCorners{
    {{-24.0f, -24.0f}, {24.0f, -24.0f}, {-24.0f, 24.0f}, {24.0f, 24.0f}}};

void squareDrawOverride(Core *core) {
  const Facts &facts = session(*core).facts;
  const std::uint32_t square = core->r[psx::cpu::kA0];
  const std::uint32_t packet = core->mem_r32(facts.cursor);
  if (!psx::cpu::callOriginalOrPropagate(*core, facts.squareDraw)) {
    return;
  }
  SquareProducer::State state{.x = static_cast<std::int32_t>(core->mem_r32(square)),
                              .y = static_cast<std::int32_t>(core->mem_r32(square + 4u)),
                              .angle = core->mem_r32(square + 8u)};
  for (std::uint32_t word = 0; word < state.words.size(); ++word) {
    state.words[word] = core->mem_r32(packet + 4u * (word + 1u));
  }
  core->frameStates.save(core->emission.current(), state);
}

} // namespace

float SquareProducer::sine(float angle) const {
  float a = std::fmod(angle, kTurn);
  if (a < 0.0f) {
    a += kTurn;
  }
  const float quarter = kTurn / 4.0f;
  const float sign = a < 2.0f * quarter ? 1.0f : -1.0f;
  a = std::fmod(a, 2.0f * quarter);
  if (a > quarter) {
    a = 2.0f * quarter - a;
  }
  const auto low = static_cast<std::size_t>(a);
  const std::size_t high = std::min<std::size_t>(low + 1u, sineQuarter_.size() - 1u);
  const float fraction = a - static_cast<float>(low);
  return sign * (static_cast<float>(sineQuarter_[low]) * (1.0f - fraction) +
                 static_cast<float>(sineQuarter_[high]) * fraction);
}

void SquareProducer::render(const psx::present::StateInterval &states, psx::present::PrimitiveSink &sink) const {
  const State to = psx::present::stateAs<State>(states.to);
  psx::gpu::PacketBuffer<5> packet(std::span<const std::uint32_t, 5>(to.words));
  if (states.t < 1.0f) {
    const State from = psx::present::stateAs<State>(states.from);
    const float t = states.t;
    const float x = static_cast<float>(from.x) + static_cast<float>(to.x - from.x) * t;
    const float y = static_cast<float>(from.y) + static_cast<float>(to.y - from.y) * t;
    // The turn the short way round, in 1/64 turns.
    const auto turned = static_cast<std::int32_t>((to.angle - from.angle) & 63u);
    const float angle = static_cast<float>(from.angle) + static_cast<float>(turned >= 32 ? turned - 64 : turned) * t;
    const float s = sine(angle) / 4096.0f;
    const float c = sine(angle + kTurn / 4.0f) / 4096.0f;
    for (std::size_t corner = 0; corner < kCorners.size(); ++corner) {
      const auto [cx, cy] = kCorners[corner];
      packet.storeVertex(corner + 1u, psx::gte::ProjectedVertex::nearest({x + cx * c - cy * s, y + cx * s + cy * c}));
    }
  }
  const std::optional<psx::present::DrawPrimitive> primitive = psx::gpu::decodePacketPrimitive(packet.view());
  if (!primitive) {
    lucent::error("testprog", "the square packet 0x{:08X} does not decode", to.words[0]);
    std::abort();
  }
  sink.emit({kOrderingTable, kSquareBucket}, *primitive);
}

void SquareProducer::install(Core &core, const Facts &facts) {
  std::array<std::int16_t, 17> sineQuarter{};
  for (std::size_t i = 0; i < sineQuarter.size(); ++i) {
    sineQuarter[i] = static_cast<std::int16_t>(core.mem_r16(facts.sineQuarter + 2u * static_cast<std::uint32_t>(i)));
  }
  session(core).producers.install(facts.squareDraw, std::make_unique<SquareProducer>(sineQuarter));
  psx::cpu::installNativeOverride(core,
                                  facts.squareDraw,
                                  "testprog::squareDraw",
                                  squareDrawOverride,
                                  psx::present::Producer{psx::present::Arg::A0});
}

} // namespace psx::testprog
