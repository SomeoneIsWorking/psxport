// psx::cpu::Gte over the Beetle GTE (mednafen/psx/gte.c, compiled as C from the beetle-psx fork).
#include "beetle_gte.h"
#include "core.h"
#include "gte.h"
#include "render_substrate.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

// Knobs the vendored gte.c reads by extern; all off for faithful math.
bool psx_gte_overclock = false;
uint8_t widescreen_hack = 0;
uint8_t widescreen_hack_aspect_ratio_setting = 0;
uint32_t gMode = 0;

namespace psx::cpu {

namespace {

inline constexpr std::uint32_t kOpMask = 0x3F;
inline constexpr std::uint32_t kOpRtps = 0x01;
inline constexpr std::uint32_t kOpRtpt = 0x30;
bool isScreenFifo(std::uint32_t reg) {
  return reg >= gte::kSxy0 && reg <= gte::kSxyP;
}

} // namespace

Gte::Gte(Core &core) : core_(core) {
  GTE_Init();
}

void Gte::bind() {
  GTE_BindState(&regs_);
}

void Gte::release() {
  if (GTE_CurState() == &regs_) {
    GTE_BindState(nullptr);
  }
}

std::uint32_t Gte::readData(std::uint32_t reg) {
  if (isScreenFifo(reg)) {
    lucent::error("gte", "data register {} read as a word; a projected vertex is read through Gte::projected", reg);
    std::abort();
  }
  bind();
  return GTE_ReadDR(reg);
}

void Gte::writeData(std::uint32_t reg, std::uint32_t value) {
  bind();
  GTE_WriteDR(reg, value);
  if (!isScreenFifo(reg)) {
    return;
  }
  const GtePlace place = gte::ProjectedVertex::whole(value).place();
  if (reg == gte::kSxyP) {
    regs_.places[0] = regs_.places[1];
    regs_.places[1] = regs_.places[2];
    regs_.places[2] = place;
  } else {
    regs_.places[reg - gte::kSxy0] = place;
  }
}

gte::ProjectedVertex Gte::projected(std::uint32_t entry) {
  bind();
  return {GTE_ReadDR(gte::kSxy0 + entry), regs_.places[entry]};
}

std::uint32_t Gte::readControl(std::uint32_t reg) {
  bind();
  return GTE_ReadCR(reg);
}

void Gte::writeControl(std::uint32_t reg, std::uint32_t value) {
  bind();
  GTE_WriteCR(reg, value);
}

void Gte::execute(std::uint32_t instruction) {
  bind();
  observer_.observeAround(&core_, core_.pc, instruction, [instruction] {
    GTE_Instruction(instruction);
  });
  const std::uint32_t op = instruction & kOpMask;
  if (op != kOpRtps && op != kOpRtpt) {
    return;
  }
  core_.rsub.otAttr.trackGte(&core_);
}

GteRawState Gte::saveRaw() const {
  GteRawState state{};
  std::memcpy(state.reg, regs_.REG, sizeof state.reg);
  state.flags = regs_.FLAGS;
  std::memcpy(state.places, regs_.places, sizeof state.places);
  return state;
}

void Gte::restoreRaw(const GteRawState &state) {
  std::memcpy(regs_.REG, state.reg, sizeof state.reg);
  regs_.FLAGS = state.flags;
  std::memcpy(regs_.places, state.places, sizeof regs_.places);
}

} // namespace psx::cpu
