#version 450
// Pack interpolated RGB -> PSX 1555 and write to the R16_UINT VRAM color attachment. (Mask bit and
// ordered dither are added in M3; opaque only here.)
layout(location = 0) in vec3 v_col;
layout(location = 0) out uint o_px;
void main() {
    uint r = uint(clamp(v_col.r, 0.0, 1.0) * 31.0 + 0.5);
    uint g = uint(clamp(v_col.g, 0.0, 1.0) * 31.0 + 0.5);
    uint b = uint(clamp(v_col.b, 0.0, 1.0) * 31.0 + 0.5);
    o_px = r | (g << 5) | (b << 10);
}
