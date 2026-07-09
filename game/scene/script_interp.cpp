// game/scene/script_interp.cpp — PC-native cutscene SCRIPT INTERPRETER.
//
// RE'd verbatim from disas of the four resident MAIN.EXE fns 0x80040CDC / 0x80040DE0 / 0x80040E54 /
// 0x80041098 + the op-table entry at 0x800412CC. See docs/findings/scene.md "Cutscene SCRIPT
// INTERPRETER" for the full RE (opcode entry format, flag bits, handler table location, ret-code
// switch).
//
// TOP-DOWN DOCTRINE — the interpreter LEAVES it doesn't own yet (each opcode handler at the 63-
// entry table 0x800A3B78) stay reachable via rec_dispatch. Only the DISPATCH LOOP is owned here,
// plus op 0x03E (call fnptr) which is native-routed through BehaviorDispatch::dispatchObj — so any
// script-driven fade / cutscene fnptr registered as a native `beh_*` runs native automatically;
// unregistered fnptrs fall through to the substrate leaf. No revived rec_set_override.
//
// The advance sub-machine at guest FUN_80040FA0 (small post-advance JT wrapper around advanceEntry)
// stays substrate for now — it's pure state manipulation on the driven object, easy to promote to
// a native method in a follow-up once the caller chain reaches this class from native code. Same
// for the 62 unowned opcode handlers.

#include "scene/script_interp.h"
#include "core.h"
#include "core/engine.h"
#include "object/behavior_dispatch.h"
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {
// The four object-field offsets we touch, named for readability. Layout matches the guest exactly
// (see script_interp.h header comment).
constexpr uint32_t OBJ_EXTRA_BLOCK   = 0x64u;  // 4x u16 extra block for entries with flag 0x2000
constexpr uint32_t OBJ_MARKER_46     = 0x46u;  // init writes 0xFF here
constexpr uint32_t OBJ_SCRATCH_10    = 0x10u;  // init zeroes this word
constexpr uint32_t OBJ_SCRIPT_PTR    = 0x6Cu;  // current script cursor
constexpr uint32_t OBJ_PROGRESS_70   = 0x70u;  // loop guard byte (signed; step exits when <= 0)
constexpr uint32_t OBJ_FLAGS_71      = 0x71u;  // flag byte (bit1 = paused, bit2 = 0x4000-flag)
constexpr uint32_t OBJ_ARG_A_72      = 0x72u;  // current entry's argA (u16)
constexpr uint32_t OBJ_FNPTR_LO_74   = 0x74u;  // current entry's argB — LOW half of op-0x03E fnptr
constexpr uint32_t OBJ_FNPTR_HI_76   = 0x76u;  // current entry's argC — HIGH half of op-0x03E fnptr
constexpr uint32_t OBJ_SCRATCH_78    = 0x78u;  // init clears
constexpr uint32_t OBJ_TABLE_A_7C    = 0x7Cu;  // secondary script table pointer, set at init

// Opcode word bit layout (u16 at scriptPtr+0). See header comment.
constexpr uint16_t OP_ID_MASK        = 0x07FFu;  // low 11 bits = opcode id (0..2047, but table has 63)
constexpr uint16_t OP_FLAG_HAS_EXTRA = 0x2000u;  // if set, entry is 16 bytes (extra block at +8)
constexpr uint16_t OP_FLAG_INIT_MODE = 0x1000u;  // init: obj[+0x71] = 2 when this bit is set
constexpr uint16_t OP_FLAG_INIT_BIT2 = 0x4000u;  // init: obj[+0x71] |= 4 when this bit is set

// step()'s return-code convention (VERBATIM from disas of FUN_80041098 @0x800410FC..0x80041174):
//   ret == 0      -> set obj[+0x71] |= 2, EXIT step loop immediately (no advance)
//   ret == 1      -> FUN_80040FA0(obj, 0), continue-if-FA0-returns-1
//   ret == 2      -> FUN_80040FA0(obj, 1), continue-if-FA0-returns-1
//   ret == 3      -> FUN_80040FA0(obj, 0), continue-if-FA0-returns-1  (same as ret==1 — falls into it)
//   ret >= 4      -> no advance; exit loop (recomp defaults s0=3, loop check `s0 == 1` fails)
// The loop continues ONLY when FA0 returns exactly 1; anything else exits with the FA0 return
// discarded (guest fn's overall v0 is 0).
constexpr uint32_t RET_PAUSE       = 0u;
constexpr uint32_t RET_ADVANCE_0_A = 1u;   // FA0(obj, 0)
constexpr uint32_t RET_ADVANCE_1   = 2u;   // FA0(obj, 1)
constexpr uint32_t RET_ADVANCE_0_B = 3u;   // FA0(obj, 0)
}  // namespace

void ScriptInterp::init(uint32_t obj, uint32_t tableA, uint32_t scriptPtr) {
  Core* c = core;
  // FUN_80040CDC prologue-equivalent state writes on `obj`.
  c->mem_w32(obj + OBJ_TABLE_A_7C,  tableA);
  c->mem_w8 (obj + OBJ_MARKER_46,  0xFFu);
  c->mem_w32(obj + OBJ_SCRATCH_10, 0u);
  c->mem_w8 (obj + OBJ_PROGRESS_70, 0u);
  c->mem_w8 (obj + OBJ_SCRATCH_78, 0u);
  c->mem_w8 (obj + OBJ_FLAGS_71,   0u);
  // Load the first entry so subsequent step() calls see argA/B/C and the (optional) extra block.
  loadCurrentEntry(obj, scriptPtr);
  // Reload obj+0x71 from the first entry's flag bits (matches the recomp's post-load flag scan).
  const uint16_t op0 = c->mem_r16(scriptPtr + 0);
  uint8_t flags = c->mem_r8(obj + OBJ_FLAGS_71);
  if (op0 & OP_FLAG_INIT_MODE) flags = 2u;
  c->mem_w8(obj + OBJ_FLAGS_71, flags);
  if (op0 & OP_FLAG_INIT_BIT2) {
    uint8_t f = c->mem_r8(obj + OBJ_FLAGS_71);
    c->mem_w8(obj + OBJ_FLAGS_71, (uint8_t)(f | 0x04u));
  }
}

void ScriptInterp::loadCurrentEntry(uint32_t obj, uint32_t scriptPtr) {
  Core* c = core;
  // FUN_80040DE0 body — copy the 3 u16 args and record the cursor.
  c->mem_w32(obj + OBJ_SCRIPT_PTR, scriptPtr);
  c->mem_w16(obj + OBJ_ARG_A_72,   c->mem_r16(scriptPtr + 2));
  c->mem_w16(obj + OBJ_FNPTR_LO_74, c->mem_r16(scriptPtr + 4));
  c->mem_w16(obj + OBJ_FNPTR_HI_76, c->mem_r16(scriptPtr + 6));
  // If the opcode says "extra block", copy 4 more halfwords into obj[+0x64..+0x6A].
  const uint16_t op0 = c->mem_r16(scriptPtr + 0);
  if (op0 & OP_FLAG_HAS_EXTRA) {
    const uint32_t ex = scriptPtr + 8;
    c->mem_w16(obj + OBJ_EXTRA_BLOCK + 0, c->mem_r16(ex + 0));
    c->mem_w16(obj + OBJ_EXTRA_BLOCK + 2, c->mem_r16(ex + 2));
    c->mem_w16(obj + OBJ_EXTRA_BLOCK + 4, c->mem_r16(ex + 4));
    c->mem_w16(obj + OBJ_EXTRA_BLOCK + 6, c->mem_r16(ex + 6));
  }
}

int ScriptInterp::advanceEntry(uint32_t obj, uint32_t kindArg) {
  Core* c = core;
  // The advance sub-machine at guest FUN_80040E54 has a fine-grained 3-bit-flag → advance-amount /
  // branch-fetch table; the FUN_80040FA0 wrapper adds a small post-advance JT on the return code.
  // Both stay substrate for now — pure state manipulation on `obj`, easy to promote in a follow-up.
  // We route through 0x80040FA0 (the wrapper the step loop uses) so obj+0x70 stays in sync.
  c->r[4] = obj;
  c->r[5] = kindArg;
  const uint32_t sp_save = c->r[29];
  const uint32_t ra_save = c->r[31];
  rec_dispatch(c, 0x80040FA0u);
  c->r[29] = sp_save;
  c->r[31] = ra_save;
  return (int)c->r[2];
}

namespace {
// Shared byte-array anchors op06/op34 read/write (guest MAIN.EXE .data/.bss, resident). Purpose
// beyond "flag storage indexed by argB" / "single-byte gate" not traced further this pass — see
// the header banner for the honest caveat.
constexpr uint32_t kSceneFlagTable = 0x800BF870u + 324u;  // op06: byte[argB]
constexpr uint32_t kClaimGateByte  = 0x800BF86Fu;          // op34: single-byte semaphore
}  // namespace

// FUN_80042090 — DRAFT, UNWIRED (wide-RE 2026-07-10). 1:1 with generated/shard_7.c:5216.
// NOTE ON RETURN VALUE: the guest computes `(decremented << 16) >> 31` (arithmetic) — a MIPS
// sign-replicate idiom, NOT a 0/1 flag. It yields exactly 0 (still counting down) or -1 i.e.
// 0xFFFFFFFF (went negative). Per step()'s ret-code switch (RET_PAUSE=0 / RET_ADVANCE_*={1,2,3}),
// 0 hits RET_PAUSE (sets the pause flag, exits); -1 matches NONE of the switch's cases and falls
// to `default` (exits WITHOUT setting the pause flag, WITHOUT advancing) — so this op never
// actually advances the script cursor itself in either branch; only the pause-flag side effect
// differs. Do not "fix" this to return 1 — it would change step()'s dispatch outcome.
int ScriptInterp::op05WaitFrames(uint32_t obj) {
  Core* c = core;
  const uint16_t decremented = (uint16_t)(c->mem_r16(obj + OBJ_ARG_A_72) - 1u);
  c->mem_w16(obj + OBJ_ARG_A_72, decremented);
  return ((int16_t)decremented < 0) ? -1 : 0;
}

// FUN_800420AC — DRAFT, UNWIRED (wide-RE 2026-07-10). 1:1 with generated/shard_0.c:5231. See
// script_interp.h for the 3-mode (eq/and-nonzero/and-zero) semantics.
int ScriptInterp::op06TestSceneFlag(uint32_t obj) {
  Core* c = core;
  const int16_t argA = (int16_t)c->mem_r16(obj + OBJ_ARG_A_72);
  const int16_t argB = (int16_t)c->mem_r16(obj + OBJ_FNPTR_LO_74);
  const int16_t argC = (int16_t)c->mem_r16(obj + OBJ_FNPTR_HI_76);
  const uint8_t tableByte = c->mem_r8(kSceneFlagTable + (uint32_t)(int32_t)argB);
  if (argA == 1) {
    // Guest ANDs the zero-extended byte (0..255) with the sign-extended argC; the AND's upper 24
    // bits are always 0 (tableByte has no high bits), so truncating argC to a byte before the AND
    // gives the identical low-byte result — safe, unlike the equality arm below.
    return ((tableByte & (uint8_t)argC) != 0) ? 1 : 0;
  } else if (argA == 0) {
    // EXACT match: guest compares the zero-extended byte (r3, always 0..255) against the FULL
    // sign-extended argC (r2, 32-bit) — NOT a byte-truncated compare. A negative or >255 argC can
    // never equal a byte, so this must sign-extend argC, not truncate it.
    return ((uint32_t)tableByte == (uint32_t)(int32_t)argC) ? 1 : 0;
  } else if (argA == 2) {
    return ((tableByte & (uint8_t)argC) == 0) ? 1 : 0;
  }
  return 0;  // argA >= 3: unreachable in the guest's own byte-shape, kept for parity
}

// FUN_80042E10 — DRAFT, UNWIRED (wide-RE 2026-07-10). 1:1 with generated/shard_2.c:4772. See
// script_interp.h for the claim/poll phase-machine semantics.
int ScriptInterp::op34ClaimGate(uint32_t obj) {
  Core* c = core;
  uint8_t phase = c->mem_r8(obj + OBJ_SCRATCH_78);
  if (phase == 0) {
    const uint16_t argARaw = c->mem_r16(obj + OBJ_ARG_A_72);
    const uint8_t claimTag = (uint8_t)(argARaw & 7u);
    // Guest control flow: when claimTag==0 it jumps STRAIGHT to the phase-increment tail, skipping
    // the write AND the sign-bit ("no-wait") check entirely — both are gated on claimTag != 0.
    if (claimTag != 0) {
      if (c->mem_r8(kClaimGateByte) == 0) c->mem_w8(kClaimGateByte, claimTag);
      if (argARaw & 0x8000u) return 1;            // "no-wait" argA -> advance immediately
    }
    c->mem_w8(obj + OBJ_SCRATCH_78, (uint8_t)(phase + 1));
    return 0;
  } else if (phase == 1) {
    return (c->mem_r8(kClaimGateByte) == 0) ? 1 : 0;
  }
  return 0;  // phase >= 2: unreachable in the guest's own byte-shape, kept for parity
}

// FUN_80040FA0 — DRAFT, UNWIRED (wide-RE 2026-07-10). 1:1 with generated/shard_2.c:4564. See
// script_interp.h for the naming caveat (advanceEntry() above already rec_dispatch's HERE) and the
// 7-entry switch-table verification. The sp-=24/ra-spill guest frame is this function's OWN
// activation record (not shared/observable state beyond the call) so it is not reproduced.
int ScriptInterp::advanceStep(uint32_t obj, uint32_t kindArg) {
  Core* c = core;
  c->r[4] = obj;
  c->r[5] = kindArg;
  rec_dispatch(c, 0x80040E54u);  // still-substrate FUN_80040E54 (out of band for this pass)
  const uint32_t ret = c->r[2];
  if (ret >= 7u) return -1;      // matches table[3]'s literal -1 (index 3 IS 0x80041080)
  switch (ret) {
    case 0: {  // table[0] = 0x80040FE0
      const uint8_t state = c->mem_r8(obj + OBJ_PROGRESS_70);
      int32_t r3;
      if (state == 2u) { c->mem_w8(obj + OBJ_FLAGS_71, state); r3 = 1; }
      else { c->mem_w8(obj + OBJ_FLAGS_71, 0); c->mem_w8(obj + OBJ_PROGRESS_70, 0); r3 = 0; }
      c->mem_w8(obj + OBJ_SCRATCH_78, 0);
      return r3;
    }
    case 1: {  // table[1] = 0x8004100C
      const uint8_t state = c->mem_r8(obj + OBJ_PROGRESS_70);
      int32_t r3;
      if (state == 2u) { c->mem_w8(obj + OBJ_FLAGS_71, 6); r3 = 1; }
      else { c->mem_w8(obj + OBJ_FLAGS_71, 4); c->mem_w8(obj + OBJ_PROGRESS_70, 0); r3 = 0; }
      c->mem_w8(obj + OBJ_SCRATCH_78, 0);
      return r3;
    }
    case 2:  // table[2] = 0x8004103C — jumps straight to return, SKIPS the +0x78 tail write
      c->mem_w8(obj + OBJ_PROGRESS_70, 255);
      c->mem_w8(obj + OBJ_FLAGS_71, 255);
      return 0;
    case 3:  // table[3] = 0x80041080 — no writes at all
      return -1;
    case 4:  // table[4] = 0x80041050
      c->mem_w8(obj + OBJ_FLAGS_71, 2);
      c->mem_w8(obj + OBJ_SCRATCH_78, 0);
      return 1;
    case 5:  // table[5] = 0x8004105C
      c->mem_w8(obj + OBJ_FLAGS_71, 6);
      c->mem_w8(obj + OBJ_SCRATCH_78, 0);
      return 1;
    case 6:  // table[6] = 0x80041070
      c->mem_w8(obj + OBJ_FLAGS_71, 0);
      c->mem_w8(obj + OBJ_PROGRESS_70, 0);
      c->mem_w8(obj + OBJ_SCRATCH_78, 0);
      return 0;
    default:
      return -1;  // unreachable (ret already bounded to 0..6 above)
  }
}

int ScriptInterp::callFnptr(uint32_t obj) {
  Core* c = core;
  // Guest 0x800412CC: `lw v0, 0x74(a0); jalr v0; jr ra` — v0 (the ret code) is EXACTLY what the
  // fnptr callee left there. Compose the 32-bit fnptr from the two adjacent halfwords (LE order
  // matches the guest `lw`), route via BehaviorDispatch so any fnptr owned as native `beh_*` runs
  // native; unowned addresses fall through to rec_dispatch (substrate).
  //
  // RET-CODE PRESERVATION: rec_dispatch sets c->r[2] to the substrate leaf's return, so the
  // substrate path is transparent. Native `beh_*` handlers are `void`-returning, so a native
  // fade fn MUST set `c->r[2] = ret` before returning (matches how the recomp body would leave
  // v0). Every fade fn in game/ai/beh_a06_fade_*.cpp follows this convention.
  const uint16_t lo = c->mem_r16(obj + OBJ_FNPTR_LO_74);
  const uint16_t hi = c->mem_r16(obj + OBJ_FNPTR_HI_76);
  const uint32_t fnptr = ((uint32_t)hi << 16) | (uint32_t)lo;
  c->engine.behaviors.dispatchObj(obj, fnptr);
  return (int)c->r[2];
}

// C-ABI wrapper for BehaviorDispatch::kTable registration. Takes obj from a0 (c->r[4]) — matches
// the recomp's calling convention so a native ancestor that already used dispatchObj lands here
// transparently. The wrapper is deliberately trivial: pull obj out of a0 and forward to step().
void beh_script_interp_step(Core* c) {
  c->engine.script.step(c->r[4]);
}

void ScriptInterp::step(uint32_t obj) {
  Core* c = core;
  // FUN_80041098 body — the dispatch loop. Runs while obj[+0x70] > 0 (signed).
  for (;;) {
    const int8_t prog = (int8_t)c->mem_r8(obj + OBJ_PROGRESS_70);
    if (prog <= 0) return;
    const uint32_t scriptPtr = c->mem_r32(obj + OBJ_SCRIPT_PTR);
    const uint16_t opWord = c->mem_r16(scriptPtr + 0);
    const uint32_t oid = (uint32_t)(opWord & OP_ID_MASK);

    uint32_t ret;
    if (oid == 0x3Eu) {
      // NATIVE routing for op 0x03E — the "call fnptr" mechanism the script-driven cutscene fade
      // family rides. Any fade fn registered in BehaviorDispatch::kTable runs native transparently.
      ret = (uint32_t)callFnptr(obj);
    } else {
      // Every OTHER opcode: dispatch to the substrate handler via the resident handler table at
      // 0x800A3B78. Guest ABI: a0=obj; returns v0. Save/restore sp+ra so any downstream leaf that
      // reads its own stack args lands cleanly.
      const uint32_t handler = c->mem_r32(kHandlerTableBase + oid * 4u);
      c->r[4] = obj;
      const uint32_t sp_save = c->r[29];
      const uint32_t ra_save = c->r[31];
      rec_dispatch(c, handler);
      c->r[29] = sp_save;
      c->r[31] = ra_save;
      ret = c->r[2];
    }

    // ret-code switch (VERBATIM disas 0x80041100..0x80041168). The recomp calls advanceEntry with
    // a kind byte selected by the ret code, THEN checks its return: only if FA0 returns exactly 1
    // does the outer loop iterate (recomp's `beq s0, s2, 0x800410c0`). Any other FA0 return exits.
    uint32_t faKind;
    switch (ret) {
      case RET_PAUSE: {
        uint8_t f = c->mem_r8(obj + OBJ_FLAGS_71);
        c->mem_w8(obj + OBJ_FLAGS_71, (uint8_t)(f | 0x02u));
        return;
      }
      case RET_ADVANCE_0_A:
      case RET_ADVANCE_0_B: faKind = 0u; break;
      case RET_ADVANCE_1:   faKind = 1u; break;
      default:
        // ret >= 4: recomp falls through with s0=3 → loop check `s0 == 1` fails → exit.
        return;
    }
    const int faRet = advanceEntry(obj, faKind);
    if (faRet != 1) return;   // any non-1 return exits the loop (matches `beq s0, s2, LOOP`)
  }
}
