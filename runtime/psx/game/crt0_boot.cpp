// crt0_boot.cpp — planCrt0's arithmetic and Crt0Boot, which binds it to a Core.
#include "crt0_boot.h"

#include "core.h"
#include "guest_call.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::game {

Crt0Plan planCrt0(const Crt0Scan &scan, std::uint32_t stackTopWord, std::uint32_t stackReserveWord, const char *who) {
  Crt0Plan p;
  const std::string unresolved = scan.unresolved();
  if (!unresolved.empty()) {
    lucent::error("crt0",
                  "{}: refusing: the scan stopped on {} after {} instruction(s) ({} zero) without resolving {}",
                  who,
                  scan.stopped,
                  scan.decoded,
                  scan.allZero,
                  unresolved);
    return p;
  }
  if (scan.bssHi <= scan.bssLo || (scan.bssLo & 3u) != 0u || (scan.bssHi & 3u) != 0u) {
    lucent::error("crt0", "{}: refusing: .bss [0x{:08X}, 0x{:08X}) is not a word-aligned span", who, scan.bssLo, scan.bssHi);
    return p;
  }
  // v0 = mem[stackTop] + bias; sp = fp = v0 | KSEG0; a1 = (v0 - mem[reserve]) - (heapBase & 0x1FFFFFFF).
  const auto v0 = static_cast<std::uint32_t>(static_cast<std::int64_t>(stackTopWord) + scan.bias);
  const std::uint32_t heapBaseMasked = scan.heapBase & 0x1FFFFFFFu;
  const std::uint32_t heapSize = (v0 - stackReserveWord) - heapBaseMasked;
  if (heapSize == 0 || heapSize >= kCrt0MaxPlausibleHeap) {
    lucent::error("crt0",
                  "{}: refusing: heap size 0x{:X} from mem[0x{:08X}]=0x{:08X} bias {} - mem[0x{:08X}]=0x{:08X} - 0x{:08X}",
                  who,
                  heapSize,
                  scan.stackTopBase,
                  stackTopWord,
                  scan.bias,
                  scan.stackTopBase2,
                  stackReserveWord,
                  heapBaseMasked);
    return p;
  }
  p.ok = true;
  p.bssLo = scan.bssLo;
  p.bssHi = scan.bssHi;
  p.sp = v0 | 0x80000000u;
  p.gp = scan.gp;
  p.heapBaseMasked = heapBaseMasked;
  p.a0 = (heapBaseMasked | 0x80000000u) + 4u;
  p.a1 = heapSize;
  p.libcInit = scan.libcInit;
  p.storeHeapSize = scan.sawHeapSizeStore;
  p.heapSizeAddr = scan.heapSizePtr;
  p.storeHeapBase = scan.sawHeapBaseStore;
  p.heapBaseAddr = scan.heapBasePtr;
  lucent::info("crt0",
               "{}: bss [0x{:08X},0x{:08X}) | sp=fp=0x{:08X} (mem[0x{:08X}]=0x{:08X} bias {}) | gp=0x{:08X} | "
               "libcInit 0x{:08X}(a0=0x{:08X}, a1=0x{:X}; InitHeap thunk {}, a1 live {}) | heap-size store {} | "
               "heap-base store {}",
               who,
               p.bssLo,
               p.bssHi,
               p.sp,
               scan.stackTopBase,
               stackTopWord,
               scan.bias,
               p.gp,
               p.libcInit,
               p.a0,
               p.a1,
               scan.libcInitIsInitHeap ? "yes" : "no",
               scan.a1LiveAtCall ? "yes" : "no",
               p.storeHeapSize ? lucent::format("0x{:08X}", p.heapSizeAddr) : std::string("none"),
               p.storeHeapBase ? lucent::format("0x{:08X}", p.heapBaseAddr) : std::string("none"));
  return p;
}

void Crt0Boot::run(Core &core) const {
  const Crt0Scan scan = scanCrt0(image_.entry, [&core](std::uint32_t address) {
    return core.mem_r32(address);
  });
  const Crt0Plan p = planCrt0(scan, core.mem_r32(scan.stackTopBase), core.mem_r32(scan.stackTopBase2), "Crt0Boot");
  if (!p.ok) {
    lucent::error("crt0", "boot aborted at crt0 0x{:08X}; no guest state was modified", image_.entry);
    std::exit(1);
  }
  struct CoreWriter {
    Core &c;
    void w32(std::uint32_t a, std::uint32_t v) {
      c.mem_w32(a, v);
    }
    void reg(int i, std::uint32_t v) {
      c.r[i] = v;
    }
    void call(std::uint32_t entry) {
      psx::cpu::callGuestNow(c, "native crt0 libc initialization", entry);
    }
  } w{core};
  applyCrt0(p, w);
}

} // namespace psx::game
