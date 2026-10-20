// psx::cpu::Gte over the Beetle GTE (mednafen/psx/gte.c, compiled as C from the beetle-psx fork).
#include "beetle_gte.h"
#include "core.h"
#include "gte.h"
#include "render_substrate.h"

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
  bind();
  return GTE_ReadDR(reg);
}

void Gte::writeData(std::uint32_t reg, std::uint32_t value) {
  bind();
  GTE_WriteDR(reg, value);
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
  return state;
}

void Gte::restoreRaw(const GteRawState &state) {
  std::memcpy(regs_.REG, state.reg, sizeof state.reg);
  regs_.FLAGS = state.flags;
}

} // namespace psx::cpu
