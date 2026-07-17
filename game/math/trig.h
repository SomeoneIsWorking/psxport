// class Trig — PC-native libgte trig helpers (rsin, rcos, ratan2, angleCmp).
//
// PROPER OOP: instance subsystem, one per Core (embedded as `Core::trig`). Back-pointer wired in
// Core::Core(). Callers reach the methods as `trigOf(c).rsin(angle)` — no Core* on the method
// surface; the class reads the guest sin/atan LUTs via `this->core`. Same shape as `Rng`. Was
// previously a static-methods "library" (Trig::rsin(c, angle)); promoted per the class-instance
// arc — instance-with-back-pointer replaces the `A::B(core, ...)` shape.
//
// The angle unit is the PSX 12-bit angle where 4096 == 2π; results are Q12 fixed (in [-4096, 4096]
// for sin/cos). The sine table lives at guest 0x800A5AF0 in MAIN.EXE .rodata (1024 int16 entries
// for sin(0..π/2)). Methods read that table via the Core so state stays consistent with any
// still-substrate reader.
#pragma once
#include <cstdint>
class Core;

class Trig {
public:
  static constexpr uint32_t SIN_TAB = 0x800A5AF0u;
  static constexpr uint32_t ATAN_TAB = 0x800AA490u;   // 1025 int16, atan(k/1024)*4096/2π for k=0..1024

  Core* core = nullptr;

  // rsin (guest FUN_80083E80): sin(angle12) -> Q12 in [-4096, 4096].
  int32_t rsin(int32_t angle) const;
  // rcos (guest FUN_80083F50): cos(angle12) -> Q12.
  int32_t rcos(int32_t angle) const;
  // ratan2 (guest FUN_80085690): angle12 of vector (y, x). MIPS calling convention: first arg is y, second is x.
  //   Result is a 12-bit signed angle where 4096 == 2π; caller usually casts to int16 for wrap.
  //   Mirrors the guest byte-for-byte: sign-strip, first-octant reduction (|small|/|big| or a large-input
  //   path that reduces |big| by 10 bits first to keep the divisor in range), table[q] lookup, then quadrant
  //   fixup (2048 - v for x_neg, negate for y_neg).
  int32_t ratan2(int32_t y, int32_t x) const;

  // angleCmp (guest FUN_80077768): angle-signed-half compare. Returns 1 iff `(a-b-1024) & 0xFFF`
  //   sits in the first half [0, 2048) of the 12-bit angle circle (when mode==0), OR in the second
  //   half [2048, 4096) (when mode!=0). A "does angle a lead angle b by π/4..3π/4" test used by the
  //   scripted-camera path (CutsceneCamera::snapFollowA). Pure computation over int32 inputs — no
  //   Core access, kept static.
  static int32_t angleCmp(int32_t a, int32_t b, int32_t mode);

  // Wire rsin/ratan2 as the guest-address overrides (0x80083E80/80085690) so the rec_dispatch/
  // guest_leaf + direct substrate func_<addr>(c) callers run these ports instead of the emulated GTE
  // leaves. These read the SAME guest tables (SIN_TAB/ATAN_TAB) as the substrate, so byte-exact;
  // MIRROR_VERIFY-gated (102593/79617 passes). angleCmp (0x80077768) deferred — not exercised by
  // current replays (cutscene-camera caller). Found by codemap --substrate-fallthrough.
  static void registerOverrides(class Game* game);
};
