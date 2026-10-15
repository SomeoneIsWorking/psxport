// crt0_boot.cpp — Crt0Boot: the thin applier that binds the derived crt0 plan to a Core.
#include "crt0_boot.h"

#include "core.h"
#include "crt0_verify.h"
#include "guest_call.h"

#include <cstdlib>

#include <lucent/log.h>

namespace psx::game {

void Crt0Boot::run(Core &core) const {
  const Crt0Image *image = &image_;
  // The two words the guest crt0 loads. Read before the .bss clear, exactly as the guest does — and
  // read through the plan's inputs rather than inside it, so the plan stays pure and testable.
  const uint32_t stackTopWord = core.mem_r32(image->stackTopWordAddress);
  const uint32_t reserveWord = core.mem_r32(image->stackReserveWordAddress);
  const Crt0Plan p = psx::game::planCrt0(image, stackTopWord, reserveWord, "Crt0Boot");
  if (!p.ok) {
    // psx::game::planCrt0 has already named the missing fields and its denominator. Refuse before mutating any
    // guest state.
    lucent::error("crt0",
                  "boot ABORTED: the game's crt0 boot group is incomplete (see above). No "
                  "guest state has been modified.");
    exit(1);
  }
  // CROSS-CHECK THE SHIPPED CONSTANTS AGAINST THE GUEST'S OWN crt0 BYTES, before applying any of them.
  // This is the gate that was missing: every field above is a MEASURED value hand-copied into the game's
  // derived runtime, and nothing compared the copy to the measurement. psx::game::auditCrt0 re-derives the group
  // from the instruction stream at image->crt0Entry and refuses a CONFIRMED disagreement (crt0_verify.h).
  if (!psx::game::auditCrt0(
          image,
          p,
          [&core](uint32_t a) {
            return core.mem_r32(a);
          },
          "Crt0Boot")) {
    lucent::error("crt0",
                  "boot ABORTED: the shipped crt0 boot group DISAGREES with the guest's own "
                  "crt0 (see above). No guest state has been modified.");
    exit(1);
  }
  // a1 = heap size USED TO BE MISSING: libcInit is the BIOS A(39h) InitHeap(ptr, size) thunk in every
  // consumer measured so far, and hle.cpp's `case 0x39` copies a1 straight into Hle::heap_size — the
  // capacity every BIOS malloc is checked against. Log the incoming register value beside the
  // measured capacity so the diagnostic exposes a caller that failed to initialize a1.
  lucent::info("crt0",
               "libcInit 0x{:08X}: a0=0x{:08X} a1=0x{:X} (a1 held 0x{:08X} = {} before crt0 set "
               "it — that stale value is the heap capacity this port passed to InitHeap before "
               "the r[5] fix; a difference here is the bug's blast radius)",
               p.libcInit,
               p.a0,
               p.a1,
               core.r[5],
               core.r[5]);
  // The write sequence itself comes from psx::game::applyCrt0, NOT from lines here: the defect was in the
  // application (an unconditional store through a zero pointer), so the sequence lives in the tested
  // header and this is only the adapter that binds it to a Core.
  struct CoreWriter {
    Core &c;
    void w32(uint32_t a, uint32_t v) {
      c.mem_w32(a, v);
    }
    void reg(int i, uint32_t v) {
      c.r[i] = v;
    }
    void call(uint32_t entry) {
      psx::cpu::dispatchGuestToReturn0(
          c, entry, psx::cpu::ExecutionBudget::currentTurn(c), "native crt0 libc initialization");
    }
  } w{core};
  psx::game::applyCrt0(p, w);
}

} // namespace psx::game
