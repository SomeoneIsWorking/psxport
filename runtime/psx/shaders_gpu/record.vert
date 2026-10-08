#version 450
// Record rasterizer: one rectangle of target pixels per RecordQuad, six vertices each.
struct Quad {
    ivec4 rect; // x0, y0, x1, y1 in target pixels, half open
    uvec4 info; // op index, param, row offset
};
layout(std430, set = 0, binding = 0) readonly buffer Quads { Quad quads[]; };
layout(set = 1, binding = 0) uniform Target { vec4 size; } target;
layout(location = 0) flat out uvec4 v_info;
void main() {
    Quad q = quads[gl_VertexIndex / 6];
    int corner = gl_VertexIndex % 6;
    bool right = corner == 1 || corner == 3 || corner == 4;
    bool bottom = corner == 2 || corner == 4 || corner == 5;
    vec2 p = vec2(right ? q.rect.z : q.rect.x, bottom ? q.rect.w : q.rect.y);
    // Offscreen targets are Y-up: row 0 is NDC +1.
    gl_Position = vec4(p.x / target.size.x * 2.0 - 1.0, -(p.y / target.size.y * 2.0 - 1.0), 0.0, 1.0);
    v_info = q.info;
}
