#version 450
// SDL_GPU untextured opaque PSX triangle (fragment). The VRAM color target is R8G8_UNORM (R=low byte,
// G=high byte of the 1555 word — SDL_GPU forbids integer SAMPLER formats, so we can't render to/sample an
// R16_UINT; RG8 round-trips the 16 bits exactly and is sampler-legal everywhere incl. Metal). Pack the PSX
// 1555 word, then emit its two bytes. STP bit left 0 (opaque).
layout(location = 0) in vec3 v_col;
layout(location = 0) out vec4 o_col;
void main() {
    uint r = uint(clamp(v_col.r, 0.0, 1.0) * 31.0 + 0.5);
    uint g = uint(clamp(v_col.g, 0.0, 1.0) * 31.0 + 0.5);
    uint b = uint(clamp(v_col.b, 0.0, 1.0) * 31.0 + 0.5);
    uint w = r | (g << 5) | (b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
