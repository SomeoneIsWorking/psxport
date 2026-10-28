// record_fault.h — the render path's refusal of guest data that sizes GPU work a console never finishes, and the
// cycle check every linked draw-list walk uses.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class Core;

namespace psx::gpu {

// What the guest data asked for. A console hangs on each, so a hit is always a defect upstream.
enum class RecordFaultKind : std::uint8_t {
  DrawListCycle,        // a linked draw list revisits a node
  PolyLineUnterminated, // a packet ends inside a poly-line
  RecordEntries,        // one frame record past kMaxEntries
  RecordPool,           // one frame record's CLUT or upload pixels past kMaxPoolHalfwords
};

const char *recordFaultName(RecordFaultKind kind);

// Ends the process before the record holding the work is sealed, so none of it reaches the GPU. `owner` is the
// refusing layer as file:function.
[[noreturn]] void
raiseRecordFault(RecordFaultKind kind, std::uint32_t guestAddress, std::string_view owner, std::string_view detail);

// The guest code that last stored the word at `address`, as a fault's detail names it.
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
