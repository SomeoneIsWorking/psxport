// draw_list_cycle.h — a linked draw list that revisits a node, which a console's walk never leaves: refused before
// the GPU sees it, and the cycle check every linked draw-list walk uses.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class Core;

namespace psx::gpu {

// Latches the presentation device off with the cycle as its fault, so nothing more reaches the GPU and the session
// ends at its next step with the player told. `node` is the revisited node, `refuser` the walk as file:function. The
// caller ends its walk.
void refuseDrawListCycle(Core &core, std::uint32_t node, std::string_view refuser, std::string_view detail);

// The guest code that last stored the word at `address`, as a refusal's detail names it.
std::string guestWriterOf(const Core &core, std::uint32_t address);

// Brent's check over one linked walk: a revisited node is reported within two laps of the cycle, with no memory.
class LinkCycleCheck {
public:
  // False when `node` closes a cycle.
  bool enter(std::uint32_t node) {
    if (node == anchor_) {
      return false;
    }
    if (++steps_ == lap_) {
      anchor_ = node;
      lap_ <<= 1u;
      steps_ = 0;
    }
    return true;
  }

private:
  // Never a node: nodes are word-aligned.
  static constexpr std::uint32_t kNoAnchor = 0xFFFFFFFFu;
  std::uint32_t anchor_ = kNoAnchor;
  std::uint64_t lap_ = 1;
  std::uint64_t steps_ = 0;
};

} // namespace psx::gpu
