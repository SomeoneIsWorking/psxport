// crt0_boot.h — THE ONE derivation of a PSY-Q crt0's register/memory writes from GameConfig, and the
// ONE place that decides whether a game's boot group is complete enough to run.
//
// WHY THIS FILE EXISTS (megamanx4 docs/issues/0005). `native_boot.cpp::crt0_setup` was a faithful
// transcription of the FIRST consumer's crt0 (Tomba!2, FUN_800896E0) wearing generic clothes. That was
// established by MEASUREMENT, not by reading the C. The extractor is `tools/crt0_extract` (it shares
// `crt0_scan` with the boot-time audit, so the two cannot drift); run over Tomba!2's MAIN.EXE it
// re-derives all NINE values Tomba2Engine's game_config.cpp had recorded independently by hand —
// bssZeroLo/Hi, stackTopBase/2, heapBase, heapSizePtr, heapBasePtr, gp, libcInit — with zero
// disagreements, which is what licenses trusting it on the five executables nobody had hand-RE'd.
// Six of its ten steps generalise. Four did not:
//
//   1. it hardcoded `mem_r32(stackTopBase) - 8`. Tomba!2 / Spyro / Spider-Man / Vagrant Story really do
//      `addi v0,v0,-8`; Mega Man X4's crt0 (0x800DAE8C) has NO bias instruction, so its sp came out
//      0x801FFFF8 instead of 0x80200000 — and because the biased word also feeds the heap-size
//      subtraction, the heap length was wrong by the same 8 bytes. MEASURED over all six executables
//      by `tools/crt0_extract`: it is 4 biasing vs 2 not, because TOY STORY 2 also has no bias
//      instruction (entry 0x80082D60, measured 0). So "-8 is the norm and X4 is the exception" was
//      itself too small a sample — a third of the corpus does not bias.
//   2/3. it stored the heap size and heap base to `heapSizePtr` / `heapBasePtr` UNCONDITIONALLY. X4's
//      crt0 stores them NOWHERE (it passes both in registers; its only absolute store is `sw ra`), so
//      those fields have no honest value — and left at 0, the framework wrote the heap size and base
//      to GUEST ADDRESS 0x00000000 on every boot, silently, injecting two writes the real game never
//      makes and diverging any RAM compare. A write through an unset pointer is the write-time form of
//      "a diagnostic that can print nothing": it cannot fail visibly, so it never gets reported.
//   4. it set only `c->r[4]` before dispatching `libcInit`. Both games' libcInit is the BIOS A(39h)
//      `InitHeap(ptr, size)` thunk and BOTH pass the size in `a1`, so `c->r[5]` was whatever the
//      register file happened to hold. This was a LIVE bug against the shipping consumer, not only a
//      portability gap: hle.cpp's `case 0x39` reads a1 and Hle::heapInit copies it into `heap_size`,
//      which is the arena capacity every BIOS malloc is measured against.
//
// THE SHAPE OF THE FIX. The arithmetic and every completeness decision live HERE, in a pure function
// over (GameConfig, the two guest words it reads). `crt0_setup` became a thin applier that performs no
// arithmetic of its own, so the code the tests exercise IS the code that ships — there is no helper
// beside the shipping path to drift from it.
//
// ABSENT vs UNSET, which is the distinction the old code could not express:
//   * ABSENT   — "this guest crt0 has no such global". heapSizePtr / heapBasePtr == 0. That is the
//                framework's documented meaning of zero everywhere else in GameConfig, and it is a
//                MEASURED answer, so the plan simply does not perform the store, and SAYS SO with the
//                store count it did perform. Never a write to guest 0.
//   * UNSET    — a REQUIRED value nobody has reverse-engineered yet. The plan REFUSES: `ok == false`,
//                one error line naming every missing field AND the denominator (how many were
//                checked), and the caller must not boot. Refusing is the only honest answer, because
//                every remaining alternative fabricates guest state.
//
// WHAT A NEGATIVE PRINTS. `crt0_plan` never returns quietly. A refusal names the fields and the count
// checked; a success prints the whole applied group (sp/gp/a0/a1, the bss span, and which of the two
// optional stores it performed out of how many the game declares). "No store performed" and "I was
// never given an address" are therefore different lines, which is the entire point.
#pragma once
#include "game_iface.h"
#include <lucent/log.h>
#include <stdint.h>
#include <stdio.h> // snprintf

// The largest heap a PSX crt0 can be computing. Main RAM is 2 MiB on retail and 8 MiB on a dev unit,
// so a "heap size" at or above 8 MiB is not a heap size — it is the subtraction underflowing because
// one of the two guest words was never loaded (a zero stack-top word makes `(v0 - v1) - a0` wrap to
// something near 2^32). This is a RAM-map fact, not a tuned threshold: no value in [1, 8 MiB) is
// rejected, so every real consumer's heap (Tomba!2 ~0xF9xxx, X4 0x820C8, Vagrant Story 0x1BBE50)
// passes untouched.
static const uint32_t CRT0_MAX_PLAUSIBLE_HEAP = 0x00800000u;

// Crt0Plan — everything the guest crt0 does, COMPUTED BUT NOT YET APPLIED. `ok == false` means the
// caller must refuse to boot; every other field is then meaningless and left zero.
struct Crt0Plan {
  bool ok = false;

  uint32_t bssLo = 0, bssHi = 0; // .bss clear span, [lo, hi), word-stepped
  uint32_t sp = 0, gp = 0;       // sp (== fp) and gp
  uint32_t a0 = 0, a1 = 0;       // the libcInit arguments: heap base + 4, heap size
  uint32_t libcInit = 0;         // the entry to dispatch once the writes are applied

  // The two OPTIONAL absolute stores. `store*` false = this guest crt0 has no such global (ABSENT);
  // the address is then 0 and must not be written.
  bool storeHeapSize = false;
  uint32_t heapSizeAddr = 0;
  bool storeHeapBase = false;
  uint32_t heapBaseAddr = 0;

  // Intermediate values, kept because they are what a boot log needs to be readable.
  uint32_t heapBaseMasked = 0; // heapBase & 0x1FFFFFFF
  uint32_t stackTopWord = 0;   // mem[stackTopBase], as read
  int32_t stackTopBias = 0;    // the guest crt0's own adjustment of that word
};

// The required fields, as (name, value) pairs. One list, walked by the checker AND printed in the
// refusal, so a field can never be required-but-unnamed or named-but-unchecked.
struct Crt0Required {
  const char *name;
  uint32_t value;
};

// crt0_plan — derive the plan, or refuse.
//
// `stackTopWord` / `stackReserveWord` are the two words the guest crt0 LOADS (mem[stackTopBase] and
// mem[stackTopBase2]); they are passed in rather than read here so this function is pure and can be
// exercised hermetically against the exact words a real boot saw.
//
// `who` names the caller in every emitted line (a boot has more than one Core: SBS boots two).
static inline Crt0Plan
crt0_plan(const GameConfig *cfg, uint32_t stackTopWord, uint32_t stackReserveWord, const char *who) {
  Crt0Plan p;
  if (!cfg) {
    lucent::error("crt0",
                  "{}: NO GameConfig installed — the boot group could not be read at all, so "
                  "0 of 8 required fields were checked. Refusing to run a crt0 (the previous "
                  "behaviour was to clear a zero-length .bss and write the heap base to guest "
                  "0x00000000).",
                  who);
    return p;
  }

  // ── the REQUIRED group ──────────────────────────────────────────────────────────────────────────
  // Every one of these is an address in guest RAM (0x8xxxxxxx) or, for the bias declaration, an
  // explicit flag, so ZERO is unambiguously "not reverse-engineered yet" and never a real value.
  const Crt0Required req[] = {
      {"bssZeroLo", cfg->bssZeroLo},
      {"bssZeroHi", cfg->bssZeroHi},
      {"stackTopBase", cfg->stackTopBase},
      {"stackTopBase2", cfg->stackTopBase2},
      {"heapBase", cfg->heapBase},
      {"gp", cfg->gp},
      {"libcInit", cfg->libcInit},
      // The stack-top bias CANNOT use zero-means-absent: 0 is X4's real, measured value. So the
      // declaration is explicit and separate from the value, and an undeclared bias refuses rather than
      // inheriting the stock PSY-Q -8. Defaulting to -8 would be right for four of the five ports in
      // this workspace and silently 8 bytes wrong for the fifth — which is precisely the class of bug
      // this file exists to end.
      {"stackBias.declared", cfg->stackBias.declared},
  };
  const int n_req = (int)(sizeof req / sizeof req[0]);
  char missing[256];
  missing[0] = 0;
  int n_missing = 0;
  for (int i = 0; i < n_req; i++) {
    if (req[i].value) {
      continue;
    }
    n_missing++;
    size_t at = 0;
    while (at < sizeof missing && missing[at]) {
      at++;
    }
    snprintf(missing + at, sizeof missing - at, "%s%s", at ? ", " : "", req[i].name);
  }
  if (n_missing) {
    lucent::error("crt0",
                  "{}: REFUSING TO BOOT — {} of {} required boot-group field(s) are UNSET: {}."
                  "\n  This is an un-reverse-engineered crt0, not a framework failure. The "
                  "group is consumed AS A GROUP: running it half-filled clears a wrong .bss "
                  "span, builds sp from mem[0], and hands InitHeap a garbage size."
                  "\n  Fill them in the game's GameConfig (game/core/game_config.cpp) with a "
                  "citation per field. `stackBias` must be stated explicitly even when the "
                  "measured bias is 0 — see crt0_boot.h.",
                  who,
                  n_missing,
                  n_req,
                  missing);
    return p;
  }

  // ── relations that must hold for the group to describe a crt0 at all ────────────────────────────
  if (cfg->bssZeroHi <= cfg->bssZeroLo || (cfg->bssZeroLo & 3u) || (cfg->bssZeroHi & 3u)) {
    lucent::error("crt0",
                  "{}: REFUSING — .bss clear span [0x{:08X}, 0x{:08X}) is not a word-aligned "
                  "non-empty range. All {} required fields are set, so this is a WRONG value, "
                  "not a missing one.",
                  who,
                  cfg->bssZeroLo,
                  cfg->bssZeroHi,
                  n_req);
    return p;
  }

  // ── the arithmetic, in the guest's own order ─────────────────────────────────────────────────────
  //   v0 = mem[stackTopBase] (+ bias)      sp = fp = v0 | 0x80000000
  //   a0 = heapBase & 0x1FFFFFFF           a1 = (v0 - mem[stackTopBase2]) - a0
  //   a0 |= 0x80000000                     libcInit(a0 + 4, a1)
  const int32_t bias = cfg->stackBias.value;
  const uint32_t v0 = (uint32_t)((int64_t)stackTopWord + bias);
  const uint32_t a0m = cfg->heapBase & 0x1FFFFFFFu;
  const uint32_t hsz = (v0 - stackReserveWord) - a0m;

  if (hsz == 0 || hsz >= CRT0_MAX_PLAUSIBLE_HEAP) {
    lucent::error("crt0",
                  "{}: REFUSING — computed heap size 0x{:X} is not a heap size (0 or >= "
                  "0x{:X}, the largest PSX main RAM). ((mem[0x{:08X}]=0x{:08X} {} {}) - "
                  "mem[0x{:08X}]=0x{:08X}) - 0x{:08X}. Every required field is set, so one of "
                  "the two loaded WORDS is wrong: the usual cause is a stackTopBase that does "
                  "not name the stack-top global, whose word then reads 0 and underflows this "
                  "subtraction. InitHeap would be handed this value as its arena capacity.",
                  who,
                  hsz,
                  CRT0_MAX_PLAUSIBLE_HEAP,
                  cfg->stackTopBase,
                  stackTopWord,
                  bias < 0 ? "-" : "+",
                  bias < 0 ? -(int64_t)bias : (int64_t)bias,
                  cfg->stackTopBase2,
                  stackReserveWord,
                  a0m);
    return p;
  }

  p.ok = true;
  p.bssLo = cfg->bssZeroLo;
  p.bssHi = cfg->bssZeroHi;
  p.stackTopWord = stackTopWord;
  p.stackTopBias = bias;
  p.sp = v0 | 0x80000000u;
  p.gp = cfg->gp;
  p.heapBaseMasked = a0m;
  p.a0 = (a0m | 0x80000000u) + 4u;
  p.a1 = hsz;
  p.libcInit = cfg->libcInit;
  p.storeHeapSize = cfg->heapSizePtr != 0u;
  p.heapSizeAddr = cfg->heapSizePtr;
  p.storeHeapBase = cfg->heapBasePtr != 0u;
  p.heapBaseAddr = cfg->heapBasePtr;

  // The POSITIVE report. Printed at info on every boot, not behind a debug channel, because every
  // number here is a guest fact that decides whether the game runs: an 8-byte sp error and a wrong
  // InitHeap capacity both boot fine and fail somewhere else entirely. The store line states the
  // denominator ("1 of 2 declared") so "stored nothing" is distinguishable from "was never asked to".
  const int n_stores = (p.storeHeapSize ? 1 : 0) + (p.storeHeapBase ? 1 : 0);
  lucent::info("crt0",
               "{}: bss [0x{:08X},0x{:08X}) {} bytes | sp=fp=0x{:08X} (mem[0x{:08X}]=0x{:08X} "
               "bias {}) | gp=0x{:08X} | heap base 0x{:08X} size 0x{:X} | libcInit 0x{:08X}"
               "(a0=0x{:08X}, a1=0x{:X}) | {} of 2 declared absolute store(s)",
               who,
               p.bssLo,
               p.bssHi,
               p.bssHi - p.bssLo,
               p.sp,
               cfg->stackTopBase,
               stackTopWord,
               bias,
               p.gp,
               a0m | 0x80000000u,
               p.a1,
               p.libcInit,
               p.a0,
               p.a1,
               n_stores);
  if (n_stores != 2) {
    lucent::info("crt0",
                 "{}: heap-size global {} / heap-base global {} — this crt0 keeps {} in "
                 "REGISTERS ONLY, so no store is performed. That is a MEASURED absence "
                 "(GameConfig leaves the pointer 0), not a missing address: the framework used "
                 "to write these to guest 0x00000000.",
                 who,
                 p.storeHeapSize ? "present" : "ABSENT",
                 p.storeHeapBase ? "present" : "ABSENT",
                 n_stores == 0 ? "both" : "one of them");
  }
  return p;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// crt0_apply — APPLY a plan. Also here, and also templated, for ONE reason: the defect this file
// exists to fix was not in the arithmetic, it was in the APPLICATION — `mem_w32(cfg->heapSizePtr, …)`
// executed unconditionally with `heapSizePtr == 0`. A test that only checked the plan would assert
// `storeHeapSize == false` and still not know whether the caller honoured it. So the write SEQUENCE is
// generated here, over an abstract writer, and the hermetic test drives it with a RECORDING writer that
// asserts the set of addresses written — i.e. "no write to guest 0" is a checked postcondition of the
// shipping code path rather than a property a human read off `native_boot.cpp`.
//
// `Writer` must provide:
//   void w32(uint32_t addr, uint32_t val);   // a guest word store
//   void reg(int idx, uint32_t val);         // a guest register assignment
//   void call(uint32_t entry);               // dispatch the guest libcInit
//
// Returns false (writing NOTHING) when the plan was refused, so a caller that forgot to check `ok`
// still cannot fabricate guest state.
template <class Writer> static inline bool crt0_apply(const Crt0Plan &p, Writer &w) {
  if (!p.ok) {
    return false;
  }
  for (uint32_t a = p.bssLo; a < p.bssHi; a += 4) {
    w.w32(a, 0); // .bss clear
  }
  if (p.storeHeapSize) {
    w.w32(p.heapSizeAddr, p.a1); // heap-size global, IF ANY
  }
  if (p.storeHeapBase) {
    w.w32(p.heapBaseAddr, p.heapBaseMasked | 0x80000000u); // heap-base global, IF ANY
  }
  w.reg(28, p.gp); // gp
  w.reg(29, p.sp);
  w.reg(30, p.sp); // sp, fp
  w.reg(4, p.a0);  // a0 = heap base|KSEG0 + 4
  w.reg(5, p.a1);  // a1 = heap size — WAS MISSING
  w.call(p.libcInit);
  return true;
}
