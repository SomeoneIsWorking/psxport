// hud_anchor_packets.h — a guest HUD element drawn at an authored 4:3 place, moved to the drawn picture's edge under a
// widening.
#pragma once

#include "core.h"
#include "gp0_primitive_decode.h"
#include "guest_widescreen_projection.h"
#include "record_geometry.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

namespace psx::present {

// The packets one element drawer took from a bump pool, [first, end): libgpu packets laid end to end.
struct HudElementPackets {
  std::uint32_t first = 0;
  std::uint32_t end = 0;
};

// An element wholly left of `centreColumn` moves to the drawn picture's left edge and one wholly right of it to the
// right edge, through the host packets of the walk from `walkRoot`; one across it stays. Guest RAM is unchanged, and
// with no margins drawn nothing moves.
inline void anchorHudElement(Core &core, std::uint32_t walkRoot, HudElementPackets element, int centreColumn) {
  const HorizontalMargins edges = frameWindows(core).drawn;
  if (edges.total() == 0) {
    return;
  }
  constexpr std::uint32_t kRam = 0x00FFFFFFu;
  const std::uint32_t first = element.first & kRam;
  const std::uint32_t end = element.end & kRam;
  const auto packetEnd = [&core](std::uint32_t packet) {
    return packet + (core.mem_r8(packet + 3u) + 1u) * 4u;
  };
  int left = INT_MAX;
  int right = INT_MIN;
  std::array<std::uint32_t, 0xFF> words{};
  for (std::uint32_t packet = first; packet < end; packet = packetEnd(packet)) {
    const std::size_t count = core.mem_r8(packet + 3u);
    for (std::size_t word = 0; word < count; ++word) {
      words[word] = core.mem_r32(packet + 4u + static_cast<std::uint32_t>(word) * 4u);
    }
    const auto primitive = gpu::decodePacketPrimitive({.words = std::span(words.data(), count), .places = {}});
    if (!primitive) {
      continue;
    }
    const gpu::RecordRect bounds = gpu::primitiveBounds(*primitive);
    left = std::min(left, bounds.x0);
    right = std::max(right, bounds.x1);
  }
  int dx = 0;
  if (right <= centreColumn) {
    dx = -edges.left;
  } else if (left >= centreColumn) {
    dx = edges.right;
  }
  if (dx == 0) {
    return;
  }
  for (std::uint32_t packet = first; packet < end; packet = packetEnd(packet)) {
    core.hostPackets.moveX(core, walkRoot, packet | 0x80000000u, dx);
  }
}

} // namespace psx::present
