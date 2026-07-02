// engine/engine_render.h — see class Render (game/render/render.h) for the per-frame render
// orchestrator methods. This header remains for its historical include path; the two entry points
// ov_render_frame / ov_render_frame_x are now Render::frame() / Render::frameX() methods, reached
// as c->mRender->frame() / c->mRender->frameX().
#pragma once
