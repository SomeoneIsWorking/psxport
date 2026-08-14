#version 450
// Authored-order untextured painter object. Dither is a primitive property and is applied only to
// Gouraud shading, matching the PSX GPU. gl_FragCoord is at the internal-resolution raster size, so
// divide by scale before selecting the native 4x4 matrix cell.
layout(location = 0) in vec3 v_col;
layout(location = 1) flat in float v_gouraud;
layout(location = 2) flat in float v_dither;
layout(location = 0) out vec4 o_col;
layout(set = 3, binding = 0) uniform PC { int scale; } pc;
const int dm[16] = int[16](-4,0,-3,1, 2,-2,3,-1, -3,1,-4,0, 3,-1,2,-2);
void main() {
    ivec3 c8 = ivec3(clamp(v_col, 0.0, 1.0) * 255.0 + 0.5);
    if (v_gouraud > 0.5 && v_dither > 0.5) {
        ivec2 p = ivec2(gl_FragCoord.xy) / max(pc.scale, 1);
        c8 = clamp(c8 + ivec3(dm[(p.y & 3) * 4 + (p.x & 3)]), ivec3(0), ivec3(255));
    }
    uvec3 c5 = uvec3(c8) >> 3;
    uint w = c5.r | (c5.g << 5) | (c5.b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
