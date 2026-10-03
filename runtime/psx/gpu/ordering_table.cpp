#include "ordering_table.h"

#include "core.h"

namespace psx::gpu {

namespace {
// Read one node's header word out of guest main RAM. The single RAM touch of this owner: every other
// function here works on a value that came through here, so there is one place a masking or alignment
// change could be wrong.
OtNode readNode(Core &core, std::uint32_t mainRamOffset) {
  return OtNode(mainRamOffset, core.mem_r32(mainRamOffset));
}
} // namespace

OrderingTableCursor::OrderingTableCursor(Core &core, std::uint32_t rootGuestAddress)
    : core_(core), node_(readNode(core, mainRamOffsetOf(rootGuestAddress))) {
  nodesEntered_ = 1;
}

bool OrderingTableCursor::advance() {
  if (node_.endsChain()) {
    return false;
  }
  // The CAP is a refusal, not a length. A well-formed table never reaches it — a DrawOTag is bounded by
  // the packet pool — so hitting it means the chain is cyclic or malformed, and the honest thing is to
  // stop and record that. Recorded separately from the end-of-chain case because the two mean opposite
  // things, and a caller that cannot tell them apart reads a hung frame as an empty table.
  if (nodesEntered_ >= kOtNodeLimit) {
    truncated_ = true;
    return false;
  }
  node_ = readNode(core_, node_.nextRamOffset());
  nodesEntered_++;
  return true;
}

} // namespace psx::gpu
