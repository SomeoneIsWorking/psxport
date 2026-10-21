// guest_abi.h — native-override helpers that keep the MIPS register and stack ABI the guest sees.
// Frame sizes and spill offsets come from the guest function's prologue.
#pragma once
#include "core.h"

#include <cstdint>
#include <span>

// A callee-saved register held as a local: every access is c->r[N], so a nested guest call spills
// the live value.
template <int N> struct GuestReg {
  Core *c;
  explicit GuestReg(Core *c_) : c(c_) {}

  operator uint32_t() const {
    return c->r[N];
  }
  GuestReg &operator=(uint32_t v) {
    c->r[N] = v;
    return *this;
  }
  GuestReg &operator=(const GuestReg &o) {
    c->r[N] = static_cast<uint32_t>(o);
    return *this;
  }
  GuestReg &operator+=(uint32_t v) {
    c->r[N] += v;
    return *this;
  }
  GuestReg &operator-=(uint32_t v) {
    c->r[N] -= v;
    return *this;
  }
};

// One prologue spill: a register and its sp-relative offset.
struct GuestFrameSpill {
  int reg;
  int offset;
};

namespace psx::cpu {

// A guest prologue: lowers sp by the frame size and spills each register at its offset.
inline void enterGuestFrame(Core &core,
                            std::uint32_t frameSize,
                            std::span<const GuestFrameSpill> spills) {
  core.r[kSp] -= frameSize;
  for (const GuestFrameSpill &spill : spills) {
    core.mem_w32(core.r[kSp] + static_cast<std::uint32_t>(spill.offset), core.r[spill.reg]);
  }
}

} // namespace psx::cpu

// The guest's stack frame: lowers sp and spills on entry, reloads and raises sp on exit.
// GuestFrame<0, 0> is a leaf with no frame.
template <int FrameSize, int NumSpills> struct GuestFrame {
  Core *c;
  const GuestFrameSpill (&spills)[NumSpills];

  GuestFrame(Core *c_, const GuestFrameSpill (&spills_)[NumSpills]) : c(c_), spills(spills_) {
    psx::cpu::enterGuestFrame(*c, FrameSize, spills);
  }
  ~GuestFrame() {
    for (int i = 0; i < NumSpills; i++) {
      c->r[spills[i].reg] = c->mem_r32(c->r[psx::cpu::kSp] + (uint32_t)spills[i].offset);
    }
    c->r[psx::cpu::kSp] += FrameSize;
  }
};

namespace psx::cpu {

// MIPS mult with its guest-visible HI/LO. Division is psx::cpu::divide (cpu_divide.h).
inline int64_t multiply(Core *c, int32_t a, int32_t b) {
  int64_t p = (int64_t)a * (int64_t)b;
  c->lo = (uint32_t)p;
  c->hi = (uint32_t)((uint64_t)p >> 32);
  return p;
}

// A retail body returns with the $ra it entered with; declare this first in an override.
class PreservedReturnAddress {
public:
  explicit PreservedReturnAddress(Core &core) : core_(core), entry_(core.r[kRa]) {}
  PreservedReturnAddress(const PreservedReturnAddress &) = delete;
  PreservedReturnAddress &operator=(const PreservedReturnAddress &) = delete;
  PreservedReturnAddress(PreservedReturnAddress &&) = delete;
  PreservedReturnAddress &operator=(PreservedReturnAddress &&) = delete;
  ~PreservedReturnAddress() {
    core_.r[kRa] = entry_;
  }

private:
  Core &core_;
  std::uint32_t entry_;
};

// The retail body's lowered $sp for a frameless override's callees; the entry $sp comes back after.
class GuestFrameScope {
public:
  GuestFrameScope(Core &core, std::uint32_t frame) : core_(core), entry_(core.r[kSp]) {
    core_.r[kSp] = frame;
  }
  GuestFrameScope(const GuestFrameScope &) = delete;
  GuestFrameScope &operator=(const GuestFrameScope &) = delete;
  GuestFrameScope(GuestFrameScope &&) = delete;
  GuestFrameScope &operator=(GuestFrameScope &&) = delete;
  ~GuestFrameScope() {
    core_.r[kSp] = entry_;
  }

private:
  Core &core_;
  std::uint32_t entry_;
};

} // namespace psx::cpu
