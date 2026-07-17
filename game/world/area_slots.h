// game/world/area_slots.h — PC-native AREA-SLOT state machine.
//
// PROPER OOP: one instance per Core, embedded on Engine, reached as
// `eng(c).areaSlots.method(args)`. Back-pointer wired at Core construction (same pattern as
// Spawn / SceneEvents).
//
// SCOPE: the 24-entry × 12-byte area-slot table at 0x800BE238 + its armed-mask at 0x800BE358 —
// the per-frame update tail (FUN_80075A80) and the single-slot ack primitive (FUN_80074AF0).
#pragma once
#include <cstdint>
struct Core;

class AreaSlots {
public:
  Core* core = nullptr;

  // updateTail: the last direct child of ov_field_frame's gameplay block — per-frame area-slot
  //   state machine at guest 0x80075A80. Iterates a 24-entry × 12-byte slot table at 0x800BE238
  //   keyed by the counter at 0x800BED78; per slot the kind byte drives one of three arms:
  //     kind == 0    -> skip to the buf[slot] post-check (nothing to do this frame).
  //     kind == 0xFF -> fire the action leaf FUN_80092660(slot_s16, hword_g, sub1, sub2, [3 stacked
  //                     bytes]) using either the g_a4f7e hword (top bit of sub2 set, arm-hi) or
  //                     the 0xBED84 hword (arm-lo), clear the "armed" bit in the 24-bit mask at
  //                     0x800BE358, then decrement kind.
  //     other        -> if slot[7] == 4, decrement kind; if it went to 0, SET the bit in 0x800BE358
  //                     and zero slot[1]/slot[2]. Else just decrement kind.
  //   Then a per-slot post-check reads buf[slot] filled by FUN_800998e4 at entry (0=zero slot[1];
  //   3=fall through unchanged; other=fall through). Tail: if 0x800BE358 nonzero call FUN_80098F90(0)
  //   + clear it; then FUN_80075824(0x800BE1F8), FUN_80099490(0x800BE1F8); if key2 = mem_r16s(0x800BED80)
  //   != -1, clear 0x800BE1F8, look up hword table[key2].w0 at 0x800BE368+key2*8, call FUN_8008E0C0
  //   with that hword and 0 — if it returns nonzero on the low16, fall to epilogue; else read the
  //   sub-object id at 0x800BE22A, if zero call FUN_80074E48 else call FUN_80074BF8(id) and clear
  //   both 0x800BED80 and 0x800BE22A. All 8 callees stay substrate. Replaces `d0(c, 0x80075a80u)`.
  void updateTail();

  // ackIfMatch(arg): FUN_80074AF0 — SIGNATURE-MATCHED slot ack primitive against the same
  // 24-entry × 12-byte slot table at 0x800BE238 that updateTail iterates. The `arg` carries the
  // entry index in its low byte (arg & 0xFF) plus a 3-byte SIGNATURE in the high bytes; if the high
  // 3 bytes match the u32 stored at slot[idx].w0, this method sets the "armed" bit at
  // *(u32)0x800BE358 |= (1 << idx) AND clears the trigger-pending byte at slot[idx].b1.
  // Mismatched signatures are a no-op. RE'd from disas 0x80074AF0..0x80074B40.
  void ackIfMatch(uint32_t arg);

  // primeCountdown(idx): FUN_80074A38 — sets slot[idx].kind (byte 0) to 10 unconditionally (a
  // 10-frame countdown; updateTail's "other-kind" arm decrements it each frame and, once it hits 0
  // with slot[8]==4, sets the armed bit). Pure 1-store leaf. RE'd via Ghidra headless
  // (scratch/decomp/cluster1.c: FUN_80074a38).
  void primeCountdown(uint32_t idx);

  // updateCell(sigArg, dx, dy): FUN_8007496C — SIGNATURE-MATCHED grid-cell tracker over the same
  // slot table ackIfMatch uses: idx = sigArg & 0xFF; if the high 3 bytes of sigArg don't match the
  // u32 stored at slot[idx].w0, no-op (return false). On match: subtract a fixed global cell offset
  // (the byte at 0x800FB165, scaled ×16) from dx/dy, clamp both to [0,127], store the clamped
  // cell coords into slot[idx]+6/+7, and notify FUN_80092E3C(idx, dx_cell<<7, dy_cell<<7) (kept
  // substrate — a still-unRE'd notifier outside this band). RE'd via Ghidra headless
  // (scratch/decomp/cluster1.c: FUN_8007496c).
  bool updateCell(uint32_t sigArg, int32_t dx, int32_t dy);

  // registerOverrides(): wires FUN_80074A38 / FUN_8007496C into the override registry
  // (overrides::install) — both are ONLY ever reached via an indirect rec_dispatch (no static
  // `func_<addr>(c)` call site in the recompiled output), so no shard_set_override is needed.
  void registerOverrides();
};
