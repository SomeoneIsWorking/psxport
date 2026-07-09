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

// ---- op36/op31 movement-script family fields (wide-RE 2026-07-10, dedicated follow-up session) --
// Generic actor-struct offsets these two opcode handlers (and their callees) touch. Named per the
// RE in script_interp.h's op36MoveTowardScriptTarget/op31TurnTowardTarget banners.
constexpr uint32_t OBJ_POS_Z_2E      = 0x2Eu;  // self Z position (also `target`'s Z when read via a
constexpr uint32_t OBJ_POS_Y_32      = 0x32u;  // self Y position   resolved actor pointer, not obj)
constexpr uint32_t OBJ_POS_X_36      = 0x36u;  // self X position
constexpr uint32_t OBJ_FACE_ANGLE_56 = 0x56u;  // "current facing" angle FUN_8004139C steps toward
constexpr uint32_t OBJ_FACE_AUX_58   = 0x58u;  // companion field op31 clears while still turning
constexpr uint32_t OBJ_OWNER_PTR_38  = 0x38u;  // op36's stepEventPulse: *(obj+0x38) = owner/parent ptr
constexpr uint32_t OBJ_STEPS_REM_44  = 0x44u;  // op36: Q12 "fraction of move remaining", starts 0x1000
constexpr uint32_t OBJ_DZ_48         = 0x48u;  // op36: targetZ - selfZ
constexpr uint32_t OBJ_DY_4A         = 0x4Au;  // op36: targetY - selfY
constexpr uint32_t OBJ_DX_4C         = 0x4Cu;  // op36: targetX - selfX (raw truncated, not sign-ext)
// OBJ_EXTRA_BLOCK (0x64) is dual-purpose for op36: +0 (0x64) holds the requested step-count in its
// HIGH byte at init, then gets overwritten with the solved per-call step divisor; +2 (0x66) is the
// turn-mode flag / second turn-target angle; +4 (0x68) / +6 (0x6A) are stepEventPulse's flagsPtr
// target + packed (id,pitch) arg.
constexpr uint32_t OBJ_STEP_DIV_64   = OBJ_EXTRA_BLOCK + 0u;
constexpr uint32_t OBJ_TURN_ANGLE2_66 = OBJ_EXTRA_BLOCK + 2u;
constexpr uint32_t OBJ_EVENT_FLAGS_68 = OBJ_EXTRA_BLOCK + 4u;
constexpr uint32_t OBJ_EVENT_ARG_6A   = OBJ_EXTRA_BLOCK + 6u;

// Scratchpad globals op31 reads (resident 0x1F800000 region; same "8064<<16" recompiler-literal
// convention documented in melee_proximity.cpp / object_table.cpp for scratchpad-relative constants).
constexpr uint32_t SCRATCH_BASE       = 0x1F800000u;
constexpr uint32_t SCRATCH_ANCHOR_X_164 = SCRATCH_BASE + 0x164u;  // op31 mode 1/2 anchor X
constexpr uint32_t SCRATCH_ANCHOR_Z_160 = SCRATCH_BASE + 0x160u;  // op31 mode 1/2 anchor Z
constexpr uint32_t SCRATCH_SECONDARY_ACTOR_214 = SCRATCH_BASE + 0x214u;  // op31's global secondary actor ptr
constexpr uint32_t SCRATCH_EVENT_MASK_17C = SCRATCH_BASE + 0x17Cu;       // stepEventPulse repeat-gate mask

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

// =================================================================================================
// Wide-RE pass 2026-07-10 (dedicated follow-up session) — op36/op31 movement-script family.
// Method-by-method commentary is in script_interp.h; this banner covers the shared verification
// method. Source of truth: generated/shard_5.c:5667 (op36) / generated/shard_3.c:11362 (op31) /
// generated/shard_2.c:4628 (turnFacing) / generated/shard_1.c:6657 (stepAngleToward) /
// generated/shard_3.c:11682 (stepEventPulse) — instruction-exact per CLAUDE.md. Cross-checked
// against Ghidra headless decompile (scratch/decomp/op36_op31_band.c, this session) for structure;
// ONE real Ghidra-vs-raw-C divergence was caught and resolved in op36's favor of the raw C (Ghidra's
// decompile misplaced a `trap(0x1c00)` call right after the first division's step-count clamp —
// no such trap exists there in the recompiled C; the real div-by-zero trap at that spot belongs to
// the SECOND division a few lines later and Ghidra folded it into the wrong position). A second,
// self-caught slip (this session's own first hand-trace of op31's raw C, corrected against a second
// re-read + Ghidra agreement) is noted inline at its fix site below. Frames are guest-stack-mirrored
// per CLAUDE.md ("MIRROR THE GUEST STACK") even though this pass is unwired/ungated, so the eventual
// wiring's SBS gate has nothing left to fix on the stack-fidelity front.
// =================================================================================================

// FUN_8004139C — leaf angle-stepper (no guest frame). See script_interp.h for the semantics.
int ScriptInterp::stepAngleToward(uint32_t anglePtr, int16_t targetAngle, int16_t step) {
  Core* c = core;
  const int16_t cur = (int16_t)c->mem_r16(anglePtr);
  int32_t absDelta = (int32_t)targetAngle - (int32_t)cur;
  if (absDelta < 0) absDelta = -absDelta;
  if (absDelta <= (int32_t)step) {
    c->mem_w16(anglePtr, (uint16_t)targetAngle);
    return 1;
  }
  // Step by `step` toward whichever direction is the SHORT way around the 12-bit angle circle
  // (masked delta < half-turn (0x800) -> forward, else backward).
  int16_t dirStep = step;
  const uint32_t maskedDelta = (uint32_t)((int32_t)targetAngle - (int32_t)cur) & 0xFFFu;
  if (maskedDelta > 0x7FFu) dirStep = (int16_t)-step;
  const int16_t newCur = (int16_t)(cur + dirStep);
  c->mem_w16(anglePtr, (uint16_t)newCur);
  const uint32_t maskedDelta2 = (uint32_t)((int32_t)targetAngle - (int32_t)newCur) & 0xFFFu;
  if ((int32_t)maskedDelta2 <= (int32_t)step) {
    c->mem_w16(anglePtr, (uint16_t)targetAngle);
    return 1;
  }
  return 0;
}

// FUN_80041438 — thin wrapper: turnFacing(obj, targetAngle, step) = stepAngleToward(obj+0x56, ...).
// `obj` here is whichever actor is being turned — NOT always the script-driven object (op31 calls
// this on its resolved target actor; op36 calls it on itself).
int ScriptInterp::turnFacing(uint32_t obj, int16_t targetAngle, int16_t step) {
  return stepAngleToward(obj + OBJ_FACE_ANGLE_56, targetAngle, step);
}

// Guest-ABI twin — mirrors FUN_80041438's own sp-=24 / ra-spill-at-+16 frame, nested on top of
// whatever the CALLER (op36/op31) already descended c->r[29] to. Args/result via a0..a2/v0.
void ScriptInterp::turnFacingFramed() {
  Core* c = core;
  const uint32_t obj = c->r[4];
  const int16_t targetAngle = (int16_t)c->r[5];
  const int16_t step = (int16_t)c->r[6];
  const uint32_t savedSp = c->r[29];
  const uint32_t savedRa = c->r[31];
  c->r[29] = savedSp - 24u;
  c->mem_w32(c->r[29] + 16u, savedRa);
  c->r[2] = (uint32_t)turnFacing(obj, targetAngle, step);
  c->r[31] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = savedSp;
}

// FUN_80042EA4 — see script_interp.h for the full semantics writeup.
int ScriptInterp::stepEventPulse(uint32_t obj, uint32_t flagsPtr, uint32_t packedArg) {
  Core* c = core;
  const uint16_t flagsWord = c->mem_r16(flagsPtr);
  const uint8_t argLo = (uint8_t)(packedArg & 0xFFu);
  const int8_t argHi = (int8_t)((packedArg >> 8) & 0xFFu);
  if (flagsWord == 0) return 0;
  if ((flagsWord & 0x3Fu) == 0) {
    if ((flagsWord & 0x80u) == 0) return 0;
    const uint32_t ownerPtr = c->mem_r32(obj + OBJ_OWNER_PTR_38);
    const uint8_t ownerFlag = c->mem_r8(ownerPtr + 4u);
    if (ownerFlag == 0) {
      c->mem_w16(flagsPtr, (uint16_t)(flagsWord & 0xFFBFu));  // clear bit 0x40
      return 0;
    }
    if ((flagsWord & 0x40u) != 0) return 0;  // already latched — wait for the consumer to clear it
    c->mem_w16(flagsPtr, (uint16_t)(flagsWord | 0x40u));
    c->engine.sfx.trigger(argLo, 0, argHi);
    return 1;
  }
  // low 6 bits nonzero: fire every call the shared scratchpad mask has none of flagsWord's bits set.
  const uint16_t scratchMask = c->mem_r16(SCRATCH_EVENT_MASK_17C);
  if ((scratchMask & flagsWord) == 0) {
    c->engine.sfx.trigger(argLo, 0, argHi);
  }
  return 0;
}

// Guest-ABI twin — mirrors FUN_80042EA4's own sp-=24 / ra-spill-at-+16 frame.
void ScriptInterp::stepEventPulseFramed() {
  Core* c = core;
  const uint32_t obj = c->r[4];
  const uint32_t flagsPtr = c->r[5];
  const uint32_t packedArg = c->r[6];
  const uint32_t savedSp = c->r[29];
  const uint32_t savedRa = c->r[31];
  c->r[29] = savedSp - 24u;
  c->mem_w32(c->r[29] + 16u, savedRa);
  c->r[2] = (uint32_t)stepEventPulse(obj, flagsPtr, packedArg);
  c->r[31] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = savedSp;
}

// op36 — FUN_80043108 (opcode table index 36). See script_interp.h for the summary. The labels below
// (`afterInit`/`commit`) exist purely to preserve the guest's own branch-delay-slot control flow
// exactly (a MIPS instruction after a compare/branch executes UNCONDITIONALLY before the branch
// decision takes effect) rather than to "clean up" it — see the file banner above.
int ScriptInterp::op36MoveTowardScriptTarget(uint32_t obj) {
  Core* c = core;
  // ---- guest frame mirror: sp-=40; spill s0@+16 s4@+32 ra@+36 s3@+28 s2@+24 s1@+20 (LIVE values).
  const uint32_t savedSp = c->r[29], savedRa = c->r[31];
  const uint32_t savedS0 = c->r[16], savedS1 = c->r[17], savedS2 = c->r[18];
  const uint32_t savedS3 = c->r[19], savedS4 = c->r[20];
  c->r[29] = savedSp - 40u;
  c->r[16] = obj;   // s0 = obj
  c->r[20] = 1u;    // s4 = constant 1
  c->mem_w32(c->r[29] + 16u, savedS0);
  c->mem_w32(c->r[29] + 32u, savedS4);
  c->mem_w32(c->r[29] + 36u, savedRa);
  c->mem_w32(c->r[29] + 28u, savedS3);
  c->mem_w32(c->r[29] + 24u, savedS2);
  c->mem_w32(c->r[29] + 20u, savedS1);

  auto epilogue = [&](int32_t ret) -> int32_t {
    c->r[31] = c->mem_r32(c->r[29] + 36u);
    c->r[20] = c->mem_r32(c->r[29] + 32u);
    c->r[19] = c->mem_r32(c->r[29] + 28u);
    c->r[18] = c->mem_r32(c->r[29] + 24u);
    c->r[17] = c->mem_r32(c->r[29] + 20u);
    c->r[16] = c->mem_r32(c->r[29] + 16u);
    c->r[29] = savedSp;
    return ret;
  };

  const uint32_t scriptPtr = c->mem_r32(obj + OBJ_SCRIPT_PTR);   // s2 — the CURRENT script entry;
  c->r[18] = scriptPtr;                                          // +2/+4/+6 are its argA/B/C (target
                                                                  // Z/Y/X), +10 is extended-block #2
  const uint8_t phase = c->mem_r8(obj + OBJ_SCRATCH_78);          // bVar1
  const int16_t sVar2 = (int16_t)c->mem_r16(scriptPtr + 10u);     // s3 — turn-mode flag: -1/0/other
  c->r[19] = (uint32_t)(uint16_t)sVar2;

  if (phase != 1) {
    if (phase > 1) {
      if (phase != 2) return epilogue(0);   // dead per guest byte-shape, kept for parity
      goto commit;                           // LAB_80043310 direct
    }
    if (phase != 0) return epilogue(0);      // dead: phase is guaranteed 0 in this arm

    // ---- phase 0: init the movement (solve a sqrt+2-div step schedule; compute turn angle(s)). ---
    {
      int32_t stepsRequested = (int32_t)(int8_t)(c->mem_r16(obj + OBJ_STEP_DIV_64) >> 8);
      if (stepsRequested == 0) stepsRequested = 10;
      c->r[17] = (uint32_t)stepsRequested;   // s1 — LIVE cross-call local (survives the sqrt call)

      const int16_t targetZ = (int16_t)c->mem_r16(obj + OBJ_ARG_A_72);
      const int16_t targetY = (int16_t)c->mem_r16(obj + OBJ_FNPTR_LO_74);
      const int16_t targetX = (int16_t)c->mem_r16(obj + OBJ_FNPTR_HI_76);
      const int16_t dz = (int16_t)(targetZ - (int16_t)c->mem_r16(obj + OBJ_POS_Z_2E));
      const int16_t dy = (int16_t)(targetY - (int16_t)c->mem_r16(obj + OBJ_POS_Y_32));
      const uint16_t dxRaw = (uint16_t)(targetX - (int16_t)c->mem_r16(obj + OBJ_POS_X_36));
      const int16_t dx = (int16_t)dxRaw;
      c->mem_w16(obj + OBJ_DZ_48, (uint16_t)dz);
      c->mem_w16(obj + OBJ_DY_4A, (uint16_t)dy);
      c->mem_w16(obj + OBJ_DX_4C, dxRaw);

      // sqrt(dx^2 + dz^2) — still-substrate GTE-LZCS sqrt leaf (game/ai/melee_proximity.cpp
      // precedent; NOT Math::isqrt16).
      c->r[4] = (uint32_t)(int32_t)(dx * dx + dz * dz);
      rec_dispatch(c, 0x80084080u);
      const int16_t dist = (int16_t)c->r[2];

      // First guarded division: dist / stepsRequested. Faithfully reproduces the guest's own
      // rec_break(7168) div-by-zero and rec_break(6144) INT_MIN/-1 overflow traps (same cpu_div +
      // rec_break idiom as game/audio/sequencer.cpp) — the overflow trap is dead code for THIS call
      // site (a 16-bit-sourced dividend can never equal INT32_MIN) but is kept for byte-for-byte
      // fidelity with the guest's own bounds check.
      cpu_div(c, (uint32_t)(int32_t)dist, (uint32_t)(int32_t)stepsRequested);
      if (stepsRequested == 0) rec_break(c, 7168u);
      if (stepsRequested == -1 && (int32_t)dist == (int32_t)0x80000000) rec_break(c, 6144u);
      uint16_t stepDiv = (uint16_t)c->lo;
      if (stepDiv == 0) stepDiv = 1;   // clamp: guest re-checks `(quotient & 0xffff)==0` post-trunc
      c->mem_w16(obj + OBJ_STEP_DIV_64, stepDiv);
      // NOTE: Ghidra's decompile placed a THIRD `trap(0x1c00)` right here (testing the just-clamped
      // value for zero again) — that trap does NOT exist in the raw recompiled C at this point; it
      // misplaced the SECOND division's own div-by-zero trap (below) into this spot. Raw generated
      // C (ground truth) has no trap between the clamp and the second cpu_div — confirmed by direct
      // re-read of generated/shard_5.c:5727-5736.

      c->mem_w16(obj + OBJ_STEPS_REM_44, 0x1000u);   // stepsRemaining = 4096 (Q12 "100%")

      // Second guarded division: 4096 / stepDiv (same trap pair; both dead-code here too — divisor
      // can't be exactly -1 with dividend exactly INT32_MIN since the dividend is the literal 4096).
      cpu_div(c, 0x1000u, (uint32_t)(int32_t)(int16_t)stepDiv);
      if ((int16_t)stepDiv == 0) rec_break(c, 7168u);
      if ((int16_t)stepDiv == -1 && 0x1000 == (int32_t)0x80000000) rec_break(c, 6144u);
      stepDiv = (uint16_t)c->lo;
      c->mem_w16(obj + OBJ_STEP_DIV_64, stepDiv);   // per-call step size, overwrites the field again

      // ---- turn-mode dispatch on sVar2 (extended-block halfword #2) ----
      if (sVar2 == -1) {
        c->r[4] = (uint32_t)(int32_t)(-(int32_t)dx);
        c->r[5] = (uint32_t)(int32_t)dz;
        rec_dispatch(c, 0x80085690u);  // Trig::ratan2 (wired override) — ratan2(-dx, dz)
        c->mem_w16(obj + OBJ_FACE_ANGLE_56, (uint16_t)(c->r[2] & 0xFFFu));
      } else if (sVar2 == 0) {
        c->mem_w8(obj + OBJ_SCRATCH_78, 2u);
        return epilogue(0);
      }
      // (sVar2 == -1 falls through from above; sVar2 not in {-1,0} skips the obj+0x56 write but
      // still reaches this second ratan2 call — matches the guest's own fallthrough shape.)
      c->r[4] = (uint32_t)(int32_t)(-(int32_t)dx);
      c->r[5] = (uint32_t)(int32_t)dz;
      rec_dispatch(c, 0x80085690u);
      c->mem_w16(obj + OBJ_TURN_ANGLE2_66, (uint16_t)(c->r[2] & 0xFFFu));
      c->mem_w8(obj + OBJ_SCRATCH_78, (uint8_t)(c->mem_r8(obj + OBJ_SCRATCH_78) + 1u));
    }
  }

  // ---- phase 0 fallthrough / phase 1 entry: turn self toward the computed angle (obj+0x66). ------
  {
    const int16_t turnAngle2 = (int16_t)c->mem_r16(obj + OBJ_TURN_ANGLE2_66);
    c->r[4] = obj;
    c->r[5] = (uint32_t)(int32_t)turnAngle2;
    c->r[6] = 0x100u;
    turnFacingFramed();
    if (c->r[2] != 0) {
      c->mem_w8(obj + OBJ_SCRATCH_78, (uint8_t)(c->mem_r8(obj + OBJ_SCRATCH_78) + 1u));
    }
    if (sVar2 == 1) return epilogue(0);
  }

commit:
  {
    // ---- LAB_80043310: advance stepsRemaining and interpolate self position toward the target. --
    uint16_t stepsRemaining = (uint16_t)(c->mem_r16(obj + OBJ_STEPS_REM_44) -
                                          c->mem_r16(obj + OBJ_STEP_DIV_64));
    if ((int16_t)stepsRemaining <= 0) stepsRemaining = 0;
    c->mem_w16(obj + OBJ_STEPS_REM_44, stepsRemaining);

    const int16_t dz = (int16_t)c->mem_r16(obj + OBJ_DZ_48);
    const int16_t dy = (int16_t)c->mem_r16(obj + OBJ_DY_4A);
    const int16_t dx = (int16_t)c->mem_r16(obj + OBJ_DX_4C);
    const uint32_t curScriptPtr = c->mem_r32(obj + OBJ_SCRIPT_PTR);  // same script entry (s2), re-
                                                                       // read fresh per the guest's
                                                                       // own iVar8 access pattern

    int32_t scaledZ = (int32_t)(int16_t)stepsRemaining * (int32_t)dz;
    if (scaledZ < 0) scaledZ += 0xFFF;
    const int16_t newZ = (int16_t)(c->mem_r16(curScriptPtr + 2u) - (int16_t)(scaledZ >> 12));
    c->mem_w16(obj + OBJ_POS_Z_2E, (uint16_t)newZ);

    int32_t scaledY = (int32_t)(int16_t)stepsRemaining * (int32_t)dy;
    if (scaledY < 0) scaledY += 0xFFF;
    const int16_t newY = (int16_t)(c->mem_r16(curScriptPtr + 4u) - (int16_t)(scaledY >> 12));
    c->mem_w16(obj + OBJ_POS_Y_32, (uint16_t)newY);

    int32_t scaledX = (int32_t)(int16_t)stepsRemaining * (int32_t)dx;
    if (scaledX < 0) scaledX += 0xFFF;
    const int16_t newX = (int16_t)(c->mem_r16(curScriptPtr + 6u) - (int16_t)(scaledX >> 12));
    c->mem_w16(obj + OBJ_POS_X_36, (uint16_t)newX);

    if (stepsRemaining != 0) {
      c->r[4] = obj;
      c->r[5] = obj + OBJ_EVENT_FLAGS_68;
      c->r[6] = (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + OBJ_EVENT_ARG_6A);
      stepEventPulseFramed();
      return epilogue(0);
    }
    return epilogue(1);
  }
}

// op31 — FUN_80041468 (opcode table index 31). See script_interp.h for the summary.
int ScriptInterp::op31TurnTowardTarget(uint32_t obj) {
  Core* c = core;
  // ---- guest frame mirror: sp-=48; spill s0@+32 ra@+40 s1@+36 (LIVE values).
  const uint32_t savedSp = c->r[29], savedRa = c->r[31];
  const uint32_t savedS0 = c->r[16], savedS1 = c->r[17];
  c->r[29] = savedSp - 48u;
  c->r[16] = obj;
  c->mem_w32(c->r[29] + 32u, savedS0);
  c->mem_w32(c->r[29] + 40u, savedRa);
  c->mem_w32(c->r[29] + 36u, savedS1);

  auto epilogue = [&](int32_t ret) -> int32_t {
    c->r[31] = c->mem_r32(c->r[29] + 40u);
    c->r[17] = c->mem_r32(c->r[29] + 36u);
    c->r[16] = c->mem_r32(c->r[29] + 32u);
    c->r[29] = savedSp;
    return ret;
  };

  // Resolve the acted-upon actor: self if argA's sign bit is clear, else the global secondary actor
  // slot at scratchpad 0x1F800214. BUG-FIX note: this session's own FIRST hand-trace of the raw
  // generated C (before writing any code) mislabeled this as "sign set -> self", the opposite of
  // the truth — caught by re-deriving a second time directly from the recompiled C's delay-slot
  // ordering AND independently cross-checking against Ghidra's decompile (both agree: sign bit SET
  // selects the scratchpad global; CLEAR selects self). Fixed before writing any code.
  const int16_t argA = (int16_t)c->mem_r16(obj + OBJ_ARG_A_72);
  const uint32_t target = ((uint16_t)argA & 0x8000u)
                               ? c->mem_r32(SCRATCH_SECONDARY_ACTOR_214)
                               : obj;
  c->r[17] = target;

  const uint8_t phase = c->mem_r8(obj + OBJ_SCRATCH_78);
  if (phase != 0) {
    if (phase != 1) return epilogue(0);
    // ---- phase 1: poll the incremental turn toward the already-computed target angle. -----------
    c->r[4] = target;
    c->r[5] = (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + OBJ_FNPTR_HI_76);   // targetAngle
    c->r[6] = (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + OBJ_FNPTR_LO_74);   // step/threshold
    turnFacingFramed();
    return epilogue((int32_t)c->r[2]);
  }

  // ---- phase 0: compute a fresh target angle into obj+0x76 per the low-15-bit mode in argA. ------
  {
    const uint16_t mode = (uint16_t)argA & 0x7FFFu;
    if (mode == 2) {
      // mode 2: ratan2(anchor vs target) then flip 180 degrees (2048 == half the 4096-unit circle).
      const int32_t y = (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_X_36) -
                         (int32_t)(int16_t)c->mem_r16(SCRATCH_ANCHOR_X_164);
      const int32_t x = (int32_t)(int16_t)c->mem_r16(SCRATCH_ANCHOR_Z_160) -
                         (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_Z_2E);
      c->r[4] = (uint32_t)y; c->r[5] = (uint32_t)x;
      rec_dispatch(c, 0x80085690u);
      const int32_t angle = (int32_t)c->r[2] - 2048;
      c->mem_w16(obj + OBJ_FNPTR_HI_76,
                 (uint16_t)(c->mem_r16(obj + OBJ_FNPTR_HI_76) + ((uint32_t)angle & 0xFFFu)));
    } else if (mode < 3) {
      if (mode == 1) {
        // mode 1: same ratan2(anchor vs target) as mode 2, WITHOUT the 180-degree flip.
        const int32_t y = (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_X_36) -
                           (int32_t)(int16_t)c->mem_r16(SCRATCH_ANCHOR_X_164);
        const int32_t x = (int32_t)(int16_t)c->mem_r16(SCRATCH_ANCHOR_Z_160) -
                           (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_Z_2E);
        c->r[4] = (uint32_t)y; c->r[5] = (uint32_t)x;
        rec_dispatch(c, 0x80085690u);
        c->mem_w16(obj + OBJ_FNPTR_HI_76,
                   (uint16_t)(c->mem_r16(obj + OBJ_FNPTR_HI_76) + (c->r[2] & 0xFFFu)));
      }
      // mode 0: no ratan2, no obj+0x76 write — use the target angle already sitting there. Falls
      // straight to the commit tail below (matches the guest's own unconditional fallthrough).
    } else if (mode == 3) {
      // mode 3: ratan2(target actor vs self) — dx = target.X - self.X, dz = self.Z - target.Z.
      const int32_t y = (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_X_36) -
                         (int32_t)(int16_t)c->mem_r16(obj + OBJ_POS_X_36);
      const int32_t x = (int32_t)(int16_t)c->mem_r16(obj + OBJ_POS_Z_2E) -
                         (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_Z_2E);
      c->r[4] = (uint32_t)y; c->r[5] = (uint32_t)x;
      rec_dispatch(c, 0x80085690u);
      c->mem_w16(obj + OBJ_FNPTR_HI_76,
                 (uint16_t)(c->mem_r16(obj + OBJ_FNPTR_HI_76) + (c->r[2] & 0xFFFu)));
    } else if (mode == 10) {
      // mode 10: track a LIVE point via obj+0x76/obj+0x74 (already-set target angle/threshold from a
      // PRIOR call) vs the target actor's own position, then OVERWRITE (not +=) obj+0x76 and reset
      // obj+0x74 to the fixed turn-step constant 0x100 — a "continuously re-aim" mode.
      const int32_t y = (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_X_36) -
                         (int32_t)(int16_t)c->mem_r16(obj + OBJ_FNPTR_HI_76);
      const int32_t x = (int32_t)(int16_t)c->mem_r16(obj + OBJ_FNPTR_LO_74) -
                         (int32_t)(int16_t)c->mem_r16(target + OBJ_POS_Z_2E);
      c->r[4] = (uint32_t)y; c->r[5] = (uint32_t)x;
      rec_dispatch(c, 0x80085690u);
      c->mem_w16(obj + OBJ_FNPTR_HI_76, (uint16_t)(c->r[2] & 0xFFFu));
      c->mem_w16(obj + OBJ_FNPTR_LO_74, 0x100u);
    }
    // (mode not in {0,1,2,3,10}: no-op, falls straight to the commit tail — matches the guest's own
    // unconditional fallthrough for any other value.)
  }

  // ---- commit tail: snap-or-arm the acted-upon actor's facing (target+0x56) toward obj+0x76. -----
  {
    const int16_t targetAngle = (int16_t)c->mem_r16(obj + OBJ_FNPTR_HI_76);
    const int16_t curAngle = (int16_t)c->mem_r16(target + OBJ_FACE_ANGLE_56);
    const int16_t threshold = (int16_t)c->mem_r16(obj + OBJ_FNPTR_LO_74);
    const uint32_t maskedDelta = (uint32_t)((int32_t)targetAngle - (int32_t)curAngle) & 0xFFFu;
    // BUG-FIX note: snap unless (maskedDelta >= threshold AND threshold > 0) — i.e. snap when
    // maskedDelta < threshold, OR when threshold <= 0 (a non-positive threshold forces an instant
    // snap). Re-derived directly from the raw generated C's fallthrough-into-snap when NEITHER of
    // its two branch checks is taken; an initial reading that treated the fallthrough as "keep
    // turning" would have inverted this polarity — caught before writing any code.
    if ((int32_t)maskedDelta < (int32_t)threshold || threshold <= 0) {
      c->mem_w16(target + OBJ_FACE_ANGLE_56, (uint16_t)targetAngle);  // SNAP
      return epilogue(1);
    }
    c->mem_w16(target + OBJ_FACE_AUX_58, 0u);
    c->mem_w8(obj + OBJ_SCRATCH_78, (uint8_t)(c->mem_r8(obj + OBJ_SCRATCH_78) + 1u));
    return epilogue(0);
  }
}
