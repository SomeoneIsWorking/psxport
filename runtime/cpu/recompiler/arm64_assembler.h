// arm64_assembler.h — the AArch64 instruction encoder the ARM64 backend assembles one block with.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psx::cpu::recompiler::arm64 {

using Reg = std::uint32_t; // 0..30; 31 is the zero register or sp, as the instruction defines
inline constexpr Reg kZr = 31;
inline constexpr Reg kSp = 31;

enum Cond : std::uint32_t {
  kEq = 0,
  kNe = 1,
  kHs = 2,
  kLo = 3,
  kHi = 8,
  kLs = 9,
  kGe = 10,
  kLt = 11,
  kGt = 12,
  kLe = 13,
};

// A position in the block, bound once; branches to it before binding are patched at bind.
struct Label {
  long position = -1;
  std::vector<std::size_t> uses;
};

// AArch64 encodings over one growing word buffer. `w` forms are 32-bit, `x` forms 64-bit.
class Assembler {
public:
  [[nodiscard]] const std::vector<std::uint32_t> &words() const {
    return words_;
  }
  [[nodiscard]] std::size_t size() const {
    return words_.size();
  }

  void emit(std::uint32_t word) {
    words_.push_back(word);
  }

  // ---- Labels and branches ----
  void bind(Label &label) {
    label.position = static_cast<long>(words_.size());
    for (std::size_t use : label.uses) {
      patch(use, label.position);
    }
    label.uses.clear();
  }
  void b(Label &label) {
    branchTo(label, 0x14000000u);
  }
  void bcond(Cond cond, Label &label) {
    branchTo(label, 0x54000000u | cond);
  }
  void cbz(Reg rt, Label &label) {
    branchTo(label, 0x34000000u | rt);
  }
  void cbnz(Reg rt, Label &label) {
    branchTo(label, 0x35000000u | rt);
  }
  void blr(Reg rn) {
    emit(0xD63F0000u | (rn << 5));
  }
  void ret() {
    emit(0xD65F03C0u);
  }

  // ---- Moves ----
  void movw(Reg rd, std::uint32_t value) {
    const std::uint32_t lo = value & 0xFFFFu;
    const std::uint32_t hi = value >> 16;
    if (hi == 0) {
      emit(0x52800000u | (lo << 5) | rd); // movz
    } else if (lo == 0) {
      emit(0x52800000u | (1u << 21) | (hi << 5) | rd);
    } else if (hi == 0xFFFFu) {
      emit(0x12800000u | ((~lo & 0xFFFFu) << 5) | rd); // movn
    } else {
      emit(0x52800000u | (lo << 5) | rd);
      emit(0x72800000u | (1u << 21) | (hi << 5) | rd); // movk
    }
  }
  void movx(Reg rd, Reg rm) {
    emit(0xAA0003E0u | (rm << 16) | rd);
  }
  void movw(Reg rd, Reg rm, bool /*registerForm*/) {
    emit(0x2A0003E0u | (rm << 16) | rd);
  }

  // ---- Arithmetic (shifted register) ----
  void addw(Reg rd, Reg rn, Reg rm) {
    emit(0x0B000000u | (rm << 16) | (rn << 5) | rd);
  }
  void addx(Reg rd, Reg rn, Reg rm) {
    emit(0x8B000000u | (rm << 16) | (rn << 5) | rd);
  }
  void subw(Reg rd, Reg rn, Reg rm) {
    emit(0x4B000000u | (rm << 16) | (rn << 5) | rd);
  }
  void cmpw(Reg rn, Reg rm) {
    emit(0x6B000000u | (rm << 16) | (rn << 5) | kZr);
  }
  void cmpx(Reg rn, Reg rm) {
    emit(0xEB000000u | (rm << 16) | (rn << 5) | kZr);
  }
  void andw(Reg rd, Reg rn, Reg rm) {
    emit(0x0A000000u | (rm << 16) | (rn << 5) | rd);
  }
  void orrw(Reg rd, Reg rn, Reg rm) {
    emit(0x2A000000u | (rm << 16) | (rn << 5) | rd);
  }
  void eorw(Reg rd, Reg rn, Reg rm) {
    emit(0x4A000000u | (rm << 16) | (rn << 5) | rd);
  }
  void ornw(Reg rd, Reg rn, Reg rm) {
    emit(0x2A200000u | (rm << 16) | (rn << 5) | rd);
  }
  void lslvw(Reg rd, Reg rn, Reg rm) {
    emit(0x1AC02000u | (rm << 16) | (rn << 5) | rd);
  }
  void lsrvw(Reg rd, Reg rn, Reg rm) {
    emit(0x1AC02400u | (rm << 16) | (rn << 5) | rd);
  }
  void asrvw(Reg rd, Reg rn, Reg rm) {
    emit(0x1AC02800u | (rm << 16) | (rn << 5) | rd);
  }
  void smull(Reg xd, Reg wn, Reg wm) {
    emit(0x9B200000u | (wm << 16) | (kZr << 10) | (wn << 5) | xd);
  }
  void umull(Reg xd, Reg wn, Reg wm) {
    emit(0x9BA00000u | (wm << 16) | (kZr << 10) | (wn << 5) | xd);
  }

  // ---- Arithmetic (immediate, 0..4095) ----
  void addwi(Reg rd, Reg rn, std::uint32_t imm12) {
    emit(0x11000000u | (imm12 << 10) | (rn << 5) | rd);
  }
  // rd = rn + (imm12 << 12)
  void addwiHigh(Reg rd, Reg rn, std::uint32_t imm12) {
    emit(0x11400000u | (imm12 << 10) | (rn << 5) | rd);
  }
  void subwi(Reg rd, Reg rn, std::uint32_t imm12) {
    emit(0x51000000u | (imm12 << 10) | (rn << 5) | rd);
  }
  void addxi(Reg rd, Reg rn, std::uint32_t imm12) {
    emit(0x91000000u | (imm12 << 10) | (rn << 5) | rd);
  }
  void cmpwi(Reg rn, std::uint32_t imm12) {
    emit(0x71000000u | (imm12 << 10) | (rn << 5) | kZr);
  }
  void cmnwi(Reg rn, std::uint32_t imm12) {
    emit(0x31000000u | (imm12 << 10) | (rn << 5) | kZr);
  }

  // ---- Bitfield ----
  void lslwi(Reg rd, Reg rn, std::uint32_t shift) {
    emit(0x53000000u | (((32u - shift) & 31u) << 16) | ((31u - shift) << 10) | (rn << 5) | rd);
  }
  void lsrwi(Reg rd, Reg rn, std::uint32_t shift) {
    emit(0x53000000u | (shift << 16) | (31u << 10) | (rn << 5) | rd);
  }
  void asrwi(Reg rd, Reg rn, std::uint32_t shift) {
    emit(0x13000000u | (shift << 16) | (31u << 10) | (rn << 5) | rd);
  }
  void lsrvx(Reg rd, Reg rn, Reg rm) {
    emit(0x9AC02400u | (rm << 16) | (rn << 5) | rd);
  }
  // xt = [xn + (xm << 3)]
  void ldrxScaled(Reg rt, Reg rn, Reg rm) {
    emit(0xF8607800u | (rm << 16) | (rn << 5) | rt);
  }
  void tbnz(Reg rt, std::uint32_t bitIndex, Label &label) {
    branchTo(label, 0x37000000u | ((bitIndex & 0x1Fu) << 19) | ((bitIndex >> 5) << 31) | rt);
  }
  void cbzx(Reg rt, Label &label) {
    branchTo(label, 0xB4000000u | rt);
  }
  void lsrxi(Reg rd, Reg rn, std::uint32_t shift) {
    emit(0xD3400000u | (shift << 16) | (63u << 10) | (rn << 5) | rd);
  }

  // ---- Logical immediate; false when `value` has no bitmask encoding ----
  bool andwi(Reg rd, Reg rn, std::uint32_t value) {
    return logicalImmediate(0x12000000u, rd, rn, value);
  }
  bool orrwi(Reg rd, Reg rn, std::uint32_t value) {
    return logicalImmediate(0x32000000u, rd, rn, value);
  }
  bool eorwi(Reg rd, Reg rn, std::uint32_t value) {
    return logicalImmediate(0x52000000u, rd, rn, value);
  }
  bool tstwi(Reg rn, std::uint32_t value) {
    return logicalImmediate(0x72000000u, kZr, rn, value); // ands wzr
  }

  // ---- Conditional select ----
  void csetw(Reg rd, Cond cond) {
    emit(0x1A800400u | (kZr << 16) | ((cond ^ 1u) << 12) | (kZr << 5) | rd); // csinc rd, wzr, wzr, !cond
  }
  void cselw(Reg rd, Reg rn, Reg rm, Cond cond) {
    emit(0x1A800000u | (rm << 16) | (cond << 12) | (rn << 5) | rd);
  }

  // ---- Memory (unsigned scaled offsets) ----
  void ldrw(Reg rt, Reg rn, std::uint32_t offset) {
    emit(0xB9400000u | ((offset / 4u) << 10) | (rn << 5) | rt);
  }
  void strw(Reg rt, Reg rn, std::uint32_t offset) {
    emit(0xB9000000u | ((offset / 4u) << 10) | (rn << 5) | rt);
  }
  void ldrx(Reg rt, Reg rn, std::uint32_t offset) {
    emit(0xF9400000u | ((offset / 8u) << 10) | (rn << 5) | rt);
  }
  void strx(Reg rt, Reg rn, std::uint32_t offset) {
    emit(0xF9000000u | ((offset / 8u) << 10) | (rn << 5) | rt);
  }
  void ldrb(Reg rt, Reg rn, std::uint32_t offset) {
    emit(0x39400000u | (offset << 10) | (rn << 5) | rt);
  }
  // Register-offset loads: [xn + xm].
  void ldrwr(Reg rt, Reg rn, Reg rm) {
    emit(0xB8606800u | (rm << 16) | (rn << 5) | rt);
  }
  void ldrbr(Reg rt, Reg rn, Reg rm) {
    emit(0x38606800u | (rm << 16) | (rn << 5) | rt);
  }
  void ldrsbwr(Reg rt, Reg rn, Reg rm) {
    emit(0x38E06800u | (rm << 16) | (rn << 5) | rt);
  }
  void ldrhr(Reg rt, Reg rn, Reg rm) {
    emit(0x78606800u | (rm << 16) | (rn << 5) | rt);
  }
  void ldrshwr(Reg rt, Reg rn, Reg rm) {
    emit(0x78E06800u | (rm << 16) | (rn << 5) | rt);
  }
  // Pairs: pre-indexed store, post-indexed load, signed-offset store and load (offsets in bytes, multiples of 8).
  void stpxPre(Reg rt, Reg rt2, Reg rn, int offset) {
    emit(0xA9800000u | ((static_cast<std::uint32_t>(offset / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt);
  }
  void ldpxPost(Reg rt, Reg rt2, Reg rn, int offset) {
    emit(0xA8C00000u | ((static_cast<std::uint32_t>(offset / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt);
  }
  void stpx(Reg rt, Reg rt2, Reg rn, int offset) {
    emit(0xA9000000u | ((static_cast<std::uint32_t>(offset / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt);
  }
  void ldpx(Reg rt, Reg rt2, Reg rn, int offset) {
    emit(0xA9400000u | ((static_cast<std::uint32_t>(offset / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt);
  }

private:
  void branchTo(Label &label, std::uint32_t opcode) {
    const std::size_t at = words_.size();
    emit(opcode);
    if (label.position >= 0) {
      patch(at, label.position);
    } else {
      label.uses.push_back(at);
    }
  }
  void patch(std::size_t at, long target) {
    const long delta = target - static_cast<long>(at);
    std::uint32_t &word = words_[at];
    if ((word & 0xFC000000u) == 0x14000000u) {
      word |= static_cast<std::uint32_t>(delta) & 0x03FFFFFFu;
    } else if ((word & 0x7E000000u) == 0x36000000u) { // tbz/tbnz
      word |= (static_cast<std::uint32_t>(delta) & 0x3FFFu) << 5;
    } else {
      word |= (static_cast<std::uint32_t>(delta) & 0x7FFFFu) << 5;
    }
  }
  bool logicalImmediate(std::uint32_t opcode, Reg rd, Reg rn, std::uint32_t value) {
    std::uint32_t immr = 0;
    std::uint32_t imms = 0;
    if (!encodeLogical32(value, immr, imms)) {
      return false;
    }
    emit(opcode | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    return true;
  }
  // A 32-bit bitmask immediate: a rotated run of ones replicated across 2..32-bit elements.
  static bool encodeLogical32(std::uint32_t value, std::uint32_t &immr, std::uint32_t &imms) {
    if (value == 0 || value == 0xFFFFFFFFu) {
      return false;
    }
    std::uint32_t size = 32;
    while (size > 2) {
      const std::uint32_t half = size / 2;
      const std::uint32_t mask = (1u << half) - 1u;
      if ((value & mask) != ((value >> half) & mask)) {
        break;
      }
      size = half;
    }
    const std::uint32_t mask = size == 32 ? 0xFFFFFFFFu : (1u << size) - 1u;
    const std::uint32_t element = value & mask;
    const std::uint32_t ones = static_cast<std::uint32_t>(__builtin_popcount(element));
    const std::uint32_t run = ones == 32 ? 0xFFFFFFFFu : (1u << ones) - 1u;
    for (std::uint32_t rotation = 0; rotation < size; ++rotation) {
      // ROL(element, rotation) within `size` bits
      const std::uint32_t rotated =
          rotation == 0 ? element : (((element << rotation) | (element >> (size - rotation))) & mask);
      if (rotated == run) {
        immr = rotation;
        imms = ((~(size - 1u) << 1) & 0x3Fu) | (ones - 1u);
        return true;
      }
    }
    return false;
  }

  std::vector<std::uint32_t> words_;
};

} // namespace psx::cpu::recompiler::arm64
