#version 450
// ires (internal-resolution) downsample: box-filter the scaled 3D target's display sub-rect back down to
// native VRAM pixels (gpu_gpu.cpp render_geom's 3D band). A plain SDL_BlitGPUTexture LINEAR blit is a
// single bilinear tap per destination texel — correct for magnifying (the seed/upsample blit) but WRONG
// for a >1:1 minify: sampling only the nearest 2x2 source texels aliases high-frequency content (grass
// blades, leaf clusters) into visible per-pixel confetti noise (found live at i=2 AND i=4 during ires
// bring-up, 2026-07-15 — proof: the raw ires target read back via `iresdump`/gpu_gpu_ires_rawdump is
// perfectly clean; only the downsampled composite showed the artifact). This shader instead averages the
// FULL pc.n x pc.n source-texel box per destination pixel, in unpacked RGB space (packed-1555 bytes can't
// be linearly averaged as raw bytes — same reasoning as decode.frag/encode.frag), then re-packs. Reused
// fsq.vert; viewport is set to the native destination sub-rect [sx,sy,disp_w,h) so gl_FragCoord IS the
// destination VRAM pixel directly (no separate UV math needed).
//
// bug #55 (part 2 — per-texel coverage mix): this composite-back used to run unconditionally over the
// WHOLE display sub-rect, overwriting destination pixels the 3D pass never touched this frame with a box
// average that mixed in the seed-blit's LINEAR-upsampled (lossy) copy of whatever was underneath — most
// visibly, a 2D_BG band prim (the pause-menu text — RQ_OM_2D_BG; see PSXPORT_DEBUG=b55) drawn NATIVE-
// resolution by band 1 in render_geom, then blurred back out by this pass because the 3D world's opaque
// geometry PARTIALLY covers the same destination pixel's source box (e.g. a dithered/punch-through-alpha
// texture edge): a per-destination-pixel ALL-OR-NOTHING discard (an earlier version of this fix) still
// averaged in the blurry seed for every uncovered sub-texel of a PARTIALLY covered box, which stayed
// visibly soft. Fixed by tracking coverage PER SUB-TEXEL (via the ires depth target, u_depth — Pass A
// clears depth to 0.0 and only OPAQUE 3D fragments write depth with a GREATER_OR_EQUAL test, so
// depth>0.0 at a given scaled texel means an opaque 3D fragment landed there this frame) and, for any
// UNCOVERED sub-texel, substituting u_native's single native-resolution sample (the destination pixel's
// OWN vram_tex value, snapshotted by render_geom right after band 1 draws and before the seed blit —
// GpuGpuState::s_ires_bg_snap) instead of the (lossy, upsampled) u_src value. Every sub-texel's
// contribution is now either a real, sharp 3D-covered sample or the exact same real, sharp native pixel
// repeated — so a box with ZERO 3D coverage reduces exactly to u_native's value (pixel-exact, matches a
// full discard) and a box with partial coverage properly antialiases real 3D edges against the real
// background instead of a blurred one.
// Residual (documented, not silently patched): a SEMI-only 3D fragment (no opaque backing at that pixel)
// does not write depth (real-HW-blend semi is test-only, depth_write=false, so multiple overlapping semi
// quads can all blend) — such a sub-texel is (wrongly) treated as uncovered and falls back to u_native.
// Narrow in practice (this engine's semi content typically overlays opaque terrain/objects), but a
// semi-coverage stencil bit would close it fully; not implemented here.
layout(location = 0) in vec2 v_uv;   // unused (fsq.vert emits it; gl_FragCoord is what we need here)
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_src;     // the ires-scaled color target (packed 1555, RG8)
layout(set = 2, binding = 1) uniform sampler2D u_depth;   // the ires-scaled depth target (D32_FLOAT, sampled)
layout(set = 2, binding = 2) uniform sampler2D u_native;  // native-res vram_tex snapshot taken after band 1 (packed 1555, RG8)
layout(set = 3, binding = 0) uniform PC { int n; } pc;    // ires scale factor (box side = n)
vec3 unpack555(vec2 rg) {
    uint p = uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8);
    return vec3(float(p & 31u), float((p >> 5) & 31u), float((p >> 10) & 31u)) / 31.0;
}
void main() {
    ivec2 dstPixel = ivec2(gl_FragCoord.xy);   // native destination pixel (also the u_native lookup coord)
    ivec2 base = dstPixel * pc.n;
    vec3 nativeCol = unpack555(texelFetch(u_native, dstPixel, 0).rg);   // this destination pixel's own sharp value
    vec3 sum = vec3(0.0);
    for (int j = 0; j < pc.n; j++) {
        for (int i = 0; i < pc.n; i++) {
            ivec2 texel = base + ivec2(i, j);
            bool covered = texelFetch(u_depth, texel, 0).r > 0.0;
            sum += covered ? unpack555(texelFetch(u_src, texel, 0).rg) : nativeCol;
        }
    }
    vec3 avg = sum / float(pc.n * pc.n);
    uint r = uint(avg.r * 31.0 + 0.5), g = uint(avg.g * 31.0 + 0.5), b = uint(avg.b * 31.0 + 0.5);
    uint w = r | (g << 5) | (b << 10);
    o_col = vec4(float(w & 0xFFu) / 255.0, float((w >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
