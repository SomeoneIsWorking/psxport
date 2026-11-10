// r3000.h — the R3000A register file of one Core, its register numbers and names.
#pragma once
#include <array>
#include <cstdint>

struct R3000 {
  std::uint32_t r[32];  // GPRs; r[0] is hardwired 0
  std::uint32_t hi, lo; // mult/div result registers
  std::uint32_t pc;     // per Core, synchronized at every executor exit
};

namespace psx::cpu {

// MIPS o32 register numbers, the index into R3000::r.
enum Gpr : std::uint8_t {
  kZero = 0,
  kAt = 1,
  kV0 = 2,
  kV1 = 3,
  kA0 = 4,
  kA1 = 5,
  kA2 = 6,
  kA3 = 7,
  kT0 = 8,
  kT1 = 9,
  kT2 = 10,
  kT3 = 11,
  kT4 = 12,
  kT5 = 13,
  kT6 = 14,
  kT7 = 15,
  kS0 = 16,
  kS1 = 17,
  kS2 = 18,
  kS3 = 19,
  kS4 = 20,
  kS5 = 21,
  kS6 = 22,
  kS7 = 23,
  kT8 = 24,
  kT9 = 25,
  kK0 = 26,
  kK1 = 27,
  kGp = 28,
  kSp = 29,
  kFp = 30,
  kRa = 31,
};

inline constexpr std::size_t kGprCount = 32;

inline constexpr std::array<const char *, kGprCount> kGprNames = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

// Primary opcodes of the memory accesses whose register a vertex place follows. GPR loads are LB..LWR.
inline constexpr std::uint32_t kOpLb = 0x20u;
inline constexpr std::uint32_t kOpLw = 0x23u;
inline constexpr std::uint32_t kOpLwr = 0x26u;
inline constexpr std::uint32_t kOpSw = 0x2Bu;
inline constexpr std::uint32_t kOpSwc2 = 0x3Au;
// COP2 and its rs field for a move from a data or control register into a GPR.
inline constexpr std::uint32_t kOpCop2 = 0x12u;
inline constexpr std::uint32_t kCop2Mfc2 = 0x00u;
inline constexpr std::uint32_t kCop2Cfc2 = 0x02u;

constexpr std::uint32_t primaryOpcode(std::uint32_t instruction) {
  return instruction >> 26;
}
// The rt field: the GPR a load or store moves, or the GTE data register of LWC2/SWC2.
constexpr std::uint32_t rtField(std::uint32_t instruction) {
  return (instruction >> 16) & 0x1Fu;
}

// The rd field: the coprocessor register a COP2 move names.
constexpr std::uint32_t rdField(std::uint32_t instruction) {
  return (instruction >> 11) & 0x1Fu;
}

// The o32 name of a register number; "?" past r31.
constexpr const char *gprName(std::uint32_t index) {
  return index < kGprCount ? kGprNames[index] : "?";
}

// COP0 register numbers.
inline constexpr std::uint32_t kCop0Status = 12;
inline constexpr std::uint32_t kCop0Cause = 13;
inline constexpr std::uint32_t kCop0Epc = 14;

// Saves the whole register file and puts it back when the scope ends or at restore().
class GuestContextScope {
public:
  explicit GuestContextScope(R3000 &live) : live_(live), saved_(live) {}
  GuestContextScope(const GuestContextScope &) = delete;
  GuestContextScope &operator=(const GuestContextScope &) = delete;
  GuestContextScope(GuestContextScope &&) = delete;
  GuestContextScope &operator=(GuestContextScope &&) = delete;
  ~GuestContextScope() {
    restore();
  }

  void restore() {
    live_ = saved_;
  }

private:
  R3000 &live_;
  R3000 saved_;
};

} // namespace psx::cpu
