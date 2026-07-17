// ObjectTable::dispatch — see object_table.h. Faithful port of guest FUN_80026C88 with the
// existing `disp26c88verify` A/B gate preserved.
#include "object_table.h"
#include "game_ctx.h"
#include "game.h"        // c->game->verify — the shared A/B verify scaffold
#include "core.h"
#include "cfg.h"
#include "core/engine.h"           // eng(c).animation etc (not needed here but consistent)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);
// MV_CHECK (verify_harness.h, pulled in via game.h) — strict mirror TDD gate for the
// dispatchFaithful() fork below.

// FUN_80027254 — the sole handler installed in HANDLER_TABLE (all 4 slot indices point at this
// one function). A 4-state PARTICLE STATE MACHINE (state byte @obj+4):
//   * 0 = INIT   : read obj+1 flags, obj+2 pattern index, obj+0x33 initial life; use LCG PRNG
//                  (FUN_8009A450) to jitter initial velocities; consult per-pattern tables at
//                  0x8009D53C / 0x8009D54C (velocity direction lookup) + 0x8009D55C / 0x8009D56C
//                  (dampening) + 0x8009D57C (gravity scale). Flips state → 1.
//   * 1 = RUNNING: decay life obj+0xC by 1/128 per frame; velocity integrate positions at obj+0x1E
//                  / 0x26 / 0x28 / 0x2A / 0x2C from deltas at obj+0x10/14/16/A/1A; run gravity
//                  obj+0x12 += obj+0x18; scale motion by *0x100 for the 0x20 accumulator; despawn
//                  (state → 3) when obj+0x2E + 0x800 < obj+0x12 or the obj+0x8 timer hits 1.
//   * 2 = (unused): no-op (recomp: `if (bVar1 != 2 && bVar1 == 3)` false → nothing).
//   * 3 = DESPAWN: dispatch FUN_8007B2AC (substrate pool return).
// Ghidra decomp scratch/decomp/objtable_handler_27254.c. NO SFX (verified).
namespace {

constexpr uint32_t H_27254           = 0x80027254u;
constexpr uint32_t PATTERN_TBL_A     = 0x8009D53Cu;   // (obj+1 & 2)==0 branch (walking pattern)
constexpr uint32_t PATTERN_TBL_B     = 0x8009D54Cu;   // (obj+1 & 2)!=0 branch (running pattern)
constexpr uint32_t DAMPEN_TBL        = 0x8009D55Cu;   // horiz-dampen  (16 s16 entries)
constexpr uint32_t DAMPEN_TBL_2      = 0x8009D56Cu;   // vert-dampen   (16 s16 entries)
constexpr uint32_t GRAVITY_TBL       = 0x8009D57Cu;   // gravity scale (16 s16 entries)
constexpr uint32_t LEAF_POOL_RETURN  = 0x8007B2ACu;   // FUN_8007B2AC — pool free (substrate)

inline uint32_t prng(Core* c) { return rngOf(c).next(); }

}  // namespace

void ObjectTable::handler27254(uint32_t obj) {
  Core* c = core;
  const uint8_t st = c->mem_r8(obj + 4u);

  if (st == 1) {
    // RUNNING body — full byte-exact.
    const uint16_t life = c->mem_r16(obj + 0xCu);
    c->mem_w16(obj + 0xCu, (uint16_t)(life - (life >> 7)));
    const int16_t vx  = (int16_t)c->mem_r16(obj + 0x10u);
    const int16_t vz  = (int16_t)c->mem_r16(obj + 0x14u);
    const int16_t vw  = (int16_t)c->mem_r16(obj + 0x16u);
    const int16_t vy0 = (int16_t)c->mem_r16(obj + 0xAu);
    c->mem_w16(obj + 0x1Eu, (uint16_t)((int16_t)c->mem_r16(obj + 0x1Eu) + vx));
    c->mem_w16(obj + 0x26u, (uint16_t)((int16_t)c->mem_r16(obj + 0x26u) + vz));
    c->mem_w16(obj + 0x28u, (uint16_t)((int16_t)c->mem_r16(obj + 0x28u) + vw));
    c->mem_w16(obj + 0x2Au, (uint16_t)((int16_t)c->mem_r16(obj + 0x2Au) + vy0));
    const int16_t g_prev = (int16_t)c->mem_r16(obj + 0x12u);
    const int16_t g_add  = (int16_t)c->mem_r16(obj + 0x1Au);
    c->mem_w16(obj + 0x2Cu, (uint16_t)((int16_t)c->mem_r16(obj + 0x2Cu) + g_add));
    const int16_t g_new  = (int16_t)(g_prev + (int16_t)c->mem_r16(obj + 0x18u));
    c->mem_w16(obj + 0x12u, (uint16_t)g_new);
    c->mem_w32(obj + 0x20u, c->mem_r32(obj + 0x20u) + (uint32_t)((int32_t)g_prev * 0x100));

    const int16_t threshold = (int16_t)c->mem_r16(obj + 0x2Eu);
    const int16_t timer     = (int16_t)c->mem_r16(obj + 0x8u);
    c->mem_w16(obj + 0x8u, (uint16_t)(timer - 1));
    if ((int32_t)threshold + 0x800 < (int32_t)g_new || timer == 1) {
      c->mem_w8(obj + 4u, 3);
    }
    return;
  }

  if (st == 0) {
    // INIT — jitter velocities using PRNG + per-pattern tables.
    c->mem_w8(obj + 4u, 1);
    const uint8_t flag  = c->mem_r8(obj + 1u);
    const uint8_t patt  = c->mem_r8(obj + 2u);
    const int16_t seed  = (int16_t)((patt & 1) + 2);
    c->mem_w8 (obj + 0xEu, c->mem_r8(obj + 0x33u));
    c->mem_w16(obj + 0x14u, (uint16_t)seed);
    c->mem_w16(obj + 0x10u, (uint16_t)seed);
    c->mem_w16(obj + 0x10u, (uint16_t)((int16_t)c->mem_r16(obj + 0x10u) + (int16_t)(prng(c) & 3)));
    c->mem_w16(obj + 0x14u, (uint16_t)((int16_t)c->mem_r16(obj + 0x14u) + (int16_t)(prng(c) & 3)));

    const uint32_t pattTable = (flag & 2) == 0 ? PATTERN_TBL_A : PATTERN_TBL_B;
    if ((flag & 1) == 0) {
      // Standard pattern.
      c->mem_w16(obj + 0x8u, 0x30);
      const int16_t vx0 = (int16_t)c->mem_r16(obj + 0x10u);
      const int16_t vz0 = (int16_t)c->mem_r16(obj + 0x14u);
      const int16_t mx  = (int16_t)c->mem_r16(pattTable + (patt & 7) * 2u);
      const int16_t mz  = (int16_t)c->mem_r16(pattTable + ((patt + 6) & 7) * 2u);
      c->mem_w16(obj + 0x10u, (uint16_t)(vx0 * 4 * mx));
      c->mem_w16(obj + 0x14u, (uint16_t)(vz0 * 4 * mz));
      const int16_t jitter = (int16_t)((((prng(c) & 3) + 1) * -8) - 0x20);
      c->mem_w16(obj + 0x12u, (uint16_t)(jitter * 0x100));
      c->mem_w16(obj + 0x18u, 0x300);
    } else {
      // Fast/high pattern.
      c->mem_w16(obj + 0x8u, 0x18);
      c->mem_w16(obj + 0x12u, 0xE000);
      c->mem_w16(obj + 0x18u, 0x400);
      const int16_t vx0 = (int16_t)c->mem_r16(obj + 0x10u);
      const int16_t vz0 = (int16_t)c->mem_r16(obj + 0x14u);
      const int16_t mx  = (int16_t)c->mem_r16(pattTable + (patt & 7) * 2u);
      const int16_t mz  = (int16_t)c->mem_r16(pattTable + ((patt + 6) & 7) * 2u);
      c->mem_w16(obj + 0x10u, (uint16_t)(vx0 * 2 * mx));
      c->mem_w16(obj + 0x14u, (uint16_t)(vz0 * 2 * mz));
    }

    // Post-pattern dampening + gravity-scale bake.
    const int16_t combined = (int16_t)((c->mem_r16(obj + 0x10u) | c->mem_r16(obj + 0x14u)) * 0x14);
    c->mem_w16(obj + 0x1Au, (uint16_t)combined);
    c->mem_w16(obj + 0xAu,  (uint16_t)combined);
    c->mem_w16(obj + 0x16u, (uint16_t)combined);
    c->mem_w16(obj + 0x16u, (uint16_t)(combined * (int16_t)c->mem_r16(DAMPEN_TBL   + patt * 2u)));
    c->mem_w16(obj + 0xAu,  (uint16_t)((int16_t)c->mem_r16(obj + 0xAu) * (int16_t)c->mem_r16(DAMPEN_TBL_2 + patt * 2u)));
    const int16_t gravityScale = (int16_t)c->mem_r16(GRAVITY_TBL + patt * 2u);
    c->mem_w16(obj + 0x28u, 0);
    c->mem_w16(obj + 0x2Au, 0);
    c->mem_w16(obj + 0x2Cu, 0);
    c->mem_w16(obj + 0x2Eu, (uint16_t)(-(int16_t)c->mem_r16(obj + 0x12u)));
    c->mem_w16(obj + 0x1Au, (uint16_t)((int16_t)c->mem_r16(obj + 0x1Au) * gravityScale));
    return;
  }

  if (st == 3) {
    c->r[4] = obj;
    rec_dispatch(c, LEAF_POOL_RETURN);                                // pool free (substrate)
    return;
  }

  // st == 2 or any other: no-op (recomp: `bVar1 != 2 && bVar1 == 3` false-branch).
}

void ObjectTable::dispatch() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { MV_CHECK(c, 0x80026C88u, dispatchFaithful()); return; }

  auto body = [&]() {
    uint32_t obj = TABLE_BASE;
    for (int i = 0; i < SLOT_COUNT; i++, obj += SLOT_STRIDE) {
      if (c->mem_r8(obj + 0) == 0) continue;
      uint32_t idx = c->mem_r8(obj + 1);
      uint32_t fn  = c->mem_r32(HANDLER_TABLE + (idx << 2));
      c->r[4] = obj;
      if (fn == H_27254) { handler27254(obj); continue; }             // native — the only real entry
      rec_dispatch(c, fn);   // any other handler (defensive; the table only holds H_27254 today)
    }
  };

  int s_v = c->game->verify.on("disp26c88verify");
  if (!s_v) { body(); return; }

  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  body();

  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80026C88u);

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1;
  for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("disp26c88verify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[disp26c88verify] MISMATCH ram@%x spad@%x sp=%x\n", ro, so, sp);
  } else if (++ng % 50 == 0) fprintf(stderr, "[disp26c88verify] %ld matches\n", ng);
}

// ObjectTable::dispatchFaithful — byte-mirror of gen_func_80026C88 (generated/shard_2.c:1607-1637).
// Reproduces the guest frame descent, the s0/s1/s2/ra stack spill at [sp+16/20/24/28] with the
// LIVE incoming register values (spilled BEFORE each register is repurposed, matching gen's
// instruction order exactly), the per-iteration jal-site r31=0x80026CE0 before dispatching an
// active slot's handler, and the full epilogue restore. The 40-slot loop runs off the guest
// s0 (obj cursor, c->r[16]) / s1 (loop counter, c->r[17]) / s2 (handler table ptr, c->r[18])
// registers rather than local C++ variables, since those 16 bytes of guest stack are
// observable under SBS strict compare.
void ObjectTable::dispatchFaithful() {
  Core* c = core;
  uint32_t ra = c->r[31], sp = c->r[29];
  uint32_t s0 = c->r[16], s1 = c->r[17], s2 = c->r[18];

  c->r[29] = sp - 32;                       // addiu sp,-0x20
  c->mem_w32(c->r[29] + 20, s1);            // sw s1,0x14(sp) — LIVE incoming s1
  c->r[17] = 0;                             // s1 = i = 0
  c->mem_w32(c->r[29] + 24, s2);            // sw s2,0x18(sp) — LIVE incoming s2
  c->r[18] = HANDLER_TABLE;                 // s2 = 0x8009D52C
  c->mem_w32(c->r[29] + 16, s0);            // sw s0,0x10(sp) — LIVE incoming s0
  c->r[16] = TABLE_BASE;                    // s0 = 0x800EC188 (obj cursor)
  c->mem_w32(c->r[29] + 28, ra);            // sw ra,0x1c(sp)

  for (int i = 0; i < SLOT_COUNT; i++) {
    uint32_t obj = c->r[16];
    if (c->mem_r8(obj + 0) != 0) {
      uint32_t idx = c->mem_r8(obj + 1);
      uint32_t fn  = c->mem_r32(c->r[18] + (idx << 2));
      c->r[31] = 0x80026CE0u;               // jal-site ra (matches gen exactly)
      c->r[4]  = obj;
      // pc_faithful (this mirror) MUST reach the literal gen body for EVERY handler, including
      // H_27254 — same fix as BehaviorDispatch::dispatchObj (game/object/behavior_dispatch.cpp):
      // handler27254()'s native port reproduces the RESULT, not the PSX bytes (its INIT state
      // calls prng(c) -> rngOf(c).next(), which does not reproduce gen_func_8009A450's ABI
      // end-state — v0/v1/hi-lo — the way rec_dispatch to the real LCG body does). Taking the
      // native shortcut here (as the pc_skip=true `body()` lambda above intentionally does) is
      // what caused the 0x80106B98 strict-mirror-verify FAILURE (12+ diffs at 0x801FE8xx / v0 /
      // v1): this call is reached from ObjectTable::dispatch()'s own MV_CHECK, but that check is
      // a no-op while nested inside an outer strictCheck (no nesting, verify_harness.h), so the
      // divergence went uncaught here and only surfaced at the outermost fieldRunFaithful check.
      rec_dispatch(c, fn);
    }
    c->r[17] = c->r[17] + 1;                // s1++
    c->r[16] = c->r[16] + SLOT_STRIDE;      // s0 += 64 (delay-slot semantics: always executes)
  }

  c->r[31] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] = c->r[29] + 32;
}
