#version 450
// SDL_GPU present pass (fragment): sample the R16_UINT VRAM image (PSX 1555) over the display region,
// unpack 1555 -> RGB, apply the engine screen fade. SAME logic as the Vulkan present.frag; only the
// resource binding decls change for SDL_GPU's convention:
//   fragment SAMPLERS live in set=2, fragment UNIFORM BUFFERS in set=3 (SDL_GPU has no push constants).
// The uniform block is fed once per draw by SDL_PushGPUFragmentUniformData(cmd, slot 0, &PC, sizeof PC).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
// VRAM is an R8G8_UNORM texture: R = low byte, G = high byte of the PSX 1555 word (SDL_GPU forbids SAMPLER
// usage on INTEGER formats, so we can't use R16_UINT/usampler2D — RG8 round-trips the 16 bits exactly and
// is sampler-legal on every backend incl. Metal). Reconstruct the 16-bit word from the two bytes.
layout(set = 2, binding = 0) uniform sampler2D u_vram;
layout(set = 3, binding = 0) uniform PC {
    ivec4 disp;   // x, y, w, h — VRAM display/FB region to sample
    ivec4 fade;   // mode (0 none / 1 additive-white / 2 subtractive-black), r, g, b (0..255)
    ivec4 fmt;    // x = display colour depth: 0 = 15-bit (1555), 1 = 24-bit (packed RGB888)
} pc;
// One VRAM byte. The texture is RG8 = (low byte, high byte) of a little-endian halfword, so byte index
// bx lives in texel bx>>1, component R when bx is even and G when odd. Already UNORM, so the returned
// 0..1 value IS the 8-bit channel — no rescale.
float vram_byte(int bx, int y) {
    vec2 rg = texelFetch(u_vram, ivec2((bx >> 1) & 1023, y), 0).rg;
    return ((bx & 1) == 0) ? rg.r : rg.g;
}
void main() {
    ivec2 t = pc.disp.xy + ivec2(v_uv * vec2(pc.disp.zw));
    t = clamp(t, pc.disp.xy, pc.disp.xy + pc.disp.zw - ivec2(1));
    vec3 rgb;
    if (pc.fmt.x == 1) {
        // 24bpp packs RGB888 across 1.5 halfwords per pixel, so display column x is at BYTE offset
        // disp.x*2 + x*3 in the row — not at halfword disp.x + x. Reading it as 1555 scrambles every
        // colour and shows only two thirds of the width.
        int lx = clamp(int(v_uv.x * float(pc.disp.z)), 0, pc.disp.z - 1);
        int bx = pc.disp.x * 2 + lx * 3;
        rgb = vec3(vram_byte(bx, t.y), vram_byte(bx + 1, t.y), vram_byte(bx + 2, t.y));
    } else {
    vec2 rg = texelFetch(u_vram, t, 0).rg;
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    rgb = vec3(float(p & 31u) / 31.0, float((p >> 5) & 31u) / 31.0, float((p >> 10) & 31u) / 31.0);
    }
    if (pc.fade.x == 1)      rgb = min(rgb + vec3(pc.fade.yzw) / 255.0, vec3(1.0));   // additive (white)
    else if (pc.fade.x == 2) rgb = max(rgb - vec3(pc.fade.yzw) / 255.0, vec3(0.0));   // subtractive (black)
    o_col = vec4(rgb, 1.0);
}
