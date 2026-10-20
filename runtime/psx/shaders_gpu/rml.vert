#version 450
// RmlUi geometry in pixels (y down) plus the draw translate; clip Y negated because SDL_GPU presents Y-up.
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
