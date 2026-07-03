// game/scene/script_interp.h — PC-native CUTSCENE SCRIPT INTERPRETER.
//
// PROPER OOP: one instance per Core, embedded on Engine (`c->engine.script`). Back-pointer wired
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
// NATIVE-ROUTING FOR OP 0x03E — step()'s op-0x03E path calls `c->engine.behaviors.dispatchObj(obj,
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
};
