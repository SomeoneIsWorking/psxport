// class ScreenFade — the screen-fade subsystem, class-shaped port of the PSX FUN_8007e9c8 leaf.
//
// PROPER OOP: an instance per Core (embedded as `Core::screenFade`). Callers use it as
// `fade(c).method(args)`. Back-pointer to Core stored once at Game construction time.
//
// PSX behaviour (RE'd from FUN_8007e9c8, MAIN.EXE 0x8007e9c8..0x8007eadc):
//   fade is TWO chained GP0 packets appended to OT slot `a2` (typically 4). The builder:
//     1. Writes a temp GsBOXF header in scratchpad 0x1F800004..0x1F80000F (color, attr=0x62, 0,0 320x240)
//     2. Reads the packet-pool write ptr from 0x800BF544; call it P.
//     3. Reads the OT slot address = *0x800ED8C8 + a2*4. Chains packet 1:
//          pool[P+0]  = (*OT_slot) | 0x03000000   (3-word packet link tag)
//          *OT_slot   = P
//          pool[P+4]  = color word
//          pool[P+8]  = xy word    (=0)
//          pool[P+12] = wh word    (=(240<<16)|320)
//        Sets pool_ptr = P+16, writes it back to 0x800BF544.
//     4. Calls FUN_80083de0(pool[P+16], 0,0, 32/64, 0) — builds packet 2 (semi-tp attr).
//        FUN_80083de0 advances 0x800BF544 internally.
//     5. Chains packet 2 at pool[P+16]: link tag = (*OT_slot) | 0x02000000, *OT_slot = P+16.
//     6. Advances 0x800BF544 by 12 more (past the callee's writes + our link).
//   The two packets together render a full-screen semi-transparent rect that blends over the frame.
//
// PC-native model:
//   Native STATE lives on this class as C++ members (host memory) — the frame-scoped fade `{mode,r,g,b}`,
//   reset to NONE at the top of every logic frame by frameStart(). NO guest-RAM shadow at some invented
//   BSS address — that would produce a divergence in gameplay-mode SBS where B substrate never touches it.
//   The native renderer reads this class's members via `get()` to draw the fade.
//
//   `set()` / `applyLeafCall()` are host-state-only: they update the C++ members and write nothing
//   to guest RAM, in BOTH pc_skip modes. The guest packet-pool + scratchpad writes of FUN_8007e9c8
//   fire only where a still-substrate caller runs the recomp body directly.
//
//   Still-recomp fade callers reach this class through installLeafTap() — a shard-level override on
//   FUN_8007e9c8 (sanctioned leaf-engine global ownership, CLAUDE.md engine-overrides directive) that
//   runs the original gen body for byte-exact guest state and mirrors the args into the host frame
//   state. Before the tap (pre-2026-07-16), substrate callers ran gen directly and the class never
//   saw them — the fisherman-cutscene fade-in (#63) presented at full brightness because its ramp
//   only existed as guest OT packets.
//
//   NO cross-frame "held fully-faded" latch. An earlier revision inferred a persistent black/white HOLD
//   from the last fade value's magnitude (a magic-threshold heuristic — banned by the no-magic-constant
//   rule) whenever no caller called set() that frame, to avoid a residual-VRAM flicker during a few
//   known menu/loading admin frames. It broke free-roam: the sm[0x4e] new-game bootstrap transition
//   (Engine::fieldRun case 10, guest FUN_80106b98) ramps to full black via sm+0x6e, then hands off through
//   states 7/8/6/0 (none of which call the fade leaf, on PSX OR here — RE'd via Ghidra decompile of
//   FUN_80106b98) straight into steady case 1 gameplay, which likewise never calls the fade leaf again.
//   On PSX this is correct: OT slot 4 goes unwritten those frames -> empty -> scene renders through
//   immediately. The inferred HOLD instead latched the last (near-black) value FOREVER, since nothing
//   ever called set() with a lower value to "release" it — a permanently black composite. Matching PSX:
//   frameStart() resets to NONE every frame, full stop; a caller that needs a color held across multiple
//   frames (real PSX callers always do, e.g. submitPage810c's pause-menu dim below) calls set()/
//   applyLeafCall() EVERY one of those frames itself, which is already how every ported SM in this file
//   behaves. See docs/findings/render.md "ScreenFade held-latch permanent black".
#pragma once
#include <cstdint>
class Core;

class ScreenFade {
public:
  enum Mode : uint8_t {
    NONE        = 0,   // no fade this frame -> scene renders normally
    ADDITIVE    = 1,   // dst += (r,g,b) clamped -> fade to/from white
    SUBTRACTIVE = 2,   // dst -= (r,g,b) clamped -> fade to/from black
  };

  struct State { Mode mode; uint8_t r, g, b; };

  // Canonical OT slot for full-screen fades — every substrate caller in Tomba passes 4. Callers
  // that pass a different slot override via the explicit `otSlot` parameter of applyLeafCall.
  static constexpr uint32_t DEFAULT_OT_SLOT = 4;

  // Back-pointer set once by Game's constructor. Not owned; ScreenFade never outlives its Core.
  Core* core = nullptr;

  // Called ONCE at the top of each logic frame. Resets the FRAME-SCOPED state to NONE. Does NOT
  // touch the held fully-faded state — that persists across admin frames.
  void frameStart();

  // Set the fade for THIS FRAME. Host-state-only: no guest-RAM writes in either pc_skip mode
  // (`otSlot` is accepted for the guest-ABI shape but unused). Last call wins for this frame; a
  // caller that needs the fade held across multiple frames must call this every one of them
  // (matches PSX: OT slot 4 is rebuilt fresh each frame, so an unwritten frame renders no rect).
  void set(Mode mode, uint8_t r, uint8_t g, uint8_t b, uint32_t otSlot = DEFAULT_OT_SLOT);

  // Guest ABI convenience: `color` is 0x00RRGGBB in a0; `a1` selects blend (a1!=0 => ADDITIVE / white,
  // a1==0 => SUBTRACTIVE / black); `otSlot` is a2 in the substrate ABI (typically 4).
  void applyLeafCall(uint32_t color, uint32_t a1, uint32_t otSlot = DEFAULT_OT_SLOT);

  // sequence(node): the GAME-overlay a0l per-node fade sequencer (guest FUN_8010957C, was
  // ov_scene_fade_seq / Engine::fadeSequencer). Multi-step ramp SM driven by node+2 (outer
  // state 0=init, 1=running), node+3 (running-substep 0..5), node+106 (fade level 0..31),
  // node+104 (step-2 delay counter). Called by Engine::fieldRun's sm[0x4e]==0xb branch with
  // node = 0x800E8008. Each substep drives `applyLeafCall` at a computed ramp level; the two
  // still-substrate leaves it touches (0x8010CC68 helper, 0x8010D030 init poke) stay dispatched.
  void sequence(uint32_t node);

  // Read this frame's effective state (native renderer's present prologue). Returns the frame-scoped
  // state set by a caller this frame, otherwise NONE (matches PSX: nobody wrote OT slot 4 this frame).
  State get() const;

  // Global ownership of the FUN_8007e9c8 leaf (engine-overrides directive): installs an oracle-gated
  // shard override that runs the original gen body (guest state byte-exact) and mirrors the args into
  // this class — so STATIC gen-to-gen fade callers (invisible to rec_dispatch) also feed get().
  // Idempotent; called from Game setup alongside the other *_install() wirings.
  static void installLeafTap();

private:
  // Host-owned state (was previously shadowed at guest 0x800E7DE0..7, an invented BSS address that
  // the substrate never touched — see class header note for the removal rationale).
  Mode    mFrameMode = NONE;
  uint8_t mFrameR    = 0;
  uint8_t mFrameG    = 0;
  uint8_t mFrameB    = 0;

  // `debug fadetrace` channel — logs every native-path fade call with the calling context; prints
  // the C++ backtrace only on FIRST occurrence of a given (op,mode,rgb) tuple (mSeen dedupe).
  void fadetrace(const char* op, uint8_t mode, uint32_t rgb, const char* extra);
  int      mTraceOn = -1;      // lazy cfg latch
  uint32_t mSeen[64] = {};     // first-time (op,mode,rgb) dedupe keys
  int      mSeenN = 0;
};
