#version 450
// SDL_GPU fullscreen triangle targeting an OFFSCREEN VRAM-space render target (decode/encode passes for
// the real-HW-blend semi path, 2026-07-01 dark-outline fix — see gpu_vk.cpp render_geom). Shared by
// decode.frag (1555 -> float RGBA) and encode.frag (float RGBA -> 1555): both read/write the SAME
// VRAM_W x VRAM_H space as tri.vert/tritex.vert, so v_uv.y=0 must land at NDC+1 (row 0), matching those
// shaders' "offscreen targets are Y-up" convention — NOT the present/image vert's swapchain flip.
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, -(p.y * 2.0 - 1.0), 0.0, 1.0);
}
