// class ScreenFade — the screen-fade subsystem, class-shaped port of the PSX FUN_8007e9c8 leaf.
//
// PROPER OOP: an instance per Core (embedded as `Core::screenFade`). Callers use it as
// `c->screenFade.method(args)`. Back-pointer to Core stored once at Game construction time.
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
//   Native STATE lives on this class as C++ members (host memory) — the frame-scoped fade `{mode,r,g,b}`
//   and a held-fully-faded latch (see FULLY_FADED_THRESHOLD). NO guest-RAM shadow at some invented BSS
//   address — that would produce a divergence in gameplay-mode SBS where B substrate never touches it.
//   The native renderer reads this class's members via `get()` to draw the fade.
//
//   pc_faithful SIDE EFFECTS: when `game->mPcSkip==false` (pc_faithful branch), `set()` /
//   `applyLeafCall()` ALSO dispatches the recomp body of FUN_8007e9c8 so the guest packet-pool +
//   scratchpad writes fire — this is what keeps SBS byte-identical with core-B substrate. Under
//   normal PC play (pc_skip, mPcSkip=true) only the C++ members are updated (the PSX-packet path
//   is skipped — the native renderer doesn't read them anyway).
//
//   Still-recomp fade callers do NOT reach this class; they run the substrate FUN_8007e9c8 body
//   directly. Each substrate caller is a top-down port task tracked in docs/port-progress.md.
//   Overrides are NOT the answer (violates top-down ownership).
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

  // "Fully faded" threshold. When the last fade rect the game drew was at or above this in every
  // channel (subtractive => screen was fully black; additive => fully white), the class latches a
  // HOLD. On subsequent frames where no caller sets a fade (mode NONE), the held state is what the
  // renderer sees, so scene content freshly loaded during those admin frames doesn't leak through
  // as a bright flash between fade-out and the next fade-in. The hold releases as soon as any
  // caller sets a fade below this threshold in ANY channel (i.e. ramping back toward visible).
  static constexpr uint8_t FULLY_FADED_THRESHOLD = 0xE0;

  // Canonical OT slot for full-screen fades — every substrate caller in Tomba passes 4. Callers
  // that pass a different slot override via the explicit `otSlot` parameter of applyLeafCall.
  static constexpr uint32_t DEFAULT_OT_SLOT = 4;

  // Back-pointer set once by Game's constructor. Not owned; ScreenFade never outlives its Core.
  Core* core = nullptr;

  // Called ONCE at the top of each logic frame. Resets the FRAME-SCOPED state to NONE. Does NOT
  // touch the held fully-faded state — that persists across admin frames.
  void frameStart();

  // Set the fade for THIS FRAME. If (mode, r, g, b) is at or above FULLY_FADED_THRESHOLD in every
  // channel the HOLD is latched; if below in any channel the hold is released. Under pc_faithful
  // (mPcSkip=false), ALSO fires the substrate FUN_8007e9c8 body (color = (r<<16)|(g<<8)|b,
  // a1 = mode==ADDITIVE, a2 = otSlot) so guest packet-pool + scratchpad writes match recomp_path
  // byte-for-byte.
  void set(Mode mode, uint8_t r, uint8_t g, uint8_t b, uint32_t otSlot = DEFAULT_OT_SLOT);

  // Guest ABI convenience: `color` is 0x00RRGGBB in a0; `a1` selects blend (a1!=0 => ADDITIVE / white,
  // a1==0 => SUBTRACTIVE / black); `otSlot` is a2 in the substrate ABI (typically 4).
  void applyLeafCall(uint32_t color, uint32_t a1, uint32_t otSlot = DEFAULT_OT_SLOT);

  // Read this frame's effective state (native renderer's present prologue). Returns the frame-scoped
  // state if a caller set it this frame; otherwise returns the held fully-faded state (if latched),
  // otherwise NONE.
  State get() const;

private:
  // Host-owned state (was previously shadowed at guest 0x800E7DE0..7, an invented BSS address that
  // the substrate never touched — see class header note for the removal rationale).
  Mode    mFrameMode = NONE;
  uint8_t mFrameR    = 0;
  uint8_t mFrameG    = 0;
  uint8_t mFrameB    = 0;
  Mode    mHeldMode  = NONE;
  uint8_t mHeldR     = 0;
  uint8_t mHeldG     = 0;
  uint8_t mHeldB     = 0;
};
