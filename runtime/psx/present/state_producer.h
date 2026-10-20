// state_producer.h — a producer's native render from saved state, and the lookup of them.
#pragma once

#include "frame_record.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>

namespace psx::present {

// Where a render puts its primitives. Positions are draw-offset relative, as the guest packet holds
// them. The render sets the texture page, texture mode and blend mode; draw area, offset, mask,
// texture window and interlace come from the object's own entries, and the CLUT from `clutWord`.
class PrimitiveSink {
public:
  virtual ~PrimitiveSink() = default;
  virtual void emit(OtSlot slot, const DrawPrimitive &primitive) = 0;
};

// An object's two saved states and the point `t` between them. With no earlier state (birth, cut),
// `from` is `to`.
struct StateInterval {
  std::span<const std::byte> from;
  std::span<const std::byte> to;
  float t = 1.0f;
};

// The object at `t` between two of its saved states. Never writes guest memory, and reads it only for
// data that does not change while the object lives (level geometry). At `t = 1` it emits exactly the
// primitives its guest packets drew.
class StateProducer {
public:
  virtual ~StateProducer() = default;
  virtual void render(const StateInterval &states, PrimitiveSink &sink) const = 0;
};

// The renders by producer guest address. A producer without one keeps its entries as recorded.
class StateProducers {
public:
  virtual ~StateProducers() = default;
  virtual const StateProducer *find(std::uint32_t producer) const = 0;
};

// A title's renders, held in its session state and answered from GameRuntime::stateProducer.
class StateProducerTable final : public StateProducers {
public:
  // Aborts on a second render for one producer.
  void install(std::uint32_t producer, std::unique_ptr<const StateProducer> render);
  const StateProducer *find(std::uint32_t producer) const override;

private:
  std::unordered_map<std::uint32_t, std::unique_ptr<const StateProducer>> renders_;
};

} // namespace psx::present
