#version 450
// SDL_GPU present pass (vertex): fullscreen triangle, no vertex buffer. 3 verts cover the viewport,
// UV 0..1. Identical to the Vulkan present.vert — SDL_GPU's Vulkan/Metal backends consume this SPIR-V.
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    // SDL_GPU presents the swapchain Y-up (it flips the swapchain so content shows right-side-up),
    // opposite to Vulkan's Y-down NDC. Negate clip-space Y so VRAM row 0 (v_uv.y=0) maps to screen TOP —
    // without this the 2D/FMV present renders upside down. (The VK build's present.vert did NOT negate.)
    gl_Position = vec4(p.x * 2.0 - 1.0, -(p.y * 2.0 - 1.0), 0.0, 1.0);
}
