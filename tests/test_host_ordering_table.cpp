// test_host_ordering_table.cpp — packets linked into a host ordering table come out as primitives, bucket by bucket.
#include "host_ordering_table.h"
#include "testutil.h"

#include <utility>
#include <vector>

namespace {

using psx::present::DrawPrimitive;
using psx::present::EmitMemory;
using psx::present::HostMemory;
using psx::present::HostOrderingTable;
using psx::present::OtSlot;

constexpr std::uint32_t kHeads = 0x80200000u;
constexpr std::uint32_t kPackets = 0x80201000u;
constexpr std::uint32_t kBuckets = 4u;
constexpr std::uint32_t kF3Words = 4u;

class Collect final : public psx::present::PrimitiveSink {
public:
  void emit(OtSlot slot, const DrawPrimitive &primitive) override {
    emitted.emplace_back(slot, primitive);
  }
  std::vector<std::pair<OtSlot, DrawPrimitive>> emitted;
};

// A flat triangle whose first vertex is at x, linked ahead of what the bucket held.
void link(HostMemory &host, std::uint32_t packet, std::uint32_t bucket, int x) {
  std::uint32_t head = 0;
  host.read(kHeads + bucket * 4u, &head, sizeof(head));
  const std::uint32_t tag = (kF3Words << 24) | (head & 0xFFFFFFu);
  const std::uint32_t words[] = {tag, 0x20FFFFFFu, static_cast<std::uint32_t>(x), 0x00000010u, 0x00100000u};
  host.write(packet, words, sizeof(words));
  const std::uint32_t link = packet & 0xFFFFFFu;
  host.write(kHeads + bucket * 4u, &link, sizeof(link));
}

HostOrderingTable table() {
  return HostOrderingTable{kHeads, kBuckets, OtSlot{3, 10}, 16u};
}

} // namespace

static void test_buckets_walk_from_the_last_and_a_bucket_from_its_newest_packet(void) {
  HostMemory host;
  host.zero(kHeads, kBuckets * 4u);
  host.zero(kPackets, 0x100u);
  link(host, kPackets, 1u, 5);
  link(host, kPackets + 0x20u, 3u, 7);
  link(host, kPackets + 0x40u, 1u, 9);

  Collect sink;
  psx::present::emitHostOrderingTable(host, table(), sink);
  CHECK_EQ(sink.emitted.size(), 3u);
  CHECK(sink.emitted[0].first == (OtSlot{3, 13}));
  CHECK_EQ(sink.emitted[0].second.vertices[0].x, 7);
  CHECK(sink.emitted[1].first == (OtSlot{3, 11}));
  CHECK_EQ(sink.emitted[1].second.vertices[0].x, 9);
  CHECK_EQ(sink.emitted[2].second.vertices[0].x, 5);
  CHECK_EQ(sink.emitted[2].second.vertexCount, 3);
}

static void test_an_empty_table_emits_nothing(void) {
  HostMemory host;
  host.zero(kHeads, kBuckets * 4u);
  Collect sink;
  psx::present::emitHostOrderingTable(host, table(), sink);
  CHECK_EQ(sink.emitted.size(), 0u);
}

int main(void) {
  RUN(buckets_walk_from_the_last_and_a_bucket_from_its_newest_packet);
  RUN(an_empty_table_emits_nothing);
  return pt_summary();
}
