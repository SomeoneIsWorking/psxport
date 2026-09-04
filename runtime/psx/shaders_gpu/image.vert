#version 450
// SDL_GPU fullscreen IMAGE present (vertex): fullscreen triangle, no vertex buffer. Used by
// gpu_vk_present_image (SCEA boot splash) — a plain RGBA8 texture letterboxed to 4:3 by the viewport.
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    // Negate clip-space Y: SDL_GPU presents the swapchain Y-up, so image row 0 must map to screen TOP
    // (else the SCEA/FMV image is upside down). See present.vert.
    gl_Position = vec4(p.x * 2.0 - 1.0, -(p.y * 2.0 - 1.0), 0.0, 1.0);
}
