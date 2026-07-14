#version 450
// Pack the semi-blend intermediate's float RGBA back into the PSX 1555 VRAM word (2026-07-01 dark-outline
// fix, gpu_gpu.cpp render_geom). Runs once after the real-HW-blend semi pass, so downstream consumers
// (present, shot/vkvram readback, provat, SBS) keep seeing the ordinary packed VRAM they always have.
// STP is written 0 (opaque) — nothing this same frame re-reads this region as a semi TEXTURE source (the
// semi pass samples the ORIGINAL packed atlas via its own `u_vram`, never this encoded output).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_color;
void main() {
    // textureSize(), not a hardcoded 1024x512 — see decode.frag's comment (same ires-scale reasoning).
    ivec2 sz = textureSize(u_color, 0);
    ivec2 t = ivec2(v_uv * vec2(sz));
    vec3 rgb = clamp(texelFetch(u_color, t, 0).rgb, 0.0, 1.0);
    uint r = uint(rgb.r * 31.0 + 0.5), g = uint(rgb.g * 31.0 + 0.5), b = uint(rgb.b * 31.0 + 0.5);
    uint w = r | (g << 5) | (b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
