// class DualviewSnapshot — save/restore FULL guest state on ONE Core, so the dual-view
// (engine-native left | PSX-recomp right) render harness can render the SAME game state
// twice from a single per-frame gameplay pass.
//
// Two snapshots per Core:
//   pre  = post-gameplay / pre-render state (captured in ov_field_frame by capturePre() right
//          before the native render consumes per-frame queues) — the PSX render pass rewinds
//          to this via restorePre() so it starts from the same guest memory the native pass did.
//   post = the real post-frame canonical state, captured after the native render completes and
//          restored after the PSX pass runs, so the running game is unaffected by the extra render.
//
// State captured: main RAM (2 MB) + scratchpad (1 KB) + GTE regs (32 ctrl + 32 data).
//
// Per-Core so SBS / dualcore never mix one core's snapshot into the other (was the
// process-globals `g_dv_have_pre` + a set of file-scope buffers in dualview_snapshot.cpp;
// deglobalize-game 2026-07-03). Reached as `core->rsub.dualviewSnapshot`.
#pragma once
#include <stdint.h>
class Core;

class DualviewSnapshot {
public:
  bool havePre() const  { return mHavePre; }
  void clearPre()       { mHavePre = false; }   // called after restorePost — snapshot consumed

  void capturePre(Core* c);    // called from ov_field_frame before the native render
  void capturePost(Core* c);   // called after the native render, before the PSX pass rewinds
  void restorePre(Core* c);    // rewind to pre so the PSX render pass sees the pre-render state
  void restorePost(Core* c);   // undo the extra PSX pass so the canonical game resumes

private:
  bool     mHavePre = false;
  uint8_t  mPreRam[0x200000];   uint8_t  mPostRam[0x200000];
  uint8_t  mPreSpad[0x400];     uint8_t  mPostSpad[0x400];
  uint32_t mPreGc[32],  mPostGc[32];   // GTE control regs
  uint32_t mPreGd[32],  mPostGd[32];   // GTE data regs
};
