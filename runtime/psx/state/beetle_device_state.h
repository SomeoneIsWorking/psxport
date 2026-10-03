// beetle_device_state.h — Beetle's OWN device save-state functions, wrapped.
//
// The vendored Beetle SPU, MDEC and GPU each carry a complete `*_StateAction(StateMem *, int load,
// int data_only)` in the fork (mednafen/psx/spu.c, mdec.c, gpu.c). Re-describing those devices here
// would be a second, worse copy of a state format that is already written, reviewed and
// bug-fixed in the fork — and the SPU alone carries 24 voices' worth of ADSR/envelope/sweep state
// plus 512 KB of SPU RAM, which is exactly the state a hand-written enumerator gets wrong
// silently.
//
// So this module owns nothing but the plumbing: one RAII `StateMem` over a heap buffer (the fork's
// state.c allocates and grows it; nothing here does), and the bind rule those functions need.
//
// THE BIND RULE. `SPU_StateAction` and `MDEC_StateAction` operate on whichever instance
// SPU_BindState / MDEC_BindState last selected, and Beetle's GPU on the process-global GPU. The
// whole-machine owner therefore binds the instance it is capturing BEFORE each call and restores
// the previous binding after it — the same discipline `SpuDevice::bind` uses per frame-step. The
// save and load paths go through the identical sequence, because a state that round-trips only in
// one direction is worse than one that does not round-trip at all.
//
// The GTE is deliberately NOT here. psxport's GTE state is `GteRegs` — the 64 register words and
// FLAGS, which `MachineState` writes directly — and Beetle's `GTE_StateAction` additionally saves
// Matrices/CRVectors duplicates for compatibility with savestates this port does not read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

class Core;

namespace psx::state {

// Beetle's own state stream (mednafen/state.h). Declared here rather than including the vendor
// header, the same way every other adapter in this tree declares a Beetle API.
struct BeetleStateMem {
  std::uint8_t *data;
  std::uint32_t loc;
  std::uint32_t len;
  std::uint32_t malloced;
  std::uint32_t initial_malloc;
};

// Which devices of the vendored fork this bridge can reach. The independent GPU ORACLE
// (runtime/psx/gpu/gpu_beetle.cpp) is deliberately absent: it is not the GPU the guest draws with and it
// is inert unless a run enabled it, so there is nothing guest-visible here to carry.
enum class BeetleDevice : unsigned { Spu = 0, Mdec = 1 };
inline constexpr std::size_t kBeetleDeviceCount = 2;

// Run Beetle's save then its load for one device, and report whether the state survived. This is the
// selftest the wrapper is trusted on, and it refuses when the fixture cannot MOVE the device — a
// round-trip against an unchanged device is the vacuous zero this tree keeps publishing by mistake.
bool beetleDeviceRoundTrips(Core &core, BeetleDevice device, std::string &error);

// One device's state, as Beetle writes and reads it. Owns its stream; copy and move are deleted
// because the fork's `realloc` hands the buffer back on destruction and a copied StateMem would
// free the same pointer twice.

class BeetleDeviceState {
public:
  BeetleDeviceState() = default;
  ~BeetleDeviceState();
  BeetleDeviceState(const BeetleDeviceState &) = delete;
  BeetleDeviceState &operator=(const BeetleDeviceState &) = delete;

  // Serialize `device` as bound to `core` into this stream. False when the fork refused — a short
  // write, or its own reader failing to find the section it just wrote — with the reason filled in.
  bool save(Core &core, BeetleDevice device, std::string &error);
  // Restore `payload` into `device` as bound to `core`. False when the fork's reader could not find
  // or decode the section.
  bool load(Core &core, BeetleDevice device, std::span<const std::uint8_t> payload, std::string &error);

  [[nodiscard]] std::span<const std::uint8_t> bytes() const {
    return {sm.data, sm.len};
  }
  // Bytes the stream holds, and bytes the save asked it to hold — so a truncated write can be
  // reported as a pair rather than as a smaller file that looks complete.
  [[nodiscard]] std::size_t size() const {
    return sm.len;
  }
  std::vector<std::uint8_t> bytesOut() const;
  // The fork's stream, for the whole-stream selftest that drives MDFNSS_SaveSM/MDFNSS_LoadSM.
  BeetleStateMem &rawStream();
  void clear();

private:
  BeetleStateMem sm{};
};
} // namespace psx::state
