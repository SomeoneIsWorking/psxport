// game/render/render_native.h — the NATIVE render pass entry point.
//
// render_scene_native(c) builds the frame from native SCENE DATA (entity lists -> RenderScene) and draws
// every 3D-mesh object with float transforms + real depth, FULLY DECOUPLED from the PSX render path (no
// OT, no GP0, no GTE op). It is an ADDITIVE pass: the existing PSX-vanilla render path is untouched. The
// invocation is gated behind the `rendernative` diagnostic channel until this becomes the default.
#ifndef GAME_RENDER_NATIVE_H
#define GAME_RENDER_NATIVE_H
struct Core;
void render_scene_native(Core* c);
#endif
