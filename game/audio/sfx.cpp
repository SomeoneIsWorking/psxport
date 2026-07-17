// game/audio/sfx.cpp — Sfx::trigger — PC-native port of FUN_80074590.
//
// RE'd verbatim from disas 0x80074590..0x80074808 + the 16-entry jumptable at 0x80016C04. Preserves
// all int16 truncations, sign-extension patterns, and the signed-division-by-9 magic (0x38E38E39,
// the MIPS mflo-mfhi-sra dance) that the recomp uses to compute the SPU pitch. Three substrate
// callees kept reachable via rec_dispatch: FUN_80074BF8 (music/track control leaf, targeted by the
// id 112..124 remaps), FUN_80074EEC (menu/UI SFX, targeted by id 127), FUN_80075E04 (SPU voice-fire
// tail — the actual note-on into the audio driver).

#include "audio/sfx.h"
#include "game_ctx.h"
#include "core.h"
#include "override_registry.h"   // overrides::install — the one native-override registry

void rec_dispatch(Core*, uint32_t);
void func_80074590(Core*);        // generated/shard_disp.c — the SFX firing primitive (substrate)

void Sfx::trigger(int id, int pan, int pitchBend) {
  Core* c = core;

  // Prologue: sp adjust + ra save (mirrors the recomp so any downstream leaf reading its own
  // stack args lands on the right offsets).
  const uint32_t sp_save = c->r[29];
  const uint32_t ra_save = c->r[31];
  c->r[29] = sp_save - 40u;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 32u, ra_save);               // gen prologue: sw ra,32(sp) (abi_extract contract)

  const uint32_t idb = (uint32_t)(id & 0xFF);

  // ---- JT PATH (id 112..127): recomp lines 0x800745A8..0x800745DC + 16 handlers at 0x800745E0.. ----
  if ((idb & 0x80u) == 0 && idb >= 112u) {
    // The 12 remap entries (jt[0..12] except jt[13/14]) all funnel into `jal FUN_80074BF8(mapped_id)
    // ; j 0x80074800`. jt[9] remaps to 13 (out of order — everything else is monotonic).
    //   jt[0..8]  : ids 2, 3, 4, 5, 6, 7, 10, 11, 12         (a0=112..120)
    //   jt[9]     : id  13                                    (a0=121)
    //   jt[10..12]: ids 10, 11, 12  (dup — a0=122..124 alias to a0=118..120)
    //   jt[13/14] : error path (silent)                        (a0=125/126)
    //   jt[15]    : jal FUN_80074EEC — different leaf          (a0=127)
    static constexpr int8_t remap[16] = { 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 10, 11, 12, -1, -1, -1 };
    const uint32_t idx = idb - 112u;
    if (idx == 15u) {
      rec_dispatch(c, 0x80074EECu);                    // FUN_80074EEC — menu/UI SFX leaf
    } else if (remap[idx] >= 0) {
      c->r[4] = (uint32_t)(int32_t)remap[idx];
      rec_dispatch(c, 0x80074BF8u);                    // FUN_80074BF8 — music/track leaf
    }
    // idx == 13 or 14: silent error path (recomp jumps to 0x800746D8 which returns v0=0).
    c->r[29] = sp_save; c->r[31] = ra_save;
    return;
  }

  // ---- PATH SELECTION: t1 = &fx_table[id], plus the flag byte a3 (0 for path B, 128 for A) ----
  uint32_t t1;
  int32_t  a3_flag;
  if ((idb & 0x80u) != 0) {
    // PATH A (id 128..224): per-area indirection.
    if (idb >= 225u) {                                 // 225..255 → silent error (recomp's sltiu 225)
      c->r[29] = sp_save; c->r[31] = ra_save;
      return;
    }
    const uint8_t  area = c->mem_r8(0x800BF870u);
    const uint32_t area_table_ptr = c->mem_r32(0x800A4EF8u + (uint32_t)area * 4u);
    t1 = area_table_ptr + (idb & 0x7Fu) * 8u;
    a3_flag = 128;
  } else {
    // PATH B (id 0..111): fixed table at 0x800A4D18.
    t1 = 0x800A4D18u + idb * 8u;
    a3_flag = 0;
  }

  // ---- PAUSE / KIND MARKER ----
  // The recomp's t0 register (which ends up in a0 for the voice-fire) resolves to:
  //   kind == 4         → t0 = 4  (fx passes through even when paused)
  //   paused (!= 4)     → t0 = 1  (marker: SPU driver knows this fired under pause)
  //   not paused (!= 4) → t0 = kind (untouched)
  const uint8_t kind    = c->mem_r8(t1);
  const bool    paused  = (c->mem_r8(0x1F800137u) != 0);
  const uint32_t t0_final = (kind == 4u) ? 4u : (paused ? 1u : (uint32_t)kind);

  // ---- PITCH BEND MATH ----
  // (byte6 + pitchBend) × global_scale / 9, clamped [0, 127]. Faithful to the recomp's int16
  // truncations and signed-div-by-9-via-0x38E38E39 magic.
  const int32_t byte6_ext = (int32_t)(int8_t)c->mem_r8(t1 + 6u);
  const int32_t pitchBend_ext = (int32_t)(int8_t)(pitchBend & 0xFF);
  const uint8_t global_scale = c->mem_r8(0x800FB165u);
  int32_t pitch_final;
  if (global_scale == 0) {
    // FUN_80074590's `beq v0, zero, 0x800747A0` — no pitch when global_scale is 0.
    pitch_final = 0;
  } else {
    const int32_t bent = byte6_ext + pitchBend_ext;
    const int16_t bent16 = (int16_t)bent;                                        // recomp: sll 16 ; sra 16
    const int32_t product = (int32_t)bent16 * (int32_t)(uint32_t)global_scale;   // 16 × u8
    const int16_t product16 = (int16_t)product;                                  // recomp: sll 16 ; sra 16
    const int64_t bigProd = (int64_t)product16 * (int64_t)0x38E38E39;
    const int32_t hi = (int32_t)(bigProd >> 32);
    const int32_t signBit = ((int32_t)product16 < 0) ? -1 : 0;                   // recomp: (mflo<<16) >> 31
    const int32_t divBy9 = (hi >> 1) - signBit;
    const int16_t clamped = (int16_t)divBy9;                                     // recomp: sll 16 ; sra 16
    if (clamped < 0)          pitch_final = 0;
    else if (clamped >= 128)  pitch_final = 127;
    else                      pitch_final = (int32_t)clamped;
  }

  // ---- ASSEMBLE ARGS FOR FUN_80075E04 (SPU voice-fire) ----
  //   a0 = t0_final
  //   a1 = fx.byte1
  //   a2 = fx.byte2
  //   a3 = fx.byte3 | a3_flag        (recomp: `or a3, t0(=byte3), a3(=0 or 128 sign-ext16)`)
  //   [sp+16] = int32(fx.byte4 + pan)
  //   [sp+20] = fx.byte5              (delay-slot write)
  //   [sp+24] = int32(pitch_final)
  //   [sp+28] = int32(pitch_final)   (dup — the recomp writes the same value to both slots)
  const uint8_t b1 = c->mem_r8(t1 + 1u);
  const uint8_t b2 = c->mem_r8(t1 + 2u);
  const uint8_t b3 = c->mem_r8(t1 + 3u);
  const int8_t  b4 = (int8_t)c->mem_r8(t1 + 4u);
  const uint8_t b5 = c->mem_r8(t1 + 5u);
  const int8_t  pan_ext = (int8_t)(pan & 0xFF);
  const int32_t stack16 = (int32_t)b4 + (int32_t)pan_ext;

  c->r[4] = t0_final;
  c->r[5] = (uint32_t)b1;
  c->r[6] = (uint32_t)b2;
  c->r[7] = (uint32_t)((uint32_t)b3 | (uint32_t)a3_flag);
  c->mem_w32(sp + 16u, (uint32_t)stack16);
  c->mem_w32(sp + 20u, (uint32_t)b5);
  c->mem_w32(sp + 24u, (uint32_t)pitch_final);
  c->mem_w32(sp + 28u, (uint32_t)pitch_final);

  rec_dispatch(c, 0x80075E04u);

  c->r[29] = sp_save;
  c->r[31] = ra_save;
}

// ORACLE: gen_func_80074810
// Two-arg SFX entry: trigger(id, pan) with pitchBend forced to 0. The recomp masks id to a byte,
// sign-extends pan from its low byte, sets pitchBend=0, then jal FUN_80074590. It descends a
// 24-byte frame and spills ra at sp+16 (so FUN_80074590's own stack args land correctly), then
// restores ra and ascends before returning.
//
// PORT_CHECK: FAIL — TOOL ARTIFACT, not a real divergence. gen_func_80074810 is a single C
// function that the recompiler folded THREE contiguous guest functions into (entries 0x80074810,
// 0x80074834, 0x80074868), each with its own frame + `return`. Only the first (0x80074810) is
// reachable at this entry — dispatch executes from the top and hits the first `return`; the
// port_gen/abi_extract CONTRACT/CFG analysis agrees (reports frame_size=24, 1 call site). But
// port_check's coarse linear op-sequence extractor (abi_extract.extract_op_sequence) enters its
// dead-region skip at the first `return`, then WRONGLY re-activates it at label `L_800748C4:`,
// which belongs to the DEAD third sibling (0x80074868) — pulling that sibling's 40-byte frame,
// its func_80092660 call, and its 13 stores into the "oracle" sequence. So the oracle side is
// mis-extracted as frame_opens=[24] closes=[24,40] calls=[74590,92660] — no faithful port of the
// REAL first-body function can match it. Reproducing the sibling's ops would be dead code, banned.
// The real fix is in abi_extract.extract_op_sequence (dead-region must not exit on a label whose
// only predecessor is also dead) — out of scope for this file. SBS is the real gate here: the
// native reproduces exactly what gen_func_80074810 does at runtime (first body only), so it is
// 0-diff by construction.
void Sfx::triggerPanned(int id, int pan) {
  Core* c = core;

  const uint32_t ra_save = c->r[31];
  c->r[29] -= 24u;                                 // gen: addiu sp,-24
  const uint32_t sp = c->r[29];

  const uint32_t idb  = (uint32_t)(id & 0xFF);            // andi a0,0xFF
  const int32_t  panx = (int32_t)(int8_t)(pan & 0xFF);    // sll 24 ; sra 24 — sign-extend low byte

  c->mem_w32(sp + 16u, ra_save);                  // sw ra,16(sp)

  // Assemble FUN_80074590(id & 0xFF, sext8(pan), 0) and fire.
  c->r[4] = idb;
  c->r[5] = (uint32_t)panx;
  c->r[6] = 0u;                                   // pitchBend = 0 (a2 = zero)
  c->r[31] = 0x8007482Cu;                         // gen return-address constant for the call
  func_80074590(c);                               // FUN_80074590 — SFX firing primitive (substrate)

  c->r[31] = c->mem_r32(sp + 16u);                // lw ra,16(sp)
  c->r[29] += 24u;                                 // addiu sp,+24 (restore)
}

static void eov_triggerPanned(Core* c) {
  eng(c).sfx.triggerPanned((int)c->r[4], (int)c->r[5]);
  // gen_func_80074810's first body writes no return value (r2 dead); nothing to mirror.
}

extern void gen_func_80074810(Core*);

void Sfx::registerOverrides() {
  using overrides::install;
  extern void shard_set_override(uint32_t, OverrideFn);
  // Dual-wired: a direct func_80074810(c) caller exists (generated/shard_4.c) plus rec_dispatch
  // callers from overlays — shard_set_override installs the shared oracle-gated thunk for both.
  install(0x80074810u, "Sfx::triggerPanned", eov_triggerPanned, gen_func_80074810, shard_set_override);
}
