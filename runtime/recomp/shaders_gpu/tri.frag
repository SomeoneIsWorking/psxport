#version 450
// SDL_GPU untextured opaque PSX triangle (fragment). The color target is an R16_UINT VRAM image, so the
// fragment OUTPUTS the packed PSX 1555 word directly as a uint (no A1R5G5B5 alias / no HW blend — SDL_GPU
// has no format aliasing, unlike the Vulkan build). STP bit left 0 (opaque).
layout(location = 0) in vec3 v_col;
layout(location = 0) out uint o_px;
void main() {
    uint r = uint(clamp(v_col.r, 0.0, 1.0) * 31.0 + 0.5);
    uint g = uint(clamp(v_col.g, 0.0, 1.0) * 31.0 + 0.5);
    uint b = uint(clamp(v_col.b, 0.0, 1.0) * 31.0 + 0.5);
    o_px = r | (g << 5) | (b << 10);
}
