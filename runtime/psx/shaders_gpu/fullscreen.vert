#version 450
// Fullscreen triangle, no vertex buffer; clip Y is negated because SDL_GPU presents Y-up.
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, -(p.y * 2.0 - 1.0), 0.0, 1.0);
}
