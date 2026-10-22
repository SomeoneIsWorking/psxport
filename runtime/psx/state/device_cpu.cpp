// The CPU, COP0, the GTE, main RAM and the scratchpad.
#include "device_sections.h"

#include "core.h"
#include "game.h"
#include "gte_register_transfer.h"
#include "gte_state.h"

#include <algorithm>
#include <array>
#include <span>

namespace psx::state {
namespace {

constexpr std::uint8_t kCpuLayoutVersion = 4;

} // namespace

void writeCpuSection(Core &core, psx::platform::BlobWriter &out) {
  out.u8(kCpuLayoutVersion);
  out.array(core.r, std::size(core.r));
  out.u32(core.hi);
  out.u32(core.lo);
  out.u32(core.pc);
  out.array(core.cop0, std::size(core.cop0));
  out.i32(core.pending_work);
}

bool readCpuSection(Core &core, psx::platform::BlobReader &in, std::string &error) {
  if (!in.expectLayout("cpu", kCpuLayoutVersion, error)) {
    return false;
  }
  std::array<std::uint32_t, 32> gpr{};
  in.array(gpr.data(), gpr.size());
  const std::uint32_t hi = in.u32();
  const std::uint32_t lo = in.u32();
  const std::uint32_t pc = in.u32();
  std::array<std::uint32_t, 16> cop0{};
  in.array(cop0.data(), cop0.size());
  const std::int32_t pendingWork = in.i32();
  if (!in.decoded("cpu", error)) {
    return false;
  }

  std::copy(gpr.begin(), gpr.end(), core.r);
  core.r[psx::cpu::kZero] = 0;
  core.hi = hi;
  core.lo = lo;
  core.pc = pc;
  std::copy(cop0.begin(), cop0.end(), core.cop0);
  core.pending_work = pendingWork;
  // Saves are taken between frames, never inside an override.
  core.active_native_address = 0;
  return true;
}

void writeRamSection(Core &core, psx::platform::BlobWriter &out) {
  out.bytes(std::span<const std::uint8_t>(core.ram, sizeof core.ram));
}

bool readRamSection(Core &core, psx::platform::BlobReader &in, std::string &error) {
  if (!in.bytes(std::span<std::uint8_t>(core.ram, sizeof core.ram))) {
    error = "the ram section is " + std::to_string(in.size()) + " bytes; " + std::to_string(sizeof core.ram) +
            " were needed";
    return false;
  }
  return true;
}

void writeScratchpadSection(Core &core, psx::platform::BlobWriter &out) {
  out.bytes(std::span<const std::uint8_t>(core.scratch, sizeof core.scratch));
}

bool readScratchpadSection(Core &core, psx::platform::BlobReader &in, std::string &error) {
  if (!in.bytes(std::span<std::uint8_t>(core.scratch, sizeof core.scratch))) {
    error = "the scratchpad section is " + std::to_string(in.size()) + " bytes; " +
            std::to_string(sizeof core.scratch) + " were needed";
    return false;
  }
  return true;
}

void writeGteSection(Game &game, psx::platform::BlobWriter &out) {
  const GteRegs &gte = game.core.gte().registers();
  out.array(gte.REG, std::size(gte.REG));
  out.u32(gte.FLAGS);
}

bool readGteSection(Game &game, psx::platform::BlobReader &in, std::string &error) {
  std::array<std::uint32_t, 64> regs{};
  in.array(regs.data(), regs.size());
  const std::uint32_t flags = in.u32();
  if (!in.decoded("gte", error)) {
    return false;
  }
  GteRegs &gte = game.core.gte().registers();
  psx::cpu::placeReplacedScreenEntries(gte, std::span<const std::uint32_t, 32>(regs.data(), 32));
  std::copy(regs.begin(), regs.end(), gte.REG);
  gte.FLAGS = flags;
  // DR and CR alias REG and REG+32.
  gte.DR = gte.REG;
  gte.CR = gte.REG + 32;
  return true;
}

} // namespace psx::state
