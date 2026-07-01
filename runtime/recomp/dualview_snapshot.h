#ifndef DUALVIEW_SNAPSHOT_H
#define DUALVIEW_SNAPSHOT_H
struct Core;
// Snapshot/restore FULL guest state (main RAM + scratchpad + GTE regs) so the DUAL-VIEW harness can render
// the SAME game state twice (native | PSX side-by-side): 'pre' = post-gameplay/pre-render state, 'post' =
// the real post-frame canonical state. See native_step_frame's dual-view block (native_boot.cpp).
extern "C" void dv_snapshot(Core* c);       // capture 'pre' (called from ov_field_frame before the native render)
extern "C" void dv_capture_post(Core* c);   // capture 'post' (the real canonical post-frame state)
extern "C" void dv_restore_pre(Core* c);    // rewind to 'pre' (so the PSX render pass sees the right state)
extern "C" void dv_restore_post(Core* c);   // restore 'post' (undo the extra PSX pass, canonical game resumes)
#endif
