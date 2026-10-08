#version 450
// Plain NxN box downsample of the ires composite C -> native VRAM. USED ONLY for the headless `shot` /
// VRAM-space readback (the WINDOW presents from C directly at high res — see gpu_vk.cpp render_geom /
// present, the present-at-high-res unification, USER 2026-07-16). Averages the full pc.n x pc.n source
// box per destination native pixel in UNPACKED RGB (packed-1555 bytes can't be linearly averaged as raw
// bytes — same reasoning as decode.frag/encode.frag), then re-packs. Viewport is set to the native
// destination sub-rect, so gl_FragCoord IS the destination VRAM pixel (no UV math needed).
//
// This REPLACES the old coverage-mixing version (u_depth / u_native bug #55 machinery): that only existed
// because the composite was downsampled to native BEFORE present and had to stay sharp; now the composite
// is presented at full res, so the shot just wants a faithful box downscale of the whole composite.
layout(location = 0) in vec2 v_uv;   // unused
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_src;   // the composite C (packed 1555, RG8), scaled by n
layout(set = 3, binding = 0) uniform PC { int n; } pc;  // ires scale factor (box side = n)
vec3 unpack555(vec2 rg) {
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    return vec3(float(p & 31u), float((p >> 5) & 31u), float((p >> 10) & 31u)) / 31.0;
}
void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * pc.n;
    vec3 sum = vec3(0.0);
    for (int j = 0; j < pc.n; j++)
        for (int i = 0; i < pc.n; i++)
            sum += unpack555(texelFetch(u_src, base + ivec2(i, j), 0).rg);
    vec3 avg = sum / float(pc.n * pc.n);
    uint r = uint(avg.r * 31.0 + 0.5), g = uint(avg.g * 31.0 + 0.5), b = uint(avg.b * 31.0 + 0.5);
    uint w = r | (g << 5) | (b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
