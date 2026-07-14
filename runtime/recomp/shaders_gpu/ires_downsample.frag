#version 450
// ires (internal-resolution) downsample: box-filter the scaled 3D target's display sub-rect back down to
// native VRAM pixels (gpu_gpu.cpp render_geom's ires_composite_back). A plain SDL_BlitGPUTexture LINEAR
// blit is a single bilinear tap per destination texel — correct for magnifying (the seed/upsample blit)
// but WRONG for a >1:1 minify: sampling only the nearest 2x2 source texels aliases high-frequency content
// (grass blades, leaf clusters) into visible per-pixel confetti noise (found live at i=2 AND i=4 during
// ires bring-up, 2026-07-15 — proof: the raw ires target read back via `iresdump`/gpu_gpu_ires_rawdump is
// perfectly clean; only the downsampled composite showed the artifact). This shader instead averages the
// FULL pc.n x pc.n source-texel box per destination pixel, in unpacked RGB space (packed-1555 bytes can't
// be linearly averaged as raw bytes — same reasoning as decode.frag/encode.frag), then re-packs. Reused
// fsq.vert; viewport is set to the native destination sub-rect [sx,sy,disp_w,h) so gl_FragCoord IS the
// destination VRAM pixel directly (no separate UV math needed).
layout(location = 0) in vec2 v_uv;   // unused (fsq.vert emits it; gl_FragCoord is what we need here)
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_src;   // the ires-scaled color target (packed 1555, RG8)
layout(set = 3, binding = 0) uniform PC { int n; } pc;  // ires scale factor (box side = n)
void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * pc.n;
    vec3 sum = vec3(0.0);
    for (int j = 0; j < pc.n; j++) {
        for (int i = 0; i < pc.n; i++) {
            vec2 rg = texelFetch(u_src, base + ivec2(i, j), 0).rg;
            uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
            sum += vec3(float(p & 31u), float((p >> 5) & 31u), float((p >> 10) & 31u)) / 31.0;
        }
    }
    vec3 avg = sum / float(pc.n * pc.n);
    uint r = uint(avg.r * 31.0 + 0.5), g = uint(avg.g * 31.0 + 0.5), b = uint(avg.b * 31.0 + 0.5);
    uint w = r | (g << 5) | (b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
