// guest_writer_latch.h — the guest code storing now, for the owner ledger: the block-entry PC the executor last
// entered and the function a shadow call stack holds (writer attribution, call target, return address).
#pragma once

#include "guest_call_site.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace psx::cpu {

class GuestWriterLatch {
public:
  // A block entry at `pc` with `$ra` = `ra` and the GPR file `gpr`. `readCallWord()` returns the instruction at
  // ra - 8; it is called only when that word decides the entry: `ra` changed since it was last decoded and the entry
  // is neither a return nor at `ra` itself.
  template <typename ReadCallWord>
  void enterBlock(std::uint32_t pc, std::uint32_t ra, const std::uint32_t *gpr, ReadCallWord &&readCallWord) {
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
      callWord_ = readCallWord();
    }
    if (!callsTo(GuestCallSite{pc, ra}, gpr)) {
      return;
    }
    if (depth_ != 0 && frames_[top()].entry == pc && frames_[top()].returnPc == ra) {
      return; // a loop back to the entry block
    }
    ++depth_;
    frames_[top()] = {pc, ra};
  }

  std::uint32_t blockPc() const {
    return blockPc_;
  }
  // The innermost call target the stack holds; 0 when the stack is empty.
  std::uint32_t function() const {
    return depth_ != 0 ? frames_[top()].entry : 0u;
  }
  // The open calls' return addresses, innermost first, into `out`; how many it wrote.
  std::size_t returns(std::span<std::uint32_t> out) const {
    const std::size_t count = std::min({depth_, kDepth, out.size()});
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = frames_[(top() + kDepth - i) % kDepth].returnPc;
    }
    return count;
  }
  // A guest function the host enters at `entry`, returning to `returnPc` (0 for a run to its exit) without a
  // `jal`: the stack holds it from here until the scope ends, however the call exits.
  class HostCall {
  public:
    HostCall(GuestWriterLatch &latch, std::uint32_t entry, std::uint32_t returnPc)
        : latch_(latch), depth_(latch.depth_) {
      ++latch_.depth_;
      latch_.frames_[latch_.top()] = {entry, returnPc};
    }
    ~HostCall() {
      latch_.depth_ = depth_;
    }
    HostCall(const HostCall &) = delete;
    HostCall &operator=(const HostCall &) = delete;
    HostCall(HostCall &&) = delete;
    HostCall &operator=(HostCall &&) = delete;

  private:
    GuestWriterLatch &latch_;
    std::size_t depth_;
  };
  [[nodiscard]] HostCall hostCall(std::uint32_t entry, std::uint32_t returnPc) {
    return {*this, entry, returnPc};
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
  // The word at the call's return address - 8 is a `jal` to its entry, or a `jalr` whose register still holds it.
  bool callsTo(GuestCallSite call, const std::uint32_t *gpr) const {
    const std::uint32_t opcode = callWord_ >> 26;
    if (opcode == 3u) {
      const std::uint32_t site = call.returnPc - kJalReturnOffset;
      const std::uint32_t target = (site & 0xF0000000u) | ((callWord_ & 0x03FFFFFFu) << 2);
      return (target & 0x1FFFFFFFu) == (call.entry & 0x1FFFFFFFu);
    }
    return opcode == 0u && (callWord_ & 0x3Fu) == 9u && gpr[(callWord_ >> 21) & 0x1Fu] == call.entry;
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
