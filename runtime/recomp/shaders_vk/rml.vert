#version 450
// RmlUi 2D overlay vertex stage. Positions arrive in pixel space (origin top-left, y-down); the
// per-draw translate is added, then mapped to Vulkan clip space (NDC y is already down, so direct).
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aCol;   // R8G8B8A8_UNORM -> normalized, premultiplied alpha
layout(location=2) in vec2 aUv;
layout(location=0) out vec4 vCol;
layout(location=1) out vec2 vUv;
layout(binding=0, std140) uniform UBO { vec2 uTranslate; vec2 uViewport; } ubo;
void main() {
    vec2 p = aPos + ubo.uTranslate;
    gl_Position = vec4(2.0 * p.x / ubo.uViewport.x - 1.0,
                       2.0 * p.y / ubo.uViewport.y - 1.0, 0.0, 1.0);
    vCol = aCol;
    vUv = aUv;
}
