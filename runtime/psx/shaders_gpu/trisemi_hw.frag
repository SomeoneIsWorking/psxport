#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_uv.glsl"
// REAL HARDWARE blend for PSX semi-transparent world quads (2026-07-01 dark-outline fix, replaces the old
// in-shader "sample a VRAM snapshot as the destination" approach in tritex.frag, which read a STALE
// pre-frame buffer for the native render path — see gpu_vk.cpp render_geom's header comment for the full
// root-cause). This shader still samples its OWN texture/CLUT exactly like tritex.frag (u_vram = the
// ordinary packed atlas snapshot, unaffected by this pass), but instead of manually blending against a
// second sampled "destination", it emits a colour PRE-SHAPED for the GPU's fixed-function blend unit to
// combine correctly with whatever is ALREADY in the (float) render target — the actual current frame's
// content, guaranteed fresh and race-free because it's the same hardware ROP the opaque pass just wrote to.
//
// PSX rule preserved: a texel's own STP bit (not the primitive's semi flag) decides blend-vs-opaque. We
// can't vary the blend equation per-fragment in fixed-function hardware, so we fold the STP decision into
// the shader's OWN alpha output and pick blend factors that make "STP=0 -> opaque" and "STP=1 -> the real
// PSX equation" both true from ONE static per-pipeline blend state (one pipeline per PSX blend mode, see
// gpu_vk.cpp's 4 s_semi_pipe_* pipelines). Alpha is the destination coefficient: 0 for an opaque texel,
// 0.5 for an ABR0 blending texel, and 1 for an ABR1/2/3 blending texel.
//   pipeline state: src_color_factor=ONE, dst_color_factor=SRC_ALPHA, op = ADD (avg/add/add4) or
//                   REVERSE_SUBTRACT (sub).
//   avg  (B+F)/2: colour = stp ? F*.5-0.25/31 : F, alpha=.5 -> encode floors 5-bit AVG; the quarter-LSB
//                   bias sits safely between the integer/half-integer results despite RGBA16F rounding
//   add  (B+F):   colour = F, alpha=1                       -> B+F
//   add4 (F/4+B): colour = stp ? floor(F*31/4)/31 : F       -> B+floor(F5/4)/31
//   sub  (B-F):   colour = stp ? F : -F, alpha=1            -> B-F
// (sub's -F needs an unclamped/float target — the whole reason this intermediate is float, not the
// UNORM-packed VRAM texture itself.) No manual destination sample, no snapshot, no staleness window.
layout(location = 0) in vec3 v_col;
layout(location = 1) noperspective in vec2 v_uv;
layout(location = 2) flat in ivec4 v_tp;     // tpx, tpy, mode, raw
layout(location = 3) flat in ivec4 v_clut;   // clutx, cluty, semi, blend
layout(location = 4) flat in ivec4 v_tw;     // texture window
layout(location = 5) flat in ivec4 v_da;
layout(location = 6) flat in ivec4 v_uvbb;     // draw-area clip
layout(set = 2, binding = 0) uniform sampler2D u_vram;
// ires (internal-resolution) scale — see tritex.frag's identical comment: v_da is native-unit, gl_FragCoord
// is ires-scaled, divide back down before the clip test. 1 at i==1 (no-op, byte-identical to pre-ires).
layout(set = 3, binding = 0) uniform PC { int scale; } pc;
layout(location = 0) out vec4 o_col;

uint vram_at(int x, int y) { vec2 rg = texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).rg;
                             return uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8); }

void main() {
    int px = int(gl_FragCoord.x) / pc.scale, py = int(gl_FragCoord.y) / pc.scale;
    if (px < v_da.x || px > v_da.z || py < v_da.y || py > v_da.w) discard;
    int mode = v_tp.z;
    vec3 F; uint stp = 1u;
    if (mode == 3) {
        F = clamp(v_col, 0.0, 1.0);          // untextured: vertex colour is the foreground directly
    } else {
        vec2 psx_uv = psxUvAtIntegerPixel(v_uv, pc.scale, v_uvbb);
        int u = int(psx_uv.x), v = int(psx_uv.y);
        u = (u & ~(v_tw.x * 8)) | ((v_tw.z & v_tw.x) * 8);
        v = (v & ~(v_tw.y * 8)) | ((v_tw.w & v_tw.y) * 8);
        int tpx = v_tp.x, tpy = v_tp.y, clutx = v_clut.x, cluty = v_clut.y;
        uint texel;
        if (mode == 0)      { uint w = vram_at(tpx+(u>>2), tpy+v); texel = vram_at(clutx+int((w>>((u&3)*4))&0xFu), cluty); }
        else if (mode == 1) { uint w = vram_at(tpx+(u>>1), tpy+v); texel = vram_at(clutx+int((w>>((u&1)*8))&0xFFu), cluty); }
        else                { texel = vram_at(tpx+u, tpy+v); }
        if (texel == 0u) discard;
        stp = (texel >> 15) & 1u;
        uvec3 texel5 = uvec3(texel & 31u, (texel >> 5) & 31u, (texel >> 10) & 31u);
        if (v_tp.w == 0) {
            uvec3 color8 = uvec3(clamp(v_col, 0.0, 1.0) * 255.0 + 0.5);
            F = vec3(min(uvec3(31u), (texel5 * color8) / 128u)) / 31.0;
        } else {
            F = vec3(texel5) / 31.0;
        }
    }
    int blend = v_clut.w & 3;
    float a = (stp == 0u) ? 0.0 : ((blend == 0) ? 0.5 : 1.0);
    vec3 colour;
    if (blend == 0)      colour = (stp != 0u) ? F * 0.5 - vec3(0.25/31.0) : F; // integer avg
    else if (blend == 1) colour = F;                            // add (opaque case == blended case)
    else if (blend == 2) colour = (stp != 0u) ? F : -F;         // sub (REVERSE_SUBTRACT pipeline)
    else                 colour = (stp != 0u) ? floor(F * 31.0 / 4.0) / 31.0 : F; // integer add4
    o_col = vec4(colour, a);
}
