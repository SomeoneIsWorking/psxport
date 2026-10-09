#include "dma_linked_list.h"

#include "core.h"

#include <lucent/log.h>

namespace psx::dma {

unsigned syncMode(std::uint32_t chcr) {
  return (chcr >> 9) & 3u;
}

int chainWords(Core &core, std::uint32_t madr, std::uint32_t *endMadr, bool *refused) {
  *refused = false;
  std::uint32_t node = madr & 0x1FFFFC;
  int total = 0;
  *endMadr = node;
  for (int hops = 0; hops < kChainWordCap; hops++) {
    const std::uint32_t header = core.mem_r32(node);
    const std::uint32_t count = header >> 24;
    const std::uint32_t next = header & 0x00FFFFFFu;
    if (count > static_cast<std::uint32_t>(kChainWordCap) || total + static_cast<int>(count) > kChainWordCap) {
      lucent::warn("dma",
                   "chain at 0x{:08X} declares {} words at node 0x{:08X} with {} already walked, over the "
                   "{}-word ceiling; REFUSING the whole transfer rather than completing a partial one",
                   0x80000000u | (madr & 0x1FFFFC),
                   count,
                   0x80000000u | node,
                   total,
                   kChainWordCap);
      *refused = true;
      *endMadr = madr & 0x1FFFFC;
      return 0;
    }
    total += static_cast<int>(count);
    // Where the last word LANDS, one past the final payload word — not this header. Hardware leaves MADR
    // there, and a guest that reads it back and continues from it would otherwise restart the final
    // node's payload from its beginning.
    const std::uint32_t landed = (node + 4u + count * 4u) & 0x1FFFFCu;
    if (next == 0x00FFFFFFu) {
      *endMadr = landed;
      return total;
    }
    if ((next & 0x1FFFFC) == node) {
      // A self-link is the cheapest cycle a guest can write into its own RAM. Reported, then treated as
      // the end, so the transfer that already counted still completes and the guest is not left polling
      // a busy channel it cannot clear.
      lucent::warn("dma",
                   "chain at 0x{:08X} links to itself at node 0x{:08X}; treating it as the end",
                   0x80000000u | (madr & 0x1FFFFC),
                   0x80000000u | node);
      *endMadr = landed;
      return total;
    }
    node = next & 0x1FFFFC;
  }
  return total;
}

} // namespace psx::dma
