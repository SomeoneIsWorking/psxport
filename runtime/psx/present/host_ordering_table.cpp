// host_ordering_table.cpp — see host_ordering_table.h.
#include "host_ordering_table.h"

#include "gp0_primitive_decode.h"

#include <lucent/log.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>

namespace psx::present {
namespace {

constexpr std::uint32_t kTagWordShift = 24u;
constexpr std::uint32_t kTagNextMask = 0xFFFFFFu;
constexpr std::uint32_t kDrawModeCommand = 0xE1u;
constexpr std::size_t kMaxPacketWords = 16;

} // namespace

void emitHostOrderingTable(const HostMemory &host, const HostOrderingTable &table, PrimitiveSink &sink) {
  const std::span<const std::byte> heads = host.view(table.heads, table.buckets * 4u);
  std::uint16_t drawMode = 0;
  for (std::uint32_t bucket = table.buckets; bucket-- > 0;) {
    std::uint32_t head = 0;
    std::memcpy(&head, heads.data() + bucket * 4u, sizeof(head));
    std::uint32_t packet = head & kTagNextMask;
    for (std::uint32_t linked = 0; packet != 0; ++linked) {
      std::uint32_t tag = 0;
      host.read(packet, &tag, sizeof(tag));
      const std::uint32_t words = tag >> kTagWordShift;
      std::array<std::uint32_t, kMaxPacketWords> command{};
      if (words > command.size() || linked > table.packetLimit) {
        lucent::error("host-ot", "packet at 0x{:08X} in bucket {} has {} words", packet, bucket, words);
        std::abort();
      }
      host.read(packet + 4u, command.data(), words * 4u);
      if (words > 0 && command[0] >> kTagWordShift == kDrawModeCommand) {
        drawMode = static_cast<std::uint16_t>(command[0]);
        packet = tag & kTagNextMask;
        continue;
      }
      auto primitive = gpu::decodePacketPrimitive(std::span<const std::uint32_t>(command.data(), words));
      if (!primitive) {
        lucent::error("host-ot", "packet at 0x{:08X} in bucket {} is not a polygon or sprite", packet, bucket);
        std::abort();
      }
      if (primitive->kind != PrimitiveKind::Polygon) {
        gpu::applyTexPageAttribute(primitive->state, drawMode);
      }
      sink.emit(OtSlot{table.first.table, table.first.index + bucket}, *primitive);
      packet = tag & kTagNextMask;
    }
  }
}

} // namespace psx::present
