// game/scene/script_interp.h — PC-native CUTSCENE SCRIPT INTERPRETER.
//
// PROPER OOP: one instance per Core, embedded on Engine (`eng(c).script`). Back-pointer wired
// at Core construction (same pattern as Sfx / SceneEvents / Cull). No `extern "C"` shims.
//
// SCOPE — the resident cutscene-script bytecode machine at guest 0x80040CDC / DE0 / E54 / 41098
// (all in MAIN.EXE .text). This is the interpreter A06 (and other overlays) hand a script pointer
// to; the per-frame `step` iterates through the bytecode until the current opcode says "yield". The
// script tables themselves live in per-area overlay data (e.g. A06 has scripts at 0x80149A20 and
// 0x80149CDC that drive cutscene fades via op 0x03E "call fnptr"). See docs/findings/scene.md
// "Cutscene SCRIPT INTERPRETER" for the full RE.
//
// ENTRY FORMAT — 8 bytes: { u16 opcodeWord, u16 argA, u16 argB, u16 argC }.
//   If opcodeWord bit 0x2000 is set → the entry is 16 bytes (extra { u16 x 4 } block at offset +8).
//   The OPCODE ID is `opcodeWord & 0x07FF` (low 11 bits). The top 5 bits are FLAGS: 0x0800 = cond,
//   0x1000 = "valid entry" seen on every real entry, 0x2000 = has extra block, 0x4000 = sets
//   obj[+0x71] bit 2 at init, 0x8000 = advance modifier for the entry advance.
//
// HANDLER TABLE — 63 entries at `0x800A3B78` (resident MAIN.EXE .rodata), each a 32-bit fn ptr into
// MAIN.EXE `0x8004xxxx`. Indexed by opcode ID (0..62). Op 0x03E is the "call fnptr" mechanism the
// script-driven cutscene fade dispatch rides — it does `lw v0, 0x74(a0); jalr v0` where obj[+0x74]
// is the 32-bit fnptr the previous entry loaded from the script.
//
// NATIVE-ROUTING FOR OP 0x03E — step()'s op-0x03E path calls `eng(c).behaviors.dispatchObj(obj,
// fnptr)` instead of doing a raw jalr; that routes the fnptr through the standard native-vs-
// substrate registry (BehaviorDispatch::kTable). Any script-driven fade fn REGISTERED as a native
// `beh_*` in that table runs native; unregistered addresses fall through to `rec_dispatch` (the
// substrate leaf). This is the top-down doctrine — no revived `rec_set_override`.
//
// OBJECT LAYOUT (the fields this interpreter reads/writes on the driven object) — same shape the
// guest code uses, so a scriptPtr installed by substrate is picked up correctly here and vice
// versa:
//   +0x10  u32  runtime scratch (init clears)
//   +0x46  u8   0xFF marker set at init
//   +0x64..+0x6A  4x u16  extended-entry block (loaded when opcode bit 0x2000 set)
//   +0x6C  u32  current script pointer (advances as the interpreter walks)
//   +0x70  s8   loop guard / progress counter (step exits when <= 0)
//   +0x71  u8   flag byte (bit 1 = paused, bit 2 = 0x4000-flag)
//   +0x72  u16  argA of current entry
//   +0x74  u16  argB of current entry (low half of fnptr for op 0x03E)
//   +0x76  u16  argC of current entry (high half of fnptr for op 0x03E)
//   +0x78  u8   init clears
//   +0x7C  u32  script's tableA (secondary table pointer, set at init)
#pragma once
#include <cstdint>
struct Core;

class ScriptInterp {
public:
  Core* core = nullptr;

  // Guest addresses this class owns (registered in BehaviorDispatch::kTable so a native caller can
  // route to this class transparently). Kept as `constexpr` so callers can reference them without
  // magic hex.
  static constexpr uint32_t kInitAddr    = 0x80040CDCu;  // FUN_80040CDC — init(obj, tableA, scriptPtr)
  static constexpr uint32_t kLoadAddr    = 0x80040DE0u;  // FUN_80040DE0 — loadCurrentEntry(obj, scriptPtr)
  static constexpr uint32_t kAdvanceAddr = 0x80040E54u;  // FUN_80040E54 — advanceEntry(obj, kindArg)
  static constexpr uint32_t kStepAddr    = 0x80041098u;  // FUN_80041098 — step(obj)
  static constexpr uint32_t kOp03EAddr   = 0x800412CCu;  // op-table[0x3E] handler — call fnptr

  // Verified-and-wired opcode handler addresses (frontier tier, 2026-07-10 — see registerOverrides()).
  static constexpr uint32_t kOp05Addr = 0x80042090u;  // op05WaitFrames
  static constexpr uint32_t kOp06Addr = 0x800420ACu;  // op06TestSceneFlag
  static constexpr uint32_t kOp34Addr = 0x80042E10u;  // op34ClaimGate
  static constexpr uint32_t kOp36Addr = 0x80043108u;  // op36MoveTowardScriptTarget
  static constexpr uint32_t kOp31Addr = 0x80041468u;  // op31TurnTowardTarget

  // The MAIN.EXE handler table base (63 entries, indexed by `opcodeWord & 0x07FF`).
  static constexpr uint32_t kHandlerTableBase = 0x800A3B78u;

  // init(obj, tableA, scriptPtr): FUN_80040CDC. Wire an object as script-driven — set the secondary
  //   table + zero the progress/flag/scratch, then load the first entry via loadCurrentEntry.
  void init(uint32_t obj, uint32_t tableA, uint32_t scriptPtr);

  // loadCurrentEntry(obj, scriptPtr): FUN_80040DE0. Copy the current entry's 3 arg words into
  //   obj[+0x72..+0x76]; if the opcode word's flag 0x2000 is set, also copy the extra 4 halfword
  //   block at scriptPtr+8 into obj[+0x64..+0x6A]. Records scriptPtr into obj[+0x6C].
  void loadCurrentEntry(uint32_t obj, uint32_t scriptPtr);

  // advanceEntry(obj, kindArg): FUN_80040E54. Advance the script pointer per the current entry's
  //   top-3 flag bits (opcodeWord & 0xE000): 0x2000/0x4000/0x6000 = 16/24 bytes ahead, 0x8000/
  //   0xA000/0xC000/0xE000 = follow a branch pointer stored within the entry. Then re-loads the
  //   new current entry. Returns a small status code (0 or the entry's advance kind).
  int advanceEntry(uint32_t obj, uint32_t kindArg);

  // step(obj): FUN_80041098 — the dispatch loop. Read the current opcode, index the handler
  //   table at 0x800A3B78, invoke the handler, act on its return code (0=pause, 1=re-run,
  //   2=advance, 3=advance-branch, other=exit). Loops while obj[+0x70] > 0. Op 0x03E goes through
  //   the class-owned callFnptr() below so registered natives take over automatically.
  void step(uint32_t obj);

  // callFnptr(obj): the native reimplementation of op 0x03E's handler at guest 0x800412CC. Reads
  //   the 32-bit fnptr composed from obj[+0x74] (low) + obj[+0x76] (high) and routes it through
  //   BehaviorDispatch::dispatchObj(obj, fnptr). Returns 0 (the guest handler's return convention
  //   — signals "advance" in the step loop; matches guest fall-through to jr ra after the jalr).
  int callFnptr(uint32_t obj);

  // ---- Opcode-table handlers (0x800A3B78, 63 entries) — VERIFIED + WIRED (frontier tier, 2026-07-10) ----
  // These read/write generic obj fields that step()/loadCurrentEntry() already load per-entry:
  //   argA = obj[+0x72] (OBJ_ARG_A_72), argB = obj[+0x74] (OBJ_FNPTR_LO_74, reused as a plain u16
  //   arg by every opcode except 0x3E), argC = obj[+0x76] (OBJ_FNPTR_HI_76, ditto). obj[+0x78]
  //   (OBJ_SCRATCH_78) doubles as a per-opcode PHASE byte for multi-call ops (34/36/31 below), same
  //   slot init() clears to 0. Verified against the resident opcode table read out of MAIN.EXE
  //   .rodata directly (scratch/re/opcode_table.txt, this session): table[5]=0x80042090,
  //   table[6]=0x800420AC, table[31]=0x80041468, table[34]=0x80042E10, table[36]=0x80043108 — so
  //   these ARE opcode handlers, called as handler(obj) with ABI a0=obj / ret=v0, exactly the shape
  //   step()'s non-0x3E dispatch already uses via rec_dispatch(handler). WIRED (frontier tier,
  //   2026-07-10) via registerOverrides() below — step()'s rec_dispatch(handler) call needed no
  //   change since the override registry is already the interception point on that call path. The
  //   remaining 58 opcode handlers stay substrate.

  // op05 — FUN_80042090: "wait N frames". Decrements argA each call; returns 0 (RET_PAUSE) while
  //   still counting down, or -1 (0xFFFFFFFF — matches NO case in step()'s ret-code switch, so it
  //   falls to `default`: exit without pause-flag, without advance) once the decremented value goes
  //   negative. No object state besides argA touched. Faithful from generated/shard_7.c:5216 (leaf,
  //   no frame).
  int op05WaitFrames(uint32_t obj);

  // op06 — FUN_800420AC: "test scene-flag byte" (the COND-flagged script entries' predicate — see
  //   the 0x0800 opcode-word flag bit in the header comment above). Reads a shared byte array at
  //   guest 0x800BF870+324 (DAT_800bf870, the same area/scene-context anchor documented elsewhere
  //   in docs/engine_re.md — table purpose beyond "flag byte array" NOT traced this pass), indexed
  //   by argB, then combines with argC per argA's 3-way mode:
  //     argA==0 -> return 1 iff table[argB] == argC (exact match)
  //     argA==1 -> return 1 iff (table[argB] & argC) != 0   (any masked bit set)
  //     argA==2 -> return 1 iff (table[argB] & argC) == 0   (masked bits all clear)
  //     argA>=3 -> return 0 (falls through the guest's own unreachable default, byte-shape says 0)
  //   Faithful from generated/shard_0.c:5231 (leaf, no frame).
  int op06TestSceneFlag(uint32_t obj);

  // op34 — FUN_80042E10: "claim-and-wait on a shared 1-byte gate" at guest 0x800BF80F (§9 re-verify
  //   2026-07-10: the original wide-RE draft had this at 0x800BF86F, an 0x60 transcription slip —
  //   recomputed from generated/shard_2.c:4772's own constant math (32780<<16 + -2040 + 7, confirmed
  //   independently against the poll path's + -2033); NOT part of op06's 0x800BF870 table family —
  //   consumer/producer of this specific byte NOT traced this pass). Uses obj[+0x78] as a 2-phase
  //   counter (0=claim, 1=poll):
  //     phase 0: if (argA & 7) == 0, skip straight to the phase-increment tail (no claim attempt,
  //       no sign-bit check — return 0/pause, phase becomes 1). Else (claimTag != 0): if the gate
  //       byte currently reads 0, claim it by writing claimTag into the gate byte; then if argA's
  //       sign bit (0x8000 on the raw 16-bit read) is set, return 1 (advance) immediately without
  //       ever polling; otherwise fall through to the same phase++ / return 0 tail.
  //     phase 1: poll the gate byte; return 1 (advance, gate cleared) once it reads 0, else 0
  //       (pause, keep polling).
  //     phase >=2: return 0 (no-op; guest byte-shape never reaches this, kept for parity).
  //   Faithful from generated/shard_2.c:4772 (leaf, no frame).
  int op34ClaimGate(uint32_t obj);

  // ---- FUN_80040FA0 — VERIFIED + WIRED (frontier tier, 2026-07-10) — advanceEntry() now calls this directly ----
  // "Advance sub-machine" wrapper the guest step() calls after every non-pause handler return
  // (see the `RET_ADVANCE_*` comment above `advanceEntry()`). NOTE ON NAMING: the CURRENT
  // `advanceEntry()` method above (kAdvanceAddr = 0x80040E54 per its own doc comment) actually
  // `rec_dispatch`es to THIS address (0x80040FA0), not to 0x80040E54 — i.e. the wired method's
  // real behavior today IS this function's substrate body, reached indirectly. This draft is the
  // faithful native body of 0x80040FA0 itself (which in turn calls the STILL-substrate
  // 0x80040E54 for the raw entry-advance, out of band for this wide-RE pass — not in the assigned
  // address range). Once verified, this should REPLACE `advanceEntry`'s `rec_dispatch(c,
  // 0x80040FA0u)` body (a frontier-tier wiring step, not done here).
  //
  // Byte-shape source: generated/shard_2.c:4564 (gen_func_80040FA0). Frame: sp-=24, spill s0(obj)
  // and ra at +16/+20. Calls the substrate FUN_80040E54(obj, kindArg) [still rec_dispatch'd — out
  // of band], gets a 0..6 index (or >=7 -> return -1 immediately, matching table slot 3's literal),
  // and runs one of 7 straight-line case blocks that manipulate the SAME bytes step()'s own loop
  // uses: obj[+0x70] (OBJ_PROGRESS_70, the step() loop-guard byte) and obj[+0x71] (OBJ_FLAGS_71).
  // Table read directly out of MAIN.EXE (scratch/re/advance_switch_table.txt, this session) at
  // guest 0x8001534C, 7 x u32, values 0x80040FE0/8004100C/8004103C/80041080/80041050/8004105C/
  // 80041070 for index 0..6 respectively (matches the recomp's switch-case literals exactly).
  int advanceStep(uint32_t obj, uint32_t kindArg);

  // ==== Wide-RE pass 2026-07-10 (dedicated follow-up session) — op36/op31 movement-script family ==
  // VERIFIED + WIRED (frontier tier, 2026-07-10 wiring pass). Both cross-checked via Ghidra headless decompile (scratch/decomp/
  // op36_op31_band.c) against generated/shard_3.c / shard_5.c (instruction-exact ground truth) —
  // see game/scene/script_interp.cpp for the per-function commentary and the two self-caught
  // transcription slips (both corrected before landing, documented inline at the fix site).

  // op36 — FUN_80043108 (opcode table index 36, 95 dispatch hits, the highest-traffic unowned leaf
  //   in the original wide-RE band). "Move toward a script-literal target position." The entry's
  //   argA/argB/argC (obj+0x72/0x74/0x76) hold TARGET Z/Y/X directly (NOT a secondary object
  //   pointer, despite the original mapped-only pass's guess — see .cpp banner for the correction);
  //   the extended-block halfword at obj+0x66 (scriptPtr+10) is a turn-mode flag (-1/0/other) and
  //   obj+0x68/0x6A (scriptPtr+12/14) are a flags-word + packed-arg pair forwarded to the still-
  //   unowned stepEventPulse (FUN_80042EA4) leaf once per call. Phase machine on obj+0x78 (0=init:
  //   solve a sqrt+2-div step schedule with the guest's own rec_break(7168)/rec_break(6144) traps,
  //   1=turn toward the computed angle via turnFacing(), 2=pure position interpolation). Frame:
  //   sp-=40, spills s0/s1/s2/s3/s4/ra (all LIVE cross-call locals — s1 survives the sqrt call).
  int op36MoveTowardScriptTarget(uint32_t obj);

  // op31 — FUN_80041468 (opcode table index 31, 11 dispatch hits). "Turn a self-or-designated actor
  //   toward a computed angle." The acted-upon actor is `self` (obj) when argA's sign bit (0x8000)
  //   is CLEAR, or the single GLOBAL secondary-actor slot at scratchpad 0x1F800214 when SET — NOT a
  //   per-entry table (the original mapped-only pass's "0x8064*10000-ish constant table" guess was a
  //   single scratchpad pointer slot, not a table). Phase 0: a 5-way mode switch on argA's low 15
  //   bits (0/1/2/3/10) computes a target angle into obj+0x76 (via Trig::ratan2, modes 1/2/3/10 each
  //   reading a different position-pair — two of them share scratchpad anchors 0x1F800160/0x1F800164
  //   — mode 2 additionally subtracts a 180 degree/2048 half-turn), then snaps-or-arms the acted-upon
  //   actor's facing (target+0x56) toward it. Phase 1: polls turnFacing() every call until it snaps.
  //   Frame: sp-=48, spills s0/s1/ra.
  int op31TurnTowardTarget(uint32_t obj);

  // FUN_80041438 — thin ABI wrapper: turnFacing(obj, targetAngle, step) turns obj's OWN facing angle
  //   field (obj+0x56) toward targetAngle by up to `step` (Q12 angle units), snapping if within
  //   reach — delegates to the leaf stepAngleToward() below at anglePtr = obj+0x56. Both op36 (on
  //   itself) and op31 (on the resolved actor, self-or-global) call this by address, so the object
  //   passed here is NOT always the driven script object. Frame: sp-=24, spills ra only.
  int turnFacing(uint32_t obj, int16_t targetAngle, int16_t step);
  // Guest-ABI twin: reads obj/targetAngle/step from c->r[4..6], mirrors FUN_80041438's own sp-=24/
  // ra-spill frame (nested ON TOP of the caller's already-descended sp), writes result to c->r[2].
  void turnFacingFramed();

  // FUN_8004139C — leaf angle-stepper (no guest frame; called only via turnFacing/turnFacingFramed
  //   above). Nudge the 16-bit angle at `anglePtr` toward `targetAngle` by up to `step` (both Q12/
  //   4096 angle units): snap immediately if already within `step`; otherwise step by `step` in
  //   whichever direction is the SHORT way around the circle (per the masked-delta-vs-2048 test),
  //   then re-test and snap if the remainder is now within `step`. Returns 1 iff it snapped exactly
  //   to targetAngle this call, else 0. Faithful from generated/shard_1.c:6657.
  int stepAngleToward(uint32_t anglePtr, int16_t targetAngle, int16_t step);

  // FUN_80042EA4 — stepEventPulse(obj, flagsPtr, packedArg): op36's per-call "movement event" gate
  //   (obj unused inside except via `*(obj+0x38)` — an owner/parent pointer — for the bit-0x80 arm).
  //   flagsPtr points at a 16-bit flags word (obj+0x68 in every known caller): 0 -> no-op; low 6
  //   bits clear + bit 0x80 set -> an "arm once" latch gated on `*(int*)(obj+0x38)+4` (clears/sets
  //   bit 0x40, firing Sfx::trigger exactly once on the 0->1 edge, returns 1 that call); low 6 bits
  //   clear + bit 0x80 clear -> no-op; low 6 bits nonzero -> fires Sfx::trigger every call the
  //   scratchpad mask at 0x1F80017C has none of flagsWord's bits set (a per-frame repeat pulse,
  //   gated OFF by that shared mask, e.g. a "sound already playing this frame" guard). packedArg
  //   packs (lowByte=sfx id, highByte=pitch bend) — matches Sfx::trigger(id, pan, pitchBend)'s ABI
  //   with pan=0. Frame: sp-=24, spills ra only. Faithful from generated/shard_3.c:11682.
  int stepEventPulse(uint32_t obj, uint32_t flagsPtr, uint32_t packedArg);
  void stepEventPulseFramed();  // guest-ABI twin: obj/flagsPtr/packedArg from c->r[4..6]

  // Wire the §9-verified opcode handlers into the override registry (overrides::install) at their
  // guest addresses (0x80042090/0x800420AC/0x80042E10/0x80043108/0x80041468) — frontier-tier
  // promotion, 2026-07-10. step()'s rec_dispatch(handler) call already routes through the override
  // registry (oracle-gated: core B / psx_fallback never consults the table), so registering here is
  // sufficient — no change needed
  // in step()'s dispatch loop itself. The other draft leaves (turnFacing/stepAngleToward/
  // stepEventPulse/advanceStep) are reached only as C++ callees of the two opcode handlers, not
  // separately dispatched, so they need no table entry of their own.
  void registerOverrides();

  // ---- Resident-leaf sweep (2026-07-17) — five small unowned MAIN.EXE leaves homed here. NOT script
  // opcodes; generic object/scratchpad helpers with no prior native owner. Byte-faithful to their
  // gen_func_* oracles (see script_interp.cpp for the per-function commentary + the ORACLE markers).
  void refreshCachedTailHi(uint32_t obj);   // FUN_80031708 — cache tail node+4, flag byte @ node+3
  void refreshCachedTailLo(uint32_t obj);   // FUN_80031744 — cache tail node+1, flag byte @ node+0
  int  matchesActiveByKind(uint32_t obj);   // FUN_80042170 — self/global match-byte query (0/1)
  int  mirrorGlobalStatusByte();            // FUN_80044090 — copy 0x800E7EAA -> scratchpad 0x1F800207
  int  advanceGauge(uint32_t obj, uint32_t rec);  // FUN_80073194 — wrapping gauge + wrap-event pulse (ready-frame)
};
