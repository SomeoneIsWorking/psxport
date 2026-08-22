// crt0_verify.h — RE-DERIVE the crt0 boot group FROM THE GUEST'S OWN INSTRUCTIONS and diff it against
// the constants the game shipped in its `GuestProgramImage`.
//
// ═══ WHY THIS EXISTS: THE GATE RULE ═════════════════════════════════════════════════════════════════
// Every crt0 field in GuestProgramImage is a value somebody MEASURED out of the executable and then
// TYPED into the game's derived runtime. Before this file, nothing compared the typed copy to the
// measurement — the shape of defect found independently in four of five ports in this workspace: a
// per-repo tool holds its own copy of the answer and asserts the binary matches THAT, while the port
// holds a second copy, and no code relates the two. Sabotage proved every such gate green with the
// shipped value wrong.
//
// The fix shape required is "the tool PARSES the shipping value and diffs it against the measurement".
// Here that is done in the FRAMEWORK, at boot, against the most authoritative source there is — the
// guest instruction stream — so it reaches EVERY consumer at once and no per-repo table can drift. It
// also retro-covers the six fields that were never gated by anything, not just the ones this change
// adds.
//
// ═══ WHY IT IS NOT FRAGILE: THE PATTERN WAS MEASURED ACROSS FIVE EXECUTABLES ════════════════════════
// Disassembled 2026-08-12 (scratch/crt0_dump.py over each port's extracted boot executable). From the
// stack-top load to the `jal`, all five are the SAME PSY-Q crt0, instruction for instruction, differing
// only in immediates:
//
//   Tomba!2  MAIN.EXE      0x800896E0   bias -8 @0x80089710   heap globals 0x800ABEF8 / 0x800ABEF4
//   Spyro    SCUS_942.28   0x8005B8E0   bias -8 @0x8005B910   heap globals 0x800730C4 / 0x800730C0
//   SpiderM. SLUS_008.75   0x8008739C   bias -8 @0x800873CC   heap globals 0x800B1240 / 0x800B123C
//   Vagrant  SLUS_010.40   0x8001F544   bias -8 @0x8001F574   heap globals 0x80030FB8 / 0x80030FB4
//   MMX4     SLUS_005.61   0x800DAE8C   bias  0 (NO addi)     heap globals: NEITHER — no such store
//
// and in all five the `jal` target's first three words are the same BIOS thunk, `addiu t2,zero,0xA0 /
// jr t2 / addiu t1,zero,0x39` — A-table function 0x39, `InitHeap(ptr, size)`. That is why the a1 fix is
// a fix for every consumer rather than a per-game knob.
//
// ═══ WHAT A NEGATIVE PRINTS (the rule this file was written under) ══════════════════════════════════
// The audit distinguishes THREE outcomes per field and never conflates them:
//   * AGREES     — observed == shipped. Printed with both numbers.
//   * DISAGREES  — observed != shipped, and the observation is resolved. REFUSES the boot.
//   * unresolved — the scan could not decide. Printed as such, WITH the instruction index it got to and
//                  the count it decoded, and does NOT block. An unresolved field is the scanner's
//                  limitation, not evidence about the game, so it must not read as a pass — hence it is
//                  counted and named in the summary line rather than omitted.
// A scan that resolves NOTHING (e.g. the crt0 window reads all zeroes because the guest image is not
// loaded yet) says exactly that, with its denominator, instead of silently reporting "0 disagreements".
#pragma once
#include "crt0_boot.h"
#include "game_iface.h"
#include <lucent/log.h>
#include <stdint.h>
#include <stdio.h>

// How far to scan. All five measured crt0s reach their `jal` within 34 instructions; 96 is generous
// without being unbounded.
static const int CRT0_SCAN_LIMIT = 96;

// ── the symbolic register file ──────────────────────────────────────────────────────────────────────
// Straight-line only: the .bss loop's back-edge is deliberately NOT followed, because every value being
// extracted is established before or after the loop, never by iterating it.
enum Crt0Tag {
  CT_UNKNOWN = 0, // nothing is known about this register
  CT_CONST,       // a resolved constant (lui/addiu/ori/addu chains)
  CT_LOADED,      // the result of `lw` from a resolved address: src = that address, bias = later addi
  CT_HEAPSIZE,    // the `subu` chain result — the value the guest passes as InitHeap's `size`
  CT_HEAPBASE,    // the masked (and possibly KSEG0'd) heap base — InitHeap's `ptr`
};
struct Crt0SReg {
  Crt0Tag tag = CT_UNKNOWN;
  uint32_t val = 0; // valid when tag == CT_CONST (or CT_HEAPBASE, which is also resolved)
  uint32_t src = 0; // valid when tag == CT_LOADED: the absolute address loaded from
  int32_t bias = 0; // valid when tag == CT_LOADED: accumulated `addi` on the loaded value
};

// ── what the scan managed to observe ────────────────────────────────────────────────────────────────
struct Crt0Observed {
  int decoded = 0;                    // DENOMINATOR: instructions decoded before stopping
  const char *stopped = "scan limit"; // why it stopped
  int allZero = 0;                    // words in the window that were 0x00000000 (image not loaded?)

  bool haveBssLo = false;
  uint32_t bssLo = 0;
  bool haveBssHi = false;
  uint32_t bssHi = 0;
  bool haveStackTop = false;
  uint32_t stackTopBase = 0;
  bool haveBias = false;
  int32_t bias = 0; // resolved WITH the stack-top; 0 = no `addi`
  bool haveReserve = false;
  uint32_t stackTopBase2 = 0;
  bool haveHeapBase = false;
  uint32_t heapBase = 0; // UNMASKED, as GuestProgramImage states it
  bool haveGp = false;
  uint32_t gp = 0;
  bool haveLibcInit = false;
  uint32_t libcInit = 0;

  // The two optional absolute stores. `scanComplete` is what makes "no store" a CONCLUSION rather than
  // "I stopped early": it is true only when the scan reached the `jal`, i.e. saw the whole prologue.
  bool scanComplete = false;
  bool sawHeapSizeStore = false;
  uint32_t heapSizePtr = 0;
  bool sawHeapBaseStore = false;
  uint32_t heapBasePtr = 0;

  bool a1LiveAtCall = false;       // r[5] still holds the heap size when the guest reaches its `jal`
  bool a0PlusFour = false;         // the `jal` delay slot is `addi a0,a0,4` (the +4 crt0_plan applies)
  bool libcInitIsInitHeap = false; // libcInit's first 3 words are the A(39h) BIOS thunk
};

// crt0_scan — decode the guest crt0 at `entry`, reading guest words through `rd32(addr)`.
template <class Reader> static inline Crt0Observed crt0_scan(uint32_t entry, Reader rd32) {
  Crt0Observed o;
  Crt0SReg r[32];
  r[0].tag = CT_CONST;
  r[0].val = 0; // $zero

  int spSrcReg = -1; // the register whose CT_LOADED value became sp
  for (int i = 0; i < CRT0_SCAN_LIMIT; i++) {
    const uint32_t pc = entry + 4u * (uint32_t)i;
    const uint32_t w = rd32(pc);
    o.decoded = i + 1;
    if (w == 0) {
      o.allZero++;
      continue;
    } // nop / unloaded memory — both look like this

    const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    const uint32_t sa = (w >> 6) & 31, fn = w & 0x3F;
    const int32_t imm = (int32_t)(int16_t)(uint16_t)(w & 0xFFFF);

    if (op == 0x03) { // jal — the end of the prologue
      o.haveLibcInit = true;
      o.libcInit = ((pc + 4u) & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
      o.a1LiveAtCall = (r[5].tag == CT_HEAPSIZE);
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
      r[rt].tag = CT_CONST;
      r[rt].val = (uint32_t)(w & 0xFFFF) << 16;
      break;
    case 0x08:
    case 0x09:                      // addi / addiu
      if (r[rs].tag == CT_LOADED) { // biasing a loaded word — this is the sp bias
        r[rt] = r[rs];
        r[rt].bias += imm;
      } else if (r[rs].tag == CT_CONST) {
        r[rt].tag = CT_CONST;
        r[rt].val = r[rs].val + (uint32_t)imm;
      } else if (r[rs].tag == CT_HEAPBASE) {
        r[rt] = r[rs];
        r[rt].val += (uint32_t)imm;
      } else {
        r[rt] = Crt0SReg{};
      }
      // $gp is built by `lui gp,hi / addiu gp,gp,lo`, so it is resolved on the second half. A crt0
      // that set gp with a bare `lui` would leave this unresolved and be REPORTED as unresolved,
      // which is the intended behaviour — not silently skipped.
      if (rt == 28 && r[28].tag == CT_CONST) {
        o.haveGp = true;
        o.gp = r[28].val;
      }
      break;
    case 0x0D: // ori
      if (r[rs].tag == CT_CONST) {
        r[rt].tag = CT_CONST;
        r[rt].val = r[rs].val | (w & 0xFFFF);
      } else {
        r[rt] = Crt0SReg{};
      }
      break;
    case 0x23: // lw
      if (r[rs].tag == CT_CONST) {
        r[rt].tag = CT_LOADED;
        r[rt].src = r[rs].val + (uint32_t)imm;
        r[rt].bias = 0;
      } else {
        r[rt] = Crt0SReg{};
      }
      break;
    case 0x2B: // sw
      if (r[rs].tag == CT_CONST) {
        const uint32_t addr = r[rs].val + (uint32_t)imm;
        if (r[rt].tag == CT_CONST && r[rt].val == 0 && !o.haveBssLo) { // `sw zero,0(v0)` — .bss loop
          o.haveBssLo = true;
          o.bssLo = addr;
        } else if (r[rt].tag == CT_HEAPSIZE) {
          o.sawHeapSizeStore = true;
          o.heapSizePtr = addr;
        } else if (r[rt].tag == CT_HEAPBASE) {
          o.sawHeapBaseStore = true;
          o.heapBasePtr = addr;
        }
      }
      break;
    case 0x00:
      switch (fn) {
      case 0x21: // addu
        if (r[rs].tag == CT_CONST && r[rt].tag == CT_CONST) {
          r[rd].tag = CT_CONST;
          r[rd].val = r[rs].val + r[rt].val;
        } else if (r[rs].tag == CT_CONST && r[rs].val == 0) {
          r[rd] = r[rt];
        } else if (r[rt].tag == CT_CONST && r[rt].val == 0) {
          r[rd] = r[rs];
        } else {
          r[rd] = Crt0SReg{};
        }
        break;
      case 0x23: // subu — the heap-size chain
        if (r[rs].tag == CT_LOADED && r[rt].tag == CT_LOADED) {
          // (stack-top word ± bias) - (reserve word): names the SECOND global.
          if (spSrcReg >= 0 && r[rs].src == o.stackTopBase) {
            o.haveReserve = true;
            o.stackTopBase2 = r[rt].src;
          }
          r[rd].tag = CT_HEAPSIZE;
        } else if (r[rs].tag == CT_HEAPSIZE && r[rt].tag == CT_HEAPBASE) {
          r[rd].tag = CT_HEAPSIZE; // …minus the masked heap base
        } else {
          r[rd] = Crt0SReg{};
        }
        break;
      case 0x25:        // or — sp = word|KSEG0, and heapbase|KSEG0
        if (rd == 29) { // $sp
          const int src = (r[rs].tag == CT_LOADED) ? (int)rs : (r[rt].tag == CT_LOADED) ? (int)rt : -1;
          if (src >= 0) {
            spSrcReg = src;
            o.haveStackTop = true;
            o.stackTopBase = r[src].src;
            o.haveBias = true;
            o.bias = r[src].bias;
          }
        }
        if (r[rs].tag == CT_HEAPBASE) {
          r[rd] = r[rs];
        } else if (r[rt].tag == CT_HEAPBASE) {
          r[rd] = r[rt];
        } else if (rd != 29) {
          r[rd] = Crt0SReg{};
        }
        break;
      case 0x00: // sll — the `& 0x1FFFFFFF` mask, first half
        if (r[rt].tag == CT_CONST && sa == 3) {
          o.haveHeapBase = true;
          o.heapBase = r[rt].val; // UNMASKED, which is what GuestProgramImage holds
          r[rd].tag = CT_HEAPBASE;
          r[rd].val = r[rt].val << 3;
        } else {
          r[rd] = Crt0SReg{};
        }
        break;
      case 0x02: // srl — the mask's second half
        if (r[rt].tag == CT_HEAPBASE && sa == 3) {
          r[rd].tag = CT_HEAPBASE;
          r[rd].val = r[rt].val >> 3;
        } else {
          r[rd] = Crt0SReg{};
        }
        break;
      case 0x08:
        o.stopped = "jr";
        return o; // left the prologue without a jal
      case 0x0D:
        o.stopped = "break";
        return o;
      default:
        r[rd] = Crt0SReg{};
        break;
      }
      break;
    default:
      break; // branches and everything else define no tracked reg
    }
    // The .bss loop's terminator is `sltu at, vLo, vHi` (SPECIAL fn 0x2B): the second operand holds the
    // END of the span. Handled here rather than in the switch because it is a READ, not a definition.
    if (op == 0x00 && fn == 0x2B && !o.haveBssHi && r[rt].tag == CT_CONST) {
      o.haveBssHi = true;
      o.bssHi = r[rt].val;
    }
  }
  return o;
}

// ── the diff ────────────────────────────────────────────────────────────────────────────────────────
// One row per field. `resolved` false means the scan could not decide — counted and named, never
// silently treated as agreement.
struct Crt0AuditRow {
  const char *name;
  bool resolved;
  uint32_t observed, shipped;
  bool agree;
};

// crt0_audit — scan the guest crt0 and diff it against the shipped GuestProgramImage + derived plan.
// Returns FALSE only on a CONFIRMED disagreement (the caller must then refuse to boot).
template <class Reader>
static inline bool crt0_audit(const GuestProgramImage *image, const Crt0Plan &p, Reader rd32, const char *who) {
  if (!image || !image->crt0Entry) {
    lucent::warn("crt0",
                 "{}: NOT AUDITED — GuestProgramImage::crt0Entry is 0, so there is no guest crt0 to compare "
                 "the shipped boot group against. 0 of 10 fields were checked; every constant "
                 "below is an UNVERIFIED hand copy. Fill in `crt0Entry` (the PS-EXE entry pc) to turn "
                 "this audit on.",
                 who);
    return true;
  }
  const Crt0Observed o = crt0_scan(image->crt0Entry, rd32);

  // WHICH SIDE EACH ROW COMPARES AGAINST, and why it matters. Where the plan CARRIES the value it is
  // about to apply (bssLo/bssHi/gp/libcInit/the bias), the row is diffed against THE PLAN — so the audit
  // gates the value that actually reaches guest state, not merely the constant the game declared. That
  // distinction is not academic: the pre-fix framework read `cfg->stackBias` nowhere and hardcoded -8, a
  // fault that an image-only audit would have declared clean. The three addresses the plan only reads
  // THROUGH (stackTopWordAddress/stackReserveWordAddress/heapBase) are compared against the image,
  // because a wrong one there shows up as a wrong loaded word, which crt0_plan's own plausibility
  // check catches.
  Crt0AuditRow rows[] = {
      {"bssZeroLo", o.haveBssLo, o.bssLo, p.bssLo, o.bssLo == p.bssLo},
      {"bssZeroHi", o.haveBssHi, o.bssHi, p.bssHi, o.bssHi == p.bssHi},
      {"stackTopWordAddress",
       o.haveStackTop,
       o.stackTopBase,
       image->stackTopWordAddress,
       o.stackTopBase == image->stackTopWordAddress},
      {"stackReserveWordAddress",
       o.haveReserve,
       o.stackTopBase2,
       image->stackReserveWordAddress,
       o.stackTopBase2 == image->stackReserveWordAddress},
      {"heapBase", o.haveHeapBase, o.heapBase, image->heapBase, o.heapBase == image->heapBase},
      {"gp", o.haveGp, o.gp, p.gp, o.gp == p.gp},
      {"libcInit", o.haveLibcInit, o.libcInit, p.libcInit, o.libcInit == p.libcInit},
      // The bias the plan APPLIED, not merely the one the image declared — see the note above.
      {"stackBias(applied)", o.haveBias, (uint32_t)o.bias, (uint32_t)p.stackTopBias, o.bias == p.stackTopBias},
      // The two optional stores are only DECIDABLE once the scan reached the jal — otherwise "no store
      // seen" is indistinguishable from "stopped before the store".
      {"heapSizeStoreAddress",
       o.scanComplete,
       o.heapSizePtr,
       image->heapSizeStoreAddress,
       o.heapSizePtr == image->heapSizeStoreAddress},
      {"heapBaseStoreAddress",
       o.scanComplete,
       o.heapBasePtr,
       image->heapBaseStoreAddress,
       o.heapBasePtr == image->heapBaseStoreAddress},
  };
  const int n = (int)(sizeof rows / sizeof rows[0]);

  int agreed = 0, disagreed = 0, unresolved = 0;
  char bad[512];
  bad[0] = 0;
  char unk[256];
  unk[0] = 0;
  for (int i = 0; i < n; i++) {
    if (!rows[i].resolved) {
      unresolved++;
      size_t at = 0;
      while (at < sizeof unk && unk[at]) {
        at++;
      }
      snprintf(unk + at, sizeof unk - at, "%s%s", at ? ", " : "", rows[i].name);
      continue;
    }
    if (rows[i].agree) {
      agreed++;
      continue;
    }
    disagreed++;
    size_t at = 0;
    while (at < sizeof bad && bad[at]) {
      at++;
    }
    snprintf(bad + at,
             sizeof bad - at,
             "%s%s: guest says 0x%08X, derived runtime ships 0x%08X",
             at ? " | " : "",
             rows[i].name,
             rows[i].observed,
             rows[i].shipped);
  }

  // THE DENOMINATOR, always. `decoded`, `allZero` and where it stopped are what make a clean audit
  // distinguishable from an audit of nothing: a window of unloaded memory decodes as all-zero words and
  // would otherwise resolve no fields and report no disagreements.
  lucent::info("crt0",
               "{}: guest-crt0 AUDIT at 0x{:08X} — {} instruction(s) decoded ({} zero word(s)), "
               "stopped at {}: {} field(s) AGREE, {} DISAGREE, {} unresolved{}{}{}. "
               "libcInit {} the A(39h) InitHeap thunk; a1 {} live at the guest's own jal; "
               "delay slot {} `addi a0,a0,4`.",
               who,
               image->crt0Entry,
               o.decoded,
               o.allZero,
               o.stopped,
               agreed,
               disagreed,
               unresolved,
               unresolved ? " (" : "",
               unresolved ? unk : "",
               unresolved ? ")" : "",
               o.libcInitIsInitHeap ? "IS" : "is NOT",
               o.a1LiveAtCall ? "IS" : "is NOT",
               o.a0PlusFour ? "is" : "is NOT");
  lucent::info("crt0",
               "{}: the plan this audit covers applies sp=0x{:08X} a0=0x{:08X} a1=0x{:X} "
               "libcInit=0x{:08X} — printed next to the audit so the verified CONSTANTS and the "
               "APPLIED values appear in the same log, never one without the other.",
               who,
               p.sp,
               p.a0,
               p.a1,
               p.libcInit);
  if (unresolved) {
    lucent::info("crt0",
                 "{}: the {} unresolved field(s) above are a limit of the SCANNER, not a "
                 "statement about the game — they are NOT verified and must not be read as "
                 "agreeing. Fields: {}.",
                 who,
                 unresolved,
                 unk);
  }
  if (o.decoded && o.allZero == o.decoded) {
    lucent::warn("crt0",
                 "{}: every one of the {} words at 0x{:08X} read as 0x00000000 — the guest image "
                 "is not present there, so this audit examined NOTHING. Do not read the counts "
                 "above as a pass.",
                 who,
                 o.decoded,
                 image->crt0Entry);
  }
  // `a1` is the whole of item 4: if the guest keeps the size live at its own jal, a port that does not
  // set r[5] is passing a stale register. Stated on every boot, for every consumer.
  if (o.scanComplete && o.libcInitIsInitHeap && !o.a1LiveAtCall) {
    lucent::warn("crt0",
                 "{}: the guest reaches libcInit with a1 NOT holding the heap size — this game "
                 "does NOT match the A(39h) InitHeap(ptr,size) shape the framework applies. "
                 "crt0_apply still sets r[5]; verify that is faithful for this title.",
                 who);
  }
  if (disagreed) {
    lucent::error("crt0",
                  "{}: REFUSING TO BOOT — {} of {} crt0 constant(s) shipped in the game's "
                  "GuestProgramImage DISAGREE with the guest's own crt0 at 0x{:08X}: {}."
                  "\n  The guest bytes are the measurement; the runtime image is a hand copy of "
                  "it. Fix the derived runtime (or, if the scanner is wrong, say so with the disassembly — "
                  "runtime/recomp/crt0_verify.h documents the five crt0s it was measured on).",
                  who,
                  disagreed,
                  n,
                  image->crt0Entry,
                  bad);
    return false;
  }
  // A "success" with nothing resolved is not a success, and must not be reported as one.
  if (agreed == 0) {
    lucent::warn("crt0",
                 "{}: the audit resolved ZERO fields, so it PROVED NOTHING about the {} shipped "
                 "constants. Boot continues (this is the scanner's limitation, not a detected "
                 "fault), but treat the boot group as unverified.",
                 who,
                 n);
  }
  return true;
}
