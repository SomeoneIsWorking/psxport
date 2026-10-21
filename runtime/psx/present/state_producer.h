// state_producer.h — a producer's native render from saved state, and the lookup of them.
#pragma once

#include "frame_record.h"
#include "gp0_command.h"
#include "gte_state.h"
#include "guest_memory_map.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psx::present {

// The place a packet vertex word names exactly, as the GPU reads it: a corner no projection made.
[[nodiscard]] inline GtePlace wholePlace(std::uint32_t word) {
  const gpu::Gp0VertexPos position = gpu::Gp0Command(word).vertexPos();
  return {static_cast<float>(position.x), static_cast<float>(position.y)};
}

// `place` carried by the whole-pixel step from SXY word `from` to `to`: a corner the guest offset from a
// projected point.
[[nodiscard]] inline GtePlace placeOffset(GtePlace place, std::uint32_t from, std::uint32_t to) {
  const GtePlace start = wholePlace(from);
  const GtePlace end = wholePlace(to);
  return {place.x + end.x - start.x, place.y + end.y - start.y};
}

// Where a render puts its primitives. Positions are draw-offset relative, as the guest packet holds
// them. The render sets the texture page, texture mode and blend mode; draw area, offset, mask,
// texture window and interlace come from the object's own entries, and the CLUT from `clutWord`.
class PrimitiveSink {
public:
  virtual ~PrimitiveSink() = default;
  virtual void emit(OtSlot slot, const DrawPrimitive &primitive) = 0;
  // The producer's unrounded screen place of polygon or line vertex `vertex` in the packet it wrote at `packet`,
  // declared before the packet is emitted.
  virtual void place(std::uint32_t /*packet*/, int /*vertex*/, const GtePlace & /*place*/) {}
  // The places of every vertex of the packet at `packet` (any segment), in packet order.
  void placeCorners(std::uint32_t packet, std::span<const GtePlace> corners) {
    for (std::size_t vertex = 0; vertex < corners.size(); ++vertex) {
      place(psx::kseg0Address(packet), static_cast<int>(vertex), corners[vertex]);
    }
  }
  // The place declared for that vertex, for a producer that cuts a packet it already wrote.
  [[nodiscard]] virtual std::optional<GtePlace> placed(std::uint32_t /*packet*/, int /*vertex*/) {
    return std::nullopt;
  }
};

// A sink that keeps what it is handed, in order; a polygon or line from a packet with declared places takes
// their offsets from its whole pixels. `clear` keeps the storage for the next render.
class PrimitiveList final : public PrimitiveSink {
public:
  void emit(OtSlot slot, const DrawPrimitive &primitive) override;
  void place(std::uint32_t packet, int vertex, const GtePlace &place) override;
  [[nodiscard]] std::optional<GtePlace> placed(std::uint32_t packet, int vertex) override;
  void clear();
  std::vector<std::pair<OtSlot, DrawPrimitive>> primitives;

private:
  struct VertexPlace {
    std::uint64_t key = 0; // packet << 2 | vertex
    GtePlace place{};
  };
  std::vector<VertexPlace> places_;
  bool placesSorted_ = true;
};

// Where between two shown records the in-between present of fps60 is drawn (FramePresenter).
inline constexpr float kInBetweenAt = 0.5f;

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
  // What the object's entries become in the frame at `states`.
  enum class Composition : std::uint8_t {
    Render,   // replaced by `render`
    Recorded, // kept as the guest drew them, which is the object at `t`
    Refused,  // the object cannot be drawn at `t`, so the frame is shown as S whole
  };

  virtual ~StateProducer() = default;
  virtual void render(const StateInterval &states, PrimitiveSink &sink) const = 0;
  [[nodiscard]] virtual Composition composition(const StateInterval & /*states*/) const {
    return Composition::Render;
  }
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
