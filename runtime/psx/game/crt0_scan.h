// crt0_scan.h — a PSY-Q crt0's boot group, decoded symbolically from the guest's own prologue.
//
// Measured on Spyro 1/2/3, Crash 1/2/3, Crash Bash, Tomba 1/2, Spider-Man, Mega Man X4 and Toy Story 2: one
// crt0 differing only in immediates, ending in `jal` to the A(39h) InitHeap(ptr, size) thunk. Straight-line
// only; the .bss loop's back-edge is not followed.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace psx::game {

// Every measured crt0 reaches its `jal` within 36 instructions.
inline constexpr int kCrt0ScanLimit = 96;

enum class Crt0Value : std::uint8_t {
  Unknown,
  Constant, // lui/addiu/ori/addu chains
  Loaded,   // `lw` from a resolved address, plus any later addi
  HeapSize, // the subu chain InitHeap receives as its size
  HeapBase, // the masked heap base InitHeap receives as its pointer
};

struct Crt0SymbolicRegister {
  Crt0Value tag = Crt0Value::Unknown;
  std::uint32_t val = 0;
  std::uint32_t src = 0; // Loaded: the address read
  std::int32_t bias = 0; // Loaded: the accumulated addi
};

struct Crt0Scan {
  const char *stopped = "scan limit";
  int decoded = 0;
  int allZero = 0;

  std::uint32_t bssLo = 0;
  std::uint32_t bssHi = 0;
  std::uint32_t stackTopBase = 0;
  std::int32_t bias = 0;
  std::uint32_t stackTopBase2 = 0;
  std::uint32_t heapBase = 0; // unmasked
  std::uint32_t gp = 0;
  std::uint32_t libcInit = 0;
  // The optional absolute stores; 0 when the complete prologue has none.
  std::uint32_t heapSizePtr = 0;
  std::uint32_t heapBasePtr = 0;

  bool haveBssLo = false;
  bool haveBssHi = false;
  bool haveStackTop = false;
  bool haveBias = false;
  bool haveReserve = false;
  bool haveHeapBase = false;
  bool haveGp = false;
  bool haveLibcInit = false;

  // True once the scan reached the `jal`, so an unseen store is an absence.
  bool scanComplete = false;
  bool sawHeapSizeStore = false;
  bool sawHeapBaseStore = false;
  bool a1LiveAtCall = false;       // a1 holds the heap size at the `jal`
  bool a0PlusFour = false;         // the delay slot is `addi a0,a0,4`
  bool libcInitIsInitHeap = false; // the callee is the A(39h) thunk

  // The required fields the scan left unresolved, comma-separated; empty when the group is complete.
  [[nodiscard]] std::string unresolved() const {
    const std::array<std::pair<const char *, bool>, 9> fields{{
        {"prologue", scanComplete},
        {"bssZeroLo", haveBssLo},
        {"bssZeroHi", haveBssHi},
        {"stackTopBase", haveStackTop},
        {"stackBias", haveBias},
        {"stackTopBase2", haveReserve},
        {"heapBase", haveHeapBase},
        {"gp", haveGp},
        {"libcInit", haveLibcInit},
    }};
    std::string names;
    for (const auto &[name, resolved] : fields) {
      if (!resolved) {
        names += names.empty() ? "" : ", ";
        names += name;
      }
    }
    return names;
  }
};

// Decodes the crt0 at `entry`, reading guest words through `rd32(address)`.
template <class Reader> Crt0Scan scanCrt0(uint32_t entry, Reader rd32) {
  Crt0Scan o;
  std::array<Crt0SymbolicRegister, 32> r{};
  r[0].tag = Crt0Value::Constant;
  r[0].val = 0; // $zero

  int spSrcReg = -1; // the register whose Crt0Value::Loaded value became sp
  for (int i = 0; i < kCrt0ScanLimit; i++) {
    const uint32_t pc = entry + 4u * (uint32_t)i;
    const uint32_t w = rd32(pc);
    o.decoded = i + 1;
    if (w == 0) {
      o.allZero++;
      continue;
    }

    const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    const uint32_t sa = (w >> 6) & 31, fn = w & 0x3F;
    const int32_t imm = (int32_t)(int16_t)(uint16_t)(w & 0xFFFF);

    if (op == 0x03) { // jal — the end of the prologue
      o.haveLibcInit = true;
      o.libcInit = ((pc + 4u) & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
      o.a1LiveAtCall = (r[5].tag == Crt0Value::HeapSize);
      const uint32_t ds = rd32(pc + 4u); // the delay slot really is part of the call
      o.a0PlusFour = ((ds >> 26) == 0x08 || (ds >> 26) == 0x09) && ((ds >> 21) & 31) == 4 && ((ds >> 16) & 31) == 4 &&
                     (int32_t)(int16_t)(uint16_t)(ds & 0xFFFF) == 4;
      // The A(39h) InitHeap thunk signature: addiu t2,zero,0xA0 / jr t2 / addiu t1,zero,0x39.
      o.libcInitIsInitHeap = rd32(o.libcInit) == 0x240A00A0u && rd32(o.libcInit + 4u) == 0x01400008u &&
                             rd32(o.libcInit + 8u) == 0x24090039u;
      o.scanComplete = true;
      o.stopped = "jal (libcInit)";
      break;
    }
    switch (op) {
    case 0x0F: // lui
      r[rt].tag = Crt0Value::Constant;
      r[rt].val = (uint32_t)(w & 0xFFFF) << 16;
      break;
    case 0x08:
    case 0x09:                      // addi / addiu
      if (r[rs].tag == Crt0Value::Loaded) { // biasing a loaded word — this is the sp bias
        r[rt] = r[rs];
        r[rt].bias += imm;
      } else if (r[rs].tag == Crt0Value::Constant) {
        r[rt].tag = Crt0Value::Constant;
        r[rt].val = r[rs].val + (uint32_t)imm;
      } else if (r[rs].tag == Crt0Value::HeapBase) {
        r[rt] = r[rs];
        r[rt].val += (uint32_t)imm;
      } else {
        r[rt] = Crt0SymbolicRegister{};
      }
      // $gp is `lui gp,hi / addiu gp,gp,lo`, resolved on the second half.
      if (rt == 28 && r[28].tag == Crt0Value::Constant) {
        o.haveGp = true;
        o.gp = r[28].val;
      }
      break;
    case 0x0D: // ori
      if (r[rs].tag == Crt0Value::Constant) {
        r[rt].tag = Crt0Value::Constant;
        r[rt].val = r[rs].val | (w & 0xFFFF);
      } else {
        r[rt] = Crt0SymbolicRegister{};
      }
      break;
    case 0x23: // lw
      if (r[rs].tag == Crt0Value::Constant) {
        r[rt].tag = Crt0Value::Loaded;
        r[rt].src = r[rs].val + (uint32_t)imm;
        r[rt].bias = 0;
      } else {
        r[rt] = Crt0SymbolicRegister{};
      }
      break;
    case 0x2B: // sw
      if (r[rs].tag == Crt0Value::Constant) {
        const uint32_t addr = r[rs].val + (uint32_t)imm;
        if (r[rt].tag == Crt0Value::Constant && r[rt].val == 0 && !o.haveBssLo) { // `sw zero,0(v0)` — .bss loop
          o.haveBssLo = true;
          o.bssLo = addr;
        } else if (r[rt].tag == Crt0Value::HeapSize) {
          o.sawHeapSizeStore = true;
          o.heapSizePtr = addr;
        } else if (r[rt].tag == Crt0Value::HeapBase) {
          o.sawHeapBaseStore = true;
          o.heapBasePtr = addr;
        }
      }
      break;
    case 0x00:
      switch (fn) {
      case 0x21: // addu
        if (r[rs].tag == Crt0Value::Constant && r[rt].tag == Crt0Value::Constant) {
          r[rd].tag = Crt0Value::Constant;
          r[rd].val = r[rs].val + r[rt].val;
        } else if (r[rs].tag == Crt0Value::Constant && r[rs].val == 0) {
          r[rd] = r[rt];
        } else if (r[rt].tag == Crt0Value::Constant && r[rt].val == 0) {
          r[rd] = r[rs];
        } else {
          r[rd] = Crt0SymbolicRegister{};
        }
        break;
      case 0x23: // subu — the heap-size chain
        if (r[rs].tag == Crt0Value::Loaded && r[rt].tag == Crt0Value::Loaded) {
          // (stack-top word ± bias) - (reserve word): names the SECOND global.
          if (spSrcReg >= 0 && r[rs].src == o.stackTopBase) {
            o.haveReserve = true;
            o.stackTopBase2 = r[rt].src;
          }
          r[rd].tag = Crt0Value::HeapSize;
        } else if (r[rs].tag == Crt0Value::HeapSize && r[rt].tag == Crt0Value::HeapBase) {
          r[rd].tag = Crt0Value::HeapSize; // …minus the masked heap base
        } else {
          r[rd] = Crt0SymbolicRegister{};
        }
        break;
      case 0x25:        // or — sp = word|KSEG0, and heapbase|KSEG0
        if (rd == 29) { // $sp
          const int src = (r[rs].tag == Crt0Value::Loaded) ? (int)rs : (r[rt].tag == Crt0Value::Loaded) ? (int)rt : -1;
          if (src >= 0) {
            spSrcReg = src;
            o.haveStackTop = true;
            o.stackTopBase = r[src].src;
            o.haveBias = true;
            o.bias = r[src].bias;
          }
        }
        if (r[rs].tag == Crt0Value::HeapBase) {
          r[rd] = r[rs];
        } else if (r[rt].tag == Crt0Value::HeapBase) {
          r[rd] = r[rt];
        } else if (rd != 29) {
          r[rd] = Crt0SymbolicRegister{};
        }
        break;
      case 0x00: // sll — the `& 0x1FFFFFFF` mask, first half
        if (r[rt].tag == Crt0Value::Constant && sa == 3) {
          o.haveHeapBase = true;
          o.heapBase = r[rt].val;
          r[rd].tag = Crt0Value::HeapBase;
          r[rd].val = r[rt].val << 3;
        } else {
          r[rd] = Crt0SymbolicRegister{};
        }
        break;
      case 0x02: // srl — the mask's second half
        if (r[rt].tag == Crt0Value::HeapBase && sa == 3) {
          r[rd].tag = Crt0Value::HeapBase;
          r[rd].val = r[rt].val >> 3;
        } else {
          r[rd] = Crt0SymbolicRegister{};
        }
        break;
      case 0x08:
        o.stopped = "jr";
        return o; // left the prologue without a jal
      case 0x0D:
        o.stopped = "break";
        return o;
      default:
        r[rd] = Crt0SymbolicRegister{};
        break;
      }
      break;
    default:
      break; // branches and everything else define no tracked reg
    }
    // The .bss loop's `sltu at, vLo, vHi` reads the span's end.
    if (op == 0x00 && fn == 0x2B && !o.haveBssHi && r[rt].tag == Crt0Value::Constant) {
      o.haveBssHi = true;
      o.bssHi = r[rt].val;
    }
  }
  return o;
}

} // namespace psx::game
