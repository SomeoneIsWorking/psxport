// device_cpu.cpp — the CPU, COP0, the GTE, main RAM, the scratchpad, and the Core-private DMA
// channel shadows those four need in order to describe a machine that can be resumed.
//
// RAM IS AN EXECUTABLE WRITE. Putting 2 MB back into guest memory behind Lightrec's back would
// leave every block translated from the bytes being replaced reachable, so `readRamSection` copies
// first and the whole-machine owner reports ONE invalidation over the RAM span afterwards, with the
// single write source (`Savestate`). It reports one span, not one per differing run, because a
// savestate's RAM is replaced wholesale and a per-run notification would be a thousand scans of a
// region whose every byte just changed.
#include "device_sections.h"

#include "core.h"
#include "game.h"
#include "gte_state.h"

#include <algorithm>
#include <array>

namespace psx::state {
namespace {

// A guard the whole state refuses to load across. The exact-PC observer counts instructions at guest
// addresses; a state loaded underneath a live observer would have that observer's counters jump
// across an unrelated run, and every divergence report built on it would be nonsense. Recorded as a
// field rather than tested at save time only, so BOTH directions are caught: saving under an armed
// observer would also write counters that mean nothing on the other side.
constexpr std::uint8_t kCpuLayoutVersion = 1;

} // namespace

void writeCpuSection(Core &core, BlobWriter &out) {
  out.u8(kCpuLayoutVersion);
  out.boolean(core.pcObserver.armed());
  out.array(core.r, std::size(core.r));
  out.u32(core.hi);
  out.u32(core.lo);
  out.u32(core.pc);
  out.array(core.cop0, std::size(core.cop0));
  out.i32(core.pending_work);
  out.u32(core.io_gpustat_toggle);

  const Core::BusShadows shadows = core.busShadows();
  for (const Core::DmaChannelShadow &channel : shadows.channel) {
    out.u32(channel.madr);
    out.u32(channel.bcr);
    out.u32(channel.chcr);
  }
  out.u32(shadows.mdec0Addr);
  out.i32(shadows.mdec0Left);
  out.u32(shadows.mdec1Addr);
  out.i32(shadows.mdec1Left);
  out.u32(shadows.spuXferAddr);
}

bool readCpuSection(Core &core, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  const bool observerArmed = in.boolean();
  std::array<std::uint32_t, 32> gpr{};
  in.array(gpr.data(), gpr.size());
  const std::uint32_t hi = in.u32();
  const std::uint32_t lo = in.u32();
  const std::uint32_t pc = in.u32();
  std::array<std::uint32_t, 16> cop0{};
  in.array(cop0.data(), cop0.size());
  const std::int32_t pendingWork = in.i32();
  const std::uint32_t gpuStatToggle = in.u32();
  Core::BusShadows shadows;
  for (Core::DmaChannelShadow &channel : shadows.channel) {
    channel.madr = in.u32();
    channel.bcr = in.u32();
    channel.chcr = in.u32();
  }
  shadows.mdec0Addr = in.u32();
  shadows.mdec0Left = in.i32();
  shadows.mdec1Addr = in.u32();
  shadows.mdec1Left = in.i32();
  shadows.spuXferAddr = in.u32();
  if (!in.ok()) {
    error = "the cpu section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  if (layout != kCpuLayoutVersion) {
    error = "cpu section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kCpuLayoutVersion);
    return false;
  }
  if (observerArmed || core.pcObserver.armed()) {
    error = "the exact-PC observer is armed on one side of this state; its per-address counters would "
            "be meaningless across the load";
    return false;
  }

  std::copy(gpr.begin(), gpr.end(), core.r);
  core.r[0] = 0; // architectural: MIPS $zero does not hold a value, whatever the file says
  core.hi = hi;
  core.lo = lo;
  core.pc = pc;
  std::copy(cop0.begin(), cop0.end(), core.cop0);
  core.pending_work = pendingWork;
  core.io_gpustat_toggle = gpuStatToggle;
  // `active_native_address` is deliberately NOT restored: it is only ever non-zero inside a native
  // override, and a save cannot be taken there (the control channel services between frames), so a
  // loaded state that found one set would be describing a machine mid-override.
  core.active_native_address = 0;
  core.restoreBusShadows(shadows);
  return true;
}

void writeRamSection(Core &core, BlobWriter &out) {
  out.bytes(std::span<const std::uint8_t>(core.ram, sizeof core.ram));
}

bool readRamSection(Core &core, BlobReader &in, std::string &error) {
  if (!in.bytes(std::span<std::uint8_t>(core.ram, sizeof core.ram))) {
    error = "the ram section is " + std::to_string(in.size()) + " bytes; " + std::to_string(sizeof core.ram) +
            " were needed";
    return false;
  }
  return true;
}

void writeScratchpadSection(Core &core, BlobWriter &out) {
  out.bytes(std::span<const std::uint8_t>(core.scratch, sizeof core.scratch));
}

bool readScratchpadSection(Core &core, BlobReader &in, std::string &error) {
  if (!in.bytes(std::span<std::uint8_t>(core.scratch, sizeof core.scratch))) {
    error = "the scratchpad section is " + std::to_string(in.size()) + " bytes; " +
            std::to_string(sizeof core.scratch) + " were needed";
    return false;
  }
  return true;
}

void writeGteSection(Game &game, BlobWriter &out) {
  out.array(game.gte.REG, std::size(game.gte.REG));
  out.u32(game.gte.FLAGS);
}

bool readGteSection(Game &game, BlobReader &in, std::string &error) {
  std::array<std::uint32_t, 64> regs{};
  in.array(regs.data(), regs.size());
  const std::uint32_t flags = in.u32();
  if (!in.ok()) {
    error = "the gte section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  std::copy(regs.begin(), regs.end(), game.gte.REG);
  game.gte.FLAGS = flags;
  // CR/DR alias REG+32/REG and are re-derived from the copy just made, so a state that renamed them
  // cannot leave the GTE reading through a stale pointer.
  game.gte.DR = game.gte.REG;
  game.gte.CR = game.gte.REG + 32;
  return true;
}

} // namespace psx::state
Core::BusShadows Core::busShadows() const {
  BusShadows shadows;
  shadows.channel[0] = {s_dma0_madr, s_dma0_bcr, s_dma0_chcr};
  shadows.channel[1] = {s_dma1_madr, s_dma1_bcr, s_dma1_chcr};
  shadows.channel[2] = {s_dma2_madr, s_dma2_bcr, s_dma2_chcr};
  shadows.channel[4] = {s_dma4_madr, s_dma4_bcr, s_dma4_chcr};
  shadows.channel[6] = {s_dma6_madr, s_dma6_bcr, s_dma6_chcr};
  shadows.mdec0Addr = s_mdec0_addr;
  shadows.mdec0Left = s_mdec0_left;
  shadows.mdec1Addr = s_mdec1_addr;
  shadows.mdec1Left = s_mdec1_left;
  shadows.spuXferAddr = s_spu_xfer_addr;
  return shadows;
}

void Core::restoreBusShadows(const Core::BusShadows &shadows) {
  s_dma0_madr = shadows.channel[0].madr;
  s_dma0_bcr = shadows.channel[0].bcr;
  s_dma0_chcr = shadows.channel[0].chcr;
  s_dma1_madr = shadows.channel[1].madr;
  s_dma1_bcr = shadows.channel[1].bcr;
  s_dma1_chcr = shadows.channel[1].chcr;
  s_dma2_madr = shadows.channel[2].madr;
  s_dma2_bcr = shadows.channel[2].bcr;
  s_dma2_chcr = shadows.channel[2].chcr;
  s_dma4_madr = shadows.channel[4].madr;
  s_dma4_bcr = shadows.channel[4].bcr;
  s_dma4_chcr = shadows.channel[4].chcr;
  s_dma6_madr = shadows.channel[6].madr;
  s_dma6_bcr = shadows.channel[6].bcr;
  s_dma6_chcr = shadows.channel[6].chcr;
  s_mdec0_addr = shadows.mdec0Addr;
  s_mdec0_left = shadows.mdec0Left;
  s_mdec1_addr = shadows.mdec1Addr;
  s_mdec1_left = shadows.mdec1Left;
  s_spu_xfer_addr = shadows.spuXferAddr;
  // A state that arrives with an MDEC remainder still pending must not keep the one-shot wedge
  // report from before the load: the guest has not been told about this stall yet.
  s_mdec_stall_reported = 0;
  s_mdec_defer_note = 0;
}
