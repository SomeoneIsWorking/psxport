// crt0_boot.h — a title's PSY-Q crt0 replayed natively: the boot group comes from scanCrt0 over the guest's own
// prologue, planCrt0 is the arithmetic, applyCrt0 the write sequence.
#pragma once

#include "crt0_scan.h"

#include <cstdint>

class Core;

namespace psx::game {

// The executable's crt0 entry; everything else is decoded from the guest bytes there.
struct Crt0Image {
  std::uint32_t entry = 0;
};

// A heap at or above 8 MiB (dev-unit RAM) is the subtraction underflowing on an unloaded word.
inline constexpr std::uint32_t kCrt0MaxPlausibleHeap = 0x00800000u;

// The crt0's writes, computed but not applied. `ok == false` refuses the boot.
struct Crt0Plan {
  bool ok = false;
  std::uint32_t bssLo = 0; // [bssLo, bssHi), word-stepped
  std::uint32_t bssHi = 0;
  std::uint32_t sp = 0; // sp and fp
  std::uint32_t gp = 0;
  std::uint32_t a0 = 0; // InitHeap(heap base | KSEG0 + 4, heap size)
  std::uint32_t a1 = 0;
  std::uint32_t libcInit = 0;
  bool storeHeapSize = false;
  std::uint32_t heapSizeAddr = 0;
  bool storeHeapBase = false;
  std::uint32_t heapBaseAddr = 0;
  std::uint32_t heapBaseMasked = 0;
};

// `stackTopWord` and `stackReserveWord` are the two guest words the crt0 loads; `who` names the caller in the log.
Crt0Plan planCrt0(const Crt0Scan &scan, std::uint32_t stackTopWord, std::uint32_t stackReserveWord, const char *who);

// Writes nothing for a refused plan. `Writer` provides w32(address, value), reg(index, value) and call(entry).
template <class Writer> bool applyCrt0(const Crt0Plan &p, Writer &w) {
  if (!p.ok) {
    return false;
  }
  for (std::uint32_t a = p.bssLo; a < p.bssHi; a += 4) {
    w.w32(a, 0);
  }
  if (p.storeHeapSize) {
    w.w32(p.heapSizeAddr, p.a1);
  }
  if (p.storeHeapBase) {
    w.w32(p.heapBaseAddr, p.heapBaseMasked | 0x80000000u);
  }
  w.reg(28, p.gp);
  w.reg(29, p.sp);
  w.reg(30, p.sp);
  w.reg(4, p.a0);
  w.reg(5, p.a1);
  w.call(p.libcInit);
  return true;
}

// Scans the crt0 at the image's entry in guest RAM, refuses a group it cannot resolve, then applies the plan
// and calls libcInit.
class Crt0Boot {
public:
  explicit Crt0Boot(const Crt0Image &image) : image_(image) {}
  void run(Core &core) const;

private:
  Crt0Image image_;
};

} // namespace psx::game
