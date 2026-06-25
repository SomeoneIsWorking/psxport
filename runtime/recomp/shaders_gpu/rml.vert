#version 450
// RmlUi 2D overlay vertex stage (SDL_GPU). Positions arrive in pixel space (origin top-left, y-down);
// the per-draw translate is added, then mapped to clip space. SDL_GPU presents the swapchain Y-UP (it
// flips so content shows right-side-up), so — exactly like present.vert — we NEGATE clip-space Y so a
// pixel y of 0 (the menu's TOP) lands at the screen TOP.
//
// SDL_GPU binding convention: VERTEX uniform buffers live in set=1 (fed once per draw by
// SDL_PushGPUVertexUniformData(cmd, slot 0, &UBO, sizeof UBO)).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aCol;   // R8G8B8A8_UNORM -> normalized, premultiplied alpha
layout(location = 2) in vec2 aUv;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUv;
layout(set = 1, binding = 0) uniform UBO { vec2 uTranslate; vec2 uViewport; } ubo;
void main() {
    vec2 p = aPos + ubo.uTranslate;
    float x = 2.0 * p.x / ubo.uViewport.x - 1.0;
    float y = 2.0 * p.y / ubo.uViewport.y - 1.0;
    gl_Position = vec4(x, -y, 0.0, 1.0);
    vCol = aCol;
    vUv = aUv;
}
