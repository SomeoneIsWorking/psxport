// host_ordering_table.h — the packets a body linked into an ordering table held in host memory, as record
// primitives in the buckets it linked them into.
#pragma once

#include "emit_memory.h"
#include "state_producer.h"

#include <cstdint>

namespace psx::present {

// `buckets` four-byte bucket heads from `heads`, bucket `i` being slot `first.index + i` of table `first.table`.
// A bucket is walked from its head along each packet's tag link; the table from its last bucket to its first.
struct HostOrderingTable {
  std::uint32_t heads = 0;
  std::uint32_t buckets = 0;
  OtSlot first;
  // The most packets the body could have linked; a longer chain is a cycle.
  std::uint32_t packetLimit = 0;
};

// Aborts on a packet that is not a polygon or a sprite. A sprite or line takes the texture page of the last
// draw mode packet before it in its chain.
void emitHostOrderingTable(const HostMemory &host, const HostOrderingTable &table, PrimitiveSink &sink);

} // namespace psx::present
