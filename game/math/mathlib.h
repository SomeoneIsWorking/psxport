// game/math/mathlib.h — game bitmap-flag bit-test subsystem.
// The trig LUTs live on `class Trig` (game/math/trig.h); the PRNG lives on `class Rng`
// (`rngOf(c).next()`). This header exposes the remaining bitmap-bit-test primitives as
// `class Bit`, an INSTANCE subsystem owned by Engine — the ops read specific per-run game
// progress bitmaps at fixed guest RAM addresses, so this is a subsystem (game flag state),
// not a stateless math/computation library.
#ifndef ENGINE_MATHLIB_H
#define ENGINE_MATHLIB_H
#include <cstdint>
struct Core;

// PROPER OOP: one instance per Core, reached as `eng(c).bit.method(idx, …)`. Back-pointer
// `core` wired once at Core construction time (same pattern as Animation / Collision / Font).
// Was the free functions `ov_bittest_4d7ec` / `ov_bittest_4d868`, taking their MIPS args via
// c->r[4]/r[5] taxi. Now real instance methods with explicit typed arguments; the surface
// returns the masked byte directly instead of via c->r[2].
class Bit {
public:
  Core* core = nullptr;

  // test7EC (guest FUN_8004D7EC): read byte at bitmap[(int16)(idx/8)] then return
  // byte & (1 << ((int16)(idx%8) & 31)). Bitmap base is 0x800BFD34 when (sel&0xff)!=0 else
  // 0x800BFCB4. `bitverify` REPL A/B channel retained.
  uint32_t test7EC(int32_t idx, uint32_t sel);

  // test868 (guest FUN_8004D868): sibling — same bit-test against a fixed third bitmap
  // @0x800BFDB4 (no sel selector). Shares the `bitverify` gate.
  uint32_t test868(int32_t idx);

  // testFE48 (guest FUN_8006EFF4): bit-test on the u32 flag WORD at 0x800BFE48. Returns
  // `(*(u32)0x800BFE48 >> idx) & 1` — the recomp uses `srav v0, v0, a0` so the shift amount is
  // masked to 5 bits by the hardware (idx & 31). Body from disas 0x8006EFF4..0x8006F008.
  uint32_t testFE48(int32_t idx);

  // setFE34 (guest FUN_8006F02C): bit-SET on the u32 flag WORD at 0x800BFE34. Does
  // `*(u32)0x800BFE34 |= (1 << idx)` — `sllv` gives the same idx & 31 masking as above.
  // Body from disas 0x8006F02C..0x8006F048.
  void     setFE34(int32_t idx);

  // setFE48 (guest FUN_8006F00C): bit-SET on the u32 flag WORD at 0x800BFE48 — the SAME word
  // testFE48 polls. Sibling of setFE34: `*(u32)0x800BFE48 |= (1 << idx)`. Body from disas
  // 0x8006F00C..0x8006F02C.
  void     setFE48(int32_t idx);

  // processLinkRequest (guest FUN_8006F04C): arbitrates the pending child-link REQUEST mailbox
  // byte at 0x800BF840 (bit 0x80 = pending, low nibble = slot id 0..8; jump table 0x80016A8C, 9
  // entries, id>=9 falls straight to the miss path). Ids 0/1/6 are RETRY-LIMITED via a per-id
  // byte counter at 0x800BFE3A[id]: while the counter is <3 it just increments and clears the
  // mailbox (no grant); once it reaches 3 the request finally grants (setFE48). Ids 7/8 grant
  // immediately, additionally raising setFE34(id) first. Ids 2..5 (and any id>=9) are silently
  // dropped — no grant. A "grant" = setFE48(id), the exact bit beh_pad_child_linker's testFE48
  // polls before spawning a linked-overlay child (Spawn::spawnOverlayVariant). Every path clears
  // the mailbox byte on exit. Body from disas 0x8006F04C..0x8006F0E0.
  void     processLinkRequest();
};

#endif
