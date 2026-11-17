#version 450
#extension GL_GOOGLE_include_directive : require
// Record rasterizer: six vertices per RecordShape. A rect draws as two triangles; a triangle draws through its three
// corners, its last three vertices collapsed onto the third so they cover nothing.
#include "record_op.glsl"
struct Shape {
    ivec4 rect;        // x0, y0, x1, y1 in target pixels, half open; a triangle's bounds
    uvec4 info;        // op index, param, mapping (RecordMapping), kind (RecordShapeKind)
    vec4 corners[3];   // a triangle's corners: x, y in target pixels, u, v texel interpolants
    uvec2 colours[3];  // their colour interpolants, kColourStep a step from kColourFloor: r | g << 16, b
    uvec2 texels;      // the texels a textured triangle samples (RecordShape::texels), unused
};
layout(std430, set = 0, binding = 0) readonly buffer Shapes { Shape shapes[]; };
layout(set = 1, binding = 0) uniform Target { vec4 size; } target;
layout(location = 0) flat out uvec4 v_info;
layout(location = 1) noperspective out vec3 v_colour;
layout(location = 2) noperspective out vec2 v_uv;
layout(location = 3) flat out uint v_texels;
void main() {
    Shape q = shapes[gl_VertexIndex / 6];
    int corner = gl_VertexIndex % 6;
    vec2 p;
    v_colour = vec3(0.0);
    v_uv = vec2(0.0);
    if (q.info.w == kShapeTriangle) {
        int c = min(corner, 2);
        p = q.corners[c].xy;
        v_uv = q.corners[c].zw;
        uvec2 bits = q.colours[c];
        v_colour = vec3(float(bits.x & 0xFFFFu), float(bits.x >> 16), float(bits.y & 0xFFFFu)) / float(kColourStep) +
                   float(kColourFloor);
    } else {
        bool right = corner == 1 || corner == 3 || corner == 4;
        bool bottom = corner == 2 || corner == 4 || corner == 5;
        p = vec2(right ? q.rect.z : q.rect.x, bottom ? q.rect.w : q.rect.y);
    }
    // Offscreen targets are Y-up: row 0 is NDC +1.
    gl_Position = vec4(p.x / target.size.x * 2.0 - 1.0, -(p.y / target.size.y * 2.0 - 1.0), 0.0, 1.0);
    v_info = q.info;
    v_texels = q.texels.x;
}
