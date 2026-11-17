// r3000_decode.h — R3000 instruction fields, opcodes, the block shape both guest engines share, and the GPRs
// each instruction reads and writes.
#pragma once

#include "r3000.h"

#include <cstdint>

namespace psx::cpu {

constexpr std::uint32_t rsField(std::uint32_t instruction) {
  return (instruction >> 21) & 0x1Fu;
}
constexpr std::uint32_t functField(std::uint32_t instruction) {
  return instruction & 0x3Fu;
}
constexpr std::uint32_t shamtField(std::uint32_t instruction) {
  return (instruction >> 6) & 0x1Fu;
}
constexpr std::uint32_t immediate(std::uint32_t instruction) {
  return instruction & 0xFFFFu;
}
constexpr std::uint32_t signedImmediate(std::uint32_t instruction) {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(instruction & 0xFFFFu)));
}

// Primary opcodes.
enum : std::uint32_t {
  kSpecial = 0x00,
  kRegimm = 0x01,
  kJ = 0x02,
  kJal = 0x03,
  kBeq = 0x04,
  kBne = 0x05,
  kBlez = 0x06,
  kBgtz = 0x07,
  kAddi = 0x08,
  kAddiu = 0x09,
  kSlti = 0x0A,
  kSltiu = 0x0B,
  kAndi = 0x0C,
  kOri = 0x0D,
  kXori = 0x0E,
  kLui = 0x0F,
  kCop0 = 0x10,
  kCop2 = 0x12,
  kLb = 0x20,
  kLh = 0x21,
  kLwl = 0x22,
  kLw = 0x23,
  kLbu = 0x24,
  kLhu = 0x25,
  kLwr = 0x26,
  kSb = 0x28,
  kSh = 0x29,
  kSwl = 0x2A,
  kSw = 0x2B,
  kSwr = 0x2E,
  kLwc2 = 0x32,
  kSwc2 = 0x3A,
};

// SPECIAL function codes.
enum : std::uint32_t {
  kSll = 0x00,
  kSrl = 0x02,
  kSra = 0x03,
  kSllv = 0x04,
  kSrlv = 0x06,
  kSrav = 0x07,
  kJr = 0x08,
  kJalr = 0x09,
  kSyscall = 0x0C,
  kBreak = 0x0D,
  kMfhi = 0x10,
  kMthi = 0x11,
  kMflo = 0x12,
  kMtlo = 0x13,
  kMult = 0x18,
  kMultu = 0x19,
  kDiv = 0x1A,
  kDivu = 0x1B,
  kAdd = 0x20,
  kAddu = 0x21,
  kSub = 0x22,
  kSubu = 0x23,
  kAnd = 0x24,
  kOr = 0x25,
  kXor = 0x26,
  kNor = 0x27,
  kSlt = 0x2A,
  kSltu = 0x2B,
};

// Coprocessor rs fields.
enum : std::uint32_t {
  kMfc = 0x00,
  kCfc = 0x02,
  kMtc = 0x04,
  kCtc = 0x06,
};

// RFE: COP0 with the CO bit and function 0x10.
constexpr bool isRfe(std::uint32_t instruction) {
  return primaryOpcode(instruction) == kCop0 && (instruction & 0x0200003Fu) == 0x02000010u;
}

// A jump the block cannot run past: J, JAL, JR, JALR, and the always-taken branch forms.
constexpr bool isUnconditionalJump(std::uint32_t instruction) {
  switch (primaryOpcode(instruction)) {
  case kSpecial:
    return functField(instruction) == kJr || functField(instruction) == kJalr;
  case kJ:
  case kJal:
    return true;
  case kBeq:
  case kBlez:
    return rsField(instruction) == rtField(instruction);
  case kRegimm: {
    const std::uint32_t rt = rtField(instruction);
    return (rt == 0x01u || rt == 0x11u) && rsField(instruction) == 0;
  }
  default:
    return false;
  }
}

// Any instruction with a delay slot: the jumps and every branch form.
constexpr bool isBranchOrJump(std::uint32_t instruction) {
  switch (primaryOpcode(instruction)) {
  case kSpecial:
    return functField(instruction) == kJr || functField(instruction) == kJalr;
  case kRegimm:
  case kJ:
  case kJal:
  case kBeq:
  case kBne:
  case kBlez:
  case kBgtz:
    return true;
  default:
    return false;
  }
}

// An instruction that ends its block on its own: SYSCALL, or a write to Status or Cause.
constexpr bool endsBlock(std::uint32_t instruction) {
  if (primaryOpcode(instruction) == kSpecial) {
    return functField(instruction) == kSyscall;
  }
  if (primaryOpcode(instruction) == kCop0) {
    const std::uint32_t move = rsField(instruction);
    return (move == kMtc || move == kCtc) &&
           (rdField(instruction) == kCop0Status || rdField(instruction) == kCop0Cause);
  }
  return false;
}

constexpr bool isStore(std::uint32_t op) {
  return op == kSb || op == kSh || op == kSwl || op == kSw || op == kSwr || op == kSwc2;
}

// The GPR a delayed load fills (LB..LWR, MFC0/CFC0, MFC2/CFC2), or 0 for every other instruction: its value
// reaches the register after the next instruction, unless that instruction writes or loads it first.
constexpr std::uint32_t delayedLoadTarget(std::uint32_t instruction) {
  const std::uint32_t op = primaryOpcode(instruction);
  if (op >= kLb && op <= kLwr) {
    return rtField(instruction);
  }
  if (op == kCop0 || op == kCop2) {
    const std::uint32_t move = rsField(instruction);
    if ((instruction & (1u << 25)) == 0 && (move == kMfc || move == kCfc)) {
      return rtField(instruction);
    }
  }
  return 0;
}

// The GPRs an instruction reads, one bit per register (bit 0, r0, never set). LWL/LWR read their own rt (the merge).
constexpr std::uint32_t gprReads(std::uint32_t instruction) {
  const auto bit = [](std::uint32_t reg) {
    return reg == 0 ? 0u : 1u << reg;
  };
  const std::uint32_t rs = rsField(instruction);
  const std::uint32_t rt = rtField(instruction);
  switch (primaryOpcode(instruction)) {
  case kSpecial:
    switch (functField(instruction)) {
    case kSll:
    case kSrl:
    case kSra:
      return bit(rt);
    case kJr:
    case kJalr:
    case kMthi:
    case kMtlo:
      return bit(rs);
    case kSyscall:
    case kBreak:
    case kMfhi:
    case kMflo:
      return 0;
    default:
      return bit(rs) | bit(rt);
    }
  case kRegimm:
  case kBlez:
  case kBgtz:
  case kAddi:
  case kAddiu:
  case kSlti:
  case kSltiu:
  case kAndi:
  case kOri:
  case kXori:
  case kLb:
  case kLh:
  case kLw:
  case kLbu:
  case kLhu:
  case kLwc2:
    return bit(rs);
  case kBeq:
  case kBne:
  case kLwl:
  case kLwr:
  case kSb:
  case kSh:
  case kSwl:
  case kSw:
  case kSwr:
    return bit(rs) | bit(rt);
  case kSwc2:
    return bit(rs);
  case kCop0:
  case kCop2:
    if ((instruction & (1u << 25)) == 0 && (rs == kMtc || rs == kCtc)) {
      return bit(rt);
    }
    return 0;
  default:
    return 0;
  }
}

// The GPR an instruction writes when it runs (not through a delayed load), or 0.
constexpr std::uint32_t immediateGprWrite(std::uint32_t instruction) {
  switch (primaryOpcode(instruction)) {
  case kSpecial:
    switch (functField(instruction)) {
    case kSll:
    case kSrl:
    case kSra:
    case kSllv:
    case kSrlv:
    case kSrav:
    case kJalr:
    case kMfhi:
    case kMflo:
    case kAdd:
    case kAddu:
    case kSub:
    case kSubu:
    case kAnd:
    case kOr:
    case kXor:
    case kNor:
    case kSlt:
    case kSltu:
      return rdField(instruction);
    default:
      return 0;
    }
  case kRegimm:
    return (rtField(instruction) & 0x1Eu) == 0x10u ? kRa : 0u;
  case kJal:
    return kRa;
  case kAddi:
  case kAddiu:
  case kSlti:
  case kSltiu:
  case kAndi:
  case kOri:
  case kXori:
  case kLui:
    return rtField(instruction);
  default:
    return 0;
  }
}

// The named fault the interpreter raises for an instruction it has no semantics for, or null for a known one.
constexpr const char *unknownInstructionFault(std::uint32_t instruction) {
  switch (primaryOpcode(instruction)) {
  case kSpecial:
    switch (functField(instruction)) {
    case kSll:
    case kSrl:
    case kSra:
    case kSllv:
    case kSrlv:
    case kSrav:
    case kJr:
    case kJalr:
    case kSyscall:
    case kBreak:
    case kMfhi:
    case kMthi:
    case kMflo:
    case kMtlo:
    case kMult:
    case kMultu:
    case kDiv:
    case kDivu:
    case kAdd:
    case kAddu:
    case kSub:
    case kSubu:
    case kAnd:
    case kOr:
    case kXor:
    case kNor:
    case kSlt:
    case kSltu:
      return nullptr;
    default:
      return "unknown SPECIAL instruction";
    }
  case kCop0:
    switch (rsField(instruction)) {
    case kMfc:
    case kCfc:
    case kMtc:
    case kCtc:
      return nullptr;
    default:
      return isRfe(instruction) ? nullptr : "unknown COP0 instruction";
    }
  case kCop2:
    if ((instruction & (1u << 25)) != 0) {
      return nullptr;
    }
    switch (rsField(instruction)) {
    case kMfc:
    case kCfc:
    case kMtc:
    case kCtc:
      return nullptr;
    default:
      return "unknown COP2 instruction";
    }
  case kRegimm:
  case kJ:
  case kJal:
  case kBeq:
  case kBne:
  case kBlez:
  case kBgtz:
  case kAddi:
  case kAddiu:
  case kSlti:
  case kSltiu:
  case kAndi:
  case kOri:
  case kXori:
  case kLui:
  case kLb:
  case kLh:
  case kLwl:
  case kLw:
  case kLbu:
  case kLhu:
  case kLwr:
  case kSb:
  case kSh:
  case kSwl:
  case kSw:
  case kSwr:
  case kLwc2:
  case kSwc2:
    return nullptr;
  default:
    return "unknown instruction";
  }
}

} // namespace psx::cpu
