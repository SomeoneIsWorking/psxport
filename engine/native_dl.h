// RETIRED (2026-06-20, later-164 follow-up). The native classified display list — a host prim arena
// keyed by GUEST OT bucket-anchor address, walked in guest-OT order — was the PSX-OT-keyed render path.
// It is fully superseded by the engine-owned RenderQueue (engine/render_queue.{h,cpp}), which owns draw
// ORDER by explicit layer + real depth and never honors the guest OT. native_dl had no live callers
// (ndl_alloc was never invoked, so the arena was always empty); removed per the "retire scaffolding"
// directive. This header is intentionally empty; do not reintroduce.
#ifndef NATIVE_DL_H
#define NATIVE_DL_H
#endif
