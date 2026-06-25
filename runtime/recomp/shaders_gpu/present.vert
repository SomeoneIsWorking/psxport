#version 450
// SDL_GPU present pass (vertex): fullscreen triangle, no vertex buffer. 3 verts cover the viewport,
// UV 0..1. Identical to the Vulkan present.vert — SDL_GPU's Vulkan/Metal backends consume this SPIR-V.
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
