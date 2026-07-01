// engine/engine_render.h — PC-native per-frame RENDER ORCHESTRATION + object render walk.
//
// This module owns the engine's render frame: the top-level render-pass driver (0x8003f9a8 / its
// transition twin 0x8003fa44) and the per-object RENDER-QUEUE WALKER (0x8003bf00) that dispatches each
// queued object to its per-type render handler. It is the contiguous native parent of the per-object
// render (0x8003cca4 -> 0x8003cdd8) and thus the place the world-coord projection (engine_project) is
// wired into the LIVE field render. Render code lives HERE, not in gte_beetle.cpp.
#pragma once
#include <stdint.h>
struct Core;

// Per-frame render orchestrator = MAIN.EXE 0x8003f9a8 (the 11-pass render driver called by the native
// field per-frame update). `_x` = the mid-transition twin 0x8003fa44 (reduced pass set). The per-object
// render-queue WALKS it drives are owned in engine_submit.cpp (ov_rwalk_aux_bf00 etc.) and wired in here.
void ov_render_frame(struct Core* c);
void ov_render_frame_x(struct Core* c);
