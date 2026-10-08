// guest_packet_pool_windows.h — direct-runtime guest RAM facts for the 2D packet pool.
//
// WHY THIS EXISTS. `OtAttr` can only attribute a guest packet to the producer that wrote it if it
// knows which guest RAM is the packet pool: it records spans inside that window and nothing outside
// it. Until now the window came from ONE place, `GameConfig::packetPool*`, so every title that
// installs a typed `GameRuntime` (`core.cfg == nullptr`) had a filter that was blind — not because
// the title had not reverse-engineered its pool, but because there was nowhere for a typed title to
// say so. The consequence was a confident zero: `GuestPacketFilter::suppressesPacket` matched no
// span and returned "not owned", which reads exactly like a guest that submitted nothing there.
//
// This is the same seam the direct-runtime facts above it use (`guestCdStreamCallbackLayout`,
// `discEnvVar`, `hostIdentity`): the runtime declares immutable guest-RAM facts, the framework reads
// them, and the legacy `GameConfig` path is untouched for titles still on the adapter. A runtime
// that declares none keeps today's behaviour and today's warning.
#pragma once

#include <cstdint>

class Core;

// How the title's pool is laid out. The representations are the layouts guest engines actually use,
// and they are not interchangeable: collapsing two separately-allocated parity pools into one range
// would classify the gap between them — unrelated RAM — as render output, and inventing equal halves
// for a pool that has none declares a window whose top is fiction.
struct GuestPacketPoolWindows {
  enum class Representation : std::uint8_t {
    // One contiguous array of two parity pools: base, and a stride measured in BYTES, each half the
    // declared stride wide. This is `GameConfig::packetPoolBase` / `packetPoolStride`.
    FixedBaseStride,
    // Two independently allocated parity pools, each named by a guest GLOBAL whose value holds the
    // pool's live base and a second one its live end. The bounds are re-read from those globals, so a
    // pool the guest reallocates mid-run is tracked without the framework being told.
    // This is `GameConfig::packetPoolBasePtrs` / `packetPoolEndPtrs`.
    LiveBaseEndPointers,
    // ONE descending pool window that holds BOTH parity ordering tables at its top. Measured on
    // C-12: a walk of each parity OT reaches the same lowest packet address, with the two OT heads
    // 0x10000 apart inside the one window. That is neither an array of two equal halves nor two
    // independently allocated pools: describing it as FixedBaseStride with a half-stride declares a
    // top that is wrong by however much the two halves differ, and LiveBaseEndPointers needs guest
    // globals that reallocate, which this guest does not have. So the pool's measured extent is
    // declared, rather than reconstructed from a shape the guest does not have.
    SingleWindow,
  };

  Representation representation = Representation::FixedBaseStride;

  // FixedBaseStride. `base` is the first byte of the array and `stride` the byte width of ONE parity
  // pool; the window is two strides wide, matching the legacy field pair exactly.
  std::uint32_t base = 0;
  std::uint32_t stride = 0;

  // SingleWindow: `base` is the lowest guest address the pool ever occupies and `end` the exclusive
  // top — the measured extent of the window, not a shape it was derived from.
  std::uint32_t end = 0;

  // LiveBaseEndPointers, one pair per parity pool (0 and 1). A pair whose members are both 0 is
  // absent, not broken: a game with a single parity pool declares one pair. A pair with exactly one
  // member set is refused by name rather than treated as a window of unknown extent.
  std::uint32_t basePointer[2] = {};
  std::uint32_t endPointer[2] = {};

  // Is this a declaration at all? A default-constructed value declares no pool, which is the honest
  // answer for a title whose pool has not been located.
  bool valid() const {
    if (representation == Representation::FixedBaseStride) {
      return base != 0 && stride != 0;
    }
    if (representation == Representation::SingleWindow) {
      return base != 0 && end != 0 && base < end;
    }
    for (int i = 0; i < 2; ++i) {
      if (basePointer[i] || endPointer[i]) {
        return true;
      }
    }
    return false;
  }
};

// The windows this Core's game declares, or null. A legacy `GameConfig` game declares none here —
// its window is read from the config, so that path keeps its existing owner and its existing cache
// key. Declared in a header of its own so a caller can ask the question without pulling in `Game`.
const GuestPacketPoolWindows *declaredGuestPacketPoolWindows(const Core &core);
