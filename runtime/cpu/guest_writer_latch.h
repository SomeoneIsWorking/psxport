// guest_writer_latch.h — the guest code storing now, for the owner ledger: the block-entry PC the executor last
// entered and the function a shadow call stack holds (writer attribution, call target, return address).
#pragma once

#include "guest_call_site.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::cpu {

class GuestWriterLatch {
public:
  // A block entry at `pc` with `$ra` = `ra` and the GPR file `gpr`; `callWord` is the instruction at ra - 8,
  // read only when `ra` changed since the last entry (`needsCallWord`).
  void enterBlock(std::uint32_t pc, std::uint32_t ra, const std::uint32_t *gpr, std::uint32_t callWord) {
    blockPc_ = pc;
    if (depth_ != 0 && pc == frames_[top()].returnPc) {
      --depth_;
      return;
    }
    if (pc == ra) {
      // A return the stack never saw the call of: pop through the frame that returns here, if one does.
      for (std::size_t i = 1; i < depth_ && i < kDepth; ++i) {
        if (frames_[(top() + kDepth - i) % kDepth].returnPc == pc) {
          depth_ -= i + 1;
          return;
        }
      }
      return;
    }
    if (ra != decodedRa_) {
      decodedRa_ = ra;
      callWord_ = callWord;
    }
    if (!callsTo(pc, ra, gpr)) {
      return;
    }
    if (depth_ != 0 && frames_[top()].entry == pc && frames_[top()].returnPc == ra) {
      return; // a loop back to the entry block
    }
    ++depth_;
    frames_[top()] = {pc, ra};
  }

  bool needsCallWord(std::uint32_t ra) const {
    return ra != decodedRa_;
  }
  std::uint32_t blockPc() const {
    return blockPc_;
  }
  // The innermost call target the stack holds; 0 when the stack is empty.
  std::uint32_t function() const {
    return depth_ != 0 ? frames_[top()].entry : 0u;
  }
  // A machine state was replaced: no call is open.
  void clear() {
    depth_ = 0;
    blockPc_ = 0;
    decodedRa_ = 1u;
  }

private:
  static constexpr std::size_t kDepth = 64;

  std::size_t top() const {
    return (depth_ - 1u) % kDepth;
  }
  // The word at ra - 8 is a `jal pc`, or a `jalr` whose register still holds pc.
  bool callsTo(std::uint32_t pc, std::uint32_t ra, const std::uint32_t *gpr) const {
    const std::uint32_t opcode = callWord_ >> 26;
    if (opcode == 3u) {
      const std::uint32_t site = ra - kJalReturnOffset;
      const std::uint32_t target = (site & 0xF0000000u) | ((callWord_ & 0x03FFFFFFu) << 2);
      return (target & 0x1FFFFFFFu) == (pc & 0x1FFFFFFFu);
    }
    return opcode == 0u && (callWord_ & 0x3Fu) == 9u && gpr[(callWord_ >> 21) & 0x1Fu] == pc;
  }

  struct Frame {
    std::uint32_t entry = 0;
    std::uint32_t returnPc = 0;
  };
  std::array<Frame, kDepth> frames_{};
  std::size_t depth_ = 0;
  std::uint32_t blockPc_ = 0;
  std::uint32_t decodedRa_ = 1u; // never a real $ra, so the first entry decodes
  std::uint32_t callWord_ = 0;
};

} // namespace psx::cpu
